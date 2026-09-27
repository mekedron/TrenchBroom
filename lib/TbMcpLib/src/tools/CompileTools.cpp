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

#include "mcp/tools/CompileTools.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/CompileRuns.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/ServerState.h"
#include "mcp/Session.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/CompileLog.h"
#include "mcp/tools/CompileUtils.h"
#include "mcp/tools/GameTools.h"
#include "mdl/BrushNode.h"
#include "mdl/CompilationConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/PointTrace.h"
#include "mdl/PortalFile.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <chrono>

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr size_t MaxReportedMessages = 50;
constexpr size_t MaxReportedPoints = 1000;
constexpr size_t NearestEntityCount = 3;

Schema wouldDoField()
{
  return string().describe("Dry run only: what the call would do");
}

std::string joined(const std::vector<std::string>& strings)
{
  return strings.empty() ? "(none)" : kdl::str_join(strings, ", ");
}

// game lookup

/**
 * The game named by the 'game' argument, else the game of the target document. The
 * game manager's copy is returned when possible because it holds the current compile
 * profiles.
 */
Result<const mdl::GameInfo*, ToolError> targetGame(CallContext& context, const Args& args)
{
  auto& host = context.host();
  if (const auto gameName = args.getOptional<std::string>("game"))
  {
    if (const auto* gameInfo = findGame(host, *gameName))
    {
      return gameInfo;
    }
    return unknownGameError(host, *gameName);
  }

  if (!context.hasDocument())
  {
    return makeError(
      ErrorCode::NoDocument,
      "No game was given and no document is open.",
      "Pass 'game', e.g. {\"game\": \"Quake\"}; game_list lists the games.");
  }

  const auto& documentGame = context.map().gameInfo();
  if (const auto* gameInfo = host.gameManager().gameInfo(documentGame.gameConfig.name))
  {
    return gameInfo;
  }
  return &documentGame;
}

Json familyJson(const mdl::GameConfig& gameConfig)
{
  const auto family = compileFamily(gameConfig);
  return family ? Json(std::string{toString(*family)}) : Json(nullptr);
}

const mdl::CompilationTool* findTool(
  const mdl::GameConfig& gameConfig, const std::string_view name)
{
  const auto it = std::ranges::find_if(
    gameConfig.compilationTools, [&](const auto& tool) { return tool.name == name; });
  return it != gameConfig.compilationTools.end() ? &*it : nullptr;
}

std::vector<std::string> toolNames(const mdl::GameConfig& gameConfig)
{
  auto result = std::vector<std::string>{};
  for (const auto& tool : gameConfig.compilationTools)
  {
    result.push_back(tool.name);
  }
  return result;
}

std::vector<std::string> profileNames(const mdl::GameInfo& gameInfo)
{
  auto result = std::vector<std::string>{};
  for (const auto& profile : gameInfo.compilationConfig.profiles)
  {
    result.push_back(profile.name);
  }
  return result;
}

std::vector<std::string> presetNames(const mdl::GameConfig& gameConfig)
{
  auto result = std::vector<std::string>{};
  for (const auto& preset : compilePresets(gameConfig))
  {
    result.push_back(preset.name);
  }
  return result;
}

const mdl::CompilationProfile* findProfile(
  const mdl::GameInfo& gameInfo, const std::string_view name)
{
  const auto& profiles = gameInfo.compilationConfig.profiles;
  const auto it =
    std::ranges::find_if(profiles, [&](const auto& p) { return p.name == name; });
  return it != profiles.end() ? &*it : nullptr;
}

ToolError unknownPresetError(const mdl::GameConfig& gameConfig, const std::string& name)
{
  const auto names = presetNames(gameConfig);
  return makeError(
    ErrorCode::InvalidArgument,
    names.empty() ? fmt::format("There are no compile presets for {}.", gameConfig.name)
                  : fmt::format(
                      "Unknown compile preset '{}' for {}; available: {}.",
                      name,
                      gameConfig.name,
                      joined(names)),
    names.empty() ? "Save a profile with compile_profile_save and run it by name."
                  : "Use compile_presets_list to see the presets.");
}

ToolError unknownProfileError(const mdl::GameInfo& gameInfo, const std::string& name)
{
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format(
      "Unknown compile profile '{}' for {}; saved profiles: {}.",
      name,
      gameInfo.gameConfig.name,
      joined(profileNames(gameInfo))),
    "Use compile_profiles_list to see the profiles, or run a preset with "
    "{\"preset\": \"normal\"}.");
}

// compile_tools_get, compile_tools_set

Json toolsPayload(const mdl::GameInfo& gameInfo)
{
  auto tools = Json::array();
  auto allConfigured = true;
  for (const auto& tool : gameInfo.gameConfig.compilationTools)
  {
    const auto status = compileToolStatus(tool);
    allConfigured = allConfigured && status.status == "ok";
    tools.push_back(toJson(status));
  }

  return Json{
    {"game", gameInfo.gameConfig.name},
    {"family", familyJson(gameInfo.gameConfig)},
    {"gamePath", gamePath(gameInfo).string()},
    {"tools", std::move(tools)},
    {"allConfigured", allConfigured},
  };
}

std::optional<std::string> toolStatusWarningCode(const std::string& status)
{
  if (status == "notFound")
  {
    return "TOOL_NOT_FOUND";
  }
  if (status == "notAFile")
  {
    return "TOOL_NOT_A_FILE";
  }
  if (status == "notExecutable")
  {
    return "TOOL_NOT_EXECUTABLE";
  }
  return std::nullopt;
}

ToolResult compileToolsGet(CallContext& context, const Args& args)
{
  auto gameInfo = targetGame(context, args);
  if (gameInfo.is_error())
  {
    return errorOf(gameInfo);
  }
  return toolsPayload(*gameInfo.value());
}

