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
#include "mcp/AgentCamera.h"
#include "mcp/MapManifest.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/bbox_io.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

MapManifest sampleManifest()
{
  auto manifest = MapManifest{};
  manifest.spaces = {
    ManifestSpace{
      "space:1",
      "Hall",
      "arrival",
      "the player starts here",
      vm::bbox3d{{0, 0, 0}, {256, 256, 128}}},
    ManifestSpace{"armory", std::nullopt, "weapons", std::nullopt, std::nullopt},
  };
  manifest.keyPoints = {
    ManifestKeyPoint{"altar", {0, 128, 16}, "the key lies here"},
    ManifestKeyPoint{"door", {256, 64, 0}, std::nullopt},
  };
  manifest.notes = {"Doors open towards the hall.", "Use k_tile on floors."};
  manifest.cameras = {
    ManifestCamera{"hall", lookAtCamera({-128, -128, 64}, {128, 128, 32})},
    ManifestCamera{
      "plan", orthographicCamera(OrthoView::Top, {128, 128, 0}, 2.0, {{-1024}, {1024}})},
  };
  return manifest;
}

void checkSameCamera(const AgentCamera& actual, const AgentCamera& expected)
{
  CHECK(actual.projection == expected.projection);
  CHECK(actual.position == vm::approx{expected.position, 1e-5});
  CHECK(actual.direction == vm::approx{expected.direction, 1e-5});
  CHECK(actual.up == vm::approx{expected.up, 1e-5});
  CHECK(actual.fov == vm::approx{expected.fov, 1e-5});
  CHECK(actual.zoom == vm::approx{expected.zoom, 1e-5});
  CHECK(actual.nearPlane == vm::approx{expected.nearPlane, 1e-5});
  CHECK(actual.farPlane == vm::approx{expected.farPlane, 1e-5});
}

void checkSameManifest(const MapManifest& actual, const MapManifest& expected)
{
  CHECK(actual.spaces == expected.spaces);
  CHECK(actual.keyPoints == expected.keyPoints);
  CHECK(actual.notes == expected.notes);
  CHECK(actual.extra == expected.extra);
  REQUIRE(actual.cameras.size() == expected.cameras.size());
  for (size_t i = 0; i < actual.cameras.size(); ++i)
  {
    CHECK(actual.cameras[i].name == expected.cameras[i].name);
    checkSameCamera(actual.cameras[i].camera, expected.cameras[i].camera);
  }
}

std::string errorOf(const auto& result)
{
  return std::get<std::string>(result.error());
}

} // namespace

