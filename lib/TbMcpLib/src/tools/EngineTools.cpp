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

#include "mcp/tools/EngineTools.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "base/Uuid.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Host.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/CompileUtils.h"
#include "mdl/GameConfig.h"
#include "mdl/GameEngineConfig.h"
#include "mdl/GameEngineProfile.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>

namespace tb::mcp
{
namespace
{
using namespace schema;

std::string joined(const std::vector<std::string>& strings)
{
  return strings.empty() ? "(none)" : kdl::str_join(strings, ", ");
}

/**
 * The game named by the 'game' argument, else the game of the target document. The game
 * manager's copy is returned when possible because it holds the current engine profiles.
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

std::vector<std::string> profileNames(const mdl::GameInfo& gameInfo)
{
  auto result = std::vector<std::string>{};
  for (const auto& profile : gameInfo.gameEngineConfig.profiles)
  {
    result.push_back(profile.name);
  }
  return result;
}

/**
 * The status of an engine path as checkCompileToolPath reports it, except that an
 * application bundle (a folder ending in .app, macOS) counts as executable.
 */
std::string enginePathStatus(const std::filesystem::path& path)
{
  const auto status = checkCompileToolPath(path);
  return status == "notAFile" && path.extension() == ".app" ? "ok" : status;
}

Json toJson(const mdl::GameEngineProfile& profile)
{
  const auto status = enginePathStatus(profile.path);
  return Json{
    {"id", profile.id},
    {"name", profile.name},
    {"path", profile.path.string()},
    {"parameters", profile.parameterSpec},
    {"status", status},
    {"exists", status != "notSet" && status != "notFound"},
    {"executable", status == "ok"},
  };
}

void warnAboutPath(CallContext& context, const mdl::GameEngineProfile& profile)
{
  const auto status = enginePathStatus(profile.path);
  const auto warning = status == "notFound"   ? std::pair{"TOOL_NOT_FOUND", "missing"}
                       : status == "notAFile" ? std::pair{"TOOL_NOT_A_FILE", "not a file"}
                       : status == "notExecutable"
                         ? std::pair{"TOOL_NOT_EXECUTABLE", "not executable"}
                         : std::pair{"", ""};
  if (*warning.first)
  {
    context.warn(
      warning.first,
      fmt::format(
        "The engine of profile '{}' is {}: {}",
        profile.name,
        warning.second,
        profile.path));
  }
}

// engine_profiles_list

ToolResult engineProfilesList(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameInfo = *found.value();

  if (gameInfo.gameEngineConfigParseFailed)
  {
    context.warn(
      "ENGINE_CONFIG_INVALID",
      fmt::format(
        "The engine profiles of {} could not be read; saving a profile replaces them.",
        gameInfo.gameConfig.name));
  }

  auto profiles = Json::array();
  for (const auto& profile : gameInfo.gameEngineConfig.profiles)
  {
    profiles.push_back(toJson(profile));
  }

  return Json{
    {"game", gameInfo.gameConfig.name},
    {"profiles", std::move(profiles)},
  };
}

// engine_profile_save

ToolResult engineProfileSave(CallContext& context, const Args& args)
{
  auto found = targetGame(context, args);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& gameInfo = *found.value();
  const auto& gameName = gameInfo.gameConfig.name;

  const auto name = kdl::str_trim(args.get<std::string>("name"));
  if (name.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The profile name must not be empty.",
      "Pass a name, e.g. {\"name\": \"Quakespasm\"}.");
  }

  auto path = absolutePathArgument(args, "path");
  if (path.is_error())
  {
    return errorOf(path);
  }

  auto config = gameInfo.gameEngineConfig;
  auto it = std::ranges::find_if(
    config.profiles, [&](const auto& profile) { return profile.name == name; });
  const auto created = it == config.profiles.end();
  if (!created && !args.get<bool>("overwrite"))
  {
    return makeError(
      ErrorCode::FileExists,
      fmt::format("{} already has an engine profile named '{}'.", gameName, name),
      "Pass overwrite: true to replace it, or choose another name.");
  }

  auto profile = mdl::GameEngineProfile{
    .id = created ? generateUuid() : it->id,
    .name = name,
    .path = std::move(path.value()),
    .parameterSpec = args.getOptional<std::string>("parameters")
                       .value_or(created ? std::string{} : it->parameterSpec),
  };
  warnAboutPath(context, profile);

  auto result = Json{
    {"game", gameName},
    {"profile", toJson(profile)},
    {"created", created},
  };

  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format(
      "{} the engine profile '{}' of {} ({} {})",
      created ? "create" : "replace",
      profile.name,
      gameName,
      profile.path,
      profile.parameterSpec);
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

