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

#include "mcp/tools/CompileLog.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

using State = CompileTaskState;

mdl::CompilationTask exportMap()
{
  return mdl::CompilationExportMap{
    .enabled = true,
    .stripTbProperties = true,
    .stripEntityPattern = std::nullopt,
    .entityToAdd = std::nullopt,
    .targetSpec = "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map",
  };
}

mdl::CompilationTask runTool(
  const std::string& tool, const bool treatNonZeroResultCodeAsError = true)
{
  return mdl::CompilationRunTool{
    .enabled = true,
    .toolSpec = "${" + tool + "}",
    .parameterSpec = "",
    .treatNonZeroResultCodeAsError = treatNonZeroResultCodeAsError,
  };
}

mdl::CompilationTask copyFiles()
{
  return mdl::CompilationCopyFiles{
    .enabled = true,
    .sourceSpec = "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp",
    .targetSpec = "${GAME_DIR_PATH}/${MODS[-1]}/maps",
  };
}

mdl::CompilationTask renameFile()
{
  return mdl::CompilationRenameFile{
    .enabled = true,
    .sourceSpec = "a.bsp",
    .targetSpec = "b.bsp",
  };
}

mdl::CompilationTask deleteFiles()
{
  return mdl::CompilationDeleteFiles{
    .enabled = true,
    .targetSpec = "*.prt",
  };
}

mdl::CompilationTask launchEngine(const bool treatLaunchFailureAsError = true)
{
  return mdl::CompilationLaunchEngine{
    .enabled = true,
    .engineProfileId = "qs",
    .treatLaunchFailureAsError = treatLaunchFailureAsError,
  };
}

std::vector<mdl::CompilationTask> quakeTasks()
{
  return {exportMap(), runTool("qbsp"), runTool("vis"), runTool("light"), copyFiles()};
}

std::vector<State> states(const CompileLogAnalysis& analysis)
{
  auto result = std::vector<State>{};
  for (const auto& task : analysis.tasks)
  {
    result.push_back(task.state);
  }
  return result;
}

std::vector<std::string> messages(const std::vector<CompileMessage>& messages)
{
  auto result = std::vector<std::string>{};
  for (const auto& message : messages)
  {
    result.push_back(message.message);
  }
  return result;
}

const auto QuakeSuccessLog = std::string{R"(#### Using working directory '/home/user/maps'
#### Exporting map file '/home/user/maps/compile/start.map'
#### Executing '/opt/ericw/qbsp /home/user/maps/compile/start.map'
---- qbsp / ericw-tools 2.0.0-alpha8 ----
Input file: /home/user/maps/compile/start.map
Output file: /home/user/maps/compile/start.bsp
---- LoadMapFile ----
       1 entities
      86 brushes
WARNING: Texture wbrick1_5 not found
---- WriteBSPFile ----
Wrote /home/user/maps/compile/start.bsp
#### Finished with exit code 0

#### Executing '/opt/ericw/vis /home/user/maps/compile/start.bsp'
---- vis / ericw-tools 2.0.0-alpha8 ----
#### Finished with exit code 0

#### Executing '/opt/ericw/light -extra /home/user/maps/compile/start.bsp'
---- light / ericw-tools 2.0.0-alpha8 ----
WARNING: unmatched target "door1" in light entity
#### Finished with exit code 0

#### Copying to '/home/user/quake/id1/maps/': /home/user/maps/compile/start.bsp
)"};

} // namespace

