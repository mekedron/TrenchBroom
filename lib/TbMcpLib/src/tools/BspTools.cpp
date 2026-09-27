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

#include "mcp/tools/BspTools.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "fs/File.h"
#include "fs/Reader.h"
#include "mcp/AgentCamera.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/CameraProjection.h"
#include "mcp/Image.h"
#include "mcp/JsonVm.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/AssetUtils.h"
#include "mcp/tools/BspFile.h"
#include "mcp/tools/BspRender.h"
#include "mcp/tools/SnapshotTools.h"
#include "mdl/GameConfig.h"
#include "mdl/GameFileSystem.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"

#include "kd/string_compare.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;
using namespace std::chrono_literals;

constexpr auto MaxImageSize = size_t(2048);
constexpr auto MinImageSize = size_t(16);
constexpr auto MaxViews = size_t(8);
constexpr auto PollInterval = 20ms;

ToolError cancelledError()
{
  return makeError(ErrorCode::Cancelled, "The call was cancelled.");
}

std::optional<std::string> readFileBytes(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return std::nullopt;
  }
  auto buffer = std::ostringstream{};
  buffer << stream.rdbuf();
  if (stream.bad())
  {
    return std::nullopt;
  }
  return std::move(buffer).str();
}

std::optional<std::filesystem::file_time_type> modificationTime(
  const std::filesystem::path& path)
{
  auto ec = std::error_code{};
  const auto time = std::filesystem::last_write_time(path, ec);
  return ec ? std::nullopt : std::optional{time};
}

bool isDirectory(const std::filesystem::path& path)
{
  auto ec = std::error_code{};
  return std::filesystem::is_directory(path, ec);
}

/** The file with the given name in the folder, compared case-insensitively. */
std::optional<std::filesystem::path> findFileCi(
  const std::filesystem::path& directory, const std::string& name)
{
  if (!isDirectory(directory))
  {
    return std::nullopt;
  }
  if (const auto exact = directory / name; pathExists(exact))
  {
    return exact;
  }
  auto ec = std::error_code{};
  for (const auto& entry : std::filesystem::directory_iterator{directory, ec})
  {
    if (kdl::ci::str_is_equal(entry.path().filename().string(), name))
    {
      return entry.path();
    }
  }
  return std::nullopt;
}

/** The folders searched for WAD files and the Quake palette: the BSP's ancestors. */
std::vector<std::filesystem::path> searchFolders(
  const std::filesystem::path& bspPath, const std::optional<std::filesystem::path>& game)
{
  auto result = std::vector<std::filesystem::path>{};
  const auto add = [&](const std::filesystem::path& folder) {
    if (
      !folder.empty() && isDirectory(folder)
      && std::ranges::find(result, folder) == result.end())
    {
      result.push_back(folder);
    }
  };
  const auto addWithChildren = [&](const std::filesystem::path& folder) {
    add(folder);
    auto ec = std::error_code{};
    if (isDirectory(folder))
    {
      for (const auto& entry : std::filesystem::directory_iterator{folder, ec})
      {
        if (entry.is_directory(ec))
        {
          add(entry.path());
        }
      }
    }
  };

  auto folder = bspPath.parent_path();
  for (size_t i = 0; i < 3 && !folder.empty(); ++i)
  {
    add(folder);
    folder = folder.parent_path();
  }
  // the game folder with its mods (e.g. <Half-Life>/valve)
  if (game)
  {
    addWithChildren(*game);
  }
  // <game>/<mod>/maps/name.bsp: the other mods of the game (e.g. valve)
  if (kdl::ci::str_is_equal(bspPath.parent_path().filename().string(), "maps"))
  {
    addWithChildren(bspPath.parent_path().parent_path().parent_path());
  }
  return result;
}

/** Finds a WAD of the BSP's wad list: the path itself, or its file name in the folders.
 */
std::optional<std::filesystem::path> findWad(
  const std::string& wadPath, const std::vector<std::filesystem::path>& folders)
{
  auto normalized = wadPath;
  std::ranges::replace(normalized, '\\', '/');
  const auto path = std::filesystem::path{normalized};
  if (path.is_absolute() && pathExists(path))
  {
    return path;
  }
  const auto name = path.filename().string();
  if (name.empty())
  {
    return std::nullopt;
  }
  for (const auto& folder : folders)
  {
    if (auto found = findFileCi(folder, name))
    {
      return found;
    }
  }
  return std::nullopt;
}

struct LoadedWad
{
  std::string listed;
  std::optional<std::filesystem::path> path;
  std::optional<WadFile> wad;
  std::optional<std::string> error;
  size_t texturesUsed = 0;
};

