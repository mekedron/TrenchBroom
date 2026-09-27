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

#pragma once

#include "base/Result.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/Schema.h"
#include "mdl/CompilationProfile.h"
#include "mdl/CompilationTask.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
struct CompilationTool;
struct GameConfig;
} // namespace tb::mdl

namespace tb::mcp
{

// Helpers of the compile tools: compile presets per game family, compile tool checks and
// the JSON form of compilation profiles.

/** The tool chains that compile presets exist for. */
enum class CompileFamily
{
  /** Half-Life: csg, bsp, vis, rad (VHLT / ZHLT). */
  HalfLife,
  /** Quake: qbsp, vis, light (ericw-tools). */
  Quake,
  /** Quake 2: bsp, vis, light (ericw-tools 2.x with -q2bsp). */
  Quake2,
  /** Quake 3: q3map2 stages. */
  Quake3,
};

/** "halflife", "quake", "quake2", "quake3". */
std::string_view toString(CompileFamily family);

/**
 * The compile family of a game, determined by the names of its compilation tools:
 * csg/bsp/vis/rad (Half-Life), qbsp/vis/light (Quake), bsp/vis/light with a Quake 2 map
 * format (Quake 2), q3map2 with the search path baseq3 (Quake 3); otherwise nullopt.
 */
std::optional<CompileFamily> compileFamily(const mdl::GameConfig& gameConfig);

/**
 * A built-in compile profile. Every preset uses ${MAP_DIR_PATH} as the working directory,
 * exports the map to ${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map (stripping TrenchBroom
 * properties), runs the tool chain on it with treatNonZeroResultCodeAsError, and copies
 * compile/${MAP_BASE_NAME}.bsp (and related files such as .lit) into
 * ${GAME_DIR_PATH}/${MODS[-1]}/maps.
 */
struct CompilePreset
{
  /** "fast", "normal" or "full". */
  std::string name;
  /** What the preset does, including the tool arguments, in one or two sentences. */
  std::string description;
  /** The profile; its name is e.g. "Half-Life (normal)". */
  mdl::CompilationProfile profile;
  /** The compilation tool variables the profile uses, e.g. {"csg", "bsp", "rad"}. */
  std::vector<std::string> tools;
};

/** The presets of the game's family (fast, normal, full), or an empty list. */
std::vector<CompilePreset> compilePresets(const mdl::GameConfig& gameConfig);

/** The preset with the given name (case-insensitive) for the game, or nullopt. */
std::optional<CompilePreset> findCompilePreset(
  const mdl::GameConfig& gameConfig, std::string_view name);

/**
 * A summary of the presets of all families and their tool arguments, for tool
 * descriptions.
 */
std::string compilePresetsSummary();

/** The state of a compile tool path. */
struct CompileToolStatus
{
  std::string name;
  std::optional<std::string> description;
  /** The configured path; empty if not set. */
  std::filesystem::path path;
  /** "ok", "notSet", "notFound", "notAFile" or "notExecutable". */
  std::string status;
};

/** Checks the given path: "ok", "notSet", "notFound", "notAFile" or "notExecutable". */
std::string checkCompileToolPath(const std::filesystem::path& path);

/** The status of the given compilation tool, reading its path preference. */
CompileToolStatus compileToolStatus(const mdl::CompilationTool& tool);

/** `{"name", "description", "variable": "${name}", "path", "status"}`. */
Json toJson(const CompileToolStatus& status);

/** The type name of a task: "exportMap", "copyFiles", "renameFile", "deleteFiles",
 * "runTool" or "launchEngine". */
std::string taskType(const mdl::CompilationTask& task);

/** Whether the task is enabled. */
bool isTaskEnabled(const mdl::CompilationTask& task);

/** The enabled tasks of a profile, in order. */
std::vector<mdl::CompilationTask> enabledTasks(const mdl::CompilationProfile& profile);

/**
 * The JSON form of a task. Every task has "type" and "enabled"; the other keys by type:
 * - exportMap: "target", "stripTbProperties", "stripEntityPattern" (string or null),
 *   "entityToAdd" ({"properties": {...}} or null; placed at the camera position)
 * - copyFiles: "source", "target"
 * - renameFile: "source", "target"
 * - deleteFiles: "target"
 * - runTool: "tool", "parameters", "treatNonZeroExitCodeAsError"
 * - launchEngine: "engineProfile", "treatLaunchFailureAsError"
 */
Json toJson(const mdl::CompilationTask& task);

/** `{"name", "workDir", "tasks": [...]}`. */
Json toJson(const mdl::CompilationProfile& profile);

/**
 * The input schema of a task: an object with "type" (required) and the optional keys of
 * all task types. Missing keys get defaults: enabled true, stripTbProperties true,
 * treatNonZeroExitCodeAsError true, treatLaunchFailureAsError true.
 */
schema::Schema compilationTaskSchema();

/** The input schema of a profile: "name" (required), "workDir", "tasks" (required). */
schema::Schema compilationProfileSchema();

/**
 * Converts a task validated by compilationTaskSchema. Returns INVALID_ARGUMENT for keys
 * that do not belong to the type and for missing required keys (runTool: "tool";
 * copyFiles and renameFile: "source" and "target"; deleteFiles and exportMap: "target";
 * launchEngine: "engineProfile"). `path` prefixes error messages, e.g. "tasks[2]".
 */
Result<mdl::CompilationTask, ToolError> compilationTaskFromJson(
  const Json& json, const std::string& path);

/**
 * Converts a profile validated by compilationProfileSchema. The work directory defaults
 * to "${MAP_DIR_PATH}".
 */
Result<mdl::CompilationProfile, ToolError> compilationProfileFromJson(const Json& json);

} // namespace tb::mcp
