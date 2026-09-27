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
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <algorithm>
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

} // namespace tb::mcp
