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

#include "mcp/tools/CompileUtils.h"

#include "base/PreferenceManager.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"

#include "kd/overload.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <system_error>

namespace tb::mcp
{
namespace
{

const auto MapDirPath = std::string{"${MAP_DIR_PATH}"};

/** The exported map without extension, quoted because paths may contain spaces. */
const auto CompileBase = std::string{"${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}"};

std::string quoted(const std::string& str)
{
  return "\"" + str + "\"";
}

const auto QuotedBase = quoted(CompileBase);
const auto QuotedMap = quoted(CompileBase + ".map");
const auto QuotedBsp = quoted(CompileBase + ".bsp");

/** The q3map2 options that select the game and its file system. */
const auto Q3Map2GameOptions =
  std::string{"-game quake3 -fs_basepath \"${GAME_DIR_PATH}\" -fs_game ${MODS[-1]}"};

struct ToolStep
{
  std::string tool;
  std::string parameters;
};

std::string displayName(const CompileFamily family)
{
  switch (family)
  {
  case CompileFamily::HalfLife:
    return "Half-Life";
  case CompileFamily::Quake:
    return "Quake";
  case CompileFamily::Quake2:
    return "Quake 2";
  case CompileFamily::Quake3:
    return "Quake 3";
  }
  return "";
}

CompilePreset makePreset(
  const CompileFamily family,
  std::string name,
  std::string description,
  const std::vector<ToolStep>& steps,
  const std::vector<std::string>& copiedExtensions)
{
  auto tasks = std::vector<mdl::CompilationTask>{};
  tasks.emplace_back(mdl::CompilationExportMap{
    .enabled = true,
    .stripTbProperties = true,
    .stripEntityPattern = std::nullopt,
    .entityToAdd = std::nullopt,
    .targetSpec = CompileBase + ".map",
  });

  auto tools = std::vector<std::string>{};
  for (const auto& step : steps)
  {
    tasks.emplace_back(mdl::CompilationRunTool{
      .enabled = true,
      .toolSpec = "${" + step.tool + "}",
      .parameterSpec = step.parameters,
      .treatNonZeroResultCodeAsError = true,
    });
    if (std::ranges::find(tools, step.tool) == tools.end())
    {
      tools.push_back(step.tool);
    }
  }

  for (const auto& extension : copiedExtensions)
  {
    tasks.emplace_back(mdl::CompilationCopyFiles{
      .enabled = true,
      .sourceSpec = CompileBase + extension,
      .targetSpec = "${GAME_DIR_PATH}/${MODS[-1]}/maps",
    });
  }

  auto profileName = fmt::format("{} ({})", displayName(family), name);
  return CompilePreset{
    std::move(name),
    std::move(description),
    mdl::CompilationProfile{
      std::move(profileName),
      MapDirPath,
      std::move(tasks),
    },
    std::move(tools),
  };
}

std::vector<CompilePreset> halfLifePresets()
{
  const auto& p = QuotedBase;
  const auto copied = std::vector<std::string>{".bsp"};
  return {
    makePreset(
      CompileFamily::HalfLife,
      "fast",
      "Quick preview without vis: csg <map>, bsp <map>, rad -fast <map> (VHLT / ZHLT; "
      "<map> is the exported map without extension).",
      {{"csg", p}, {"bsp", p}, {"rad", "-fast " + p}},
      copied),
    makePreset(
      CompileFamily::HalfLife,
      "normal",
      "Standard compile: csg <map>, bsp <map>, vis <map>, rad <map> (VHLT / ZHLT; <map> "
      "is the exported map without extension).",
      {{"csg", p}, {"bsp", p}, {"vis", p}, {"rad", p}},
      copied),
    makePreset(
      CompileFamily::HalfLife,
      "full",
      "Release compile: csg <map>, bsp <map>, vis -full <map>, rad -extra <map> (VHLT / "
      "ZHLT; <map> is the exported map without extension).",
      {{"csg", p}, {"bsp", p}, {"vis", "-full " + p}, {"rad", "-extra " + p}},
      copied),
  };
}

std::vector<CompilePreset> quakePresets(
  const CompileFamily family,
  const std::string& bspTool,
  const std::string& bspOptions,
  const std::vector<std::string>& copied)
{
  const auto bspParameters = bspOptions + QuotedMap;
  const auto bspCommand = bspTool + " " + bspOptions + "<map>.map";
  return {
    makePreset(
      family,
      "fast",
      fmt::format(
        "Quick preview without vis: {}, light <map>.bsp (ericw-tools).", bspCommand),
      {{bspTool, bspParameters}, {"light", QuotedBsp}},
      copied),
    makePreset(
      family,
      "normal",
      fmt::format(
        "Standard compile: {}, vis <map>.bsp, light -extra <map>.bsp (ericw-tools).",
        bspCommand),
      {{bspTool, bspParameters}, {"vis", QuotedBsp}, {"light", "-extra " + QuotedBsp}},
      copied),
    makePreset(
      family,
      "full",
      fmt::format(
        "Release compile: {}, vis -level 4 <map>.bsp, light -extra4 -bounce <map>.bsp "
        "(ericw-tools).",
        bspCommand),
      {{bspTool, bspParameters},
       {"vis", "-level 4 " + QuotedBsp},
       {"light", "-extra4 -bounce " + QuotedBsp}},
      copied),
  };
}

std::vector<CompilePreset> quake3Presets()
{
  const auto stage = [](const std::string& options) {
    return ToolStep{"q3map2", Q3Map2GameOptions + " " + options + " " + QuotedMap};
  };
  const auto copied = std::vector<std::string>{".bsp"};
  return {
    makePreset(
      CompileFamily::Quake3,
      "fast",
      "Quick preview without vis: q3map2 -meta <map>.map, q3map2 -light -fast <map>.map "
      "(each stage also gets -game quake3 -fs_basepath <game dir> -fs_game <mod>).",
      {stage("-meta"), stage("-light -fast")},
      copied),
    makePreset(
      CompileFamily::Quake3,
      "normal",
      "Standard compile: q3map2 -meta <map>.map, q3map2 -vis -saveprt <map>.map, q3map2 "
      "-light -fast -filter <map>.map (each stage also gets -game quake3 -fs_basepath "
      "<game dir> -fs_game <mod>).",
      {stage("-meta"), stage("-vis -saveprt"), stage("-light -fast -filter")},
      copied),
    makePreset(
      CompileFamily::Quake3,
      "full",
      "Release compile: q3map2 -meta <map>.map, q3map2 -vis -saveprt <map>.map, q3map2 "
      "-light -fast -super 2 -filter -bounce 8 <map>.map (each stage also gets -game "
      "quake3 -fs_basepath <game dir> -fs_game <mod>).",
      {stage("-meta"),
       stage("-vis -saveprt"),
       stage("-light -fast -super 2 -filter -bounce 8")},
      copied),
  };
}

std::vector<CompilePreset> presetsOf(const CompileFamily family)
{
  switch (family)
  {
  case CompileFamily::HalfLife:
    return halfLifePresets();
  case CompileFamily::Quake:
    return quakePresets(family, "qbsp", "", {".bsp", ".lit"});
  case CompileFamily::Quake2:
    return quakePresets(family, "bsp", "-q2bsp ", {".bsp"});
  case CompileFamily::Quake3:
    return quake3Presets();
  }
  return {};
}

#ifdef _WIN32
bool isExecutable(const std::filesystem::path& path, const std::filesystem::file_status&)
{
  const auto extension = path.extension().string();
  return std::ranges::any_of(
    std::array{".exe", ".bat", ".cmd", ".com"},
    [&](const auto* candidate) { return kdl::ci::str_is_equal(extension, candidate); });
}
#else
bool isExecutable(
  const std::filesystem::path&, const std::filesystem::file_status& status)
{
  using std::filesystem::perms;
  return (status.permissions()
          & (perms::owner_exec | perms::group_exec | perms::others_exec))
         != perms::none;
}
#endif

// Task JSON

constexpr auto ExportMapType = "exportMap";
constexpr auto CopyFilesType = "copyFiles";
constexpr auto RenameFileType = "renameFile";
constexpr auto DeleteFilesType = "deleteFiles";
constexpr auto RunToolType = "runTool";
constexpr auto LaunchEngineType = "launchEngine";

const auto TaskTypes = std::vector<std::string>{
  ExportMapType,
  CopyFilesType,
  RenameFileType,
  DeleteFilesType,
  RunToolType,
  LaunchEngineType,
};

/** The keys a task of the given type may have besides "type" and "enabled". */
std::vector<std::string> taskKeys(const std::string& type)
{
  if (type == ExportMapType)
  {
    return {"target", "stripTbProperties", "stripEntityPattern", "entityToAdd"};
  }
  if (type == CopyFilesType || type == RenameFileType)
  {
    return {"source", "target"};
  }
  if (type == DeleteFilesType)
  {
    return {"target"};
  }
  if (type == RunToolType)
  {
    return {"tool", "parameters", "treatNonZeroExitCodeAsError"};
  }
  if (type == LaunchEngineType)
  {
    return {"engineProfile", "treatLaunchFailureAsError"};
  }
  return {};
}

/** The keys a task of the given type must have. */
std::vector<std::string> requiredTaskKeys(const std::string& type)
{
  if (type == ExportMapType || type == DeleteFilesType)
  {
    return {"target"};
  }
  if (type == CopyFilesType || type == RenameFileType)
  {
    return {"source", "target"};
  }
  if (type == RunToolType)
  {
    return {"tool"};
  }
  if (type == LaunchEngineType)
  {
    return {"engineProfile"};
  }
  return {};
}

Json toJson(const mdl::Entity& entity)
{
  auto properties = Json::object();
  for (const auto& property : entity.properties())
  {
    properties[property.key()] = property.value();
  }
  return Json{{"properties", std::move(properties)}};
}

mdl::Entity entityFromJson(const Json& json)
{
  auto properties = std::vector<mdl::EntityProperty>{};
  if (const auto* propertiesJson = findMember(json, "properties"))
  {
    if (!propertiesJson->is_object())
    {
      throw std::invalid_argument{"entityToAdd.properties must be an object"};
    }
    for (const auto& [key, value] : propertiesJson->items())
    {
      if (!value.is_string())
      {
        throw std::invalid_argument{
          fmt::format("entityToAdd.properties.{} must be a string", key)};
      }
      properties.emplace_back(key, value.get<std::string>());
    }
  }
  return mdl::Entity{std::move(properties)};
}

/** The value of the key, or nullopt if it is missing or null. */
const Json* memberOrNull(const Json& json, std::string_view key)
{
  const auto* member = findMember(json, key);
  return member && !member->is_null() ? member : nullptr;
}

template <typename T>
T valueOr(const Json& json, std::string_view key, T defaultValue)
{
  if (const auto* member = memberOrNull(json, key))
  {
    return member->get<T>();
  }
  return defaultValue;
}

/** "a runTool" or "an exportMap". */
std::string withArticle(const std::string& type)
{
  return (std::string_view{"aeiou"}.find(type.front()) != std::string_view::npos ? "an "
                                                                                 : "a ")
         + type;
}

std::string prefixed(const std::string& path, const std::string& message)
{
  return path.empty() ? message : path + ": " + message;
}

schema::Schema propertiesSchema()
{
  return schema::object({}).allowAdditionalProperties().withCheck(
    [](const Json& value) -> std::optional<std::string> {
      for (const auto& [key, member] : value.items())
      {
        if (!member.is_string())
        {
          return fmt::format("property '{}' must be a string", key);
        }
      }
      return std::nullopt;
    });
}

} // namespace

std::string_view toString(const CompileFamily family)
{
  switch (family)
  {
  case CompileFamily::HalfLife:
    return "halflife";
  case CompileFamily::Quake:
    return "quake";
  case CompileFamily::Quake2:
    return "quake2";
  case CompileFamily::Quake3:
    return "quake3";
  }
  return "";
}

std::optional<CompileFamily> compileFamily(const mdl::GameConfig& gameConfig)
{
  const auto hasTool = [&](std::string_view name) {
    return std::ranges::any_of(gameConfig.compilationTools, [&](const auto& tool) {
      return kdl::ci::str_is_equal(tool.name, name);
    });
  };
  const auto hasQuake2Format = std::ranges::any_of(
    gameConfig.fileFormats,
    [](const auto& format) { return kdl::cs::str_is_prefix(format.format, "Quake2"); });

  if (hasTool("csg") && hasTool("bsp") && hasTool("vis") && hasTool("rad"))
  {
    return CompileFamily::HalfLife;
  }
  if (hasTool("qbsp") && hasTool("vis") && hasTool("light"))
  {
    return CompileFamily::Quake;
  }
  if (
    hasTool("bsp") && hasTool("vis") && hasTool("light") && !hasTool("csg")
    && hasQuake2Format)
  {
    return CompileFamily::Quake2;
  }
  // q3map2 is used by other games (e.g. Wrath) with other -game arguments
  if (hasTool("q3map2") && gameConfig.fileSystemConfig.searchPath == "baseq3")
  {
    return CompileFamily::Quake3;
  }
  return std::nullopt;
}

std::vector<CompilePreset> compilePresets(const mdl::GameConfig& gameConfig)
{
  if (const auto family = compileFamily(gameConfig))
  {
    return presetsOf(*family);
  }
  return {};
}

std::optional<CompilePreset> findCompilePreset(
  const mdl::GameConfig& gameConfig, const std::string_view name)
{
  auto presets = compilePresets(gameConfig);
  const auto it = std::ranges::find_if(presets, [&](const auto& preset) {
    return kdl::ci::str_is_equal(preset.name, name);
  });
  return it != presets.end() ? std::optional{std::move(*it)} : std::nullopt;
}

std::string compilePresetsSummary()
{
  return R"(Compile presets ("fast", "normal", "full") exist for four tool chains, chosen by the game's compilation tools:
- Half-Life (tools csg, bsp, vis, rad; VHLT / ZHLT; path without extension): fast = csg, bsp, rad -fast (no vis); normal = csg, bsp, vis, rad; full = csg, bsp, vis -full, rad -extra.
- Quake (tools qbsp, vis, light; ericw-tools): fast = qbsp <map>.map, light <map>.bsp (no vis); normal = qbsp, vis, light -extra; full = qbsp, vis -level 4, light -extra4 -bounce.
- Quake 2 (tools bsp, vis, light; ericw-tools 2.x): as Quake, but the bsp tool gets -q2bsp.
- Quake 3 (tool q3map2, every stage with -game quake3 -fs_basepath <game dir> -fs_game <mod>): fast = -meta, -light -fast (no vis); normal = -meta, -vis -saveprt, -light -fast -filter; full = -meta, -vis -saveprt, -light -fast -super 2 -filter -bounce 8.
Every preset exports the map (without TrenchBroom properties) to compile/<map>.map next to the map file, runs the tools on it in that compile/ folder (a non-zero exit code fails the run), and copies compile/<map>.bsp (Quake: also .lit) into <game dir>/<last mod>/maps. The tool paths are the compilation tool variables such as ${qbsp}; set them in the game configuration. On a leak the bsp tool writes compile/<map>.pts (q3map2: .lin) next to the exported map.)";
}

