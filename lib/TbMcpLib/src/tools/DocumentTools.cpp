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

#include "mcp/tools/DocumentTools.h"

#include "ToolUtils.h"
#include "fs/DiskIO.h"
#include "fs/PathInfo.h"
#include "fs/PathMatcher.h"
#include "fs/TraversalMode.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/Pagination.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GameTools.h"
#include "mcp/tools/SessionTools.h"
#include "mdl/Autosaver.h"
#include "mdl/ExportOptions.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/MapHeader.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/path_utils.h"
#include "kd/string_compare.h"
#include "kd/string_compare_natural.h"
#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <chrono>

namespace tb::mcp
{
namespace
{
using namespace schema;

// Values of the `unsavedChanges` parameter
constexpr auto UnsavedError = "error";
constexpr auto UnsavedSave = "save";
constexpr auto UnsavedDiscard = "discard";

Field unsavedChangesField(const std::string& action)
{
  return field(
           "unsavedChanges",
           enumOf({UnsavedError, UnsavedSave, UnsavedDiscard}).defaultsTo(UnsavedError))
    .describe(
      "What to do if the document has unsaved changes before it is " + action
      + ": 'error' (default) fails with UNSAVED_CHANGES, 'save' saves it first "
        "(it must have a file path), 'discard' drops the changes. Ask the user before "
        "discarding their work.");
}

Schema documentInfoSchema()
{
  return object(
           {
             field("id", string()).required().describe("Document handle, e.g. 'doc:1'"),
             field("title", string()),
             field("path", any()).describe("Absolute file path, or null if never saved"),
             field("game", string()),
             field("format", string()),
             field("modified", boolean()),
             field("focused", boolean()),
             field("active", boolean()),
             field("activeIn", array(string())),
             field("gamePath", string()).describe("Configured game folder ('' if unset)"),
             field("gamePathValid", boolean())
               .describe("Whether the game folder exists (game_set_path)"),
             field("worldBounds", box()).describe("Hard limits of the map, map units"),
             field("mods", any()).describe("{default, enabled}"),
             field("entityDefinitions", any()).describe("{type, path, definitionCount}"),
             field("materials", any()).describe("Material collections or WAD files"),
             field("softBounds", any()).describe("Soft map bounds in effect"),
           })
    .allowAdditionalProperties();
}

Schema logMessagesSchema()
{
  return array(object({
    field("level", enumOf({"warning", "error"})),
    field("message", string()),
  }));
}

std::string describeDocument(const DocumentInfo& document)
{
  const auto& map = document.document->map();
  return fmt::format("{} ({})", document.id, map.filename());
}

/**
 * Applies the `unsavedChanges` policy to a document that is about to be closed, reverted
 * or replaced. Saves the document if asked to (not in a dry run).
 */
std::optional<ToolError> handleUnsavedChanges(
  CallContext& context,
  const DocumentInfo& document,
  const std::string& policy,
  const std::string& action)
{
  auto& map = document.document->map();
  if (!map.modified() || policy == UnsavedDiscard)
  {
    return std::nullopt;
  }

  if (policy == UnsavedError)
  {
    return makeError(
      ErrorCode::UnsavedChanges,
      fmt::format(
        "{} has unsaved changes and would be {}.", describeDocument(document), action),
      "Pass unsavedChanges: 'save' to save it first, or 'discard' to drop the changes "
      "(ask the user before discarding their work).");
  }

  // save first
  if (!map.persistent())
  {
    return makeError(
      ErrorCode::UnsavedChanges,
      fmt::format(
        "{} has unsaved changes but was never saved, so it cannot be saved first.",
        describeDocument(document)),
      "Save it with document_save_as {\"path\": ...} first, or pass unsavedChanges: "
      "'discard'.");
  }

  if (context.dryRun())
  {
    return std::nullopt;
  }

  if (const auto saved = map.save(); saved.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not save {}: {}", map.path(), errorMessage(saved)),
      "Check that the file is writable, or use unsavedChanges: 'discard'.");
  }
  return std::nullopt;
}

std::optional<DocumentInfo> findOpenDocument(
  McpHost& host, const std::filesystem::path& path)
{
  for (const auto& document : host.documents())
  {
    const auto& map = document.document->map();
    if (!map.persistent())
    {
      continue;
    }

    auto ec = std::error_code{};
    if (map.path() == path || (std::filesystem::equivalent(map.path(), path, ec) && !ec))
    {
      return document;
    }
  }
  return std::nullopt;
}

const std::vector<std::string> formatNames(const mdl::GameInfo& gameInfo)
{
  auto result = std::vector<std::string>{};
  for (const auto& format : gameInfo.gameConfig.fileFormats)
  {
    result.push_back(format.format);
  }
  return result;
}

/**
 * Parses a map format name for the given game. The name is matched case-insensitively.
 */
Result<mdl::MapFormat, ToolError> parseFormat(
  const mdl::GameInfo& gameInfo, const std::string& name)
{
  const auto names = formatNames(gameInfo);
  const auto it = std::ranges::find_if(
    names, [&](const auto& format) { return kdl::ci::str_is_equal(format, name); });
  if (it == names.end())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The game {} does not support the map format '{}'.",
        gameInfo.gameConfig.name,
        name),
      fmt::format("Use one of: {}.", kdl::str_join(names, ", ")));
  }
  return mdl::formatFromName(*it);
}