ToolResult compileToolsSet(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameConfig = found.value()->gameConfig;
  auto* gameInfo = context.host().gameManager().gameInfo(gameConfig.name);
  if (!gameInfo)
  {
    return makeError(
      ErrorCode::UnsupportedInHost,
      fmt::format("The game {} is not configured in this editor.", gameConfig.name),
      "Use game_list to see the configured games.");
  }

  const auto& tools = args.get<Json>("tools");
  if (tools.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'tools' is empty.",
      fmt::format(
        "Pass tool paths, e.g. {{\"tools\": {{\"{}\": \"/opt/tools/{}\"}}}}; {} uses: "
        "{}.",
        toolNames(gameConfig).empty() ? "qbsp" : toolNames(gameConfig).front(),
        toolNames(gameConfig).empty() ? "qbsp" : toolNames(gameConfig).front(),
        gameConfig.name,
        joined(toolNames(gameConfig))));
  }

  // validate everything before changing anything
  auto changes = std::vector<std::pair<mdl::CompilationTool*, std::filesystem::path>>{};
  for (const auto& [name, value] : tools.items())
  {
    auto it = std::ranges::find_if(
      gameInfo->gameConfig.compilationTools,
      [&](const auto& tool) { return tool.name == name; });
    if (it == gameInfo->gameConfig.compilationTools.end())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "{} has no compile tool '{}'; its tools are: {}.",
          gameConfig.name,
          name,
          joined(toolNames(gameConfig))),
        "Use compile_tools_get to see the tools of the game.");
    }
    if (!value.is_string())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The path of '{}' must be a string.", name),
        "Pass an absolute path, or '' to clear it.");
    }

    auto path = std::filesystem::path{value.get<std::string>()};
    if (!path.empty() && !path.is_absolute())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The path of '{}' must be absolute: {}", name, path),
        "Pass the absolute path of the executable, e.g. /opt/ericw-tools/bin/qbsp.");
    }
    changes.emplace_back(&*it, path.lexically_normal());
  }

  for (const auto& [tool, path] : changes)
  {
    if (!path.empty())
    {
      if (const auto code = toolStatusWarningCode(checkCompileToolPath(path)))
      {
        context.warn(
          *code,
          fmt::format(
            "The path of '{}' is {}: {}",
            tool->name,
            *code == "TOOL_NOT_FOUND"    ? "missing"
            : *code == "TOOL_NOT_A_FILE" ? "not a file"
                                         : "not executable",
            path));
      }
    }
  }

  auto changed = Json::array();
  for (const auto& [tool, path] : changes)
  {
    if (pref(tool->pathPreference) != path)
    {
      changed.push_back(tool->name);
    }
  }

  if (context.dryRun())
  {
    auto descriptions = std::vector<std::string>{};
    for (const auto& [tool, path] : changes)
    {
      descriptions.push_back(fmt::format("{} = '{}'", tool->name, path));
    }
    auto result = toolsPayload(*gameInfo);
    result["changed"] = std::move(changed);
    result["wouldDo"] = fmt::format(
      "set the compile tool paths of {}: {}", gameConfig.name, joined(descriptions));
    return result;
  }

  for (const auto& [tool, path] : changes)
  {
    setPref(tool->pathPreference, path);
  }
  if (!changed.empty())
  {
    // the game configuration lists the compile tool paths
    context.server().notifyResourceUpdated(gameConfigUri(gameConfig.name));
  }

  auto result = toolsPayload(*gameInfo);
  result["changed"] = std::move(changed);
  return result;
}

// compile_presets_list, compile_profiles_list

ToolResult compilePresetsList(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameConfig = found.value()->gameConfig;

  auto presets = Json::array();
  for (const auto& preset : compilePresets(gameConfig))
  {
    auto missing = Json::array();
    for (const auto& name : preset.tools)
    {
      const auto* tool = findTool(gameConfig, name);
      if (!tool || compileToolStatus(*tool).status != "ok")
      {
        missing.push_back(name);
      }
    }

    presets.push_back(Json{
      {"name", preset.name},
      {"description", preset.description},
      {"profile", toJson(preset.profile)},
      {"tools", preset.tools},
      {"missingTools", std::move(missing)},
    });
  }

  if (presets.empty())
  {
    context.warn(
      "NO_PRESETS",
      fmt::format(
        "There are no compile presets for {}; its compile tools ({}) do not match a "
        "known "
        "tool chain.",
        gameConfig.name,
        joined(toolNames(gameConfig))));
  }

  return Json{
    {"game", gameConfig.name},
    {"family", familyJson(gameConfig)},
    {"presets", std::move(presets)},
    {"summary", compilePresetsSummary()},
  };
}

ToolResult compileProfilesList(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameInfo = *found.value();

  if (gameInfo.compilationConfigParseFailed)
  {
    context.warn(
      "COMPILATION_CONFIG_INVALID",
      fmt::format(
        "The compile profiles of {} could not be read; saving a profile replaces them "
        "(the editor keeps a backup of the old file).",
        gameInfo.gameConfig.name));
  }

  auto profiles = Json::array();
  for (const auto& profile : gameInfo.compilationConfig.profiles)
  {
    auto json = toJson(profile);
    json["enabledTaskCount"] = enabledTasks(profile).size();
    profiles.push_back(std::move(json));
  }

  return Json{
    {"game", gameInfo.gameConfig.name},
    {"profiles", std::move(profiles)},
  };
}

// compile_profile_save, compile_profile_delete

Result<void, ToolError> writeCompilationConfig(
  CallContext& context, const mdl::GameInfo& gameInfo, mdl::CompilationConfig config)
{
  auto nullLogger = NullLogger{};
  auto& logger = context.hasDocument() ? context.document().logger()
                                       : static_cast<Logger&>(nullLogger);
  const auto& gameName = gameInfo.gameConfig.name;
  const auto result = context.host().gameManager().updateCompilationConfig(
    gameName, std::move(config), logger);
  if (result.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format(
        "Could not save the compile profiles of {}: {}", gameName, errorMessage(result)),
      "Check that the user configuration folder of TrenchBroom is writable.");
  }
  return Result<void, ToolError>{};
}

ToolResult compileProfileSave(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameInfo = *found.value();
  const auto& gameConfig = gameInfo.gameConfig;

  if (args.has("profile") == args.has("preset"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass exactly one of 'profile' and 'preset'.",
      "Save a preset with {\"preset\": \"normal\"}, or a custom profile with "
      "{\"profile\": {\"name\": ..., \"tasks\": [...]}}.");
  }

  auto profile = mdl::CompilationProfile{};
  if (const auto presetName = args.getOptional<std::string>("preset"))
  {
    const auto preset = findCompilePreset(gameConfig, *presetName);
    if (!preset)
    {
      return unknownPresetError(gameConfig, *presetName);
    }
    profile = preset->profile;
  }
  else
  {
    auto converted = compilationProfileFromJson(args.get<Json>("profile"));
    if (converted.is_error())
    {
      return errorOf(converted);
    }
    profile = std::move(converted.value());
  }

  if (const auto name = args.getOptional<std::string>("name"))
  {
    profile.name = *name;
  }
  if (kdl::str_trim(profile.name).empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The profile name must not be empty.",
      "Pass a name, e.g. {\"name\": \"My compile\"}.");
  }

  auto config = gameInfo.compilationConfig;
  auto it = std::ranges::find_if(
    config.profiles, [&](const auto& p) { return p.name == profile.name; });
  const auto created = it == config.profiles.end();
  if (!created && !args.get<bool>("overwrite"))
  {
    return makeError(
      ErrorCode::FileExists,
      fmt::format(
        "{} already has a compile profile named '{}'.", gameConfig.name, profile.name),
      "Pass overwrite: true to replace it, or choose another name.");
  }

  auto result = Json{
    {"game", gameConfig.name},
    {"profile", toJson(profile)},
    {"created", created},
  };

  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format(
      "{} the compile profile '{}' of {} ({} tasks)",
      created ? "create" : "replace",
      profile.name,
      gameConfig.name,
      profile.tasks.size());
    return result;
  }

  if (created)
  {
    config.profiles.push_back(std::move(profile));
  }
  else
  {
    *it = std::move(profile);
  }

  if (auto written = writeCompilationConfig(context, gameInfo, std::move(config));
      written.is_error())
  {
    return errorOf(written);
  }
  return result;
}

