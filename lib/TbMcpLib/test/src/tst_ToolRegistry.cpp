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

#include "mcp/Args.h"
#include "mcp/ToolRegistry.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
using namespace schema;

namespace
{

ToolResult noop(CallContext&, const Args&)
{
  return Json::object();
}

} // namespace

TEST_CASE("ToolRegistry")
{
  auto registry = ToolRegistry{};

  SECTION("add and find")
  {
    registry.add(ToolDef{"a_tool"}.title("A Tool").handler(noop));
    registry.add(ToolDef{"b_tool"}.handler(noop));

    REQUIRE(registry.find("a_tool") != nullptr);
    CHECK(registry.find("a_tool")->title() == "A Tool");
    CHECK(registry.find("b_tool")->title() == "b_tool");
    CHECK(registry.find("c_tool") == nullptr);
    CHECK(registry.tools().size() == 2);
  }

  SECTION("list")
  {
    registry.add(ToolDef{"a_tool"}.handler(noop));
    registry.add(ToolDef{"b_tool"}.handler(noop));
    registry.add(ToolDef{"c_tool"}.handler(noop));

    const auto all = registry.list("2025-11-25", std::nullopt);
    CHECK(all["tools"].size() == 3);
    CHECK(!all.contains("nextCursor"));

    const auto page1 = registry.list("2025-11-25", std::nullopt, 2);
    CHECK(page1["tools"].size() == 2);
    REQUIRE(page1.contains("nextCursor"));

    const auto page2 =
      registry.list("2025-11-25", page1["nextCursor"].get<std::string>(), 2);
    REQUIRE(page2["tools"].size() == 1);
    CHECK(page2["tools"][0]["name"] == "c_tool");
  }

  SECTION("isValidToolName")
  {
    CHECK(isValidToolName("brush_create_box"));
    CHECK(isValidToolName("a1"));
    CHECK(!isValidToolName(""));
    CHECK(!isValidToolName("1a"));
    CHECK(!isValidToolName("Brush"));
    CHECK(!isValidToolName("a-b"));
    CHECK(!isValidToolName(std::string(65, 'a')));
  }
}

TEST_CASE("ToolDef")
{
  SECTION("inputSchema")
  {
    SECTION("read-only tools get no standard parameters")
    {
      const auto tool = ToolDef{"t"}.input(object({field("x", number())})).handler(noop);
      const auto schema = tool.inputSchema();
      CHECK(schema.fields.size() == 1);
    }

    SECTION("map tools get document and dryRun")
    {
      const auto tool = ToolDef{"t"}.mutation(Mutation::Map).handler(noop);
      const auto schema = tool.inputSchema();
      CHECK(schema.findField("document") != nullptr);
      REQUIRE(schema.findField("dryRun") != nullptr);
      CHECK(schema.findField("dryRun")->schema.defaultValue == Json(false));
    }

    SECTION("paginated tools get the list parameters")
    {
      const auto tool = ToolDef{"t"}.paginated().handler(noop);
      const auto schema = tool.inputSchema();
      CHECK(schema.findField("cursor") != nullptr);
      CHECK(schema.findField("limit") != nullptr);
      CHECK(schema.findField("fields") != nullptr);
      CHECK(schema.findField("detail") != nullptr);
    }

    SECTION("declared parameters are not replaced")
    {
      const auto tool = ToolDef{"t"}
                          .input(object({field("document", documentId()).required()}))
                          .documentUse(DocumentUse::Optional)
                          .handler(noop);
      const auto schema = tool.inputSchema();
      REQUIRE(schema.findField("document") != nullptr);
      CHECK(schema.findField("document")->isRequired);
    }
  }

  SECTION("documentUse")
  {
    CHECK(ToolDef{"t"}.mutation(Mutation::Map).documentUse() == DocumentUse::Required);
    CHECK(ToolDef{"t"}.documentUse() == DocumentUse::None);
    CHECK(
      ToolDef{"t"}.documentUse(DocumentUse::Optional).documentUse()
      == DocumentUse::Optional);
  }

  SECTION("transactional")
  {
    CHECK(ToolDef{"t"}.mutation(Mutation::Map).transactional());
    CHECK(!ToolDef{"t"}.mutation(Mutation::Map).transactional(false).transactional());
    CHECK(!ToolDef{"t"}.mutation(Mutation::External).transactional());
  }

  SECTION("toJson")
  {
    const auto tool = ToolDef{"brush_create_box"}
                        .title("Create Box Brush")
                        .description("Creates a box.")
                        .input(object({field("min", vec3()).required()}))
                        .output(object({field("brush", objectId())}))
                        .mutation(Mutation::Map)
                        .handler(noop);

    const auto json = tool.toJson("2025-11-25");
    CHECK(json["name"] == "brush_create_box");
    CHECK(json["title"] == "Create Box Brush");
    CHECK(json["description"] == "Creates a box.");
    CHECK(json["inputSchema"]["required"] == Json::array({"min"}));
    CHECK(json["inputSchema"]["properties"].contains("dryRun"));
    CHECK(json["outputSchema"]["properties"]["result"]["properties"].contains("brush"));
    CHECK(!json["outputSchema"].contains("additionalProperties"));
    CHECK(
      json["annotations"]
      == Json{
        {"title", "Create Box Brush"},
        {"readOnlyHint", false},
        {"destructiveHint", false},
        {"idempotentHint", false},
        {"openWorldHint", false},
      });

    const auto old = tool.toJson("2025-03-26");
    CHECK(!old.contains("title"));
    CHECK(!old.contains("outputSchema"));
  }
}

} // namespace tb::mcp
