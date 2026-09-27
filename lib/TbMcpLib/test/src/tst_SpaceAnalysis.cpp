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
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <chrono>
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

mdl::EntityNode* findEntity(mdl::Map& map, const std::string& classname)
{
  return static_cast<mdl::EntityNode*>(
    findNode(map.worldNode(), [&](const mdl::Node& node) {
      const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
      return entityNode && entityNode->entity().classname() == classname;
    }));
}

mdl::BrushNode* findBrush(mdl::Map& map, const vm::bbox3d& bounds)
{
  return static_cast<mdl::BrushNode*>(
    findNode(map.worldNode(), [&](const mdl::Node& node) {
      return dynamic_cast<const mdl::BrushNode*>(&node) && node.logicalBounds() == bounds;
    }));
}

mdl::BrushNode* addCuboid(mdl::Map& map, const vm::bbox3d& box)
{
  auto brush = brushBuilder(map).createCuboid(box, "wall").value();
  const auto added = addBrushes(map, {std::move(brush)});
  REQUIRE(added.size() == 1);
  return static_cast<mdl::BrushNode*>(added.front());
}

mdl::EntityNode* addPointEntity(
  mdl::Map& map, const std::string& classname, const vm::vec3d& origin)
{
  auto* entityNode = new mdl::EntityNode{mdl::Entity{{
    {"classname", classname},
    {"origin",
     std::to_string(int(origin.x())) + " " + std::to_string(int(origin.y())) + " "
       + std::to_string(int(origin.z()))},
  }}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

mdl::EntityNode* addBrushEntity(
  mdl::Map& map,
  const std::string& classname,
  const std::vector<vm::bbox3d>& boxes,
  const std::string& material = "wall")
{
  auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", classname}}}};
  for (const auto& box : boxes)
  {
    entityNode->addChild(
      new mdl::BrushNode{brushBuilder(map).createCuboid(box, material).value()});
  }
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

/** A closed box room with 16 unit thick walls around the given interior. */
void addRoom(mdl::Map& map, const vm::bbox3d& interior)
{
  const auto& min = interior.min;
  const auto& max = interior.max;
  const auto t = 16.0;
  addCuboid(
    map, {{min.x() - t, min.y() - t, min.z() - t}, {max.x() + t, max.y() + t, min.z()}});
  addCuboid(
    map, {{min.x() - t, min.y() - t, max.z()}, {max.x() + t, max.y() + t, max.z() + t}});
  addCuboid(map, {{min.x() - t, min.y() - t, min.z()}, {min.x(), max.y() + t, max.z()}});
  addCuboid(map, {{max.x(), min.y() - t, min.z()}, {max.x() + t, max.y() + t, max.z()}});
  addCuboid(map, {{min.x(), min.y() - t, min.z()}, {max.x(), min.y(), max.z()}});
  addCuboid(map, {{min.x(), max.y(), min.z()}, {max.x(), max.y() + t, max.z()}});
}

std::vector<std::string> spaceIds(const SpaceMap& spaces)
{
  auto result = std::vector<std::string>{};
  for (const auto& space : spaces.spaces)
  {
    result.push_back(space.id);
  }
  return result;
}

const auto Room1 = vm::bbox3d{{0, 0, 0}, {512, 384, 192}};
const auto Room2 = vm::bbox3d{{528, 0, 0}, {1040, 384, 192}};

} // namespace

TEST_CASE("SpaceAnalysis")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "spaces.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});

  auto* door = findEntity(map, "func_door");
  auto* playerStart = findEntity(map, "info_player_start");
  auto* westWall = findBrush(map, {{-16, -16, 0}, {0, 400, 192}});
  auto* eastWall = findBrush(map, {{1040, -16, 0}, {1056, 400, 192}});
  REQUIRE(door);
  REQUIRE(playerStart);
  REQUIRE(westWall);
  REQUIRE(eastWall);

  SECTION("brushRole")
  {
    const auto role = brushRole(*westWall);
    CHECK(role.spaceSolid);
    CHECK(role.seals);
    CHECK(role.blocksPlayer);
    CHECK_FALSE(role.door);

    const auto doorRole =
      brushRole(*static_cast<mdl::BrushNode*>(door->children().front()));
    CHECK_FALSE(doorRole.spaceSolid);
    CHECK_FALSE(doorRole.seals);
    CHECK_FALSE(doorRole.blocksPlayer);
    CHECK(doorRole.door);
    CHECK(doorRole.blocksObjects);

    CHECK(isToolMaterialName("clip"));
    CHECK(isToolMaterialName("tools/toolsplayerclip"));
    CHECK(isToolMaterialName("AAATRIGGER"));
    CHECK(isToolMaterialName("hint"));
    CHECK_FALSE(isToolMaterialName("sky1"));
    CHECK_FALSE(isToolMaterialName("wall_brick"));
    CHECK(isClipMaterialName("CLIP"));
    CHECK(isLiquidMaterialName("*water1"));
    CHECK(isLiquidMaterialName("!toxic"));
    CHECK_FALSE(isLiquidMaterialName("sky1"));
  }

  SECTION("analyzeSpaces")
  {
    const auto result = analyzeSpaces(map);
    REQUIRE(result.is_success());
    const auto& spaces = result.value();
    CHECK(spaces.grid.cellSize == 16.0);
    REQUIRE(spaces.spaces.size() == 2);

    const auto room1 = spaces.spaceAt({64, 192, 24});
    const auto room2 = spaces.spaceAt({784, 192, 24});
    REQUIRE(room1);
    REQUIRE(room2);
    CHECK(*room1 != *room2);
    CHECK(spaces.spaces[*room1].bounds == Room1);
    CHECK(spaces.spaces[*room2].bounds == Room2);
    CHECK(spaces.spaces[*room1].sealed);
    CHECK(spaces.spaces[*room2].sealed);
    CHECK(spaces.spaces[*room1].neighbours == std::vector<size_t>{*room2});
    CHECK(spaces.findSpace(spaces.spaces[*room2].id) == room2);
    CHECK(spaces.spaces[*room1].id.starts_with("space:"));

    // one doorway between the rooms
    REQUIRE(spaces.openings.size() == 1);
    const auto& opening = spaces.openings.front();
    CHECK(opening.spaceB.has_value());
    CHECK(opening.axis == 0);
    CHECK(opening.onFloor);

    const auto details = describeOpening(map, spaces, 0);
    CHECK(details.kind == "doorway");
    CHECK(details.width == Catch::Approx(64));
    CHECK(details.height == Catch::Approx(112));
    CHECK(details.bottom == Catch::Approx(0));
    CHECK(details.center.y() == Catch::Approx(192));
    CHECK(details.center.z() == Catch::Approx(56));
    CHECK(details.center.x() >= 512.0);
    CHECK(details.center.x() <= 528.0);
    CHECK(details.doors == std::vector<const mdl::EntityNode*>{door});
    CHECK(std::abs(details.normal.x()) == 1.0);

    // floors, ceilings and contents
    const auto room1Details = describeSpace(map, spaces, *room1);
    REQUIRE(room1Details.floor);
    REQUIRE(room1Details.ceiling);
    CHECK(room1Details.floor->typical == 0.0);
    CHECK(room1Details.floor->min == 0.0);
    // the chair's seat and back
    CHECK(room1Details.floor->max > 0.0);
    CHECK(room1Details.ceiling->typical == 192.0);
    CHECK(room1Details.floorArea >= 512.0 * 384.0 - 32.0 * 32.0);
    CHECK(room1Details.groups == std::vector<std::string>{"Chair"});
    CHECK(
      std::ranges::find(room1Details.contents, playerStart)
      != room1Details.contents.end());
    CHECK(std::ranges::any_of(room1Details.contents, [](const auto* node) {
      return dynamic_cast<const mdl::GroupNode*>(node) != nullptr;
    }));

    const auto room2Details = describeSpace(map, spaces, *room2);
    CHECK(std::ranges::find(room2Details.layers, "Lights") != room2Details.layers.end());
    CHECK(std::ranges::none_of(
      room2Details.contents, [&](const auto* node) { return node == playerStart; }));
  }

  SECTION("describeOpening lists only the doors in the opening")
  {
    // a third room north of room 2, joined by a doorway without a door
    auto* northWall = findBrush(map, {{512, 384, 0}, {1040, 400, 192}});
    REQUIRE(northWall);
    mdl::removeNodes(map, {northWall});
    addCuboid(map, {{512, 384, 0}, {752, 400, 192}});
    addCuboid(map, {{816, 384, 0}, {1040, 400, 192}});
    addCuboid(map, {{752, 384, 112}, {816, 400, 192}});
    addCuboid(map, {{512, 384, -16}, {1056, 800, 0}});
    addCuboid(map, {{512, 384, 192}, {1056, 800, 208}});
    addCuboid(map, {{512, 400, 0}, {528, 800, 192}});
    addCuboid(map, {{1040, 400, 0}, {1056, 800, 192}});
    addCuboid(map, {{528, 784, 0}, {1040, 800, 192}});

    // a door standing in room 3 across x = 1024; the octree keeps it in a large cell, so
    // every search returns it
    auto* standingDoor = new mdl::EntityNode{mdl::Entity{{{"classname", "func_door"}}}};
    standingDoor->addChild(new mdl::BrushNode{
      brushBuilder(map).createCuboid({{1016, 500, 0}, {1032, 520, 64}}, "wall").value()});
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {standingDoor}}});

    const auto spaces = analyzeSpaces(map).value();
    REQUIRE(spaces.spaces.size() == 3);
    REQUIRE(spaces.openings.size() == 2);
    const auto room3 = spaces.spaceAt({784, 592, 24});
    REQUIRE(room3);

    auto doorsFound = 0;
    for (size_t i = 0; i < spaces.openings.size(); ++i)
    {
      const auto& opening = spaces.openings[i];
      const auto details = describeOpening(map, spaces, i);
      if (opening.spaceA == *room3 || opening.spaceB == room3)
      {
        CHECK(details.doors.empty());
      }
      else
      {
        CHECK(details.doors == std::vector<const mdl::EntityNode*>{door});
        ++doorsFound;
      }
    }
    CHECK(doorsFound == 1);
  }

  SECTION("space ids are stable across unrelated edits")
  {
    const auto before = analyzeSpaces(map).value();
    const auto ids = spaceIds(before);

    // add an entity and a brush far away
    addPointEntity(map, "item_health", {256, 64, 0});
    addCuboid(map, {{3000, 3000, 0}, {3064, 3064, 64}});
    const auto unrelated = analyzeSpaces(map).value();
    CHECK(spaceIds(unrelated) == ids);

    // move room 2's east wall inwards
    mdl::removeNodes(map, {eastWall});
    addCuboid(map, {{1008, -16, 0}, {1024, 400, 192}});
    const auto moved = analyzeSpaces(map).value();
    REQUIRE(moved.spaces.size() == 2);
    const auto room1 = moved.spaceAt({64, 192, 24});
    const auto room2 = moved.spaceAt({784, 192, 24});
    REQUIRE(room1);
    REQUIRE(room2);
    CHECK(moved.spaces[*room1].id == before.spaces[*before.spaceAt({64, 192, 24})].id);
    CHECK(moved.spaces[*room2].id != before.spaces[*before.spaceAt({784, 192, 24})].id);
    CHECK(moved.spaces[*room2].bounds == vm::bbox3d{{528, 0, 0}, {1008, 384, 192}});
  }

  SECTION("a leaking room is not sealed")
  {
    mdl::removeNodes(map, {westWall});
    const auto spaces = analyzeSpaces(map).value();
    const auto room1 = spaces.spaceAt({64, 192, 24});
    const auto room2 = spaces.spaceAt({784, 192, 24});
    REQUIRE(room1);
    REQUIRE(room2);
    CHECK_FALSE(spaces.spaces[*room1].sealed);
    CHECK_FALSE(spaces.spaces[*room2].sealed);
    // an opening to the void
    CHECK(std::ranges::any_of(spaces.openings, [&](const auto& opening) {
      return opening.spaceA == *room1 && !opening.spaceB;
    }));
  }

  SECTION("analyzeSpaces fails for too many cells")
  {
    const auto result = analyzeSpaces(map, {.cellSize = 1.0, .maxCells = 1000});
    REQUIRE(result.is_error());
  }

  SECTION("predictLeaks")
  {
    SECTION("sealed map")
    {
      const auto report = predictLeaks(map);
      CHECK(report.analyzed);
      CHECK(report.cellSize == 8.0);
      CHECK(report.checkedEntities == 3);
      CHECK(report.findings.empty());
    }

    SECTION("a removed wall")
    {
      mdl::removeNodes(map, {westWall});
      const auto report = predictLeaks(map);
      CHECK(report.analyzed);
      // all entities are reached, room 2 through the doorway
      CHECK(report.findings.size() == 3);
      const auto it = std::ranges::find_if(report.findings, [&](const auto& finding) {
        return finding.entity == playerStart;
      });
      REQUIRE(it != report.findings.end());
      CHECK(it->position == vm::vec3d{64, 192, 24});
      REQUIRE(it->gap);
      CHECK(it->gap->min.x() >= -32.0);
      CHECK(it->gap->max.x() <= 32.0);
      CHECK_FALSE(it->gapBrushes.empty());
    }

    SECTION("an entity outside the map")
    {
      auto* outside = addPointEntity(map, "light", {2000, 0, 64});
      const auto report = predictLeaks(map);
      REQUIRE(report.findings.size() == 1);
      CHECK(report.findings.front().entity == outside);
      CHECK_FALSE(report.findings.front().gap);
    }

    SECTION("an entity in a cell overlapped by a wall")
    {
      // with 20 unit cells, the cell of the entity overlaps the wall between the rooms
      auto* light = addPointEntity(map, "light", {505, 100, 64});
      CHECK(predictLeaks(map, {.cellSize = 20.0}).findings.empty());

      mdl::removeNodes(map, {westWall});
      const auto report = predictLeaks(map, {.cellSize = 20.0});
      CHECK(std::ranges::any_of(
        report.findings, [&](const auto& finding) { return finding.entity == light; }));
    }

    SECTION("func_detail does not seal")
    {
      auto* detail = new mdl::EntityNode{mdl::Entity{{{"classname", "func_detail"}}}};
      auto* brushNode = new mdl::BrushNode{
        brushBuilder(map).createCuboid({{-16, -16, 0}, {0, 400, 192}}, "wall").value()};
      detail->addChild(brushNode);
      mdl::removeNodes(map, {westWall});
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {detail}}});
      CHECK(predictLeaks(map).findings.size() == 3);
    }

    SECTION("skipped")
    {
      const auto report = predictLeaks(map, {.cellSize = 1.0, .maxCells = 1000});
      CHECK_FALSE(report.analyzed);
      CHECK_FALSE(report.skippedReason.empty());
    }
  }

  SECTION("findFreeSpots")
  {
    const auto spaces = analyzeSpaces(map).value();
    const auto room2 = *spaces.spaceAt({784, 192, 24});

    SECTION("wall")
    {
      const auto result = findFreeSpots(
        map,
        &spaces,
        {.size = {64, 4, 64},
         .placement = Placement::Wall,
         .space = room2,
         .heightAboveFloor = 64.0,
         .limit = 5});
      REQUIRE(result.is_success());
      const auto& spots = result.value().spots;
      REQUIRE(!spots.empty());
      for (const auto& spot : spots)
      {
        REQUIRE(spot.wallBrush);
        CHECK(spot.wallNormal.z() == 0.0);
        CHECK(spot.space == room2);
        CHECK(spot.box.min.z() == Catch::Approx(64.0));
        REQUIRE(spot.floor);
        CHECK(*spot.floor == Catch::Approx(0.0));
        // the box lies flat against the wall
        const auto depth = spot.rotated ? spot.box.size().x() : spot.box.size().y();
        CHECK(depth == Catch::Approx(4.0));
        CHECK(spot.heightRange);
      }
    }

    SECTION("floor")
    {
      const auto result = findFreeSpots(
        map,
        &spaces,
        {.size = {32, 32, 32}, .space = room2, .wallDistance = 16.0, .limit = 4});
      REQUIRE(result.is_success());
      const auto& spots = result.value().spots;
      REQUIRE(spots.size() == 4);
      for (const auto& spot : spots)
      {
        CHECK(spot.box.min.z() == 0.0);
        CHECK(spot.box.min.x() >= 528.0 + 16.0);
        CHECK(spot.box.max.x() <= 1040.0 - 16.0);
        CHECK(spot.clearance[4] == Catch::Approx(0.0));
      }
    }

    SECTION("walls in a group that encloses the spot keep wallDistance")
    {
      // group the world brushes, like rooms built as groups
      auto brushes = std::vector<mdl::Node*>{};
      const auto noNodes = std::vector<mdl::Node*>{};
      for (auto* child : mdl::parentForNodes(map, noNodes).children())
      {
        if (dynamic_cast<mdl::BrushNode*>(child))
        {
          brushes.push_back(child);
        }
      }
      mdl::selectNodes(map, brushes);
      REQUIRE(mdl::groupSelectedNodes(map, "Rooms"));
      mdl::deselectAll(map);
      const auto grouped = analyzeSpaces(map).value();

      const auto result = findFreeSpots(
        map,
        &grouped,
        {.size = {32, 32, 32},
         .space = grouped.spaceAt({784, 192, 24}),
         .wallDistance = 16.0,
         .limit = 4});
      REQUIRE(result.is_success());
      const auto& spots = result.value().spots;
      REQUIRE(spots.size() == 4);
      for (const auto& spot : spots)
      {
        CHECK(spot.box.min.x() >= 528.0 + 16.0);
        CHECK(spot.box.max.x() <= 1040.0 - 16.0);
        CHECK(spot.box.min.y() >= 16.0);
        CHECK(spot.box.max.y() <= 384.0 - 16.0);
      }
    }

    SECTION("near")
    {
      const auto result = findFreeSpots(
        map,
        &spaces,
        {.size = {32, 32, 32},
         .space = spaces.spaceAt({64, 192, 24}),
         .limit = 1,
         .sort = SpotSort::Near,
         .near = vm::vec3d{112, 112, 0}});
      REQUIRE(result.is_success());
      REQUIRE(result.value().spots.size() == 1);
      // not on the chair
      const auto& box = result.value().spots.front().box;
      const auto onChair =
        box.min.x() < 128 && box.max.x() > 96 && box.min.y() < 128 && box.max.y() > 96;
      CHECK_FALSE(onChair);
    }
  }

  SECTION("planWalk")
  {
    const auto result = planWalk(map);
    REQUIRE(result.is_success());
    const auto& plan = result.value();
    REQUIRE(plan.startNode);
    CHECK(plan.nodes[*plan.startNode].z == 0.0);

    // the lowest node below the roof
    const auto nodeAt = [&](const double x, const double y) -> const WalkNode* {
      const auto column = size_t((x - plan.origin.x()) / plan.cellSize);
      const auto row = size_t((y - plan.origin.y()) / plan.cellSize);
      const auto& nodes = plan.columnNodes[column + row * plan.columns];
      return nodes.empty() || plan.nodes[nodes.front()].z > 192.0
               ? nullptr
               : &plan.nodes[nodes.front()];
    };
    // room 2 is reached through the door
    const auto* room2 = nodeAt(784, 192);
    REQUIRE(room2);
    CHECK(room2->reachable);
    CHECK(room2->canReturn);
    // the chair's seat can be stepped on
    const auto* seat = nodeAt(100, 100);
    REQUIRE(seat);
    CHECK(seat->z == 24.0);
    // next to a wall there is no room for the player
    CHECK(nodeAt(8, 192) == nullptr);
  }

  SECTION("timing")
  {
    auto emptyFixture = mdl::MapFixture{};
    auto& bigMap = emptyFixture.create({.gameInfo = mdl::QuakeGameInfo});
    for (int x = 0; x < 5; ++x)
    {
      for (int y = 0; y < 4; ++y)
      {
        const auto min = vm::vec3d{x * 640.0, y * 512.0, 0.0};
        addRoom(bigMap, {min, min + vm::vec3d{512, 384, 192}});
        addPointEntity(bigMap, "light", min + vm::vec3d{256, 192, 128});
      }
    }
    const auto start = std::chrono::steady_clock::now();
    const auto report = predictLeaks(bigMap);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
    CHECK(report.analyzed);
    CHECK(report.findings.empty());
    CHECK(report.checkedEntities == 20);
    UNSCOPED_INFO("predictLeaks on 20 rooms: " << elapsed.count() << " ms");
    // about 10 ms in a debug build
    CHECK(elapsed.count() < 500);

    const auto spacesStart = std::chrono::steady_clock::now();
    const auto spaces = analyzeSpaces(bigMap);
    const auto spacesElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - spacesStart);
    REQUIRE(spaces.is_success());
    CHECK(spaces.value().spaces.size() == 20);
    UNSCOPED_INFO("analyzeSpaces on 20 rooms: " << spacesElapsed.count() << " ms");
    // about 250 ms in a debug build
    CHECK(spacesElapsed.count() < 5000);
  }
}

