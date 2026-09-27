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
#include "mcp/MapManifest.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& result)
{
  return result["result"];
}

bool hasWarning(const Json& result, const std::string& code)
{
  if (!result.contains("warnings"))
  {
    return false;
  }
  for (const auto& warning : result["warnings"])
  {
    if (warning["code"] == code)
    {
      return true;
    }
  }
  return false;
}

MapManifest readManifest(const std::filesystem::path& path)
{
  auto read = readManifestFile(path);
  REQUIRE(read.is_success());
  REQUIRE(read.value().has_value());
  return *read.value();
}

const auto HallSpace = Json{
  {"id", "space:1"},
  {"name", "Hall"},
  {"purpose", "arrival"},
  {"bounds", {{"min", {0, 0, 0}}, {"max", {256, 256, 128}}}},
};

void saveMap(McpToolFixture& fixture, const std::filesystem::path& path)
{
  fixture.call("brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {64, 64, 64}}});
  fixture.call("document_save_as", Json{{"path", path.string()}});
}

} // namespace

TEST_CASE("ManifestTools")
{
  auto env = fs::TestEnvironment{};
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();

  SECTION("map_manifest_get")
  {
    SECTION("an empty manifest of an unsaved map")
    {
      const auto result = fixture.call("map_manifest_get");
      CHECK(result["path"].is_null());
      CHECK(result["exists"] == false);
      CHECK(result["pending"] == false);
      CHECK(result["spaces"] == Json::array());
      CHECK(result["keyPoints"] == Json::array());
      CHECK(result["notes"] == Json::array());
      CHECK(result["cameras"] == Json::array());
    }

    SECTION("reads the file next to the map lazily and filters sections")
    {
      const auto path = env.dir() / "e1.map";
      saveMap(fixture, path);

      auto manifest = MapManifest{};
      manifest.notes = {"written by another session"};
      manifest.spaces = {ManifestSpace{"space:1", "Hall"}};
      REQUIRE(writeManifestFile(env.dir() / "e1.mcp.json", manifest).is_success());

      const auto result =
        fixture.call("map_manifest_get", Json{{"sections", {"notes", "spaces"}}});
      CHECK(result["path"] == (env.dir() / "e1.mcp.json").string());
      CHECK(result["exists"] == true);
      CHECK(result["notes"] == Json{"written by another session"});
      CHECK(result["spaces"][0]["name"] == "Hall");
      CHECK_FALSE(result.contains("cameras"));
      CHECK_FALSE(result.contains("keyPoints"));
    }

    SECTION("an invalid file is a clear error")
    {
      const auto path = env.dir() / "e1.map";
      saveMap(fixture, path);
      env.createFile("e1.mcp.json", "{ not json");

      const auto error = fixture.callExpectingError("map_manifest_get");
      CHECK(error.code == ErrorCode::IoError);
      CHECK(error.message.find("not valid JSON") != std::string::npos);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("map_manifest_get", Json{{"sections", {"rooms"}}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("map_manifest_set")
  {
    const auto path = env.dir() / "e1.map";
    const auto manifestFile = env.dir() / "e1.mcp.json";

    SECTION("writes the file next to a saved map")
    {
      saveMap(fixture, path);
      const auto result = fixture.call(
        "map_manifest_set",
        Json{
          {"spaces", {HallSpace}},
          {"keyPoints",
           {{{"name", "altar"}, {"position", {0, 128, 16}}, {"note", "the key"}}}},
          {"notes", {"Doors open towards the hall."}},
        });
      CHECK(resultOf(result)["written"] == true);
      CHECK(resultOf(result)["pending"] == false);
      CHECK(resultOf(result)["path"] == manifestFile.string());
      CHECK(
        resultOf(result)["counts"]
        == Json{{"spaces", 1}, {"keyPoints", 1}, {"notes", 1}, {"cameras", 0}});
      CHECK(result["undoStep"].is_null());
      CHECK_FALSE(result.contains("issuesIntroduced"));

      const auto manifest = readManifest(manifestFile);
      REQUIRE(manifest.spaces.size() == 1);
      CHECK(manifest.spaces[0].purpose == "arrival");
      CHECK(manifest.keyPoints[0].note == "the key");
      CHECK(!document.map().modified());
    }

    SECTION("merges, replaces and removes")
    {
      saveMap(fixture, path);
      fixture.call(
        "map_manifest_set",
        Json{
          {"spaces", {HallSpace, {{"id", "space:2"}, {"name", "Armory"}}}},
          {"notes", {"first", "second"}},
        });

      // merge: change one field, remove another
      auto result = fixture.call(
        "map_manifest_set",
        Json{
          {"spaces", {{{"id", "space:1"}, {"purpose", "boss fight"}, {"name", nullptr}}}},
          {"notes", {"second", "third"}},
        });
      CHECK(resultOf(result)["changed"]["spaces"] == 1);
      CHECK(resultOf(result)["changed"]["notes"] == 1);
      auto manifest = readManifest(manifestFile);
      REQUIRE(manifest.spaces.size() == 2);
      CHECK(manifest.spaces[0].purpose == "boss fight");
      CHECK(manifest.spaces[0].name == std::nullopt);
      CHECK(manifest.spaces[0].bounds.has_value());
      CHECK(manifest.notes == std::vector<std::string>{"first", "second", "third"});

      // replace a section and remove entries
      result = fixture.call(
        "map_manifest_set",
        Json{
          {"notes", {"only"}},
          {"replace", {"notes"}},
          {"remove", {{"spaces", {"space:2", "space:7"}}}},
        });
      CHECK(resultOf(result)["notFound"] == Json{"spaces:space:7"});
      CHECK(hasWarning(result, "MANIFEST_ENTRY_NOT_FOUND"));
      manifest = readManifest(manifestFile);
      CHECK(manifest.notes == std::vector<std::string>{"only"});
      REQUIRE(manifest.spaces.size() == 1);
      CHECK(manifest.spaces[0].id == "space:1");
    }

    SECTION("dry run writes nothing")
    {
      saveMap(fixture, path);
      const auto result = fixture.call(
        "map_manifest_set", Json{{"notes", {"not written"}}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(resultOf(result)["written"] == false);
      CHECK(resultOf(result)["counts"]["notes"] == 1);
      CHECK(resultOf(result).contains("wouldDo"));
      CHECK_FALSE(env.fileExists("e1.mcp.json"));
      CHECK(fixture.call("map_manifest_get")["notes"] == Json::array());
    }

    SECTION("keeps the manifest of an unsaved map until document_save_as")
    {
      const auto result = fixture.call("map_manifest_set", Json{{"spaces", {HallSpace}}});
      CHECK(resultOf(result)["pending"] == true);
      CHECK(resultOf(result)["written"] == false);
      CHECK(resultOf(result)["path"].is_null());
      CHECK(hasWarning(result, "MANIFEST_PENDING"));

      const auto get = fixture.call("map_manifest_get");
      CHECK(get["pending"] == true);
      CHECK(get["spaces"][0]["id"] == "space:1");

      saveMap(fixture, path);
      CHECK(env.fileExists("e1.mcp.json"));
      CHECK(readManifest(manifestFile).spaces[0].name == "Hall");
      CHECK(fixture.call("map_manifest_get")["pending"] == false);

      // save as carries it to the new name
      fixture.call("document_save_as", Json{{"path", (env.dir() / "e2.map").string()}});
      CHECK(readManifest(env.dir() / "e2.mcp.json").spaces[0].name == "Hall");
      CHECK(
        fixture.call("map_manifest_get")["path"] == (env.dir() / "e2.mcp.json").string());
    }

    SECTION("saving in the editor writes a pending manifest")
    {
      fixture.call("map_manifest_set", Json{{"notes", {"pending"}}});
      REQUIRE(document.map().saveAs(path).is_success());
      CHECK(readManifest(manifestFile).notes == std::vector<std::string>{"pending"});
    }

    SECTION("an invalid file is not overwritten unless asked")
    {
      saveMap(fixture, path);
      env.createFile("e1.mcp.json", "[1, 2");
      const auto error =
        fixture.callExpectingError("map_manifest_set", Json{{"notes", {"x"}}});
      CHECK(error.code == ErrorCode::IoError);
      CHECK(env.loadFile("e1.mcp.json") == "[1, 2");

      const auto result = fixture.call(
        "map_manifest_set", Json{{"notes", {"x"}}, {"overwriteInvalid", true}});
      CHECK(hasWarning(result, "MANIFEST_OVERWRITTEN"));
      CHECK(readManifest(manifestFile).notes == std::vector<std::string>{"x"});
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "map_manifest_set", Json{{"keyPoints", {{{"name", "no position"}}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("map_manifest_set", Json{{"spaces", {{{"name", "x"}}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "map_manifest_set", Json{{"spaces", {{{"id", "a"}, {"bounds", "big"}}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("map_manifest_set", Json{{"saveCameras", {"none"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("map_manifest_set", Json{{"replace", {"rooms"}}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("validator settings are stored and restored when the map is opened")
  {
    const auto path = env.dir() / "v1.map";
    saveMap(fixture, path);

    auto result = fixture.call("validators_set", Json{{"disable", {"Z_FIGHTING"}}});
    CHECK(
      resultOf(result)["manifest"]
      == Json{{"path", (env.dir() / "v1.mcp.json").string()}, {"pending", false}});
    CHECK(
      readManifest(env.dir() / "v1.mcp.json").disabledValidators
      == std::vector<std::string>{"Z_FIGHTING"});

    // a dry run writes nothing
    fixture.call("validators_set", Json{{"enableAll", true}, {"dryRun", true}});
    CHECK(readManifest(env.dir() / "v1.mcp.json").disabledValidators.size() == 1);

    fixture.call("document_close", Json{{"unsavedChanges", "discard"}});
    fixture.call("document_open", Json{{"path", path.string()}});
    CHECK(fixture.call("validators_list")["disabled"] == Json{"Z_FIGHTING"});

    // other manifest sections keep the setting
    fixture.call("map_manifest_set", Json{{"notes", {"blockout"}}});
    CHECK(readManifest(env.dir() / "v1.mcp.json").disabledValidators.size() == 1);

    fixture.call("validators_set", Json{{"enableAll", true}});
    CHECK(readManifest(env.dir() / "v1.mcp.json").disabledValidators.empty());
  }

  SECTION("an unsaved map keeps the validator setting until it is saved")
  {
    const auto result = fixture.call("validators_set", Json{{"disable", {"Z_FIGHTING"}}});
    CHECK(resultOf(result)["manifest"] == Json{{"path", nullptr}, {"pending", true}});

    const auto path = env.dir() / "v2.map";
    saveMap(fixture, path);
    CHECK(
      readManifest(env.dir() / "v2.mcp.json").disabledValidators
      == std::vector<std::string>{"Z_FIGHTING"});
  }

  SECTION("cameras are saved and restored in a later session")
  {
    const auto path = env.dir() / "e1.map";
    saveMap(fixture, path);

    fixture.call(
      "agent_camera_set",
      Json{
        {"name", "hall"},
        {"camera",
         Json{{"position", {0, 0, 64}}, {"lookAt", {100, 0, 64}}, {"fov", 70}}}});
    fixture.call(
      "agent_camera_set",
      Json{
        {"name", "plan"},
        {"camera", Json{{"view", "top"}, {"center", {256, 256, 0}}, {"zoom", 0.5}}}});

    auto result = fixture.call("map_manifest_set", Json{{"saveCameras", "all"}});
    CHECK(resultOf(result)["savedCameras"] == Json{"hall", "plan"});
    CHECK(readManifest(env.dir() / "e1.mcp.json").cameras.size() == 2);

    const auto original = fixture.call("agent_camera_get", Json{{"name", "hall"}});

    const auto session = fixture.openSession("later");
    CHECK(
      fixture.callAs(session, "agent_camera_list", Json::object())["cameras"].empty());

    result = fixture.callAs(
      session, "map_manifest_get", Json{{"restoreCameras", Json{"hall", "missing"}}});
    CHECK(result["restoredCameras"] == Json{"hall"});
    CHECK(hasWarning(result, "UNKNOWN_CAMERA"));

    const auto restored =
      fixture.callAs(session, "agent_camera_get", Json{{"name", "hall"}});
    CHECK(restored["camera"] == original["camera"]);

    result = fixture.callAs(session, "map_manifest_get", Json{{"restoreCameras", true}});
    CHECK(result["restoredCameras"] == Json{"hall", "plan"});
    CHECK(
      fixture.callAs(session, "agent_camera_list", Json::object())["cameras"].size()
      == 2);

    // remove a saved camera
    result = fixture.call("map_manifest_set", Json{{"remove", {{"cameras", {"plan"}}}}});
    CHECK(resultOf(result)["counts"]["cameras"] == 1);
  }
}

} // namespace tb::mcp