struct TextureSet
{
  BspTextureImages images;
  std::vector<std::string> missing;
  size_t embedded = 0;
  size_t fromWads = 0;
  std::vector<LoadedWad> wads;
  /** Where the Quake palette came from, if needed. */
  std::optional<std::string> paletteSource;
};

/** The Quake palette: from the document's game, else gfx/palette.lmp near the BSP. */
std::optional<std::vector<unsigned char>> loadQuakePalette(
  CallContext& context,
  const std::vector<std::filesystem::path>& folders,
  std::string& source)
{
  if (context.hasDocument())
  {
    const auto& map = context.map();
    const auto& palettePath = map.gameInfo().gameConfig.materialConfig.palette;
    if (!palettePath.empty())
    {
      auto logger = NullLogger{};
      auto fileSystem = createGameFileSystem(map, logger);
      auto file = fileSystem->openFile(palettePath);
      if (file.is_success())
      {
        auto reader = file.value()->reader();
        if (reader.size() >= 768)
        {
          auto palette = std::vector<unsigned char>(768);
          reader.read(palette.data(), palette.size());
          source = palettePath.string();
          return palette;
        }
      }
    }
  }
  for (const auto& folder : folders)
  {
    const auto path = folder / "gfx" / "palette.lmp";
    if (const auto bytes = readFileBytes(path); bytes && bytes->size() >= 768)
    {
      source = path.string();
      return std::vector<unsigned char>(bytes->begin(), bytes->begin() + 768);
    }
  }
  return std::nullopt;
}

std::vector<RgbaImage> mipImages(
  const MipTexture& texture, const std::vector<unsigned char>* palette)
{
  auto result = std::vector<RgbaImage>{};
  for (size_t level = 0; level < MipLevels; ++level)
  {
    if ((texture.width >> level) == 0 || (texture.height >> level) == 0)
    {
      break;
    }
    result.push_back(mipTextureImage(texture, level, palette));
  }
  return result;
}

TextureSet loadTextures(
  CallContext& context,
  const BspData& bsp,
  const std::filesystem::path& bspPath,
  const std::vector<std::filesystem::path>& extraWads)
{
  auto result = TextureSet{};
  result.images.resize(bsp.textures.size());

  auto game = std::optional<std::filesystem::path>{};
  if (context.hasDocument() && isGamePathValid(context.map().gameInfo()))
  {
    game = gamePath(context.map().gameInfo());
  }
  const auto folders = searchFolders(bspPath, game);

  auto palette = std::optional<std::vector<unsigned char>>{};
  if (!bsp.isHalfLife())
  {
    auto source = std::string{};
    palette = loadQuakePalette(context, folders, source);
    if (palette)
    {
      result.paletteSource = source;
    }
    else
    {
      context.warn(
        "PALETTE_NOT_FOUND",
        "The Quake palette (gfx/palette.lmp) was not found; textures are drawn in gray "
        "levels. Open the map with its game configured, or pass a BSP inside the game "
        "folder.");
    }
  }
  const auto* paletteData = palette ? &*palette : nullptr;

  // the WADs: the explicit ones first, then the BSP's wad list
  auto wadList =
    std::vector<std::pair<std::string, std::optional<std::filesystem::path>>>{};
  for (const auto& path : extraWads)
  {
    wadList.emplace_back(path.string(), path);
  }
  for (const auto& listed : bsp.wadPaths())
  {
    wadList.emplace_back(listed, findWad(listed, folders));
  }

  const auto needsWads = std::ranges::any_of(
    bsp.textures, [](const auto& texture) { return !texture.hasPixels(); });
  for (auto& [listed, path] : wadList)
  {
    auto loaded = LoadedWad{listed, path, std::nullopt, std::nullopt};
    if (!path)
    {
      loaded.error = "not found";
    }
    else if (needsWads)
    {
      if (auto bytes = readFileBytes(*path))
      {
        auto wad = readWad(std::move(*bytes));
        if (wad.is_success())
        {
          loaded.wad = std::move(wad).value();
        }
        else
        {
          loaded.error = std::get<std::string>(wad.error());
        }
      }
      else
      {
        loaded.error = "cannot be read";
      }
    }
    result.wads.push_back(std::move(loaded));
  }

  for (size_t i = 0; i < bsp.textures.size(); ++i)
  {
    const auto& texture = bsp.textures[i];
    if (texture.width == 0)
    {
      continue;
    }
    if (texture.hasPixels())
    {
      result.images[i] = mipImages(texture, paletteData);
      ++result.embedded;
      continue;
    }
    auto found = false;
    for (auto& loaded : result.wads)
    {
      if (!loaded.wad)
      {
        continue;
      }
      if (const auto index = loaded.wad->find(texture.name))
      {
        auto mip = loaded.wad->mipTexture(loaded.wad->entries[*index]);
        if (mip.is_success() && mip.value().hasPixels())
        {
          result.images[i] = mipImages(mip.value(), paletteData);
          ++loaded.texturesUsed;
          ++result.fromWads;
          found = true;
          break;
        }
      }
    }
    if (!found)
    {
      result.missing.push_back(texture.name);
    }
  }
  return result;
}