std::string checkCompileToolPath(const std::filesystem::path& path)
{
  if (path.empty())
  {
    return "notSet";
  }

  auto error = std::error_code{};
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::exists(status))
  {
    return "notFound";
  }
  if (!std::filesystem::is_regular_file(status))
  {
    return "notAFile";
  }
  if (!isExecutable(path, status))
  {
    return "notExecutable";
  }
  return "ok";
}

CompileToolStatus compileToolStatus(const mdl::CompilationTool& tool)
{
  const auto& path = pref(tool.pathPreference);
  return CompileToolStatus{
    tool.name,
    tool.description,
    path,
    checkCompileToolPath(path),
  };
}

Json toJson(const CompileToolStatus& status)
{
  return Json{
    {"name", status.name},
    {"description", status.description ? Json(*status.description) : Json(nullptr)},
    {"variable", "${" + status.name + "}"},
    {"path", status.path.string()},
    {"status", status.status},
  };
}

std::string taskType(const mdl::CompilationTask& task)
{
  return std::visit(
    kdl::overload(
      [](const mdl::CompilationExportMap&) { return std::string{ExportMapType}; },
      [](const mdl::CompilationCopyFiles&) { return std::string{CopyFilesType}; },
      [](const mdl::CompilationRenameFile&) { return std::string{RenameFileType}; },
      [](const mdl::CompilationDeleteFiles&) { return std::string{DeleteFilesType}; },
      [](const mdl::CompilationRunTool&) { return std::string{RunToolType}; },
      [](const mdl::CompilationLaunchEngine&) { return std::string{LaunchEngineType}; }),
    task);
}

