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
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/AgentCamera.h"
#include "mcp/CameraProjection.h"
#include "mcp/JsonVm.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameManager.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <optional>
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

/** Adds a box with the given material and a non-default alignment to the parent. */
mdl::BrushNode* addBox(
  mdl::Map& map, const vm::bbox3d& bounds, const std::string& material, mdl::Node* parent)
{
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, material)
                 .value();
  auto* brushNode = new mdl::BrushNode{std::move(brush)};
  mdl::addNodes(map, {{parent, {brushNode}}});
  mdl::deselectAll(map);
  mdl::selectNodes(map, {brushNode});
  REQUIRE(mdl::setBrushFaceAttributes(
    map,
    {
      .xOffset = mdl::SetValue{12.0f},
      .yOffset = mdl::SetValue{-3.0f},
      .rotation = mdl::SetValue{45.0f},
      .xScale = mdl::SetValue{0.25f},
      .yScale = mdl::SetValue{1.5f},
    }));
  mdl::deselectAll(map);
  return brushNode;
}

std::vector<std::string> materialsOf(const mdl::BrushNode& brushNode)
{
  auto result = std::vector<std::string>{};
  for (const auto& face : brushNode.brush().faces())
  {
    result.push_back(face.materialName());
  }
  return result;
}

using Alignment = std::tuple<mdl::UvAttributes, vm::vec3d, vm::vec3d>;

std::vector<Alignment> alignmentOf(const mdl::BrushNode& brushNode)
{
  auto result = std::vector<Alignment>{};
  for (const auto& face : brushNode.brush().faces())
  {
    result.emplace_back(face.uvAttributes(), face.uAxis(), face.vAxis());
  }
  return result;
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

TEST_CASE("Scenario S4")
{
  // "Insert the 'Armory' group from prefabs/rooms.map next to the east wall of the
  // selected room."
  auto fixture = McpToolFixture{};
  fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Standard"}});
  auto& map = fixture.host().documentList.back().document->map();
  mdl::selectAllNodes(map);
  mdl::removeSelectedNodes(map);

  // the WAD has wall_new_a, but not armory_metal
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});

  // the human works in the "Level" layer
  auto* level = new mdl::LayerNode{mdl::Layer{"Level"}};
  mdl::addNodes(map, {{&map.worldNode(), {level}}});
  mdl::setCurrentLayer(map, level);

  // the selected room, and a pillar outside its east wall at the south end
  const auto room = resultOf(fixture.call(
    "room_create",
    Json{
      {"min", {0, 0, 0}},
      {"max", {512, 384, 192}},
      {"thickness", 16},
      {"material", "wall_old_a"},
      {"group", "Room"},
    }));
  const auto roomId = room["group"].get<std::string>();
  auto* pillar = addBox(map, {{528, 0, 0}, {592, 64, 192}});
  fixture.call("selection_set", Json{{"ids", {roomId}}});

  // the agent reads the bounds of the selected room: its east wall ends at x = 528
  const auto selection = fixture.call("selection_get");
  REQUIRE(selection["count"] == 1);
  const auto roomBounds = *boxFromJson(selection["bounds"]);
  CHECK(roomBounds.max.x() == 528);

  // the agent lists the groups in the other map
  const auto path = (getFixtureRoot() / "test" / "mcp" / "maps" / "rooms.map").string();
  const auto contents = fixture.call("map_file_inspect", Json{{"path", path}});
  CHECK(contents["format"] == "Valve");
  CHECK(contents["converted"] == true);
  const auto& groups = contents["groups"];
  const auto armory = std::ranges::find_if(
    groups, [](const auto& group) { return group["name"] == "Armory"; });
  REQUIRE(armory != groups.end());
  const auto armoryBounds = *boxFromJson((*armory)["bounds"]);
  const auto size = armoryBounds.size();

  // it tries places along the east wall, bottom aligned with the room's, until one is
  // free: the south end is blocked by the pillar
  auto position = std::optional<vm::vec3d>{};
  for (const auto y : {roomBounds.min.y(), roomBounds.max.y() - size.y()})
  {
    const auto min = vm::vec3d{roomBounds.max.x(), y, roomBounds.min.z()};
    const auto check =
      fixture.call("space_check", Json{{"box", toJson(vm::bbox3d{min, min + size})}});
    if (check["free"] == true)
    {
      position = min;
      break;
    }
    CHECK(check["overlaps"][0]["id"] == fixture.id(*pillar));
  }
  REQUIRE(position);
  CHECK(*position == vm::vec3d{528, 208, -16});

  // it imports the group there
  const auto imported = fixture.call(
    "map_import",
    Json{
      {"path", path},
      {"group", "Armory"},
      {"position", toJson(*position)},
      {"anchor", "min"},
    });
  CHECK(imported["undoStep"] == "AI: Import Map");
  const auto& result = resultOf(imported);
  CHECK(result["sourceFormat"] == "Valve");
  CHECK(result["documentFormat"] == "Standard");
  CHECK(result["converted"] == true);
  CHECK(result["bounds"] == toJson(vm::bbox3d{*position, *position + size}));

  // the missing material is reported
  CHECK(result["missingMaterials"] == Json{"armory_metal"});
  CHECK(hasWarning(imported, "MISSING_MATERIALS"));

  // the imported group is selected and in the current layer
  REQUIRE(result["ids"].size() == 1);
  auto* group =
    dynamic_cast<mdl::GroupNode*>(fixture.node(result["ids"][0].get<std::string>()));
  REQUIRE(group);
  CHECK(group->name() == "Armory");
  CHECK(group->parent() == level);
  CHECK(result["layer"] == fixture.id(*level));
  CHECK(map.selection().nodes == std::vector<mdl::Node*>{group});

  // it overlaps no existing brush: whatever overlaps its bounds belongs to it
  const auto check = fixture.call("space_check", Json{{"box", result["bounds"]}});
  CHECK(check["overlapCount"].get<size_t>() > 0);
  for (const auto& overlap : check["overlaps"])
  {
    CHECK(fixture.node(overlap["id"].get<std::string>())->isDescendantOf(*group));
  }

  // the brushes are in the Standard format: no UV axes
  const auto text = fixture.call("map_text_get", Json{{"ids", result["ids"]}});
  CHECK(text["text"].get<std::string>().find('[') == std::string::npos);

  // one undo step removes the import
  fixture.call("undo");
  CHECK(
    fixture.callExpectingError("object_get", Json{{"ids", result["ids"]}}).code
    == ErrorCode::ObjectNotFound);
}