TEST_CASE("CompileLog")
{
  SECTION("toString")
  {
    CHECK(toString(State::Pending) == "pending");
    CHECK(toString(State::Running) == "running");
    CHECK(toString(State::Succeeded) == "succeeded");
    CHECK(toString(State::Failed) == "failed");
    CHECK(toString(State::Skipped) == "skipped");
  }

  SECTION("analyzeCompileLog")
  {
    SECTION("successful run")
    {
      const auto analysis = analyzeCompileLog(QuakeSuccessLog, quakeTasks(), true);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Succeeded,
          State::Succeeded,
          State::Succeeded,
          State::Succeeded,
          State::Succeeded});
      CHECK(analysis.completedTasks == 5);
      CHECK(!analysis.failed);
      CHECK(!analysis.terminated);
      CHECK(analysis.currentTask == std::nullopt);
      CHECK(analysis.errors.empty());
      CHECK(
        messages(analysis.warnings)
        == std::vector<std::string>{
          "Texture wbrick1_5 not found",
          "unmatched target \"door1\" in light entity",
        });
      CHECK(analysis.warnings[0].line == 10);
      CHECK(analysis.warnings[0].task == 1);
      CHECK(analysis.warnings[1].task == 3);
      CHECK(analysis.leak == std::nullopt);

      CHECK(analysis.tasks[0].type == "exportMap");
      CHECK(analysis.tasks[0].description == "/home/user/maps/compile/start.map");
      CHECK(analysis.tasks[0].line == 2);
      CHECK(analysis.tasks[1].type == "runTool");
      CHECK(
        analysis.tasks[1].description
        == "/opt/ericw/qbsp /home/user/maps/compile/start.map");
      CHECK(analysis.tasks[1].exitCode == 0);
      CHECK(analysis.tasks[1].line == 3);
      CHECK(analysis.tasks[4].type == "copyFiles");
      CHECK(
        analysis.tasks[4].description
        == "copy /home/user/maps/compile/start.bsp to /home/user/quake/id1/maps");
      CHECK(analysis.tasks[4].exitCode == std::nullopt);

      CHECK(
        analysis.exportedMaps
        == std::vector<std::filesystem::path>{"/home/user/maps/compile/start.map"});
      REQUIRE(analysis.copiedFiles.size() == 1);
      CHECK(analysis.copiedFiles[0].source == "/home/user/maps/compile/start.bsp");
      CHECK(analysis.copiedFiles[0].target == "/home/user/quake/id1/maps");
    }

    SECTION("running")
    {
      const auto log = QuakeSuccessLog.substr(0, QuakeSuccessLog.find("---- vis"));
      const auto analysis = analyzeCompileLog(log, quakeTasks(), false);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Succeeded,
          State::Succeeded,
          State::Running,
          State::Pending,
          State::Pending});
      CHECK(analysis.currentTask == 2);
      CHECK(analysis.completedTasks == 2);
      CHECK(!analysis.failed);
    }

    SECTION("export running")
    {
      const auto analysis = analyzeCompileLog(
        "#### Using working directory '/maps'\n"
        "#### Exporting map file '/maps/compile/start.map'\n",
        quakeTasks(),
        false);
      CHECK(analysis.currentTask == 0);
      CHECK(analysis.tasks[0].state == State::Running);
    }

    SECTION("nothing written yet")
    {
      const auto analysis = analyzeCompileLog("", quakeTasks(), false);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Pending,
          State::Pending,
          State::Pending,
          State::Pending,
          State::Pending});
      CHECK(analysis.currentTask == std::nullopt);
      CHECK(!analysis.failed);
    }

    SECTION("ended without output")
    {
      const auto analysis = analyzeCompileLog("", quakeTasks(), true);
      CHECK(analysis.failed);
      CHECK(analysis.tasks[0].state == State::Skipped);
    }

    SECTION("non-zero exit code fails the run")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Exporting map file '/maps/compile/start.map'
#### Executing '/opt/ericw/qbsp /maps/compile/start.map'
---- qbsp / ericw-tools 0.18.1 ----
ERROR: line 1602: Brush with duplicate plane
#### Finished with exit code 1

)"};
      const auto analysis = analyzeCompileLog(log, quakeTasks(), true);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Succeeded,
          State::Failed,
          State::Skipped,
          State::Skipped,
          State::Skipped});
      CHECK(analysis.failed);
      CHECK(analysis.completedTasks == 1);
      CHECK(analysis.tasks[1].exitCode == 1);
      CHECK(
        messages(analysis.errors)
        == std::vector<std::string>{
          "line 1602: Brush with duplicate plane", "Finished with exit code 1"});
      CHECK(analysis.errors[0].line == 5);
      CHECK(analysis.errors[0].task == 1);
      CHECK(analysis.errors[1].line == 6);
    }

    SECTION("non-zero exit code of a tolerant task")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Executing '/opt/ericw/vis /maps/start.bsp'
#### Finished with exit code 1

#### Executing '/opt/ericw/light /maps/start.bsp'
#### Finished with exit code 0

)"};
      const auto analysis =
        analyzeCompileLog(log, {runTool("vis", false), runTool("light")}, true);
      CHECK(states(analysis) == std::vector<State>{State::Succeeded, State::Succeeded});
      CHECK(analysis.tasks[0].exitCode == 1);
      CHECK(!analysis.failed);
      CHECK(analysis.errors.empty());
    }

    SECTION("crash")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Executing '/opt/ericw/light /maps/start.bsp'
