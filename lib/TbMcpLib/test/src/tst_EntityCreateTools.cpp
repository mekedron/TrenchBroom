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
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Grid.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
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

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

/** Creates a document through document_new, which loads the game's definitions. */
ui::MapDocument& newDocument(McpToolFixture& fixture, const std::string& game)
{
  const auto result = fixture.call("document_new", Json{{"game", game}});
  const auto id = resultOf(result)["document"]["id"].get<std::string>();
  for (const auto& info : fixture.host().documentList)
  {
    if (info.id == id)
    {
      return *info.document;
    }
  }
  FAIL("document not found");
  return *fixture.host().documentList.front().document;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

std::vector<std::string> selectedIds(McpToolFixture& fixture, mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto* node : map.selection().nodes)
  {
    result.push_back(fixture.id(*node));
  }
  std::ranges::sort(result);
  return result;
}

std::vector<std::string> sorted(std::vector<std::string> ids)
{
  std::ranges::sort(ids);
  return ids;
}

const mdl::EntityNode& entityNode(McpToolFixture& fixture, const Json& id)
{
  auto* node = dynamic_cast<mdl::EntityNode*>(fixture.node(id.get<std::string>()));
  REQUIRE(node);
  return *node;
}

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return resultOf(
           fixture.call("brush_create_box", Json{{"min", min}, {"max", max}}))["brush"]
    .get<std::string>();
}

} // namespace

