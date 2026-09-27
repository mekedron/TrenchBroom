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

#include "mcp/ListDetail.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::vector<mdl::Node*> addBoxes(mdl::Map& map, const size_t count)
{
  auto nodes = std::vector<mdl::Node*>{};
  for (size_t i = 0; i < count; ++i)
  {
    const auto x = double(i) * 32.0;
    auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                   .createCuboid(vm::bbox3d{{x, 0, 0}, {x + 16, 16, 16}}, "stone")
                   .value();
    nodes.push_back(new mdl::BrushNode{std::move(brush)});
  }
  mdl::addNodes(map, {{&mdl::parentForNodes(map), nodes}});
  return nodes;
}

Json idList(const size_t count, const std::string& kind = "brush")
{
  auto result = Json::array();
  for (size_t i = 0; i < count; ++i)
  {
    result.push_back(kind + ":" + std::to_string(i + 1));
  }
  return result;
}

} // namespace

TEST_CASE("ListDetail")
{
  SECTION("listDetailFromString")
  {
    CHECK(listDetailFromString("summary") == ListDetail::Summary);
    CHECK(listDetailFromString("ids") == ListDetail::Ids);
    CHECK(listDetailFromString("full") == ListDetail::Full);
    CHECK(listDetailFromString("all") == std::nullopt);
  }

  SECTION("listLimit")
  {
    CHECK(listLimit(ListDetail::Summary) == 5);
    CHECK(listLimit(ListDetail::Ids) == 50);
    CHECK(listLimit(ListDetail::Full) > 1'000'000);
  }

  SECTION("itemKind")
  {
    CHECK(itemKind("brush:12") == "brush");
    CHECK(itemKind("brush:12/face:3") == "face");
    CHECK(itemKind("layer:default") == "layer");
    CHECK(itemKind(Json{{"id", "entity:4"}, {"label", "light"}}) == "entity");
    CHECK(
      itemKind(Json{{"objectId", "brush:1"}, {"code", "Z_FIGHTING"}}) == "Z_FIGHTING");
    CHECK(itemKind("stone") == std::nullopt);
    CHECK(itemKind(42) == std::nullopt);
  }

  SECTION("countsByKind")
  {
    CHECK(
      countsByKind(Json{"brush:1", "brush:2", "entity:3", "brush:1/face:0"})
      == Json{{"brush", 2}, {"entity", 1}, {"face", 1}});
  }

  SECTION("truncateIdLists")
  {
    auto json = Json{
      {"ids", idList(7)},
      {"count", 7},
      {"materials", {"a", "b", "c", "d", "e", "f", "g"}},
      {"objects", Json::array()},
      {"instances", {idList(3), idList(8, "entity")}},
      {"nested", {{"removed", idList(6)}}},
    };
    for (size_t i = 0; i < 7; ++i)
    {
      json["objects"].push_back(Json{{"id", "group:" + std::to_string(i)}});
    }
    auto truncated = std::vector<TruncatedList>{};
    truncateIdLists(json, "result", 5, truncated);

    CHECK(json["ids"] == idList(5));
    CHECK(json["count"] == 7);
    // not an id list
    CHECK(json["materials"].size() == 7);
    CHECK(json["objects"].size() == 5);
    CHECK(json["instances"][0].size() == 3);
    CHECK(json["instances"][1].size() == 5);
    CHECK(json["nested"]["removed"].size() == 5);

    REQUIRE(truncated.size() == 4);
    CHECK(truncated[0].path == "result.ids");
    CHECK(truncated[0].items == idList(7));
    CHECK(truncated[1].path == "result.objects");
    CHECK(truncated[2].path == "result.instances[1]");
    CHECK(truncated[3].path == "result.nested.removed");

    const auto described = truncatedListsJson(truncated, "lists:1", 5);
    CHECK(described["listsId"] == "lists:1");
    CHECK(
      described["lists"][0]
      == Json{
        {"path", "result.ids"},
        {"total", 7},
        {"shown", 5},
        {"byKind", {{"brush", 7}}},
      });
    CHECK(
      described["hint"].get<std::string>().find("result_list_get") != std::string::npos);
  }
}

