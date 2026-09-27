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
#include "mcp/tools/EngineRules.h"
#include "mdl/Entity.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

/** A new Half-Life map without the default brush. */
mdl::Map& newHalfLifeMap(McpToolFixture& fixture)
{
  fixture.call("document_new", Json{{"game", "Half-Life"}});
  auto& map = fixture.host().documentList.back().document->map();
  mdl::selectAllNodes(map);
  mdl::removeSelectedNodes(map);
  return map;
}

std::string createBox(McpToolFixture& fixture, const vm::vec3d& min, const vm::vec3d& max)
{
  return resultOf(fixture.call(
    "brush_create_box",
    Json{
      {"min", {min.x(), min.y(), min.z()}},
      {"max", {max.x(), max.y(), max.z()}},
    }))["brush"]
    .get<std::string>();
}

std::string createPoint(
  McpToolFixture& fixture,
  const std::string& classname,
  const vm::vec3d& position,
  const Json& properties = Json::object())
{
  return resultOf(fixture.call(
    "entity_create_point",
    Json{
      {"classname", classname},
      {"position", {position.x(), position.y(), position.z()}},
      {"properties", properties},
      {"snapToGrid", false},
    }))["entity"]
    .get<std::string>();
}

std::vector<Json> findings(const Json& result, const std::string& code)
{
  auto items = std::vector<Json>{};
  std::ranges::copy_if(result["items"], std::back_inserter(items), [&](const auto& item) {
    return item["code"] == code;
  });
  return items;
}

bool listed(const Json& list, const std::string& value)
{
  return std::ranges::find(list, Json(value)) != list.end();
}

} // namespace