ToolResult compileProfileDelete(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameInfo = *found.value();
  const auto name = args.get<std::string>("name");

  if (!findProfile(gameInfo, name))
  {
    return unknownProfileError(gameInfo, name);
  }

  auto config = gameInfo.compilationConfig;
  std::erase_if(config.profiles, [&](const auto& p) { return p.name == name; });

  auto remaining = std::vector<std::string>{};
  for (const auto& profile : config.profiles)
  {
    remaining.push_back(profile.name);
  }

  auto result = Json{
    {"game", gameInfo.gameConfig.name},
    {"deleted", name},
    {"remaining", remaining},
  };

  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format(
      "delete the compile profile '{}' of {}", name, gameInfo.gameConfig.name);
    return result;
  }

  if (auto written = writeCompilationConfig(context, gameInfo, std::move(config));
      written.is_error())
  {
    return errorOf(written);
  }
  return result;
}

// compile_status

std::filesystem::path absoluteIn(
  const std::filesystem::path& path, const std::filesystem::path& base)
{
  return (path.is_absolute() ? path : base / path).lexically_normal();
}

bool hasExtension(const std::filesystem::path& path, const std::string_view extension)
{
  return kdl::ci::str_is_equal(path.extension().string(), extension);
}

/** The directory relative paths of the log are resolved against: the work directory. */
std::filesystem::path baseDirectory(const CompileRun& run)
{
  return run.mapPath.parent_path();
}

/** The leak's point file, see compile_status. */
std::optional<std::filesystem::path> leakPointFile(
  const CompileRun& run, const CompileLogAnalysis& analysis)
{
  const auto base = baseDirectory(run);
  const auto exportedMap = !analysis.exportedMaps.empty()
                             ? absoluteIn(analysis.exportedMaps.front(), base)
                             : run.mapPath;

  if (analysis.leak && analysis.leak->pointFile)
  {
    return absoluteIn(*analysis.leak->pointFile, exportedMap.parent_path());
  }
  if (exportedMap.empty())
  {
    return std::nullopt;
  }

  auto pts = exportedMap;
  pts.replace_extension(".pts");
  auto lin = exportedMap;
  lin.replace_extension(".lin");
  return !pathExists(pts) && pathExists(lin) ? lin : pts;
}

Json messagesJson(const std::vector<CompileMessage>& messages)
{
  auto result = Json::array();
  for (size_t i = 0; i < messages.size() && i < MaxReportedMessages; ++i)
  {
    const auto& message = messages[i];
    result.push_back(Json{
      {"message", message.message},
      {"line", message.line},
      {"task", message.task ? Json(*message.task) : Json(nullptr)},
    });
  }
  return result;
}

Json leakJson(const CompileRun& run, const CompileLogAnalysis& analysis)
{
  if (!analysis.leak)
  {
    return Json{{"detected", false}};
  }

  const auto& leak = *analysis.leak;
  const auto pointFile = leakPointFile(run, analysis);
  const auto pointFileExists = pointFile && pathExists(*pointFile);
  return Json{
    {"detected", true},
    {"entity", leak.entity ? Json(*leak.entity) : Json(nullptr)},
    {"position", leak.position ? toJson(*leak.position) : Json(nullptr)},
    {"line", leak.line},
    {"text", leak.text},
    {"pointFile", pointFile ? Json(pointFile->string()) : Json(nullptr)},
    {"pointFileExists", pointFileExists},
    {"hint",
     pointFile ? fmt::format(
                   "The map leaks: the inside is connected to the void. Load the leak "
                   "path with pointfile_load {{\"path\": \"{}\"}}; the gap is usually "
                   "where the path leaves the map (leavesMapAt). Close it with a brush "
                   "and compile again.",
                   pointFile->string())
               : std::string{"The map leaks. Load the leak path with pointfile_load, "
                             "close the gap with a brush and compile again."}},
  };
}

Json outputJson(const CompileRun& run, const CompileLogAnalysis& analysis)
{
  const auto base = baseDirectory(run);

  auto compiledFile = std::optional<std::filesystem::path>{};
  auto copiedTo = Json::array();
  for (const auto& copy : analysis.copiedFiles)
  {
    if (hasExtension(copy.source, ".bsp"))
    {
      const auto source = absoluteIn(copy.source, base);
      if (!compiledFile)
      {
        compiledFile = source;
      }
      copiedTo.push_back((absoluteIn(copy.target, base) / source.filename()).string());
    }
  }

  if (!compiledFile)
  {
    for (const auto& exportedMap : analysis.exportedMaps)
    {
      auto bsp = absoluteIn(exportedMap, base);
      bsp.replace_extension(".bsp");
      if (pathExists(bsp))
      {
        compiledFile = bsp;
        break;
      }
    }
  }

  return Json{
    {"compiledFile", compiledFile ? Json(compiledFile->string()) : Json(nullptr)},
    {"compiledFileExists", compiledFile && pathExists(*compiledFile)},
    {"copiedTo", std::move(copiedTo)},
  };
}

size_t lineCount(const std::string_view text)
{
  if (text.empty())
  {
    return 0;
  }
  const auto newlines = size_t(std::ranges::count(text, '\n'));
  return text.back() == '\n' ? newlines : newlines + 1;
}

std::string runState(const CompileRun& run, const CompileLogAnalysis& analysis)
{
  if (run.running())
  {
    return "running";
  }
  if (run.cancelRequested || run.documentClosed || analysis.terminated)
  {
    return "cancelled";
  }
  if (analysis.failed || analysis.completedTasks < run.tasks.size())
  {
    return "failed";
  }
  return "succeeded";
}