bool isTaskEnabled(const mdl::CompilationTask& task)
{
  return std::visit([](const auto& t) { return t.enabled; }, task);
}

std::vector<mdl::CompilationTask> enabledTasks(const mdl::CompilationProfile& profile)
{
  auto result = std::vector<mdl::CompilationTask>{};
  std::ranges::copy_if(profile.tasks, std::back_inserter(result), isTaskEnabled);
  return result;
}

Json toJson(const mdl::CompilationTask& task)
{
  auto result = Json{
    {"type", taskType(task)},
    {"enabled", isTaskEnabled(task)},
  };
  std::visit(
    kdl::overload(
      [&](const mdl::CompilationExportMap& t) {
        result["target"] = t.targetSpec;
        result["stripTbProperties"] = t.stripTbProperties;
        result["stripEntityPattern"] =
          t.stripEntityPattern ? Json(*t.stripEntityPattern) : Json(nullptr);
        result["entityToAdd"] = t.entityToAdd ? toJson(*t.entityToAdd) : Json(nullptr);
      },
      [&](const mdl::CompilationCopyFiles& t) {
        result["source"] = t.sourceSpec;
        result["target"] = t.targetSpec;
      },
      [&](const mdl::CompilationRenameFile& t) {
        result["source"] = t.sourceSpec;
        result["target"] = t.targetSpec;
      },
      [&](const mdl::CompilationDeleteFiles& t) { result["target"] = t.targetSpec; },
      [&](const mdl::CompilationRunTool& t) {
        result["tool"] = t.toolSpec;
        result["parameters"] = t.parameterSpec;
        result["treatNonZeroExitCodeAsError"] = t.treatNonZeroResultCodeAsError;
      },
      [&](const mdl::CompilationLaunchEngine& t) {
        result["engineProfile"] = t.engineProfileId;
        result["treatLaunchFailureAsError"] = t.treatLaunchFailureAsError;
      }),
    task);
  return result;
}

