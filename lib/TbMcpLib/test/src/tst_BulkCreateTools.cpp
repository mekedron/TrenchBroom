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
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/vec.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const mdl::BrushNode& brushOf(McpToolFixture& fixture, const Json& id)
{
  const auto* brushNode =
    dynamic_cast<const mdl::BrushNode*>(fixture.node(id.get<std::string>()));
  REQUIRE(brushNode);
  return *brushNode;
}

std::string materialOf(const mdl::BrushNode& brushNode, const vm::vec3d& normal)
{
  const auto& brush = brushNode.brush();
  const auto index = brush.findFace(normal);
  REQUIRE(index);
  return brush.face(*index).materialName();
}

const mdl::Entity& entityOf(McpToolFixture& fixture, const Json& id)
{
  const auto* entityNode =
    dynamic_cast<const mdl::EntityNode*>(fixture.node(id.get<std::string>()));
  REQUIRE(entityNode);
  return entityNode->entity();
}

std::string propertyOf(const mdl::Entity& entity, const std::string& key)
{
  const auto* value = entity.property(key);
  return value ? *value : std::string{};
}

size_t objectCount(mdl::Map& map)
{
  return map.worldNode().defaultLayer()->childCount();
}

} // namespace

TEST_CASE("brushes_create")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("boxes, shapes, hulls, materials, groups and brush entities")
  {
    const auto result = fixture.call(
      "brushes_create",
      Json{
        {"material", "wall"},
        {"faces", {{"top", "floor"}}},
        {"items",
         {
           {{"min", {0, 0, 0}}, {"max", {256, 256, 16}}},
           {{"min", {0, 0, 16}},
            {"max", {16, 256, 128}},
            {"faces", {{"sides", "brick"}}}},
           {{"shape", "stairs"},
            {"min", {64, 64, 16}},
            {"max", {128, 128, 48}},
            {"stepHeight", 16},
            {"sides", 7}},
           {{"points", {{200, 0, 16}, {232, 0, 16}, {200, 32, 16}, {200, 0, 48}}},
            {"material", "rock"},
            {"group", "rocks"}},
           {{"min", {100, 0, 16}}, {"max", {164, 8, 96}}, {"entity", "door"}},
           {{"min", {100, 248, 16}}, {"max", {164, 256, 96}}, {"entity", "door"}},
         }},
        {"brushEntities",
         {{"door", {{"classname", "func_door"}, {"properties", {{"speed", 100}}}}}}},
      });
    CHECK(result["undoStep"] == "AI: Create Brushes");
    const auto& created = result["result"];
    CHECK(created["count"] == 7);
    CHECK(created["brushesPerItem"] == Json{1, 1, 2, 1, 1, 1});
    REQUIRE(created["ids"].size() == 7);

    const auto& floor = brushOf(fixture, created["ids"][0]);
    CHECK(materialOf(floor, {0, 0, 1}) == "floor");
    CHECK(materialOf(floor, {0, 0, -1}) == "wall");
    const auto& wall = brushOf(fixture, created["ids"][1]);
    CHECK(materialOf(wall, {1, 0, 0}) == "brick");
    CHECK(materialOf(wall, {0, 0, 1}) == "floor");
    CHECK(materialOf(wall, {0, 0, -1}) == "wall");

    // groups and entities
    const auto& hull = brushOf(fixture, created["ids"][4]);
    CHECK(materialOf(hull, {0, 0, -1}) == "rock");
    const auto* group = dynamic_cast<const mdl::GroupNode*>(hull.parent());
    REQUIRE(group);
    CHECK(group->name() == "rocks");
    CHECK(created["groups"]["rocks"] == Json(fixture.id(*group)));

    const auto& door = brushOf(fixture, created["ids"][5]);
    const auto* doorEntity = dynamic_cast<const mdl::EntityNode*>(door.parent());
    REQUIRE(doorEntity);
    CHECK(doorEntity == brushOf(fixture, created["ids"][6]).parent());
    CHECK(propertyOf(doorEntity->entity(), "classname") == "func_door");
    CHECK(propertyOf(doorEntity->entity(), "speed") == "100");
    CHECK(created["entities"]["door"] == Json(fixture.id(*doorEntity)));

    // everything new is selected, in one undo step
    CHECK(map.selection().nodes.size() == 5);
    fixture.call("undo");
    CHECK(objectCount(map) == 0);
  }

  SECTION("invalid items are all reported and nothing is created")
  {
    const auto error = fixture.callExpectingError(
      "brushes_create",
      Json{
        {"items",
         {
           {{"min", {0, 0, 0}}, {"max", {64, 64, 64}}},
           {{"min", {0, 0, 0}}, {"max", {64, 64, 0}}},
           {{"points", {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}}}},
           {{"min", {0, 0, 0}}, {"max", {64, 64, 64}}, {"entity", "missing"}},
           {{"material", "wall"}},
         }},
      });
    CHECK(error.code == ErrorCode::InvalidArgument);
    CHECK(error.message.find("4 of 5 items are invalid") != std::string::npos);
    auto indices = std::vector<size_t>{};
    for (const auto& item : error.details["errors"])
    {
      indices.push_back(item["index"].get<size_t>());
    }
    CHECK(indices == std::vector<size_t>{1, 2, 3, 4});
    CHECK(error.details["errors"][0]["code"] == "INVALID_GEOMETRY");
    CHECK(objectCount(map) == 0);
  }

  SECTION("dry run")
  {
    const auto result = fixture.call(
      "brushes_create",
      Json{{"items", {{{"min", {0, 0, 0}}, {"max", {64, 64, 64}}}}}, {"dryRun", true}});
    CHECK(result["dryRun"] == true);
    CHECK(result["changes"]["created"].size() == 1);
    CHECK(result["changes"]["ephemeral"] == true);
    CHECK(objectCount(map) == 0);
  }

  SECTION("long results are cut to the detail level")
  {
    auto items = Json::array();
    for (int i = 0; i < 120; ++i)
    {
      items.push_back(Json{{"min", {i * 32, 0, 0}}, {"max", {i * 32 + 16, 16, 16}}});
    }
    const auto result =
      fixture.call("brushes_create", Json{{"items", items}, {"detail", "summary"}});
    CHECK(result["result"]["count"] == 120);
    CHECK(result["result"]["ids"].size() == 5);
    CHECK(result["changes"]["counts"]["created"] == 120);
    CHECK(result.contains("truncatedLists"));
    CHECK(objectCount(map) == 120);
  }

  SECTION("layer, default group and uv without loaded materials")
  {
    const auto layer = fixture.call(
      "layer_create",
      Json{{"name", "Shell"}, {"makeCurrent", false}})["result"]["layer"]["id"];
    const auto result = fixture.call(
      "brushes_create",
      Json{
        {"items",
         {
           {{"min", {0, 0, 0}}, {"max", {64, 64, 64}}},
           {{"min", {64, 0, 0}}, {"max", {128, 64, 64}}, {"group", ""}},
         }},
        {"layer", layer},
        {"group", "shell"},
        {"uv", "typical"},
      });
    const auto& created = result["result"];
    const auto& first = brushOf(fixture, created["ids"][0]);
    const auto& second = brushOf(fixture, created["ids"][1]);
    REQUIRE(first.parent());
    CHECK(Json(fixture.id(*first.parent())) == created["groups"]["shell"]);
    CHECK(Json(fixture.id(*first.parent()->parent())) == layer);
    CHECK(Json(fixture.id(*second.parent())) == layer);
    CHECK(created["uv"]["skipped"] == 12);
    CHECK(std::ranges::any_of(result["warnings"], [](const auto& warning) {
      return warning["code"] == "MATERIAL_NOT_LOADED";
    }));
  }
}

