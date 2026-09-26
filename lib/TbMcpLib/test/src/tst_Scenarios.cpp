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

#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::BrushNode* addBox(mdl::Map& map, const vm::bbox3d& bounds)
{
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, "stone")
                 .value();
  auto* brushNode = new mdl::BrushNode{std::move(brush)};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  mdl::deselectAll(map);
  return brushNode;
}

size_t brushCount(const mdl::Node& node)
{
  auto count = size_t(dynamic_cast<const mdl::BrushNode*>(&node) ? 1 : 0);
  for (const auto* child : node.children())
  {
    count += brushCount(*child);
  }
  return count;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return resultOf(
           fixture.call("brush_create_box", Json{{"min", min}, {"max", max}}))["brush"]
    .get<std::string>();
}

} // namespace

TEST_CASE("Scenario S7")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  const auto roomCenter = vm::vec3d{512, 512, 0};

  SECTION("12 columns in a circle facing the room center")
  {
    // a column that is thin radially (16) and wide tangentially (48), off the room center
    const auto columnBounds = vm::bbox3d{{696, 488, 0}, {712, 536, 128}};
    auto* column = addBox(map, columnBounds);
    fixture.call("selection_set", Json{{"ids", {fixture.id(*column)}}});

    const auto array = fixture.call(
      "objects_array",
      Json{
        {"pattern", "circle"},
        {"count", 12},
        {"center", {512, 512, 0}},
        {"radius", 384},
      });
    CHECK(array["undoStep"] == "AI: Array Objects");
    CHECK(hasWarning(array, "NON_INTEGER_VERTICES"));

    const auto& result = array["result"];
    REQUIRE(result["instances"].size() == 12);
    CHECK(result["created"].size() == 11);
    CHECK(brushCount(map.worldNode()) == 12);

    // evenly spaced at distance 384, starting at the original's direction (+x)
    for (size_t i = 0; i < 12; ++i)
    {
      auto* node = fixture.node(result["instances"][i][0].get<std::string>());
      REQUIRE(node);
      const auto center = node->logicalBounds().center() - roomCenter;
      CHECK(
        vm::length(vm::vec2d{center.x(), center.y()}) == Catch::Approx(384).margin(0.01));
      const auto expectedAngle = vm::to_radians(30.0 * double(i));
      CHECK(center.x() == Catch::Approx(384 * std::cos(expectedAngle)).margin(0.01));
      CHECK(center.y() == Catch::Approx(384 * std::sin(expectedAngle)).margin(0.01));
      CHECK(center.z() == Catch::Approx(64).margin(0.01));
    }

    // the original was moved to radius 384
    CHECK(vm::is_equal(column->logicalBounds().min, vm::vec3d{888, 488, 0}, 0.001));

    // the copy at 90 degrees is rotated to face the center: thin along y
    auto* quarter = fixture.node(result["instances"][3][0].get<std::string>());
    REQUIRE(quarter);
    CHECK(vm::is_equal(quarter->logicalBounds().size(), vm::vec3d{48, 16, 128}, 0.01));

    // the whole ring is selected, e.g. to group it
    CHECK(map.selection().nodes.size() == 12);

    // one undo step restores the single column
    fixture.call("undo");
    CHECK(brushCount(map.worldNode()) == 1);
    CHECK(vm::is_equal(column->logicalBounds().min, columnBounds.min, 0.001));
    CHECK(vm::is_equal(column->logicalBounds().max, columnBounds.max, 0.001));
  }

  SECTION("a spiral staircase with 20 steps")
  {
    // a step reaching 128 units out from the center of the stairs
    auto* step = addBox(map, {{512, 496, 0}, {640, 528, 16}});
    const auto stepId = fixture.id(*step);

    const auto array = fixture.call(
      "objects_array",
      Json{
        {"ids", {stepId}},
        {"pattern", "circle"},
        {"count", 20},
        {"center", {512, 512, 0}},
        {"angleStep", 18},
        {"rise", 16},
      });
    CHECK(array["undoStep"] == "AI: Array Objects");

    const auto& result = array["result"];
    REQUIRE(result["instances"].size() == 20);
    CHECK(brushCount(map.worldNode()) == 20);

    for (size_t i = 0; i < 20; ++i)
    {
      auto* node = fixture.node(result["instances"][i][0].get<std::string>());
      REQUIRE(node);
      const auto& bounds = node->logicalBounds();
      CHECK(bounds.min.z() == Catch::Approx(16.0 * double(i)).margin(0.001));
      CHECK(bounds.max.z() == Catch::Approx(16.0 * double(i) + 16).margin(0.001));
    }

    // the step at 180 degrees points towards -x
    auto* half = fixture.node(result["instances"][10][0].get<std::string>());
    REQUIRE(half);
    CHECK(vm::is_equal(half->logicalBounds().min, vm::vec3d{384, 496, 160}, 0.001));
    CHECK(vm::is_equal(half->logicalBounds().max, vm::vec3d{512, 528, 176}, 0.001));

    fixture.call("undo");
    CHECK(brushCount(map.worldNode()) == 1);
  }
}

