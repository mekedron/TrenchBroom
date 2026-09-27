/*
 Copyright (C) 2026 Nikita Rabykin

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcp/tools/MaterialKnowledgeTools.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "base/ParserStatus.h"
#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/Pagination.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/MapHeader.h"
#include "mdl/MapReader.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tb::mcp
{

MaterialKnowledge materialKnowledge(CallContext& context)
{
  return MaterialKnowledge{context.map(), context.host().knowledgeDirectory()};
}

void warnKnowledgeProblems(CallContext& context, const MaterialKnowledge& knowledge)
{
  for (const auto& problem : knowledge.problems())
  {
    context.warn(
      "KNOWLEDGE_FILE_INVALID",
      problem + " The file is ignored; fix or delete it, or rewrite it with "
                "material_corpus_scan / material_notes_set.");
  }
}

namespace
{
using namespace schema;

/** Material names listed in results before the rest is left out. */
constexpr auto MaxTopMaterials = size_t(20);
/** The maximum number of profiles material_usage returns. */
constexpr auto MaxProfiles = size_t(50);

bool hasWildcards(const std::string_view pattern)
{
  return pattern.find_first_of("*?") != std::string_view::npos;
}

bool matchesFilter(const std::string_view name, const std::string_view filter)
{
  return hasWildcards(filter) ? kdl::ci::str_matches_glob(name, filter)
                              : kdl::ci::str_contains(name, filter);
}

Result<KnowledgeScope, ToolError> requireScope(CallContext& context)
{
  const auto directory = context.host().knowledgeDirectory();
  if (!directory)
  {
    return makeError(
      ErrorCode::UnsupportedInHost,
      "This editor has no folder for level-design knowledge.",
      "material_usage still works with the map, the game configuration and the texture "
      "images.");
  }
  return knowledgeScope(context.map(), *directory);
}

Json scopeJson(const KnowledgeScope& scope, const std::string& level)
{
  auto result = toJson(scope);
  result["level"] = level;
  result["path"] = (level == "game" ? scope.gameDirectory : scope.directory).string();
  return result;
}

Schema scopeSchema()
{
  return object({
    field("game", string()).required().describe("The document's game"),
    field("mod", any()).required().describe("The document's most specific mod or null"),
    field("path", string()).required().describe("The knowledge folder"),
    field("level", enumOf({"game", "mod"})).describe("The level read or written"),
  });
}

// material_corpus_scan

/** Collects the parser's warnings and errors instead of logging them. */
class QuietParserStatus : public ParserStatus
{
public:
  explicit QuietParserStatus(Logger& logger)
    : ParserStatus{logger, ""}
  {
  }

private:
  void doProgress(double) override {}
  void doLog(LogLevel, const std::string&) override {}
};

/**
 * Reads a map file in its own format and passes every brush face to a callback. Brushes
 * and patches are dropped after sampling; entities, groups and layers are kept until the
 * reader is destroyed, since they can be parents of later nodes.
 */
class SamplingReader : public mdl::MapReader
{
private:
  std::unique_ptr<mdl::WorldNode> m_world;
  std::function<void(const mdl::BrushFace&)> m_onFace;

public:
  SamplingReader(
    const std::string_view str,
    const mdl::MapFormat format,
    const mdl::EntityPropertyConfig& entityPropertyConfig,
    std::function<void(const mdl::BrushFace&)> onFace)
    : MapReader{str, format, format, entityPropertyConfig}
    , m_world{std::make_unique<mdl::WorldNode>(
        entityPropertyConfig, mdl::Entity{}, format)}
    , m_onFace{std::move(onFace)}
  {
    m_world->disableNodeTreeUpdates();
  }

  Result<void> read(
    const vm::bbox3d& worldBounds, ParserStatus& status, kdl::task_manager& taskManager)
  {
    return readEntities(worldBounds, status, taskManager);
  }

private:
  mdl::Node* onWorldNode(std::unique_ptr<mdl::WorldNode>, ParserStatus&) override
  {
    return m_world->defaultLayer();
  }

  void onLayerNode(std::unique_ptr<mdl::Node> layerNode, ParserStatus&) override
  {
    m_world->addChild(layerNode.release());
  }

  void onNode(
    mdl::Node* parentNode, std::unique_ptr<mdl::Node> node, ParserStatus&) override
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node.get()))
    {
      for (const auto& face : brushNode->brush().faces())
      {
        m_onFace(face);
      }
      return;
    }
    if (dynamic_cast<const mdl::PatchNode*>(node.get()))
    {
      return;
    }
    auto* parent = parentNode ? parentNode : m_world->defaultLayer();
    parent->addChild(node.release());
  }
};

