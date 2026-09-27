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
#include "fs/TestEnvironment.h"
#include "mcp/Json.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Map.h"
#include "ui/CatchConfig.h"
#include "ui/MapDocument.h"
#include "ui/McpCompileHost.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::ui
{
using namespace Catch::Matchers;
using mcp::Json;

namespace
{

std::string readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path};
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

/** Polls compile_status until the run has ended and returns the final status. */
Json waitForRun(mcp::McpToolFixture& fixture, const std::string& run)
{
  auto status = Json{};
  const auto ended = QTest::qWaitFor(
    [&]() {
      status = fixture.call("compile_status", Json{{"run", run}, {"log", "full"}});
      return status["state"] != "running";
    },
    10000);
  CHECK(ended);
  return status;
}

Json customProfile(const std::string& name, const std::string& parameters)
{
  return Json{
    {"name", name},
    {"tasks",
     Json::array({
       Json{
         {"type", "exportMap"},
         {"target", "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.map"},
       },
       Json{
         {"type", "runTool"},
         {"tool", "${qbsp}"},
         {"parameters", parameters},
       },
       Json{
         {"type", "copyFiles"},
         {"source", "${WORK_DIR_PATH}/compile/${MAP_BASE_NAME}.bsp"},
         {"target", "${GAME_DIR_PATH}/${MODS[-1]}/maps"},
       },
     })},
  };
}

} // namespace

