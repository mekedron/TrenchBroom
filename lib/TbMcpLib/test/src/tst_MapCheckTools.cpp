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
#include "gl/MaterialManager.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpToolFixture.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

const auto ModelCodes = Json::array(
  {"MODEL_BELOW_FLOOR", "MODEL_FLOATING", "MODEL_PENETRATES_BRUSHES", "MODEL_NO_FLOOR"});

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

/** The entity with the given classname and, if given, property value. */
mdl::Node* findEntity(
  mdl::Map& map,
  const std::string& classname,
  const std::string& key = {},
  const std::string& value = {})
{
  return findNode(map.worldNode(), [&](const mdl::Node& node) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
    if (!entityNode || entityNode->entity().classname() != classname)
    {
      return false;
    }
    if (key.empty())
    {
      return true;
    }
    const auto* property = entityNode->entity().property(key);
    return value.empty() ? property == nullptr : property && *property == value;
  });
}

void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

/** map_check.map with the game's entity definitions and materials.wad. */
ui::MapDocument& loadDocument(McpToolFixture& fixture)
{
  const auto* gameInfo = fixture.host().gameManager().gameInfo("Quake");
  REQUIRE(gameInfo);
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "map_check.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = *gameInfo});
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json::array({wad.string()})}});
  processResources(document.map());
  REQUIRE(document.map().materialManager().materials().size() == 6);
  REQUIRE(!document.map().entityDefinitionManager().definitions().empty());
  return document;
}

std::vector<Json> findings(const Json& result, const std::string& code)
{
  auto found = std::vector<Json>{};
  for (const auto& item : result["items"])
  {
    if (item["code"] == code)
    {
      found.push_back(item);
    }
  }
  return found;
}

/** The finding with the given code about the given object, or null. */
const Json* findingFor(const Json& result, const std::string& code, const std::string& id)
{
  for (const auto& item : result["items"])
  {
    if (item["code"] == code && item["objectId"] == id)
    {
      return &item;
    }
  }
  return nullptr;
}

std::vector<std::string> findingsAbout(const Json& result, const std::string& id)
{
  auto codes = std::vector<std::string>{};
  for (const auto& item : result["items"])
  {
    if (item["objectId"] == id)
    {
      codes.push_back(item["code"].get<std::string>());
    }
  }
  return codes;
}

Json check(McpToolFixture& fixture, const std::vector<std::string>& checks)
{
  return fixture.call("map_check", Json{{"checks", checks}, {"limit", 1000}});
}

/** Calls the tool of a finding's suggested fix with its arguments. */
Json applyFix(McpToolFixture& fixture, const Json& finding)
{
  const auto& fix = finding["suggestedFix"];
  REQUIRE(fix.is_object());
  REQUIRE(fix["tool"].is_string());
  return fixture.call(fix["tool"].get<std::string>(), fix["args"]);
}

} // namespace