const auto AllFormats = std::vector<mdl::MapFormat>{
  mdl::MapFormat::Standard,
  mdl::MapFormat::Valve,
  mdl::MapFormat::Quake2,
  mdl::MapFormat::Quake2_Valve,
  mdl::MapFormat::Hexen2,
  mdl::MapFormat::Daikatana,
  mdl::MapFormat::Quake3_Legacy,
  mdl::MapFormat::Quake3_Valve,
  mdl::MapFormat::Quake3,
};

/** The formats to try for a file without a format comment: the game's, then all. */
std::vector<mdl::MapFormat> formatsToTry(const mdl::Map& map)
{
  auto result = std::vector<mdl::MapFormat>{};
  for (const auto& formatConfig : map.gameInfo().gameConfig.fileFormats)
  {
    result.push_back(mdl::formatFromName(formatConfig.format));
  }
  std::ranges::copy(AllFormats, std::back_inserter(result));

  auto seen = std::set<mdl::MapFormat>{};
  std::erase_if(result, [&](const auto format) {
    return format == mdl::MapFormat::Unknown || !seen.insert(format).second;
  });
  return result;
}

struct ScanJob
{
  ToolCompletion completion;
  KnowledgeScope scope;
  std::filesystem::path folder;
  std::string mode;
  std::vector<std::filesystem::path> files;
  size_t next = 0;
  CorpusFile corpus;
  Json failed = Json::array();
  std::vector<std::string> otherGames;
  std::unordered_map<std::string, std::optional<vm::vec2d>> textureSizes;
};

std::optional<vm::vec2d> textureSize(
  ScanJob& job, const mdl::Map& map, const std::string& name)
{
  const auto key = kdl::str_to_lower(name);
  if (const auto it = job.textureSizes.find(key); it != job.textureSizes.end())
  {
    return it->second;
  }
  const auto size = textureSizeOf(map.materialManager().material(name));
  job.textureSizes.emplace(key, size);
  return size;
}

/**
 * Samples every brush face of the file into the job's corpus. Returns an error message if
 * the file cannot be read or parsed in any format.
 */
std::optional<std::string> scanFile(
  mdl::Map& map, ScanJob& job, const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  const auto text = std::string{std::istreambuf_iterator<char>{stream}, {}};
  if (!stream.good() && !stream.eof())
  {
    return "The file could not be read.";
  }

  auto headerStream = std::istringstream{text};
  const auto [headerGame, headerFormat] =
    mdl::readMapHeader(headerStream)
    | kdl::value_or(std::pair{std::optional<std::string>{}, mdl::MapFormat::Unknown});
  const auto candidates = headerFormat != mdl::MapFormat::Unknown
                            ? std::vector<mdl::MapFormat>{headerFormat}
                            : formatsToTry(map);

  auto errors = std::vector<std::string>{};
  for (const auto format : candidates)
  {
    auto sampled = CorpusFile{};
    const auto onFace = [&](const mdl::BrushFace& face) {
      const auto& name = face.materialName();
      const auto size = textureSize(job, map, name);
      sampled.faces += 1;
      auto& entry = sampled.materials[kdl::str_to_lower(name)];
      if (entry.name.empty())
      {
        entry.name = name;
        entry.textureSize = size;
      }
      if (const auto sample = sampleFace(face, size))
      {
        entry.stats.add(*sample);
      }
    };

    auto status = QuietParserStatus{map.logger()};
    auto reader =
      SamplingReader{text, format, map.worldNode().entityPropertyConfig(), onFace};
    if (const auto read = reader.read(map.worldBounds(), status, map.taskManager());
        read.is_success())
    {
      for (auto& [key, entry] : sampled.materials)
      {
        entry.stats.cap();
      }
      sampled.files.push_back(path.string());
      job.corpus.merge(sampled);
      if (
        headerGame && !kdl::ci::str_is_equal(*headerGame, map.gameInfo().gameConfig.name))
      {
        job.otherGames.push_back(fmt::format("{} ({})", path.filename(), *headerGame));
      }
      return std::nullopt;
    }
    else
    {
      errors.push_back(mdl::formatName(format) + ": " + errorMessage(read));
    }
  }
  return fmt::format("Could not be parsed ({}).", kdl::str_join(errors, "; "));
}

