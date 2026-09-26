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
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"

#include <algorithm>
#include <string>
#include <utility>
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

/** Creates a document through document_new, so that the game's definitions are used. */
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

mdl::EntityNode* addPointEntity(
  mdl::Map& map,
  const std::string& classname,
  std::vector<mdl::EntityProperty> properties = {})
{
  properties.insert(properties.begin(), {"classname", classname});
  properties.emplace_back("origin", "0 0 24");
  auto* entityNode = new mdl::EntityNode{mdl::Entity{std::move(properties)}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

mdl::EntityNode* addBrushEntity(
  mdl::Map& map,
  const std::string& classname,
  const vm::bbox3d& bounds,
  std::vector<mdl::EntityProperty> properties = {})
{
  properties.insert(properties.begin(), {"classname", classname});
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, "stone")
                 .value();
  auto* entityNode = new mdl::EntityNode{mdl::Entity{std::move(properties)}};
  entityNode->addChild(new mdl::BrushNode{std::move(brush)});
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

std::string propertyOf(const mdl::EntityNodeBase& entityNode, const std::string& key)
{
  const auto* value = entityNode.entity().property(key);
  return value ? *value : "<unset>";
}

bool hasProperty(const mdl::EntityNodeBase& entityNode, const std::string& key)
{
  return entityNode.entity().hasProperty(key);
}

const Json* findWarning(const Json& result, const std::string& code)
{
  const auto& warnings = result["warnings"];
  const auto it = std::ranges::find_if(
    warnings, [&](const auto& warning) { return warning["code"] == code; });
  return it != warnings.end() ? &*it : nullptr;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return findWarning(result, code) != nullptr;
}

bool contains(const Json& array, const std::string& value)
{
  return std::ranges::find(array, Json(value)) != array.end();
}

} // namespace

TEST_CASE("EntityPropertyTools")
{
  auto fixture = McpToolFixture{};
  auto& document = newDocument(fixture, "Quake");
  auto& map = document.map();

  auto* ogre = addPointEntity(map, "monster_ogre");
  auto* door = addBrushEntity(map, "func_door", {{0, 0, 0}, {64, 16, 128}});
  auto* trigger = addBrushEntity(map, "trigger_once", {{128, 0, 0}, {192, 64, 64}});
  const auto ogreId = fixture.id(*ogre);
  const auto doorId = fixture.id(*door);
  const auto triggerId = fixture.id(*trigger);

  SECTION("entity_properties_set")
  {
    SECTION("explicit ids")
    {
      const auto result = fixture.call(
        "entity_properties_set",
        Json{
          {"ids", {ogreId, doorId}},
          {"properties",
           {{"targetname", "t1"},
            {"health", 300},
            {"angles", Json{0, 90.5, 0}},
            {"_shadow", true}}},
        });
      CHECK(result["undoStep"] == "AI: Set Entity Properties");
      CHECK(resultOf(result)["entities"] == Json{ogreId, doorId});
      CHECK(resultOf(result)["set"]["angles"] == "0 90.5 0");
      CHECK(contains(result["changes"]["modified"], ogreId));
      CHECK(contains(result["changes"]["modified"], doorId));
      CHECK(propertyOf(*ogre, "targetname") == "t1");
      CHECK(propertyOf(*door, "health") == "300");
      CHECK(propertyOf(*ogre, "angles") == "0 90.5 0");
      CHECK(propertyOf(*door, "_shadow") == "1");

      // monster_ogre does not define health, func_door does
      const auto* unknown = findWarning(result, "UNKNOWN_PROPERTY");
      REQUIRE(unknown);
      CHECK((*unknown)["objectIds"] == Json{ogreId});

      const auto removed = fixture.call(
        "entity_properties_set",
        Json{{"ids", {ogreId}}, {"properties", {{"targetname", nullptr}}}});
      CHECK(resultOf(removed)["removed"] == Json{"targetname"});
      CHECK(!hasProperty(*ogre, "targetname"));
      CHECK(hasProperty(*door, "targetname"));
    }

    SECTION("selection and brushes")
    {
      mdl::selectNodes(map, {door->children().front()});
      const auto result =
        fixture.call("entity_properties_set", Json{{"properties", {{"speed", "200"}}}});
      CHECK(resultOf(result)["entities"] == Json{doorId});
      CHECK(propertyOf(*door, "speed") == "200");

      const auto viaBrush = fixture.call(
        "entity_properties_set",
        Json{
          {"ids", {fixture.id(*trigger->children().front())}},
          {"properties", {{"wait", 2}}},
        });
      CHECK(resultOf(viaBrush)["entities"] == Json{triggerId});
      CHECK(propertyOf(*trigger, "wait") == "2");
    }

    SECTION("worldspawn")
    {
      const auto result = fixture.call(
        "entity_properties_set",
        Json{{"ids", {"world"}}, {"properties", {{"message", "The Keep"}}}});
      CHECK(resultOf(result)["entities"] == Json{"world"});
      CHECK(propertyOf(map.worldNode(), "message") == "The Keep");
      CHECK(contains(result["changes"]["modified"], "world"));

      const auto mixed = fixture.call(
        "entity_properties_set",
        Json{{"ids", {"world", ogreId}}, {"properties", {{"_tb_note", "x"}}}});
      CHECK(propertyOf(map.worldNode(), "_tb_note") == "x");
      CHECK(propertyOf(*ogre, "_tb_note") == "x");
      CHECK(mixed["warnings"].empty());
    }

    SECTION("validation warnings")
    {
      const auto result = fixture.call(
        "entity_properties_set",
        Json{
          {"ids", {doorId}},
          {"properties",
           {{"speed", "abc"}, {"sounds", "7"}, {"bogus", "1"}, {"spawnflags", "64"}}},
        });
      CHECK(hasWarning(result, "INVALID_PROPERTY_VALUE"));
      CHECK(hasWarning(result, "INVALID_CHOICE"));
      CHECK(hasWarning(result, "UNKNOWN_PROPERTY"));
      CHECK(hasWarning(result, "UNKNOWN_FLAGS"));
      // warnings never block
      CHECK(propertyOf(*door, "sounds") == "7");

      // identical warnings for several entities are reported once
      auto* ogre2 = addPointEntity(map, "monster_ogre");
      const auto deduped = fixture.call(
        "entity_properties_set",
        Json{{"ids", {ogreId, fixture.id(*ogre2)}}, {"properties", {{"bogus", "1"}}}});
      REQUIRE(deduped["warnings"].size() == 1);
      CHECK(deduped["warnings"][0]["objectIds"].size() == 2);

      const auto classname = fixture.call(
        "entity_properties_set",
        Json{{"ids", {ogreId}}, {"properties", {{"classname", "monster_nonexistent"}}}});
      CHECK(hasWarning(classname, "UNKNOWN_CLASSNAME"));
      CHECK(ogre->entity().classname() == "monster_nonexistent");

      // other keys are validated against the new class
      const auto changedClass = fixture.call(
        "entity_properties_set",
        Json{
          {"ids", {ogreId}},
          {"properties", {{"classname", "light"}, {"light", "bright"}}},
        });
      CHECK(hasWarning(changedClass, "INVALID_PROPERTY_VALUE"));
      CHECK(!hasWarning(changedClass, "UNKNOWN_CLASSNAME"));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "entity_properties_set",
            Json{{"ids", {ogreId}}, {"properties", Json::object()}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_properties_set", Json{{"ids", {ogreId}}, {"properties", {{"", "x"}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_properties_set",
            Json{{"ids", {ogreId}}, {"properties", {{"message", "say \"hi\""}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_properties_set",
            Json{{"ids", {ogreId}}, {"properties", {{"message", {{"a", 1}}}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_properties_set",
            Json{{"ids", {ogreId}}, {"properties", {{"classname", nullptr}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_properties_set",
            Json{{"ids", {"layer:default"}}, {"properties", {{"a", "b"}}}})
          .code
        == ErrorCode::InvalidArgument);

      mdl::deselectAll(map);
      CHECK(
        fixture
          .callExpectingError("entity_properties_set", Json{{"properties", {{"a", "b"}}}})
          .code
        == ErrorCode::NoSelection);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "entity_properties_set",
        Json{
          {"ids", {ogreId}}, {"properties", {{"targetname", "dry"}}}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(contains(result["changes"]["modified"], ogreId));
      CHECK(!hasProperty(*ogre, "targetname"));
    }
  }

  SECTION("entity_property_remove")
  {
    fixture.call(
      "entity_properties_set",
      Json{
        {"ids", {ogreId, doorId}}, {"properties", {{"targetname", "a"}, {"wait", 1}}}});

    const auto result = fixture.call(
      "entity_property_remove",
      Json{{"ids", {ogreId, doorId}}, {"keys", {"targetname", "missing_key"}}});
    CHECK(result["undoStep"] == "AI: Remove Entity Properties");
    CHECK(resultOf(result)["entities"] == Json{ogreId, doorId});
    CHECK(resultOf(result)["removed"] == Json{"targetname"});
    CHECK(hasWarning(result, "PROPERTY_NOT_PRESENT"));
    CHECK(contains(result["changes"]["modified"], ogreId));
    CHECK(!hasProperty(*ogre, "targetname"));
    CHECK(!hasProperty(*door, "targetname"));

    mdl::selectNodes(map, {ogre});
    const auto dryRun =
      fixture.call("entity_property_remove", Json{{"keys", {"wait"}}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["entities"] == Json{ogreId});
    CHECK(hasProperty(*ogre, "wait"));

    fixture.call("entity_property_remove", Json{{"keys", {"wait"}}});
    CHECK(!hasProperty(*ogre, "wait"));
    CHECK(hasProperty(*door, "wait"));

    CHECK(
      fixture
        .callExpectingError(
          "entity_property_remove", Json{{"ids", {ogreId}}, {"keys", {"classname"}}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "entity_property_remove", Json{{"ids", {ogreId}}, {"keys", Json::array()}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("entity_property_rename")
  {
    fixture.call(
      "entity_properties_set",
      Json{{"ids", {ogreId}}, {"properties", {{"target2", "a"}, {"killtarget", "b"}}}});

    const auto result = fixture.call(
      "entity_property_rename",
      Json{{"ids", {ogreId, doorId}}, {"from", "target2"}, {"to", "killtarget"}});
    CHECK(result["undoStep"] == "AI: Rename Entity Property");
    CHECK(resultOf(result)["renamed"] == Json{ogreId});
    CHECK(hasWarning(result, "PROPERTY_NOT_PRESENT"));
    CHECK(hasWarning(result, "PROPERTY_OVERWRITTEN"));
    CHECK(contains(result["changes"]["modified"], ogreId));
    CHECK(propertyOf(*ogre, "killtarget") == "a");
    CHECK(!hasProperty(*ogre, "target2"));

    mdl::selectNodes(map, {ogre});
    const auto dryRun = fixture.call(
      "entity_property_rename",
      Json{{"from", "killtarget"}, {"to", "bogus_key"}, {"dryRun", true}});
    CHECK(hasWarning(dryRun, "UNKNOWN_PROPERTY"));
    CHECK(propertyOf(*ogre, "killtarget") == "a");
    CHECK(!hasProperty(*ogre, "bogus_key"));

    CHECK(
      fixture
        .callExpectingError(
          "entity_property_rename", Json{{"from", "killtarget"}, {"to", "killtarget"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "entity_property_rename", Json{{"from", "classname"}, {"to", "foo"}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("entity_spawnflags_set")
  {
    SECTION("FGD flags")
    {
      const auto result = fixture.call(
        "entity_spawnflags_set",
        Json{{"ids", {ogreId}}, {"set", {"Not on Easy", "not_on_normal"}}});
      CHECK(result["undoStep"] == "AI: Set Spawnflags");
      CHECK(contains(result["changes"]["modified"], ogreId));
      CHECK(propertyOf(*ogre, "spawnflags") == "768");
      CHECK(
        resultOf(result)["entities"][0]
        == Json{
          {"id", ogreId},
          {"classname", "monster_ogre"},
          {"spawnflags", 768},
          {"flags", {"Not on Easy", "Not on Normal"}},
        });

      mdl::selectNodes(map, {ogre});
      const auto cleared = fixture.call(
        "entity_spawnflags_set", Json{{"set", {"Ambush"}}, {"clear", {"bit8"}}});
      CHECK(resultOf(cleared)["entities"][0]["spawnflags"] == 513);
      CHECK(propertyOf(*ogre, "spawnflags") == "513");

      const auto dryRun =
        fixture.call("entity_spawnflags_set", Json{{"clear", {"512"}}, {"dryRun", true}});
      CHECK(resultOf(dryRun)["entities"][0]["spawnflags"] == 1);
      CHECK(propertyOf(*ogre, "spawnflags") == "513");
    }

    SECTION("per class")
    {
      // "Not on Easy" means 256 for both, "Starts Open" only exists for doors
      const auto result = fixture.call(
        "entity_spawnflags_set",
        Json{{"ids", {ogreId, doorId}}, {"set", {"Not on Easy"}}});
      CHECK(propertyOf(*ogre, "spawnflags") == "256");
      CHECK(propertyOf(*door, "spawnflags") == "256");

      const auto error = fixture.callExpectingError(
        "entity_spawnflags_set",
        Json{{"ids", {ogreId, doorId}}, {"set", {"Starts Open"}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.objectIds == std::vector<std::string>{ogreId});
      CHECK(error.message.find("Ambush") != std::string::npos);
      CHECK(error.message.find("Not on Hard") != std::string::npos);
      // atomic: the door was not changed either
      CHECK(propertyOf(*door, "spawnflags") == "256");
    }

    SECTION("no definition")
    {
      auto* custom = addPointEntity(map, "my_custom_entity");
      const auto customId = fixture.id(*custom);
      const auto result = fixture.call(
        "entity_spawnflags_set", Json{{"ids", {customId}}, {"set", {"bit3", "2"}}});
      CHECK(resultOf(result)["entities"][0]["flags"] == Json{"bit1", "bit3"});
      CHECK(propertyOf(*custom, "spawnflags") == "10");

      CHECK(
        fixture
          .callExpectingError(
            "entity_spawnflags_set", Json{{"ids", {customId}}, {"set", {"Ambush"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_spawnflags_set", Json{{"ids", {customId}}, {"set", {"3"}}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("entity_spawnflags_set", Json{{"ids", {ogreId}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_spawnflags_set",
            Json{{"ids", {ogreId}}, {"set", {"Ambush"}}, {"clear", {"ambush"}}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("DEF definitions")
    {
      fixture.call(
        "entity_definitions_set", Json{{"type", "builtin"}, {"path", "Rubicon2.def"}});
      auto* breakable =
        addBrushEntity(map, "func_breakable", {{256, 0, 0}, {320, 64, 64}});
      const auto result = fixture.call(
        "entity_spawnflags_set",
        Json{{"ids", {fixture.id(*breakable)}}, {"set", {"no monsters"}}});
      CHECK(resultOf(result)["entities"][0]["flags"] == Json{"NO_MONSTERS"});
      CHECK(propertyOf(*breakable, "spawnflags") == "1");
    }

    SECTION("ENT definitions")
    {
      const auto ent = getFixtureRoot() / "games" / "Quake3" / "entities.ent";
      fixture.call(
        "entity_definitions_set", Json{{"type", "external"}, {"path", ent.string()}});
      const auto result = fixture.call(
        "entity_spawnflags_set", Json{{"ids", {doorId}}, {"set", {"crusher"}}});
      CHECK(propertyOf(*door, "spawnflags") == "4");
      REQUIRE(resultOf(result)["entities"][0]["flags"].size() == 1);

      const auto error = fixture.callExpectingError(
        "entity_spawnflags_set", Json{{"ids", {doorId}}, {"set", {"Ambush"}}});
      CHECK(error.message.find("START_OPEN") != std::string::npos);
    }
  }

  SECTION("entity_defaults_apply")
  {
    auto* custom = addPointEntity(map, "my_custom_entity");
    const auto customId = fixture.id(*custom);

    const auto result = fixture.call(
      "entity_defaults_apply", Json{{"ids", {doorId, customId}}, {"mode", "missing"}});
    CHECK(result["undoStep"] == "AI: Apply Entity Defaults");
    CHECK(contains(result["changes"]["modified"], doorId));
    CHECK(hasWarning(result, "NO_DEFINITION"));
    const auto& doorResult = resultOf(result)["entities"][0];
    CHECK(doorResult["id"] == doorId);
    CHECK(doorResult["set"]["speed"] == "100");
    CHECK(doorResult["set"]["lip"] == "8");
    CHECK(resultOf(result)["entities"][1]["set"] == Json::object());
    CHECK(propertyOf(*door, "speed") == "100");

    fixture.call(
      "entity_properties_set", Json{{"ids", {doorId}}, {"properties", {{"speed", 50}}}});
    const auto missing =
      fixture.call("entity_defaults_apply", Json{{"ids", {doorId}}, {"mode", "missing"}});
    CHECK(!resultOf(missing)["entities"][0]["set"].contains("speed"));
    CHECK(propertyOf(*door, "speed") == "50");

    mdl::selectNodes(map, {door->children().front()});
    const auto dryRun =
      fixture.call("entity_defaults_apply", Json{{"mode", "existing"}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["entities"][0]["set"]["speed"] == "100");
    CHECK(propertyOf(*door, "speed") == "50");

    fixture.call("entity_defaults_apply", Json{{"mode", "all"}});
    CHECK(propertyOf(*door, "speed") == "100");

    CHECK(
      fixture.callExpectingError("entity_defaults_apply", Json{{"mode", "some"}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("entity_link and entity_links_get")
  {
    const auto result =
      fixture.call("entity_link", Json{{"source", triggerId}, {"target", doorId}});
    CHECK(result["undoStep"] == "AI: Link Entities");
    CHECK(contains(result["changes"]["modified"], doorId));
    CHECK(contains(result["changes"]["modified"], triggerId));
    CHECK(
      resultOf(result)
      == Json{
        {"source", triggerId},
        {"target", doorId},
        {"sourceKey", "target"},
        {"targetKey", "targetname"},
        {"name", "func_door_1"},
        {"generated", true},
      });
    CHECK(propertyOf(*door, "targetname") == "func_door_1");
    CHECK(propertyOf(*trigger, "target") == "func_door_1");

    // a second door gets a new unique name
    auto* door2 = addBrushEntity(map, "func_door", {{0, 128, 0}, {64, 144, 128}});
    auto* trigger2 = addBrushEntity(map, "trigger_once", {{128, 128, 0}, {192, 192, 64}});
    const auto door2Id = fixture.id(*door2);
    const auto trigger2Id = fixture.id(*trigger2);
    const auto second =
      fixture.call("entity_link", Json{{"source", trigger2Id}, {"target", door2Id}});
    CHECK(resultOf(second)["name"] == "func_door_2");

    // an existing name is reused
    const auto reused = fixture.call(
      "entity_link", Json{{"source", ogreId}, {"target", doorId}, {"name", "other"}});
    CHECK(resultOf(reused)["name"] == "func_door_1");
    CHECK(resultOf(reused)["generated"] == false);
    CHECK(hasWarning(reused, "TARGET_ALREADY_NAMED"));
    CHECK(propertyOf(*ogre, "target") == "func_door_1");

    // relinking replaces the old value
    const auto replaced =
      fixture.call("entity_link", Json{{"source", ogreId}, {"target", door2Id}});
    CHECK(hasWarning(replaced, "LINK_REPLACED"));
    CHECK(propertyOf(*ogre, "target") == "func_door_2");

    // explicit name for an unnamed target
    auto* light = addPointEntity(map, "light");
    const auto named = fixture.call(
      "entity_link",
      Json{{"source", triggerId}, {"target", fixture.id(*light)}, {"name", "lamp"}});
    CHECK(resultOf(named)["generated"] == false);
    CHECK(propertyOf(*light, "targetname") == "lamp");
    CHECK(propertyOf(*trigger, "target") == "lamp");

    const auto dryRun = fixture.call(
      "entity_link", Json{{"source", doorId}, {"target", triggerId}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["name"] == "trigger_once_1");
    CHECK(!hasProperty(*trigger, "targetname"));

    CHECK(
      fixture
        .callExpectingError("entity_link", Json{{"source", doorId}, {"target", doorId}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError("entity_link", Json{{"source", "world"}, {"target", doorId}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "entity_link",
          Json{{"source", fixture.id(*door->children().front())}, {"target", doorId}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "entity_link",
          Json{{"source", triggerId}, {"target", fixture.id(*trigger2)}, {"name", "a b"}})
        .code
      == ErrorCode::InvalidArgument);

    // links: trigger -> lamp (trigger's target is now "lamp"), trigger2 -> door2, ogre ->
    // door2; broken: none; unreferenced: door (func_door_1)
    fixture.call(
      "entity_properties_set",
      Json{{"ids", {trigger2Id}}, {"properties", {{"killtarget", "nowhere"}}}});

    const auto all = fixture.call("entity_links_get");
    CHECK(
      all["counts"] == Json{{"links", 3}, {"missingTarget", 1}, {"missingSource", 1}});
    CHECK(all["total"] == 4);
    const auto& items = all["items"];
    CHECK(std::ranges::any_of(items, [&](const auto& item) {
      return item
             == Json{
               {"source", trigger2Id},
               {"sourceKey", "target"},
               {"target", door2Id},
               {"targetKey", "targetname"},
               {"name", "func_door_2"},
             };
    }));
    CHECK(std::ranges::any_of(items, [&](const auto& item) {
      return item
             == Json{
               {"source", trigger2Id},
               {"sourceKey", "killtarget"},
               {"target", nullptr},
               {"name", "nowhere"},
               {"broken", "missing_target"},
             };
    }));

    const auto broken = fixture.call(
      "entity_links_get", Json{{"brokenOnly", true}, {"includeUnreferenced", true}});
    CHECK(broken["total"] == 2);
    CHECK(std::ranges::any_of(broken["items"], [&](const auto& item) {
      return item
             == Json{
               {"target", doorId},
               {"targetKey", "targetname"},
               {"name", "func_door_1"},
               {"broken", "missing_source"},
             };
    }));

    // links from or to door2
    const auto door2Links = fixture.call("entity_links_get", Json{{"ids", {door2Id}}});
    CHECK(door2Links["total"] == 2);
    CHECK(door2Links["counts"]["links"] == 2);

    const auto page = fixture.call("entity_links_get", Json{{"limit", 1}});
    CHECK(page["items"].size() == 1);
    CHECK(page["nextCursor"].is_string());
  }

  SECTION("entity_color_set")
  {
    auto* light = addPointEntity(map, "light");
    const auto lightId = fixture.id(*light);

    // Quake's FGD defines no color property for lights; the compilers read _color
    const auto result = fixture.call(
      "entity_color_set", Json{{"ids", {lightId}}, {"color", {255, 128, 0}}});
    CHECK(result["undoStep"] == "AI: Set Entity Color");
    CHECK(contains(result["changes"]["modified"], lightId));
    CHECK(
      resultOf(result)["entities"][0]
      == Json{{"id", lightId}, {"key", "_color"}, {"value", "255 128 0"}});

    mdl::selectNodes(map, {light});
    fixture.call("entity_color_set", Json{{"color", {1, 0.5, 0}}});
    CHECK(propertyOf(*light, "_color") == "1 0.5 0");

    const auto dryRun = fixture.call(
      "entity_color_set",
      Json{{"color", {0, 0, 1}}, {"range", "byte"}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["entities"][0]["value"] == "0 0 1");
    CHECK(propertyOf(*light, "_color") == "1 0.5 0");

    CHECK(
      fixture.callExpectingError("entity_color_set", Json{{"color", {300, 0, 0}}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "entity_color_set", Json{{"color", {2, 0, 0}}, {"range", "float"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("entity_color_set", Json{{"color", {1, 0}}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "entity_color_set", Json{{"color", {1, 0, 0}}, {"key", "light"}})
        .code
      == ErrorCode::InvalidArgument);

    SECTION("float color property")
    {
      // Heretic 2 lights define _color as color1 (float): byte input is converted
      const auto fgd = getFixtureRoot() / "games" / "Heretic2" / "heretic2.fgd";
      fixture.call(
        "entity_definitions_set", Json{{"type", "external"}, {"path", fgd.string()}});
      const auto converted = fixture.call(
        "entity_color_set", Json{{"ids", {lightId}}, {"color", {255, 0, 255}}});
      CHECK(resultOf(converted)["entities"][0]["key"] == "_color");
      CHECK(propertyOf(*light, "_color") == "1 0 1");
    }

    SECTION("byte color property")
    {
      // Digital Paintball 2 lights define _color as color255 (byte): float input is
      // converted, extra components (brightness) are kept
      const auto fgd = getFixtureRoot() / "games" / "DigitalPaintball2" / "pball2.fgd";
      fixture.call(
        "entity_definitions_set", Json{{"type", "external"}, {"path", fgd.string()}});
      fixture.call(
        "entity_properties_set",
        Json{{"ids", {lightId}}, {"properties", {{"_color", "0 0 0 300"}}}});
      fixture.call("entity_color_set", Json{{"ids", {lightId}}, {"color", {1, 0, 1}}});
      CHECK(propertyOf(*light, "_color") == "255 0 255 300");
    }

    SECTION("ENT color property")
    {
      // Quake 3 lights define _color as an untyped color: values are stored as given
      const auto ent = getFixtureRoot() / "games" / "Quake3" / "entities.ent";
      fixture.call(
        "entity_definitions_set", Json{{"type", "external"}, {"path", ent.string()}});
      const auto q3 = fixture.call(
        "entity_color_set", Json{{"ids", {lightId}}, {"color", {0.25, 0.5, 1}}});
      CHECK(resultOf(q3)["entities"][0]["key"] == "_color");
      CHECK(propertyOf(*light, "_color") == "0.25 0.5 1");
    }
  }
}

} // namespace tb::mcp
