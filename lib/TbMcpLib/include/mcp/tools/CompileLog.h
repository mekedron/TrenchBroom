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

#include "mdl/CompilationTask.h"

#include "vm/vec.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

// Analysis of the log of a compile run: the editor's compilation runner writes a
// "#### ..." line when a task starts, fails or finishes; the tools write their own
// errors, warnings and leak reports in between.

enum class CompileTaskState
{
  /** Not started yet. */
  Pending,
  Running,
  Succeeded,
  Failed,
  /** Not run because an earlier task failed or the run was terminated. */
  Skipped,
};

/** "pending", "running", "succeeded", "failed", "skipped". */
std::string_view toString(CompileTaskState state);

struct CompileTaskReport
{
  /** Index among the enabled tasks of the run. */
  size_t index = 0;
  /** See taskType() in CompileUtils.h. */
  std::string type;
  CompileTaskState state = CompileTaskState::Pending;
  /**
   * What the task did according to the log, e.g. the executed command line
   * "/opt/vhlt/hlcsg /maps/compile/test", the exported file, or "copy a.bsp to /maps".
   */
  std::string description;
  /** runTool: the exit code, if the tool finished or crashed. */
  std::optional<int> exitCode;
  /** The first log line (1-based) of the task. */
  std::optional<size_t> line;
};

struct CompileMessage
{
  /** The message without its severity prefix, e.g. "Could not open wad file x.wad". */
  std::string message;
  /** The log line (1-based). */
  size_t line = 0;
  /** The enabled task that wrote it, if any. */
  std::optional<size_t> task;
};

struct CompileLeak
{
  /** The log line (1-based) that reported the leak. */
  size_t line = 0;
  /** The line itself. */
  std::string text;
  /** The classname of the entity the leak was reached from, if the tool reports it. */
  std::optional<std::string> entity;
  /** The position of that entity, if the tool reports it. */
  std::optional<vm::vec3d> position;
  /** The point file the tool reported writing, if any (as written in the log). */
  std::optional<std::filesystem::path> pointFile;
};

struct CompileFileCopy
{
  std::filesystem::path source;
  /** The target directory (copyFiles) or file (renameFile). */
  std::filesystem::path target;
};

struct CompileLogAnalysis
{
  /** One report per enabled task, in order. */
  std::vector<CompileTaskReport> tasks;
  /** The index of the running task, if the run has not ended. */
  std::optional<size_t> currentTask;
  /** The number of tasks that succeeded. */
  size_t completedTasks = 0;
  /** Whether a task failed or the runner reported an error that stopped the run. */
  bool failed = false;
  /** Whether the log contains "#### Terminated". */
  bool terminated = false;
  std::vector<CompileMessage> errors;
  std::vector<CompileMessage> warnings;
  std::optional<CompileLeak> leak;
  /** The files written by export tasks ("#### Exporting map file '...'"). */
  std::vector<std::filesystem::path> exportedMaps;
  /** The files copied by copy tasks ("#### Copying to '<dir>/': a, b"). */
  std::vector<CompileFileCopy> copiedFiles;
  /** The files renamed by rename tasks. */
  std::vector<CompileFileCopy> renamedFiles;
};

/**
 * Analyzes the log of a run of the given enabled tasks. `ended` says whether the job has
 * ended; a task without an outcome is then failed (or skipped if terminated), otherwise
 * it is running.
 *
 * Recognizes the runner's lines and the messages of common tools: VHLT / ZHLT
 * ("Error: ...", "Warning: ...", "=== LEAK in hull 0 ===", "Entity info_player_start @
 * (x, y, z)"), ericw-tools and tyrutils ("ERROR: ...", "WARNING: ...", "WARNING 13: ...",
 * "Reached occupant \"classname\" at (x y z), no filling performed.", "Leak file written
 * to x.pts"), q3map2 and Quake 2 tools ("************ ERROR ************" followed by
 * the message, "WARNING: ...", "******* leaked *******", "**** leaked ****").
 */
CompileLogAnalysis analyzeCompileLog(
  std::string_view log,
  const std::vector<mdl::CompilationTask>& enabledTasks,
  bool ended);

/** The last `count` lines of the log, and whether lines were omitted. */
std::pair<std::string, bool> logTail(std::string_view log, size_t count);

} // namespace tb::mcp