TEST_CASE("Scenario S3")
{
  // "Replace every wall_old* material with the matching wall_new* one, only in the
  // 'Castle' layer."
  auto fixture = McpToolFixture{};
  fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
  auto& map = fixture.host().documentList.back().document->map();

  // the WAD has wall_old_a/b/c and wall_new_a/b, but no wall_new_c
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});

  auto* castle = new mdl::LayerNode{mdl::Layer{"Castle"}};
  auto* village = new mdl::LayerNode{mdl::Layer{"Village"}};
  mdl::addNodes(map, {{&map.worldNode(), {castle, village}}});

  // the castle: a wall with wall_old_a on four faces and wall_old_b on two, a tower with
  // wall_old_c, and a floor; the village has a wall_old_a house
  auto* wall = addBox(map, {{0, 0, 0}, {256, 16, 128}}, "wall_old_a", castle);
  auto* tower = addBox(map, {{0, 64, 0}, {64, 128, 256}}, "wall_old_c", castle);
  auto* castleFloor = addBox(map, {{0, 0, -16}, {256, 256, 0}}, "floor_tile", castle);
  auto* house = addBox(map, {{512, 0, 0}, {640, 128, 128}}, "wall_old_a", village);
  mdl::selectBrushFaces(map, {{wall, 0}, {wall, 1}});
  REQUIRE(mdl::setBrushFaceAttributes(map, {.materialName = "wall_old_b"}));
  mdl::deselectAll(map);

  const auto wallMaterials = materialsOf(*wall);
  const auto alignments = std::vector{
    alignmentOf(*wall),
    alignmentOf(*tower),
    alignmentOf(*castleFloor),
    alignmentOf(*house)};

  // the agent checks which targets exist
  const auto targets = fixture.call("materials_list", Json{{"search", "wall_new*"}});
  CHECK(targets["total"] == 2);

  const auto replace = fixture.call(
    "material_replace",
    Json{{"from", "wall_old*"}, {"to", "wall_new*"}, {"layer", "Castle"}});
  CHECK(replace["undoStep"] == "AI: Replace Materials");

  const auto& result = replace["result"];
  CHECK(result["scope"]["kind"] == "layer");
  CHECK(
    result["scope"]["layer"] == Json{{"id", fixture.id(*castle)}, {"name", "Castle"}});
  CHECK(result["scope"]["faces"] == 18);

  // how many faces changed per material
  CHECK(
    result["replaced"]
    == Json{
      {{"from", "wall_old_a"}, {"to", "wall_new_a"}, {"faces", 4}},
      {{"from", "wall_old_b"}, {"to", "wall_new_b"}, {"faces", 2}},
    });
  CHECK(result["totalFaces"] == 6);

  // the material without a match is reported and left alone, not guessed
  CHECK(
    result["unmatched"]
    == Json{{{"from", "wall_old_c"}, {"to", "wall_new_c"}, {"faces", 6}}});
  CHECK(materialsOf(*tower) == std::vector<std::string>(6, "wall_old_c"));

  // only the castle changed
  for (size_t i = 0; i < 6; ++i)
  {
    CHECK(
      wall->brush().face(i).materialName()
      == (wallMaterials[i] == "wall_old_a" ? "wall_new_a" : "wall_new_b"));
  }
  CHECK(materialsOf(*castleFloor) == std::vector<std::string>(6, "floor_tile"));
  CHECK(materialsOf(*house) == std::vector<std::string>(6, "wall_old_a"));
  CHECK(replace["changes"]["modified"] == Json{fixture.id(*wall)});

  // the alignment is preserved
  CHECK(
    std::vector{
      alignmentOf(*wall),
      alignmentOf(*tower),
      alignmentOf(*castleFloor),
      alignmentOf(*house)}
    == alignments);

  // one undo reverts the whole change
  fixture.call("undo");
  CHECK(materialsOf(*wall) == wallMaterials);
  CHECK(
    fixture.call("history_get", Json{{"limit", 1}})["redo"][0]["name"]
    == "AI: Replace Materials");
}