Json runStatus(
  const CompileRun& run, const std::string& logMode = "tail", const size_t tailLines = 50)
{
  const auto log = run.log();
  const auto analysis = analyzeCompileLog(log, run.tasks, !run.running());

  auto tasks = Json::array();
  auto exitCodes = Json::array();
  for (const auto& task : analysis.tasks)
  {
    auto json = Json{
      {"index", task.index},
      {"type", task.type},
      {"state", std::string{toString(task.state)}},
      {"description", task.description},
    };
    if (task.exitCode)
    {
      json["exitCode"] = *task.exitCode;
      exitCodes.push_back(*task.exitCode);
    }
    tasks.push_back(std::move(json));
  }

  auto currentTask = Json(nullptr);
  if (analysis.currentTask && *analysis.currentTask < analysis.tasks.size())
  {
    const auto& task = analysis.tasks[*analysis.currentTask];
    currentTask = Json{
      {"index", task.index},
      {"type", task.type},
      {"description", task.description},
    };
  }

  const auto end = run.endedAt.value_or(std::chrono::system_clock::now());
  const auto elapsed =
    std::chrono::duration_cast<std::chrono::milliseconds>(end - run.startedAt).count();

  auto logJson = Json{
    {"uri", CompileRuns::logUri(run.id)},
    {"lineCount", lineCount(log)},
  };
  if (logMode == "full")
  {
    logJson["text"] = log;
    logJson["truncated"] = false;
  }
  else if (logMode == "tail")
  {
    auto [text, truncated] = logTail(log, tailLines);
    logJson["text"] = std::move(text);
    logJson["truncated"] = truncated;
  }
  else
  {
    logJson["truncated"] = false;
  }

  return Json{
    {"run", run.id},
    {"document", run.documentId},
    {"documentOpen", run.document != nullptr},
    {"game", run.gameName},
    {"profile", run.profile.name},
    {"preset", run.preset ? Json(*run.preset) : Json(nullptr)},
    {"test", run.test},
    {"state", runState(run, analysis)},
    {"startedAt", isoTime(run.startedAt)},
    {"endedAt", run.endedAt ? Json(isoTime(*run.endedAt)) : Json(nullptr)},
    {"elapsedMs", elapsed},
    {"progress",
     Json{
       {"completedTasks", analysis.completedTasks},
       {"totalTasks", run.tasks.size()},
     }},
    {"currentTask", std::move(currentTask)},
    {"tasks", std::move(tasks)},
    {"exitCodes", std::move(exitCodes)},
    {"errorCount", analysis.errors.size()},
    {"warningCount", analysis.warnings.size()},
    {"errors", messagesJson(analysis.errors)},
    {"errorsTruncated", analysis.errors.size() > MaxReportedMessages},
    {"warnings", messagesJson(analysis.warnings)},
    {"warningsTruncated", analysis.warnings.size() > MaxReportedMessages},
    {"leak", leakJson(run, analysis)},
    {"output", outputJson(run, analysis)},
    {"log", std::move(logJson)},
  };
}

/**
 * The schema of a run status plus the given fields. Without `required`, no field is
 * required (compile_run's dry run returns no status).
 */
Schema runStatusSchema(std::vector<Field> extraFields = {}, const bool required = true)
{
  auto fields = std::vector<Field>{
    field("run", string()).describe("Run handle, e.g. run:3"),
    field("document", string()).describe("Handle of the compiled document at start"),
    field("documentOpen", boolean()),
    field("game", string()),
    field("profile", string()).describe("Name of the compiled profile"),
    field("preset", any()).describe("Preset name, or null for a saved profile"),
    field("test", boolean()),
    field("state", enumOf({"running", "succeeded", "failed", "cancelled"})),
    field("startedAt", string()),
    field("endedAt", any()),
    field("elapsedMs", integer()),
    field(
      "progress",
      object({field("completedTasks", integer()), field("totalTasks", integer())})),
    field("currentTask", any()).describe("{index, type, description} or null"),
    field("tasks", array(any()))
      .describe("{index, type, state, description, exitCode?} per enabled task"),
    field("exitCodes", array(integer())),
    field("errorCount", integer()),
    field("warningCount", integer()),
    field("errors", array(any())).describe("{message, line, task}, at most 50"),
    field("errorsTruncated", boolean()),
    field("warnings", array(any())).describe("{message, line, task}, at most 50"),
    field("warningsTruncated", boolean()),
    field("leak", any())
      .describe(
        "{detected: false} or {detected: true, entity, position, line, text, pointFile, "
        "pointFileExists, hint}"),
    field("output", any())
      .describe("{compiledFile, compiledFileExists, copiedTo}: absolute paths"),
    field("log", any()).describe("{uri, lineCount, text?, truncated}"),
  };
  if (required)
  {
    fields[0] = fields[0].required();
  }
  // compile_run reports the tasks of the status, or the task definitions in a dry run
  for (auto& extraField : extraFields)
  {
    std::erase_if(fields, [&](const auto& f) { return f.name == extraField.name; });
    fields.push_back(std::move(extraField));
  }
  return object(std::move(fields));
}

ToolResult compileStatus(CallContext& context, const Args& args)
{
  auto& runs = *context.server().compileRuns;
  const CompileRun* run = nullptr;
  if (const auto runId = args.getOptional<std::string>("run"))
  {
    run = runs.find(*runId);
    if (!run)
    {
      return makeError(
        ErrorCode::ObjectNotFound,
        fmt::format("There is no compile run {}.", *runId),
        "Omit 'run' to get the latest run; only the 20 most recent ended runs are kept.");
    }
  }
  else
  {
    run = context.hasDocument() ? runs.latest(&context.document()) : nullptr;
    run = run ? run : runs.latest(nullptr);
    if (!run)
    {
      return makeError(
        ErrorCode::ObjectNotFound,
        "No compile run has been started.",
        "Start one with compile_run, e.g. {\"preset\": \"normal\"}.");
    }
  }

  return runStatus(
    *run, args.get<std::string>("log"), size_t(args.get<int>("tailLines")));
}

// compile_run

/** The names of the game's compile tools that the tool spec refers to as ${name}. */
std::vector<std::string> referencedTools(
  const mdl::GameConfig& gameConfig, const std::string& spec)
{
  auto result = std::vector<std::string>{};
  auto pos = spec.find("${");
  while (pos != std::string::npos)
  {
    const auto end = spec.find('}', pos + 2);
    if (end == std::string::npos)
    {
      break;
    }
    const auto name = kdl::str_trim(spec.substr(pos + 2, end - pos - 2));
    if (findTool(gameConfig, name) && std::ranges::find(result, name) == result.end())
    {
      result.push_back(name);
    }
    pos = spec.find("${", end);
  }
  return result;
}