TEST_CASE("McpCompile")
{
  // The compile tools of the MCP server running on the editor's compilation runner, with
  // the CmdTool stub standing in for the compile tools.
  auto env = fs::TestEnvironment{};
  env.createDirectory("maps/compile");
  env.createDirectory("game/id1");

  auto fixture = mcp::McpToolFixture{};
  auto compileHost = McpCompileHost{};
  fixture.host().compileHostOverride = &compileHost;

  const auto previousGamePath = fixture.call(
    "game_set_path",
    Json{
      {"game", "Quake"},
      {"path", (env.dir() / "game").string()}})["result"]["previousPath"];
  fixture.call(
    "compile_tools_set",
    Json{
      {"game", "Quake"},
      {"tools",
       Json{{"qbsp", CMD_TOOL_PATH}, {"vis", CMD_TOOL_PATH}, {"light", CMD_TOOL_PATH}}},
    });

  fixture.call("document_new", Json{{"game", "Quake"}});
  fixture.call(
    "document_save_as", Json{{"path", (env.dir() / "maps/test.map").string()}});
  auto& document = *fixture.host().documentList.back().document;

  const auto mapPath = env.dir() / "maps" / "test.map";
  const auto compileDir = env.dir() / "maps" / "compile";

  SECTION("a preset in test mode resolves the tool variables")
  {
    const auto started =
      fixture.call("compile_run", Json{{"preset", "normal"}, {"test", true}});
    const auto status = waitForRun(fixture, started["result"]["run"].get<std::string>());

    CHECK(status["state"] == "succeeded");
    CHECK(status["test"] == true);
    const auto log = status["log"]["text"].get<std::string>();
    CHECK_THAT(
      log,
      ContainsSubstring(
        std::string{"#### Executing '"} + CMD_TOOL_PATH + " "
        + (compileDir / "test.map").string() + "'"));
    CHECK_THAT(
      log,
      ContainsSubstring(
        std::string{"#### Executing '"} + CMD_TOOL_PATH + " -extra "
        + (compileDir / "test.bsp").string() + "'"));
    CHECK_THAT(log, ContainsSubstring((env.dir() / "game/id1/maps").string()));
    // a test run writes nothing
    CHECK(!std::filesystem::exists(compileDir / "test.map"));
  }

  SECTION("success compiles unsaved changes and reports the output paths")
  {
    fixture.call("brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {64, 64, 64}}});
    REQUIRE(document.map().modified());

    // CmdTool cannot write a bsp file, so provide one
    env.createFile("maps/compile/test.bsp", "bsp");
    fixture.call(
      "compile_profile_save",
      Json{
        {"game", "Quake"},
        {"profile", customProfile("MCP Test", "--printArgs \"WARNING: odd texture\"")},
      });

    const auto started = fixture.call("compile_run", Json{{"profile", "MCP Test"}});
    CHECK(started["result"]["state"] == "running");
    const auto status = waitForRun(fixture, started["result"]["run"].get<std::string>());

    CHECK(status["state"] == "succeeded");
    CHECK(status["progress"] == Json{{"completedTasks", 3}, {"totalTasks", 3}});
    CHECK(status["exitCodes"] == Json::array({0}));
    CHECK(status["warningCount"] == 1);
    CHECK(status["errorCount"] == 0);
    CHECK(status["leak"]["detected"] == false);
    CHECK(status["output"]["compiledFile"] == (compileDir / "test.bsp").string());
    CHECK(status["output"]["compiledFileExists"] == true);
    CHECK(
      status["output"]["copiedTo"]
      == Json::array({(env.dir() / "game/id1/maps/test.bsp").string()}));
    CHECK(std::filesystem::exists(env.dir() / "game/id1/maps/test.bsp"));

    // the exported map contains the unsaved brush, the map file itself is unchanged
    CHECK_THAT(readFile(compileDir / "test.map"), ContainsSubstring("// brush 1"));
    CHECK_THAT(readFile(mapPath), !ContainsSubstring("// brush 1"));

    const auto resource =
      fixture.rpc("resources/read", Json{{"uri", status["log"]["uri"]}});
    CHECK_THAT(
      resource["result"]["contents"][0]["text"].get<std::string>(),
      ContainsSubstring("WARNING: odd texture"));
  }

  SECTION("failure")
  {
    fixture.call(
      "compile_profile_save",
      Json{{"game", "Quake"}, {"profile", customProfile("MCP Fail", "--exit 3")}});

    const auto started = fixture.call("compile_run", Json{{"profile", "MCP Fail"}});
    const auto status = waitForRun(fixture, started["result"]["run"].get<std::string>());

    CHECK(status["state"] == "failed");
    CHECK(status["tasks"][1]["state"] == "failed");
    CHECK(status["tasks"][1]["exitCode"] == 3);
    CHECK(status["tasks"][2]["state"] == "skipped");
    CHECK(status["output"]["copiedTo"] == Json::array());
  }

  SECTION("cancel")
  {
    fixture.call(
      "compile_profile_save",
      Json{{"game", "Quake"}, {"profile", customProfile("MCP Cancel", "--printArgs x")}});

    const auto started = fixture.call("compile_run", Json{{"profile", "MCP Cancel"}});
    const auto run = started["result"]["run"].get<std::string>();
    fixture.call("compile_cancel", Json{{"run", run}});

    const auto status = waitForRun(fixture, run);
    CHECK(status["state"] == "cancelled");
    CHECK_THAT(
      status["log"]["text"].get<std::string>(), ContainsSubstring("#### Terminated"));
  }

  SECTION("a leak names the point file to load")
  {
    env.createFile("maps/compile/test.pts", "0 0 0\n0 0 64\n0 0 128\n");
    fixture.call(
      "compile_profile_save",
      Json{
        {"game", "Quake"},
        {"profile",
         customProfile(
           "MCP Leak",
           "--printArgs \"Warning: === LEAK in hull 0 ===\" \"Entity info_player_start @ "
           "( -64, -64, 36)\"")},
      });

    const auto started = fixture.call("compile_run", Json{{"profile", "MCP Leak"}});
    const auto status = waitForRun(fixture, started["result"]["run"].get<std::string>());

    const auto& leak = status["leak"];
    CHECK(leak["detected"] == true);
    CHECK(leak["entity"] == "info_player_start");
    CHECK(leak["position"] == Json::array({-64.0, -64.0, 36.0}));
    CHECK(leak["pointFile"] == (compileDir / "test.pts").string());
    CHECK(leak["pointFileExists"] == true);

    const auto loaded = fixture.call("pointfile_load");
    CHECK(loaded["result"]["path"] == (compileDir / "test.pts").string());
    CHECK(loaded["result"]["pointCount"] == 3);
    CHECK(document.isPointFileLoaded());
  }

  fixture.call(
    "compile_tools_set",
    Json{{"game", "Quake"}, {"tools", Json{{"qbsp", ""}, {"vis", ""}, {"light", ""}}}});
  fixture.call("game_set_path", Json{{"game", "Quake"}, {"path", previousGamePath}});
}

} // namespace tb::ui