mdl::MapFormat defaultFormat(const mdl::GameInfo& gameInfo)
{
  const auto& formats = gameInfo.gameConfig.fileFormats;
  return formats.empty() ? mdl::MapFormat::Standard
                         : mdl::formatFromName(formats.front().format);
}

void warnIfGamePathInvalid(CallContext& context, const mdl::GameInfo& gameInfo)
{
  if (!isGamePathValid(gameInfo))
  {
    context.warn(
      "GAME_PATH_NOT_SET",
      fmt::format(
        "The game folder of {} is not set up ('{}'), so materials, models and mods "
        "cannot be loaded.",
        gameInfo.gameConfig.name,
        gamePath(gameInfo)),
      {});
  }
}

void activate(CallContext& context, const DocumentInfo& document)
{
  context.server().setActiveDocument(context.session(), document.id);
}

/**
 * Checks that a new or loaded document may replace the given one (single window mode): it
 * must not be the active document of another session.
 */
std::optional<ToolError> checkReplace(CallContext& context, const DocumentInfo& replaced)
{
  for (const auto& [id, session] : context.server().sessions)
  {
    if (session.get() != &context.session() && session->activeDocumentId == replaced.id)
    {
      return makeError(
        ErrorCode::DocumentInUse,
        fmt::format(
          "The editor shows one document at a time, and the new document would replace "
          "{}, the active document of another session ({}).",
          describeDocument(replaced),
          session->clientDisplayName()),
        "Ask the user to turn off single-window mode in the preferences so that each "
        "document gets its own window, or wait until the other session is done.");
    }
  }
  return std::nullopt;
}

Json fileEntry(ServerState& server, const std::filesystem::path& path)
{
  auto ec = std::error_code{};
  const auto size = std::filesystem::file_size(path, ec);
  const auto sizeValue = ec ? Json(nullptr) : Json(size);

  ec.clear();
  const auto time = std::filesystem::last_write_time(path, ec);
  const auto timeValue = ec ? Json(nullptr) : Json(isoTime(time));

  const auto open = findOpenDocument(server.host, path);
  return Json{
    {"path", path.string()},
    {"name", path.filename().string()},
    {"size", sizeValue},
    {"modified", timeValue},
    {"openAs", open ? Json(open->id) : Json(nullptr)},
  };
}

Result<std::filesystem::path, ToolError> outputPath(
  const Args& args, const std::string_view key, const bool overwrite)
{
  return absolutePathArgument(args, key)
         | kdl::and_then([&](auto path) -> Result<std::filesystem::path, ToolError> {
             auto ec = std::error_code{};
             if (std::filesystem::is_directory(path, ec))
             {
               return makeError(
                 ErrorCode::InvalidArgument,
                 fmt::format("{} is a folder.", path),
                 "Pass a file path, e.g. <folder>/name.map.");
             }
             if (
               !path.parent_path().empty()
               && !std::filesystem::is_directory(path.parent_path(), ec))
             {
               return makeError(
                 ErrorCode::IoError,
                 fmt::format("The folder {} does not exist.", path.parent_path()),
                 "Choose an existing folder; the server does not create folders.");
             }
             if (!overwrite && pathExists(path))
             {
               return makeError(
                 ErrorCode::FileExists,
                 fmt::format("{} already exists.", path),
                 "Pass overwrite: true to replace it (ask the user first if it is not "
                 "your file), or choose another path.");
             }
             return path;
           });
}

// document_new

ToolResult documentNew(CallContext& context, const Args& args)
{
  auto& host = context.host();
  const auto gameName = args.get<std::string>("game");
  const auto* gameInfo = findGame(host, gameName);
  if (!gameInfo)
  {
    return unknownGameError(host, gameName);
  }

  auto format = defaultFormat(*gameInfo);
  if (const auto formatName = args.getOptional<std::string>("format"))
  {
    auto parsed = parseFormat(*gameInfo, *formatName);
    if (parsed.is_error())
    {
      return errorOf(parsed);
    }
    format = parsed.value();
  }

  const auto replaced = host.documentHost().documentToReplace();
  if (replaced)
  {
    if (auto error = checkReplace(context, *replaced))
    {
      return *error;
    }
    if (
      auto error = handleUnsavedChanges(
        context, *replaced, args.get<std::string>("unsavedChanges"), "replaced"))
    {
      return *error;
    }
  }

  warnIfGamePathInvalid(context, *gameInfo);

  const auto& config = gameInfo->gameConfig;
  const auto initialMap = config.forceEmptyNewMap
                            ? std::filesystem::path{}
                            : config.findInitialMap(mdl::formatName(format));
  const auto initialMapValue = !initialMap.empty() && pathExists(initialMap)
                                 ? Json(initialMap.string())
                                 : Json(nullptr);

  if (context.dryRun())
  {
    return Json{
      {"wouldDo",
       fmt::format(
         "create a new {} map in {} format{}",
         config.name,
         mdl::formatName(format),
         replaced ? " replacing " + describeDocument(*replaced) : "")},
      {"initialMap", initialMapValue},
    };
  }

  const auto created = host.documentHost().createDocument(*gameInfo, format);
  if (created.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      "Could not create the map: " + errorMessage(created),
      "Check the game configuration with game_info.");
  }

  const auto& opened = created.value();
  activate(context, opened.document);

  // the objects of the initial map template, e.g. a single brush the agent usually
  // deletes
  auto& newDocument = *opened.document.document;
  const auto& ids = context.server().documentState(newDocument).ids;
  auto initialObjects = Json::array();
  for (const auto* layer : newDocument.map().worldNode().allLayers())
  {
    for (const auto* child : layer->children())
    {
      initialObjects.push_back(ids.format(*child));
    }
  }
  if (!initialObjects.empty())
  {
    context.warn(
      "INITIAL_OBJECTS",
      fmt::format(
        "The new map contains {} object(s) from the game's initial map template; delete "
        "them with objects_delete before building if you do not need them (they overlap "
        "the first room built at the origin).",
        initialObjects.size()),
      initialObjects.get<std::vector<std::string>>());
  }

  return Json{
    {"document", documentInfo(context.server(), opened.document, context.session())},
    {"initialMap", initialMapValue},
    {"initialObjects", std::move(initialObjects)},
    {"replaced", replaced ? Json(replaced->id) : Json(nullptr)},
    {"loadMessages", toJson(opened.messages)},
  };
}