ToolResult compileRun(CallContext& context, const Args& args)
{
  auto* compileHost = context.host().compileHost();
  if (!compileHost)
  {
    return makeError(
      ErrorCode::UnsupportedInHost,
      "This host cannot compile maps.",
      "Compile in the TrenchBroom editor.");
  }

  const auto& document = context.documentInfo();
  auto& map = context.map();
  auto& runs = *context.server().compileRuns;

  if (args.has("profile") == args.has("preset"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass exactly one of 'profile' and 'preset'.",
      "Run a preset with {\"preset\": \"normal\"} (see compile_presets_list), or a saved "
      "profile with {\"profile\": \"<name>\"} (see compile_profiles_list).");
  }

  if (!map.path().is_absolute())
  {
    return makeError(
      ErrorCode::UnsavedChanges,
      fmt::format(
        "{} has never been saved; the compilation needs the map's folder.", document.id),
      "Save it first with document_save_as {\"path\": \"/absolute/path/name.map\"}.");
  }

  if (const auto* running = runs.running(context.document()))
  {
    return makeError(
      ErrorCode::CompileRunning,
      fmt::format(
        "Compile {} of {} is still running; one compile at a time per document.",
        running->id,
        document.id),
      fmt::format(
        "Poll it with compile_status {{\"run\": \"{0}\"}} or stop it with compile_cancel "
        "{{\"run\": \"{0}\"}}.",
        running->id));
  }
  if (context.host().isCompileRunning(context.document()))
  {
    return makeError(
      ErrorCode::CompileRunning,
      fmt::format(
        "The editor's compilation dialog is compiling {}; one compile at a time per "
        "document.",
        document.id),
      "Wait until the compilation in the editor has finished (editor_status reports "
      "compileRunning).");
  }

  const auto& documentGame = map.gameInfo();
  const auto* managedGame =
    context.host().gameManager().gameInfo(documentGame.gameConfig.name);
  const auto& gameInfo = managedGame ? *managedGame : documentGame;
  const auto& gameConfig = gameInfo.gameConfig;

  auto profile = mdl::CompilationProfile{};
  auto presetName = std::optional<std::string>{};
  if (const auto name = args.getOptional<std::string>("preset"))
  {
    const auto preset = findCompilePreset(gameConfig, *name);
    if (!preset)
    {
      return unknownPresetError(gameConfig, *name);
    }
    profile = preset->profile;
    presetName = preset->name;
  }
  else
  {
    const auto profileName = args.get<std::string>("profile");
    const auto* found = findProfile(gameInfo, profileName);
    if (!found)
    {
      return unknownProfileError(gameInfo, profileName);
    }
    profile = *found;
  }

  const auto tasks = enabledTasks(profile);
  if (tasks.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("The compile profile '{}' has no enabled task.", profile.name),
      "Enable tasks with compile_profile_save (overwrite: true).");
  }

  const auto test = args.get<bool>("test");
  if (!test)
  {
    auto problems = std::vector<std::string>{};
    for (const auto& task : tasks)
    {
      if (const auto* runTool = std::get_if<mdl::CompilationRunTool>(&task))
      {
        for (const auto& name : referencedTools(gameConfig, runTool->toolSpec))
        {
          const auto status = compileToolStatus(*findTool(gameConfig, name));
          if (status.status != "ok" && std::ranges::find_if(problems, [&](const auto& p) {
                                         return p.starts_with(name + ": ");
                                       }) == problems.end())
          {
            problems.push_back(
              status.path.empty()
                ? fmt::format("{}: {}", name, status.status)
                : fmt::format("{}: {} ({})", name, status.status, status.path));
          }
        }
      }
    }

    if (!problems.empty())
    {
      auto error = makeError(
        ErrorCode::OperationFailed,
        fmt::format(
          "The compile tools of {} are not set up: {}.",
          gameConfig.name,
          kdl::str_join(problems, "; ")),
        "Set their paths with compile_tools_set, e.g. {\"tools\": {\"qbsp\": "
        "\"/opt/ericw-tools/bin/qbsp\"}}; compile_tools_get shows the status. Or pass "
        "test: true to see the commands without running them.");
      error.details["tools"] = toolsPayload(gameInfo)["tools"];
      return error;
    }
  }

  if (!isGamePathValid(gameInfo))
  {
    context.warn(
      "GAME_PATH_NOT_SET",
      fmt::format(
        "The game folder of {} is not set, so copying the compiled map into the game's "
        "maps folder will fail.",
        gameConfig.name),
      {});
  }

  if (context.dryRun())
  {
    auto tasksJson = Json::array();
    for (const auto& task : tasks)
    {
      tasksJson.push_back(toJson(task));
    }
    return Json{
      {"wouldDo",
       fmt::format(
         "{} compile profile '{}' ({} tasks) for {}",
         test ? "test" : "run",
         profile.name,
         tasks.size(),
         document.id)},
      {"tasks", std::move(tasksJson)},
    };
  }

  auto started = runs.start(
    *compileHost,
    context.document(),
    CompileRunSpec{
      gameConfig.name,
      document.id,
      context.session().id,
      std::move(profile),
      std::move(presetName),
      test,
    });
  if (started.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format("Could not start the compilation: {}", errorMessage(started)),
      "Check the profile's work directory (compile_profiles_list).");
  }

  // the editor status reports running compilations
  context.server().scheduleResourceUpdate("trenchbroom://editor/status");
  return runStatus(*started.value());
}

// compile_cancel

ToolResult compileCancel(CallContext& context, const Args& args)
{
  auto& runs = *context.server().compileRuns;
  CompileRun* run = nullptr;
  if (const auto runId = args.getOptional<std::string>("run"))
  {
    run = runs.find(*runId);
    if (!run)
    {
      return makeError(
        ErrorCode::ObjectNotFound,
        fmt::format("There is no compile run {}.", *runId),
        "Use compile_status to see the latest run.");
    }
  }
  else if (context.hasDocument())
  {
    run = runs.running(context.document());
    run = run ? run : runs.latest(&context.document());
  }
  if (!run)
  {
    return makeError(
      ErrorCode::ObjectNotFound,
      "No compile run of the document has been started.",
      "Pass 'run', e.g. {\"run\": \"run:1\"}.");
  }

  if (!run->running())
  {
    context.warn("NOT_RUNNING", fmt::format("Compile {} has already ended.", run->id));
    return runStatus(*run);
  }

  if (context.dryRun())
  {
    auto result = runStatus(*run);
    result["wouldDo"] = fmt::format("cancel compile {}", run->id);
    return result;
  }

  runs.cancel(*run);
  return runStatus(*run);
}

// pointfile_load, pointfile_unload, portalfile_load, portalfile_unload

std::vector<std::filesystem::path> mapFileCandidates(
  const mdl::Map& map, const std::vector<std::string>& extensions)
{
  const auto& path = map.path();
  const auto directory = path.parent_path();
  const auto base = path.stem().string();

  auto result = std::vector<std::filesystem::path>{};
  for (const auto& extension : extensions)
  {
    result.push_back(directory / "compile" / (base + extension));
    result.push_back(directory / (base + extension));
  }
  return result;
}