TEST_CASE("Scenario S1 and S6 entities")
{
  // Populate two Quake rooms using the game's definitions only: a player start, lights,
  // monsters on the floor with difficulty spawnflags, and a door opened by a trigger.
  auto fixture = McpToolFixture{};
  fixture.call("document_new", Json{{"game", "Quake"}});

  // the rooms are away from the new document's default brush at the origin; floors at z =
  // 0
  fixture.call("room_create", Json{{"min", {1024, 0, 0}}, {"max", {1536, 512, 256}}});
  fixture.call("room_create", Json{{"min", {2048, 0, 0}}, {"max", {2560, 512, 256}}});

  // the agent reads the available classes and their spawnflags from the definitions
  const auto monsters =
    fixture.call("entity_classes_list", Json{{"prefix", "monster_"}, {"type", "point"}});
  CHECK(monsters["total"].get<size_t>() > 5);
  const auto ogre =
    fixture.call("entity_class_describe", Json{{"classname", "monster_ogre"}});
  CHECK(std::ranges::any_of(
    ogre["spawnflags"], [](const auto& flag) { return flag["name"] == "Not on Easy"; }));

  // player start in the first room, dropped onto the floor
  const auto start = resultOf(fixture.call(
    "entity_create_point",
    Json{
      {"classname", "info_player_start"},
      {"position", {1152, 256, 100}},
      {"angle", 0},
      {"dropToFloor", true},
    }));
  CHECK(start["onFloor"] == true);
  CHECK(start["overlaps"].empty());
  CHECK(start["origin"] == Json{1152, 256, 24});

  // a light in each room
  for (const auto x : {1280, 2304})
  {
    const auto light = fixture.call(
      "entity_create_point",
      Json{
        {"classname", "light"},
        {"position", {x, 256, 192}},
        {"properties", {{"light", 300}}},
      });
    CHECK(!hasWarning(light, "ENTITY_OVERLAPS_BRUSHES"));
    CHECK(resultOf(light)["properties"]["light"] == "300");
  }

  // three monsters on the floor of the second room, harder ones only on higher skills
  const auto monsterSpecs = std::vector<std::tuple<std::string, int, Json>>{
    {"monster_army", 2176, Json::array()},
    {"monster_ogre", 2304, Json{"Not on Easy"}},
    {"monster_shambler", 2432, Json{"Not on Easy", "Not on Normal"}},
  };
  auto monsterIds = std::vector<std::string>{};
  for (const auto& [classname, x, notOn] : monsterSpecs)
  {
    const auto monster = fixture.call(
      "entity_create_point",
      Json{
        {"classname", classname},
        {"position", {x, 384, 64}},
        {"angle", 270},
        {"dropToFloor", true},
      });
    CHECK(!hasWarning(monster, "ENTITY_OVERLAPS_BRUSHES"));
    CHECK(resultOf(monster)["onFloor"] == true);
    CHECK(resultOf(monster)["bounds"]["min"][2] == 0);
    const auto id = resultOf(monster)["entity"].get<std::string>();
    monsterIds.push_back(id);

    if (!notOn.empty())
    {
      fixture.call("entity_spawnflags_set", Json{{"ids", {id}}, {"set", notOn}});
    }

    // free space: the monster does not intersect any brush
    const auto check = fixture.call(
      "space_check", Json{{"box", resultOf(monster)["bounds"]}, {"ignore", {id}}});
    CHECK(check["free"] == true);
  }

  const auto spawnflags = [&](const std::string& id) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(fixture.node(id));
    REQUIRE(entityNode);
    const auto* value = entityNode->entity().property("spawnflags");
    return value ? *value : std::string{};
  };
  CHECK(spawnflags(monsterIds[0]).empty());
  CHECK(spawnflags(monsterIds[1]) == "256");
  CHECK(spawnflags(monsterIds[2]) == "768");

  // a door in the first room and a trigger in front of it that opens it
  const auto doorBrush = createBox(fixture, {1504, 192, 0}, {1520, 320, 128});
  const auto door = resultOf(fixture.call(
    "entity_create_brush",
    Json{
      {"classname", "func_door"},
      {"ids", {doorBrush}},
      {"properties", {{"angle", 90}}}}));
  const auto doorId = door["entity"].get<std::string>();

  const auto triggerBrush = createBox(fixture, {1408, 192, 0}, {1472, 320, 128});
  const auto trigger = resultOf(fixture.call(
    "entity_create_brush",
    Json{{"classname", "trigger_multiple"}, {"ids", {triggerBrush}}}));
  const auto triggerId = trigger["entity"].get<std::string>();

  const auto link =
    fixture.call("entity_link", Json{{"source", triggerId}, {"target", doorId}});
  CHECK(resultOf(link)["generated"] == true);
  const auto name = resultOf(link)["name"].get<std::string>();

  const auto links = fixture.call("entity_links_get");
  CHECK(links["counts"]["links"] == 1);
  CHECK(links["counts"]["missingTarget"] == 0);
  REQUIRE(links["items"].size() == 1);
  CHECK(links["items"][0]["source"] == triggerId);
  CHECK(links["items"][0]["target"] == doorId);
  CHECK(links["items"][0]["name"] == name);

  // every step is one named undo step
  const auto history = fixture.call("history_get", Json{{"limit", 20}});
  CHECK(history["undo"].size() == 15);
  for (const auto& step : history["undo"])
  {
    CHECK(step["name"].get<std::string>().starts_with("AI: "));
  }
}

} // namespace tb::mcp