// document_open

struct OpenPlan
{
  std::filesystem::path path;
  const mdl::GameInfo* gameInfo = nullptr;
  std::string gameSource;
  mdl::MapFormat format = mdl::MapFormat::Unknown;
  std::string formatSource;
};

Result<OpenPlan, ToolError> planOpen(CallContext& context, const Args& args)
{
  auto& host = context.host();

  auto pathResult = absolutePathArgument(args, "path");
  if (pathResult.is_error())
  {
    return errorOf(pathResult);
  }

  auto plan = OpenPlan{};
  plan.path = pathResult.value();

  if (fs::Disk::pathInfo(plan.path) != fs::PathInfo::File)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("There is no map file at {}.", plan.path),
      "Use map_files_list {\"folder\": ...} or document_recent to find map files.");
  }

  auto header =
    fs::Disk::withInputStream(plan.path, mdl::readMapHeader)
    | kdl::value_or(std::pair{std::optional<std::string>{}, mdl::MapFormat::Unknown});
  const auto& [headerGame, headerFormat] = header;

  if (const auto gameName = args.getOptional<std::string>("game"))
  {
    plan.gameInfo = findGame(host, *gameName);
    if (!plan.gameInfo)
    {
      return unknownGameError(host, *gameName);
    }
    plan.gameSource = "argument";
  }
  else if (headerGame)
  {
    plan.gameInfo = findGame(host, *headerGame);
    if (!plan.gameInfo)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "{} was made for the game '{}', which is not configured.",
          plan.path,
          *headerGame),
        fmt::format(
          "Pass 'game' to open it as one of: {}.", kdl::str_join(gameNames(host), ", ")));
    }
    plan.gameSource = "fileHeader";
  }
  else
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "Could not detect the game of {}: the file has no '// Game:' comment.",
        plan.path),
      fmt::format(
        "Pass 'game', one of: {}. game_list shows which games are set up.",
        kdl::str_join(gameNames(host), ", ")));
  }

  if (const auto formatName = args.getOptional<std::string>("format"))
  {
    auto format = parseFormat(*plan.gameInfo, *formatName);
    if (format.is_error())
    {
      return errorOf(format);
    }
    plan.format = format.value();
    plan.formatSource = "argument";
  }
  else if (headerFormat != mdl::MapFormat::Unknown)
  {
    plan.format = headerFormat;
    plan.formatSource = "fileHeader";
  }
  else
  {
    plan.formatSource = "detected";
  }

  return plan;
}

Json openResult(
  CallContext& context,
  const DocumentInfo& document,
  const bool alreadyOpen,
  const OpenPlan& plan,
  const std::vector<LogMessage>& messages)
{
  return Json{
    {"document", documentInfo(context.server(), document, context.session())},
    {"alreadyOpen", alreadyOpen},
    {"detection",
     Json{
       {"game", plan.gameInfo ? Json(plan.gameInfo->gameConfig.name) : Json(nullptr)},
       {"gameSource", plan.gameSource},
       {"formatSource", plan.formatSource},
     }},
    {"loadMessages", toJson(messages)},
  };
}

