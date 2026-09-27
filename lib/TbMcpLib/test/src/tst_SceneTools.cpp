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
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mcp/tools/SceneTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Selection.h"
#include "mdl/Tag.h"
#include "mdl/TagMatcher.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

const auto MapPath = []() {
  return getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map";
};

template <typename F>
void forEachNode(mdl::Node& node, const F& f)
{
  f(node);
  for (auto* child : node.children())
  {
    forEachNode(*child, f);
  }
}

std::vector<mdl::EntityNode*> findEntities(mdl::Map& map, const std::string& classname)
{
  auto result = std::vector<mdl::EntityNode*>{};
  forEachNode(map.worldNode(), [&](mdl::Node& node) {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node);
        entityNode && entityNode->entity().classname() == classname)
    {
      result.push_back(entityNode);
    }
  });
  return result;
}

std::vector<mdl::BrushNode*> findBrushes(mdl::Map& map, const std::string& material)
{
  auto result = std::vector<mdl::BrushNode*>{};
  forEachNode(map.worldNode(), [&](mdl::Node& node) {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node);
        brushNode && brushNode->brush().face(0).materialName() == material)
    {
      result.push_back(brushNode);
    }
  });
  return result;
}

mdl::GroupNode* findGroup(mdl::Map& map)
{
  auto* result = static_cast<mdl::GroupNode*>(nullptr);
  forEachNode(map.worldNode(), [&](mdl::Node& node) {
    if (auto* groupNode = dynamic_cast<mdl::GroupNode*>(&node))
    {
      result = groupNode;
    }
  });
  return result;
}

std::set<std::string> itemIds(const Json& page)
{
  auto result = std::set<std::string>{};
  for (const auto& item : page["items"])
  {
    result.insert(item["id"].get<std::string>());
  }
  return result;
}

std::set<std::string> idsOf(McpToolFixture& fixture, const auto& nodes)
{
  auto result = std::set<std::string>{};
  for (const auto* node : nodes)
  {
    result.insert(fixture.id(*node));
  }
  return result;
}

} // namespace

