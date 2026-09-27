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
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return fixture
    .call("brush_create_box", Json{{"min", min}, {"max", max}})["result"]["brush"]
    .get<std::string>();
}

std::vector<Json> itemsWith(const Json& result, const std::string& code)
{
  auto items = std::vector<Json>{};
  for (const auto& item : result["items"])
  {
    if (item["code"] == code)
    {
      items.push_back(item);
    }
  }
  return items;
}

} // namespace

TEST_CASE("ValidationTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("issues_list")
  {
    // an editor issue: an entity without classname
    auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
    const auto entityId = fixture.id(*entityNode);

    // an MCP issue: z-fighting
    const auto slab = createBox(fixture, {0, 0, 0}, {128, 128, 16});
    const auto inset = createBox(fixture, {32, 32, 8}, {96, 96, 16});
    const auto other = createBox(fixture, {512, 0, 0}, {576, 64, 64});

    SECTION("lists editor and MCP issues")
    {
      const auto result = fixture.call("issues_list");

      const auto missing = itemsWith(result, "MISSING_ENTITY_CLASSNAME");
      REQUIRE(missing.size() == 1);
      CHECK(missing[0]["source"] == "editor");
      CHECK(missing[0]["type"] == "Missing entity classname");
      CHECK(missing[0]["objectId"] == entityId);
      CHECK(missing[0]["id"].get<std::string>().starts_with("issue:"));
      CHECK(missing[0]["hidden"] == false);
      CHECK(missing[0]["lineNumber"].is_null());
      CHECK(!missing[0]["fixes"].empty());

      const auto zFighting = itemsWith(result, "Z_FIGHTING");
      REQUIRE(zFighting.size() == 1);
      CHECK(zFighting[0]["source"] == "mcp");
      CHECK(zFighting[0]["type"] == "Z-fighting");
      CHECK(zFighting[0]["details"]["area"] == 4096);
      const auto faces = zFighting[0]["details"]["brushes"];
      CHECK((faces == Json{slab, inset} || faces == Json{inset, slab}));

      CHECK(result["counts"]["Z_FIGHTING"] == 1);
      CHECK(result["counts"]["MISSING_ENTITY_CLASSNAME"] == 1);
      CHECK(result["total"] == result["items"].size());
      CHECK(result.contains("leakCheck"));
    }

    SECTION("filters by source, code and object")
    {
      auto result = fixture.call("issues_list", Json{{"sources", {"editor"}}});
      CHECK(itemsWith(result, "Z_FIGHTING").empty());
      CHECK(!itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      CHECK_FALSE(result.contains("leakCheck"));

      result = fixture.call("issues_list", Json{{"sources", {"mcp"}}});
      CHECK(itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      CHECK(itemsWith(result, "Z_FIGHTING").size() == 1);

      // codes and editor type names, case-insensitive
      result = fixture.call("issues_list", Json{{"codes", {"z_fighting"}}});
      CHECK(result["total"] == 1);
      CHECK_FALSE(result.contains("leakCheck"));
      result = fixture.call("issues_list", Json{{"codes", {"missing entity classname"}}});
      CHECK(result["total"] == 1);
      CHECK(result["items"][0]["objectId"] == entityId);

      result = fixture.call("issues_list", Json{{"ids", {other}}});
      CHECK(result["total"] == 0);
      result = fixture.call("issues_list", Json{{"ids", {inset}}});
      CHECK(itemsWith(result, "Z_FIGHTING").size() == 1);
      CHECK(itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      result = fixture.call("issues_list", Json{{"ids", {entityId}}});
      CHECK(!itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      CHECK(itemsWith(result, "Z_FIGHTING").empty());
    }

    SECTION("hidden issues")
    {
      const auto validators = map.worldNode().registeredValidators();
      for (const auto* issue : entityNode->issues(validators))
      {
        map.setIssueHidden(*issue, true);
      }

      auto result = fixture.call("issues_list", Json{{"sources", {"editor"}}});
      CHECK(itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());

      result = fixture.call(
        "issues_list", Json{{"sources", {"editor"}}, {"includeHidden", true}});
      const auto hidden = itemsWith(result, "MISSING_ENTITY_CLASSNAME");
      REQUIRE(hidden.size() == 1);
      CHECK(hidden[0]["hidden"] == true);
    }

    SECTION("pagination")
    {
      const auto all = fixture.call("issues_list");
      REQUIRE(all["total"].get<size_t>() >= 2);

      const auto first = fixture.call("issues_list", Json{{"limit", 1}});
      CHECK(first["items"].size() == 1);
      CHECK(first["total"] == all["total"]);
      REQUIRE(first["nextCursor"].is_string());

      const auto second =
        fixture.call("issues_list", Json{{"limit", 1}, {"cursor", first["nextCursor"]}});
      CHECK(second["items"].size() == 1);
      CHECK(second["items"][0] == all["items"][1]);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("issues_list", Json{{"ids", {"brush:999999999"}}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("issues_list", Json{{"sources", {"compiler"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("issues_list", Json{{"cursor", "garbage"}}).code
        == ErrorCode::InvalidArgument);
    }
  }
}

} // namespace tb::mcp