void documentOpen(CallContext& context, const Args& args, ToolCompletion completion)
{
  auto& host = context.host();

  auto planResult = planOpen(context, args);
  if (planResult.is_error())
  {
    completion(errorOf(planResult));
    return;
  }
  auto plan = planResult.value();

  if (const auto open = findOpenDocument(host, plan.path))
  {
    // Opening a map twice would create two documents for the same file
    plan.gameSource = "alreadyOpen";
    plan.formatSource = "alreadyOpen";
    plan.gameInfo = nullptr;
    if (!context.dryRun())
    {
      activate(context, *open);
    }
    completion(openResult(context, *open, true, plan, {}));
    return;
  }

  const auto replaced = host.documentHost().documentToReplace();
  if (replaced)
  {
    if (auto error = checkReplace(context, *replaced))
    {
      completion(*error);
      return;
    }
    if (
      auto error = handleUnsavedChanges(
        context, *replaced, args.get<std::string>("unsavedChanges"), "replaced"))
    {
      completion(*error);
      return;
    }
  }

  warnIfGamePathInvalid(context, *plan.gameInfo);

  const auto formatDescription = plan.format == mdl::MapFormat::Unknown
                                   ? std::string{"auto-detected format"}
                                   : mdl::formatName(plan.format) + " format";
  if (context.dryRun())
  {
    completion(Json{
      {"wouldDo",
       fmt::format(
         "open {} as a {} map in {}{}",
         plan.path,
         plan.gameInfo->gameConfig.name,
         formatDescription,
         replaced ? " replacing " + describeDocument(*replaced) : "")},
    });
    return;
  }

  context.progress(
    0,
    3,
    fmt::format(
      "Opening {} as {} ({})",
      plan.path.filename(),
      plan.gameInfo->gameConfig.name,
      formatDescription));

  // Load in a later step so that a cancellation sent meanwhile is processed
  context.defer([&context, plan, completion]() {
    if (context.cancelled())
    {
      completion(makeError(
        ErrorCode::Cancelled, "The call was cancelled before the map was loaded."));
      return;
    }

    context.progress(1, 3, fmt::format("Loading {}", plan.path.filename()));
    const auto loaded =
      context.host().documentHost().loadDocument(*plan.gameInfo, plan.format, plan.path);
    if (loaded.is_error())
    {
      completion(makeError(
        ErrorCode::IoError,
        fmt::format("Could not load {}: {}", plan.path, errorMessage(loaded)),
        "If the game or format was detected wrongly, pass 'game' and 'format' "
        "explicitly."));
      return;
    }

    const auto& opened = loaded.value();
    context.progress(3, 3, fmt::format("Loaded {}", plan.path.filename()));
    activate(context, opened.document);
    completion(openResult(context, opened.document, false, plan, opened.messages));
  });
}

// document_save, document_save_as

/** Saving writes or carries the map manifest (DocumentState); report a failure. */
void warnManifestNotWritten(CallContext& context)
{
  if (const auto& error = context.documentState().manifest.saveError())
  {
    context.warn("MANIFEST_NOT_WRITTEN", *error);
  }
}

ToolResult documentSave(CallContext& context, const Args&)
{
  auto& map = context.map();
  if (!map.persistent())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} has never been saved and has no file path.", context.documentInfo().id),
      "Use document_save_as {\"path\": \"/absolute/path/name.map\"}.");
  }

  if (context.dryRun())
  {
    return Json{
      {"wouldDo", fmt::format("save {}", map.path())}, {"path", map.path().string()}};
  }

  if (const auto saved = map.save(); saved.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not save {}: {}", map.path(), errorMessage(saved)),
      "Check that the file and folder are writable, or save elsewhere with "
      "document_save_as.");
  }
  warnManifestNotWritten(context);
  return Json{
    {"path", map.path().string()},
    {"savedAt", isoTime(std::chrono::system_clock::now())},
  };
}

ToolResult documentSaveAs(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto overwrite = args.get<bool>("overwrite");

  auto pathResult = absolutePathArgument(args, "path");
  if (pathResult.is_error())
  {
    return errorOf(pathResult);
  }
  const auto ownFile = map.persistent() && pathResult.value() == map.path();

  // saving over the document's own file is a normal save
  auto checked = outputPath(args, "path", overwrite || ownFile);
  if (checked.is_error())
  {
    return errorOf(checked);
  }
  const auto path = checked.value();
  const auto existed = pathExists(path);

  if (path.extension() != ".map")
  {
    context.warn(
      "UNUSUAL_EXTENSION",
      fmt::format(
        "{} does not end with .map; compilers and the editor expect .map.", path));
  }

  const auto previousPath = map.persistent() ? Json(map.path().string()) : Json(nullptr);
  if (context.dryRun())
  {
    return Json{
      {"wouldDo",
       fmt::format(
         "save {} as {}{}",
         context.documentInfo().id,
         path,
         existed ? " (overwrite)" : "")},
      {"path", path.string()},
    };
  }

  if (const auto saved = map.saveAs(path); saved.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not save {}: {}", path, errorMessage(saved)),
      "Check that the folder is writable.");
  }
  warnManifestNotWritten(context);
  return Json{
    {"path", map.path().string()},
    {"previousPath", previousPath},
    {"overwritten", existed},
    {"savedAt", isoTime(std::chrono::system_clock::now())},
  };
}

// document_close, document_revert

/** A COMPILE_RUNNING error if a compile_run run of the document is running. */
std::optional<ToolError> compileRunError(
  CallContext& context, const DocumentInfo& document, const std::string_view action)
{
  if (const auto* run = context.server().compileRuns->running(*document.document))
  {
    return makeError(
      ErrorCode::CompileRunning,
      fmt::format(
        "Compile {} of {} is running; the document cannot be {} meanwhile.",
        run->id,
        describeDocument(document),
        action),
      fmt::format(
        "Wait until compile_status {{\"run\": \"{0}\"}} reports that it has ended, or "
        "stop it with compile_cancel {{\"run\": \"{0}\"}}.",
        run->id));
  }
  return std::nullopt;
}

