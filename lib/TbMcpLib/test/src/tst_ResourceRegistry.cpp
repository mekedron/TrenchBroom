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

#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/ServerState.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("ResourceRegistry")
{
  auto fixture = McpToolFixture{};
  auto& state = fixture.server().state();
  auto& session = *state.findSession(fixture.sessionId());

  auto registry = ResourceRegistry{};
  registry.add(ResourceDef{
    "test://static",
    "static",
    "Static",
    "A static resource",
    "application/json",
    [](ServerState&, Session&, const std::string& uri, const ResourceVariables&)
      -> Result<Json, ToolError> { return jsonResourceContents(uri, Json{{"a", 1}}); },
  });
  registry.addTemplate(ResourceTemplateDef{
    "test://items/{item}/info",
    "item-info",
    "Item Info",
    "Info about an item",
    "application/json",
    [](ServerState&, Session&, const std::string& uri, const ResourceVariables& variables)
      -> Result<Json, ToolError> {
      if (variables.at("item") == "missing")
      {
        return makeError(ErrorCode::ObjectNotFound, "missing");
      }
      return jsonResourceContents(uri, Json{{"item", variables.at("item")}});
    },
    [](ServerState&, Session&) {
      return std::vector<Json>{Json{{"uri", "test://items/1/info"}, {"name", "item 1"}}};
    },
  });

  SECTION("exists")
  {
    CHECK(registry.exists("test://static"));
    CHECK(registry.exists("test://items/7/info"));
    CHECK(!registry.exists("test://items/7/other"));
    CHECK(!registry.exists("test://other"));
  }

  SECTION("list")
  {
    const auto result = registry.list(state, session, std::nullopt);
    REQUIRE(result["resources"].size() == 2);
    CHECK(result["resources"][0]["uri"] == "test://static");
    CHECK(result["resources"][1]["uri"] == "test://items/1/info");

    const auto page = registry.list(state, session, std::nullopt, 1);
    CHECK(page["resources"].size() == 1);
    CHECK(page.contains("nextCursor"));
  }

  SECTION("listTemplates")
  {
    const auto result = registry.listTemplates();
    REQUIRE(result["resourceTemplates"].size() == 1);
    CHECK(result["resourceTemplates"][0]["uriTemplate"] == "test://items/{item}/info");
  }

  SECTION("read")
  {
    const auto staticContents = registry.read(state, session, "test://static");
    REQUIRE(staticContents.is_success());
    CHECK(staticContents.value()[0]["text"] == R"({"a":1})");

    const auto item = registry.read(state, session, "test://items/7/info");
    REQUIRE(item.is_success());
    CHECK(item.value()[0]["text"] == R"({"item":"7"})");

    CHECK(registry.read(state, session, "test://items/missing/info").is_error());
    CHECK(registry.read(state, session, "test://nothing").is_error());
  }

  SECTION("matchUriTemplate")
  {
    CHECK(
      matchUriTemplate("a://{x}/b/{y}", "a://1/b/2")
      == ResourceVariables{{"x", "1"}, {"y", "2"}});
    CHECK(matchUriTemplate("a://{x}/b", "a://1/c") == std::nullopt);
    CHECK(matchUriTemplate("a://{x}", "a://1/2") == std::nullopt);
    CHECK(matchUriTemplate("a://{x}", "a://") == std::nullopt);
    CHECK(matchUriTemplate("a://x", "a://x") == ResourceVariables{});
  }
}

} // namespace tb::mcp