TEST_CASE("SceneTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.load(
    MapPath(), {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  auto& map = document.map();

  auto* arena = map.worldNode().customLayers().front();
  const auto arenaId = fixture.id(*arena);
  auto* pillars = findGroup(map);
  REQUIRE(pillars != nullptr);
  const auto pillarsId = fixture.id(*pillars);
  const auto playerStartId = fixture.id(*findEntities(map, "info_player_start").front());
  const auto doorId = fixture.id(*findEntities(map, "func_door").front());

  SECTION("map_summary")
  {
    const auto result = fixture.call("map_summary");
    CHECK(result["game"] == "Quake");
    CHECK(result["format"] == "Standard");
    CHECK(result["gridSize"].is_number());
    CHECK(
      result["counts"]
      == Json{
        {"layers", 2},
        {"groups", 1},
        {"linkedGroups", 0},
        {"entities", 8},
        {"pointEntities", 6},
        {"brushEntities", 2},
        {"brushes", 24},
        {"patches", 0},
        {"faces", 144},
      });
    CHECK(result["entitiesByClass"].begin().key() == "light");
    CHECK(result["entitiesByClass"]["light"] == 2);
    CHECK(result["entitiesByClass"].size() == 7);
    CHECK(result["otherClasses"] == 0);

    REQUIRE(result["layers"].size() == 2);
    CHECK(
      result["layers"][0]
      == Json{
        {"id", "layer:default"},
        {"name", "Default Layer"},
        {"objects", 18},
        {"hidden", false},
        {"locked", false},
        {"current", true},
      });
    CHECK(result["layers"][1]["id"] == arenaId);
    CHECK(result["layers"][1]["name"] == "Arena");
    CHECK(result["layers"][1]["objects"] == 15);

    CHECK(result["materials"]["distinct"] == 10);
    CHECK(result["materials"]["top"][0] == Json{{"name", "wall_brick"}, {"faces", 42}});
    CHECK(result["bounds"] == Json{{"min", {-16, -16, -16}}, {"max", {1296, 528, 272}}});
    CHECK(result["issues"].is_number_integer());
    CHECK(result["worldspawn"]["message"] == "MCP sample: two rooms and a corridor");

    // the resource helper returns the same
    CHECK(
      mapSummary(map, fixture.server().state().documentState(document).ids) == result);
  }

  SECTION("map_tree")
  {
    SECTION("default depth")
    {
      const auto result = fixture.call("map_tree", Json{{"limit", 1000}});
      CHECK(result["total"] == 1 + 2 + 16 + 13);
      CHECK(
        result["items"][0]
        == Json{
          {"id", "world"},
          {"kind", "world"},
          {"label", "worldspawn"},
          {"depth", 0},
          {"parent", nullptr},
          {"childCount", 2},
        });
      CHECK(result["items"][1]["id"] == "layer:default");
      CHECK(result["items"][1]["parent"] == "world");
      CHECK(!result["items"][1].contains("descendants"));

      const auto& items = result["items"];
      const auto group = std::ranges::find_if(
        items, [&](const auto& item) { return item["id"] == pillarsId; });
      REQUIRE(group != items.end());
      CHECK((*group)["depth"] == 2);
      CHECK((*group)["label"] == "Pillars");
      CHECK((*group)["descendants"] == Json{{"brush", 2}});
    }

    SECTION("root and depth")
    {
      const auto result =
        fixture.call("map_tree", Json{{"root", pillarsId}, {"depth", 1}});
      CHECK(result["total"] == 3);
      CHECK(result["items"][1]["parent"] == pillarsId);
      CHECK(result["items"][1]["kind"] == "brush");
      CHECK(!result["items"][1].contains("bounds"));
    }

    SECTION("full detail")
    {
      const auto result = fixture.call(
        "map_tree", Json{{"root", pillarsId}, {"depth", 1}, {"detail", "full"}});
      CHECK(result["items"][1]["materials"] == Json::array({"pillar_stone"}));
      CHECK(result["items"][1]["bounds"].is_object());
      CHECK(result["items"][1]["state"]["visible"] == true);
    }

    SECTION("kinds")
    {
      const auto layers = fixture.call("map_tree", Json{{"kinds", {"layer"}}});
      CHECK(layers["total"] == 2);

      const auto entities =
        fixture.call("map_tree", Json{{"kinds", {"entity"}}, {"depth", 16}});
      CHECK(entities["total"] == 8);
    }

    SECTION("visibleOnly")
    {
      mdl::hideNodes(map, {arena});
      const auto result = fixture.call(
        "map_tree", Json{{"visibleOnly", true}, {"limit", 1000}, {"depth", 16}});
      CHECK(result["total"] == 1 + 1 + 18);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("map_tree", Json{{"depth", 0}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("map_tree", Json{{"kinds", {"face"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("map_tree", Json{{"root", "brush:999999"}}).code
        == ErrorCode::ObjectNotFound);
    }
  }

  SECTION("object_get")
  {
    SECTION("entity")
    {
      const auto result = fixture.call("object_get", Json{{"ids", {playerStartId}}});
      REQUIRE(result["objects"].size() == 1);
      const auto& object = result["objects"][0];
      CHECK(object["id"] == playerStartId);
      CHECK(object["kind"] == "entity");
      CHECK(object["classname"] == "info_player_start");
      CHECK(object["parent"] == "layer:default");
      CHECK(object["layer"] == "layer:default");
      CHECK(object["group"] == nullptr);
      CHECK(
        object["properties"]
        == Json{
          {"classname", "info_player_start"}, {"origin", "64 64 24"}, {"angle", "0"}});
      CHECK(object["origin"] == Json{64, 64, 24});
      CHECK(object["angle"] == 0);
      CHECK(object["state"]["visible"] == true);
      CHECK(object["lineNumber"].is_number_integer());
      CHECK(object["linkId"].is_string());
    }

    SECTION("brush entity")
    {
      const auto result = fixture.call("object_get", Json{{"ids", {doorId}}});
      const auto& object = result["objects"][0];
      CHECK(object["brushes"] == 1);
      CHECK(!object.contains("origin"));
      CHECK(object["layer"] == arenaId);
    }

    SECTION("brush and face")
    {
      auto* doorBrush = findBrushes(map, "door_metal").front();
      const auto brushId = fixture.id(*doorBrush);
      const auto result =
        fixture.call("object_get", Json{{"ids", {brushId, brushId + "/face:2"}}});
      REQUIRE(result["objects"].size() == 2);

      const auto& brush = result["objects"][0];
      CHECK(brush["kind"] == "brush");
      CHECK(brush["entity"] == doorId);
      CHECK(brush["parent"] == doorId);
      CHECK(brush["materials"] == Json::array({"door_metal"}));
      REQUIRE(brush["faces"].size() == 6);
      CHECK(brush["faces"][0]["material"] == "door_metal");
      CHECK(brush["faces"][0]["scale"] == Json{1, 1});
      CHECK(brush["faces"][0]["offset"] == Json{0, 0});
      CHECK(brush["faces"][0]["rotation"] == 0);

      const auto& face = result["objects"][1];
      CHECK(face["kind"] == "face");
      CHECK(face["id"] == brushId + "/face:2");
      CHECK(face["brush"] == brushId);
      CHECK(face["index"] == 2);
      CHECK(face["material"] == "door_metal");
      CHECK(face["vertices"].size() == 4);
    }

    SECTION("group and layer")
    {
      const auto result =
        fixture.call("object_get", Json{{"ids", {pillarsId, arenaId, "world"}}});
      const auto& group = result["objects"][0];
      CHECK(group["kind"] == "group");
      CHECK(group["name"] == "Pillars");
      CHECK(group["linked"] == false);
      CHECK(group["persistentId"] == 2);
      CHECK(group["childCount"] == 2);

      const auto& layer = result["objects"][1];
      CHECK(layer["name"] == "Arena");
      CHECK(layer["sortIndex"] == 0);
      CHECK(layer["persistentId"] == 1);
      CHECK(layer["default"] == false);
      CHECK(layer["current"] == false);
      CHECK(layer["omitFromExport"] == false);

      const auto& world = result["objects"][2];
      CHECK(world["classname"] == "worldspawn");
      CHECK(world["layers"] == 2);
    }

    SECTION("fields and summary")
    {
      const auto result = fixture.call(
        "object_get",
        Json{{"ids", {playerStartId, doorId}}, {"fields", {"id", "classname"}}});
      CHECK(
        result["objects"]
        == Json{
          {{"id", playerStartId}, {"classname", "info_player_start"}},
          {{"id", doorId}, {"classname", "func_door"}},
        });
      CHECK(!result.contains("warnings"));

      // unknown fields are ignored with a warning; fields that some objects have are not
      // unknown
      const auto unknown = fixture.call(
        "object_get",
        Json{
          {"ids", {playerStartId, doorId}},
          {"fields", {"id", "origin", "vertices", "faces.bogus"}}});
      CHECK(unknown["objects"][1] == Json{{"id", doorId}});
      REQUIRE(unknown["warnings"].size() == 1);
      CHECK(unknown["warnings"][0]["code"] == "UNKNOWN_FIELD");
      const auto message = unknown["warnings"][0]["message"].get<std::string>();
      CHECK(message.find("'vertices', 'faces.bogus'") != std::string::npos);
      CHECK(message.find("origin") == std::string::npos);

      const auto summary =
        fixture.call("object_get", Json{{"ids", {playerStartId}}, {"detail", "summary"}});
      CHECK(!summary["objects"][0].contains("properties"));
      CHECK(summary["objects"][0].contains("state"));
    }

    SECTION("unknown ids")
    {
      const auto error = fixture.callExpectingError(
        "object_get", Json{{"ids", {playerStartId, "brush:999999", "entity:999998"}}});
      CHECK(error.code == ErrorCode::ObjectNotFound);
      CHECK(error.objectIds == std::vector<std::string>{"brush:999999", "entity:999998"});

      auto* brush = findBrushes(map, "trigger").front();
      CHECK(
        fixture
          .callExpectingError(
            "object_get", Json{{"ids", {fixture.id(*brush) + "/face:6"}}})
          .code
        == ErrorCode::ObjectNotFound);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("object_get", Json{{"ids", Json::array()}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("object_get", Json{{"ids", {"nonsense"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.callExpectingError("object_get").code == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_find")
  {
    SECTION("material")
    {
      const auto metal = fixture.call("objects_find", Json{{"material", "wall_metal"}});
      CHECK(itemIds(metal) == idsOf(fixture, findBrushes(map, "wall_metal")));
      CHECK(metal["total"] == 5);
      CHECK(metal["counts"] == Json{{"brush", 5}});
      CHECK(metal["items"][0]["materials"] == Json::array({"wall_metal"}));

      const auto walls = fixture.call("objects_find", Json{{"material", "WALL_*"}});
      CHECK(walls["total"] == 7 + 2 + 5);
    }

    SECTION("region")
    {
      const auto roomA = Json{{"min", {0, 0, 0}}, {"max", {512, 512, 256}}};

      const auto inside =
        fixture.call("objects_find", Json{{"region", roomA}, {"regionMode", "inside"}});
      const auto insideIds = itemIds(inside);
      CHECK(insideIds.contains(playerStartId));
      CHECK(insideIds.contains(fixture.id(*findEntities(map, "light").front())));
      CHECK(insideIds.contains(pillarsId));
      for (const auto* brush : findBrushes(map, "pillar_stone"))
      {
        CHECK(insideIds.contains(fixture.id(*brush)));
      }
      for (const auto* brush : findBrushes(map, "floor_stone"))
      {
        CHECK(!insideIds.contains(fixture.id(*brush)));
      }
      CHECK(!insideIds.contains(doorId));

      const auto intersecting = fixture.call("objects_find", Json{{"region", roomA}});
      const auto intersectingIds = itemIds(intersecting);
      CHECK(intersectingIds.contains(fixture.id(*findBrushes(map, "floor_stone")[0])));
      CHECK(intersectingIds.contains(playerStartId));
      CHECK(!intersectingIds.contains(doorId));
      CHECK(intersecting["total"] > inside["total"]);
    }

    SECTION("classname and properties")
    {
      const auto monsters =
        fixture.call("objects_find", Json{{"classname", "monster_*"}});
      CHECK(monsters["total"] == 2);
      CHECK(monsters["counts"] == Json{{"entity", 2}});

      const auto door = fixture.call(
        "objects_find",
        Json{{"property", {{"key", "targetname"}, {"value", "arena_door"}}}});
      CHECK(itemIds(door) == std::set<std::string>{doorId});

      const auto targets =
        fixture.call("objects_find", Json{{"property", {{"key", "target"}}}});
      CHECK(targets["total"] == 1);
      CHECK(targets["items"][0]["classname"] == "trigger_once");

      const auto world = fixture.call(
        "objects_find", Json{{"kinds", {"world"}}, {"classname", "worldspawn"}});
      CHECK(itemIds(world) == std::set<std::string>{"world"});
    }

    SECTION("layer and group")
    {
      const auto arenaObjects = fixture.call("objects_find", Json{{"layer", arenaId}});
      CHECK(arenaObjects["total"] == 15);
      CHECK(arenaObjects["counts"] == Json{{"entity", 5}, {"brush", 10}});

      const auto defaultEntities = fixture.call(
        "objects_find", Json{{"layer", "layer:default"}, {"kinds", {"entity"}}});
      CHECK(defaultEntities["total"] == 3);

      const auto pillarBrushes = fixture.call("objects_find", Json{{"group", pillarsId}});
      CHECK(itemIds(pillarBrushes) == idsOf(fixture, findBrushes(map, "pillar_stone")));

      CHECK(
        fixture.callExpectingError("objects_find", Json{{"layer", pillarsId}}).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("kinds, visibility and selection")
    {
      const auto layers = fixture.call("objects_find", Json{{"kinds", {"layer"}}});
      CHECK(layers["total"] == 2);

      const auto all = fixture.call("objects_find", Json{{"limit", 1}});
      CHECK(all["total"] == 1 + 8 + 24);

      mdl::hideNodes(map, {arena});
      const auto hidden = fixture.call("objects_find", Json{{"visible", false}});
      CHECK(hidden["total"] == 15);
      const auto visible =
        fixture.call("objects_find", Json{{"visible", true}, {"kinds", {"entity"}}});
      CHECK(visible["total"] == 3);

      mdl::selectNodes(map, {findEntities(map, "light").front()});
      const auto selected = fixture.call("objects_find", Json{{"selected", true}});
      CHECK(selected["total"] == 1);
      CHECK(selected["items"][0]["classname"] == "light");
    }

    SECTION("pagination, fields and detail")
    {
      auto seen = std::set<std::string>{};
      auto args = Json{{"kinds", {"entity"}}, {"limit", 3}, {"fields", {"id"}}};
      auto pages = 0;
      while (true)
      {
        const auto page = fixture.call("objects_find", args);
        CHECK(page["total"] == 8);
        for (const auto& item : page["items"])
        {
          CHECK(item.size() == 1);
          seen.insert(item["id"].get<std::string>());
        }
        ++pages;
        if (page["nextCursor"].is_null())
        {
          break;
        }
        args["cursor"] = page["nextCursor"];
      }
      CHECK(pages == 3);
      CHECK(seen.size() == 8);

      const auto full = fixture.call(
        "objects_find", Json{{"classname", "func_door"}, {"detail", "full"}});
      CHECK(full["items"][0]["properties"]["targetname"] == "arena_door");
      CHECK(full["items"][0]["state"]["selected"] == false);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "objects_find", Json{{"region", {{"min", {1, 1, 1}}, {"max", {0, 0, 0}}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_find", Json{{"regionMode", "touching"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_find", Json{{"property", {{"value", "x"}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_find", Json{{"group", "group:999999"}}).code
        == ErrorCode::ObjectNotFound);
    }

    SECTION("unknown tag")
    {
      const auto result = fixture.callRaw("objects_find", Json{{"tag", "trigger"}});
      CHECK(result["structuredContent"]["total"] == 0);
      CHECK(result["structuredContent"]["warnings"][0]["code"] == "UNKNOWN_TAG");
    }
  }

  SECTION("map_text_get")
  {
    SECTION("whole map with paging")
    {
      const auto first = fixture.call("map_text_get", Json{{"maxLines", 10}});
      CHECK(first["startLine"] == 1);
      CHECK(first["lineCount"] == 10);
      CHECK(first["truncated"] == true);
      CHECK(first["nextStartLine"] == 11);
      const auto totalLines = first["totalLines"].get<size_t>();
      CHECK(totalLines > 200);
      CHECK_THAT(
        first["text"].get<std::string>(),
        Catch::Matchers::ContainsSubstring("worldspawn"));

      const auto second =
        fixture.call("map_text_get", Json{{"startLine", 11}, {"maxLines", 10}});
      const auto both = fixture.call("map_text_get", Json{{"maxLines", 20}});
      CHECK(
        first["text"].get<std::string>() + second["text"].get<std::string>()
        == both["text"].get<std::string>());

      const auto last =
        fixture.call("map_text_get", Json{{"startLine", totalLines}, {"maxLines", 10}});
      CHECK(last["lineCount"] == 1);
      CHECK(last["truncated"] == false);
      CHECK(last["nextStartLine"] == nullptr);

      const auto all = fixture.call("map_text_get", Json{{"maxLines", 5000}});
      CHECK_THAT(
        all["text"].get<std::string>(), Catch::Matchers::ContainsSubstring("\"Arena\""));
      CHECK_THAT(
        all["text"].get<std::string>(),
        Catch::Matchers::ContainsSubstring("monster_knight"));
    }

    SECTION("objects")
    {
      const auto result = fixture.call("map_text_get", Json{{"ids", {doorId}}});
      const auto text = result["text"].get<std::string>();
      CHECK_THAT(text, Catch::Matchers::ContainsSubstring("func_door"));
      CHECK_THAT(text, Catch::Matchers::ContainsSubstring("door_metal"));
      CHECK_THAT(text, !Catch::Matchers::ContainsSubstring("worldspawn"));
      CHECK(result["truncated"] == false);

      // a brush of the world is written inside a worldspawn entity; duplicates are
      // dropped
      auto* brush = findBrushes(map, "trigger").front();
      const auto withChild = fixture.call(
        "map_text_get",
        Json{{"ids", {fixture.id(*brush->parent()), fixture.id(*brush)}}});
      const auto withChildText = withChild["text"].get<std::string>();
      CHECK(std::ranges::count(withChildText, '{') == 2); // one entity, one brush
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("map_text_get", Json{{"startLine", 100000}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("map_text_get", Json{{"maxLines", 5001}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("map_text_get", Json{{"ids", {"brush:999999"}}}).code
        == ErrorCode::ObjectNotFound);
    }
  }

  SECTION("map_stats")
  {
    const auto result = fixture.call("map_stats", Json{{"limit", 3}});
    CHECK(result["brushes"] == 24);
    CHECK(result["faces"] == 144);
    CHECK(result["patches"] == 0);
    CHECK(
      result["brushCountByEntity"]
      == Json{{"worldspawn", 22}, {"func_door", 1}, {"trigger_once", 1}});
    CHECK(result["entitiesByClass"]["light"] == 2);
    CHECK(result["materials"].size() == 3);
    CHECK(result["materials"][0] == Json{{"name", "wall_brick"}, {"faces", 42}});
    CHECK(result["distinctMaterials"] == 10);
    CHECK(result["materialsTruncated"] == true);
    REQUIRE(result["layers"].size() == 2);
    CHECK(
      result["layers"][1]
      == Json{
        {"id", arenaId},
        {"name", "Arena"},
        {"bounds", {{"min", {592, -16, -16}}, {"max", {1296, 528, 272}}}},
        {"brushes", 10},
        {"patches", 0},
        {"entities", 5},
        {"groups", 0},
      });
    CHECK(result["layers"][0]["brushes"] == 14);
    CHECK(result["groups"] == Json{{"count", 1}, {"linked", 0}});

    CHECK(
      fixture.callExpectingError("map_stats", Json{{"limit", 0}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("objects_find with smart tags")
  {
    auto gameInfo = mdl::QuakeGameInfo;
    gameInfo.gameConfig.smartTags = {
      mdl::SmartTag{
        "trigger",
        {},
        std::make_unique<mdl::EntityClassNameTagMatcher>("trigger*", "trigger"),
      },
    };

    auto tagFixture = McpToolFixture{};
    auto& tagDocument = tagFixture.load(
      MapPath(), {.mapFormat = mdl::MapFormat::Standard, .gameInfo = gameInfo});
    auto& tagMap = tagDocument.map();

    const auto result = tagFixture.call("objects_find", Json{{"tag", "trigger"}});
    const auto triggerBrush = tagFixture.id(*findBrushes(tagMap, "trigger").front());
    CHECK(itemIds(result).contains(triggerBrush));
    CHECK(result["counts"].value("entity", 0) + result["counts"].value("brush", 0) >= 1);

    const auto object = tagFixture.call("object_get", Json{{"ids", {triggerBrush}}});
    CHECK(object["objects"][0]["tags"] == Json::array({"trigger"}));
  }
}

TEST_CASE("SceneQuestions")
{
  // The questions of epic E3, answered with tool calls only, as an agent would.
  auto fixture = McpToolFixture{};
  fixture.load(
    MapPath(), {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});

  const auto labelsOf = [](const Json& items) {
    auto result = std::multiset<std::string>{};
    for (const auto& item : items)
    {
      result.insert(item["label"].get<std::string>());
    }
    return result;
  };

  SECTION("what is in this room")
  {
    // find the room: the player start is in it
    const auto starts =
      fixture.call("objects_find", Json{{"classname", "info_player_start"}});
    REQUIRE(starts["items"].size() == 1);

    // the room's inner box, extended into the floor slab because point entities
    // without a loaded definition have default bounds centered at their origin
    const auto room = Json{{"min", {0, 0, -16}}, {"max", {512, 512, 256}}};
    const auto contents = fixture.call(
      "objects_find",
      Json{{"region", room}, {"regionMode", "inside"}, {"kinds", {"entity", "group"}}});
    CHECK(
      labelsOf(contents["items"])
      == std::multiset<std::string>{
        "info_player_start", "light", "item_health", "Pillars"});
  }

  SECTION("what is under this entity")
  {
    const auto ogres = fixture.call("objects_find", Json{{"classname", "monster_ogre"}});
    REQUIRE(ogres["items"].size() == 1);
    const auto ogreId = ogres["items"][0]["id"].get<std::string>();

    const auto pick = fixture.call("ray_pick", Json{{"from", ogreId}});
    REQUIRE(pick["hit"].is_object());
    CHECK(pick["hit"]["material"] == "floor_metal");
    CHECK(pick["hit"]["point"][2] == 0);

    const auto floor = fixture.call("object_get", Json{{"ids", {pick["hit"]["object"]}}});
    CHECK(floor["objects"][0]["kind"] == "brush");
  }

  SECTION("which brushes use this material")
  {
    const auto brushes = fixture.call(
      "objects_find", Json{{"material", "wall_metal"}, {"kinds", {"brush"}}});
    CHECK(brushes["total"] == 5);
    for (const auto& item : brushes["items"])
    {
      CHECK(item["layer"].get<std::string>().starts_with("layer:"));
      CHECK(item["materials"] == Json{"wall_metal"});
    }
  }
}

} // namespace tb::mcp