#### Error 'Crashed' occurred when communicating with process

#### Crashed with exit code 11

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("light"), copyFiles()}, true);
      CHECK(states(analysis) == std::vector<State>{State::Failed, State::Skipped});
      CHECK(analysis.tasks[0].exitCode == 11);
      CHECK(
        messages(analysis.errors)
        == std::vector<std::string>{
          "Error 'Crashed' occurred when communicating with process",
          "Crashed with exit code 11"});
    }

    SECTION("process failed to start")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Executing '/missing/qbsp /maps/start.map'
#### Error 'FailedToStart' occurred when communicating with process

#### Execution failed: Failed to start process
)"};
      const auto analysis =
        analyzeCompileLog(log, {runTool("qbsp"), runTool("vis")}, true);
      CHECK(states(analysis) == std::vector<State>{State::Failed, State::Skipped});
      CHECK(analysis.errors.size() == 2);
      CHECK(analysis.errors[1].task == 0);
    }

    SECTION("failure before the start line")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Exporting map file '/maps/compile/start.map'
#### Execution failed: Could not interpolate expression '${qbsp}': unknown variable
)"};
      const auto analysis = analyzeCompileLog(log, quakeTasks(), true);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Succeeded,
          State::Failed,
          State::Skipped,
          State::Skipped,
          State::Skipped});
      CHECK(analysis.tasks[1].line == 3);
      CHECK(analysis.errors.size() == 1);
      CHECK(analysis.errors[0].task == 1);
    }

    SECTION("export failure")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Exporting map file '/maps/compile/start.map'
#### Export failed: Could not create directory '/maps/compile'
)"};
      const auto analysis = analyzeCompileLog(log, quakeTasks(), true);
      CHECK(analysis.tasks[0].state == State::Failed);
      CHECK(analysis.tasks[1].state == State::Skipped);
      CHECK(
        messages(analysis.errors)
        == std::vector<std::string>{
          "Export failed: Could not create directory '/maps/compile'"});
    }

    SECTION("copy, rename and delete lines")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Copying to '/games/quake/id1/maps/': /maps/compile/a.bsp, /maps/compile/a.lit
#### Copying to '/games/quake/id1/maps/': "/maps/my maps/b.bsp"
#### Renaming '/maps/a.bsp' to '/maps/b.bsp'
#### Deleting: /maps/a.prt, /maps/b.prt
#### Copying to '/games/quake/id1/maps/':
)"};
      const auto analysis = analyzeCompileLog(
        log, {copyFiles(), copyFiles(), renameFile(), deleteFiles(), copyFiles()}, true);
      CHECK(analysis.completedTasks == 5);
      CHECK(!analysis.failed);
      REQUIRE(analysis.copiedFiles.size() == 3);
      CHECK(analysis.copiedFiles[0].source == "/maps/compile/a.bsp");
      CHECK(analysis.copiedFiles[0].target == "/games/quake/id1/maps");
      CHECK(analysis.copiedFiles[1].source == "/maps/compile/a.lit");
      CHECK(analysis.copiedFiles[2].source == "/maps/my maps/b.bsp");
      REQUIRE(analysis.renamedFiles.size() == 1);
      CHECK(analysis.renamedFiles[0].source == "/maps/a.bsp");
      CHECK(analysis.renamedFiles[0].target == "/maps/b.bsp");
      CHECK(analysis.tasks[2].description == "rename /maps/a.bsp to /maps/b.bsp");
      CHECK(analysis.tasks[3].description == "delete /maps/a.prt, /maps/b.prt");
      CHECK(
        analysis.tasks[4].description
        == "copy nothing (no matching files) to /games/quake/id1/maps");
    }

    SECTION("copy, rename and delete failures")
    {
      CHECK(
        states(analyzeCompileLog(
          "#### Copy failed: Could not copy\n", {copyFiles(), copyFiles()}, true))
        == std::vector<State>{State::Failed, State::Skipped});
      CHECK(
        states(analyzeCompileLog(
          "#### Renaming '/a' to '/b'\n#### Rename failed: Could not move\n",
          {renameFile()},
          true))
        == std::vector<State>{State::Failed});
      CHECK(
        states(analyzeCompileLog(
          "#### Deleting: /a.prt\n#### Delete failed: Could not delete\n",
          {deleteFiles()},
          true))
        == std::vector<State>{State::Failed});
    }

    SECTION("launch engine")
    {
      const auto launched = analyzeCompileLog(
        "#### Launching engine profile 'QuakeSpasm' at '/games/quakespasm'\n"
        "#### Launched\n",
        {launchEngine()},
        true);
      CHECK(launched.tasks[0].state == State::Succeeded);
      CHECK(launched.tasks[0].description == "launch QuakeSpasm at /games/quakespasm");

      const auto failed = analyzeCompileLog(
        "#### Launch failed: Engine profile 'qs' was not found\n",
        {launchEngine()},
        true);
      CHECK(failed.tasks[0].state == State::Failed);
      CHECK(failed.failed);
      CHECK(failed.errors.size() == 1);

      const auto continued = analyzeCompileLog(
        "#### Launch failed: Engine profile is not set\n"
        "#### Continuing despite launch failure\n",
        {launchEngine(false)},
        true);
      CHECK(continued.tasks[0].state == State::Succeeded);
      CHECK(!continued.failed);
      CHECK(continued.errors.empty());
      CHECK(
        messages(continued.warnings)
        == std::vector<std::string>{"Launch failed: Engine profile is not set"});
    }

    SECTION("working directory")
    {
      const auto missing = analyzeCompileLog(
        "#### Error: working directory '/maps' does not exist\n"
        "#### Exporting map file '/maps/compile/start.map'\n",
        {exportMap()},
        true);
      CHECK(missing.tasks[0].state == State::Succeeded);
      CHECK(!missing.failed);
      CHECK(
        messages(missing.warnings)
        == std::vector<std::string>{"Error: working directory '/maps' does not exist"});

      const auto fatal = analyzeCompileLog(
        "#### Error: Could not get determine working directory: unknown variable\n",
        {exportMap(), runTool("qbsp")},
        false);
      CHECK(fatal.failed);
      CHECK(fatal.currentTask == std::nullopt);
      CHECK(states(fatal) == std::vector<State>{State::Skipped, State::Skipped});
      CHECK(fatal.errors.size() == 1);
    }

    SECTION("termination")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Exporting map file '/maps/compile/start.map'