/** The BSP of the document: next to the map, in compile/ or in the game's maps. */
Result<std::filesystem::path, ToolError> defaultBspPath(CallContext& context)
{
  if (!context.hasDocument())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass path: there is no document whose compiled map could be previewed.",
      "Pass the absolute path of a .bsp file, e.g. compile_status's compiledFile.");
  }
  const auto& map = context.map();
  const auto mapPath = map.path();
  if (mapPath.empty() || !mapPath.is_absolute())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The map was never saved, so it has no compiled BSP.",
      "Save and compile the map (compile_run), or pass path.");
  }
  auto name = mapPath.filename();
  name.replace_extension(".bsp");
  auto candidates = std::vector<std::filesystem::path>{
    mapPath.parent_path() / name, mapPath.parent_path() / "compile" / name};
  if (isGamePathValid(map.gameInfo()))
  {
    auto ec = std::error_code{};
    for (const auto& entry :
         std::filesystem::directory_iterator{gamePath(map.gameInfo()), ec})
    {
      candidates.push_back(entry.path() / "maps" / name);
    }
  }

  auto best = std::optional<std::filesystem::path>{};
  auto bestTime = std::filesystem::file_time_type{};
  for (const auto& candidate : candidates)
  {
    if (const auto time = modificationTime(candidate);
        time && (!best || *time > bestTime))
    {
      best = candidate;
      bestTime = *time;
    }
  }
  if (!best)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("There is no compiled {} next to the map yet.", name.string()),
      "Compile the map with compile_run, or pass the path of a .bsp file.");
  }
  return *best;
}

// Views

struct ViewInput
{
  std::string label;
  ResolvedCamera camera;
};

struct ViewOutput
{
  BspRenderResult render;
  std::optional<std::string> png;
};

/** The work of one call, shared with the render thread. */
struct PreviewJob
{
  std::atomic<bool> cancel = false;
  std::atomic<bool> done = false;
  std::atomic<size_t> viewsDone = 0;

  BspData bsp;
  BspTextureImages textures;
  BspRenderOptions options;
  std::vector<ImageProjection> projections;

  std::vector<ViewOutput> outputs;
};

void runJob(const std::shared_ptr<PreviewJob>& job)
{
  for (const auto& projection : job->projections)
  {
    if (job->cancel.load())
    {
      break;
    }
    auto output = ViewOutput{};
    output.render =
      renderBsp(job->bsp, job->textures, projection, job->options, &job->cancel);
    if (!output.render.cancelled)
    {
      output.png = encodePng(output.render.image);
    }
    job->outputs.push_back(std::move(output));
    ++job->viewsDone;
  }
  job->done.store(true);
}

Json statsJson(const ImageLightStats& stats)
{
  return Json{
    {"coverage", roundForOutput(stats.coverage)},
    {"meanLight", roundForOutput(stats.meanLight)},
    {"meanBrightness", roundForOutput(stats.meanBrightness)},
    {"darkFraction", roundForOutput(stats.darkFraction)},
    {"dimFraction", roundForOutput(stats.dimFraction)},
    {"overexposedFraction", roundForOutput(stats.overexposedFraction)},
    {"skyPixels", stats.skyPixels},
    {"liquidPixels", stats.liquidPixels},
  };
}

Json lightmapStatsJson(const LightmapStats& stats)
{
  return Json{
    {"faces", stats.faces},
    {"unlitFaces", stats.unlitFaces},
    {"luxels", stats.luxels},
    {"meanLight", roundForOutput(stats.meanLight)},
    {"darkFraction", roundForOutput(stats.darkFraction)},
    {"dimFraction", roundForOutput(stats.dimFraction)},
    {"overexposedFraction", roundForOutput(stats.overexposedFraction)},
  };
}

std::string percent(const double fraction)
{
  return fmt::format("{}%", int(std::lround(fraction * 100.0)));
}