TEST_CASE("SpaceAnalysis regressions")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create({.gameInfo = mdl::QuakeGameInfo});

  SECTION("wall details next to a door do not split a room")
  {
    // a cafe (0..768 x 0..512 x 0..160) south of a taller dance hall; the double door
    // between them has a trigger in front of it, and the cafe's wall has trims below
    // 48, posters beside the door and a lamp under the ceiling in front of it, which
    // leave only a diagonal link between parts of the cafe's core
    addCuboid(map, {{-16, -464, -16}, {784, 528, 0}});
    addCuboid(map, {{-16, -16, 160}, {784, 528, 176}});
    addCuboid(map, {{80, -464, 224}, {688, -16, 240}});
    addCuboid(map, {{-16, -16, 0}, {0, 528, 160}});
    addCuboid(map, {{768, -16, 0}, {784, 528, 160}});
    addCuboid(map, {{0, 512, 0}, {768, 528, 160}});
    addCuboid(map, {{80, -464, 0}, {96, -16, 224}});
    addCuboid(map, {{672, -464, 0}, {688, -16, 224}});
    addCuboid(map, {{96, -464, 0}, {672, -448, 224}});
    // the wall between the rooms with the door opening 448..544 x 0..112
    addCuboid(map, {{-16, -16, 0}, {448, 0, 240}});
    addCuboid(map, {{544, -16, 0}, {784, 0, 240}});
    addCuboid(map, {{448, -16, 112}, {544, 0, 240}});
    // trims, posters and the lamp
    addCuboid(map, {{0, 0, 0}, {448, 4, 48}});
    addCuboid(map, {{544, 0, 0}, {768, 4, 48}});
    addCuboid(map, {{320, 0, 72}, {432, 2, 136}});
    addCuboid(map, {{560, 0, 56}, {624, 2, 152}});
    addCuboid(map, {{496, 120, 156}, {560, 136, 160}});
    // the door leaves and the trigger
    const auto* leftLeaf =
      addBrushEntity(map, "func_door", {{{448, -10, 0}, {496, -6, 112}}});
    const auto* rightLeaf =
      addBrushEntity(map, "func_door", {{{496, -10, 0}, {544, -6, 112}}});
    addBrushEntity(map, "trigger_multiple", {{{448, -80, 0}, {544, 64, 112}}}, "trigger");

    const auto spaces = analyzeSpaces(map).value();
    const auto cafe = spaces.spaceAt({384, 256, 80});
    const auto hall = spaces.spaceAt({384, -256, 80});
    REQUIRE(cafe);
    REQUIRE(hall);
    CHECK(*cafe != *hall);
    CHECK(spaces.spaces.size() == 2);
    // in front of the door, under the lamp
    CHECK(spaces.spaceAt({496, 56, 100}) == cafe);
    CHECK(spaces.spaceAt({496, 8, 150}) == cafe);

    // one doorway with both door leaves
    REQUIRE(spaces.openings.size() == 1);
    const auto details = describeOpening(map, spaces, 0);
    CHECK(details.kind == "doorway");
    CHECK(details.width == Catch::Approx(96));
    CHECK(details.height == Catch::Approx(112));
    CHECK(details.bottom == Catch::Approx(0));
    CHECK(details.doors.size() == 2);
    CHECK(std::ranges::find(details.doors, leftLeaf) != details.doors.end());
    CHECK(std::ranges::find(details.doors, rightLeaf) != details.doors.end());
  }

  SECTION("walls that meet only at an edge seal a room")
  {
    // the walls leave a 16 x 16 column open in every corner, which touches the room only
    // along a vertical edge
    addCuboid(map, {{-16, -16, -16}, {272, 272, 0}});
    addCuboid(map, {{-16, -16, 128}, {272, 272, 144}});
    addCuboid(map, {{-16, 0, 0}, {0, 256, 128}});
    addCuboid(map, {{256, 0, 0}, {272, 256, 128}});
    addCuboid(map, {{0, -16, 0}, {256, 0, 128}});
    addCuboid(map, {{0, 256, 0}, {256, 272, 128}});
    auto* light = addPointEntity(map, "light", {128, 128, 64});

    const auto spaces = analyzeSpaces(map).value();
    const auto room = spaces.spaceAt({128, 128, 64});
    REQUIRE(room);
    const auto& space = spaces.spaces[*room];
    CHECK(space.sealed);
    CHECK(space.bounds == vm::bbox3d{{0, 0, 0}, {256, 256, 128}});
    CHECK(std::ranges::none_of(spaces.openings, [&](const auto& opening) {
      return opening.spaceA == *room || opening.spaceB == room;
    }));
    // the corner columns are reported separately
    REQUIRE(space.edgeGap);
    CHECK(std::ranges::any_of(
      std::array<vm::vec3d, 4>{
        {{-8, -8, 64}, {264, -8, 64}, {-8, 264, 64}, {264, 264, 64}}},
      [&](const auto& corner) {
        return space.edgeGap->min.x() <= corner.x()
               && corner.x() <= space.edgeGap->max.x()
               && space.edgeGap->min.y() <= corner.y()
               && corner.y() <= space.edgeGap->max.y();
      }));

    const auto report = predictLeaks(map);
    CHECK(report.analyzed);
    CHECK(std::ranges::none_of(
      report.findings, [&](const auto& finding) { return finding.entity == light; }));

    // a real gap is still a leak
    addCuboid(map, {{-16, -16, 0}, {0, 0, 128}});
    addCuboid(map, {{256, -16, 0}, {272, 0, 128}});
    addCuboid(map, {{-16, 256, 0}, {0, 272, 128}});
    mdl::removeNodes(map, {findBrush(map, {{0, 256, 0}, {256, 272, 128}})});
    addCuboid(map, {{0, 256, 0}, {112, 272, 128}});
    addCuboid(map, {{144, 256, 0}, {256, 272, 128}});
    const auto leaking = analyzeSpaces(map).value();
    const auto leakingRoom = leaking.spaceAt({128, 128, 64});
    REQUIRE(leakingRoom);
    CHECK_FALSE(leaking.spaces[*leakingRoom].sealed);
  }

  SECTION("planWalk: steps narrower than the player and low floor tiles")
  {
    // a room 0..512 x 0..384 x 0..192 with a pool (256..448 x 64..320, 80 deep) whose
    // stairs have 16 x 16 steps, and a dance floor of 2 unit high tiles in a checker
    // pattern
    addCuboid(map, {{-16, -16, -16}, {256, 400, 0}});
    addCuboid(map, {{448, -16, -16}, {528, 400, 0}});
    addCuboid(map, {{256, -16, -16}, {448, 64, 0}});
    addCuboid(map, {{256, 320, -16}, {448, 400, 0}});
    addCuboid(map, {{240, 48, -96}, {464, 336, -80}});
    addCuboid(map, {{240, 48, -80}, {256, 336, -16}});
    addCuboid(map, {{448, 48, -80}, {464, 336, -16}});
    addCuboid(map, {{256, 48, -80}, {448, 64, -16}});
    addCuboid(map, {{256, 320, -80}, {448, 336, -16}});
    for (int step = 0; step < 4; ++step)
    {
      const auto x = 256.0 + step * 16.0;
      addCuboid(map, {{x, 64, -80}, {x + 16, 128, -16.0 - step * 16.0}});
    }
    addCuboid(map, {{-16, -16, 192}, {528, 400, 208}});
    addCuboid(map, {{-16, -16, 0}, {0, 400, 192}});
    addCuboid(map, {{512, -16, 0}, {528, 400, 192}});
    addCuboid(map, {{0, -16, 0}, {512, 0, 192}});
    addCuboid(map, {{0, 384, 0}, {512, 400, 192}});
    for (int i = 0; i < 6; ++i)
    {
      for (int j = 0; j < 6; ++j)
      {
        if ((i + j) % 2 == 0)
        {
          const auto min = vm::vec3d{32.0 + i * 32.0, 96.0 + j * 32.0, 0.0};
          addCuboid(map, {min, min + vm::vec3d{32, 32, 2}});
        }
      }
    }

    const auto plan = planWalk(map, {.start = vm::vec3d{64, 48, 24}}).value();
    REQUIRE(plan.startNode);
    auto unreachable = std::vector<vm::vec3d>{};
    for (const auto& node : plan.nodes)
    {
      if (node.z < 100.0 && (!node.reachable || !node.canReturn))
      {
        unreachable.push_back(plan.position(node));
      }
    }
    CHECK(unreachable.empty());

    const auto nodesAt = [&](const double x, const double y) {
      const auto column = size_t((x - plan.origin.x()) / plan.cellSize);
      const auto row = size_t((y - plan.origin.y()) / plan.cellSize);
      auto result = std::vector<double>{};
      for (const auto index : plan.columnNodes[column + row * plan.columns])
      {
        // below the roof
        if (plan.nodes[index].z < 100.0)
        {
          result.push_back(plan.nodes[index].z);
        }
      }
      return result;
    };
    // the player stands on the highest surface under its box
    CHECK(nodesAt(352, 200) == std::vector<double>{-80});
    CHECK(nodesAt(280, 100) == std::vector<double>{-16});
    CHECK(nodesAt(72, 104) == std::vector<double>{2});
  }
}

} // namespace tb::mcp