Result<std::filesystem::path, ToolError> compileFileArgument(
  CallContext& context,
  const Args& args,
  const std::vector<std::filesystem::path>& candidates,
  const std::string& kind)
{
  if (args.has("path"))
  {
    return absolutePathArgument(args, "path");
  }

  if (!context.map().path().is_absolute())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} has never been saved, so there is no default {}.",
        context.documentInfo().id,
        kind),
      "Pass 'path' with the absolute path of the file.");
  }

  for (const auto& candidate : candidates)
  {
    if (pathExists(candidate))
    {
      return candidate;
    }
  }

  auto names = std::vector<std::string>{};
  for (const auto& candidate : candidates)
  {
    names.push_back(candidate.string());
  }
  return makeError(
    ErrorCode::IoError,
    fmt::format("No {} found; looked for: {}.", kind, kdl::str_join(names, ", ")),
    "Compile the map first (compile_run), or pass 'path'.");
}

struct NearEntity
{
  const mdl::EntityNode* node;
  double distance;
};

void collectPointEntities(
  const mdl::Node& node,
  std::vector<const mdl::EntityNode*>& entities,
  vm::bbox3d& brushBounds,
  bool& hasBrushes)
{
  if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
      entityNode && !entityNode->hasChildren())
  {
    entities.push_back(entityNode);
  }
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    brushBounds = hasBrushes ? vm::merge(brushBounds, brushNode->logicalBounds())
                             : brushNode->logicalBounds();
    hasBrushes = true;
  }
  for (const auto* child : node.children())
  {
    collectPointEntities(*child, entities, brushBounds, hasBrushes);
  }
}

Json nearestEntitiesJson(
  CallContext& context,
  const std::vector<const mdl::EntityNode*>& entities,
  const vm::vec3d& point)
{
  auto nearest = std::vector<NearEntity>{};
  for (const auto* entityNode : entities)
  {
    nearest.push_back(
      NearEntity{entityNode, vm::distance(entityNode->entity().origin(), point)});
  }
  std::ranges::sort(nearest, [](const auto& lhs, const auto& rhs) {
    return lhs.distance < rhs.distance;
  });

  auto result = Json::array();
  for (size_t i = 0; i < nearest.size() && i < NearestEntityCount; ++i)
  {
    const auto& entity = nearest[i].node->entity();
    result.push_back(Json{
      {"id", context.ids().format(*nearest[i].node)},
      {"classname", entity.classname()},
      {"origin", toJson(entity.origin())},
      {"distance", nearest[i].distance},
    });
  }
  return result;
}

ToolResult pointFileLoad(CallContext& context, const Args& args)
{
  auto& runs = *context.server().compileRuns;
  auto candidates = std::vector<std::filesystem::path>{};
  if (const auto* run = runs.latest(&context.document()))
  {
    const auto log = run->log();
    const auto analysis = analyzeCompileLog(log, run->tasks, !run->running());
    if (analysis.leak)
    {
      if (const auto pointFile = leakPointFile(*run, analysis))
      {
        candidates.push_back(*pointFile);
      }
    }
  }
  for (const auto& candidate : mapFileCandidates(context.map(), {".pts", ".lin"}))
  {
    if (std::ranges::find(candidates, candidate) == candidates.end())
    {
      candidates.push_back(candidate);
    }
  }

  auto path = compileFileArgument(context, args, candidates, "point file");
  if (path.is_error())
  {
    return errorOf(path);
  }

  if (context.dryRun())
  {
    return Json{
      {"wouldDo", fmt::format("load the point file {}", path.value())},
      {"path", path.value().string()},
    };
  }

  auto& document = context.document();
  document.loadPointFile(path.value());
  const auto* trace = document.pointTrace();
  if (!document.isPointFileLoaded() || !trace)
  {
    auto messages = std::vector<std::string>{};
    for (const auto& message : context.loggedProblems())
    {
      messages.push_back(message.text);
    }
    return makeError(
      ErrorCode::IoError,
      fmt::format("Could not load the point file {}: {}", path.value(), joined(messages)),
      "A point file contains one point per line ('x y z'), at least two.");
  }

  auto entities = std::vector<const mdl::EntityNode*>{};
  auto brushBounds = vm::bbox3d{};
  auto hasBrushes = false;
  collectPointEntities(context.map().worldNode(), entities, brushBounds, hasBrushes);

  const auto& points = trace->points();
  auto pointsJson = Json::array();
  auto length = 0.0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    if (i < MaxReportedPoints)
    {
      pointsJson.push_back(toJson(vm::vec3d{points[i]}));
    }
    if (i > 0)
    {
      length += double(vm::distance(points[i - 1], points[i]));
    }
  }

  const auto endJson = [&](const vm::vec3f& point) {
    return Json{
      {"point", toJson(vm::vec3d{point})},
      {"nearestEntities", nearestEntitiesJson(context, entities, vm::vec3d{point})},
    };
  };

  // walk from the end inside the map's brushes to where the path leaves them
  auto leavesMapAt = Json(nullptr);
  if (hasBrushes && !points.empty())
  {
    const auto inside = [&](const vm::vec3f& point) {
      return brushBounds.contains(vm::vec3d{point});
    };
    const auto walk = [&](auto begin, auto end) {
      const auto it = std::find_if(begin, end, [&](const auto& p) { return !inside(p); });
      if (it != end)
      {
        leavesMapAt = toJson(vm::vec3d{*it});
      }
    };
    if (inside(points.front()))
    {
      walk(points.begin(), points.end());
    }
    else if (inside(points.back()))
    {
      walk(points.rbegin(), points.rend());
    }
  }

  return Json{
    {"path", path.value().string()},
    {"pointCount", points.size()},
    {"points", std::move(pointsJson)},
    {"truncated", points.size() > MaxReportedPoints},
    {"length", length},
    {"start", endJson(points.front())},
    {"end", endJson(points.back())},
    {"leavesMapAt", leavesMapAt},
    {"hint",
     "The path runs from an entity inside the map to the void. The gap in the map's "
     "hull is usually near leavesMapAt (or near 'end'); close it with a brush, then "
     "compile again and unload the point file with pointfile_unload."},
  };
}

ToolResult pointFileUnload(CallContext& context, const Args&)
{
  auto& document = context.document();
  const auto loaded = document.isPointFileLoaded();
  if (context.dryRun())
  {
    return Json{
      {"wouldDo", loaded ? "unload the point file" : "nothing, no point file is loaded"},
      {"unloaded", loaded},
    };
  }
  if (loaded)
  {
    document.unloadPointFile();
  }
  else
  {
    context.warn("NOT_LOADED", "No point file is loaded.");
  }
  return Json{{"unloaded", loaded}};
}