TEST_CASE("Scenario E12")
{
  // Two rooms and a doorway: pick the chair in a snapshot, list both spaces with their
  // doorway, find a wall spot for a poster, and get z-fighting and an entity outside
  // the hull reported right after the calls that caused them.
  auto fixture = McpToolFixture{};
  const auto* gameInfo = fixture.host().gameManager().gameInfo("Quake");
  REQUIRE(gameInfo);
  fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "spaces.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = *gameInfo});

  // the chair's seat is the brush from (96 96 0) to (128 128 24) in the Chair group
  const auto shot = fixture.call(
    "view_snapshot",
    Json{
      {"camera", Json{{"position", {40, 40, 96}}, {"lookAt", {112, 112, 12}}}},
      {"width", 320},
      {"height", 240},
    });
  auto camera = AgentCamera{};
  camera.position = *vec3FromJson(shot["camera"]["position"]);
  camera.direction = *vec3FromJson(shot["camera"]["direction"]);
  camera.up = *vec3FromJson(shot["camera"]["up"]);
  camera.fov = shot["camera"]["fov"].get<double>();
  camera.nearPlane = shot["camera"]["near"].get<double>();
  camera.farPlane = shot["camera"]["far"].get<double>();
  const auto projection = ImageProjection::create(camera, 320, 240) | kdl::value();
  const auto seatTop = projection.project(vm::vec3d{108, 108, 24});
  REQUIRE(seatTop.inFront);

  const auto pick = fixture.call(
    "view_pick",
    Json{
      {"snapshot", shot["snapshotId"]},
      {"pixel", {{"x", int64_t(seatTop.x)}, {"y", int64_t(seatTop.y)}}},
    });
  const auto& hit = pick["hit"];
  REQUIRE(hit.is_object());
  auto* seat = fixture.node(hit["object"].get<std::string>());
  REQUIRE(seat);
  CHECK(seat->logicalBounds() == vm::bbox3d{{96, 96, 0}, {128, 128, 24}});
  CHECK(hit["face"].get<std::string>().starts_with(hit["object"].get<std::string>()));
  CHECK(*vec3FromJson(hit["normal"]) == vm::vec3d{0, 0, 1});
  auto* chair = fixture.node(hit["group"].get<std::string>());
  REQUIRE(dynamic_cast<mdl::GroupNode*>(chair));
  CHECK(static_cast<mdl::GroupNode*>(chair)->name() == "Chair");

  // both spaces with their doorway
  const auto spaces = fixture.call("spaces_list");
  REQUIRE(spaces["count"] == 2);
  const auto& opening = spaces["openings"];
  REQUIRE(opening.size() == 1);
  CHECK(opening[0]["kind"] == "doorway");
  CHECK(opening[0]["width"] == 64);
  CHECK(opening[0]["height"] == 112);
  const auto spaceIds = std::vector{spaces["spaces"][0]["id"], spaces["spaces"][1]["id"]};
  CHECK(std::ranges::find(spaceIds, opening[0]["spaces"][0]) != spaceIds.end());
  CHECK(std::ranges::find(spaceIds, opening[0]["spaces"][1]) != spaceIds.end());

  // a free wall spot for a poster in the second room
  const auto& room2 = spaces["spaces"][0]["bounds"]["min"][0].get<double>() > 256
                        ? spaces["spaces"][0]
                        : spaces["spaces"][1];
  const auto spots = fixture.call(
    "free_spots",
    Json{
      {"size", {64, 4, 64}},
      {"placement", "wall"},
      {"space", room2["id"]},
      {"heightAboveFloor", 64},
    });
  REQUIRE(spots["count"].get<size_t>() > 0);
  const auto& wall = spots["spots"][0]["wall"];
  const auto wallFace = wall["face"].get<std::string>();
  CHECK(wallFace.find("/face:") != std::string::npos);
  const auto normal = *vec3FromJson(wall["normal"]);
  CHECK(vm::abs(normal.z()) == 0.0);
  CHECK(fixture.node(wall["brush"].get<std::string>()) != nullptr);

  // z-fighting: a slab whose top lies in the floor plane of the first room
  const auto slab = fixture.call(
    "brush_create_box", Json{{"min", {200, 200, -8}}, {"max", {264, 264, 0}}});
  auto zfighting = std::vector<Json>{};
  for (const auto& issue : slab["issuesIntroduced"])
  {
    if (issue["code"] == "Z_FIGHTING")
    {
      zfighting.push_back(issue);
    }
  }
  REQUIRE(zfighting.size() == 1);
  CHECK(zfighting[0]["details"]["faces"].size() == 2);

  // an entity outside the hull
  const auto outside = fixture.call(
    "entity_create_point", Json{{"classname", "light"}, {"position", {2000, 2000, 64}}});
  const auto entityId = outside["result"]["entity"];
  CHECK(std::ranges::any_of(outside["issuesIntroduced"], [&](const auto& issue) {
    return issue["code"] == "ENTITY_OUTSIDE_HULL" && issue["objectId"] == entityId;
  }));
}