  auto nullLogger = NullLogger{};
  auto& logger = context.hasDocument() ? context.document().logger()
                                       : static_cast<Logger&>(nullLogger);
  const auto written = context.host().gameManager().updateGameEngineConfig(
    gameName, std::move(config), logger);
  if (written.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format(
        "Could not save the engine profiles of {}: {}", gameName, errorMessage(written)),
      "Check that the user configuration folder of TrenchBroom is writable.");
  }
  return result;
}

// engine_launch

Result<const mdl::GameEngineProfile*, ToolError> findLaunchProfile(
  const mdl::GameInfo& gameInfo, const std::optional<std::string>& name)
{
  const auto& profiles = gameInfo.gameEngineConfig.profiles;
  const auto& gameName = gameInfo.gameConfig.name;
  if (profiles.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("{} has no engine profiles.", gameName),
      "Add one with engine_profile_save {\"name\": \"Quakespasm\", \"path\": "
      "\"/opt/quakespasm/quakespasm\", \"parameters\": \"+map ${MAP_BASE_NAME}\"}.");
  }

  if (!name)
  {
    if (profiles.size() == 1)
    {
      return &profiles.front();
    }
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "{} has {} engine profiles; pass 'profile': {}.",
        gameName,
        profiles.size(),
        joined(profileNames(gameInfo))),
      fmt::format("E.g. {{\"profile\": \"{}\"}}.", profiles.front().name));
  }

  auto it =
    std::ranges::find_if(profiles, [&](const auto& p) { return p.name == *name; });
  if (it == profiles.end())
  {
    it = std::ranges::find_if(profiles, [&](const auto& p) { return p.id == *name; });
  }
  if (it == profiles.end())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "Unknown engine profile '{}' for {}; saved profiles: {}.",
        *name,
        gameName,
        joined(profileNames(gameInfo))),
      "Use engine_profiles_list to see the profiles.");
  }
  return &*it;
}

ToolResult engineLaunch(CallContext& context, const Args& args)
{
  auto* engineHost = context.host().engineHost();
  if (!engineHost)
  {
    return makeError(
      ErrorCode::UnsupportedInHost,
      "This host cannot launch game engines.",
      "Launch the engine from the TrenchBroom editor.");
  }

  const auto& document = context.documentInfo();
  auto& map = context.map();
  if (!map.path().is_absolute())
  {
    return makeError(
      ErrorCode::UnsavedChanges,
      fmt::format(
        "{} has never been saved; the engine loads the compiled map by the map's name.",
        document.id),
      "Save it with document_save_as {\"path\": \"/absolute/path/name.map\"} and compile "
      "it with compile_run.");
  }

  const auto& documentGame = map.gameInfo();
  const auto* managedGame =
    context.host().gameManager().gameInfo(documentGame.gameConfig.name);
  const auto& gameInfo = managedGame ? *managedGame : documentGame;

  auto found = findLaunchProfile(gameInfo, args.getOptional<std::string>("profile"));
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& profile = *found.value();

  const auto status = enginePathStatus(profile.path);
  if (status != "ok")
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format(
        "The engine of profile '{}' cannot be started ({}): '{}'.",
        profile.name,
        status,
        profile.path),
      fmt::format(
        "Fix its path with engine_profile_save {{\"name\": \"{}\", \"path\": "
        "\"/absolute/path/engine\", \"overwrite\": true}}.",
        profile.name));
  }

  const auto parameterSpec = args.getOptional<std::string>("parameters");
  const auto parameters = engineHost->engineParameters(
    context.document(), parameterSpec.value_or(profile.parameterSpec));
  if (parameters.is_error())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "Could not interpolate the parameters '{}': {}",
        parameterSpec.value_or(profile.parameterSpec),
        errorMessage(parameters)),
      "Use variables such as ${MAP_BASE_NAME}, ${GAME_DIR_PATH} and ${MODS[-1]}; "
      "write $$ for a literal $.");
  }

  if (map.modified())
  {
    context.warn(
      "UNSAVED_CHANGES",
      fmt::format(
        "{} has unsaved changes; the engine loads the last compiled map, which does not "
        "contain them. Run compile_run first to test the current state.",
        document.id));
  }

  auto result = Json{
    {"profile", profile.name},
    {"path", profile.path.string()},
    {"parameters", parameters.value()},
  };

  if (context.dryRun())
  {
    result["wouldDo"] =
      fmt::format("launch '{}' {} for {}", profile.path, parameters.value(), document.id);
    return result;
  }

  const auto processId =
    engineHost->launchEngine(context.document(), profile, parameterSpec);
  if (processId.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format(
        "Could not launch the engine of profile '{}': {}",
        profile.name,
        errorMessage(processId)),
      "Check the profile with engine_profiles_list.");
  }

  result["processId"] = processId.value();
  return result;
}