TEST_CASE("MapManifest")
{
  SECTION("manifestPath")
  {
    CHECK(manifestPath("/x/foo.map") == std::filesystem::path{"/x/foo.mcp.json"});
    CHECK(manifestPath("/x/foo") == std::filesystem::path{"/x/foo.mcp.json"});
    CHECK(manifestPath("/x/a.b.map") == std::filesystem::path{"/x/a.b.mcp.json"});
  }

  SECTION("toJson and manifestFromJson")
  {
    SECTION("round trip")
    {
      auto manifest = sampleManifest();
      manifest.extra = Json{{"author", "agent"}};

      const auto json = toJson(manifest);
      CHECK(json["format"] == "trenchbroom-mcp-manifest");
      CHECK(json["version"] == 1);
      CHECK(json["spaces"][0]["id"] == "space:1");
      CHECK(json["spaces"][0]["bounds"]["max"] == Json{256, 256, 128});
      CHECK_FALSE(json["spaces"][1].contains("name"));
      CHECK(json["keyPoints"][0]["position"] == Json{0, 128, 16});
      CHECK(json["cameras"][0]["camera"]["projection"] == "perspective");
      CHECK(json["cameras"][1]["camera"]["view"] == "top");
      CHECK(json["author"] == "agent");

      const auto parsed = manifestFromJson(json);
      REQUIRE(parsed.is_success());
      checkSameManifest(parsed.value(), manifest);
    }

    SECTION("missing sections are empty")
    {
      const auto parsed = manifestFromJson(Json::object());
      REQUIRE(parsed.is_success());
      CHECK(parsed.value().empty());
    }

    SECTION("invalid manifests")
    {
      const auto fails = [](const Json& json, const std::string& text) {
        const auto parsed = manifestFromJson(json);
        REQUIRE(parsed.is_error());
        CHECK_THAT(errorOf(parsed), Catch::Matchers::ContainsSubstring(text));
      };
      fails(Json::array(), "object");
      fails(Json{{"format", "something else"}}, "format");
      fails(Json{{"version", 99}}, "version");
      fails(Json{{"spaces", Json{{{"name", "no id"}}}}}, "spaces[0]");
      fails(Json{{"keyPoints", Json{{{"name", "x"}}}}}, "position");
      fails(Json{{"notes", Json{1, 2}}}, "notes");
      fails(
        Json{{"cameras", Json{{{"name", "c"}, {"camera", {{"projection", "fisheye"}}}}}}},
        "projection");
      fails(
        Json{
          {"cameras",
           Json{
             {{"name", "c"},
              {"camera",
               {{"projection", "perspective"},
                {"position", {0, 0, 0}},
                {"direction", {1, 0, 0}},
                {"up", {1, 0, 0}}}}}}}},
        "parallel");
    }
  }

  SECTION("cameraFromJson normalizes directions")
  {
    const auto camera = cameraFromJson(Json{
      {"projection", "perspective"},
      {"position", {1, 2, 3}},
      {"direction", {2, 0, 0}},
      {"up", {0.1, 0, 1}},
      {"fov", 70},
    });
    REQUIRE(camera.is_success());
    CHECK(camera.value().direction == vm::vec3d{1, 0, 0});
    CHECK(camera.value().up == vm::approx{vm::vec3d{0, 0, 1}});
    CHECK(camera.value().fov == 70.0);
  }

  SECTION("readManifestFile and writeManifestFile")
  {
    auto env = fs::TestEnvironment{};
    const auto path = env.dir() / "map.mcp.json";

    SECTION("a missing file is no manifest")
    {
      const auto read = readManifestFile(path);
      REQUIRE(read.is_success());
      CHECK_FALSE(read.value().has_value());
    }

    SECTION("round trip through a file")
    {
      const auto manifest = sampleManifest();
      REQUIRE(writeManifestFile(path, manifest).is_success());
      CHECK(env.fileExists("map.mcp.json"));
      CHECK_FALSE(env.fileExists("map.mcp.json.tmp"));

      const auto read = readManifestFile(path);
      REQUIRE(read.is_success());
      REQUIRE(read.value().has_value());
      checkSameManifest(*read.value(), manifest);
    }

    SECTION("invalid files")
    {
      env.createFile("map.mcp.json", "{ not json");
      auto read = readManifestFile(path);
      REQUIRE(read.is_error());
      CHECK_THAT(errorOf(read), Catch::Matchers::ContainsSubstring("not valid JSON"));

      env.createFile("map.mcp.json", R"({"spaces": 5})");
      read = readManifestFile(path);
      REQUIRE(read.is_error());
      CHECK_THAT(errorOf(read), Catch::Matchers::ContainsSubstring("invalid"));
    }
  }

  SECTION("applyUpdate")
  {
    auto manifest = sampleManifest();

    SECTION("merges spaces by id and key points by name")
    {
      auto update = ManifestUpdate{};
      update.spaces = {
        SpaceUpdate{
          "space:1",
          FieldUpdate<std::string>{"Great hall"},
          FieldUpdate<std::string>{},
          FieldUpdate<std::string>{std::optional<std::string>{}},
          FieldUpdate<vm::bbox3d>{}},
        SpaceUpdate{"space:9", FieldUpdate<std::string>{"Cellar"}},
      };
      update.keyPoints = {
        KeyPointUpdate{"altar", std::nullopt, FieldUpdate<std::string>{"moved"}},
        KeyPointUpdate{"exit", vm::vec3d{512, 0, 0}},
      };
      update.notes = {"Doors open towards the hall.", "New note"};

      const auto result = applyUpdate(manifest, update);
      REQUIRE(result.is_success());
      CHECK(result.value().spaces == 2);
      CHECK(result.value().keyPoints == 2);
      CHECK(result.value().notes == 1);

      REQUIRE(manifest.spaces.size() == 3);
      CHECK(manifest.spaces[0].name == "Great hall");
      CHECK(manifest.spaces[0].purpose == "arrival");
      CHECK(manifest.spaces[0].notes == std::nullopt);
      CHECK(manifest.spaces[0].bounds.has_value());
      CHECK(manifest.spaces[2].id == "space:9");
      CHECK(manifest.spaces[2].name == "Cellar");

      REQUIRE(manifest.keyPoints.size() == 3);
      CHECK(manifest.keyPoints[0].position == vm::vec3d{0, 128, 16});
      CHECK(manifest.keyPoints[0].note == "moved");
      CHECK(manifest.keyPoints[2].position == vm::vec3d{512, 0, 0});

      CHECK(
        manifest.notes
        == std::vector<std::string>{
          "Doors open towards the hall.", "Use k_tile on floors.", "New note"});
    }

    SECTION("replaces sections and removes entries")
    {
      auto update = ManifestUpdate{};
      update.replace = {"notes", "cameras"};
      update.notes = {"Only note"};
      update.removeSpaces = {"armory", "unknown"};
      update.removeKeyPoints = {"door"};

      const auto result = applyUpdate(manifest, update);
      REQUIRE(result.is_success());
      CHECK(manifest.notes == std::vector<std::string>{"Only note"});
      CHECK(manifest.cameras.empty());
      REQUIRE(manifest.spaces.size() == 1);
      CHECK(manifest.spaces[0].id == "space:1");
      REQUIRE(manifest.keyPoints.size() == 1);
      CHECK(result.value().removed == 2 + 2 + 1 + 1);
      CHECK(result.value().notFound == std::vector<std::string>{"spaces:unknown"});
    }

    SECTION("errors leave the manifest unchanged")
    {
      const auto before = manifest;
      auto update = ManifestUpdate{};
      update.notes = {"added"};
      update.keyPoints = {KeyPointUpdate{"new point"}};
      const auto result = applyUpdate(manifest, update);
      REQUIRE(result.is_error());
      CHECK_THAT(errorOf(result), Catch::Matchers::ContainsSubstring("position"));
      CHECK(manifest == before);

      update = ManifestUpdate{};
      update.replace = {"everything"};
      CHECK(applyUpdate(manifest, update).is_error());
    }
  }

  SECTION("spaceUpdateFromJson and keyPointUpdateFromJson")
  {
    const auto space =
      spaceUpdateFromJson(Json{{"id", "space:1"}, {"name", "Hall"}, {"notes", nullptr}});
    REQUIRE(space.is_success());
    CHECK(space.value().name == FieldUpdate<std::string>{"Hall"});
    CHECK(space.value().notes == FieldUpdate<std::string>{std::optional<std::string>{}});
    CHECK_FALSE(space.value().purpose.has_value());
    CHECK(spaceUpdateFromJson(Json{{"name", "no id"}}).is_error());
    CHECK(spaceUpdateFromJson(Json{{"id", "a"}, {"bounds", "big"}}).is_error());

    const auto keyPoint =
      keyPointUpdateFromJson(Json{{"name", "altar"}, {"position", {1, 2, 3}}});
    REQUIRE(keyPoint.is_success());
    CHECK(keyPoint.value().position == vm::vec3d{1, 2, 3});
    CHECK(keyPointUpdateFromJson(Json{{"name", "altar"}, {"position", "x"}}).is_error());
  }

  SECTION("ManifestStore")
  {
    auto env = fs::TestEnvironment{};
    const auto mapPath = env.dir() / "e1.map";
    const auto filePath = env.dir() / "e1.mcp.json";

    SECTION("reads the file lazily")
    {
      REQUIRE(writeManifestFile(filePath, sampleManifest()).is_success());
      auto store = ManifestStore{mapPath};
      CHECK(store.filePath() == filePath);
      const auto manifest = store.get();
      REQUIRE(manifest.is_success());
      CHECK(manifest.value().spaces.size() == 2);
      CHECK_FALSE(store.pending());
    }

    SECTION("writes on set when the map has a file")
    {
      auto store = ManifestStore{mapPath};
      REQUIRE(store.get().is_success());
      REQUIRE(store.set(sampleManifest()).is_success());
      CHECK_FALSE(store.pending());
      const auto read = readManifestFile(filePath);
      REQUIRE(read.is_success());
      CHECK(read.value().has_value());
    }

    SECTION("an invalid file is an error and is not overwritten by saving")
    {
      env.createFile("e1.mcp.json", "{ broken");
      auto store = ManifestStore{mapPath};
      CHECK(store.get().is_error());

      store.mapWasSaved(env.dir() / "e2.map");
      REQUIRE(store.saveError().has_value());
      CHECK(env.loadFile("e1.mcp.json") == "{ broken");
      CHECK_FALSE(env.fileExists("e2.mcp.json"));
    }

    SECTION("keeps the manifest of an unsaved map in memory until it is saved")
    {
      auto store = ManifestStore{"unnamed.map"};
      CHECK_FALSE(store.filePath().has_value());
      REQUIRE(store.set(sampleManifest()).is_success());
      CHECK(store.pending());

      store.mapWasSaved(mapPath);
      CHECK_FALSE(store.pending());
      CHECK_FALSE(store.saveError().has_value());
      const auto read = readManifestFile(filePath);
      REQUIRE(read.is_success());
      REQUIRE(read.value().has_value());
      CHECK(read.value()->notes == sampleManifest().notes);
    }

    SECTION("save as carries the manifest to the new name")
    {
      REQUIRE(writeManifestFile(filePath, sampleManifest()).is_success());
      auto store = ManifestStore{mapPath};

      // not read yet: read from the old file and written to the new one
      store.mapWasSaved(env.dir() / "copy.map");
      CHECK(store.filePath() == env.dir() / "copy.mcp.json");
      const auto read = readManifestFile(env.dir() / "copy.mcp.json");
      REQUIRE(read.is_success());
      REQUIRE(read.value().has_value());
      CHECK(read.value()->spaces.size() == 2);
      CHECK(env.fileExists("e1.mcp.json"));
    }

    SECTION("saving without a manifest writes nothing")
    {
      auto store = ManifestStore{mapPath};
      store.mapWasSaved(mapPath);
      store.mapWasSaved(env.dir() / "other.map");
      CHECK_FALSE(env.fileExists("e1.mcp.json"));
      CHECK_FALSE(env.fileExists("other.mcp.json"));
    }

    SECTION("loading another map forgets the manifest")
    {
      auto store = ManifestStore{"unnamed.map"};
      REQUIRE(store.set(sampleManifest()).is_success());
      store.mapWasLoaded(mapPath);
      CHECK_FALSE(store.pending());
      const auto manifest = store.get();
      REQUIRE(manifest.is_success());
      CHECK(manifest.value().empty());
    }
  }
}

} // namespace tb::mcp