TEST_CASE("Scenario S2")
{
  // Open a map with problems, report every issue, apply the fixes that resolve them and
  // list what remains with reasons. The fixes are named undo steps.
  auto fixture = McpToolFixture{};
  fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "issues.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  fixture.call(
    "entity_definitions_set",
    {{"type", "external"},
     {"path", (getFixtureRoot() / "test" / "mcp" / "models.fgd").string()}});

  // every issue has a type, an object and an explanation
  const auto before = fixture.call("issues_list");
  const auto total = before["total"].get<size_t>();
  REQUIRE(total >= 10);
  for (const auto& item : before["items"])
  {
    CHECK(!item["type"].get<std::string>().empty());
    CHECK(!item["objectId"].get<std::string>().empty());
    CHECK(!item["description"].get<std::string>().empty());
  }

  // fix every issue that has exactly one fix, one call per code; deleting entities
  // because the definition file lacks their class is not safe
  auto codes = std::vector<std::string>{};
  for (const auto& item : before["items"])
  {
    const auto code = item["code"].get<std::string>();
    if (
      item["fixes"].size() == 1 && code != "MISSING_ENTITY_DEFINITION"
      && std::ranges::find(codes, code) == codes.end())
    {
      codes.push_back(code);
    }
  }
  REQUIRE(codes.size() >= 8);

  auto removed = std::vector<std::string>{};
  auto modified = std::vector<std::string>{};
  for (const auto& code : codes)
  {
    const auto result = fixture.call("issue_fix", Json{{"codes", {code}}});
    INFO(code);
    CHECK(result["result"]["fixedCount"].get<size_t>() > 0);
    CHECK(result["result"]["notFixedCount"] == 0);
    CHECK(result["undoStep"] == "AI: Fix Issues");
    // the agent can report every object it deleted or changed
    for (const auto& id : result["changes"]["removed"])
    {
      removed.push_back(id.get<std::string>());
    }
    for (const auto& id : result["changes"]["modified"])
    {
      modified.push_back(id.get<std::string>());
    }
  }
  CHECK(!removed.empty());
  CHECK(!modified.empty());

  // the issue count dropped; the remaining issues are listed with reasons
  const auto after = fixture.call("issues_list");
  CHECK(after["total"].get<size_t>() < total);
  REQUIRE(after["total"].get<size_t>() > 0);
  for (const auto& item : after["items"])
  {
    CHECK((item["fixes"].empty() || item["code"] == "MISSING_ENTITY_DEFINITION"));
    CHECK(!item["description"].get<std::string>().empty());
  }
  CHECK(after["counts"].contains("Z_FIGHTING"));

  const auto remaining = fixture.call("issue_fix", Json{{"codes", {"Z_FIGHTING"}}});
  CHECK(remaining["result"]["fixedCount"] == 0);
  CHECK(!remaining["result"]["notFixed"][0]["reason"].get<std::string>().empty());

  // one undo step per fix call
  const auto history = fixture.call("history_get");
  const auto fixSteps = std::ranges::count_if(
    history["undo"], [](const auto& step) { return step["name"] == "AI: Fix Issues"; });
  CHECK(size_t(fixSteps) == codes.size());
}

} // namespace tb::mcp
