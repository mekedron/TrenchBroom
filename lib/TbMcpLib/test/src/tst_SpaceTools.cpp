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

#include "TestEnvironment.h"
#include "mcp/McpToolFixture.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::Node* findNode(
  mdl::Node& node, const std::function<bool(const mdl::Node&)>& predicate)
{
  if (predicate(node))
  {
    return &node;
  }
  for (auto* child : node.children())
  {
    if (auto* result = findNode(*child, predicate))
    {
      return result;
    }
  }
  return nullptr;
}

mdl::Node* findEntity(mdl::Map& map, const std::string& classname)
{
  return findNode(map.worldNode(), [&](const mdl::Node& node) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
    return entityNode && entityNode->entity().classname() == classname;
  });
}

std::vector<Json> contentOf(const Json& raw, const std::string& type)
{
  auto result = std::vector<Json>{};
  for (const auto& block : raw["content"])
  {
    if (block["type"] == type)
    {
      result.push_back(block);
    }
  }
  return result;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return result.contains("warnings")
         && std::ranges::any_of(result["warnings"], [&](const auto& warning) {
              return warning["code"] == code;
            });
}

const Json* findSpaceContaining(const Json& spaces, const double x, const double y)
{
  for (const auto& space : spaces)
  {
    const auto& bounds = space["bounds"];
    if (
      bounds["min"][0].get<double>() <= x && x <= bounds["max"][0].get<double>()
      && bounds["min"][1].get<double>() <= y && y <= bounds["max"][1].get<double>())
    {
      return &space;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("SpaceTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "spaces.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  auto& map = document.map();

  auto* door = findEntity(map, "func_door");
  auto* playerStart = findEntity(map, "info_player_start");
  REQUIRE(door);
  REQUIRE(playerStart);

  SECTION("spaces_list")
  {
    SECTION("two rooms and a doorway")
    {
      const auto result = fixture.call("spaces_list");
      CHECK(result["count"] == 2);
      CHECK(result["cellSize"] == 16);
      REQUIRE(result["spaces"].size() == 2);
      const auto* room1 = findSpaceContaining(result["spaces"], 64, 192);
      const auto* room2 = findSpaceContaining(result["spaces"], 784, 192);
      REQUIRE(room1);
      REQUIRE(room2);
      CHECK((*room1)["bounds"] == Json{{"min", {0, 0, 0}}, {"max", {512, 384, 192}}});
      CHECK((*room1)["floor"]["typical"] == 0);
      CHECK((*room1)["ceiling"]["typical"] == 192);
      CHECK((*room1)["height"] == 192);
      CHECK((*room1)["sealed"] == true);
      CHECK((*room1)["neighbours"] == Json::array({(*room2)["id"]}));
      CHECK((*room1)["groups"] == Json::array({"Chair"}));
      CHECK((*room1)["objects"]["classnames"]["info_player_start"] == 1);
      CHECK((*room1)["objects"]["classnames"]["light"] == 1);
      CHECK((*room1)["objects"]["groups"] == 1);
      CHECK((*room2)["layers"] == Json::array({"Lights"}));
      CHECK(!(*room1).contains("contents"));

      REQUIRE(result["openings"].size() == 1);
      const auto& opening = result["openings"][0];
      CHECK(opening["kind"] == "doorway");
      CHECK(opening["width"] == 64);
      CHECK(opening["height"] == 112);
      CHECK(opening["bottom"] == 0);
      CHECK(opening["center"][1] == 192);
      CHECK(opening["doors"] == Json::array({fixture.id(*door)}));
      CHECK(
        (opening["spaces"] == Json::array({(*room1)["id"], (*room2)["id"]})
         || opening["spaces"] == Json::array({(*room2)["id"], (*room1)["id"]})));
      CHECK(result["outsideOpenings"].empty());
      CHECK((*room1)["openings"] == Json::array({opening["id"]}));
    }

    SECTION("openingSize")
    {
      // the doorway is 64 wide and 112 high: openings up to openingSize separate, and
      // openingSize is rounded down to a multiple of 2 x cellSize
      const auto count = [&](const double openingSize) {
        return fixture.call("spaces_list", Json{{"openingSize", openingSize}})["count"];
      };
      CHECK(count(64) == 2);
      CHECK(count(95) == 2);
      CHECK(count(63) == 1);

      // not smaller than the rooms' height (192): the rooms have no core
      const auto tooLarge = fixture.call("spaces_list", Json{{"openingSize", 192}});
      CHECK(hasWarning(tooLarge, "OPENING_SIZE_TOO_LARGE"));
      const auto fits = fixture.call("spaces_list", Json{{"openingSize", 176}});
      CHECK(fits["count"] == 2);
      CHECK_FALSE(hasWarning(fits, "OPENING_SIZE_TOO_LARGE"));
    }

    SECTION("full detail and region filter")
    {
      const auto result = fixture.call(
        "spaces_list",
        Json{
          {"detail", "full"},
          {"region", Json{{"min", {0, 0, 0}}, {"max", {256, 256, 64}}}},
        });
      REQUIRE(result["spaces"].size() == 1);
      const auto& contents = result["spaces"][0]["contents"];
      CHECK(std::ranges::any_of(contents, [&](const auto& item) {
        return item["id"] == fixture.id(*playerStart);
      }));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("spaces_list", Json{{"cellSize", 0}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("spaces_list", Json{{"cellSize", 2}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("surroundings")
  {
    SECTION("inside room 1")
    {
      const auto result = fixture.call("surroundings", Json{{"point", {64, 192, 48}}});
      REQUIRE(result["space"].is_object());
      CHECK(result["inside"] == "space");
      CHECK(result["floor"]["z"] == 0);
      CHECK(result["floor"]["distance"] == 48);
      CHECK(result["ceiling"]["z"] == 192);
      REQUIRE(result["walls"].size() == 8);
      const auto west = std::ranges::find_if(
        result["walls"], [](const auto& wall) { return wall["direction"] == "W"; });
      REQUIRE(west != result["walls"].end());
      CHECK((*west)["distance"] == 64);
      CHECK((*west)["material"] == "wall_brick");
      CHECK(
        (*west)["face"].template get<std::string>().find("/face:") != std::string::npos);
      const auto north = std::ranges::find_if(
        result["walls"], [](const auto& wall) { return wall["direction"] == "N"; });
      CHECK((*north)["distance"] == 192);

      REQUIRE(!result["objects"].empty());
      CHECK(result["objects"][0]["id"] == fixture.id(*playerStart));
      CHECK(result["objects"][0]["direction"] == "below");
      const auto chair = std::ranges::find_if(
        result["objects"], [](const auto& object) { return object["label"] == "Chair"; });
      REQUIRE(chair != result["objects"].end());
      CHECK((*chair)["direction"] == "SE");

      const auto description = result["description"].get<std::string>();
      CHECK(
        description.find(result["space"]["id"].get<std::string>()) != std::string::npos);
      CHECK(description.find("W 64") != std::string::npos);
      CHECK(description.find("Chair") != std::string::npos);
    }

    SECTION("without the space and without diagonals")
    {
      const auto result = fixture.call(
        "surroundings",
        Json{{"point", {784, 192, 48}}, {"includeSpace", false}, {"diagonals", false}});
      CHECK(result["space"].is_null());
      CHECK(result["walls"].size() == 4);
    }

    SECTION("outside")
    {
      const auto result = fixture.call("surroundings", Json{{"point", {256, 192, 1000}}});
      CHECK(result["space"].is_null());
      CHECK(result["inside"] == "void");
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("surroundings", Json::object()).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("free_spots")
  {
    const auto spaces = fixture.call("spaces_list")["spaces"];
    const auto room2 = (*findSpaceContaining(spaces, 784, 192))["id"];

    SECTION("a wall spot for a poster")
    {
      const auto result = fixture.call(
        "free_spots",
        Json{
          {"size", {64, 4, 64}},
          {"placement", "wall"},
          {"space", room2},
          {"heightAboveFloor", 64},
          {"limit", 3},
        });
      REQUIRE(result["count"] == 3);
      for (const auto& spot : result["spots"])
      {
        CHECK(spot["space"] == room2);
        CHECK(spot["min"][2] == 64);
        const auto& wall = spot["wall"];
        const auto faceId = wall["face"].get<std::string>();
        CHECK(faceId.starts_with(wall["brush"].get<std::string>() + "/face:"));
        CHECK(wall["normal"][2] == 0);
        CHECK(wall["material"] == "wall_brick");
        // the face exists and faces the way reported
        const auto face = fixture.call("object_get", Json{{"ids", {faceId}}});
        CHECK(face.dump().find("wall_brick") != std::string::npos);
        // the box touches the wall
        const auto normal = wall["normal"];
        const auto axis = normal[0] != 0 ? 0 : 1;
        CHECK(spot["size"][axis] == 4);
        const auto touching =
          std::string{normal[axis] == 1 ? "-" : "+"} + (axis == 0 ? "x" : "y");
        CHECK(spot["clearance"][touching] == 0);
      }
    }

    SECTION("floor spots keep their distance")
    {
      const auto result = fixture.call(
        "free_spots",
        Json{
          {"size", {32, 32, 56}},
          {"space", room2},
          {"wallDistance", 32},
          {"limit", 5},
        });
      REQUIRE(result["count"] == 5);
      for (const auto& spot : result["spots"])
      {
        CHECK(spot["min"][2] == 0);
        CHECK(spot["floor"] == 0);
        CHECK(spot["min"][0].get<double>() >= 528 + 32);
        CHECK(spot["max"][0].get<double>() <= 1040 - 32);
        CHECK(spot["origin"][2] == 0);
      }
    }

    SECTION("region and near")
    {
      const auto result = fixture.call(
        "free_spots",
        Json{
          {"size", {16, 16, 16}},
          {"region", Json{{"min", {0, 0, 0}}, {"max", {512, 384, 192}}}},
          {"near", {112, 112, 30}},
          {"limit", 1},
        });
      REQUIRE(result["count"] == 1);
      // on the chair's seat
      CHECK(result["spots"][0]["min"][2] == 24);
    }

    SECTION("ceiling")
    {
      const auto result = fixture.call(
        "free_spots",
        Json{
          {"size", {16, 16, 8}},
          {"placement", "ceiling"},
          {"space", room2},
          {"limit", 2}});
      REQUIRE(result["count"] == 2);
      CHECK(result["spots"][0]["max"][2] == 192);
    }

    SECTION("nothing fits")
    {
      const auto result =
        fixture.call("free_spots", Json{{"size", {2000, 2000, 32}}, {"space", room2}});
      CHECK(result["count"] == 0);
      CHECK(hasWarning(result, "NO_FREE_SPOT"));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("free_spots", Json{{"size", {0, 16, 16}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "free_spots", Json{{"size", {16, 16, 16}}, {"space", "space:00000000"}})
          .code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture
          .callExpectingError(
            "free_spots", Json{{"size", {16, 16, 16}}, {"sort", "near"}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("walkable_plan")
  {
    SECTION("text")
    {
      const auto result = fixture.call("walkable_plan");
      CHECK(result["cellSize"] == 16);
      CHECK(result["player"]["width"] == 32);
      CHECK(result["player"]["stepHeight"] == 18);
      CHECK(result["start"]["point"] == Json::array({64, 192, 24}));
      CHECK(result["start"]["floor"][2] == 0);
      CHECK(result["reachableCells"].get<size_t>() > 1000);
      CHECK(result["spacesReached"].size() == 2);
      const auto text = result["text"].get<std::string>();
      CHECK(text.find('S') != std::string::npos);
      CHECK(text.find('D') != std::string::npos);
      CHECK(text.find('#') != std::string::npos);
      CHECK(result["legend"].contains("."));
      // the roofs are walkable, but outside the spaces
      CHECK(result["outsideCells"].get<size_t>() > 0);
      CHECK(result["unreachableAreas"].empty());
    }

    SECTION("image and height range")
    {
      const auto raw = fixture.callRaw(
        "walkable_plan",
        Json{
          {"format", "image"},
          {"heightRange", {-8, 64}},
          {"from", fixture.id(*playerStart)}});
      REQUIRE(raw["isError"] == false);
      const auto& result = raw["structuredContent"];
      CHECK(!result.contains("text"));
      CHECK(result["image"]["format"] == "png");
      CHECK(result["unreachableAreas"].empty());
      const auto images = contentOf(raw, "image");
      REQUIRE(images.size() == 1);
      CHECK(images.front()["mimeType"] == "image/png");
    }

    SECTION("both")
    {
      const auto raw = fixture.callRaw(
        "walkable_plan", Json{{"format", "both"}, {"start", {784, 192, 24}}});
      REQUIRE(raw["isError"] == false);
      CHECK(raw["structuredContent"].contains("text"));
      CHECK(contentOf(raw, "image").size() == 1);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("walkable_plan", Json{{"format", "svg"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("walkable_plan", Json{{"from", "entity:99999"}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("walkable_plan", Json{{"cellSize", 2}}).code
        == ErrorCode::InvalidArgument);
    }
  }
}

} // namespace tb::mcp
