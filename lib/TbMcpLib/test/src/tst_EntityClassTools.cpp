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
#include "mcp/tools/EntityClassTools.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

/** Creates a document through document_new, so the game's real definitions are used. */
ui::MapDocument& newDocument(McpToolFixture& fixture, const std::string& game)
{
  const auto result = fixture.call("document_new", Json{{"game", game}});
  const auto id = result["result"]["document"]["id"].get<std::string>();
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

std::vector<std::string> names(const Json& items)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item["name"].get<std::string>());
  }
  return result;
}

bool contains(const std::vector<std::string>& strings, const std::string& string)
{
  return std::ranges::find(strings, string) != strings.end();
}

const Json* findByKey(const Json& array, const std::string& member, const Json& value)
{
  for (const auto& item : array)
  {
    if (item.contains(member) && item[member] == value)
    {
      return &item;
    }
  }
  return nullptr;
}

mdl::EntityNode* addPointEntity(
  mdl::Map& map, std::vector<std::pair<std::string, std::string>> properties)
{
  auto entityProperties = std::vector<mdl::EntityProperty>{};
  for (auto& [key, value] : properties)
  {
    entityProperties.emplace_back(std::move(key), std::move(value));
  }
  auto* entityNode = new mdl::EntityNode{mdl::Entity{std::move(entityProperties)}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

} // namespace

TEST_CASE("EntityClassTools")
{
  auto fixture = McpToolFixture{};
  auto& document = newDocument(fixture, "Quake");
  auto& map = document.map();
  REQUIRE(!map.entityDefinitionManager().definitions().empty());

  SECTION("entity_classes_list")
  {
    SECTION("prefix, sorting and groups")
    {
      const auto result = fixture.call("entity_classes_list", {{"prefix", "monster_"}});
      const auto classNames = names(result["items"]);
      CHECK(result["total"] == 16);
      CHECK(classNames.size() == 16);
      CHECK(classNames.front() == "monster_army");
      CHECK(std::ranges::is_sorted(classNames));
      CHECK(
        result["groups"]
        == Json::array(
          {Json{{"name", "monster"}, {"point", 16}, {"brush", 0}, {"used", 0}}}));

      const auto& item = *findByKey(result["items"], "name", "monster_ogre");
      CHECK(
        item
        == Json{
          {"name", "monster_ogre"},
          {"type", "point"},
          {"group", "monster"},
          {"description", "Ogre"},
          {"usageCount", 0},
        });
    }

    SECTION("glob, type, group and search filters")
    {
      auto result =
        fixture.call("entity_classes_list", {{"prefix", "*_door*"}, {"limit", 1000}});
      auto classNames = names(result["items"]);
      CHECK(contains(classNames, "func_door"));
      CHECK(contains(classNames, "func_door_secret"));

      result = fixture.call(
        "entity_classes_list", {{"type", "brush"}, {"group", "FUNC"}, {"limit", 1000}});
      CHECK(result["total"].get<size_t>() > 5);
      for (const auto& item : result["items"])
      {
        CHECK(item["type"] == "brush");
        CHECK(item["group"] == "func");
      }
      CHECK(result["groups"].size() == 1);
      CHECK(result["groups"][0]["point"] == 0);

      result =
        fixture.call("entity_classes_list", {{"type", "point"}, {"prefix", "func_"}});
      CHECK(result["total"] == 0);
      CHECK(result["groups"] == Json::array());

      result = fixture.call("entity_classes_list", {{"search", "OGRE"}});
      CHECK(
        names(result["items"])
        == std::vector<std::string>{"monster_ogre", "monster_ogre_marksman"});

      // search looks at the description, too
      result = fixture.call("entity_classes_list", {{"search", "flesh off themselves"}});
      CHECK(names(result["items"]) == std::vector<std::string>{"monster_zombie"});
    }

    SECTION("pagination and full detail")
    {
      const auto first =
        fixture.call("entity_classes_list", {{"prefix", "monster_"}, {"limit", 5}});
      CHECK(first["items"].size() == 5);
      CHECK(first["total"] == 16);
      REQUIRE(first["nextCursor"].is_string());
      // groups cover all matches, not just the page
      CHECK(first["groups"][0]["point"] == 16);

      const auto second = fixture.call(
        "entity_classes_list",
        {{"prefix", "monster_"}, {"limit", 5}, {"cursor", first["nextCursor"]}});
      CHECK(second["items"].size() == 5);
      CHECK(second["items"][0]["name"] != first["items"][0]["name"]);

      const auto full = fixture.call(
        "entity_classes_list", {{"prefix", "monster_ogre"}, {"detail", "full"}});
      const auto& ogre = *findByKey(full["items"], "name", "monster_ogre");
      CHECK(ogre["size"] == Json{{"min", {-32, -32, -24}}, {"max", {32, 32, 64}}});
      CHECK(ogre["model"] == "progs/ogre.mdl");
      CHECK(ogre["color"].size() == 3);
      CHECK(contains(ogre["propertyKeys"].get<std::vector<std::string>>(), "spawnflags"));

      const auto fields = fixture.call(
        "entity_classes_list",
        {{"prefix", "monster_ogre"}, {"fields", Json::array({"name"})}});
      CHECK(fields["items"][0] == Json{{"name", "monster_ogre"}});
    }

    SECTION("usage")
    {
      addPointEntity(map, {{"classname", "monster_ogre"}});
      addPointEntity(map, {{"classname", "monster_ogre"}});

      auto result = fixture.call("entity_classes_list", {{"used", true}});
      // worldspawn is used, too
      CHECK(
        names(result["items"]) == std::vector<std::string>{"monster_ogre", "worldspawn"});
      CHECK(result["items"][0]["usageCount"] == 2);
      CHECK((*findByKey(result["groups"], "name", "monster"))["used"] == 1);

      result = fixture.call(
        "entity_classes_list", {{"used", false}, {"prefix", "monster_ogre"}});
      CHECK(names(result["items"]) == std::vector<std::string>{"monster_ogre_marksman"});
    }

    SECTION("without definitions")
    {
      auto& other = fixture.create();
      REQUIRE(other.map().entityDefinitionManager().definitions().empty());
      const auto result =
        fixture.call("entity_classes_list", {{"document", fixture.documentId(other)}});
      CHECK(result["items"] == Json::array());
      CHECK(result["total"] == 0);
      REQUIRE(result["warnings"].size() == 1);
      CHECK(result["warnings"][0]["code"] == "NO_ENTITY_DEFINITIONS");
    }

    SECTION("invalid arguments")
    {
      CHECK(
        fixture.callExpectingError("entity_classes_list", {{"type", "model"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("entity_class_describe")
  {
    SECTION("point class from an FGD")
    {
      const auto ogre =
        fixture.call("entity_class_describe", {{"classname", "monster_ogre"}});
      CHECK(ogre["name"] == "monster_ogre");
      CHECK(ogre["type"] == "point");
      CHECK(ogre["group"] == "monster");
      CHECK(ogre["description"] == "Ogre");
      CHECK(ogre["size"] == Json{{"min", {-32, -32, -24}}, {"max", {32, 32, 64}}});
      CHECK(
        ogre["model"]
        == Json{{"default", {{"path", "progs/ogre.mdl"}, {"skin", 0}, {"frame", 0}}}});
      CHECK(ogre["decal"].is_null());
      CHECK(ogre["color"].size() == 3);
      CHECK(ogre["usageCount"] == 0);

      const auto& ambush = *findByKey(ogre["spawnflags"], "name", "Ambush");
      CHECK(
        ambush == Json{{"bit", 0}, {"value", 1}, {"name", "Ambush"}, {"default", false}});
      const auto& notOnEasy = *findByKey(ogre["spawnflags"], "name", "Not on Easy");
      CHECK(notOnEasy["bit"] == 8);
      CHECK(notOnEasy["value"] == 256);

      CHECK(ogre["linkProperties"]["sources"] == Json::array({"target", "killtarget"}));
      CHECK(ogre["linkProperties"]["targets"] == Json::array({"targetname"}));
      CHECK(ogre["rotation"] == Json{{"key", "angle"}, {"type", "angle_up_down"}});

      const auto& angle = *findByKey(ogre["properties"], "key", "angle");
      CHECK(angle["type"] == "integer");
      const auto& target = *findByKey(ogre["properties"], "key", "target");
      CHECK(target["linkRole"] == "source");
    }

    SECTION("brush class from an FGD")
    {
      const auto door =
        fixture.call("entity_class_describe", {{"classname", "func_door"}});
      CHECK(door["type"] == "brush");
      CHECK(door["description"] == "Basic door");
      CHECK(door["size"].is_null());
      CHECK(door["model"].is_null());

      const auto& sounds = *findByKey(door["properties"], "key", "sounds");
      CHECK(sounds["type"] == "choice");
      CHECK(sounds["choices"].size() == 5);
      CHECK(sounds["choices"][1] == Json{{"value", "1"}, {"description", "Stone"}});

      CHECK(door["defaults"]["speed"] == "100");
      CHECK(door["defaults"]["lip"] == "8");
      CHECK(door["defaults"]["wait"] == "3");

      CHECK(door["linkProperties"]["sources"] == Json::array({"target", "killtarget"}));
      CHECK(door["linkProperties"]["targets"] == Json::array({"targetname"}));

      auto spawnflags = std::vector<std::string>{};
      for (const auto& flag : door["spawnflags"])
      {
        spawnflags.push_back(flag["name"].get<std::string>());
      }
      CHECK(contains(spawnflags, "Starts Open"));
      CHECK(contains(spawnflags, "Gold Key required"));
      CHECK((*findByKey(door["spawnflags"], "name", "Toggle"))["bit"] == 5);
    }

    SECTION("light")
    {
      const auto light = fixture.call("entity_class_describe", {{"classname", "light"}});
      CHECK(light["size"] == Json{{"min", {-8, -8, -8}}, {"max", {8, 8, 8}}});
      CHECK(light["model"] == Json{{"default", nullptr}});
      CHECK((*findByKey(light["properties"], "key", "style"))["type"] == "choice");
      CHECK((*findByKey(light["properties"], "key", "light"))["default"] == "300");
      CHECK(light["spawnflags"].size() == 1);
      CHECK(light["spawnflags"][0]["name"] == "Start off");
      // lights keep their orientation unless they have angle properties
      CHECK(light["rotation"].is_null());
    }

    SECTION("case-insensitive match")
    {
      CHECK(
        fixture.call("entity_class_describe", {{"classname", "MONSTER_OGRE"}})["name"]
        == "monster_ogre");
    }

    SECTION("usage count")
    {
      addPointEntity(map, {{"classname", "monster_ogre"}});
      CHECK(
        fixture.call(
          "entity_class_describe", {{"classname", "monster_ogre"}})["usageCount"]
        == 1);
    }

    SECTION("unknown class")
    {
      const auto error = fixture.callExpectingError(
        "entity_class_describe", {{"classname", "monster_ogr"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK_THAT(error.message, Catch::Matchers::ContainsSubstring("monster_ogre"));
      CHECK_THAT(error.hint, Catch::Matchers::ContainsSubstring("entity_classes_list"));

      const auto other = fixture.callExpectingError(
        "entity_class_describe", {{"classname", "weapon_rocketlaunch"}});
      CHECK_THAT(
        other.message, Catch::Matchers::ContainsSubstring("weapon_rocketlauncher"));

      CHECK(
        fixture.callExpectingError("entity_class_describe", {{"classname", ""}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("entity_class_describe").code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("DEF definitions")
  {
    fixture.call(
      "entity_definitions_set", {{"type", "builtin"}, {"path", "Rubicon2.def"}});

    const auto breakable =
      fixture.call("entity_class_describe", {{"classname", "func_breakable"}});
    CHECK(breakable["type"] == "brush");
    CHECK_THAT(
      breakable["description"].get<std::string>(),
      Catch::Matchers::ContainsSubstring("destroyed by shooting"));
    REQUIRE(!breakable["spawnflags"].empty());
    CHECK(breakable["spawnflags"][0]["name"] == "NO_MONSTERS");
    CHECK(breakable["spawnflags"][0]["value"] == 1);

    const auto list = fixture.call("entity_classes_list", {{"prefix", "func_breakable"}});
    CHECK(names(list["items"]) == std::vector<std::string>{"func_breakable"});
  }

  SECTION("ENT definitions")
  {
    const auto path = getFixtureRoot() / "games" / "Quake3" / "entities.ent";
    fixture.call(
      "entity_definitions_set", {{"type", "external"}, {"path", path.string()}});

    const auto door = fixture.call("entity_class_describe", {{"classname", "func_door"}});
    CHECK(door["type"] == "brush");
    CHECK_THAT(
      door["description"].get<std::string>(),
      Catch::Matchers::ContainsSubstring("Normal sliding door"));

    const auto& speed = *findByKey(door["properties"], "key", "speed");
    CHECK(speed["type"] == "float");
    CHECK(std::stod(speed["default"].get<std::string>()) == 400.0);
    CHECK((*findByKey(door["properties"], "key", "color"))["type"] == "color");

    REQUIRE(door["spawnflags"].size() == 2);
    CHECK(door["spawnflags"][0]["value"] == 1);
    CHECK(door["spawnflags"][1]["value"] == 4);
    CHECK(door["spawnflags"][1]["bit"] == 2);

    const auto armor =
      fixture.call("entity_class_describe", {{"classname", "item_armor_body"}});
    CHECK(armor["type"] == "point");
    CHECK(armor["size"] == Json{{"min", {-16, -16, -16}}, {"max", {16, 16, 16}}});
    CHECK(armor["model"]["default"]["path"] == "models/powerups/armor/armor_red.md3");
  }

  SECTION("entity_model_info")
  {
    auto* ogre = addPointEntity(
      map, {{"classname", "monster_ogre"}, {"origin", "64 0 24"}, {"angle", "90"}});
    auto* zombie =
      addPointEntity(map, {{"classname", "monster_zombie"}, {"spawnflags", "1"}});
    auto* unknown = addPointEntity(map, {{"classname", "no_such_class"}});

    auto* door = new mdl::EntityNode{mdl::Entity{{{"classname", "func_door"}}}};
    door->addChild(mdl::createBrushNode(map));
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {door}}});

    SECTION("explicit ids")
    {
      const auto result = fixture.call(
        "entity_model_info",
        {{"ids",
          {fixture.id(*ogre),
           fixture.id(*zombie),
           fixture.id(*unknown),
           fixture.id(*door)}}});
      REQUIRE(result["entities"].size() == 4);

      const auto& ogreInfo = result["entities"][0];
      CHECK(ogreInfo["id"] == fixture.id(*ogre));
      CHECK(ogreInfo["classname"] == "monster_ogre");
      CHECK(ogreInfo["pointEntity"] == true);
      CHECK(
        ogreInfo["model"] == Json{{"path", "progs/ogre.mdl"}, {"skin", 0}, {"frame", 0}});
      CHECK(ogreInfo["modelLoaded"] == false);
      CHECK(ogreInfo["modelBounds"].is_null());
      CHECK(
        ogreInfo["definitionBounds"]
        == Json{{"min", {-32, -32, -24}}, {"max", {32, 32, 64}}});
      CHECK(ogreInfo["bounds"] == Json{{"min", {32, -32, 0}}, {"max", {96, 32, 88}}});
      CHECK(ogreInfo["scale"] == Json::array({1, 1, 1}));
      CHECK(ogreInfo["rotation"]["key"] == "angle");
      CHECK(ogreInfo["rotation"]["yawPitchRoll"] == Json::array({90, 0, 0}));

      // the model expression depends on the spawnflags
      CHECK(result["entities"][1]["model"]["frame"] == 192);

      const auto& unknownInfo = result["entities"][2];
      CHECK(unknownInfo["model"].is_null());
      CHECK(unknownInfo["definitionBounds"].is_null());

      const auto& doorInfo = result["entities"][3];
      CHECK(doorInfo["pointEntity"] == false);
      CHECK(doorInfo["model"].is_null());
      CHECK(doorInfo["rotation"].is_null());
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {ogre});
      const auto result = fixture.call("entity_model_info");
      REQUIRE(result["entities"].size() == 1);
      CHECK(result["entities"][0]["id"] == fixture.id(*ogre));

      // a selected brush of a brush entity stands for its entity
      mdl::deselectAll(map);
      mdl::selectNodes(map, {door->children().front()});
      CHECK(fixture.call("entity_model_info")["entities"][0]["id"] == fixture.id(*door));
    }

    SECTION("errors")
    {
      CHECK(
        fixture.callExpectingError("entity_model_info").code == ErrorCode::NoSelection);
      CHECK(
        fixture.callExpectingError("entity_model_info", {{"ids", {"entity:99999"}}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture
          .callExpectingError(
            "entity_model_info", {{"ids", {fixture.id(*door->children().front())}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("entityDefinitionsResource")
  {
    const auto resource = entityDefinitionsResource(map);
    CHECK(resource["spec"] == "builtin:Quake.fgd");
    CHECK(resource["count"] == map.entityDefinitionManager().definitions().size());
    const auto& ogre = *findByKey(resource["classes"], "name", "monster_ogre");
    CHECK(ogre["size"] == Json{{"min", {-32, -32, -24}}, {"max", {32, 32, 64}}});
    CHECK(contains(ogre["spawnflags"].get<std::vector<std::string>>(), "Not on Easy"));
    CHECK(contains(ogre["propertyKeys"].get<std::vector<std::string>>(), "angle"));
    const auto& door = *findByKey(resource["classes"], "name", "func_door");
    CHECK(!door.contains("size"));
  }
}

} // namespace tb::mcp