Field gameField()
{
  return field("game", string())
    .describe(
      "Game name as game_list lists it, e.g. 'Quake'; default: the target "
      "document's game");
}

Schema profileSchema()
{
  return object({
    field("id", string()).describe("Id, used by launchEngine compile tasks"),
    field("name", string()).required().describe("Profile name"),
    field("path", string()).describe("Absolute path of the engine executable"),
    field("parameters", string()).describe("Parameter spec with ${...} variables"),
    field("status", string()).describe("ok, notSet, notFound, notAFile or notExecutable"),
    field("exists", boolean()).describe("Whether the executable exists"),
    field("executable", boolean()).describe("Whether it can be executed"),
  });
}

} // namespace

void registerEngineTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"engine_profiles_list"}
      .title("List Engine Profiles")
      .description(
        "Lists the game engine profiles of a game (the editor's Launch Engine dialog): "
        "name, id, executable path with its status, and the parameter spec, e.g. "
        "'+map ${MAP_BASE_NAME}'. launchEngine compile tasks refer to a profile by its "
        "id. Read-only; add or change profiles with engine_profile_save and start one "
        "with engine_launch. Examples: {}; {\"game\": \"Quake\"}")
      .input(object({gameField()}))
      .output(object({
        field("game", string()).required(),
        field("profiles", array(profileSchema())).required(),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(engineProfilesList));

  registry.add(
    ToolDef{"engine_profile_save"}
      .title("Save Engine Profile")
      .description(
        "Saves a game engine profile of a game (the store of the editor's engine "
        "dialog, not undoable). The path must be absolute; a path that is missing or "
        "not executable is saved anyway and reported as a warning. The parameters may "
        "use the variables ${MAP_BASE_NAME}, ${GAME_DIR_PATH}, ${MODS} (e.g. "
        "${MODS[-1]}) and the compile tools. A profile with the same name is only "
        "replaced with overwrite: true; omitted parameters then keep their value. "
        "Returns the saved profile with its path status. Example: {\"name\": "
        "\"Half-Life\", \"path\": \"/opt/hl/hl_linux\", \"parameters\": \"-game valve "
        "-dev +map ${MAP_BASE_NAME}\"}")
      .input(object({
        gameField(),
        field("name", string()).required().describe("Profile name, e.g. 'Quakespasm'"),
        field("path", string())
          .required()
          .describe("Absolute path of the engine executable"),
        field("parameters", string())
          .describe("Command line parameters with ${...} variables, e.g. '+map "
                    "${MAP_BASE_NAME}'; default: '' (or the replaced profile's)"),
        field("overwrite", boolean().defaultsTo(false))
          .describe("Replace a profile with the same name; otherwise such a name fails "
                    "with FILE_EXISTS"),
      }))
      .output(object({
        field("game", string()).required(),
        field("profile", profileSchema()).required(),
        field("created", boolean()).describe("false if an existing profile was replaced"),
        field("wouldDo", string().describe("Dry run only: what the call would do")),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .handler(engineProfileSave));

  registry.add(
    ToolDef{"engine_launch"}
      .title("Launch Engine")
      .description(
        "Starts the game engine of an engine profile with the document's map, like "
        "the editor's Launch Engine dialog, and returns at once with the process id. "
        "The engine loads the compiled map (.bsp) by the map's name, so compile first "
        "(compile_run); unsaved changes are reported as a warning, and a map that was "
        "never saved is an error. 'parameters' overrides the profile's parameter spec "
        "for this launch; ${...} variables are interpolated. 'profile' may be omitted "
        "if the game has exactly one (engine_profiles_list). Not undoable. Returns the "
        "profile, the executable path, the interpolated parameters and processId. "
        "Examples: {}; {\"profile\": \"Quakespasm\", \"parameters\": \"-basedir "
        "/opt/quake +map ${MAP_BASE_NAME}\"}")
      .input(object({
        field("profile", string())
          .describe(
            "Name (or id) of an engine profile (engine_profiles_list); default: the "
            "game's only profile"),
        field("parameters", string())
          .describe("Parameters with ${...} variables for this launch only; default: the "
                    "profile's"),
      }))
      .output(object({
        field("profile", string()).required(),
        field("path", string()).required(),
        field("parameters", string()).describe("The interpolated parameters"),
        field("processId", integer()).describe("Process id of the started engine"),
        field("wouldDo", string().describe("Dry run only: what the call would do")),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(engineLaunch));
}

} // namespace tb::mcp