#### Executing '/opt/ericw/qbsp /maps/compile/start.map'
---- qbsp / ericw-tools 2.0.0 ----
---- LoadMapFile ----


#### Terminated
)"};
      const auto analysis = analyzeCompileLog(log, quakeTasks(), true);
      CHECK(analysis.terminated);
      CHECK(!analysis.failed);
      CHECK(analysis.currentTask == std::nullopt);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Succeeded,
          State::Skipped,
          State::Skipped,
          State::Skipped,
          State::Skipped});
      CHECK(analysis.completedTasks == 1);

      const auto withReason = analyzeCompileLog(
        "#### Executing 'qbsp a.map'\n\n\n#### Terminated: timeout after 600 s\n",
        {runTool("qbsp")},
        false);
      CHECK(withReason.terminated);
      CHECK(withReason.currentTask == std::nullopt);
      CHECK(withReason.tasks[0].state == State::Skipped);
    }

    SECTION("run ended in the middle of a task")
    {
      const auto log = std::string{R"(#### Exporting map file '/maps/compile/start.map'
#### Executing '/opt/ericw/qbsp /maps/compile/start.map'
---- qbsp ----
)"};
      const auto analysis = analyzeCompileLog(log, quakeTasks(), true);
      CHECK(analysis.failed);
      CHECK(analysis.tasks[1].state == State::Failed);
      CHECK(analysis.tasks[2].state == State::Skipped);
    }

    SECTION("test run")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Exporting map file '/maps/compile/start.map'