TEST_CASE("ListDetail in tool results")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("small responses are unchanged")
  {
    const auto nodes = addBoxes(map, 3);
    const auto result = fixture.call(
      "objects_delete",
      Json{
        {"ids",
         Json::array(
           {fixture.id(*nodes[0]), fixture.id(*nodes[1]), fixture.id(*nodes[2])})}});
    CHECK(result["result"]["removed"].size() == 3);
    CHECK(result["changes"]["removed"].size() == 3);
    CHECK_FALSE(result["changes"].contains("truncated"));
    CHECK_FALSE(result["changes"].contains("counts"));
    CHECK_FALSE(result.contains("truncatedLists"));
  }

  SECTION("long lists are cut to 50 ids by default")
  {
    const auto nodes = addBoxes(map, 120);
    mdl::selectNodes(map, nodes);
    const auto result = fixture.call("objects_delete");

    CHECK(result["result"]["removed"].size() == 50);
    CHECK(result["result"]["count"] == 120);
    CHECK(result["changes"]["removed"].size() == 50);
    CHECK(result["changes"]["truncated"] == true);
    CHECK(result["changes"]["counts"]["removed"] == 120);
    CHECK(result["changes"]["countsByKind"]["removed"] == Json{{"brush", 120}});

    const auto& lists = result["truncatedLists"];
    REQUIRE(lists.is_object());
    const auto listsId = lists["listsId"].get<std::string>();
    auto paths = std::vector<std::string>{};
    for (const auto& list : lists["lists"])
    {
      paths.push_back(list["path"].get<std::string>());
      CHECK(list["total"] == 120);
      CHECK(list["shown"] == 50);
    }
    CHECK(paths == std::vector<std::string>{"result.removed", "changes.removed"});

    // the full list, paged
    const auto page = fixture.call(
      "result_list_get",
      Json{{"listsId", listsId}, {"path", "changes.removed"}, {"limit", 100}});
    CHECK(page["total"] == 120);
    CHECK(page["items"].size() == 100);
    CHECK(page["tool"] == "objects_delete");
    REQUIRE(page["nextCursor"].is_string());
    const auto rest = fixture.call(
      "result_list_get",
      Json{
        {"listsId", listsId},
        {"path", "changes.removed"},
        {"cursor", page["nextCursor"]}});
    CHECK(rest["items"].size() == 20);
    CHECK(rest["nextCursor"].is_null());

    CHECK(
      fixture
        .callExpectingError(
          "result_list_get", Json{{"listsId", listsId}, {"path", "result.ids"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "result_list_get", Json{{"listsId", "lists:999"}, {"path", "changes.removed"}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("detail summary")
  {
    const auto nodes = addBoxes(map, 12);
    mdl::selectNodes(map, nodes);
    const auto result = fixture.call("objects_duplicate", Json{{"detail", "summary"}});

    CHECK(result["changes"]["created"].size() == 5);
    CHECK(result["changes"]["counts"]["created"] == 12);
    CHECK(result["changes"]["countsByKind"]["created"] == Json{{"brush", 12}});
    CHECK(result["selection"]["count"] == 12);
    CHECK(result["selection"]["ids"].size() == 5);
    CHECK(result["selection"]["truncated"] == true);
    CHECK(result.contains("truncatedLists"));
  }

  SECTION("detail full")
  {
    const auto nodes = addBoxes(map, 120);
    mdl::selectNodes(map, nodes);
    const auto result = fixture.call("objects_delete", Json{{"detail", "full"}});
    CHECK(result["result"]["removed"].size() == 120);
    CHECK(result["changes"]["removed"].size() == 120);
    CHECK_FALSE(result["changes"].contains("truncated"));
    CHECK_FALSE(result.contains("truncatedLists"));
  }

  SECTION("dry runs cut lists, too")
  {
    const auto nodes = addBoxes(map, 60);
    mdl::selectNodes(map, nodes);
    const auto result = fixture.call("objects_delete", Json{{"dryRun", true}});
    CHECK(result["changes"]["removed"].size() == 50);
    CHECK(result.contains("truncatedLists"));
  }
}

} // namespace tb::mcp
