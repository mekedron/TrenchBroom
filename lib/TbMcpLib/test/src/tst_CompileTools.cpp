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

#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mcp/CompileRuns.h"
#include "mcp/FakeHost.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mdl/CompilationTask.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include "kd/invoke.h"
#include "kd/overload.h"

#include "vm/vec.h"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

void resetToolPaths(FakeHost& host)
{
  auto& gameManager = host.gameManager();
  auto names = std::vector<std::string>{};
  for (const auto& gameInfo : gameManager.gameInfos())
  {
    names.push_back(gameInfo.gameConfig.name);
  }
  for (const auto& name : names)
  {
    for (auto& tool : gameManager.gameInfo(name)->gameConfig.compilationTools)
    {
      setPref(tool.pathPreference, std::filesystem::path{});
    }
  }
}

std::filesystem::path createExecutable(
  fs::TestEnvironment& env, const std::filesystem::path& path)
{
  env.createDirectory(path.parent_path());
#ifdef _WIN32
  // Windows decides by the extension whether a file is executable
  const auto scriptPath = std::filesystem::path{path}.replace_extension(".bat");
  env.createFile(scriptPath, "@exit 0\r\n");
#else
  const auto scriptPath = path;
  env.createFile(scriptPath, "#!/bin/sh\nexit 0\n");
#endif
  const auto absolute = env.dir() / scriptPath;
  std::filesystem::permissions(
    absolute,
    std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec
      | std::filesystem::perms::others_exec,
    std::filesystem::perm_options::add);
  return absolute;
}

/** Creates a document of the given game and saves it as <env>/maps/test.map. */
ui::MapDocument& createSavedDocument(
  McpToolFixture& fixture, fs::TestEnvironment& env, const std::string& game = "Quake")
{
  fixture.call("document_new", Json{{"game", game}});
  env.createDirectory("maps/compile");
  fixture.call(
    "document_save_as", Json{{"path", (env.dir() / "maps/test.map").string()}});
  return *fixture.host().documentList.back().document;
}

/** Sets every compile tool of the game to an executable. */
void configureTools(
  McpToolFixture& fixture, fs::TestEnvironment& env, const std::string& game)
{
  auto tools = Json::object();
  for (const auto& tool :
       fixture.host().gameManager().gameInfo(game)->gameConfig.compilationTools)
  {
    tools[tool.name] = createExecutable(env, "tools/" + tool.name).string();
  }
  fixture.call("compile_tools_set", Json{{"game", game}, {"tools", tools}});
}

/**
 * The log of the editor's runner for the given profile, run on <env>/maps/test.map.
 * The first tool writes `toolOutput` and exits with `exitCode`; a failing tool ends the
 * run.
 */
std::string runnerLog(
  const mdl::CompilationProfile& profile,
  const fs::TestEnvironment& env,
  const std::string& toolOutput = "",
  const int exitCode = 0)
{
  const auto compileDir = env.dir() / "maps" / "compile";
  const auto mapsDir = env.dir() / "game" / "id1" / "maps";

  auto log = std::string{};
  auto firstTool = true;
  for (const auto& task : profile.tasks)
  {
    const auto stop = std::visit(
      kdl::overload(
        [&](const mdl::CompilationExportMap&) {
          log += "#### Exporting map file '" + (compileDir / "test.map").string() + "'\n";
          return false;
        },
        [&](const mdl::CompilationRunTool& runTool) {
          log += "#### Executing '/tools/" + runTool.toolSpec + " test'\n";
          const auto code = firstTool ? exitCode : 0;
          if (firstTool)
          {
            log += toolOutput;
            firstTool = false;
          }
          log += "#### Finished with exit code " + std::to_string(code) + "\n\n";
          return code != 0;
        },
        [&](const mdl::CompilationCopyFiles& copy) {
          const auto extension = std::filesystem::path{copy.sourceSpec}.extension();
          log += "#### Copying to '" + mapsDir.string()
                 + "/': " + (compileDir / "test").string() + extension.string() + "\n";
          return false;
        },
        [](const auto&) { return false; }),
      task);
    if (stop)
    {
      break;
    }
  }
  return log;
}

std::vector<std::string> updatedUris(const CapturingNotificationStream& stream)
{
  auto result = std::vector<std::string>{};
  for (const auto& notification : stream.notifications)
  {
    if (notification["method"] == "notifications/resources/updated")
    {
      result.push_back(notification["params"]["uri"].get<std::string>());
    }
  }
  return result;
}