#### Executing '/opt/ericw/qbsp /maps/compile/start.map'
#### Executing '/opt/ericw/vis /maps/compile/start.bsp'
#### Executing '/opt/ericw/light /maps/compile/start.bsp'
#### Copying to '/games/quake/id1/maps/':
)"};
      const auto analysis = analyzeCompileLog(log, quakeTasks(), true);
      CHECK(analysis.completedTasks == 5);
      CHECK(!analysis.failed);

      const auto endsWithTool = analyzeCompileLog(
        "#### Exporting map file '/maps/compile/start.map'\n"
        "#### Executing '/opt/ericw/qbsp /maps/compile/start.map'\n",
        {exportMap(), runTool("qbsp")},
        true);
      CHECK(
        states(endsWithTool) == std::vector<State>{State::Succeeded, State::Succeeded});
      CHECK(endsWithTool.tasks[1].exitCode == std::nullopt);
    }

    SECTION("more runner lines than tasks")
    {
      const auto analysis = analyzeCompileLog(
        "#### Executing 'a'\n#### Finished with exit code 0\n\n#### Executing 'b'\n"
        "Error: something\n",
        {runTool("a")},
        true);
      CHECK(analysis.tasks.size() == 1);
      CHECK(analysis.tasks[0].state == State::Succeeded);
      CHECK(analysis.errors.size() == 1);
      CHECK(analysis.errors[0].task == std::nullopt);
    }

    SECTION("VHLT leak")
    {
      const auto log = std::string{R"(#### Using working directory '/maps'
#### Exporting map file '/maps/compile/test.map'
#### Executing '/opt/vhlt/hlcsg /maps/compile/test'
hlcsg v3.4 VHLT (Sep 29 2015)
Warning: Could not locate wad file halflife.wad
#### Finished with exit code 0

#### Executing '/opt/vhlt/hlbsp /maps/compile/test'
hlbsp v3.4 VHLT (Sep 29 2015)
SolidBSP [hull 0] 175...
Warning: === LEAK in hull 0 ===
Entity info_player_start @ (  -64, -64,  36)
  Error:
  A LEAK is a hole in the map, where the inside of it is exposed to the
(unwanted) outside region.  The entity listed in the error is just a helpful
indication of where the beginning of the leak pointfile starts, so the
beginning of the line can be quickly found and traced to until reaching the
outside. Unless this entity is accidentally on the outside of the map, it
probably should not be deleted.  Some complex rotating objects entities need
their origins outside the map.  To deal with these, just enclose the origin
brush with a solid world brush

Leak pointfile generated

#### Finished with exit code 1

)"};
      const auto analysis = analyzeCompileLog(
        log,
        {exportMap(), runTool("csg"), runTool("bsp"), runTool("vis"), runTool("rad")},
        true);
      CHECK(
        states(analysis)
        == std::vector<State>{
          State::Succeeded,
          State::Succeeded,
          State::Failed,
          State::Skipped,
          State::Skipped});
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->line == 11);
      CHECK(analysis.leak->text == "Warning: === LEAK in hull 0 ===");
      CHECK(analysis.leak->entity == "info_player_start");
      CHECK(analysis.leak->position == vm::vec3d{-64, -64, 36});
      CHECK(analysis.leak->pointFile == std::nullopt);
      CHECK(
        messages(analysis.warnings)
        == std::vector<std::string>{
          "Could not locate wad file halflife.wad", "=== LEAK in hull 0 ==="});
      CHECK(
        messages(analysis.errors)
        == std::vector<std::string>{
          "A LEAK is a hole in the map, where the inside of it is exposed to the",
          "Finished with exit code 1"});
      CHECK(analysis.errors[0].line == 13);
    }

    SECTION("sdHLT leak")
    {
      const auto log =
        std::string{R"(#### Executing '/opt/sdhlt/sdHLBSP /maps/compile/test'
SolidBSP [hull 0] 45 (0.00 seconds)
Warning: === LEAK in hull 0 ===
Entity light at ( 656  256  112)
Error:
  A LEAK is a hole in the map, where the inside of it is exposed to the
(unwanted) outside region.  The entity listed in the error is just a helpful
indication of where the beginning of the leak pointfile starts.

Leak pointfile generated

#### Finished with exit code 1

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("bsp")}, true);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->line == 3);
      CHECK(analysis.leak->entity == "light");
      CHECK(analysis.leak->position == vm::vec3d{656, 256, 112});
    }

    SECTION("id qbsp leak")
    {
      const auto log = std::string{R"(#### Executing 'qbsp start.map'
reached occupant at: ( -64, -64,  36)
no filling performed
#### Finished with exit code 0

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("qbsp")}, true);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->entity == std::nullopt);
      CHECK(analysis.leak->position == vm::vec3d{-64, -64, 36});
    }

    SECTION("ericw-tools leak")
    {
      const auto log =
        std::string{R"(#### Executing '/opt/ericw/qbsp /maps/compile/start.map'
---- qbsp / ericw-tools 0.18.1 ----
---- FillOutside ----
WARNING: Reached occupant "info_player_start" at (-64 -64 36), no filling performed.
Leak file written to /maps/compile/start.pts
      44 leaks filled
#### Finished with exit code 0

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("qbsp")}, true);
      CHECK(analysis.tasks[0].state == State::Succeeded);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->line == 4);
      CHECK(analysis.leak->entity == "info_player_start");
      CHECK(analysis.leak->position == vm::vec3d{-64, -64, 36});
      CHECK(analysis.leak->pointFile == "/maps/compile/start.pts");
      CHECK(analysis.warnings.size() == 1);
    }

    SECTION("tyrutils leak")
    {
      const auto log = std::string{R"(#### Executing 'qbsp start.map'
*** WARNING 12: Reached occupant at (-64 -64 36), no filling performed.
Writing start.pts
#### Finished with exit code 0

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("qbsp")}, true);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->entity == std::nullopt);
      CHECK(analysis.leak->position == vm::vec3d{-64, -64, 36});
      CHECK(analysis.leak->pointFile == "start.pts");
      CHECK(
        messages(analysis.warnings)
        == std::vector<std::string>{
          "Reached occupant at (-64 -64 36), no filling performed."});
    }

    SECTION("q3map2 leak")
    {
      const auto log = std::string{
        R"(#### Executing '/opt/q3map2 -game quake3 -meta /maps/compile/q3test.map'
--- ProcessWorldModel ---
--- FloodEntities ---
******* leaked *******
**********************
******* leaked *******
**********************
--- LeakFile ---
Writing /maps/compile/q3test.lin
      23 point linefile
#### Finished with exit code 0

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("q3map2")}, true);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->line == 4);
      CHECK(analysis.leak->text == "******* leaked *******");
      CHECK(analysis.leak->pointFile == "/maps/compile/q3test.lin");
      CHECK(analysis.errors.empty());
    }

    SECTION("Quake 2 leak")
    {
      const auto log = std::string{R"(#### Executing 'qbsp3 base1.map'
**** leaked ****
#### Finished with exit code 0

)"};
      const auto analysis = analyzeCompileLog(log, {runTool("bsp")}, true);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->text == "**** leaked ****");
    }

    SECTION("leak warning with a number")
    {
      const auto analysis = analyzeCompileLog(
        "#### Executing 'qbsp a.map'\nWARNING 13: Map leaks, see a.pts\n",
        {runTool("qbsp")},
        false);
      REQUIRE(analysis.leak);
      CHECK(analysis.leak->line == 2);
      CHECK(
        messages(analysis.warnings) == std::vector<std::string>{"Map leaks, see a.pts"});
    }

    SECTION("error banners")
    {
      const auto log = std::string{R"(#### Executing '/opt/q3map2 -meta /maps/test.map'
************ ERROR ************
MAX_MAP_DRAW_SURFS (65536) exceeded

#### Finished with exit code 1

#### Executing 'hlrad test'
*** ERROR: something bad
fatal error: out of memory
Error 3: numbered error
The level has 0 errors
)"};
      const auto analysis =
        analyzeCompileLog(log, {runTool("q3map2"), runTool("rad")}, true);
      CHECK(
        messages(analysis.errors)
        == std::vector<std::string>{
          "MAX_MAP_DRAW_SURFS (65536) exceeded",
          "Finished with exit code 1",
          "something bad",
          "out of memory",
          "numbered error"});
      CHECK(analysis.errors[0].line == 2);
      CHECK(analysis.errors[2].task == 1);
    }

    SECTION("windows line endings")
    {
      const auto analysis = analyzeCompileLog(
        "#### Executing 'qbsp a.map'\r\nWARNING: odd\r\n#### Finished with exit code "
        "0\r\n\r\n",
        {runTool("qbsp")},
        true);
      CHECK(analysis.tasks[0].state == State::Succeeded);
      CHECK(analysis.tasks[0].exitCode == 0);
      CHECK(messages(analysis.warnings) == std::vector<std::string>{"odd"});
    }
  }

  SECTION("logTail")
  {
    CHECK(logTail("", 5) == std::pair<std::string, bool>{"", false});
    CHECK(logTail("a\nb\nc\n", 3) == std::pair<std::string, bool>{"a\nb\nc\n", false});
    CHECK(logTail("a\nb\nc\n", 2) == std::pair<std::string, bool>{"b\nc\n", true});
    CHECK(logTail("a\nb\nc", 1) == std::pair<std::string, bool>{"c", true});
    CHECK(logTail("a\n\nc\n", 2) == std::pair<std::string, bool>{"\nc\n", true});
    CHECK(logTail("a\nb\n", 0) == std::pair<std::string, bool>{"", true});
  }
}

} // namespace tb::mcp
