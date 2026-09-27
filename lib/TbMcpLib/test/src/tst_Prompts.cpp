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

#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Resources.h"
#include "mcp/ToolRegistry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

bool isWordChar(const char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/** Whether a word looks like a tool name: lower case snake_case with at least one '_'. */
bool looksLikeToolName(const std::string_view word)
{
  if (word.empty() || !std::islower(static_cast<unsigned char>(word.front())))
  {
    return false;
  }
  if (word.find('_') == std::string_view::npos || word.back() == '_')
  {
    return false;
  }
  return std::ranges::all_of(word, [](const char c) {
    return std::islower(static_cast<unsigned char>(c))
           || std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

struct ToolMentions
{
  /** Every text between a pair of backticks. */
  std::vector<std::string> backticked;
  /** The words outside backticks that look like tool names. */
  std::vector<std::string> plain;
};

/**
 * The texts name tools in backticks, and only tools: `map_check`. Snake_case words
 * outside backticks (classnames such as info_player_start) are not tools.
 */
ToolMentions toolMentions(const std::string_view text)
{
  auto result = ToolMentions{};
  auto outside = std::string{};
  auto inBackticks = false;
  auto current = std::string{};
  for (const auto c : text)
  {
    if (c == '`')
    {
      if (inBackticks)
      {
        result.backticked.push_back(current);
        current.clear();
      }
      inBackticks = !inBackticks;
      outside.push_back(' ');
    }
    else if (inBackticks)
    {
      current.push_back(c);
    }
    else
    {
      outside.push_back(c);
    }
  }
  REQUIRE(!inBackticks);

  auto word = std::string{};
  for (const auto c : outside + ' ')
  {
    if (isWordChar(c))
    {
      word.push_back(c);
    }
    else
    {
      if (looksLikeToolName(word))
      {
        result.plain.push_back(word);
      }
      word.clear();
    }
  }
  return result;
}

void checkToolMentions(McpToolFixture& fixture, const std::string_view text)
{
  const auto& tools = fixture.server().tools();
  const auto mentions = toolMentions(text);
  for (const auto& name : mentions.backticked)
  {
    CAPTURE(name);
    CHECK(tools.find(name) != nullptr);
  }
  for (const auto& word : mentions.plain)
  {
    // a tool name outside backticks would escape the check above
    CAPTURE(word);
    CHECK(tools.find(word) == nullptr);
  }
}

Json getPrompt(McpToolFixture& fixture, const std::string& name, Json arguments)
{
  const auto response =
    fixture.rpc("prompts/get", Json{{"name", name}, {"arguments", std::move(arguments)}});
  REQUIRE(response.contains("result"));
  return response["result"];
}

std::string promptText(const Json& result)
{
  const auto& messages = result["messages"];
  REQUIRE(messages.size() == 1);
  CHECK(messages[0]["role"] == "user");
  CHECK(messages[0]["content"]["type"] == "text");
  return messages[0]["content"]["text"].get<std::string>();
}

bool contains(const std::string& text, const std::string_view part)
{
  return text.find(part) != std::string::npos;
}

struct PromptCase
{
  std::string name;
  std::vector<std::string> required;
  std::vector<std::string> optional;
};

const auto Prompts = std::vector<PromptCase>{
  {"blockout_level", {"description"}, {"game", "style"}},
  {"populate_level", {}, {"difficulty", "theme", "spaces"}},
  {"lighting_pass", {}, {"mood", "spaces"}},
  {"texture_pass", {}, {"theme", "materials"}},
  {"fix_issues", {}, {"scope"}},
  {"compile_and_debug", {}, {"preset", "profile"}},
  {"explain_map", {}, {"focus"}},
  {"explain_entity", {"classname"}, {}},
  {"cleanup_map", {}, {"format", "exportPath"}},
};

} // namespace

TEST_CASE("Prompts")
{
  auto fixture = McpToolFixture{};

  SECTION("prompts/list lists every prompt with its arguments")
  {
    const auto prompts = fixture.rpc("prompts/list")["result"]["prompts"];
    REQUIRE(prompts.size() == Prompts.size());

    for (const auto& expected : Prompts)
    {
      CAPTURE(expected.name);
      const auto it = std::ranges::find_if(
        prompts, [&](const auto& prompt) { return prompt["name"] == expected.name; });
      REQUIRE(it != prompts.end());
      CHECK(!(*it)["title"].get<std::string>().empty());
      CHECK(!(*it)["description"].get<std::string>().empty());

      auto names = std::vector<std::string>{};
      for (const auto& argument : (*it)["arguments"])
      {
        CHECK(!argument["description"].get<std::string>().empty());
        const auto name = argument["name"].get<std::string>();
        const auto required = argument["required"].get<bool>();
        CHECK(
          required
          == (std::ranges::find(expected.required, name) != expected.required.end()));
        names.push_back(name);
      }
      auto expectedNames = expected.required;
      expectedNames.insert(
        expectedNames.end(), expected.optional.begin(), expected.optional.end());
      std::ranges::sort(names);
      std::ranges::sort(expectedNames);
      CHECK(names == expectedNames);
    }
  }

  SECTION("prompts/get returns one user message that names registered tools")
  {
    for (const auto& prompt : Prompts)
    {
      CAPTURE(prompt.name);
      auto arguments = Json::object();
      for (const auto& name : prompt.required)
      {
        arguments[name] = "value_of_" + name;
      }

      const auto result = getPrompt(fixture, prompt.name, arguments);
      CHECK(!result["description"].get<std::string>().empty());
      const auto text = promptText(result);
      CHECK(!toolMentions(text).backticked.empty());
      checkToolMentions(fixture, text);
      for (const auto& name : prompt.required)
      {
        CHECK(contains(text, "value_of_" + name));
      }
    }
  }

  SECTION("every variant of the prompts names registered tools")
  {
    for (const auto& [name, arguments] : std::vector<std::pair<std::string, Json>>{
           {"blockout_level", {{"description", "a hall"}, {"game", "Quake"}}},
           {"populate_level", {{"difficulty", "easy"}}},
           {"populate_level", {{"difficulty", "normal"}}},
           {"populate_level", {{"difficulty", "hard"}}},
           {"texture_pass", {{"materials", "base.wad"}}},
           {"fix_issues", {{"scope", "all"}}},
           {"compile_and_debug", {{"profile", "Full"}}},
           {"cleanup_map", {{"format", "obj"}}},
           {"cleanup_map", {{"format", "Valve"}}},
         })
    {
      CAPTURE(name, arguments);
      checkToolMentions(fixture, promptText(getPrompt(fixture, name, arguments)));
    }
  }

  SECTION("a missing required argument is an error")
  {
    for (const auto& name : {"blockout_level", "explain_entity"})
    {
      CAPTURE(name);
      const auto response =
        fixture.rpc("prompts/get", Json{{"name", name}, {"arguments", Json::object()}});
      REQUIRE(response.contains("error"));
      CHECK(response["error"]["code"] == jsonrpc::ErrorCode::InvalidParams);
    }
  }

  SECTION("optional arguments are inserted, missing ones get defaults")
  {
    const auto blockout = promptText(getPrompt(
      fixture,
      "blockout_level",
      {{"description", "two rooms and a corridor"},
       {"game", "Half-Life"},
       {"style", "Black Mesa labs"}}));
    CHECK(contains(blockout, "two rooms and a corridor"));
    CHECK(contains(blockout, "Half-Life"));
    CHECK(contains(blockout, "Black Mesa labs"));

    const auto defaultBlockout =
      promptText(getPrompt(fixture, "blockout_level", {{"description", "a hall"}}));
    CHECK(contains(defaultBlockout, "Use the active document's game"));

    const auto populate =
      promptText(getPrompt(fixture, "populate_level", {{"difficulty", "hard"}}));
    CHECK(contains(populate, "appear on hard only"));
    CHECK(contains(
      promptText(getPrompt(fixture, "populate_level", Json::object())),
      "a base population for every skill"));

    CHECK(contains(
      promptText(getPrompt(fixture, "compile_and_debug", Json::object())),
      "{\"preset\": \"normal\"}"));
    CHECK(contains(
      promptText(getPrompt(fixture, "compile_and_debug", {{"preset", "full"}})),
      "{\"preset\": \"full\"}"));
    CHECK(contains(
      promptText(getPrompt(fixture, "compile_and_debug", {{"profile", "Mine"}})),
      "{\"profile\": \"Mine\"}"));

    CHECK(contains(
      promptText(getPrompt(fixture, "fix_issues", Json::object())), "Scope: safe"));
    CHECK(contains(
      promptText(getPrompt(fixture, "fix_issues", {{"scope", "all"}})), "Scope: all"));

    CHECK(contains(
      promptText(getPrompt(fixture, "explain_entity", {{"classname", "func_door"}})),
      "{\"classname\": \"func_door\"}"));

    const auto cleanup = promptText(getPrompt(
      fixture, "cleanup_map", {{"format", "obj"}, {"exportPath", "/tmp/out.obj"}}));
    CHECK(contains(cleanup, "`document_export_obj`"));
    CHECK(contains(cleanup, "/tmp/out.obj"));
    CHECK(contains(
      promptText(getPrompt(fixture, "cleanup_map", Json::object())),
      "`document_export_map`"));

    // blank values count as missing
    CHECK(contains(
      promptText(getPrompt(fixture, "fix_issues", {{"scope", "  "}})), "Scope: safe"));
  }
}

TEST_CASE("AgentGuide")
{
  auto fixture = McpToolFixture{};

  SECTION("the resource returns the guide")
  {
    const auto response =
      fixture.rpc("resources/read", Json{{"uri", "trenchbroom://guide"}});
    REQUIRE(response.contains("result"));
    const auto& contents = response["result"]["contents"];
    REQUIRE(contents.size() == 1);
    CHECK(contents[0]["mimeType"] == "text/markdown");
    CHECK(contents[0]["text"] == std::string{agentGuide()});
  }

  SECTION("the guide names registered tools")
  {
    const auto guide = agentGuide();
    CHECK(toolMentions(guide).backticked.size() > 50);
    checkToolMentions(fixture, guide);
  }
}

} // namespace tb::mcp
