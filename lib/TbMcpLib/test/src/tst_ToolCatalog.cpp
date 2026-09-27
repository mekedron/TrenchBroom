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

#include "mcp/Json.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Schema.h"
#include "mcp/ToolRegistry.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

constexpr auto MinDescriptionLength = size_t{60};

/**
 * Returns the end (one past the closing brace) of the JSON object that starts at the
 * given position, honoring strings and escapes, or nothing if the braces do not balance.
 */
std::optional<size_t> findObjectEnd(const std::string_view text, const size_t start)
{
  auto depth = 0;
  auto inString = false;
  for (auto i = start; i < text.size(); ++i)
  {
    const auto c = text[i];
    if (inString)
    {
      if (c == '\\')
      {
        ++i;
      }
      else if (c == '"')
      {
        inString = false;
      }
    }
    else if (c == '"')
    {
      inString = true;
    }
    else if (c == '{')
    {
      ++depth;
    }
    else if (c == '}' && --depth == 0)
    {
      return i + 1;
    }
  }
  return std::nullopt;
}

/**
 * Checks the examples of a description: every JSON object that follows the first
 * "Example" must parse and pass the tool's input schema, and there must be at least one.
 */
void checkExamples(
  const ToolDef& tool,
  const std::string_view description,
  std::vector<std::string>& problems)
{
  const auto examplePos = description.find("Example");
  if (examplePos == std::string_view::npos)
  {
    problems.push_back("the description has no 'Example'");
    return;
  }

  const auto inputSchema = tool.inputSchema();
  auto count = size_t{0};
  auto pos = description.find('{', examplePos);
  while (pos != std::string_view::npos)
  {
    const auto end = findObjectEnd(description, pos);
    if (!end)
    {
      problems.push_back(
        "unbalanced braces in the example at: " + std::string{description.substr(pos)});
      return;
    }

    ++count;
    const auto text = description.substr(pos, *end - pos);
    if (const auto example = parseJson(text))
    {
      auto errors = std::vector<schema::SchemaError>{};
      if (!inputSchema.validate(*example, errors))
      {
        problems.push_back(
          "example " + std::string{text}
          + " fails the input schema: " + schema::formatErrors(errors));
      }
    }
    else
    {
      problems.push_back("example is not valid JSON: " + std::string{text});
    }
    pos = description.find('{', *end);
  }

  if (count == 0)
  {
    problems.push_back("no JSON object follows 'Example'");
  }
}

/**
 * Checks that every property of every object in the given published schema has a
 * description, recursing into properties, array items and oneOf branches.
 */
void checkPropertyDescriptions(
  const Json& schema, const std::string& path, std::vector<std::string>& problems)
{
  if (!schema.is_object())
  {
    return;
  }

  if (const auto* properties = findMember(schema, "properties"))
  {
    for (const auto& [name, property] : properties->items())
    {
      const auto propertyPath = path + "/" + name;
      const auto* description = findMember(property, "description");
      if (
        !description || !description->is_string()
        || description->get<std::string>().empty())
      {
        problems.push_back("input property " + propertyPath + " has no description");
      }
      checkPropertyDescriptions(property, propertyPath, problems);
    }
  }
  if (const auto* items = findMember(schema, "items"))
  {
    checkPropertyDescriptions(*items, path + "[]", problems);
  }
  if (const auto* alternatives = findMember(schema, "oneOf"))
  {
    for (size_t i = 0; i < alternatives->size(); ++i)
    {
      checkPropertyDescriptions(
        (*alternatives)[i], path + "|oneOf" + std::to_string(i), problems);
    }
  }
}

std::vector<std::string> catalogProblems(const ToolDef& tool, const Json& published)
{
  auto problems = std::vector<std::string>{};

  const auto* title = findMember(published, "title");
  if (!title || !title->is_string() || title->get<std::string>().empty())
  {
    problems.push_back("no title");
  }
  else if (title->get<std::string>() == tool.name())
  {
    problems.push_back("the title is not set (it equals the tool name)");
  }

  const auto description = published.value("description", std::string{});
  if (description.size() < MinDescriptionLength)
  {
    problems.push_back(
      "the description is shorter than " + std::to_string(MinDescriptionLength)
      + " characters");
  }
  if (description.empty() || !std::isupper(static_cast<unsigned char>(description[0])))
  {
    problems.push_back("the description does not start with an upper-case letter");
  }
  checkExamples(tool, description, problems);

  const auto* inputSchema = findMember(published, "inputSchema");
  if (!inputSchema || inputSchema->value("type", std::string{}) != "object")
  {
    problems.push_back("the input schema is not of type object");
  }
  else
  {
    checkPropertyDescriptions(*inputSchema, "", problems);
  }

  if (!findMember(published, "outputSchema"))
  {
    problems.push_back("no output schema");
  }

  return problems;
}

} // namespace

TEST_CASE("ToolCatalog")
{
  auto fixture = McpToolFixture{};

  const auto response = fixture.rpc("tools/list");
  REQUIRE(findMember(response, "result"));
  const auto& published = response["result"]["tools"];
  const auto& registry = fixture.server().tools();
  REQUIRE(published.size() == registry.tools().size());
  REQUIRE(!published.empty());

  for (const auto& entry : published)
  {
    const auto name = entry["name"].get<std::string>();
    const auto* tool = registry.find(name);
    REQUIRE(tool != nullptr);

    INFO("tool: " << name);
    const auto problems = catalogProblems(*tool, entry);
    CHECK(problems == std::vector<std::string>{});
  }
}

} // namespace tb::mcp