Json toJson(const mdl::CompilationProfile& profile)
{
  auto tasks = Json::array();
  for (const auto& task : profile.tasks)
  {
    tasks.push_back(toJson(task));
  }
  return Json{
    {"name", profile.name},
    {"workDir", profile.workDirSpec},
    {"tasks", std::move(tasks)},
  };
}

schema::Schema compilationTaskSchema()
{
  using namespace schema;
  return object(
           {
             field("type", enumOf(TaskTypes))
               .required()
               .describe(
                 "The task type. exportMap writes the map to a file; copyFiles copies "
                 "files into a directory; renameFile moves a file; deleteFiles deletes "
                 "files; runTool runs a program; launchEngine starts a game engine "
                 "profile"),
             field("enabled", boolean())
               .defaultsTo(true)
               .describe("Disabled tasks are kept in the profile but not run"),
             field("target", string())
               .describe(
                 "exportMap: the map file to write (required). copyFiles: the directory "
                 "to copy into (required; created if missing). renameFile: the new path "
                 "(required). deleteFiles: the file(s) to delete (required; wildcards * "
                 "and ? in the file name). Variables such as ${WORK_DIR_PATH} are "
                 "allowed; relative paths are relative to the working directory"),
             field("source", string())
               .describe(
                 "copyFiles: the file(s) to copy (required; wildcards * and ? in the "
                 "file name; no match copies nothing). renameFile: the file to move "
                 "(required). Variables are allowed; relative paths are relative to the "
                 "working directory"),
             field("tool", string())
               .describe(
                 "runTool: the program to run (required), usually a compilation tool "
                 "variable such as ${qbsp} or an absolute path"),
             field("parameters", string())
               .describe(
                 "runTool: the arguments (default \"\"). Arguments are separated by "
                 "spaces; double quotes group an argument that contains spaces, so quote "
                 "file paths, e.g. \"${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map\""),
             field("stripTbProperties", boolean())
               .describe(
                 "exportMap: remove TrenchBroom's own _tb_ properties from the exported "
                 "map (default true); some compilers cannot handle them"),
             field("stripEntityPattern", string())
               .describe(
                 "exportMap: a glob; entities whose classname matches it are left out of "
                 "the exported map, e.g. \"info_player_*\""),
             field(
               "entityToAdd",
               object({
                 field("properties", propertiesSchema())
                   .required()
                   .describe(
                     "The entity's properties as key/value strings, e.g. {\"classname\": "
                     "\"info_player_start\"}"),
               }))
               .describe(
                 "exportMap: an entity added to the exported map at the 3D camera "
                 "position, with its angle set to the camera yaw"),
             field("treatNonZeroExitCodeAsError", boolean())
               .describe(
                 "runTool: whether a non-zero exit code fails the task and stops the run "
                 "(default true)"),
             field("engineProfile", string())
               .describe("launchEngine: the id of the game engine profile (required)"),
             field("treatLaunchFailureAsError", boolean())
               .describe("launchEngine: whether a failure to launch fails the run "
                         "(default true)"),
           })
    .describe(
      "A compilation task. Besides \"type\" and \"enabled\", only the keys of its type "
      "are allowed");
}

