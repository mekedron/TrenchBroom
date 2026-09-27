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

#include "fs/TestEnvironment.h"
#include "mcp/FakeHost.h"
#include "mcp/McpToolFixture.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

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

/** Creates a Quake document and saves it as <env>/maps/test.map. */
ui::MapDocument& createSavedDocument(McpToolFixture& fixture, fs::TestEnvironment& env)
{
  fixture.call("document_new", Json{{"game", "Quake"}});
  env.createDirectory("maps");
  fixture.call(
    "document_save_as", Json{{"path", (env.dir() / "maps/test.map").string()}});
  return *fixture.host().documentList.back().document;
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

const std::vector<mdl::GameEngineProfile>& quakeProfiles(McpToolFixture& fixture)
{
  return fixture.host().gameManager().gameInfo("Quake")->gameEngineConfig.profiles;
}

} // namespace

TEST_CASE("EngineTools")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};
  auto& engine = fixture.host().engine;
  const auto enginePath = createExecutable(env, "engine/quake").string();

  const auto saveProfile = [&](const std::string& name, const std::string& parameters) {
    return fixture.call(
      "engine_profile_save",
      Json{
        {"game", "Quake"},
        {"name", name},
        {"path", enginePath},
        {"parameters", parameters}})["result"];
  };

  SECTION("engine_profiles_list")
  {
    SECTION("lists the profiles of the given game")
    {
      const auto empty = fixture.call("engine_profiles_list", Json{{"game", "Quake"}});
      CHECK(empty["game"] == "Quake");
      CHECK(empty["profiles"] == Json::array());

      saveProfile("Quakespasm", "+map ${MAP_BASE_NAME}");
      const auto result = fixture.call("engine_profiles_list", Json{{"game", "quake"}});
      REQUIRE(result["profiles"].size() == 1);
      const auto& profile = result["profiles"][0];
      CHECK(profile["name"] == "Quakespasm");
      CHECK(profile["id"] == quakeProfiles(fixture)[0].id);
      CHECK(profile["path"] == enginePath);
      CHECK(profile["parameters"] == "+map ${MAP_BASE_NAME}");
      CHECK(profile["status"] == "ok");
      CHECK(profile["exists"] == true);
      CHECK(profile["executable"] == true);

      CHECK(
        fixture.call("engine_profiles_list", Json{{"game", "Half-Life"}})["profiles"]
        == Json::array());
    }

    SECTION("defaults to the game of the target document")
    {
      saveProfile("Quakespasm", "");
      createSavedDocument(fixture, env);
      const auto result = fixture.call("engine_profiles_list");
      CHECK(result["game"] == "Quake");
      CHECK(result["profiles"].size() == 1);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("engine_profiles_list").code == ErrorCode::NoDocument);
      CHECK(
        fixture.callExpectingError("engine_profiles_list", Json{{"game", "Doom"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("engine_profile_save")
  {
    SECTION("creates and replaces a profile")
    {
      const auto created = fixture.call(
        "engine_profile_save",
        Json{
          {"game", "Quake"},
          {"name", "Quakespasm"},
          {"path", enginePath},
          {"parameters", "+map ${MAP_BASE_NAME}"}});
      CHECK(warningCodes(created).empty());
      const auto& result = created["result"];
      CHECK(result["game"] == "Quake");
      CHECK(result["created"] == true);
      CHECK(result["profile"]["name"] == "Quakespasm");
      CHECK(result["profile"]["executable"] == true);

      REQUIRE(quakeProfiles(fixture).size() == 1);
      const auto id = quakeProfiles(fixture)[0].id;
      CHECK(!id.empty());
      CHECK(quakeProfiles(fixture)[0].path == enginePath);
      CHECK(quakeProfiles(fixture)[0].parameterSpec == "+map ${MAP_BASE_NAME}");

      const auto exists = fixture.callExpectingError(
        "engine_profile_save",
        Json{{"game", "Quake"}, {"name", "Quakespasm"}, {"path", enginePath}});
      CHECK(exists.code == ErrorCode::FileExists);
      CHECK(exists.hint.find("overwrite") != std::string::npos);

      // omitted parameters keep their value, the id stays the same
      const auto otherPath = createExecutable(env, "engine/other").string();
      const auto replaced = fixture.call(
        "engine_profile_save",
        Json{
          {"game", "Quake"},
          {"name", "Quakespasm"},
          {"path", otherPath},
          {"overwrite", true}})["result"];
      CHECK(replaced["created"] == false);
      REQUIRE(quakeProfiles(fixture).size() == 1);
      CHECK(quakeProfiles(fixture)[0].id == id);
      CHECK(quakeProfiles(fixture)[0].path == otherPath);
      CHECK(quakeProfiles(fixture)[0].parameterSpec == "+map ${MAP_BASE_NAME}");

      saveProfile("Other", "");
      CHECK(quakeProfiles(fixture).size() == 2);
      CHECK(quakeProfiles(fixture)[1].id != id);
    }

    SECTION("warns about a missing engine")
    {
      const auto missing = (env.dir() / "engine/missing").string();
      const auto saved = fixture.call(
        "engine_profile_save",
        Json{{"game", "Quake"}, {"name", "Missing"}, {"path", missing}});
      CHECK(warningCodes(saved) == std::vector<std::string>{"TOOL_NOT_FOUND"});
      CHECK(saved["result"]["profile"]["exists"] == false);
      CHECK(saved["result"]["profile"]["executable"] == false);
      CHECK(quakeProfiles(fixture).size() == 1);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "engine_profile_save",
            Json{{"game", "Quake"}, {"name", " "}, {"path", enginePath}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "engine_profile_save",
            Json{{"game", "Quake"}, {"name", "A"}, {"path", "engine/quake"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "engine_profile_save", Json{{"game", "Quake"}, {"name", "A"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(quakeProfiles(fixture).empty());
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "engine_profile_save",
        Json{
          {"game", "Quake"},
          {"name", "Quakespasm"},
          {"path", enginePath},
          {"dryRun", true}})["result"];
      CHECK(result["wouldDo"].is_string());
      CHECK(result["created"] == true);
      CHECK(quakeProfiles(fixture).empty());
    }
  }

  SECTION("engine_launch")
  {
    SECTION("host without engine support")
    {
      createSavedDocument(fixture, env);
      saveProfile("Quakespasm", "");
      fixture.host().supportsEngine = false;
      CHECK(
        fixture.callExpectingError("engine_launch").code == ErrorCode::UnsupportedInHost);
    }

    SECTION("never saved document")
    {
      fixture.call("document_new", Json{{"game", "Quake"}});
      saveProfile("Quakespasm", "");
      const auto error = fixture.callExpectingError("engine_launch");
      CHECK(error.code == ErrorCode::UnsavedChanges);
      CHECK(error.hint.find("document_save_as") != std::string::npos);
      CHECK(engine.launches.empty());
    }

    SECTION("launches the only profile of a saved map")
    {
      auto& document = createSavedDocument(fixture, env);
      saveProfile("Quakespasm", "-game mymod +map ${MAP_BASE_NAME}");

      const auto launched = fixture.call("engine_launch");
      CHECK(warningCodes(launched).empty());
      const auto& result = launched["result"];
      CHECK(result["profile"] == "Quakespasm");
      CHECK(result["path"] == enginePath);
      CHECK(result["parameters"] == "-game mymod +map test");
      CHECK(result["processId"] == 4242);

      REQUIRE(engine.launches.size() == 1);
      CHECK(engine.launches[0].document == &document);
      CHECK(engine.launches[0].profile.name == "Quakespasm");
      CHECK(engine.launches[0].parameterSpec == std::nullopt);
      CHECK(engine.launches[0].parameters == "-game mymod +map test");
    }

    SECTION("parameters override the profile's")
    {
      createSavedDocument(fixture, env);
      saveProfile("Quakespasm", "+map ${MAP_BASE_NAME}");
      saveProfile("Other", "");

      const auto result = fixture.call(
        "engine_launch",
        Json{
          {"profile", "Quakespasm"},
          {"parameters", "+skill 3 +map ${MAP_BASE_NAME}"}})["result"];
      CHECK(result["parameters"] == "+skill 3 +map test");
      REQUIRE(engine.launches.size() == 1);
      CHECK(engine.launches[0].parameterSpec == "+skill 3 +map ${MAP_BASE_NAME}");
      // the profile is unchanged
      CHECK(quakeProfiles(fixture)[0].parameterSpec == "+map ${MAP_BASE_NAME}");

      // a profile can also be selected by its id
      fixture.call("engine_launch", Json{{"profile", quakeProfiles(fixture)[1].id}});
      REQUIRE(engine.launches.size() == 2);
      CHECK(engine.launches[1].profile.name == "Other");
    }

    SECTION("unsaved changes are a warning")
    {
      auto& document = createSavedDocument(fixture, env);
      saveProfile("Quakespasm", "+map ${MAP_BASE_NAME}");
      fixture.call(
        "entity_create_point",
        Json{{"classname", "info_player_start"}, {"position", {0, 0, 0}}});
      REQUIRE(document.map().modified());

      const auto launched = fixture.call("engine_launch");
      CHECK(warningCodes(launched) == std::vector<std::string>{"UNSAVED_CHANGES"});
      CHECK(launched["result"]["processId"] == 4242);
      CHECK(engine.launches.size() == 1);
    }

    SECTION("unknown profile")
    {
      createSavedDocument(fixture, env);

      const auto none = fixture.callExpectingError("engine_launch");
      CHECK(none.code == ErrorCode::InvalidArgument);
      CHECK(none.hint.find("engine_profile_save") != std::string::npos);

      saveProfile("A", "");
      saveProfile("B", "");
      const auto ambiguous = fixture.callExpectingError("engine_launch");
      CHECK(ambiguous.code == ErrorCode::InvalidArgument);
      CHECK(ambiguous.message.find("A, B") != std::string::npos);

      const auto unknown =
        fixture.callExpectingError("engine_launch", Json{{"profile", "C"}});
      CHECK(unknown.code == ErrorCode::InvalidArgument);
      CHECK(unknown.hint.find("engine_profiles_list") != std::string::npos);
      CHECK(engine.launches.empty());
    }

    SECTION("invalid input")
    {
      createSavedDocument(fixture, env);
      fixture.call(
        "engine_profile_save",
        Json{
          {"game", "Quake"},
          {"name", "Missing"},
          {"path", (env.dir() / "engine/missing").string()}});
      const auto missing = fixture.callExpectingError("engine_launch");
      CHECK(missing.code == ErrorCode::OperationFailed);
      CHECK(missing.message.find("notFound") != std::string::npos);

      saveProfile("Quakespasm", "");
      engine.parametersError = "unknown variable";
      const auto parameters =
        fixture.callExpectingError("engine_launch", Json{{"profile", "Quakespasm"}});
      CHECK(parameters.code == ErrorCode::InvalidArgument);
      CHECK(parameters.message.find("unknown variable") != std::string::npos);
      CHECK(
        fixture
          .callExpectingError("engine_launch", Json{{"profile", "Quakespasm"}, {"x", 1}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(engine.launches.empty());
    }

    SECTION("launch failure")
    {
      createSavedDocument(fixture, env);
      saveProfile("Quakespasm", "");
      engine.startError = "No such file";
      const auto error = fixture.callExpectingError("engine_launch");
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.message.find("No such file") != std::string::npos);
    }

    SECTION("dry run")
    {
      createSavedDocument(fixture, env);
      saveProfile("Quakespasm", "+map ${MAP_BASE_NAME}");
      const auto result = fixture.call("engine_launch", Json{{"dryRun", true}})["result"];
      CHECK(result["wouldDo"].is_string());
      CHECK(result["parameters"] == "+map test");
      CHECK(!result.contains("processId"));
      CHECK(engine.launches.empty());
    }
  }
}

} // namespace tb::mcp