/** Short statements about the light of an image that an agent can act on. */
Json viewFindings(const BspRenderResult& render)
{
  auto findings = Json::array();
  const auto& stats = render.stats;
  if (stats.pixels == 0)
  {
    findings.push_back(
      "No lit surface is visible (only sky, liquids or the void); check the camera.");
    return findings;
  }
  if (stats.darkFraction >= 0.5)
  {
    findings.push_back(fmt::format(
      "{} of the visible surfaces are pitch black (light < {}).",
      percent(stats.darkFraction),
      int(DarkLight)));
  }
  else if (stats.dimFraction >= 0.6)
  {
    findings.push_back(fmt::format(
      "{} of the visible surfaces are dim (light < {}).",
      percent(stats.dimFraction),
      int(DimLight)));
  }
  if (stats.overexposedFraction >= 0.2)
  {
    findings.push_back(fmt::format(
      "{} of the visible surfaces are saturated (light >= {}): too bright.",
      percent(stats.overexposedFraction),
      int(BrightLight)));
  }
  for (size_t i = 0; i < render.regions.size(); ++i)
  {
    const auto& region = render.regions[i];
    if (region.coverage < 0.25)
    {
      continue;
    }
    if (region.darkFraction >= 0.8 && stats.darkFraction < 0.5)
    {
      findings.push_back(
        fmt::format("The {} of the image is pitch black.", imageRegionName(i)));
    }
    if (region.overexposedFraction >= 0.5 && stats.overexposedFraction < 0.2)
    {
      findings.push_back(fmt::format(
        "The {} of the image is saturated (too bright).", imageRegionName(i)));
    }
  }
  return findings;
}

Result<std::optional<std::filesystem::path>, ToolError> savePath(
  const Args& args, const size_t index, const size_t count)
{
  if (!args.has("saveTo"))
  {
    return std::optional<std::filesystem::path>{};
  }
  auto path = absolutePathArgument(args, "saveTo");
  if (path.is_error())
  {
    return errorOf(path);
  }
  auto result = path.value();
  if (count > 1)
  {
    const auto extension = result.extension();
    result.replace_extension();
    result += fmt::format("-{}", index + 1);
    result += extension.empty() ? std::filesystem::path{".png"} : extension;
  }
  if (!args.getOr("overwrite", false) && pathExists(result))
  {
    return makeError(
      ErrorCode::FileExists,
      fmt::format("{} already exists.", result.string()),
      "Pass overwrite: true to replace it, or choose another path.");
  }
  if (!isDirectory(result.parent_path()))
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("The folder {} does not exist.", result.parent_path().string()),
      "Save into an existing folder; the server does not create folders.");
  }
  return std::optional{result};
}

Result<LightStyleWeights, ToolError> parseLightStyles(
  const Args& args, const BspData& bsp)
{
  const auto value = args.getOr<Json>("lightStyles", Json("initial"));
  if (value.is_array())
  {
    return lightStyleWeights(value.get<std::vector<int>>());
  }
  const auto name = value.get<std::string>();
  return lightStyleWeights(
    bsp,
    name == "base"  ? LightStyleSet::Base
    : name == "all" ? LightStyleSet::All
                    : LightStyleSet::Initial);
}

struct RegionInput
{
  std::string name;
  vm::bbox3d box;
};

Result<std::vector<RegionInput>, ToolError> parseRegions(
  CallContext& context, const Args& args)
{
  auto result = std::vector<RegionInput>{};
  if (!args.has("regions"))
  {
    return result;
  }
  const auto regions = args.get<Json>("regions");
  if (regions.is_string())
  {
    if (!context.hasDocument())
    {
      return makeError(
        ErrorCode::NoDocument,
        "regions: \"spaces\" needs an open document with a map manifest.",
        "Pass regions as [{\"name\": ..., \"box\": {\"min\": ..., \"max\": ...}}].");
    }
    auto manifest = context.documentState().manifest.get();
    if (manifest.is_error())
    {
      return makeError(
        ErrorCode::OperationFailed,
        fmt::format(
          "The map manifest cannot be read: {}", std::get<std::string>(manifest.error())),
        "Fix or remove the manifest file, or pass regions as boxes.");
    }
    for (const auto& space : manifest.value().spaces)
    {
      if (space.bounds)
      {
        result.push_back(RegionInput{space.name.value_or(space.id), *space.bounds});
      }
    }
    if (result.empty())
    {
      context.warn(
        "NO_SPACES",
        "The map manifest has no spaces with bounds; store them with map_manifest_set.");
    }
    return result;
  }
  for (const auto& region : regions)
  {
    result.push_back(
      RegionInput{region["name"].get<std::string>(), *boxFromJson(region["box"])});
  }
  return result;
}