schema::Schema compilationProfileSchema()
{
  using namespace schema;
  return object({
    field("name", string().nonEmpty()).required().describe("The profile name"),
    field("workDir", string())
      .defaultsTo(MapDirPath)
      .describe(
        "The working directory of the tools; variables such as ${MAP_DIR_PATH} are "
        "allowed. It is available to the tasks as ${WORK_DIR_PATH}"),
    field("tasks", array(compilationTaskSchema()))
      .required()
      .describe("The tasks, run in order; the run stops at the first failing task"),
  });
}

Result<mdl::CompilationTask, ToolError> compilationTaskFromJson(
  const Json& json, const std::string& path)
{
  const auto fail =
    [&](const std::string& message) -> Result<mdl::CompilationTask, ToolError> {
    return makeError(ErrorCode::InvalidArgument, prefixed(path, message));
  };

  if (!json.is_object())
  {
    return fail("a task must be an object");
  }

  const auto* typeJson = memberOrNull(json, "type");
  if (!typeJson || !typeJson->is_string())
  {
    return fail(fmt::format(
      "the task type is missing; expected one of {}", kdl::str_join(TaskTypes, ", ")));
  }

  const auto type = typeJson->get<std::string>();
  if (std::ranges::find(TaskTypes, type) == TaskTypes.end())
  {
    return fail(fmt::format(
      "unknown task type '{}'; expected one of {}",
      type,
      kdl::str_join(TaskTypes, ", ")));
  }

  const auto keys = taskKeys(type);
  for (const auto& [key, value] : json.items())
  {
    if (
      key != "type" && key != "enabled" && !value.is_null()
      && std::ranges::find(keys, key) == keys.end())
    {
      return fail(fmt::format(
        "'{}' does not belong to {} task; allowed keys: type, enabled, {}",
        key,
        withArticle(type),
        kdl::str_join(keys, ", ")));
    }
  }

  for (const auto& key : requiredTaskKeys(type))
  {
    if (!memberOrNull(json, key))
    {
      return fail(fmt::format("{} task requires '{}'", withArticle(type), key));
    }
  }

  try
  {
    const auto enabled = valueOr(json, "enabled", true);
    if (type == ExportMapType)
    {
      const auto* entityToAdd = memberOrNull(json, "entityToAdd");
      const auto* stripEntityPattern = memberOrNull(json, "stripEntityPattern");
      return mdl::CompilationTask{mdl::CompilationExportMap{
        .enabled = enabled,
        .stripTbProperties = valueOr(json, "stripTbProperties", true),
        .stripEntityPattern = stripEntityPattern
                                ? std::optional{stripEntityPattern->get<std::string>()}
                                : std::nullopt,
        .entityToAdd =
          entityToAdd ? std::optional{entityFromJson(*entityToAdd)} : std::nullopt,
        .targetSpec = json.at("target").get<std::string>(),
      }};
    }
    if (type == CopyFilesType)
    {
      return mdl::CompilationTask{mdl::CompilationCopyFiles{
        .enabled = enabled,
        .sourceSpec = json.at("source").get<std::string>(),
        .targetSpec = json.at("target").get<std::string>(),
      }};
    }
    if (type == RenameFileType)
    {
      return mdl::CompilationTask{mdl::CompilationRenameFile{
        .enabled = enabled,
        .sourceSpec = json.at("source").get<std::string>(),
        .targetSpec = json.at("target").get<std::string>(),
      }};
    }
    if (type == DeleteFilesType)
    {
      return mdl::CompilationTask{mdl::CompilationDeleteFiles{
        .enabled = enabled,
        .targetSpec = json.at("target").get<std::string>(),
      }};
    }
    if (type == RunToolType)
    {
      return mdl::CompilationTask{mdl::CompilationRunTool{
        .enabled = enabled,
        .toolSpec = json.at("tool").get<std::string>(),
        .parameterSpec = valueOr(json, "parameters", std::string{}),
        .treatNonZeroResultCodeAsError =
          valueOr(json, "treatNonZeroExitCodeAsError", true),
      }};
    }
    return mdl::CompilationTask{mdl::CompilationLaunchEngine{
      .enabled = enabled,
      .engineProfileId = json.at("engineProfile").get<std::string>(),
      .treatLaunchFailureAsError = valueOr(json, "treatLaunchFailureAsError", true),
    }};
  }
  catch (const Json::exception& e)
  {
    return fail(fmt::format("invalid {} task: {}", type, e.what()));
  }
  catch (const std::invalid_argument& e)
  {
    return fail(e.what());
  }
}