Result<std::vector<std::filesystem::path>, ToolError> listMapFiles(
  const std::filesystem::path& folder, const bool recursive, const std::string& pattern)
{
  auto error = std::error_code{};
  if (!std::filesystem::is_directory(folder, error))
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("{} is not a folder.", folder),
      "Pass the absolute path of a folder with reference .map files.");
  }

  auto files = std::vector<std::filesystem::path>{};
  const auto add = [&](const std::filesystem::directory_entry& entry) {
    auto entryError = std::error_code{};
    if (
      entry.is_regular_file(entryError)
      && kdl::ci::str_matches_glob(entry.path().filename().string(), pattern))
    {
      files.push_back(entry.path());
    }
  };
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  if (recursive)
  {
    for (auto it = std::filesystem::recursive_directory_iterator{folder, options, error};
         !error && it != std::filesystem::recursive_directory_iterator{};
         it.increment(error))
    {
      add(*it);
    }
  }
  else
  {
    for (auto it = std::filesystem::directory_iterator{folder, options, error};
         !error && it != std::filesystem::directory_iterator{};
         it.increment(error))
    {
      add(*it);
    }
  }
  if (error)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not list {}: {}", folder, error.message()),
      "Check that the folder exists and is readable, or pass another folder.");
  }
  if (files.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "No file in {} matches '{}'{}.",
        folder,
        pattern,
        recursive ? "" : " (subfolders were not searched)"),
      "Check 'folder' and 'pattern'; use map_files_list to find map files.");
  }
  std::ranges::sort(files, [](const auto& lhs, const auto& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  return files;
}

/** The materials of the corpus with the most samples, with their summaries. */
Json topMaterials(const CorpusFile& corpus, const mdl::Map& map)
{
  auto entries = std::vector<const CorpusFile::Entry*>{};
  for (const auto& [key, entry] : corpus.materials)
  {
    entries.push_back(&entry);
  }
  std::ranges::stable_sort(entries, [](const auto* lhs, const auto* rhs) {
    return lhs->stats.samples > rhs->stats.samples;
  });

  auto result = Json::array();
  for (const auto* entry : entries)
  {
    if (result.size() == MaxTopMaterials)
    {
      break;
    }
    const auto* material = map.materialManager().material(entry->name);
    const auto size = material ? textureSizeOf(material) : entry->textureSize;
    auto item = toJson(summarize(entry->stats, size));
    item["name"] = entry->name;
    item["textureSize"] = size ? toJson(*size) : Json(nullptr);
    result.push_back(std::move(item));
  }
  return result;
}

void finishScan(CallContext& context, const std::shared_ptr<ScanJob>& job)
{
  auto& map = context.map();
  const auto scanned = job->files.size() - job->failed.size();
  const auto scannedFaces = job->corpus.faces;
  const auto scannedMaterials = job->corpus.materials.size();
  const auto corpusPath = job->scope.corpusPath();

  job->corpus.game = job->scope.game;
  job->corpus.mod = job->scope.mod;
  job->corpus.folders = {job->folder.string()};
  job->corpus.scannedAt = isoTime(std::chrono::system_clock::now());

  auto corpus = job->corpus;
  if (job->mode == "merge")
  {
    auto existing = readCorpusFile(corpusPath);
    if (existing.is_success() && existing.value())
    {
      corpus = *existing.value();
      corpus.game = job->scope.game;
      corpus.mod = job->scope.mod;
      corpus.merge(job->corpus);
    }
    else if (existing.is_error())
    {
      context.warn(
        "KNOWLEDGE_FILE_INVALID",
        std::get<std::string>(existing.error()) + " It is replaced by this scan.");
    }
  }

  if (!job->otherGames.empty())
  {
    context.warn(
      "GAME_MISMATCH",
      fmt::format(
        "{} file(s) were made for another game than {}: {}. Their statistics were "
        "added anyway.",
        job->otherGames.size(),
        map.gameInfo().gameConfig.name,
        kdl::str_join(job->otherGames, ", ")));
  }
  if (scanned == 0)
  {
    job->completion(makeError(
      ErrorCode::InvalidArgument,
      "None of the files could be read as a map file; nothing was written.",
      "Check that the folder holds .map files of this game.",
      {}));
    return;
  }

  auto result = Json{
    {"scope", scopeJson(job->scope, job->scope.mod ? "mod" : "game")},
    {"mode", job->mode},
    {"files",
     Json{
       {"total", job->files.size()},
       {"scanned", scanned},
       {"failed", job->failed},
     }},
    {"faces", scannedFaces},
    {"materials", scannedMaterials},
    {"topMaterials", topMaterials(job->corpus, map)},
    {"corpus",
     Json{
       {"path", corpusPath.string()},
       {"files", corpus.files.size()},
       {"faces", corpus.faces},
       {"materials", corpus.materials.size()},
     }},
  };

  if (context.dryRun())
  {
    result["written"] = false;
    result["wouldDo"] = fmt::format(
      "{} {} with the statistics of {} material(s)",
      job->mode == "merge" ? "update" : "write",
      corpusPath,
      corpus.materials.size());
    job->completion(std::move(result));
    return;
  }

  if (auto written = writeCorpusFile(corpusPath, corpus); written.is_error())
  {
    job->completion(errorOf(written));
    return;
  }
  result["written"] = true;
  job->completion(std::move(result));
}

void scanStep(CallContext& context, const std::shared_ptr<ScanJob>& job)
{
  if (context.cancelled())
  {
    job->completion(makeError(
      ErrorCode::Cancelled,
      fmt::format(
        "The scan was cancelled after {} of {} file(s); nothing was written.",
        job->next,
        job->files.size())));
    return;
  }

  if (job->next == job->files.size())
  {
    context.progress(
      double(job->files.size()), double(job->files.size()), "Writing the corpus");
    finishScan(context, job);
    return;
  }

  const auto& path = job->files[job->next];
  context.progress(
    double(job->next),
    double(job->files.size()),
    fmt::format("Scanning {}", path.filename()));
  if (const auto error = scanFile(context.map(), *job, path))
  {
    job->failed.push_back(Json{{"path", path.string()}, {"message", *error}});
  }
  job->next += 1;

  context.defer([&context, job]() { scanStep(context, job); });
}

void materialCorpusScan(CallContext& context, const Args& args, ToolCompletion completion)
{
  auto scope = requireScope(context);
  if (scope.is_error())
  {
    completion(errorOf(scope));
    return;
  }
  auto folder = absolutePathArgument(args, "folder");
  if (folder.is_error())
  {
    completion(errorOf(folder));
    return;
  }
  auto files = listMapFiles(
    folder.value(), args.get<bool>("recursive"), args.get<std::string>("pattern"));
  if (files.is_error())
  {
    completion(errorOf(files));
    return;
  }

  auto job = std::make_shared<ScanJob>();
  job->completion = std::move(completion);
  job->scope = std::move(scope).value();
  job->folder = folder.value();
  job->mode = args.get<std::string>("mode");
  job->files = std::move(files).value();

  context.progress(
    0,
    double(job->files.size()),
    fmt::format("Found {} file(s) in {}", job->files.size(), job->folder));
  // scan in later steps, one file per step, so that a cancellation is processed
  context.defer([&context, job]() { scanStep(context, job); });
}

// material_notes_get

ToolResult materialNotesGet(CallContext& context, const Args& args)
{
  auto scope = requireScope(context);
  if (scope.is_error())
  {
    return errorOf(scope);
  }
  const auto& knowledgeScope = scope.value();
  const auto level = args.get<std::string>("scope");
  if (level == "mod" && !knowledgeScope.mod)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The document has no mod, so there are no mod-level notes.",
      "Use scope \"game\" or \"effective\", or enable a mod with mods_set.");
  }

  const auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto notes = std::map<std::string, std::pair<MaterialNote, std::string>>{};
  const auto add = [&](
                     const std::filesystem::path& path,
                     const std::string& noteLevel) -> std::optional<ToolError> {
    auto file = readNotesFile(path);
    if (file.is_error())
    {
      return makeError(
        ErrorCode::IoError,
        std::get<std::string>(file.error()),
        "Fix or delete the file, or rewrite the notes with material_notes_set.");
    }
    if (file.value())
    {
      for (const auto& [key, note] : file.value()->notes)
      {
        notes[key] = {note, noteLevel};
      }
    }
    return std::nullopt;
  };

  if (level != "mod")
  {
    if (auto error = add(knowledgeScope.gameNotesPath(), "game"))
    {
      return *error;
    }
  }
  if (level != "game" && knowledgeScope.mod)
  {
    if (auto error = add(knowledgeScope.notesPath(), "mod"))
    {
      return *error;
    }
  }

  const auto filter = args.getOptional<std::string>("filter");
  auto items = std::vector<Json>{};
  for (const auto& [key, entry] : notes)
  {
    if (filter && !matchesFilter(entry.first.name, *filter))
    {
      continue;
    }
    auto item = toJson(entry.first);
    item["scope"] = entry.second;
    items.push_back(std::move(item));
  }

  auto result = makePage(items, request.value(), 0);
  result["scope"] =
    scopeJson(knowledgeScope, level == "game" || !knowledgeScope.mod ? "game" : "mod");
  return result;
}

