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

#include <QtTest/QTest>

#include "CmdTool.h"
#include "base/Preference.h"
#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mdl/BrushNode.h"
#include "mdl/CompilationProfile.h"
#include "mdl/CompilationTask.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestFactory.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"
#include "ui/McpCompileHost.h"

#include "kd/result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::ui
{
using namespace Catch::Matchers;

namespace
{

mdl::CompilationProfile makeProfile(std::vector<mdl::CompilationTask> tasks)
{
  return mdl::CompilationProfile{"Test", "${MAP_DIR_PATH}", std::move(tasks)};
}

mdl::CompilationRunTool runCmdTool(
  std::string parameters, const bool treatNonZeroResultCodeAsError = false)
{
  return mdl::CompilationRunTool{
    true, CMD_TOOL_PATH, std::move(parameters), treatNonZeroResultCodeAsError};
}

bool waitForEnd(const mcp::CompileJob& job)
{
  return QTest::qWaitFor([&]() { return !job.running(); }, 10000);
}

} // namespace

TEST_CASE("McpCompileHost")
{
  auto env = fs::TestEnvironment{};

  auto toolPathPreference = Preference<std::filesystem::path>{
    std::filesystem::path{"Games"} / "McpCompileHostTest" / "Tool Path" / "qbsp", {}};

  auto config = mdl::QuakeFixtureConfig;
  config.gameInfo.gameConfig.compilationTools = {
    mdl::CompilationTool{"qbsp", std::nullopt, toolPathPreference},
  };

  auto fixture = MapDocumentFixture{};
  auto& document = fixture.create(config);
  auto& map = document.map();
  REQUIRE(map.saveAs(env.dir() / "test.map"));

  auto host = McpCompileHost{};

  auto outputCount = 0;
  auto endCount = 0;
  const auto callbacks = mcp::CompileJobCallbacks{
    [&]() { ++outputCount; },
    [&]() { ++endCount; },
  };

  const auto start = [&](const mdl::CompilationProfile& profile, const bool test) {
    return host.startCompile(document, profile, test, callbacks) | kdl::value();
  };

  SECTION("success")
  {
    auto job = start(makeProfile({runCmdTool("--printArgs a b c")}), false);
    REQUIRE(waitForEnd(*job));

    const auto log = job->log();
    CHECK_THAT(log, ContainsSubstring("#### Using working directory '"));
    CHECK_THAT(log, ContainsSubstring("#### Executing '"));
    CHECK_THAT(log, ContainsSubstring("a\nb\nc\n"));
    CHECK_THAT(log, ContainsSubstring("#### Finished with exit code 0"));
    CHECK(!job->running());
    CHECK(endCount == 1);
    CHECK(outputCount > 0);
  }

  SECTION("failure skips the remaining tasks")
  {
    auto job = start(
      makeProfile({runCmdTool("--exit 3", true), runCmdTool("--printArgs second")}),
      false);
    REQUIRE(waitForEnd(*job));

    const auto log = job->log();
    CHECK_THAT(log, ContainsSubstring("#### Finished with exit code 3"));
    CHECK_THAT(log, !ContainsSubstring("--printArgs"));
    CHECK(endCount == 1);
  }

  SECTION("crash")
  {
    auto job = start(makeProfile({runCmdTool("--crash")}), false);
    REQUIRE(waitForEnd(*job));

    CHECK_THAT(job->log(), ContainsSubstring("#### Crashed with exit code"));
    CHECK(endCount == 1);
  }

  SECTION("cancel")
  {
    auto job = start(
      makeProfile({runCmdTool("--printArgs first"), runCmdTool("--printArgs second")}),
      false);
    REQUIRE(job->running());

    job->cancel();
    CHECK(!job->running());
    CHECK(endCount == 1);
    CHECK_THAT(job->log(), ContainsSubstring("\n\n#### Terminated\n"));

    QTest::qWait(200);
    CHECK(endCount == 1);
    CHECK_THAT(job->log(), !ContainsSubstring("second"));

    // cancelling a job that has ended does nothing
    job->cancel();
    CHECK(endCount == 1);
  }

  SECTION("test mode")
  {
    auto job = start(
      makeProfile({
        mdl::CompilationExportMap{true, false, std::nullopt, std::nullopt, "out.map"},
        runCmdTool("--printArgs a"),
      }),
      true);

    // a test run ends synchronously
    CHECK(!job->running());
    CHECK(endCount == 1);

    const auto log = job->log();
    CHECK_THAT(log, ContainsSubstring("#### Exporting map file '"));
    CHECK_THAT(log, ContainsSubstring("#### Executing '"));
    CHECK_THAT(log, !ContainsSubstring("#### Finished"));
    CHECK(!env.fileExists("out.map"));
  }

  SECTION("export writes unsaved changes")
  {
    auto* brushNode = mdl::createBrushNode(map);
    auto* entityNode =
      new mdl::EntityNode{mdl::Entity{{{"classname", "info_player_start"}}}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode, entityNode}}});
    REQUIRE(map.modified());

    auto job = start(
      makeProfile({
        mdl::CompilationExportMap{
          true, false, std::nullopt, std::nullopt, "export/${MAP_BASE_NAME}.map"},
      }),
      false);
    REQUIRE(waitForEnd(*job));
    CHECK(endCount == 1);

    REQUIRE(env.fileExists("export/test.map"));
    const auto exported = env.loadFile("export/test.map");
    CHECK_THAT(exported, ContainsSubstring("info_player_start"));
    CHECK_THAT(exported, ContainsSubstring("// brush 0"));
    CHECK(map.modified());
  }

  SECTION("compilation tool variables")
  {
    setPref(toolPathPreference, std::filesystem::path{CMD_TOOL_PATH});

    auto job = start(
      makeProfile({
        mdl::CompilationRunTool{true, "${qbsp}", "--printArgs viaVariable", true},
      }),
      false);
    REQUIRE(waitForEnd(*job));

    const auto log = job->log();
    CHECK_THAT(log, ContainsSubstring("viaVariable\n"));
    CHECK_THAT(log, ContainsSubstring("#### Finished with exit code 0"));
  }

  SECTION("copy files")
  {
    env.createFile("test.bsp", "bsp");

    auto job = start(
      makeProfile({
        mdl::CompilationCopyFiles{true, "${MAP_BASE_NAME}.bsp", "target"},
      }),
      false);
    REQUIRE(waitForEnd(*job));

    CHECK_THAT(job->log(), ContainsSubstring("#### Copying to '"));
    CHECK(env.fileExists("target/test.bsp"));
    CHECK(env.fileExists("test.bsp"));
  }

  SECTION("export, run tool and copy files")
  {
    env.createFile("test.bsp", "bsp");

    auto job = start(
      makeProfile({
        mdl::CompilationExportMap{
          true, false, std::nullopt, std::nullopt, "${WORK_DIR_PATH}/export.map"},
        runCmdTool("--printArgs ${MAP_BASE_NAME}"),
        mdl::CompilationCopyFiles{
          true, "${WORK_DIR_PATH}/${MAP_BASE_NAME}.bsp", "${WORK_DIR_PATH}/target"},
      }),
      false);
    REQUIRE(waitForEnd(*job));

    const auto log = job->log();
    CHECK_THAT(log, StartsWith("#### Using working directory '"));
    CHECK_THAT(log, ContainsSubstring("#### Exporting map file '"));
    CHECK_THAT(log, ContainsSubstring("\ntest\n"));
    CHECK_THAT(log, ContainsSubstring("#### Copying to '"));
    CHECK(env.fileExists("export.map"));
    CHECK(env.fileExists("target/test.bsp"));
    CHECK(endCount == 1);
  }

  SECTION("reloading the document cancels the job")
  {
    auto job = start(makeProfile({runCmdTool("--printArgs a")}), false);
    REQUIRE(job->running());

    REQUIRE(document.reload());
    CHECK(!job->running());
    CHECK(endCount == 1);

    const auto log = job->log();
    CHECK_THAT(log, ContainsSubstring("\n\n#### Terminated\n"));
    CHECK_THAT(log, EndsWith("#### Terminated: the document was reloaded\n"));

    QTest::qWait(200);
    CHECK(endCount == 1);
  }

  SECTION("destroying a running job does not call the callbacks")
  {
    auto job = start(makeProfile({runCmdTool("--printArgs a")}), false);
    REQUIRE(job->running());

    const auto outputCountBefore = outputCount;
    job.reset();

    QTest::qWait(200);
    CHECK(endCount == 0);
    CHECK(outputCount == outputCountBefore);
  }
}

} // namespace tb::ui