std::string_view gameName(const BspData& bsp)
{
  return bsp.isHalfLife() ? "Half-Life" : "Quake";
}

/** Waits for the render thread in deferred steps and completes the call. */
class PreviewPoller : public std::enable_shared_from_this<PreviewPoller>
{
private:
  CallContext& m_context;
  std::shared_ptr<PreviewJob> m_job;
  ToolCompletion m_completion;
  std::vector<std::string> m_labels;
  std::vector<ResolvedCamera> m_cameras;
  std::vector<std::optional<std::filesystem::path>> m_saveTo;
  Json m_report;
  size_t m_reported = 0;

public:
  PreviewPoller(
    CallContext& context,
    std::shared_ptr<PreviewJob> job,
    ToolCompletion completion,
    std::vector<std::string> labels,
    std::vector<ResolvedCamera> cameras,
    std::vector<std::optional<std::filesystem::path>> saveTo,
    Json report)
    : m_context{context}
    , m_job{std::move(job)}
    , m_completion{std::move(completion)}
    , m_labels{std::move(labels)}
    , m_cameras{std::move(cameras)}
    , m_saveTo{std::move(saveTo)}
    , m_report(std::move(report))
  {
  }

  void schedule()
  {
    m_context.defer([self = shared_from_this()]() { self->step(); }, PollInterval);
  }

private:
  void step()
  {
    if (m_context.cancelled())
    {
      m_job->cancel.store(true);
      m_completion(cancelledError());
      return;
    }
    if (const auto done = m_job->viewsDone.load(); done > m_reported)
    {
      m_reported = done;
      m_context.progress(
        double(done),
        double(m_labels.size()),
        fmt::format("Rendered {}", m_labels[done - 1]));
    }
    if (!m_job->done.load())
    {
      schedule();
      return;
    }
    m_completion(finish());
  }

  ToolResult finish()
  {
    auto viewsJson = Json::array();
    for (size_t i = 0; i < m_job->outputs.size(); ++i)
    {
      const auto& output = m_job->outputs[i];
      if (!output.png)
      {
        return makeError(ErrorCode::InternalError, "Could not encode the image.");
      }
      auto savedTo = Json(nullptr);
      if (m_saveTo[i])
      {
        auto stream = std::ofstream{*m_saveTo[i], std::ios::binary | std::ios::trunc};
        stream.write(output.png->data(), std::streamsize(output.png->size()));
        stream.close();
        if (!stream)
        {
          return makeError(
            ErrorCode::IoError,
            fmt::format("Could not write {}.", m_saveTo[i]->string()),
            "Check that the folder is writable.");
        }
        savedTo = m_saveTo[i]->string();
      }
      if (m_job->outputs.size() > 1)
      {
        m_context.addText(m_labels[i]);
      }
      m_context.addImage(*output.png, "image/png");

      auto regions = Json::array();
      for (size_t r = 0; r < output.render.regions.size(); ++r)
      {
        auto region = statsJson(output.render.regions[r]);
        region["region"] = std::string{imageRegionName(r)};
        regions.push_back(std::move(region));
      }
      const auto& camera = m_cameras[i];
      auto view = Json{
        {"label", m_labels[i]},
        {"camera", toJson(camera.camera)},
        {"cameraName", camera.name ? Json(*camera.name) : Json(nullptr)},
        {"image",
         {{"width", output.render.image.width},
          {"height", output.render.image.height},
          {"format", "png"},
          {"bytes", output.png->size()},
          {"savedTo", std::move(savedTo)}}},
        {"facesDrawn", output.render.facesDrawn},
        {"light", statsJson(output.render.stats)},
        {"regions", std::move(regions)},
        {"findings", viewFindings(output.render)},
      };
      if (!camera.placement.is_null())
      {
        view["placement"] = camera.placement;
      }
      viewsJson.push_back(std::move(view));
    }

    auto result = std::move(m_report);
    result["views"] = std::move(viewsJson);
    return result;
  }
};