// material_notes_set

std::optional<vm::vec2d> scaleValue(const Json& value)
{
  if (value.is_number())
  {
    return vm::vec2d{value.get<double>(), value.get<double>()};
  }
  return vec2FromJson(value);
}

ToolResult materialNotesSet(CallContext& context, const Args& args)
{
  auto scope = requireScope(context);
  if (scope.is_error())
  {
    return errorOf(scope);
  }
  const auto& knowledgeScope = scope.value();

  const auto level =
    args.getOptional<std::string>("scope").value_or(knowledgeScope.mod ? "mod" : "game");
  if (level == "mod" && !knowledgeScope.mod)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The document has no mod, so notes cannot be stored for a mod.",
      "Use scope \"game\", or enable a mod with mods_set.");
  }
  const auto path =
    level == "mod" ? knowledgeScope.notesPath() : knowledgeScope.gameNotesPath();

  auto existing = readNotesFile(path);
  if (existing.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      std::get<std::string>(existing.error()),
      "Fix or delete the file before changing notes.");
  }
  auto notes = existing.value() ? *existing.value() : NotesFile{};

  const auto now = isoTime(std::chrono::system_clock::now());
  auto changed = Json::array();
  auto removed = Json::array();
  const auto entries = args.get<Json>("notes");
  for (const auto& entry : entries)
  {
    const auto material = entry["material"].get<std::string>();
    const auto key = kdl::str_to_lower(material);
    const auto remove = entry.value("remove", false);
    const auto hasValues = entry.contains("kind") || entry.contains("scale")
                           || entry.contains("faceSize") || entry.contains("text")
                           || entry.contains("clear");
    if (remove && hasValues)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The note of '{}' has 'remove' and values.", material),
        "Pass either remove: true or the values to set.");
    }
    if (!remove && !hasValues)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The note of '{}' sets nothing.", material),
        "Pass kind, scale, faceSize, text or clear, or remove: true.");
    }

    if (remove)
    {
      if (notes.notes.erase(key) > 0)
      {
        removed.push_back(material);
      }
      else
      {
        context.warn(
          "NOTE_NOT_FOUND",
          fmt::format("There is no {}-level note for '{}' to remove.", level, material));
      }
      continue;
    }

    auto note = MaterialNote{};
    if (const auto it = notes.notes.find(key); it != notes.notes.end())
    {
      note = it->second;
    }
    note.name = material;
    if (entry.contains("kind"))
    {
      note.kind = materialKindFromString(entry["kind"].get<std::string>());
    }
    if (entry.contains("scale"))
    {
      note.scale = scaleValue(entry["scale"]);
    }
    if (entry.contains("faceSize"))
    {
      note.faceSize = vec2FromJson(entry["faceSize"]);
    }
    if (entry.contains("text"))
    {
      note.text = entry["text"].get<std::string>();
    }
    for (const auto& clear : entry.value("clear", Json::array()))
    {
      const auto name = clear.get<std::string>();
      if (name == "kind")
      {
        note.kind = std::nullopt;
      }
      else if (name == "scale")
      {
        note.scale = std::nullopt;
      }
      else if (name == "faceSize")
      {
        note.faceSize = std::nullopt;
      }
      else if (name == "text")
      {
        note.text = std::nullopt;
      }
    }
    note.updated = now;

    if (note.empty())
    {
      if (notes.notes.erase(key) > 0)
      {
        removed.push_back(material);
      }
      continue;
    }
    changed.push_back(toJson(note));
    notes.notes[key] = std::move(note);
  }

  auto result = Json{
    {"scope", scopeJson(knowledgeScope, level)},
    {"set", std::move(changed)},
    {"removed", std::move(removed)},
    {"total", notes.notes.size()},
  };
  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format("write {} note(s) to {}", notes.notes.size(), path);
    return result;
  }
  if (auto written = writeNotesFile(path, notes); written.is_error())
  {
    return errorOf(written);
  }
  return result;
}