TEST_CASE("entities_create")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("entities with properties, links and a drop to the floor")
  {
    fixture.call(
      "brush_create_box", Json{{"min", {-256, -256, -16}}, {"max", {256, 256, 0}}});
    const auto existing = fixture.call(
      "entity_create_point",
      Json{
        {"classname", "info_target"},
        {"position", {0, 0, 64}},
        {"properties", {{"targetname", "goal"}}}})["result"]["entity"];

    const auto result = fixture.call(
      "entities_create",
      Json{
        {"items",
         {
           {{"classname", "light"},
            {"position", {0, 0, 128}},
            {"properties", {{"light", 300}, {"_color", {255, 128, 0}}}},
            {"ref", "lamp"}},
           {{"classname", "trigger_relay"},
            {"position", {64, 0, 32}},
            {"links", {{{"to", "lamp"}}, {{"to", existing}, {"key", "killtarget"}}}}},
           {{"classname", "monster_army"},
            {"position", {128, 0, 100}},
            {"angle", 180},
            {"dropToFloor", true}},
         }},
      });
    CHECK(result["undoStep"] == "AI: Create Entities");
    const auto& created = result["result"];
    REQUIRE(created["ids"].size() == 3);
    CHECK(created["refs"]["lamp"] == created["ids"][0]);
    CHECK(created["links"] == 2);
    CHECK(created["generatedNames"] == 1);
    CHECK(created["dropped"] == 1);

    const auto& lamp = entityOf(fixture, created["ids"][0]);
    CHECK(propertyOf(lamp, "light") == "300");
    CHECK(propertyOf(lamp, "_color") == "255 128 0");
    CHECK(propertyOf(lamp, "targetname") == "lamp");
    const auto& relay = entityOf(fixture, created["ids"][1]);
    CHECK(propertyOf(relay, "target") == "lamp");
    CHECK(propertyOf(relay, "killtarget") == "goal");
    const auto& army = entityOf(fixture, created["ids"][2]);
    CHECK(propertyOf(army, "angle") == "180");
    CHECK(army.origin().z() < 100.0);

    CHECK(map.selection().nodes.size() == 3);
    fixture.call("undo");
    CHECK(objectCount(map) == 2);
  }

  SECTION("generated names do not collide")
  {
    fixture.call(
      "entity_create_point",
      Json{
        {"classname", "info_target"},
        {"position", {0, 0, 64}},
        {"properties", {{"targetname", "light"}}}});
    const auto result = fixture.call(
      "entities_create",
      Json{
        {"items",
         {
           {{"classname", "light"}, {"position", {0, 0, 128}}, {"ref", "a"}},
           {{"classname", "light"}, {"position", {64, 0, 128}}},
           {{"classname", "trigger_relay"},
            {"position", {64, 0, 32}},
            {"links", {{{"to", "a"}}}}},
         }},
      });
    CHECK(propertyOf(entityOf(fixture, result["result"]["ids"][0]), "targetname") == "a");
  }

  SECTION("invalid items are all reported and nothing is created")
  {
    const auto error = fixture.callExpectingError(
      "entities_create",
      Json{
        {"items",
         {
           {{"classname", "light"}, {"position", {0, 0, 0}}, {"ref", "a"}},
           {{"classname", "bad name"}, {"position", {0, 0, 0}}},
           {{"classname", "light"}, {"position", {0, 0, 0}}, {"ref", "a"}},
           {{"classname", "light"},
            {"position", {0, 0, 0}},
            {"properties", {{"origin", "1 2 3"}}}},
           {{"classname", "light"}, {"position", {0, 0, 0}}, {"dropToFloor", true}},
           {{"classname", "relay"}, {"position", {0, 0, 0}}, {"links", {{{"to", "b"}}}}},
         }},
      });
    CHECK(error.code == ErrorCode::InvalidArgument);
    auto indices = std::vector<size_t>{};
    for (const auto& item : error.details["errors"])
    {
      indices.push_back(item["index"].get<size_t>());
    }
    CHECK(indices == std::vector<size_t>{1, 2, 3, 4, 5});
    CHECK(objectCount(map) == 0);
  }

  SECTION("group, layer and dry run")
  {
    const auto layer = fixture.call(
      "layer_create",
      Json{{"name", "Lights"}, {"makeCurrent", false}})["result"]["layer"]["id"];
    auto args = Json{
      {"items",
       {{{"classname", "light"}, {"position", {0, 0, 64}}},
        {{"classname", "light"}, {"position", {64, 0, 64}}}}},
      {"layer", layer},
      {"group", "lights"},
    };

    auto dryRunArgs = args;
    dryRunArgs["dryRun"] = true;
    const auto dryRun = fixture.call("entities_create", dryRunArgs);
    CHECK(dryRun["changes"]["ephemeral"] == true);
    CHECK(fixture.node(layer.get<std::string>())->childCount() == 0);

    const auto result = fixture.call("entities_create", args);
    const auto& group = result["result"]["group"];
    REQUIRE(group.is_string());
    CHECK(fixture.node(group.get<std::string>())->childCount() == 2);
    CHECK(Json(fixture.id(*fixture.node(group.get<std::string>())->parent())) == layer);
    CHECK(result["result"]["layer"] == layer);
  }
}

} // namespace tb::mcp