std::vector<std::string> warningCodes(const Json& structured)
{
  auto result = std::vector<std::string>{};
  if (structured.contains("warnings"))
  {
    for (const auto& warning : structured["warnings"])
    {
      result.push_back(warning["code"].get<std::string>());
    }
  }
  return result;
}

bool contains(const std::vector<std::string>& strings, const std::string& str)
{
  return std::ranges::find(strings, str) != strings.end();
}

} // namespace

TEST_CASE("CompileTools")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};
  auto& compile = fixture.host().compile;

  resetToolPaths(fixture.host());
  auto resetLater = kdl::invoke_later{[&]() { resetToolPaths(fixture.host()); }};

  SECTION("compile_tools_get")
  {
    const auto quake = fixture.call("compile_tools_get", Json{{"game", "Quake"}});
    CHECK(quake["game"] == "Quake");
    CHECK(quake["family"] == "quake");
    REQUIRE(quake["tools"].size() == 3);
    CHECK(quake["tools"][0]["name"] == "qbsp");
    CHECK(quake["tools"][0]["variable"] == "${qbsp}");
    CHECK(quake["tools"][0]["status"] == "notSet");
    CHECK(quake["allConfigured"] == false);

    const auto halfLife = fixture.call("compile_tools_get", Json{{"game", "half-life"}});
    CHECK(halfLife["game"] == "Half-Life");
    CHECK(halfLife["family"] == "halflife");
    CHECK(halfLife["tools"].size() == 4);

    CHECK(fixture.callExpectingError("compile_tools_get").code == ErrorCode::NoDocument);
    CHECK(
      fixture.callExpectingError("compile_tools_get", Json{{"game", "Doom"}}).code
      == ErrorCode::InvalidArgument);

    // defaults to the document's game
    fixture.call("document_new", Json{{"game", "Quake 3"}});
    const auto quake3 = fixture.call("compile_tools_get");
    CHECK(quake3["game"] == "Quake 3");
    CHECK(quake3["family"] == "quake3");
  }

  SECTION("compile_tools_set")
  {
    const auto qbsp = createExecutable(env, "tools/qbsp");

    SECTION("sets and clears paths")
    {
      const auto structured = fixture.call(
        "compile_tools_set",
        Json{{"game", "Quake"}, {"tools", {{"qbsp", qbsp.string()}}}});
      const auto& result = structured["result"];
      CHECK(result["changed"] == Json::array({"qbsp"}));
      CHECK(result["tools"][0]["path"] == qbsp.string());
      CHECK(result["tools"][0]["status"] == "ok");
      CHECK(result["allConfigured"] == false);
      CHECK(warningCodes(structured).empty());

      configureTools(fixture, env, "Quake");
      CHECK(
        fixture.call("compile_tools_get", Json{{"game", "Quake"}})["allConfigured"]
        == true);

      const auto cleared = fixture.call(
        "compile_tools_set", Json{{"game", "Quake"}, {"tools", {{"qbsp", ""}}}});
      CHECK(cleared["result"]["tools"][0]["status"] == "notSet");
    }

    SECTION("warns about unusable paths but sets them")
    {
      env.createFile("tools/notexec", "x");
      env.createDirectory("tools/dir");
      const auto structured = fixture.call(
        "compile_tools_set",
        Json{
          {"game", "Quake"},
          {"tools",
           {{"qbsp", (env.dir() / "tools/missing").string()},
            {"vis", (env.dir() / "tools/notexec").string()},
            {"light", (env.dir() / "tools/dir").string()}}}});
      const auto codes = warningCodes(structured);
      CHECK(contains(codes, "TOOL_NOT_FOUND"));
      CHECK(contains(codes, "TOOL_NOT_EXECUTABLE"));
      CHECK(contains(codes, "TOOL_NOT_A_FILE"));
      CHECK(structured["result"]["tools"][0]["status"] == "notFound");
      CHECK(structured["result"]["tools"][1]["status"] == "notExecutable");
      CHECK(structured["result"]["tools"][2]["status"] == "notAFile");
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "compile_tools_set",
            Json{{"game", "Quake"}, {"tools", {{"csg", qbsp.string()}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "compile_tools_set",
            Json{{"game", "Quake"}, {"tools", {{"qbsp", "tools/qbsp"}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "compile_tools_set", Json{{"game", "Quake"}, {"tools", {{"qbsp", 1}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.call("compile_tools_get", Json{{"game", "Quake"}})["tools"][0]["status"]
        == "notSet");
    }

    SECTION("dry run")
    {
      const auto structured = fixture.call(
        "compile_tools_set",
        Json{{"game", "Quake"}, {"tools", {{"qbsp", qbsp.string()}}}, {"dryRun", true}});
      CHECK(structured["result"]["wouldDo"].is_string());
      CHECK(structured["result"]["changed"] == Json::array({"qbsp"}));
      CHECK(
        fixture.call("compile_tools_get", Json{{"game", "Quake"}})["tools"][0]["status"]
        == "notSet");
    }
  }

  SECTION("compile_presets_list")
  {
    const auto quake = fixture.call("compile_presets_list", Json{{"game", "Quake"}});
    CHECK(quake["family"] == "quake");
    REQUIRE(quake["presets"].size() == 3);
    CHECK(quake["presets"][0]["name"] == "fast");
    CHECK(quake["presets"][1]["name"] == "normal");
    CHECK(quake["presets"][2]["name"] == "full");
    CHECK(quake["presets"][1]["missingTools"] == quake["presets"][1]["tools"]);
    CHECK(quake["presets"][1]["profile"]["tasks"].size() > 2);
    CHECK(quake["summary"].is_string());
    CHECK(warningCodes(quake).empty());

    configureTools(fixture, env, "Quake");
    CHECK(
      fixture.call(
        "compile_presets_list", Json{{"game", "Quake"}})["presets"][1]["missingTools"]
      == Json::array());

    const auto halfLife =
      fixture.call("compile_presets_list", Json{{"game", "Half-Life"}});
    CHECK(halfLife["family"] == "halflife");
    CHECK(halfLife["presets"].size() == 3);

    const auto noPresets = fixture.call("compile_presets_list", Json{{"game", "Test"}});
    CHECK(noPresets["presets"] == Json::array());
    CHECK(noPresets["family"].is_null());
    CHECK(warningCodes(noPresets) == std::vector<std::string>{"NO_PRESETS"});
  }

  SECTION("compile_profiles_list")
  {
    CHECK(
      fixture.call("compile_profiles_list", Json{{"game", "Quake"}})["profiles"]
      == Json::array());

    fixture.call(
      "compile_profile_save",
      Json{{"game", "Quake"}, {"preset", "normal"}, {"name", "Mine"}});
    const auto result = fixture.call("compile_profiles_list", Json{{"game", "Quake"}});
    CHECK(result["game"] == "Quake");
    REQUIRE(result["profiles"].size() == 1);
    CHECK(result["profiles"][0]["name"] == "Mine");
    CHECK(
      result["profiles"][0]["enabledTaskCount"] == result["profiles"][0]["tasks"].size());
  }

  SECTION("compile_profile_save")
  {
    SECTION("saves a preset")
    {
      const auto result = fixture.call(
        "compile_profile_save",
        Json{{"game", "Quake"}, {"preset", "fast"}, {"name", "Quick"}})["result"];
      CHECK(result["created"] == true);
      CHECK(result["profile"]["name"] == "Quick");
      CHECK(
        fixture.host().gameManager().gameInfo("Quake")->compilationConfig.profiles.size()
        == 1);

      CHECK(
        fixture
          .callExpectingError(
            "compile_profile_save",
            Json{{"game", "Quake"}, {"preset", "full"}, {"name", "Quick"}})
          .code
        == ErrorCode::FileExists);

      const auto replaced = fixture.call(
        "compile_profile_save",
        Json{
          {"game", "Quake"},
          {"preset", "full"},
          {"name", "Quick"},
          {"overwrite", true}})["result"];
      CHECK(replaced["created"] == false);
      CHECK(
        fixture.host().gameManager().gameInfo("Quake")->compilationConfig.profiles.size()
        == 1);
    }

    SECTION("saves a custom profile")
    {
      const auto result = fixture.call(
        "compile_profile_save",
        Json{
          {"game", "Quake"},
          {"profile",
           {{"name", "BSP only"},
            {"tasks",
             Json::array(
               {{{"type", "exportMap"}, {"target", "${WORK_DIR_PATH}/x.map"}},
                {{"type", "runTool"},
                 {"tool", "${qbsp}"},
                 {"parameters", "x.map"}}})}}}})["result"];
      CHECK(result["profile"]["name"] == "BSP only");
      CHECK(result["profile"]["tasks"].size() == 2);
      const auto& profiles =
        fixture.host().gameManager().gameInfo("Quake")->compilationConfig.profiles;
      REQUIRE(profiles.size() == 1);
      CHECK(profiles[0].tasks.size() == 2);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "compile_profile_save",
            Json{
              {"game", "Quake"},
              {"preset", "fast"},
              {"profile", {{"name", "x"}, {"tasks", Json::array()}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("compile_profile_save", Json{{"game", "Quake"}}).code
        == ErrorCode::InvalidArgument);
      const auto error = fixture.callExpectingError(
        "compile_profile_save", Json{{"game", "Quake"}, {"preset", "ultra"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("normal") != std::string::npos);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "compile_profile_save",
        Json{{"game", "Quake"}, {"preset", "fast"}, {"dryRun", true}})["result"];
      CHECK(result["wouldDo"].is_string());
      CHECK(result["created"] == true);
      CHECK(fixture.host()
              .gameManager()
              .gameInfo("Quake")
              ->compilationConfig.profiles.empty());
    }
  }

  SECTION("compile_profile_delete")
  {
    fixture.call(
      "compile_profile_save", Json{{"game", "Quake"}, {"preset", "fast"}, {"name", "A"}});
    fixture.call(
      "compile_profile_save", Json{{"game", "Quake"}, {"preset", "full"}, {"name", "B"}});
    const auto& profiles =
      fixture.host().gameManager().gameInfo("Quake")->compilationConfig.profiles;

    CHECK(
      fixture
        .callExpectingError(
          "compile_profile_delete", Json{{"game", "Quake"}, {"name", "C"}})
        .code
      == ErrorCode::InvalidArgument);

    fixture.call(
      "compile_profile_delete", Json{{"game", "Quake"}, {"name", "A"}, {"dryRun", true}});
    CHECK(profiles.size() == 2);

    const auto result = fixture.call(
      "compile_profile_delete", Json{{"game", "Quake"}, {"name", "A"}})["result"];
    CHECK(result["deleted"] == "A");
    CHECK(result["remaining"] == Json::array({"B"}));
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "B");
  }

  SECTION("compile_run")
  {
    SECTION("host without compile support")
    {
      createSavedDocument(fixture, env);
      fixture.host().supportsCompile = false;
      CHECK(
        fixture.callExpectingError("compile_run", Json{{"preset", "normal"}}).code
        == ErrorCode::UnsupportedInHost);
    }

    SECTION("never saved document")
    {
      fixture.call("document_new", Json{{"game", "Quake"}});
      const auto error =
        fixture.callExpectingError("compile_run", Json{{"preset", "normal"}});
      CHECK(error.code == ErrorCode::UnsavedChanges);
      CHECK(error.hint.find("document_save_as") != std::string::npos);
    }

    SECTION("invalid input")
    {
      createSavedDocument(fixture, env);
      CHECK(fixture.callExpectingError("compile_run").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("compile_run", Json{{"preset", "normal"}, {"profile", "x"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("compile_run", Json{{"preset", "ultra"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("compile_run", Json{{"profile", "nope"}}).code
        == ErrorCode::InvalidArgument);

      // a profile without enabled tasks
      fixture.call(
        "compile_profile_save",
        Json{
          {"profile",
           {{"name", "Disabled"},
            {"tasks",
             Json::array(
               {{{"type", "exportMap"}, {"target", "x.map"}, {"enabled", false}}})}}}});
      CHECK(
        fixture.callExpectingError("compile_run", Json{{"profile", "Disabled"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(compile.started.empty());
    }

    SECTION("missing tools")
    {
      createSavedDocument(fixture, env);
      const auto error =
        fixture.callExpectingError("compile_run", Json{{"preset", "normal"}});
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("qbsp") != std::string::npos);
      CHECK(error.hint.find("compile_tools_set") != std::string::npos);
      CHECK(compile.started.empty());
    }

    SECTION("dry run")
    {
      createSavedDocument(fixture, env);
      configureTools(fixture, env, "Quake");
      const auto result = fixture.call(
        "compile_run", Json{{"preset", "normal"}, {"dryRun", true}})["result"];
      CHECK(result["wouldDo"].is_string());
      CHECK(result["tasks"].size() > 2);
      CHECK(result["tasks"][0]["type"] == "exportMap");
      CHECK(compile.started.empty());
    }

    SECTION("runs a preset in the background")
    {
      auto& document = createSavedDocument(fixture, env);
      configureTools(fixture, env, "Quake");

      const auto structured = fixture.call("compile_run", Json{{"preset", "normal"}});
      CHECK(warningCodes(structured).empty());
      const auto& result = structured["result"];
      CHECK(result["run"] == "run:1");
      CHECK(result["state"] == "running");
      CHECK(result["preset"] == "normal");
      CHECK(result["game"] == "Quake");
      CHECK(result["test"] == false);
      CHECK(result["endedAt"].is_null());
      CHECK(result["log"]["uri"] == "trenchbroom://compile/run:1/log");

      REQUIRE(compile.started.size() == 1);
      CHECK(compile.started[0].document == &document);
      CHECK(compile.started[0].test == false);
      CHECK(fixture.call("editor_status")["compileRunning"] == true);

      SECTION("one compile at a time")
      {
        const auto error =
          fixture.callExpectingError("compile_run", Json{{"preset", "fast"}});
        CHECK(error.code == ErrorCode::CompileRunning);
        CHECK(error.message.find("run:1") != std::string::npos);
        CHECK(compile.started.size() == 1);

        CHECK(
          fixture.callExpectingError("document_close").code == ErrorCode::CompileRunning);
        CHECK(
          fixture.callExpectingError("document_revert").code
          == ErrorCode::CompileRunning);
      }

      SECTION("succeeds and reports the output")
      {
        env.createFile("maps/compile/test.bsp", "bsp");
        auto* job = compile.lastJob();
        job->append(
          runnerLog(compile.started[0].profile, env, "WARNING: 1 texture missing\n"));
        job->finish();

        const auto status = fixture.call("compile_status");
        CHECK(status["run"] == "run:1");
        CHECK(status["state"] == "succeeded");
        CHECK(status["endedAt"].is_string());
        CHECK(status["progress"]["completedTasks"] == status["progress"]["totalTasks"]);
        CHECK(status["currentTask"].is_null());
        CHECK(status["exitCodes"] == Json::array({0, 0, 0}));
        CHECK(status["errorCount"] == 0);
        CHECK(status["warningCount"] == 1);
        CHECK(status["leak"] == Json{{"detected", false}});
        CHECK(
          status["output"]["compiledFile"]
          == (env.dir() / "maps/compile/test.bsp").string());
        CHECK(status["output"]["compiledFileExists"] == true);
        CHECK(
          status["output"]["copiedTo"]
          == Json::array({(env.dir() / "game/id1/maps/test.bsp").string()}));
        CHECK(fixture.call("editor_status")["compileRunning"] == false);

        // another run is possible now
        CHECK(
          fixture.call("compile_run", Json{{"preset", "fast"}})["result"]["run"]
          == "run:2");
      }

      SECTION("fails")
      {
        auto* job = compile.lastJob();
        job->append(
          runnerLog(compile.started[0].profile, env, "Error: brush 3 is invalid\n", 1));
        job->finish();

        const auto status = fixture.call("compile_status", Json{{"run", "run:1"}});
        CHECK(status["state"] == "failed");
        CHECK(status["exitCodes"] == Json::array({1}));
        // the tool's error and the runner's report of the exit code
        CHECK(status["errorCount"] == 2);
        CHECK(status["errors"][0]["message"] == "brush 3 is invalid");
        CHECK(status["tasks"][1]["state"] == "failed");
        CHECK(status["output"]["compiledFile"].is_null());
      }
    }

    SECTION("test mode")
    {
      createSavedDocument(fixture, env);
      compile.onStart = [&](FakeCompileJob& job, const auto& started) {
        job.append(runnerLog(started.profile, env));
        job.finish();
      };

      // the tools need not be set up
      const auto result =
        fixture.call("compile_run", Json{{"preset", "normal"}, {"test", true}})["result"];
      CHECK(result["test"] == true);
      CHECK(result["state"] == "succeeded");
      CHECK(result["endedAt"].is_string());
      CHECK(compile.started[0].test == true);
      CHECK(fixture.call("editor_status")["compileRunning"] == false);
    }

    SECTION("runs a saved profile")
    {
      createSavedDocument(fixture, env);
      configureTools(fixture, env, "Quake");
      fixture.call("compile_profile_save", Json{{"preset", "fast"}, {"name", "Quick"}});
      const auto result =
        fixture.call("compile_run", Json{{"profile", "Quick"}})["result"];
      CHECK(result["profile"] == "Quick");
      CHECK(result["preset"].is_null());
      REQUIRE(compile.started.size() == 1);
      CHECK(compile.started[0].profile.name == "Quick");
    }

    SECTION("the editor is compiling")
    {
      createSavedDocument(fixture, env);
      fixture.host().compileRunning = true;
      CHECK(
        fixture
          .callExpectingError("compile_run", Json{{"preset", "normal"}, {"test", true}})
          .code
        == ErrorCode::CompileRunning);
    }

    SECTION("start failure")
    {
      createSavedDocument(fixture, env);
      compile.startError = "no work dir";
      const auto error = fixture.callExpectingError(
        "compile_run", Json{{"preset", "normal"}, {"test", true}});
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("no work dir") != std::string::npos);
    }

    SECTION("Half-Life")
    {
      createSavedDocument(fixture, env, "Half-Life");
      configureTools(fixture, env, "Half-Life");
      const auto result =
        fixture.call("compile_run", Json{{"preset", "normal"}})["result"];
      CHECK(result["game"] == "Half-Life");
      REQUIRE(compile.started.size() == 1);
      CHECK(compile.started[0].profile.tasks.size() > 4);
    }

    SECTION("relative WAD paths")
    {
      createSavedDocument(fixture, env, "Half-Life");
      configureTools(fixture, env, "Half-Life");
      const auto clean =
        fixture.call("compile_run", Json{{"preset", "normal"}, {"dryRun", true}});
      CHECK(!contains(warningCodes(clean), "RELATIVE_WAD_PATH"));

      // not found, so stored as passed
      fixture.call(
        "materials_collections_set", Json{{"wads", Json{"valve/halflife.wad"}}});
      const auto result =
        fixture.call("compile_run", Json{{"preset", "normal"}, {"dryRun", true}});
      REQUIRE(contains(warningCodes(result), "RELATIVE_WAD_PATH"));
      CHECK(dumpJson(result["warnings"]).find("valve/halflife.wad") != std::string::npos);
    }
  }

  SECTION("compile_status")
  {
    CHECK(fixture.callExpectingError("compile_status").code == ErrorCode::ObjectNotFound);

    createSavedDocument(fixture, env);
    configureTools(fixture, env, "Quake");
    fixture.call("compile_run", Json{{"preset", "normal"}});
    CHECK(
      fixture.callExpectingError("compile_status", Json{{"run", "run:9"}}).code
      == ErrorCode::ObjectNotFound);

    auto* job = compile.lastJob();
    job->append("line 1\nline 2\nline 3\n");

    SECTION("log modes")
    {
      const auto tail = fixture.call("compile_status", Json{{"tailLines", 2}});
      CHECK(tail["state"] == "running");
      CHECK(tail["log"]["lineCount"] == 3);
      CHECK(tail["log"]["text"] == "line 2\nline 3\n");
      CHECK(tail["log"]["truncated"] == true);

      const auto full = fixture.call("compile_status", Json{{"log", "full"}});
      CHECK(full["log"]["text"] == "line 1\nline 2\nline 3\n");

      const auto none = fixture.call("compile_status", Json{{"log", "none"}});
      CHECK(!none["log"].contains("text"));

      CHECK(
        fixture.callExpectingError("compile_status", Json{{"tailLines", 0}}).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("leak")
    {
      const auto pointFile = env.dir() / "maps/compile/test.pts";
      env.createFile("maps/compile/test.pts", "0 0 0\n0 0 64\n0 0 128\n");
      job->append(
        "#### Exporting map file '" + (env.dir() / "maps/compile/test.map").string()
        + "'\n#### Executing '/tools/qbsp test'\n"
          "Reached occupant \"info_player_start\" at (64 32 16), no filling "
          "performed.\nLeak file written to "
        + pointFile.string() + "\n#### Finished with exit code 0\n\n");

      const auto status = fixture.call("compile_status");
      const auto& leak = status["leak"];
      CHECK(leak["detected"] == true);
      CHECK(leak["entity"] == "info_player_start");
      CHECK(leak["position"] == Json::array({64.0, 32.0, 16.0}));
      CHECK(leak["pointFile"] == pointFile.string());
      CHECK(leak["pointFileExists"] == true);
      CHECK(leak["hint"].get<std::string>().find("pointfile_load") != std::string::npos);
    }
  }

  SECTION("compile_cancel")
  {
    createSavedDocument(fixture, env);
    configureTools(fixture, env, "Quake");
    fixture.call("compile_run", Json{{"preset", "normal"}});

    const auto dryRun = fixture.call("compile_cancel", Json{{"dryRun", true}})["result"];
    CHECK(dryRun["wouldDo"].is_string());
    CHECK(dryRun["state"] == "running");

    const auto cancelled = fixture.call("compile_cancel")["result"];
    CHECK(cancelled["run"] == "run:1");
    CHECK(cancelled["state"] == "cancelled");
    CHECK(compile.lastJob()->running() == false);

    const auto again = fixture.call("compile_cancel", Json{{"run", "run:1"}});
    CHECK(warningCodes(again) == std::vector<std::string>{"NOT_RUNNING"});
    CHECK(again["result"]["state"] == "cancelled");

    CHECK(
      fixture.callExpectingError("compile_cancel", Json{{"run", "run:5"}}).code
      == ErrorCode::ObjectNotFound);
  }

  SECTION("closing the document cancels its run")
  {
    auto& document = createSavedDocument(fixture, env);
    configureTools(fixture, env, "Quake");
    fixture.call("compile_run", Json{{"preset", "normal"}});
    compile.lastJob()->append("output\n");

    fixture.host().removeDocument(document);
    CHECK(compile.started[0].job == nullptr);

    const auto status = fixture.call("compile_status", Json{{"run", "run:1"}});
    CHECK(status["state"] == "cancelled");
    CHECK(status["documentOpen"] == false);
    CHECK(status["log"]["text"].get<std::string>().starts_with("output\n"));
  }

  SECTION("log resource")
  {
    createSavedDocument(fixture, env);
    configureTools(fixture, env, "Quake");
    fixture.call("compile_run", Json{{"preset", "normal"}});
    const auto uri = std::string{"trenchbroom://compile/run:1/log"};

    const auto list = fixture.rpc("resources/list");
    CHECK(std::ranges::any_of(list["result"]["resources"], [&](const auto& entry) {
      return entry["uri"] == uri;
    }));

    auto notifications = std::make_shared<CapturingNotificationStream>();
    REQUIRE(fixture.server().openNotificationStream(fixture.sessionId(), notifications));
    REQUIRE(fixture.rpc("resources/subscribe", Json{{"uri", uri}})["result"].is_object());

    compile.lastJob()->append("first\n");
    compile.lastJob()->append("second\n");
    fixture.scheduler().runPending();
    CHECK(std::ranges::count(updatedUris(*notifications), uri) == 1);

    notifications->notifications.clear();
    compile.lastJob()->finish();
    fixture.scheduler().runPending();
    CHECK(std::ranges::count(updatedUris(*notifications), uri) == 1);

    const auto read = fixture.rpc("resources/read", Json{{"uri", uri}});
    REQUIRE(read.contains("result"));
    const auto& contents = read["result"]["contents"];
    REQUIRE(contents.size() == 1);
    CHECK(contents[0]["mimeType"] == "text/plain");
    CHECK(contents[0]["text"] == "first\nsecond\n");

    CHECK(fixture.rpc("resources/read", Json{{"uri", "trenchbroom://compile/run:7/log"}})
            .contains("error"));
  }

  SECTION("pointfile_load")
  {
    auto& document = createSavedDocument(fixture, env);
    fixture.call(
      "brush_create_box", Json{{"min", {-256, -256, -256}}, {"max", {256, 256, 256}}});
    fixture.call(
      "entity_create_point",
      Json{{"classname", "info_player_start"}, {"position", {0, 0, 0}}});
    fixture.call(
      "entity_create_point", Json{{"classname", "light"}, {"position", {0, 0, 128}}});

    SECTION("no point file")
    {
      const auto error = fixture.callExpectingError("pointfile_load");
      CHECK(error.code == ErrorCode::IoError);
      CHECK(error.message.find("test.pts") != std::string::npos);
    }

    SECTION("loads the default point file")
    {
      env.createFile("maps/compile/test.pts", "0 0 0\n0 0 128\n0 0 512\n0 512 512\n");

      const auto dryRun = fixture.call("pointfile_load", Json{{"dryRun", true}});
      CHECK(dryRun["result"]["wouldDo"].is_string());
      CHECK(!document.isPointFileLoaded());

      const auto result = fixture.call("pointfile_load")["result"];
      CHECK(result["path"] == (env.dir() / "maps/compile/test.pts").string());
      CHECK(result["pointCount"].get<size_t>() >= 2);
      CHECK(result["truncated"] == false);
      CHECK(result["length"].get<double>() > 500.0);
      CHECK(result["start"]["point"] == Json::array({0.0, 0.0, 0.0}));
      CHECK(result["start"]["nearestEntities"][0]["classname"] == "info_player_start");
      CHECK(result["start"]["nearestEntities"][0]["distance"] == 0.0);
      CHECK(result["start"]["nearestEntities"][1]["classname"] == "light");
      CHECK(result["end"]["point"] == Json::array({0.0, 512.0, 512.0}));
      CHECK(result["leavesMapAt"].is_array());
      CHECK(document.isPointFileLoaded());

      const auto unloaded = fixture.call("pointfile_unload")["result"];
      CHECK(unloaded["unloaded"] == true);
      CHECK(!document.isPointFileLoaded());

      const auto again = fixture.call("pointfile_unload");
      CHECK(again["result"]["unloaded"] == false);
      CHECK(warningCodes(again) == std::vector<std::string>{"NOT_LOADED"});
    }

    SECTION("invalid files")
    {
      env.createFile("maps/bad.pts", "0 0 0\n");
      CHECK(
        fixture
          .callExpectingError(
            "pointfile_load", Json{{"path", (env.dir() / "maps/bad.pts").string()}})
          .code
        == ErrorCode::IoError);
      CHECK(
        fixture.callExpectingError("pointfile_load", Json{{"path", "bad.pts"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("pointfile_load between rooms")
  {
    createSavedDocument(fixture, env);
    const auto box = [&](const vm::vec3d& min, const vm::vec3d& max) {
      fixture.call(
        "brush_create_box",
        Json{{"min", {min.x(), min.y(), min.z()}}, {"max", {max.x(), max.y(), max.z()}}});
    };
    // two rooms side by side; the void between them lies inside the brush bounds
    const auto room = [&](const double x0, const double x1, const bool holeInEastWall) {
      box({x0 - 16, -144, -16}, {x1 + 16, 144, 0});   // floor
      box({x0 - 16, -144, 128}, {x1 + 16, 144, 144}); // ceiling
      box({x0 - 16, -144, 0}, {x1 + 16, -128, 128});  // south
      box({x0 - 16, 128, 0}, {x1 + 16, 144, 128});    // north
      box({x0 - 16, -128, 0}, {x0, 128, 128});        // west
      if (holeInEastWall)
      {
        box({x1, -128, 0}, {x1 + 16, -32, 128});
        box({x1, 32, 0}, {x1 + 16, 128, 128});
        box({x1, -32, 0}, {x1 + 16, 32, 32});
        box({x1, -32, 96}, {x1 + 16, 32, 128});
      }
      else
      {
        box({x1, -128, 0}, {x1 + 16, 128, 128});
      }
    };
    room(-256, 0, true);
    room(256, 512, false);

    env.createFile("maps/compile/test.pts", "-128 0 64\n128 0 64\n128 0 32\n");
    const auto result = fixture.call("pointfile_load")["result"];
    REQUIRE(result["leavesMapAt"].is_array());
    const auto x = result["leavesMapAt"][0].get<double>();
    CHECK(x >= 0.0);
    CHECK(x <= 48.0);
    CHECK(result["leavesMapAt"][1] == 0.0);
    CHECK(result["leavesMapAt"][2] == 64.0);
  }

  SECTION("portalfile_load")
  {
    auto& document = createSavedDocument(fixture, env);

    CHECK(fixture.callExpectingError("portalfile_load").code == ErrorCode::IoError);

    env.createFile(
      "maps/test.prt",
      "PRT1\n2\n2\n"
      "4 0 1 (0 0 0 ) (0 64 0 ) (0 64 64 ) (0 0 64 )\n"
      "4 0 1 (64 0 0 ) (64 64 0 ) (64 64 64 ) (64 0 64 )\n");

    const auto dryRun = fixture.call("portalfile_load", Json{{"dryRun", true}});
    CHECK(dryRun["result"]["wouldDo"].is_string());
    CHECK(!document.isPortalFileLoaded());

    const auto result = fixture.call("portalfile_load")["result"];
    CHECK(result["path"] == (env.dir() / "maps/test.prt").string());
    CHECK(result["portalCount"] == 2);
    CHECK(document.isPortalFileLoaded());

    CHECK(fixture.call("portalfile_unload")["result"]["unloaded"] == true);
    CHECK(!document.isPortalFileLoaded());
  }
}

} // namespace tb::mcp