// material_usage

/** The materials of the selected faces or brushes, most used first. */
std::vector<std::string> selectedMaterials(const mdl::Map& map)
{
  auto counts = std::map<std::string, std::pair<std::string, size_t>>{};
  for (const auto& handle : map.selection().allBrushFaces())
  {
    const auto& name = handle.face().materialName();
    auto& entry = counts[kdl::str_to_lower(name)];
    if (entry.second == 0)
    {
      entry.first = name;
    }
    entry.second += 1;
  }
  auto sorted = std::vector<std::pair<std::string, size_t>>{};
  for (const auto& [key, entry] : counts)
  {
    sorted.push_back(entry);
  }
  std::ranges::stable_sort(
    sorted, [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

  auto result = std::vector<std::string>{};
  for (const auto& [name, count] : sorted)
  {
    result.push_back(name);
  }
  return result;
}

ToolResult materialUsage(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto knowledge = materialKnowledge(context);
  warnKnowledgeProblems(context, knowledge);

  auto names = std::vector<std::string>{};
  auto seen = std::set<std::string>{};
  auto matched = size_t(0);
  const auto add = [&](const std::string& name) {
    if (seen.insert(kdl::str_to_lower(name)).second)
    {
      matched += 1;
      if (names.size() < MaxProfiles)
      {
        names.push_back(name);
      }
    }
  };

  auto source = std::string{};
  if (const auto requested = args.getOptional<std::vector<std::string>>("materials"))
  {
    source = "arguments";
    auto candidates = std::vector<std::string>{};
    for (const auto* material : map.materialManager().materials())
    {
      candidates.push_back(material->name());
    }
    for (const auto& [name, count] : knowledge.mapUsage())
    {
      candidates.push_back(name);
    }
    std::ranges::stable_sort(candidates, [](const auto& lhs, const auto& rhs) {
      return kdl::str_to_lower(lhs) < kdl::str_to_lower(rhs);
    });

    for (const auto& pattern : *requested)
    {
      // an exact name first: Quake liquids start with '*'
      const auto exact = std::ranges::find_if(candidates, [&](const auto& candidate) {
        return kdl::ci::str_is_equal(candidate, pattern);
      });
      if (!hasWildcards(pattern) || exact != candidates.end())
      {
        add(exact != candidates.end() ? *exact : pattern);
        continue;
      }

      auto any = false;
      for (const auto& candidate : candidates)
      {
        if (kdl::ci::str_matches_glob(candidate, pattern))
        {
          add(candidate);
          any = true;
        }
      }
      if (!any)
      {
        context.warn(
          "NO_MATCH",
          fmt::format(
            "'{}' matches no loaded material and no material used in the map; it is "
            "looked up as a material name.",
            pattern));
        add(pattern);
      }
    }
  }
  else if (const auto selected = selectedMaterials(map); !selected.empty())
  {
    source = "selection";
    for (const auto& name : selected)
    {
      add(name);
    }
  }
  else
  {
    source = "map";
    for (const auto& [name, count] : knowledge.mapUsage())
    {
      if (names.size() == MaxTopMaterials)
      {
        break;
      }
      if (name != mdl::BrushFace::NoMaterialName)
      {
        add(name);
      }
    }
  }

  const auto withImage = args.get<bool>("includeImage");
  const auto withStatistics = args.get<bool>("includeStatistics");
  auto profiles = Json::array();
  for (const auto& name : names)
  {
    profiles.push_back(toJson(knowledge.profile(name, withImage), withStatistics));
  }

  return Json{
    {"profiles", std::move(profiles)},
    {"materialsFrom", source},
    {"matched", matched},
    {"truncated", matched > names.size()},
    {"scope", knowledge.scope() ? toJson(*knowledge.scope()) : Json(nullptr)},
  };
}

// schemas

Schema positiveVec2()
{
  return vec2().withCheck([](const Json& value) -> std::optional<std::string> {
    const auto v = vec2FromJson(value);
    if (v && (v->x() <= 0.0 || v->y() <= 0.0))
    {
      return "both components must be greater than 0";
    }
    return std::nullopt;
  });
}

Schema noteSchema()
{
  return object({
    field("material", string()).required(),
    field("kind", string()).describe("panel, tile, trim, decal, sky, liquid or tool"),
    field("scale", array(number())).describe("[u, v]"),
    field("faceSize", array(number())).describe("[width, height] in map units"),
    field("text", string()).describe("Free advice"),
    field("updated", string()).describe("ISO time of the last change"),
    field("scope", enumOf({"game", "mod"})).describe("The level the note is stored at"),
  });
}

} // namespace

void registerMaterialKnowledgeTools(ToolRegistry& registry)
{
  auto kinds = materialKindNames();

  registry.add(
    ToolDef{"material_corpus_scan"}
      .title("Scan Material Corpus")
      .description(
        "Learns how the game's materials are meant to be applied from reference "
        "maps (e.g. the original game's map sources). Not undoable: writes "
        "corpus.json in the knowledge folder of the document's game and mod "
        "(scope.path). Reads every map file in 'folder' matching 'pattern' "
        "(subfolders too unless recursive is false) without opening it, and "
        "collects per material: scales, face sizes, texel extents, repeat counts, "
        "rotations and how often the texture is aligned to face edges. Texture "
        "sizes come from the document's loaded materials, so load the game's WADs "
        "/ texture collections first (without a size, repeats and alignment are "
        "not recorded). mode \"replace\" replaces the stored corpus, \"merge\" "
        "adds to it. material_usage, uv_check, uv_align \"typical\" and "
        "material_fit_geometry use the result. Returns file counts (files.failed "
        "lists unparsable files), faces, materials and the 20 topMaterials. "
        "Reports progress per file; cancelling writes nothing. Example: "
        "{\"folder\": \"/home/me/hl/mapsrc\", \"mode\": \"merge\"}")
      .input(object({
        field("folder", string().nonEmpty())
          .required()
          .describe("Absolute path of the folder with the reference map files"),
        field("recursive", boolean()).defaultsTo(true).describe("Also scan subfolders"),
        field("pattern", string().nonEmpty())
          .defaultsTo("*.map")
          .describe("File name glob ('*', '?'), case-insensitive"),
        field("mode", enumOf({"replace", "merge"}))
          .defaultsTo("replace")
          .describe("Replace the stored corpus or merge into it"),
      }))
      .output(object({
        field("scope", scopeSchema()).required(),
        field("mode", string()).required().describe("replace or merge"),
        field("files", any())
          .required()
          .describe("{total, scanned, failed: [{path, message}]}"),
        field("faces", integer()).required().describe("Brush faces sampled by this scan"),
        field("materials", integer())
          .required()
          .describe("Distinct materials sampled by this scan"),
        field("topMaterials", array(any()))
          .required()
          .describe(
            "The 20 most used materials of this scan: {name, textureSize, samples, "
            "sizedSamples, kind, typicalScale, scaleRange, typicalFaceSize, "
            "typicalRepeats, medianRepeats, wholeRepeatFraction, alignedFraction, "
            "typicalRotation}"),
        field("corpus", any())
          .required()
          .describe("The stored corpus after the scan: {path, files, faces, materials}"),
        field("written", boolean())
          .required()
          .describe("Whether corpus.json was written"),
        field("wouldDo", string()).describe("Dry run only: what the call would do"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .asyncHandler(materialCorpusScan));

  registry.add(
    ToolDef{"material_notes_get"}
      .title("Get Material Notes")
      .description(
        "Lists the stored notes about materials of the document's game and mod "
        "(read-only): kind, scale, faceSize and text, as written with "
        "material_notes_set. scope \"effective\" merges game-level notes with the "
        "mod's notes (a mod note replaces the game note of the same material); "
        "\"game\" or \"mod\" list one level. 'filter' is a case-insensitive "
        "substring or glob ('*', '?'). Returns items, total and the knowledge "
        "folder (scope). Example: {\"filter\": \"lab1_*\"}")
      .input(object({
        field("filter", string().nonEmpty()).describe("Material name substring or glob"),
        field("scope", enumOf({"effective", "game", "mod"}))
          .defaultsTo("effective")
          .describe(
            "effective: game notes merged with the mod's notes; game or mod: one level"),
      }))
      .output(object({
        field("items", array(noteSchema())).required().describe("The notes"),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("scope", scopeSchema()).required(),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialNotesGet));

  registry.add(
    ToolDef{"material_notes_set"}
      .title("Set Material Notes")
      .description(
        "Stores explicit facts about materials for the document's game or mod; "
        "they override all statistics in material_usage, uv_check and the fit "
        "tools. Not undoable: writes a notes file in the knowledge folder. Per "
        "note: kind, scale (a number or [u, v]), faceSize ([width, height] in map "
        "units the material is made for), text (free advice); omitted values stay, "
        "'clear' removes values, remove: true deletes the note. scope \"game\" "
        "applies to all mods, \"mod\" only to the document's most specific mod "
        "(default: the mod if the document has one, else the game); a mod note "
        "replaces the game note of the same material. Returns the notes as stored. "
        "Example: {\"notes\": [{\"material\": \"LAB1_GAD2\", \"kind\": \"panel\", "
        "\"scale\": 0.5, \"text\": \"wall panel, fit 1x1\"}], \"scope\": \"game\"}")
      .input(object({
        field(
          "notes",
          array(
            object({
              field("material", string().nonEmpty())
                .required()
                .describe("Material name (case-insensitive)"),
              field("kind", enumOf(kinds))
                .describe("panel: fit to its face; tile: seamless, repeats freely; trim: "
                          "fitted across, repeated along; decal, sky, liquid, tool"),
              field(
                "scale",
                oneOf({
                  number().withCheck([](const Json& value) {
                    return value.get<double>() > 0.0
                             ? std::nullopt
                             : std::optional<std::string>{"must be greater than 0"};
                  }),
                  positiveVec2(),
                }))
                .describe("The scale to use: a number for both axes or [u, v]"),
              field("faceSize", positiveVec2())
                .describe("[width, height] in world units the material is made for"),
              field("text", string()).describe("Free advice for this material"),
              field("clear", array(enumOf({"kind", "scale", "faceSize", "text"})))
                .describe("Values to remove from the note"),
              field("remove", boolean()).describe("Delete the whole note"),
            }))
            .nonEmpty()
            .maxSize(500))
          .required()
          .describe("The notes to store, one per material (at most 500)"),
        field("scope", enumOf({"game", "mod"}))
          .describe("Default: \"mod\" if the document has a mod, else \"game\""),
      }))
      .output(object({
        field("scope", scopeSchema()).required(),
        field("set", array(noteSchema())).required().describe("The notes as stored"),
        field("removed", array(string()))
          .required()
          .describe("Materials whose note was deleted"),
        field("total", integer()).required().describe("Notes stored at this level"),
        field("wouldDo", string()).describe("Dry run only: what the call would do"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialNotesSet));

  registry.add(
    ToolDef{"material_usage"}
      .title("Material Usage Profile")
      .description(
        "Returns how materials should be applied (read-only): per material its "
        "kind (panel: fit to its face, do not repeat; tile: seamless, repeats "
        "freely; trim: fitted across, repeated along; decal, sky, liquid, tool), "
        "typical scale per axis, scale range (10th-90th percentile and extremes), "
        "texel density (map units per texel), typical face size and repeats, "
        "whole-repeat and aligned fractions, texture size and an image analysis "
        "(seamless per axis, transparency). Each value has its source and sample "
        "count; priority: \"notes\" (material_notes_set) > \"config\" (the game's "
        "smart tags) > \"corpus\" (material_corpus_scan) > \"map\" > \"name\" > "
        "\"image\"; a scale without data is the game's default (\"config\"). "
        "'materials' are names or globs matched against loaded and used materials "
        "(at most 50 results); default: the materials of the selected faces or "
        "brushes, else the 20 most used in the map. Apply the typical scale with "
        "uv_align operation \"typical\". Example: {\"materials\": [\"LAB1_GAD2\", "
        "\"lab1_w*\"]}")
      .input(object({
        field("materials", array(string().nonEmpty()).nonEmpty().maxSize(MaxProfiles))
          .describe("Material names or globs ('*', '?'); default: the materials of the "
                    "selection, else the 20 most used"),
        field("includeImage", boolean())
          .defaultsTo(true)
          .describe("Analyze the texture images (otherwise only when needed)"),
        field("includeStatistics", boolean())
          .defaultsTo(false)
          .describe("Add the corpus and map statistics summaries"),
      }))
      .output(object({
        field("profiles", array(any()))
          .required()
          .describe(
            "[{name, loaded, textureSize, mapUsage, kind, typicalScale, scaleRange, "
            "texelDensity, typicalFaceSize, typicalRepeats, wholeRepeatFraction, "
            "alignedFraction, image, note, configTag, imageError?, statistics?}]"),
        field("materialsFrom", enumOf({"arguments", "selection", "map"}))
          .required()
          .describe("Where the materials came from"),
        field("matched", integer())
          .required()
          .describe("Number of materials that matched"),
        field("truncated", boolean())
          .required()
          .describe("More than 50 materials matched; only the first 50 are listed"),
        field("scope", any())
          .required()
          .describe("{game, mod, path} of the knowledge folder, or null"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialUsage));
}

} // namespace tb::mcp