TEST_CASE("EntityCreateTools")
{
  auto fixture = McpToolFixture{};
  auto& document = newDocument(fixture, "Quake");
  auto& map = document.map();
  REQUIRE(map.grid().actualSize() == 16);

  // a room whose floor surface is at z = 0 and whose ceiling is at z = 256; the new
  // document also contains the default brush around the origin, so entities are placed
  // away from it
  const auto room = fixture.call(
    "room_create", Json{{"min", {0, 0, 0}}, {"max", {512, 512, 256}}, {"thickness", 16}});
  const auto floorId = resultOf(room)["brushes"]["floor"].get<std::string>();

  SECTION("entity_create_point")
  {
    SECTION("info_player_start dropped onto the floor")
    {
      const auto created = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "info_player_start"},
          {"position", {192, 192, 100}},
          {"dropToFloor", true},
          {"angle", 90}});
      const auto& result = resultOf(created);
      const auto id = result["entity"].get<std::string>();
      const auto& node = entityNode(fixture, result["entity"]);

      CHECK(node.entity().classname() == "info_player_start");
      CHECK(node.entity().origin() == vm::vec3d{192, 192, 24});
      CHECK(result["origin"] == Json{192, 192, 24});
      CHECK(result["bounds"]["min"] == Json{176, 176, 0});
      CHECK(result["floor"]["z"] == 0);
      CHECK(result["floor"]["object"] == floorId);
      CHECK(result["floor"]["face"].get<std::string>().starts_with(floorId + "/face:"));
      CHECK(result["floor"]["distance"] == 76);
      CHECK(result["onFloor"] == true);
      CHECK(result["overlaps"].empty());
      CHECK(result["properties"]["angle"] == "90");
      CHECK(result["properties"]["origin"] == "192 192 24");
      CHECK(!hasWarning(created, "ENTITY_OVERLAPS_BRUSHES"));

      CHECK(created["undoStep"] == "AI: Create Point Entity");
      CHECK(created["changes"]["created"] == Json{id});
      CHECK(selectedIds(fixture, map) == std::vector{id});
      CHECK(node.parent() == map.editorContext().currentLayer());

      fixture.call("undo");
      CHECK(fixture.node(id) == nullptr);
    }

    SECTION("monster_ogre rests on a floor that is higher than the position")
    {
      // a platform: the ogre must stand on its top, not on the room floor
      createBox(fixture, {192, 192, 0}, {320, 320, 32});
      const auto created = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "monster_ogre"},
          {"position", {256, 256, 128}},
          {"dropToFloor", true}});
      const auto& result = resultOf(created);
      CHECK(result["origin"] == Json{256, 256, 56});
      CHECK(result["bounds"]["min"] == Json{224, 224, 32});
      CHECK(result["floor"]["z"] == 32);
      CHECK(result["onFloor"] == true);
      CHECK(result["overlaps"].empty());
    }

    SECTION("snapping")
    {
      const auto snapped = fixture.call(
        "entity_create_point",
        Json{{"classname", "info_player_start"}, {"position", {193, 198, 57}}});
      CHECK(resultOf(snapped)["origin"] == Json{192, 192, 64});
      CHECK(resultOf(snapped)["floor"].is_null());
      CHECK(resultOf(snapped)["onFloor"] == false);

      const auto unsnapped = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "info_player_start"},
          {"position", {193, 198, 24}},
          {"snapToGrid", false}});
      CHECK(resultOf(unsnapped)["origin"] == Json{193, 198, 24});
      CHECK(resultOf(unsnapped)["onFloor"] == true);

      // dropping keeps the exact floor height although z is not on the grid
      const auto dropped = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "monster_ogre"},
          {"position", {130, 250, 100}},
          {"dropToFloor", true}});
      CHECK(resultOf(dropped)["origin"] == Json{128, 256, 24});
    }

    SECTION("unknown classname")
    {
      const auto created = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "my_custom_thing"},
          {"position", {128, 128, 64}},
          {"properties", {{"health", 5}}}});
      const auto& result = resultOf(created);
      CHECK(hasWarning(created, "UNKNOWN_CLASSNAME"));
      const auto& node = entityNode(fixture, result["entity"]);
      CHECK(node.entity().classname() == "my_custom_thing");
      CHECK(node.entity().origin() == vm::vec3d{128, 128, 64});
      CHECK(result["properties"]["health"] == "5");
      CHECK(
        selectedIds(fixture, map) == std::vector{result["entity"].get<std::string>()});

      // the default bounds (16 units) are used to drop it
      const auto dropped = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "my_custom_thing"},
          {"position", {128, 128, 64}},
          {"dropToFloor", true}});
      CHECK(resultOf(dropped)["origin"] == Json{128, 128, 8});
    }

    SECTION("properties")
    {
      const auto created = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "light"},
          {"position", {128, 128, 128}},
          {"properties",
           {{"light", 250},
            {"wait", 1.5},
            {"_color", {255, 128, 0}},
            {"style", 99},
            {"bogus_key", "x"},
            {"_sunlight_dirt", true}}}});
      const auto& properties = resultOf(created)["properties"];
      CHECK(properties["light"] == "250");
      CHECK(properties["wait"] == "1.5");
      CHECK(properties["_color"] == "255 128 0");
      CHECK(properties["style"] == "99");
      CHECK(properties["bogus_key"] == "x");
      CHECK(properties["_sunlight_dirt"] == "1");
      CHECK(properties["origin"] == "128 128 128");
      CHECK(hasWarning(created, "INVALID_CHOICE"));
      CHECK(hasWarning(created, "UNKNOWN_PROPERTY"));
      CHECK(created["undoStep"] == "AI: Create Point Entity");

      const auto defaults = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "light"},
          {"position", {256, 128, 128}},
          {"properties", {{"light", 200}}},
          {"applyDefaults", true}});
      CHECK(resultOf(defaults)["properties"]["light"] == "200");
      CHECK(resultOf(defaults)["properties"]["wait"] == "1");
      CHECK(resultOf(defaults)["properties"]["style"] == "0");
    }

    SECTION("overlap warning")
    {
      const auto created = fixture.call(
        "entity_create_point",
        Json{{"classname", "info_player_start"}, {"position", {192, 192, 0}}});
      CHECK(hasWarning(created, "ENTITY_OVERLAPS_BRUSHES"));
      CHECK(resultOf(created)["overlaps"] == Json{floorId});
      CHECK(resultOf(created)["onFloor"] == false);

      // a trigger does not count
      const auto trigger = createBox(fixture, {256, 256, 0}, {320, 320, 64});
      fixture.call(
        "entity_create_brush", Json{{"classname", "trigger_once"}, {"ids", {trigger}}});
      const auto inTrigger = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "info_player_start"},
          {"position", {288, 288, 100}},
          {"dropToFloor", true}});
      CHECK(!hasWarning(inTrigger, "ENTITY_OVERLAPS_BRUSHES"));
      CHECK(resultOf(inTrigger)["origin"] == Json{288, 288, 24});
    }

    SECTION("outside the world bounds")
    {
      const auto created = fixture.call(
        "entity_create_point",
        Json{{"classname", "info_null"}, {"position", {0, 0, 1000000}}});
      CHECK(hasWarning(created, "OUTSIDE_WORLD_BOUNDS"));
    }

    SECTION("errors")
    {
      const auto brushClass = fixture.callExpectingError(
        "entity_create_point",
        Json{{"classname", "func_door"}, {"position", {64, 64, 64}}});
      CHECK(brushClass.code == ErrorCode::InvalidArgument);
      CHECK(brushClass.hint.find("entity_create_brush") != std::string::npos);

      const auto noFloor = fixture.callExpectingError(
        "entity_create_point",
        Json{
          {"classname", "info_player_start"},
          {"position", {2048, 2048, 64}},
          {"dropToFloor", true}});
      CHECK(noFloor.code == ErrorCode::InvalidArgument);
      CHECK(noFloor.message.find("no floor") != std::string::npos);

      CHECK(
        fixture
          .callExpectingError(
            "entity_create_point",
            Json{
              {"classname", "light"},
              {"position", {64, 64, 64}},
              {"properties", {{"classname", "light"}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_create_point",
            Json{
              {"classname", "light"},
              {"position", {64, 64, 64}},
              {"properties", {{"message", "say \"hi\""}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_create_point",
            Json{
              {"classname", "light"},
              {"position", {64, 64, 64}},
              {"properties", {{"origin", "0 0 0"}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_create_point",
            Json{
              {"classname", "light"},
              {"position", {64, 64, 64}},
              {"properties", {{"target", {{"a", 1}}}}}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("dry run")
    {
      const auto before = selectedIds(fixture, map);
      const auto dryRun = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "monster_ogre"},
          {"position", {128, 128, 128}},
          {"dropToFloor", true},
          {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(dryRun["changes"]["created"].size() == 1);
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(resultOf(dryRun)["origin"] == Json{128, 128, 24});
      CHECK(fixture.node(resultOf(dryRun)["entity"].get<std::string>()) == nullptr);
      CHECK(selectedIds(fixture, map) == before);
    }
  }

  SECTION("entity_create_brush")
  {
    const auto door1 = createBox(fixture, {64, 0, 0}, {80, 64, 128});
    const auto door2 = createBox(fixture, {80, 0, 0}, {96, 64, 128});

    SECTION("func_door from explicit ids")
    {
      const auto created = fixture.call(
        "entity_create_brush",
        Json{
          {"classname", "func_door"},
          {"ids", {door1, door2}},
          {"properties", {{"speed", 200}, {"angle", -1}}}});
      const auto& result = resultOf(created);
      const auto id = result["entity"].get<std::string>();
      const auto& node = entityNode(fixture, result["entity"]);

      CHECK(node.entity().classname() == "func_door");
      CHECK(node.children().size() == 2);
      CHECK(fixture.node(door1)->parent() == &node);
      CHECK(result["brushes"] == Json{door1, door2});
      CHECK(result["bounds"] == Json{{"min", {64, 0, 0}}, {"max", {96, 64, 128}}});
      CHECK(result["properties"]["speed"] == "200");
      CHECK(result["properties"]["angle"] == "-1");
      CHECK(result["removedEntities"].empty());
      CHECK(created["undoStep"] == "AI: Create Brush Entity");
      CHECK(
        std::ranges::find(created["changes"]["created"], Json(id))
        != created["changes"]["created"].end());
      CHECK(selectedIds(fixture, map) == sorted({door1, door2}));

      SECTION("turning all brushes of an entity into another removes it")
      {
        const auto trigger = fixture.call(
          "entity_create_brush",
          Json{{"classname", "trigger_multiple"}, {"ids", {door1, door2}}});
        CHECK(resultOf(trigger)["removedEntities"] == Json{id});
        CHECK(fixture.node(id) == nullptr);
      }
    }

    SECTION("null removes keys, including defaults set on creation")
    {
      // games like Half-Life set the definition's defaults on new entities
      map.worldNode().entityPropertyConfig().setDefaultProperties = true;

      const auto created = fixture.call(
        "entity_create_brush",
        Json{
          {"classname", "func_door"},
          {"ids", {door1, door2}},
          {"properties",
           {{"speed", 200}, {"wait", nullptr}, {"lip", nullptr}, {"message", nullptr}}}});
      const auto& properties = resultOf(created)["properties"];
      CHECK(properties["speed"] == "200");
      CHECK(properties["dmg"] == "2");
      CHECK_FALSE(properties.contains("wait"));
      CHECK_FALSE(properties.contains("lip"));
      CHECK_FALSE(properties.contains("message"));
      const auto& node = entityNode(fixture, resultOf(created)["entity"]);
      CHECK(node.entity().property("wait") == nullptr);

      const auto point = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "light"},
          {"position", {256, 128, 128}},
          {"properties", {{"style", nullptr}, {"wait", nullptr}}}});
      CHECK_FALSE(resultOf(point)["properties"].contains("style"));
      CHECK_FALSE(resultOf(point)["properties"].contains("wait"));
      CHECK(resultOf(point)["properties"]["light"] == "300");

      map.worldNode().entityPropertyConfig().setDefaultProperties = false;
      const auto withDefaults = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "light"},
          {"position", {256, 256, 128}},
          {"properties", {{"style", nullptr}}},
          {"applyDefaults", true}});
      CHECK_FALSE(resultOf(withDefaults)["properties"].contains("style"));
      CHECK(resultOf(withDefaults)["properties"]["wait"] == "1");
    }

    SECTION("trigger_once from the selection")
    {
      fixture.call("selection_set", Json{{"ids", {door1}}});
      const auto created =
        fixture.call("entity_create_brush", Json{{"classname", "trigger_once"}});
      const auto& node = entityNode(fixture, resultOf(created)["entity"]);
      CHECK(node.entity().classname() == "trigger_once");
      CHECK(resultOf(created)["brushes"] == Json{door1});
      CHECK(fixture.node(door2)->parent() == map.editorContext().currentLayer());
    }

    SECTION("unknown classname")
    {
      const auto created = fixture.call(
        "entity_create_brush",
        Json{
          {"classname", "func_custom"},
          {"ids", {door1}},
          {"properties", {{"speed", 10}}}});
      CHECK(hasWarning(created, "UNKNOWN_CLASSNAME"));
      const auto& node = entityNode(fixture, resultOf(created)["entity"]);
      CHECK(node.entity().classname() == "func_custom");
      CHECK(node.entity().property("speed") != nullptr);
      CHECK(fixture.node(door1)->parent() == &node);
    }

    SECTION("errors")
    {
      const auto pointClass = fixture.callExpectingError(
        "entity_create_brush", Json{{"classname", "light"}, {"ids", {door1}}});
      CHECK(pointClass.code == ErrorCode::InvalidArgument);
      CHECK(pointClass.hint.find("entity_create_point") != std::string::npos);

      const auto light = resultOf(fixture.call(
        "entity_create_point",
        Json{{"classname", "light"}, {"position", {128, 128, 128}}}))["entity"];
      fixture.call("selection_set", Json{{"ids", {door1, light}}});
      const auto wrongKind = fixture.callExpectingError(
        "entity_create_brush", Json{{"classname", "func_wall"}});
      CHECK(wrongKind.code == ErrorCode::WrongObjectKind);

      fixture.call("selection_clear");
      CHECK(
        fixture
          .callExpectingError("entity_create_brush", Json{{"classname", "func_wall"}})
          .code
        == ErrorCode::NoSelection);
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "entity_create_brush",
        Json{{"classname", "func_door"}, {"ids", {door1}}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(fixture.node(door1)->parent() == map.editorContext().currentLayer());
    }
  }

  SECTION("entity_move_brushes")
  {
    const auto brush1 = createBox(fixture, {64, 0, 0}, {80, 64, 128});
    const auto brush2 = createBox(fixture, {80, 0, 0}, {96, 64, 128});
    const auto door = resultOf(fixture.call(
      "entity_create_brush",
      Json{{"classname", "func_door"}, {"ids", {brush1}}}))["entity"]
                        .get<std::string>();
    fixture.call("selection_clear");

    const auto moved =
      fixture.call("entity_move_brushes", Json{{"ids", {brush2}}, {"entity", door}});
    CHECK(resultOf(moved)["entity"] == door);
    CHECK(resultOf(moved)["moved"] == Json{brush2});
    CHECK(resultOf(moved)["removedEntities"].empty());
    CHECK(fixture.node(brush2)->parent() == fixture.node(door));
    CHECK(moved["undoStep"] == "AI: Move Brushes to Entity");
    CHECK(map.selection().nodes.empty());

    const auto again = fixture.call(
      "entity_move_brushes", Json{{"ids", {brush1, brush2}}, {"entity", door}});
    CHECK(hasWarning(again, "NOTHING_MOVED"));
    CHECK(resultOf(again)["moved"].empty());

    const auto toWorld = fixture.call(
      "entity_move_brushes", Json{{"ids", {brush1, brush2}}, {"entity", "world"}});
    CHECK(resultOf(toWorld)["entity"] == "world");
    CHECK(resultOf(toWorld)["moved"] == Json{brush1, brush2});
    CHECK(resultOf(toWorld)["removedEntities"] == Json{door});
    CHECK(fixture.node(door) == nullptr);
    CHECK(fixture.node(brush1)->parent() == map.editorContext().currentLayer());

    const auto nothing =
      fixture.call("entity_move_brushes", Json{{"ids", {brush1}}, {"entity", "world"}});
    CHECK(hasWarning(nothing, "NOTHING_MOVED"));

    SECTION("selection and wrong targets")
    {
      const auto light = resultOf(fixture.call(
        "entity_create_point",
        Json{{"classname", "light"}, {"position", {128, 128, 128}}}))["entity"];
      CHECK(
        fixture
          .callExpectingError(
            "entity_move_brushes", Json{{"ids", {brush1}}, {"entity", light}})
          .code
        == ErrorCode::WrongObjectKind);
      CHECK(
        fixture
          .callExpectingError(
            "entity_move_brushes", Json{{"ids", {brush1}}, {"entity", brush2}})
          .code
        == ErrorCode::WrongObjectKind);

      const auto wall = resultOf(fixture.call(
        "entity_create_brush",
        Json{{"classname", "func_wall"}, {"ids", {brush2}}}))["entity"];
      fixture.call("selection_set", Json{{"ids", {brush1}}});
      const auto fromSelection =
        fixture.call("entity_move_brushes", Json{{"entity", wall}});
      CHECK(resultOf(fromSelection)["moved"] == Json{brush1});
      CHECK(fixture.node(brush1)->parent() == fixture.node(wall.get<std::string>()));
      CHECK(selectedIds(fixture, map) == std::vector{brush1});
    }
  }

  SECTION("other definition formats")
  {
    SECTION("DEF")
    {
      fixture.call(
        "entity_definitions_set", Json{{"type", "builtin"}, {"path", "Rubicon2.def"}});
      const auto created = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "monster_ogre"},
          {"position", {128, 128, 128}},
          {"dropToFloor", true}});
      CHECK(!hasWarning(created, "UNKNOWN_CLASSNAME"));
      CHECK(resultOf(created)["origin"] == Json{128, 128, 24});
      CHECK(resultOf(created)["bounds"]["max"] == Json{160, 160, 88});
    }

    SECTION("ENT")
    {
      const auto ent = getFixtureRoot() / "games" / "Quake3" / "entities.ent";
      fixture.call(
        "entity_definitions_set", Json{{"type", "external"}, {"path", ent.string()}});
      const auto created = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "info_player_deathmatch"},
          {"position", {128, 128, 128}},
          {"dropToFloor", true},
          {"angle", 180}});
      CHECK(!hasWarning(created, "UNKNOWN_CLASSNAME"));
      CHECK(resultOf(created)["origin"] == Json{128, 128, 24});
      CHECK(resultOf(created)["properties"]["angle"] == "180");
    }
  }
}

} // namespace tb::mcp