TEST_CASE("EngineRules")
{
  SECTION("engineRules")
  {
    const auto* halfLife = engineRules("halflife");
    REQUIRE(halfLife);
    CHECK(halfLife->budget.defaultLimit == 900);
    CHECK(halfLife->budget.maxLimit == 2048);
    CHECK_THAT(
      halfLife->budget.raiseLimit, Catch::Matchers::ContainsSubstring("-num_edicts"));
    CHECK(engineRules("quake")->budget.defaultLimit == 600);
    CHECK(engineRules("quake3") != nullptr);
    CHECK(engineRules("doom") == nullptr);
  }

  SECTION("monsterClass and engineHullBox")
  {
    const auto& rules = *engineRules("halflife");
    const auto definition = vm::bbox3d{{-8, -8, 0}, {8, 8, 16}};

    const auto scientist =
      monsterClass(rules, mdl::Entity{{{"classname", "monster_scientist"}}}, definition);
    REQUIRE(scientist);
    CHECK(scientist->bounds == vm::bbox3d{{-16, -16, 0}, {16, 16, 72}});
    CHECK(scientist->movement == MonsterMovement::Walk);

    // unknown monsters use the definition's size; corpses and other classes are none
    const auto custom =
      monsterClass(rules, mdl::Entity{{{"classname", "monster_custom"}}}, definition);
    REQUIRE(custom);
    CHECK(custom->bounds == definition);
    CHECK(!monsterClass(
      rules, mdl::Entity{{{"classname", "monster_scientist_dead"}}}, definition));
    CHECK(!monsterClass(rules, mdl::Entity{{{"classname", "info_target"}}}, definition));
    CHECK(
      monsterClass(rules, mdl::Entity{{{"classname", "monster_apache"}}}, definition)
        ->movement
      == MonsterMovement::Fly);

    // the human hull
    auto hull = engineHullBox(rules, {0, 0, 0}, scientist->bounds);
    CHECK(hull.hull.name == "human");
    CHECK(hull.box == vm::bbox3d{{-16, -16, 0}, {16, 16, 72}});

    // a headcrab (24 wide, 24 high) moves with the head hull, placed at its mins
    const auto headcrab =
      monsterClass(rules, mdl::Entity{{{"classname", "monster_headcrab"}}}, definition);
    hull = engineHullBox(rules, {100, 0, 0}, headcrab->bounds);
    CHECK(hull.hull.index == 3);
    CHECK(hull.box == vm::bbox3d{{88, -12, 0}, {120, 20, 36}});

    // an alien grunt moves with the large hull
    const auto grunt = monsterClass(
      rules, mdl::Entity{{{"classname", "monster_alien_grunt"}}}, definition);
    CHECK(engineHullBox(rules, {0, 0, 0}, grunt->bounds).hull.name == "large");

    // Quake chooses by width only
    const auto& quake = *engineRules("quake");
    CHECK(
      engineHullBox(quake, {0, 0, 0}, vm::bbox3d{{-16, -16, -24}, {16, 16, 40}})
        .hull.index
      == 1);
  }

  SECTION("map_check npc_spawn and entity_placement_check")
  {
    auto fixture = McpToolFixture{};
    newHalfLifeMap(fixture);
    const auto floor = createBox(fixture, {-512, -512, -16}, {512, 512, 0});
    const auto wall = createBox(fixture, {200, -512, 0}, {216, 512, 128});

    const auto standing = createPoint(fixture, "monster_scientist", {0, 0, 0});
    const auto inWall = createPoint(fixture, "monster_barney", {200, 0, 0});
    const auto flying = createPoint(fixture, "monster_alien_controller", {-300, 0, 64});
    // a sitting scientist never moves: touching the wall is harmless
    const auto sitting = createPoint(fixture, "monster_sitting_scientist", {190, 100, 0});
    const auto high = createPoint(fixture, "monster_scientist", {-200, -200, 100});
    const auto noFloor = createPoint(fixture, "monster_scientist", {-400, 400, 2000});
    // two headcrabs 30 units apart: their 32 wide head hulls overlap
    const auto crabA = createPoint(fixture, "monster_headcrab", {0, 300, 0});
    const auto crabB = createPoint(fixture, "monster_headcrab", {30, 300, 0});

    // a func_wall is solid, a func_illusionary is not
    const auto wallBrush = createBox(fixture, {-120, -140, 0}, {-100, -100, 64});
    fixture.call(
      "entity_create_brush", Json{{"classname", "func_wall"}, {"ids", {wallBrush}}});
    const auto byWall = createPoint(fixture, "monster_zombie", {-120, -120, 0});
    const auto ghostBrush = createBox(fixture, {300, 300, 0}, {320, 320, 64});
    fixture.call(
      "entity_create_brush",
      Json{{"classname", "func_illusionary"}, {"ids", {ghostBrush}}});
    const auto byGhost = createPoint(fixture, "monster_zombie", {300, 300, 0});

    const auto result = fixture.call(
      "map_check", Json{{"checks", {"placement", "npc_spawn"}}, {"limit", 200}});

    const auto stuck = findings(result, "NPC_STUCK");
    const auto stuckIds = [&]() {
      auto ids = std::vector<std::string>{};
      for (const auto& finding : stuck)
      {
        ids.push_back(finding["objectId"].get<std::string>());
      }
      std::ranges::sort(ids);
      return ids;
    }();
    auto expected = std::vector<std::string>{inWall, crabA, crabB, byWall};
    std::ranges::sort(expected);
    CHECK(stuckIds == expected);

    const auto inWallFinding = *std::ranges::find_if(
      stuck, [&](const auto& finding) { return finding["objectId"] == inWall; });
    CHECK(inWallFinding["severity"] == "error");
    CHECK(listed(inWallFinding["objectIds"], wall));
    CHECK(inWallFinding["details"]["hull"]["name"] == "human");
    CHECK_THAT(
      inWallFinding["description"].get<std::string>(),
      Catch::Matchers::ContainsSubstring("stuck in wall"));
    CHECK(inWallFinding["suggestedFix"]["description"].is_string());

    const auto noSupport = findings(result, "NPC_NO_SUPPORT");
    REQUIRE(noSupport.size() == 1);
    CHECK(noSupport[0]["objectId"] == noFloor);

    const auto drops = findings(result, "NPC_DROPS");
    REQUIRE(drops.size() == 1);
    CHECK(drops[0]["objectId"] == high);
    CHECK(drops[0]["details"]["drop"] == 100);
    CHECK(drops[0]["details"]["support"] == floor);
    CHECK(drops[0]["suggestedFix"]["args"]["vector"] == Json{0, 0, -100});

    // the monsters that stand fine have no findings, and placement leaves monsters out
    for (const auto& item : result["items"])
    {
      CHECK(item["objectId"] != standing);
      CHECK(item["objectId"] != flying);
      CHECK(item["objectId"] != sitting);
      CHECK(item["objectId"] != byGhost);
      CHECK(item["check"] == "npc_spawn");
    }

    // entity_placement_check reports the same spawn emulation
    const auto placement =
      fixture.call("entity_placement_check", Json{{"ids", {inWall}}});
    REQUIRE(placement["items"].size() == 1);
    const auto& item = placement["items"][0];
    CHECK(item["spawn"]["hull"]["name"] == "human");
    CHECK(listed(item["spawn"]["stuckIn"], wall));
    CHECK(std::ranges::any_of(item["findings"], [](const auto& finding) {
      return finding["code"] == "NPC_STUCK";
    }));

    const auto sittingItem =
      fixture.call("entity_placement_check", Json{{"ids", {sitting}}})["items"][0];
    CHECK(sittingItem["spawn"]["movement"] == "static");
    CHECK(listed(sittingItem["spawn"]["stuckIn"], wall));
    CHECK(sittingItem["findings"].empty());

    const auto standingItem =
      fixture.call("entity_placement_check", Json{{"ids", {standing}}})["items"][0];
    CHECK(standingItem["spawn"]["drop"] == 0);
    CHECK(standingItem["spawn"]["support"] == floor);
  }

  SECTION("map_check entity_budget")
  {
    auto fixture = McpToolFixture{};
    newHalfLifeMap(fixture);
    createBox(fixture, {-512, -512, -16}, {512, 512, 0});

    for (auto i = 0; i < 5; ++i)
    {
      createPoint(
        fixture,
        "info_target",
        {double(i) * 32, 0, 32},
        Json{{"targetname", "t" + std::to_string(i)}});
    }
    // lights without targetname are removed by the game, compiler entities by hlcsg
    for (auto i = 0; i < 3; ++i)
    {
      createPoint(fixture, "light", {double(i) * 32, 64, 32});
    }
    createPoint(fixture, "light", {0, 128, 32}, Json{{"targetname", "switch"}});
    createPoint(fixture, "info_texlights", {0, 192, 32});
    createPoint(
      fixture, "env_laser", {0, 256, 32}, Json{{"EndSprite", "sprites/glow01.spr"}});
    createPoint(fixture, "monstermaker", {0, 320, 32}, Json{{"m_imaxlivechildren", "3"}});

    auto result = fixture.call("map_check", Json{{"checks", {"entity_budget"}}});
    const auto& budget = result["entityBudget"];
    CHECK(budget["engine"] == "GoldSrc");
    CHECK(budget["limit"] == 900);
    CHECK(budget["maxLimit"] == 2048);
    CHECK(budget["mapEntities"] == 13);
    CHECK(budget["removedByCompiler"] == 1);
    CHECK(budget["removedAtSpawn"] == 3);
    CHECK(budget["spawned"] == 8);
    CHECK(budget["extra"] == 4);
    CHECK(budget["reserved"] == 66);
    CHECK(budget["total"] == 78);
    CHECK(budget["topClasses"][0] == Json{{"classname", "info_target"}, {"count", 5}});
    CHECK(result["items"].empty());

    // near the limit
    result =
      fixture.call("map_check", Json{{"checks", {"entity_budget"}}, {"entityLimit", 80}});
    auto near = findings(result, "ENTITY_LIMIT_NEAR");
    REQUIRE(near.size() == 1);
    CHECK(near[0]["severity"] == "warning");
    CHECK(near[0]["objectId"] == "world");

    // over the limit: the finding explains -num_edicts
    result = fixture.call(
      "map_check",
      Json{{"checks", {"entity_budget"}}, {"entityLimit", 60}, {"entityReserve", 0}});
    CHECK(result["entityBudget"]["total"] == 14);
    CHECK(result["items"].empty());
    result =
      fixture.call("map_check", Json{{"checks", {"entity_budget"}}, {"entityLimit", 70}});
    const auto exceeded = findings(result, "ENTITY_LIMIT_EXCEEDED");
    REQUIRE(exceeded.size() == 1);
    CHECK(exceeded[0]["severity"] == "error");
    CHECK_THAT(
      exceeded[0]["suggestedFix"]["description"].get<std::string>(),
      Catch::Matchers::ContainsSubstring("-num_edicts"));

    // a map-level check: with ids only when requested
    result = fixture.call("map_check", Json{{"ids", {"layer:default"}}});
    CHECK(!listed(result["checksRun"], "entity_budget"));
  }

  SECTION("games without engine data are skipped")
  {
    auto fixture = McpToolFixture{};
    fixture.create();
    const auto result =
      fixture.call("map_check", Json{{"checks", {"npc_spawn", "entity_budget"}}});
    CHECK(result["skipped"].size() == 2);
  }
}

} // namespace tb::mcp