ToolResult documentClose(CallContext& context, const Args& args)
{
  const auto document = context.documentInfo();
  const auto policy = args.get<std::string>("unsavedChanges");

  if (auto error = compileRunError(context, document, "closed"))
  {
    return *error;
  }

  if (context.host().isCompileRunning(context.document()))
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format("A compilation of {} is running.", describeDocument(document)),
      "Wait until compile_status reports that the run has ended, or stop it with "
      "compile_cancel, then close the document.");
  }

  const auto modified = context.map().modified();
  if (auto error = handleUnsavedChanges(context, document, policy, "closed"))
  {
    return *error;
  }

  const auto saved = modified && policy == UnsavedSave;
  const auto discarded = modified && policy == UnsavedDiscard;
  if (context.dryRun())
  {
    return Json{
      {"wouldDo",
       fmt::format(
         "close {}{}",
         describeDocument(document),
         saved       ? " after saving it"
         : discarded ? " discarding unsaved changes"
                     : "")},
      {"closed", document.id},
    };
  }

  if (context.session().activeDocumentId == document.id)
  {
    context.server().setActiveDocument(context.session(), std::nullopt);
  }
  context.host().documentHost().closeDocument(context.document());

  return Json{
    {"closed", document.id},
    {"saved", saved},
    {"discardedChanges", discarded},
  };
}

ToolResult documentRevert(CallContext& context, const Args& args)
{
  const auto document = context.documentInfo();
  auto& map = context.map();
  if (!map.persistent())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} has never been saved, so there is no file to revert to.", document.id),
      "Use document_close with unsavedChanges: 'discard' to drop the document instead.");
  }

  // reloading the document would cancel the compilation
  if (auto error = compileRunError(context, document, "reverted"))
  {
    return *error;
  }

  if (
    auto error = handleUnsavedChanges(
      context, document, args.get<std::string>("unsavedChanges"), "reverted"))
  {
    return *error;
  }

  if (context.dryRun())
  {
    return Json{
      {"wouldDo", fmt::format("reload {} from {}", document.id, map.path())},
      {"idsInvalidated", true},
    };
  }

  if (const auto reloaded = context.document().reload(); reloaded.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not reload {}: {}", document.id, errorMessage(reloaded)),
      "Check that the file still exists and is readable.");
  }
  return Json{
    {"document", documentInfo(context.server(), document, context.session())},
    {"idsInvalidated", true},
    {"loadMessages", toJson(context.loggedProblems())},
  };
}

// document_recent, map_files_list

ToolResult documentRecent(CallContext& context, const Args&)
{
  auto items = Json::array();
  for (const auto& path : context.host().documentHost().recentDocuments())
  {
    auto item = fileEntry(context.server(), path);
    item["exists"] = pathExists(path);
    items.push_back(std::move(item));
  }
  return Json{{"items", std::move(items)}};
}

ToolResult mapFilesList(CallContext& context, const Args& args)
{
  auto folderResult = absolutePathArgument(args, "folder");
  if (folderResult.is_error())
  {
    return errorOf(folderResult);
  }
  const auto folder = folderResult.value();

  if (fs::Disk::pathInfo(folder) != fs::PathInfo::Directory)
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("{} is not a folder.", folder),
      "Pass an existing folder, e.g. the folder of the active document.");
  }

  auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto pattern = args.get<std::string>("pattern");
  const auto recursive = args.get<bool>("recursive");
  const auto matcher = [&](const auto& path, const auto& getPathInfo) {
    return getPathInfo(path) == fs::PathInfo::File
           && fs::makeFilenamePathMatcher(pattern)(path, getPathInfo);
  };

  const auto found = fs::Disk::find(
    folder, recursive ? fs::TraversalMode::Recursive : fs::TraversalMode::Flat, matcher);
  if (found.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not list {}: {}", folder, errorMessage(found)),
      "Check that the folder is readable.");
  }

  auto paths = found.value();
  std::ranges::sort(paths, [](const auto& lhs, const auto& rhs) {
    return kdl::ci::string_less_natural{}(lhs.string(), rhs.string());
  });

  auto items = std::vector<Json>{};
  for (const auto& path : paths)
  {
    items.push_back(fileEntry(context.server(), path));
  }
  return makePage(items, request.value(), 0);
}

// document_export_map, document_export_obj

ToolResult documentExportMap(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto pathResult = outputPath(args, "path", args.get<bool>("overwrite"));
  if (pathResult.is_error())
  {
    return errorOf(pathResult);
  }
  const auto path = pathResult.value();

  if (map.persistent() && path == map.path())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The export must not overwrite the document's own file.",
      "Choose another path, or use document_save to save the document itself.");
  }

  const auto strip = args.get<bool>("stripEditorProperties");
  auto omittedLayers = Json::array();
  for (const auto* layerNode : map.worldNode().allLayersUserSorted())
  {
    if (layerNode->layer().omitFromExport())
    {
      omittedLayers.push_back(
        Json{{"id", context.ids().format(*layerNode)}, {"name", layerNode->name()}});
    }
  }

  if (context.dryRun())
  {
    return Json{
      {"wouldDo", fmt::format("export {} to {}", context.documentInfo().id, path)},
      {"path", path.string()},
      {"omittedLayers", std::move(omittedLayers)},
    };
  }

  const auto options = mdl::MapExportOptions{path, strip, std::nullopt, std::nullopt};
  if (const auto exported = map.exportAs(options); exported.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not export to {}: {}", path, errorMessage(exported)),
      "Check that the folder is writable.");
  }
  return Json{
    {"path", path.string()},
    {"omittedLayers", omittedLayers},
    {"strippedEditorProperties", strip},
  };
}