Result<mdl::CompilationProfile, ToolError> compilationProfileFromJson(const Json& json)
{
  if (!json.is_object())
  {
    return makeError(ErrorCode::InvalidArgument, "a profile must be an object");
  }

  const auto* name = memberOrNull(json, "name");
  if (!name || !name->is_string() || name->get<std::string>().empty())
  {
    return makeError(ErrorCode::InvalidArgument, "the profile requires a non-empty name");
  }

  const auto* workDir = memberOrNull(json, "workDir");
  if (workDir && !workDir->is_string())
  {
    return makeError(ErrorCode::InvalidArgument, "workDir must be a string");
  }

  const auto* tasksJson = memberOrNull(json, "tasks");
  if (!tasksJson || !tasksJson->is_array())
  {
    return makeError(ErrorCode::InvalidArgument, "the profile requires a tasks array");
  }

  auto tasks = std::vector<mdl::CompilationTask>{};
  for (size_t i = 0; i < tasksJson->size(); ++i)
  {
    auto task =
      compilationTaskFromJson((*tasksJson)[i], "tasks[" + std::to_string(i) + "]");
    if (task.is_error())
    {
      return errorOf(task);
    }
    tasks.push_back(std::move(task).value());
  }

  return mdl::CompilationProfile{
    name->get<std::string>(),
    workDir ? workDir->get<std::string>() : MapDirPath,
    std::move(tasks),
  };
}

} // namespace tb::mcp
