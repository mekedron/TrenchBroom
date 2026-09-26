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

#include "mcp/PromptRegistry.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("PromptRegistry")
{
  auto registry = PromptRegistry{};
  registry.add(PromptDef{
    "explain_entity",
    "Explain Entity",
    "Explains an entity class",
    {PromptArgument{"classname", "The entity class", true}},
    [](const PromptArguments& arguments) -> Result<Json, ToolError> {
      return userTextMessage("Explain " + arguments.at("classname"));
    },
  });

  SECTION("find")
  {
    CHECK(registry.find("explain_entity") != nullptr);
    CHECK(registry.find("other") == nullptr);
  }

  SECTION("list")
  {
    const auto result = registry.list("2025-11-25", std::nullopt);
    REQUIRE(result["prompts"].size() == 1);
    CHECK(
      result["prompts"][0]
      == Json{
        {"name", "explain_entity"},
        {"title", "Explain Entity"},
        {"description", "Explains an entity class"},
        {"arguments",
         Json::array({Json{
           {"name", "classname"},
           {"description", "The entity class"},
           {"required", true}}})},
      });
    CHECK(!registry.list("2025-03-26", std::nullopt)["prompts"][0].contains("title"));
  }

  SECTION("get")
  {
    const auto result = registry.get("explain_entity", {{"classname", "light"}});
    REQUIRE(result.is_success());
    CHECK(result.value()["description"] == "Explains an entity class");
    CHECK(result.value()["messages"][0]["content"]["text"] == "Explain light");

    CHECK(registry.get("explain_entity", {}).is_error());
    CHECK(registry.get("other", {}).is_error());
  }
}

} // namespace tb::mcp