void bspPreview(CallContext& context, const Args& args, ToolCompletion completion)
{
  // the BSP
  auto path = std::filesystem::path{};
  if (args.has("path"))
  {
    auto argument = absolutePathArgument(args, "path");
    if (argument.is_error())
    {
      completion(errorOf(argument));
      return;
    }
    path = argument.value();
  }
  else
  {
    auto found = defaultBspPath(context);
    if (found.is_error())
    {
      completion(errorOf(found));
      return;
    }
    path = found.value();
  }
  auto bytes = readFileBytes(path);
  if (!bytes)
  {
    completion(makeError(
      ErrorCode::IoError,
      fmt::format("Cannot read {}.", path.string()),
      "Check the path; compile_status reports the compiled file."));
    return;
  }
  auto parsed = readBsp(*bytes);
  if (parsed.is_error())
  {
    completion(makeError(
      ErrorCode::Unsupported,
      fmt::format("{}: {}", path.string(), std::get<std::string>(parsed.error())),
      "bsp_preview reads Quake (BSP 29) and Half-Life (BSP 30) maps."));
    return;
  }
  bytes.reset();

  auto job = std::make_shared<PreviewJob>();
  job->bsp = std::move(parsed).value();
  const auto& bsp = job->bsp;

  if (context.hasDocument() && context.map().path() != std::filesystem::path{})
  {
    const auto mapTime = modificationTime(context.map().path());
    const auto bspTime = modificationTime(path);
    if (mapTime && bspTime && *mapTime > *bspTime)
    {
      context.warn(
        "BSP_OUTDATED",
        "The map was saved after the BSP was compiled; the preview shows the older "
        "compile. Compile again (compile_run) to see the current map.");
    }
  }
  if (bsp.lighting.empty())
  {
    context.warn(
      "NO_LIGHT_DATA",
      "The BSP has no light data (the light / RAD stage did not run); the game draws it "
      "fully lit, and so does the preview.");
  }

  // options
  auto styles = parseLightStyles(args, bsp);
  if (styles.is_error())
  {
    completion(errorOf(styles));
    return;
  }
  auto& options = job->options;
  const auto shading = args.getOr<std::string>("shading", "lit");
  options.shading = shading == "fullbright" ? BspShading::Fullbright
                    : shading == "lightmap" ? BspShading::Lightmap
                                            : BspShading::Lit;
  options.brightness = args.getOr("brightness", 1.0);
  options.gamma = args.getOr("gamma", 1.0);
  options.hideSky = args.getOr("hideSky", false);
  options.entities = args.getOr("entities", true);
  options.styles = styles.value();

  auto regions = parseRegions(context, args);
  if (regions.is_error())
  {
    completion(errorOf(regions));
    return;
  }

  // cameras
  const auto width = size_t(args.getOr<int64_t>("width", 1024));
  const auto height = size_t(args.getOr<int64_t>("height", 768));
  const auto worldBounds = bsp.models.front().bounds;
  const auto cameraFloor = CameraFloor{
    [&bsp](const vm::vec3d& point) { return bspFloorBelow(bsp, point); },
    bsp.isHalfLife() ? 64.0 : 46.0,
    bsp.isHalfLife() ? "halflife" : "quake"};
  const auto resolve = [&](const Json& camera) {
    return resolveCameraArgument(
      context, camera, width, height, [&]() { return worldBounds; }, &cameraFloor);
  };

  auto views = std::vector<ViewInput>{};
  if (args.has("views"))
  {
    if (args.has("camera"))
    {
      completion(makeError(
        ErrorCode::InvalidArgument,
        "Pass either camera or views.",
        "views renders several cameras: [{\"label\": ..., \"camera\": ...}]."));
      return;
    }
    const auto list = args.get<Json>("views");
    for (size_t i = 0; i < list.size(); ++i)
    {
      const auto& view = list[i];
      auto camera = resolve(view["camera"]);
      if (camera.is_error())
      {
        completion(errorOf(camera));
        return;
      }
      auto label = view.contains("label") ? view["label"].get<std::string>()
                   : camera.value().name  ? *camera.value().name
                                          : fmt::format("view {}", i + 1);
      views.push_back(ViewInput{std::move(label), std::move(camera).value()});
    }
  }
  else if (args.has("camera"))
  {
    auto camera = resolve(args.get<Json>("camera"));
    if (camera.is_error())
    {
      completion(errorOf(camera));
      return;
    }
    auto label = camera.value().name ? *camera.value().name : std::string{"camera"};
    views.push_back(ViewInput{std::move(label), std::move(camera).value()});
  }
  else if (const auto start = playerStart(bsp))
  {
    auto camera = ResolvedCamera{};
    camera.camera = perspectiveCamera(
      start->origin + vm::vec3d{0, 0, eyeOffsetAboveOrigin(bsp)},
      directionFromAngles(start->yaw, 0.0));
    camera.placement = Json{{"playerStart", start->classname}};
    views.push_back(ViewInput{"player start", std::move(camera)});
  }
  else
  {
    auto camera = resolve(Json(nullptr));
    if (camera.is_error())
    {
      completion(errorOf(camera));
      return;
    }
    views.push_back(ViewInput{"overview", std::move(camera).value()});
  }

  auto saveTo = std::vector<std::optional<std::filesystem::path>>{};
  for (size_t i = 0; i < views.size(); ++i)
  {
    auto projection = ImageProjection::create(views[i].camera.camera, width, height);
    if (projection.is_error())
    {
      completion(makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "The camera of '{}' is invalid: {}", views[i].label, errorMessage(projection)),
        "Check the camera."));
      return;
    }
    job->projections.push_back(std::move(projection).value());
    auto save = savePath(args, i, views.size());
    if (save.is_error())
    {
      completion(errorOf(save));
      return;
    }
    saveTo.push_back(save.value());
  }

  // textures
  auto extraWads = std::vector<std::filesystem::path>{};
  for (const auto& wad : args.getOr<std::vector<std::string>>("wads", {}))
  {
    const auto wadPath = std::filesystem::path{wad};
    if (!wadPath.is_absolute())
    {
      completion(makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The WAD path {} is not absolute.", wad),
        "Pass absolute paths in wads."));
      return;
    }
    extraWads.push_back(wadPath.lexically_normal());
  }
  auto textures = loadTextures(context, bsp, path, extraWads);
  job->textures = std::move(textures.images);
  if (!textures.missing.empty())
  {
    context.warn(
      "TEXTURES_MISSING",
      fmt::format(
        "{} textures were found neither in the BSP nor in its WADs and are drawn as a "
        "magenta checkerboard: {}. Pass the WAD files with wads.",
        textures.missing.size(),
        fmt::join(
          textures.missing.begin(),
          textures.missing.begin()
            + std::ptrdiff_t(std::min(textures.missing.size(), size_t(10))),
          ", ")));
  }

  // the report that does not need the images
  auto wadsJson = Json::array();
  for (const auto& wad : textures.wads)
  {
    wadsJson.push_back(Json{
      {"listed", wad.listed},
      {"path", wad.path ? Json(wad.path->string()) : Json(nullptr)},
      {"texturesUsed", wad.texturesUsed},
      {"error", wad.error ? Json(*wad.error) : Json(nullptr)},
    });
  }
  auto regionsJson = Json::array();
  for (const auto& region : regions.value())
  {
    auto json = lightmapStatsJson(lightmapStats(bsp, options.styles, region.box));
    json["name"] = region.name;
    json["box"] = toJson(region.box);
    regionsJson.push_back(std::move(json));
  }
  auto report = Json{
    {"bsp",
     {
       {"path", path.string()},
       {"version", bsp.version},
       {"game", std::string{gameName(bsp)}},
       {"faces", bsp.faces.size()},
       {"models", bsp.models.size()},
       {"entities", bsp.entities.size()},
       {"bounds", toJson(worldBounds)},
       {"hasLightData", !bsp.lighting.empty()},
     }},
    {"textures",
     {
       {"count", bsp.textures.size()},
       {"embedded", textures.embedded},
       {"fromWads", textures.fromWads},
       {"missing", textures.missing},
       {"wads", std::move(wadsJson)},
       {"palette",
        textures.paletteSource ? Json(*textures.paletteSource) : Json(nullptr)},
     }},
    {"world", lightmapStatsJson(lightmapStats(bsp, options.styles))},
    {"regions", std::move(regionsJson)},
    {"thresholds",
     {{"dark", DarkLight}, {"dim", DimLight}, {"overexposed", BrightLight}}},
  };

  auto labels = std::vector<std::string>{};
  auto cameras = std::vector<ResolvedCamera>{};
  for (auto& view : views)
  {
    labels.push_back(std::move(view.label));
    cameras.push_back(std::move(view.camera));
  }

  context.progress(0, double(labels.size()), "Rendering");
  std::thread{[job]() { runJob(job); }}.detach();

  auto poller = std::make_shared<PreviewPoller>(
    context,
    std::move(job),
    std::move(completion),
    std::move(labels),
    std::move(cameras),
    std::move(saveTo),
    std::move(report));
  poller->schedule();
}

} // namespace