ToolResult documentExportObj(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto overwrite = args.get<bool>("overwrite");
  auto pathResult = outputPath(args, "path", overwrite);
  if (pathResult.is_error())
  {
    return errorOf(pathResult);
  }
  const auto path = pathResult.value();
  const auto mtlPath = kdl::path_replace_extension(path, ".mtl");
  if (!overwrite && pathExists(mtlPath))
  {
    return makeError(
      ErrorCode::FileExists,
      fmt::format("{} already exists.", mtlPath),
      "Pass overwrite: true to replace the OBJ and MTL files, or choose another path.");
  }

  const auto mode = args.get<std::string>("materialPaths") == "relativeToExportPath"
                      ? mdl::ObjMtlPathMode::RelativeToExportPath
                      : mdl::ObjMtlPathMode::RelativeToGamePath;

  if (context.dryRun())
  {
    return Json{
      {"wouldDo", fmt::format("export {} as OBJ to {}", context.documentInfo().id, path)},
      {"objPath", path.string()},
      {"mtlPath", mtlPath.string()},
    };
  }

  if (const auto exported = map.exportAs(mdl::ObjExportOptions{path, mode});
      exported.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not export to {}: {}", path, errorMessage(exported)),
      "Check that the folder is writable.");
  }
  return Json{{"objPath", path.string()}, {"mtlPath", mtlPath.string()}};
}

// autosave_list

ToolResult autosaveList(CallContext& context, const Args& args)
{
  auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto& map = context.map();
  if (!map.persistent())
  {
    auto page = makePage({}, request.value(), 0);
    page["folder"] = nullptr;
    page["note"] =
      "The document was never saved. Autosave backups are only made for saved maps.";
    return page;
  }

  const auto folder = map.path().parent_path() / "autosave";
  auto paths = std::vector<std::filesystem::path>{};
  if (fs::Disk::pathInfo(folder) == fs::PathInfo::Directory)
  {
    paths =
      fs::Disk::find(
        folder, fs::TraversalMode::Flat, mdl::makeBackupPathMatcher(map.path().stem()))
      | kdl::value_or(std::vector<std::filesystem::path>{});
  }

  // backups are named <name>.<n>.map; higher numbers are newer
  const auto backupNumber = [](const auto& path) {
    const auto extension = path.stem().extension().string();
    return extension.size() > 1 ? kdl::str_to_size(extension.substr(1)).value_or(0) : 0;
  };
  std::ranges::sort(paths, [&](const auto& lhs, const auto& rhs) {
    return backupNumber(lhs) > backupNumber(rhs);
  });

  auto items = std::vector<Json>{};
  for (const auto& path : paths)
  {
    auto item = fileEntry(context.server(), path);
    item.erase("openAs");
    item["number"] = backupNumber(path);
    items.push_back(std::move(item));
  }

  auto page = makePage(items, request.value(), 0);
  page["folder"] = folder.string();
  return page;
}

Schema fileItemSchema(std::vector<Field> extraFields = {})
{
  auto fields = std::vector<Field>{
    field("path", string()).required().describe("Absolute path"),
    field("name", string()).describe("File name"),
    field("size", any()).describe("Size in bytes, or null if unreadable"),
    field("modified", any()).describe("Last modification time, ISO 8601 UTC"),
    field("openAs", any()).describe("Handle of the document showing this file, or null"),
  };
  for (auto& extra : extraFields)
  {
    fields.push_back(std::move(extra));
  }
  return object(std::move(fields));
}

Schema wouldDoField()
{
  return string().describe("Dry run only: what the call would do");
}

} // namespace

Json documentInfo(
  ServerState& server, const DocumentInfo& document, const Session& session)
{
  auto result = documentSummary(server, document, session);

  const auto& map = document.document->map();
  const auto& gameInfo = map.gameInfo();
  result["gamePath"] = gamePath(gameInfo).string();
  result["gamePathValid"] = isGamePathValid(gameInfo);
  result["worldBounds"] = toJson(map.worldBounds());
  result["mods"] = modsJson(map);
  result["entityDefinitions"] = entityDefinitionsJson(map);
  result["materials"] = materialsJson(map);
  result["softBounds"] = softBoundsJson(map);
  return result;
}

void registerDocumentTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"document_new"}
      .title("New Document")
      .description(
        "Creates a new, unsaved map for a game and opens it in a new editor window (not "
        "undoable); it becomes the active document of this session only (other sessions "
        "keep theirs), and never replaces another session's document. The map starts "
        "from the game's initial map template for the format if it has one (otherwise "
        "with a single 128x128x32 brush, or empty for some games); 'initialObjects' "
        "lists these objects, delete them with objects_delete if you build from scratch. "
        "game_list gives game and format names; save it with document_save_as. Example: "
        "{\"game\": \"Quake\", \"format\": \"Valve\"}")
      .input(object({
        field("game", string())
          .required()
          .describe("Game name from game_list, e.g. 'Quake'"),
        field("format", string())
          .describe("Map format, e.g. 'Valve' or 'Standard'; default: the game's first"),
        unsavedChangesField("replaced (only in single-window mode)"),
      }))
      .output(object({
        field("document", documentInfoSchema()).describe("The new document"),
        field("initialMap", any()).describe("The template file used, or null"),
        field("initialObjects", array(string()))
          .describe(
            "Ids of the objects the new map starts with (the template's brush); also "
            "reported as an INITIAL_OBJECTS warning"),
        field("replaced", any())
          .describe("Handle of the document that was replaced (single-window mode)"),
        field("loadMessages", logMessagesSchema()),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::None)
      .handler(documentNew));

  registry.add(
    ToolDef{"document_open"}
      .title("Open Document")
      .description(
        "Opens a map file in a new editor window and makes it the active document of "
        "this session (other sessions keep theirs); it never replaces another session's "
        "document. The game and format are read from the file's header comments; pass "
        "'game' and "
        "'format' when the file has none or to override them (the format is otherwise "
        "detected from the content). Returns the document and any warnings logged while "
        "loading (missing materials, definition problems). If the file is already open, "
        "returns that document (alreadyOpen: true) without reloading; use "
        "document_revert to reload. Reports progress; can be cancelled until loading "
        "starts. Example: {\"path\": \"/home/me/maps/e1m1.map\"}")
      .input(object({
        field("path", string()).required().describe("Absolute path of the .map file"),
        field("game", string()).describe("Game name from game_list; default: detected"),
        field("format", string()).describe("Map format name; default: detected"),
        unsavedChangesField("replaced (only in single-window mode)"),
      }))
      .output(object({
        field("document", documentInfoSchema()),
        field("alreadyOpen", boolean()),
        field("detection", any())
          .describe("{game, gameSource, formatSource}: where game and format came from "
                    "('argument', 'fileHeader', 'detected', 'alreadyOpen')"),
        field("loadMessages", logMessagesSchema())
          .describe("Warnings and errors logged while loading"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::None)
      .openWorld()
      .asyncHandler(documentOpen));

  registry.add(
    ToolDef{"document_save"}
      .title("Save Document")
      .description(
        "Saves the document to its file (not undoable) and returns the path and time. "
        "Fails with INVALID_ARGUMENT for a document that was never saved; use "
        "document_save_as for that. Example: {}")
      .input(object({}))
      .output(object({
        field("path", string()).required(),
        field("savedAt", string()).describe("ISO 8601 UTC time"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .openWorld()
      .handler(documentSave));

  registry.add(
    ToolDef{"document_save_as"}
      .title("Save Document As")
      .description(
        "Saves the document under a new absolute path (not undoable); the document then "
        "refers to the new file. Fails with FILE_EXISTS if the file exists unless "
        "overwrite is true; the folder must exist. Use document_export_map to write a "
        "copy without switching files. Example: {\"path\": \"/home/me/maps/new.map\"}")
      .input(object({
        field("path", string()).required().describe("Absolute path, usually ending .map"),
        field("overwrite", boolean().defaultsTo(false))
          .describe("Replace an existing file"),
      }))
      .output(object({
        field("path", string()).required(),
        field("previousPath", any()).describe("The former path, or null"),
        field("overwritten", boolean()).describe("Whether an existing file was replaced"),
        field("savedAt", string()).describe("ISO 8601 UTC time"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .destructive()
      .openWorld()
      .handler(documentSaveAs));

  registry.add(
    ToolDef{"document_close"}
      .title("Close Document")
      .description(
        "Closes the document and its window (not undoable): the one named by "
        "'document', or this session's active document; it never falls back to the "
        "focused window. Other sessions that had it active get ACTIVE_DOCUMENT_CLOSED "
        "until they choose another one. With unsaved changes, "
        "'unsavedChanges' must say whether to save or discard them; otherwise the call "
        "fails with UNSAVED_CHANGES. An open agent transaction on it is rolled back; "
        "fails with COMPILE_RUNNING while it is being compiled. Example: "
        "{\"document\": \"doc:2\", \"unsavedChanges\": \"save\"}")
      .input(object({unsavedChangesField("closed")}))
      .output(object({
        field("closed", string()).required().describe("Handle of the closed document"),
        field("saved", boolean()).describe("Whether it was saved before closing"),
        field("discardedChanges", boolean())
          .describe("Whether unsaved changes were dropped"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .focusFallback(false)
      .destructive()
      .handler(documentClose));

  registry.add(
    ToolDef{"document_revert"}
      .title("Revert Document")
      .description(
        "Reloads the document (the one named by 'document', or this session's active "
        "document; never the focused window) from its file, dropping the undo history "
        "(not undoable). "
        "With unsaved changes, pass unsavedChanges: 'discard'. All object ids of the "
        "document become invalid (idsInvalidated); layer and group ids are remapped, "
        "so look objects up again (objects_find, map_tree). Example: "
        "{\"unsavedChanges\": \"discard\"}")
      .input(object({unsavedChangesField("reverted")}))
      .output(object({
        field("document", documentInfoSchema()),
        field("idsInvalidated", boolean())
          .describe("True: object ids from before the call no longer resolve"),
        field("loadMessages", logMessagesSchema())
          .describe("Warnings and errors logged while loading"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .focusFallback(false)
      .destructive()
      .handler(documentRevert));

  registry.add(
    ToolDef{"document_recent"}
      .title("Recent Documents")
      .description(
        "Lists the editor's recently opened map files (read-only), most recent first, "
        "with size, modification time, whether each still exists and which document "
        "shows it (openAs). Open one with document_open. Example: {}")
      .input(object({}))
      .output(object({
        field(
          "items",
          array(fileItemSchema({
            field("exists", boolean()).describe("Whether the file still exists"),
          })))
          .required(),
      }))
      .mutation(Mutation::None)
      .handler(documentRecent));

  registry.add(
    ToolDef{"map_files_list"}
      .title("List Map Files")
      .description(
        "Lists files in a folder (read-only) whose names match a glob pattern "
        "(case-insensitive, default '*.map'), optionally including subfolders, with "
        "size, modification time and the document showing each (openAs). Paths are "
        "absolute and sorted naturally; open one with document_open. Example: "
        "{\"folder\": \"/home/me/quake/id1/maps\", \"recursive\": true}")
      .input(object({
        field("folder", string()).required().describe("Absolute folder path"),
        field("pattern", string().defaultsTo("*.map"))
          .describe("File name glob with * and ?, e.g. '*.bsp'"),
        field("recursive", boolean().defaultsTo(false)).describe("Include subfolders"),
      }))
      .output(object({
        field("items", array(fileItemSchema())).required(),
        field("total", integer()).required().describe("Number of matching files"),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
      }))
      .mutation(Mutation::None)
      .paginated()
      .openWorld()
      .handler(mapFilesList));

  registry.add(
    ToolDef{"document_export_map"}
      .title("Export Map")
      .description(
        "Writes a copy of the map to another .map file (not undoable), leaving out "
        "layers marked omitFromExport (layer_set_state). The document keeps its own path "
        "and modified state; the document's own file cannot be the target. Fails with "
        "FILE_EXISTS unless overwrite is true. Example: "
        "{\"path\": \"/home/me/maps/export/e1m1.map\", \"stripEditorProperties\": true}")
      .input(object({
        field("path", string()).required().describe("Absolute path of the new file"),
        field("overwrite", boolean().defaultsTo(false))
          .describe("Replace an existing file"),
        field("stripEditorProperties", boolean().defaultsTo(false))
          .describe("Remove TrenchBroom-only properties (_tb_*: layers, groups, ...)"),
      }))
      .output(object({
        field("path", string()).required(),
        field("omittedLayers", array(any())).describe("Layers left out: {id, name}"),
        field("strippedEditorProperties", boolean())
          .describe("Whether _tb_* properties were removed"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .openWorld()
      .handler(documentExportMap));

  registry.add(
    ToolDef{"document_export_obj"}
      .title("Export OBJ")
      .description(
        "Exports the map geometry as a Wavefront OBJ file plus an MTL file next to it "
        "(same name, .mtl; not undoable). materialPaths chooses how material image paths "
        "are written in the MTL: relative to the game folder (default) or to the export "
        "folder. Fails with FILE_EXISTS unless overwrite is true. Example: "
        "{\"path\": \"/home/me/export/e1m1.obj\", \"materialPaths\": "
        "\"relativeToExportPath\"}")
      .input(object({
        field("path", string()).required().describe("Absolute path of the .obj file"),
        field("overwrite", boolean().defaultsTo(false))
          .describe("Replace existing OBJ and MTL files"),
        field(
          "materialPaths",
          enumOf({"relativeToGamePath", "relativeToExportPath"})
            .defaultsTo("relativeToGamePath"))
          .describe("How material paths are written in the MTL file"),
      }))
      .output(object({
        field("objPath", string()).required().describe("Path of the written OBJ file"),
        field("mtlPath", string()).required().describe("Path of the written MTL file"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .openWorld()
      .handler(documentExportObj));

  registry.add(
    ToolDef{"autosave_list"}
      .title("List Autosaves")
      .description(
        "Lists the autosave backups of the document (read-only; in the 'autosave' "
        "folder next to the map, named <map>.<n>.map), newest first. Backups are only "
        "made for saved maps. Open a backup with document_open, then document_save_as "
        "to restore it. Example: {}")
      .input(object({}))
      .output(object({
        field(
          "items",
          array(fileItemSchema({
            field("number", integer()).describe("Backup number <n>; higher is newer"),
          })))
          .required(),
        field("total", integer()).required().describe("Number of backups"),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
        field("folder", any()).describe("The autosave folder, or null"),
        field("note", string()).describe("Why the list is empty, if it is"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .paginated()
      .handler(autosaveList));
}

} // namespace tb::mcp