ToolResult portalFileLoad(CallContext& context, const Args& args)
{
  auto path = compileFileArgument(
    context, args, mapFileCandidates(context.map(), {".prt"}), "portal file");
  if (path.is_error())
  {
    return errorOf(path);
  }

  if (!mdl::canLoadPortalFile(path.value()))
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("Cannot read the portal file {}.", path.value()),
      "Check the path; vis writes <map>.prt next to the compiled map.");
  }

  if (context.dryRun())
  {
    return Json{
      {"wouldDo", fmt::format("load the portal file {}", path.value())},
      {"path", path.value().string()},
    };
  }

  auto& document = context.document();
  document.loadPortalFile(path.value());
  const auto* portals = document.portals();
  if (!document.isPortalFileLoaded() || !portals)
  {
    auto messages = std::vector<std::string>{};
    for (const auto& message : context.loggedProblems())
    {
      messages.push_back(message.text);
    }
    return makeError(
      ErrorCode::IoError,
      fmt::format(
        "Could not load the portal file {}: {}", path.value(), joined(messages)),
      "Portal files start with PRT1, PRT2 or PRT1-AM.");
  }

  return Json{
    {"path", path.value().string()},
    {"portalCount", portals->size()},
  };
}

ToolResult portalFileUnload(CallContext& context, const Args&)
{
  auto& document = context.document();
  const auto loaded = document.isPortalFileLoaded();
  if (context.dryRun())
  {
    return Json{
      {"wouldDo",
       loaded ? "unload the portal file" : "nothing, no portal file is loaded"},
      {"unloaded", loaded},
    };
  }
  if (loaded)
  {
    document.unloadPortalFile();
  }
  else
  {
    context.warn("NOT_LOADED", "No portal file is loaded.");
  }
  return Json{{"unloaded", loaded}};
}

Field gameField()
{
  return field("game", string())
    .describe("Game name (game_list); default: the target document's game");
}

const auto Workflow = std::string{
  "Workflow: compile_tools_get / compile_tools_set (tool paths) -> compile_presets_list "
  "-> compile_run {\"preset\": \"normal\"} (returns a run handle at once) -> poll "
  "compile_status until state is not 'running' -> on a leak, pointfile_load, close the "
  "gap, compile again."};

} // namespace

void registerCompileTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"compile_tools_get"}
      .title("Get Compile Tools")
      .description(
        "Returns the compile tool paths of a game (preferences) and checks each: 'ok', "
        "'notSet', 'notFound', 'notAFile' or 'notExecutable'. Half-Life uses csg, bsp, "
        "vis and rad (VHLT or ZHLT: hlcsg, hlbsp, hlvis, hlrad); Quake uses qbsp, vis "
        "and light, Quake 2 bsp, vis and light (ericw-tools); Quake 3 uses q3map2. "
        "Profiles refer to them as ${name}, e.g. ${qbsp}. "
        + Workflow + " Example: {\"game\": \"Half-Life\"}")
      .input(object({gameField()}))
      .output(object({
        field("game", string()).required(),
        field("family", any()).describe("halflife, quake, quake2, quake3 or null"),
        field("gamePath", string()),
        field("tools", array(any()))
          .describe("{name, description, variable, path, status} per tool"),
        field("allConfigured", boolean()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(compileToolsGet));

  registry.add(
    ToolDef{"compile_tools_set"}
      .title("Set Compile Tools")
      .description(
        "Sets the paths of a game's compile tools (preferences, not undoable). Paths "
        "must be absolute; '' clears a path. A path that does not exist or is not "
        "executable is set anyway and reported as a warning. Example: {\"game\": "
        "\"Half-Life\", \"tools\": {\"csg\": \"/opt/vhlt/hlcsg\", \"bsp\": "
        "\"/opt/vhlt/hlbsp\", \"vis\": \"/opt/vhlt/hlvis\", \"rad\": "
        "\"/opt/vhlt/hlrad\"}}")
      .input(object({
        gameField(),
        field("tools", object({}).allowAdditionalProperties())
          .required()
          .describe("Tool name -> absolute path of the executable, or '' to clear"),
      }))
      .output(object({
        field("game", string()).required(),
        field("family", any()),
        field("gamePath", string()),
        field("tools", array(any())),
        field("allConfigured", boolean()),
        field("changed", array(string())).describe("Tools whose path changed"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(compileToolsSet));

  registry.add(
    ToolDef{"compile_presets_list"}
      .title("List Compile Presets")
      .description(
        "Lists the built-in compile presets of a game's tool chain: 'fast' (no vis, "
        "fast light), 'normal' and 'full' quality. Each exports the map to "
        "<map folder>/compile/, runs the tools and copies the .bsp into the game's "
        "(or last mod's) maps folder. Run one with compile_run {\"preset\": "
        "\"normal\"} or save it as an editable profile with compile_profile_save. "
        "missingTools lists tools whose path is not set up (compile_tools_set). "
        + compilePresetsSummary() + " Example: {}")
      .input(object({gameField()}))
      .output(object({
        field("game", string()).required(),
        field("family", any()),
        field("presets", array(any()))
          .describe("{name, description, profile, tools, missingTools}"),
        field("summary", string()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(compilePresetsList));

  registry.add(
    ToolDef{"compile_profiles_list"}
      .title("List Compile Profiles")
      .description(
        "Lists the saved compile profiles of a game (the editor's Compile dialog) with "
        "their work directory and tasks: exportMap, runTool, copyFiles, renameFile, "
        "deleteFiles, launchEngine. Specs use variables such as ${MAP_DIR_PATH}, "
        "${WORK_DIR_PATH}, ${MAP_BASE_NAME}, ${GAME_DIR_PATH}, ${MODS[-1]} and the "
        "compile tools, e.g. ${qbsp}. Example: {}")
      .input(object({gameField()}))
      .output(object({
        field("game", string()).required(),
        field("profiles", array(any()))
          .describe("{name, workDir, tasks, enabledTaskCount} per profile"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(compileProfilesList));

  registry.add(
    ToolDef{"compile_profile_save"}
      .title("Save Compile Profile")
      .description(
        "Saves a compile profile of a game (written to the game's compilation config, "
        "not undoable), either a custom 'profile' or a built-in 'preset' (optionally "
        "renamed with 'name'). A profile with the same name is only replaced with "
        "overwrite: true. Examples: {\"preset\": \"full\", \"name\": \"Release\"}; "
        "{\"profile\": {\"name\": \"BSP only\", \"tasks\": [{\"type\": \"exportMap\", "
        "\"target\": \"${WORK_DIR_PATH}/${MAP_BASE_NAME}.map\"}, {\"type\": "
        "\"runTool\", \"tool\": \"${qbsp}\", \"parameters\": \"${MAP_BASE_NAME}\"}]}}")
      .input(object({
        gameField(),
        field("profile", compilationProfileSchema())
          .describe("The profile; exclusive with 'preset'"),
        field("preset", string()).describe("Name of a preset (compile_presets_list)"),
        field("name", string())
          .describe("Name of the saved profile; default: the profile's or preset's name"),
        field("overwrite", boolean().defaultsTo(false))
          .describe("Replace a profile with the same name"),
      }))
      .output(object({
        field("game", string()).required(),
        field("profile", any()).required(),
        field("created", boolean()).describe("false if an existing profile was replaced"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .handler(compileProfileSave));

  registry.add(
    ToolDef{"compile_profile_delete"}
      .title("Delete Compile Profile")
      .description(
        "Deletes a saved compile profile of a game (not undoable). Example: {\"name\": "
        "\"Release\"}")
      .input(object({
        gameField(),
        field("name", string()).required().describe("Profile name"),
      }))
      .output(object({
        field("game", string()).required(),
        field("deleted", string()),
        field("remaining", array(string())),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .destructive()
      .handler(compileProfileDelete));

  registry.add(
    ToolDef{"compile_run"}
      .title("Compile Map")
      .description(
        "Compiles the document with a preset or a saved profile in the background and "
        "returns at once with the run's status (run handle 'run:<n>'); poll "
        "compile_status. Unsaved changes are compiled too (the export task writes the "
        "current state), but the map must have been saved once. One compile at a time "
        "per document. test: true only logs the commands that would run. Fails if a "
        "tool the profile uses is not set up. "
        + Workflow + " " + compilePresetsSummary() + " Example: {\"preset\": \"normal\"}")
      .input(object({
        field("profile", string()).describe("Name of a saved profile"),
        field("preset", string()).describe("Name of a preset: fast, normal or full"),
        field("test", boolean().defaultsTo(false))
          .describe("Only log what the tasks would do"),
      }))
      .output(runStatusSchema(
        {
          field("wouldDo", wouldDoField()),
          field("tasks", array(any()))
            .describe("Per enabled task; dry run: the task definitions"),
        },
        false))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(compileRun));

  registry.add(
    ToolDef{"compile_status"}
      .title("Compile Status")
      .description(
        "Reports a compile run: state (running, succeeded, failed, cancelled), "
        "progress, current task, exit codes, parsed errors and warnings, leak (with "
        "the point file to load), the compiled file and where it was copied, and the "
        "log tail or full log. Defaults to the latest run of the target document. The "
        "full log is also the resource trenchbroom://compile/{run}/log. Example: "
        "{\"run\": \"run:1\", \"log\": \"tail\", \"tailLines\": 20}")
      .input(object({
        field("run", string()).describe("Run handle, e.g. run:1; default: latest run"),
        field("log", enumOf({"tail", "full", "none"}).defaultsTo("tail"))
          .describe("How much of the log to return"),
        field("tailLines", integer().min(1).max(5000).defaultsTo(50))
          .describe("Lines of the tail"),
      }))
      .output(runStatusSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .handler(compileStatus));

  registry.add(
    ToolDef{"compile_cancel"}
      .title("Cancel Compile")
      .description(
        "Stops a running compile run; the remaining tasks are skipped. Defaults to the "
        "running run of the target document. Returns the run's status. Example: "
        "{\"run\": \"run:2\"}")
      .input(object({
        field("run", string()).describe("Run handle; default: the document's run"),
      }))
      .output(runStatusSchema({field("wouldDo", wouldDoField())}))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .handler(compileCancel));

  registry.add(
    ToolDef{"pointfile_load"}
      .title("Load Point File")
      .description(
        "Loads a leak point file (.pts or .lin) into the document and shows the leak "
        "path in the editor. Default: the point file of the latest run's leak, else "
        "<map folder>/compile/<map>.pts, <map folder>/<map>.pts and the .lin variants. "
        "Returns the path's points, its length, the point entities nearest to both "
        "ends (the leak starts at an entity), and leavesMapAt: where the path leaves "
        "the bounds of the map's brushes, usually near the gap. Example: {} or "
        "{\"path\": \"/maps/compile/start.pts\"}")
      .input(object({
        field("path", string()).describe("Absolute path of the point file"),
      }))
      .output(object({
        field("path", string()).required(),
        field("pointCount", integer()),
        field("points", array(vec3())).describe("At most 1000"),
        field("truncated", boolean()),
        field("length", number()).describe("Length of the path in map units"),
        field("start", any()).describe("{point, nearestEntities}"),
        field("end", any()).describe("{point, nearestEntities}"),
        field("leavesMapAt", any()).describe("Point or null"),
        field("hint", string()),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(pointFileLoad));

  registry.add(ToolDef{"pointfile_unload"}
                 .title("Unload Point File")
                 .description("Hides the loaded leak path of the document. Example: {}")
                 .input(object({}))
                 .output(object({
                   field("unloaded", boolean()).required(),
                   field("wouldDo", wouldDoField()),
                 }))
                 .mutation(Mutation::External)
                 .documentUse(DocumentUse::Required)
                 .idempotent()
                 .handler(pointFileUnload));

  registry.add(
    ToolDef{"portalfile_load"}
      .title("Load Portal File")
      .description(
        "Loads a portal file (.prt, written by bsp/vis) into the document and shows "
        "the portals between leaves in the editor. Default: <map "
        "folder>/compile/<map>.prt, then <map folder>/<map>.prt. Example: {}")
      .input(object({
        field("path", string()).describe("Absolute path of the portal file"),
      }))
      .output(object({
        field("path", string()).required(),
        field("portalCount", integer()),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(portalFileLoad));

  registry.add(ToolDef{"portalfile_unload"}
                 .title("Unload Portal File")
                 .description("Hides the loaded portals of the document. Example: {}")
                 .input(object({}))
                 .output(object({
                   field("unloaded", boolean()).required(),
                   field("wouldDo", wouldDoField()),
                 }))
                 .mutation(Mutation::External)
                 .documentUse(DocumentUse::Required)
                 .idempotent()
                 .handler(portalFileUnload));
}

void registerCompileResources(ResourceRegistry& registry)
{
  registry.addTemplate(ResourceTemplateDef{
    "trenchbroom://compile/{run}/log",
    "compile-log",
    "Compile Log",
    "The full output of a compile run as plain text ({run} is a run handle such as "
    "run:3, from compile_run). Subscribe to get notified while output arrives and when "
    "the run ends.",
    "text/plain",
    [](ServerState& state, Session&, const std::string& uri, const auto& vars)
      -> Result<Json, ToolError> {
      const auto& runId = vars.at("run");
      if (const auto* run = state.compileRuns->find(runId))
      {
        return Json::array({Json{
          {"uri", uri},
          {"mimeType", "text/plain"},
          {"text", run->log()},
        }});
      }
      return makeError(
        ErrorCode::ObjectNotFound,
        fmt::format("There is no compile run {}.", runId),
        "Use compile_status to see the latest run.");
    },
    [](ServerState& state, Session&) {
      auto entries = std::vector<Json>{};
      for (const auto* run : state.compileRuns->runs())
      {
        entries.push_back(Json{
          {"uri", CompileRuns::logUri(run->id)},
          {"name", "compile-log-" + run->id},
          {"title", fmt::format("Compile Log: {} ({})", run->id, run->profile.name)},
          {"mimeType", "text/plain"},
        });
      }
      return entries;
    },
  });
}

} // namespace tb::mcp