void registerBspTools(ToolRegistry& registry)
{
  const auto region = object({
    field("name", string().nonEmpty()).required().describe("The region's name"),
    field("box", box()).required().describe("The region in world coordinates"),
  });

  registry.add(
    ToolDef{"bsp_preview"}
      .title("BSP Preview")
      .description(
        "Renders a compiled map (Half-Life BSP 30 or Quake BSP 29) the way the game "
        "lights it, without starting the game: textures from the BSP or its WADs "
        "multiplied by the lightmaps, from agent cameras; a CPU renderer that does not "
        "touch the user's views. path defaults to the document's newest compiled BSP "
        "(next to the map, in compile/ or in the game's maps folder). camera takes an "
        "agent camera name or an inline camera as in view_snapshot (eyeHeight stands on "
        "the BSP's floor); views renders up to 8 cameras; without either, the view "
        "from the player start. shading: lit, fullbright or lightmap; lightStyles: "
        "initial (lights on at map start), base (style 0) or all, or a list of styles; "
        "brightness and gamma adjust the image; hideSky. Each image comes with light "
        "statistics (mean light 0-255, dark < 16, dim < 48, overexposed >= 250 "
        "fractions, per 3x3 region) and findings such as 'the bottom-left of the image "
        "is pitch black'; world and regions (boxes, or 'spaces' of the map manifest) "
        "give lightmap statistics independent of the camera. Example: {\"camera\": "
        "{\"eyeHeight\": {\"point\": [0, 0, 64]}, \"yaw\": 90}, \"regions\": "
        "[{\"name\": \"basement\", \"box\": {\"min\": [-512, -512, -256], \"max\": "
        "[512, 512, 0]}}]}")
      .input(object({
        field("path", string().nonEmpty())
          .describe("Absolute path of the .bsp (default: the document's compiled map)"),
        field("camera", cameraArgumentSchema())
          .describe("An agent camera name or an inline camera (default: player start)"),
        field(
          "views",
          array(object({
                  field("label", string().nonEmpty()).describe("The image's label"),
                  field("camera", cameraArgumentSchema())
                    .required()
                    .describe("An agent camera name or an inline camera"),
                }))
            .nonEmpty()
            .maxSize(MaxViews))
          .describe("Several cameras, one image each (instead of camera)"),
        field("width", integer().min(double(MinImageSize)).max(double(MaxImageSize)))
          .describe("Image width in pixels (default 1024)"),
        field("height", integer().min(double(MinImageSize)).max(double(MaxImageSize)))
          .describe("Image height in pixels (default 768)"),
        field("shading", enumOf({"lit", "fullbright", "lightmap"}))
          .describe(
            "lit: textures times light (default); fullbright: textures only; lightmap: "
            "the light alone"),
        field(
          "lightStyles",
          oneOf({
            enumOf({"initial", "base", "all"}),
            array(integer().min(0).max(254)),
          }))
          .describe("initial: the lights on when the map starts (default); base: only "
                    "unswitchable lights (style 0); all: every style; or style numbers "
                    "(style 0 is always included)"),
        field("brightness", number().min(0.1).max(8))
          .describe("Factor on the image's colours (default 1)"),
        field("gamma", number().min(0.5).max(4))
          .describe("Output gamma; above 1 brightens dark areas (default 1)"),
        field("hideSky", boolean())
          .describe("Leave out sky faces, e.g. to look into the map from outside its sky "
                    "(default false)"),
        field("entities", boolean())
          .describe("Draw brush entities: doors, walls, glass (default true)"),
        field("wads", array(string().nonEmpty()))
          .describe("Absolute paths of WAD files searched before the BSP's own wad list"),
        field("regions", oneOf({enumOf({"spaces"}), array(region).maxSize(64)}))
          .describe(
            "Lightmap statistics per region: named boxes, or 'spaces' for the spaces "
            "of the map manifest"),
        field("saveTo", string().nonEmpty())
          .describe(
            "Absolute path of a PNG file to save the image to (several views: -1, -2, "
            "... before the extension)"),
        field("overwrite", boolean()).describe("Replace existing files (default false)"),
      }))
      .output(object({
        field("bsp", any())
          .describe(
            "{path, version, game, faces, models, entities, bounds, hasLightData}"),
        field("textures", any())
          .describe(
            "{count, embedded, fromWads, missing, wads: [{listed, path, texturesUsed, "
            "error}], palette}"),
        field("world", any())
          .describe("Lightmap statistics of all drawn faces: {faces, unlitFaces, luxels, "
                    "meanLight, darkFraction, dimFraction, overexposedFraction}"),
        field("regions", array(any()))
          .describe("The same statistics per requested region, with name and box"),
        field("thresholds", any()).describe("{dark, dim, overexposed} light levels"),
        field("views", array(any()))
          .describe(
            "Per image: {label, camera, cameraName, image, facesDrawn, light: {coverage, "
            "meanLight, meanBrightness, darkFraction, dimFraction, overexposedFraction, "
            "skyPixels, liquidPixels}, regions (3x3), findings, placement?}"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .asyncHandler(bspPreview));
}

} // namespace tb::mcp