TEST_CASE("MapCheckTools")
{
  auto fixture = McpToolFixture{};
  auto& document = loadDocument(fixture);
  auto& map = document.map();

  const auto idOf = [&](mdl::Node* node) {
    REQUIRE(node);
    return fixture.id(*node);
  };
  const auto ogre = idOf(findEntity(map, "monster_ogre"));
  const auto soldier = idOf(findEntity(map, "monster_army"));
  const auto dog = idOf(findEntity(map, "monster_dog"));
  const auto scrag = idOf(findEntity(map, "monster_wizard"));
  const auto trigger = idOf(findEntity(map, "trigger_once"));
  const auto button = idOf(findEntity(map, "func_button"));
  const auto relay = idOf(findEntity(map, "trigger_relay", "targetname", "relay1"));
  const auto unnamedRelay = idOf(findEntity(map, "trigger_relay", "targetname"));
  const auto door = idOf(findEntity(map, "func_door", "targetname", "door2"));
  const auto lonelyDoor = idOf(findEntity(map, "func_door", "targetname", "door_lonely"));
  const auto pillarLight = idOf(findEntity(map, "light", "origin", "384 320 96"));
  const auto roomLight = idOf(findEntity(map, "light", "origin", "256 192 160"));
  const auto health = idOf(findEntity(map, "item_health"));
  const auto infoNull = idOf(findEntity(map, "info_null"));

  SECTION("all checks")
  {
    const auto modificationCount = map.modificationCount();
    const auto result = fixture.call("map_check", Json{{"limit", 1000}});
    CHECK(map.modificationCount() == modificationCount);
    CHECK(
      result["checksRun"]
      == Json::array(
        {"placement",
         "npc_spawn",
         "player_start",
         "links",
         "materials",
         "rooms",
         "entity_budget"}));
    CHECK(result["skipped"] == Json::array());
    CHECK(result["total"] == result["items"].size());
    for (const auto& item : result["items"])
    {
      CHECK(item["id"].get<std::string>().starts_with(
        "check:" + item["code"].get<std::string>() + ":"));
      CHECK(result["counts"][item["code"].get<std::string>()].get<size_t>() > 0);
      CHECK(item["description"].get<std::string>().size() > 20);
    }
    // the objects without problems
    for (const auto& id : {dog, scrag, button, relay, door, roomLight, infoNull})
    {
      CAPTURE(id);
      CHECK(findingsAbout(result, id).empty());
    }
  }

  SECTION("placement")
  {
    const auto result = check(fixture, {"placement"});
    CHECK(result["checksRun"] == Json::array({"placement"}));

    SECTION("ENTITY_IN_SOLID")
    {
      // the ogre's box reaches into the east wall
      const auto* stuck = findingFor(result, "ENTITY_IN_SOLID", ogre);
      REQUIRE(stuck);
      CHECK((*stuck)["id"] == "check:ENTITY_IN_SOLID:" + ogre);
      CHECK((*stuck)["check"] == "placement");
      CHECK((*stuck)["severity"] == "error");
      CHECK((*stuck)["details"]["reason"] == "box");
      CHECK((*stuck)["details"]["brushes"].size() == 1);
      CHECK((*stuck)["position"] == Json::array({520, 96, 24}));
      CHECK_THAT(
        (*stuck)["description"].get<std::string>(),
        Catch::Matchers::ContainsSubstring("monster_ogre")
          && Catch::Matchers::ContainsSubstring("stuck in solid"));

      // the light's origin lies inside the pillar
      const auto* light = findingFor(result, "ENTITY_IN_SOLID", pillarLight);
      REQUIRE(light);
      CHECK((*light)["details"]["reason"] == "origin");

      // an info_null in a wall and standing entities are fine
      CHECK(!findingFor(result, "ENTITY_IN_SOLID", infoNull));

      // logic entities work anywhere, also inside a wall
      const auto buried =
        fixture
          .call(
            "entity_create_point",
            {{"classname", "trigger_relay"},
             {"position", {384, 300, 96}},
             {"properties", {{"targetname", "buried"}}}})["result"]["entity"]
          .get<std::string>();
      CHECK(findingsAbout(check(fixture, {"placement"}), buried).empty());
      CHECK(!findingFor(result, "ENTITY_IN_SOLID", dog));
      CHECK(!findingFor(result, "ENTITY_IN_SOLID", roomLight));

      SECTION("following the suggested fix")
      {
        CHECK((*stuck)["suggestedFix"]["tool"] == "objects_move");
        CHECK((*stuck)["suggestedFix"]["args"]["ids"] == Json::array({ogre}));
        applyFix(fixture, *stuck);
        const auto after = check(fixture, {"placement"});
        CHECK(findingsAbout(after, ogre).empty());
      }
    }

    SECTION("ENTITY_FLOATING")
    {
      const auto* floating = findingFor(result, "ENTITY_FLOATING", soldier);
      REQUIRE(floating);
      CHECK((*floating)["severity"] == "warning");
      CHECK((*floating)["details"]["gap"] == 100);
      CHECK((*floating)["suggestedFix"]["tool"] == "objects_move");
      CHECK(
        (*floating)["suggestedFix"]["args"]
        == Json{{"ids", Json::array({soldier})}, {"vector", Json::array({0, 0, -100})}});

      // a flying monster, a standing dog and a hanging light are fine
      CHECK(!findingFor(result, "ENTITY_FLOATING", scrag));
      CHECK(!findingFor(result, "ENTITY_FLOATING", dog));
      CHECK(!findingFor(result, "ENTITY_FLOATING", roomLight));

      applyFix(fixture, *floating);
      CHECK(findingsAbout(check(fixture, {"placement"}), soldier).empty());
    }

    SECTION("hidden floors count")
    {
      // the compiler builds hidden objects, so the dog still stands on the floor
      auto* floor = findNode(map.worldNode(), [](const mdl::Node& node) {
        const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
        return brushNode && brushNode->logicalBounds().max.z() == 0.0;
      });
      REQUIRE(floor);
      mdl::hideNodes(map, {floor});
      REQUIRE_FALSE(map.editorContext().visible(*floor));
      const auto hidden = check(fixture, {"placement"});
      CHECK(findingsAbout(hidden, dog).empty());
      CHECK(findingFor(hidden, "ENTITY_FLOATING", soldier));
    }

    SECTION("model placement")
    {
      fixture.call(
        "entity_definitions_set",
        {{"type", "external"},
         {"path", (getFixtureRoot() / "test" / "mcp" / "models.fgd").string()}});
      const auto createPerson = [&](const Json& position) {
        return fixture
          .call(
            "entity_create_point",
            {{"classname", "monster_person"},
             {"position", position},
             {"snapToGrid", false}})["result"]["entity"]
          .get<std::string>();
      };
      const auto standing = createPerson({200, 200, 24});
      const auto floating = createPerson({200, 288, 100});
      const auto below = createPerson({200, 100, 10});
      const auto penetrating = createPerson({310, 300, 24});
      const auto noFloor = createPerson({-200, 300, 24});
      // a light with a model that floats and reaches into the ceiling
      const auto lamp = fixture
                          .call(
                            "entity_create_point",
                            {{"classname", "light_lamp"},
                             {"position", {200, 200, 170}},
                             {"snapToGrid", false}})["result"]["entity"]
                          .get<std::string>();

      const auto models = check(fixture, {"placement"});
      CHECK(findingsAbout(models, standing).empty());

      // lights get no model checks, in map_check and issues_list alike
      CHECK(findingsAbout(models, lamp).empty());
      CHECK(
        fixture.call("issues_list", {{"codes", ModelCodes}, {"ids", {lamp}}})["total"]
        == 0);
      CHECK(
        fixture.call("issues_list", {{"codes", ModelCodes}, {"ids", {floating}}})["total"]
        == 1);

      const auto* floats = findingFor(models, "MODEL_FLOATING", floating);
      REQUIRE(floats);
      CHECK((*floats)["severity"] == "warning");
      CHECK((*floats)["suggestedFix"]["args"]["vector"] == Json::array({0, 0, -76}));
      CHECK(
        findingsAbout(models, floating) == std::vector<std::string>{"MODEL_FLOATING"});

      const auto* sinks = findingFor(models, "MODEL_BELOW_FLOOR", below);
      REQUIRE(sinks);
      CHECK((*sinks)["suggestedFix"]["args"]["vector"] == Json::array({0, 0, 14}));

      const auto* penetrates =
        findingFor(models, "MODEL_PENETRATES_BRUSHES", penetrating);
      REQUIRE(penetrates);
      CHECK((*penetrates)["suggestedFix"]["tool"] == "objects_move");

      const auto* unsupported = findingFor(models, "MODEL_NO_FLOOR", noFloor);
      REQUIRE(unsupported);
      CHECK((*unsupported)["suggestedFix"]["tool"].is_null());

      // following the fixes
      applyFix(fixture, *floats);
      applyFix(fixture, *sinks);
      applyFix(fixture, *penetrates);
      const auto fixed = check(fixture, {"placement"});
      CHECK(findingsAbout(fixed, floating).empty());
      CHECK(findingsAbout(fixed, below).empty());
      CHECK(findingsAbout(fixed, penetrating).empty());
    }
  }

  SECTION("player_start")
  {
    const auto result = check(fixture, {"player_start"});
    const auto missing = findings(result, "MISSING_PLAYER_START");
    REQUIRE(missing.size() == 1);
    CHECK(missing[0]["id"] == "check:MISSING_PLAYER_START:map");
    CHECK(missing[0]["severity"] == "error");
    CHECK(missing[0]["objectId"] == "world");
    CHECK(missing[0]["details"]["fromDefinitions"] == true);
    CHECK(
      missing[0]["details"]["startClasses"]
      == Json::array(
        {"info_player_coop",
         "info_player_deathmatch",
         "info_player_start",
         "info_player_start2"}));
    CHECK(findings(result, "MISSING_SINGLE_PLAYER_START").empty());

    SECTION("following the suggested fix")
    {
      const auto& fix = missing[0]["suggestedFix"];
      CHECK(fix["tool"] == "entity_create_point");
      CHECK(fix["args"]["classname"] == "info_player_start");
      const auto position = fix["args"]["position"];
      CHECK(position[0].get<double>() > 0);
      CHECK(position[0].get<double>() < 512);
      CHECK(position[1].get<double>() > 0);
      CHECK(position[1].get<double>() < 384);
      CHECK(position[2] == 24);

      const auto created = applyFix(fixture, missing[0]);
      CHECK(created["result"]["onFloor"] == true);
      CHECK(created["result"]["overlaps"] == Json::array());
      CHECK(check(fixture, {"player_start"})["items"] == Json::array());
    }

    SECTION("MISSING_SINGLE_PLAYER_START")
    {
      fixture.call(
        "entity_create_point",
        {{"classname", "info_player_deathmatch"},
         {"position", {64, 288, 24}},
         {"snapToGrid", false}});
      const auto deathmatch = check(fixture, {"player_start"});
      CHECK(findings(deathmatch, "MISSING_PLAYER_START").empty());
      const auto single = findings(deathmatch, "MISSING_SINGLE_PLAYER_START");
      REQUIRE(single.size() == 1);
      CHECK(single[0]["severity"] == "info");
      CHECK(single[0]["position"] == Json::array({64, 288, 24}));
      CHECK(
        single[0]["suggestedFix"]["args"]
        == Json{{"classname", "info_player_start"}, {"position", {64, 288, 24}}});

      applyFix(fixture, single[0]);
      CHECK(check(fixture, {"player_start"})["items"] == Json::array());
    }
  }

  SECTION("links")
  {
    const auto result = check(fixture, {"links"});

    SECTION("LINK_TARGET_MISSING")
    {
      const auto* broken = findingFor(result, "LINK_TARGET_MISSING", trigger);
      REQUIRE(broken);
      CHECK((*broken)["id"] == "check:LINK_TARGET_MISSING:" + trigger + ":target");
      CHECK((*broken)["details"]["name"] == "nothing_here");
      CHECK((*broken)["details"]["key"] == "target");
      CHECK(!findingFor(result, "LINK_TARGET_MISSING", button));
      CHECK(!findingFor(result, "LINK_TARGET_MISSING", relay));
      CHECK(
        (*broken)["suggestedFix"]["args"]
        == Json{{"ids", Json::array({trigger})}, {"keys", Json::array({"target"})}});

      applyFix(fixture, *broken);
      CHECK(!findingFor(check(fixture, {"links"}), "LINK_TARGET_MISSING", trigger));
    }

    SECTION("LINK_TARGET_MISSING with a similar name")
    {
      fixture.call(
        "entity_properties_set",
        {{"ids", Json::array({trigger})}, {"properties", {{"target", "Door2"}}}});
      const auto similar = check(fixture, {"links"});
      const auto* broken = findingFor(similar, "LINK_TARGET_MISSING", trigger);
      REQUIRE(broken);
      CHECK((*broken)["details"]["similar"] == "door2");
      CHECK_THAT(
        (*broken)["description"].get<std::string>(),
        Catch::Matchers::EndsWith("did you mean 'door2'?"));
      CHECK((*broken)["suggestedFix"]["tool"] == "entity_properties_set");
      applyFix(fixture, *broken);
      CHECK(!findingFor(check(fixture, {"links"}), "LINK_TARGET_MISSING", trigger));
    }

    SECTION("multi_manager keys")
    {
      const auto manager = fixture
                             .call(
                               "entity_create_point",
                               {{"classname", "multi_manager"},
                                {"position", {160, 160, 64}},
                                {"properties",
                                 {{"targetname", "mm"},
                                  {"door2", "0.5"},
                                  {"ghost#1", "1"}}}})["result"]["entity"]
                             .get<std::string>();
      const auto managed = check(fixture, {"links"});
      const auto* broken = findingFor(managed, "LINK_TARGET_MISSING", manager);
      REQUIRE(broken);
      CHECK((*broken)["details"]["key"] == "ghost#1");
      CHECK((*broken)["details"]["name"] == "ghost");
      CHECK(findings(managed, "LINK_TARGET_MISSING").size() == 2);
      // nothing triggers the multi_manager
      CHECK(findingFor(managed, "LINK_SOURCE_MISSING", manager));
    }

    SECTION("LINK_SOURCE_MISSING")
    {
      const auto* lonely = findingFor(result, "LINK_SOURCE_MISSING", lonelyDoor);
      REQUIRE(lonely);
      CHECK((*lonely)["details"]["name"] == "door_lonely");
      CHECK_THAT(
        (*lonely)["description"].get<std::string>(),
        Catch::Matchers::ContainsSubstring("stays locked"));
      CHECK(!findingFor(result, "LINK_SOURCE_MISSING", door));
      CHECK(!findingFor(result, "LINK_SOURCE_MISSING", relay));

      applyFix(fixture, *lonely);
      CHECK(findingsAbout(check(fixture, {"links"}), lonelyDoor).empty());
    }

    SECTION("NEEDS_TARGETNAME")
    {
      const auto* unnamed = findingFor(result, "NEEDS_TARGETNAME", unnamedRelay);
      REQUIRE(unnamed);
      CHECK((*unnamed)["details"]["suggestedName"] == "trigger_relay_1");
      CHECK(!findingFor(result, "NEEDS_TARGETNAME", relay));

      // with a name, nothing triggers it yet
      applyFix(fixture, *unnamed);
      const auto named = check(fixture, {"links"});
      CHECK(
        findingsAbout(named, unnamedRelay)
        == std::vector<std::string>{"LINK_SOURCE_MISSING"});

      fixture.call("entity_link", {{"source", button}, {"target", unnamedRelay}});
      CHECK(findingsAbout(check(fixture, {"links"}), unnamedRelay).empty());
    }
  }

  SECTION("materials")
  {
    const auto result = check(fixture, {"materials"});
    const auto missing = findings(result, "MISSING_MATERIAL");
    REQUIRE(missing.size() == 1);
    CHECK(missing[0]["id"] == "check:MISSING_MATERIAL:wall_old_x");
    CHECK(missing[0]["severity"] == "warning");
    CHECK(missing[0]["details"]["faceCount"] == 1);
    CHECK(missing[0]["details"]["similar"] == "wall_old_a");
    CHECK(missing[0]["objectIds"].size() == 1);
    CHECK(
      missing[0]["suggestedFix"]["args"]
      == Json{{"from", "wall_old_x"}, {"to", "wall_old_a"}, {"scope", "map"}});

    SECTION("following the suggested fix")
    {
      applyFix(fixture, missing[0]);
      CHECK(check(fixture, {"materials"})["items"] == Json::array());
    }

    SECTION("tool materials are info")
    {
      fixture.call(
        "material_apply",
        {{"material", "clip"}, {"ids", Json::array({missing[0]["objectId"]})}});
      const auto clip = findings(check(fixture, {"materials"}), "MISSING_MATERIAL");
      REQUIRE(clip.size() == 1);
      CHECK(clip[0]["details"]["material"] == "clip");
      CHECK(clip[0]["severity"] == "info");
    }
  }

  SECTION("rooms")
  {
    const auto result = check(fixture, {"rooms"});

    SECTION("ENTITY_OUTSIDE_HULL")
    {
      const auto* outside = findingFor(result, "ENTITY_OUTSIDE_HULL", health);
      REQUIRE(outside);
      CHECK((*outside)["severity"] == "error");
      CHECK((*outside)["details"]["gap"].is_null());
      CHECK(findings(result, "ENTITY_OUTSIDE_HULL").size() == 1);
      // the outside entity is not reported twice
      CHECK(!findingFor(result, "ENTITY_OUTSIDE_SPACES", health));

      applyFix(fixture, *outside);
      CHECK(findingsAbout(check(fixture, {"rooms"}), health).empty());
    }

    SECTION("ENTITY_OUTSIDE_HULL with a gap")
    {
      auto* westWall = findNode(map.worldNode(), [](const mdl::Node& node) {
        const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
        return brushNode && brushNode->logicalBounds().max.x() == 0.0;
      });
      fixture.call("objects_delete", {{"ids", Json::array({idOf(westWall)})}});

      const auto leaking = check(fixture, {"rooms"});
      const auto* outside = findingFor(leaking, "ENTITY_OUTSIDE_HULL", dog);
      REQUIRE(outside);
      CHECK((*outside)["details"]["gap"].is_object());
      // a missing wall is no hole that one box seals
      CHECK((*outside)["suggestedFix"].is_null());
      CHECK_THAT(
        (*outside)["description"].get<std::string>(),
        Catch::Matchers::ContainsSubstring("no suggested fix"));

      SECTION("a hole in the wall")
      {
        // the west wall (x -16..0) again, with a 64 x 64 hole
        for (const auto& [min, max] : std::vector<std::pair<Json, Json>>{
               {{-16, 0, 0}, {0, 160, 192}},
               {{-16, 224, 0}, {0, 384, 192}},
               {{-16, 160, 0}, {0, 224, 64}},
               {{-16, 160, 128}, {0, 224, 192}},
             })
        {
          fixture.call("brush_create_box", {{"min", min}, {"max", max}});
        }
        const auto holed = check(fixture, {"rooms"});
        const auto* hole = findingFor(holed, "ENTITY_OUTSIDE_HULL", dog);
        REQUIRE(hole);
        const auto& fix = (*hole)["suggestedFix"];
        REQUIRE(fix.is_object());
        CHECK(fix["tool"] == "brush_create_box");
        // the hole plus a cell of rim, recessed by a unit into the wall
        CHECK(fix["args"]["min"][0] == -15);
        CHECK(fix["args"]["max"][0] == -1);
        CHECK(fix["args"]["min"][1].get<double>() <= 160);
        CHECK(fix["args"]["max"][1].get<double>() >= 224);
        CHECK(fix["args"]["min"][2].get<double>() <= 64);
        CHECK(fix["args"]["max"][2].get<double>() >= 128);
        CHECK(
          fix["args"]["max"][1].get<double>() - fix["args"]["min"][1].get<double>()
          <= 96);
        CHECK(fix["args"].contains("material"));

        applyFix(fixture, *hole);
        CHECK(findings(check(fixture, {"rooms"}), "ENTITY_OUTSIDE_HULL").size() == 1);
        CHECK(!findingFor(check(fixture, {"rooms"}), "ENTITY_OUTSIDE_HULL", dog));
      }
    }

    SECTION("ENTITY_OUTSIDE_SPACES")
    {
      const auto* outside = findingFor(result, "ENTITY_OUTSIDE_SPACES", pillarLight);
      REQUIRE(outside);
      CHECK((*outside)["details"]["where"] == "solid");
      CHECK((*outside)["details"]["nearestSpace"]["id"].is_string());
      CHECK(findings(result, "ENTITY_OUTSIDE_SPACES").size() == 1);
      // next to a wall, in the room, or position-independent
      CHECK(!findingFor(result, "ENTITY_OUTSIDE_SPACES", ogre));
      CHECK(!findingFor(result, "ENTITY_OUTSIDE_SPACES", roomLight));
      CHECK(!findingFor(result, "ENTITY_OUTSIDE_SPACES", infoNull));

      applyFix(fixture, *outside);
      const auto after = check(fixture, {"rooms", "placement"});
      CHECK(findingsAbout(after, pillarLight).empty());
    }
  }

  SECTION("checks and ids filters")
  {
    const auto links = check(fixture, {"links", "placement"});
    CHECK(links["checksRun"] == Json::array({"placement", "links"}));
    for (const auto& item : links["items"])
    {
      CHECK((item["check"] == "links" || item["check"] == "placement"));
    }

    const auto one = fixture.call("map_check", {{"ids", Json::array({soldier})}});
    CHECK(
      one["checksRun"]
      == Json::array({"placement", "npc_spawn", "links", "materials", "rooms"}));
    const auto mapLevel = [](const std::string& check) {
      return Json{
        {"check", check},
        {"reason", "A map-level check; with ids it runs only when requested in checks."}};
    };
    CHECK(
      one["skipped"]
      == Json::array({mapLevel("player_start"), mapLevel("entity_budget")}));
    REQUIRE(one["total"] == 1);
    CHECK(one["items"][0]["objectId"] == soldier);

    // the pillar brush: its missing material
    const auto pillar = findings(
      check(fixture, {"materials"}), "MISSING_MATERIAL")[0]["details"]["brushes"][0];
    const auto pillarOnly = fixture.call(
      "map_check",
      {{"ids", Json::array({pillar})},
       {"checks", Json::array({"player_start", "materials"})}});
    CHECK(pillarOnly["checksRun"] == Json::array({"player_start", "materials"}));
    CHECK(pillarOnly["skipped"] == Json::array());
    CHECK(findings(pillarOnly, "MISSING_PLAYER_START").size() == 1);
    CHECK(findings(pillarOnly, "MISSING_MATERIAL").size() == 1);

    const auto soldierOnly = fixture.call(
      "map_check",
      {{"ids", Json::array({soldier})}, {"checks", Json::array({"materials"})}});
    CHECK(soldierOnly["items"] == Json::array());
  }

  SECTION("pagination")
  {
    const auto all = fixture.call("map_check", {{"limit", 1000}});
    const auto first = fixture.call("map_check", {{"limit", 2}});
    CHECK(first["items"].size() == 2);
    CHECK(first["total"] == all["total"]);
    CHECK(first["counts"] == all["counts"]);
    REQUIRE(first["nextCursor"].is_string());
    const auto second =
      fixture.call("map_check", {{"limit", 2}, {"cursor", first["nextCursor"]}});
    CHECK(second["items"][0] == all["items"][2]);
    CHECK(first["items"][0] == all["items"][0]);

    const auto fields =
      fixture.call("map_check", {{"limit", 1}, {"fields", Json::array({"id", "code"})}});
    CHECK(
      fields["items"][0]
      == Json{{"id", all["items"][0]["id"]}, {"code", all["items"][0]["code"]}});
  }

  SECTION("progress and cancellation")
  {
    SECTION("reports progress between checks")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          1200,
          "tools/call",
          Json{
            {"name", "map_check"},
            {"arguments", Json::object()},
            {"_meta", Json{{"progressToken", "check"}}},
          }));
      CHECK(!stream->response.has_value());

      fixture.scheduler().runPending();
      REQUIRE(stream->response.has_value());
      CHECK((*stream->response)["result"]["isError"] == false);
      REQUIRE(stream->notifications.size() == 8);
      for (const auto& notification : stream->notifications)
      {
        CHECK(notification["method"] == "notifications/progress");
        CHECK(notification["params"]["total"] == 7);
      }
      CHECK(stream->notifications.back()["params"]["progress"] == 7);
    }

    SECTION("can be cancelled")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          1201,
          "tools/call",
          Json{{"name", "map_check"}, {"arguments", Json::object()}}));
      REQUIRE(!stream->response.has_value());

      fixture.post(
        fixture.sessionId(),
        jsonrpc::makeNotification("notifications/cancelled", Json{{"requestId", 1201}}));
      fixture.scheduler().runPending();

      REQUIRE(stream->response.has_value());
      const auto& result = (*stream->response)["result"];
      CHECK(result["isError"] == true);
      CHECK(result["structuredContent"]["error"]["code"] == "CANCELLED");
    }
  }

  SECTION("invalid input")
  {
    CHECK(
      fixture.callExpectingError("map_check", {{"checks", Json::array({"bogus"})}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("map_check", {{"checks", Json::array()}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("map_check", {{"ids", Json::array({"entity:999999"})}})
        .code
      == ErrorCode::ObjectNotFound);
    CHECK(
      fixture.callExpectingError("map_check", {{"cursor", "garbage"}}).code
      == ErrorCode::InvalidArgument);
  }
}

} // namespace tb::mcp
