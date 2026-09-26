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

#include "mcp/CallLog.h"

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

CallLogEntry makeEntry(std::string tool)
{
  auto entry = CallLogEntry{};
  entry.tool = std::move(tool);
  return entry;
}

} // namespace

TEST_CASE("CallLog")
{
  SECTION("add")
  {
    auto log = CallLog{3};
    for (int i = 0; i < 5; ++i)
    {
      log.add(makeEntry("tool" + std::to_string(i)));
    }

    REQUIRE(log.entries().size() == 3);
    CHECK(log.entries().front().tool == "tool2");
    CHECK(log.entries().front().seq == 3);
    CHECK(log.entries().back().seq == 5);
    CHECK(!log.entries().back().time.empty());
  }

  SECTION("sinks")
  {
    auto log = CallLog{};
    auto received = std::vector<std::string>{};
    const auto id =
      log.addSink([&](const auto& entry) { received.push_back(entry.tool); });

    log.add(makeEntry("a"));
    log.removeSink(id);
    log.add(makeEntry("b"));

    CHECK(received == std::vector<std::string>{"a"});
  }

  SECTION("truncateArguments")
  {
    const auto small = Json{{"a", 1}};
    CHECK(CallLog::truncateArguments(small) == small);

    const auto big = Json{{"a", std::string(5000, 'x')}};
    const auto truncated = CallLog::truncateArguments(big);
    REQUIRE(truncated.is_string());
    CHECK(truncated.get<std::string>().size() < 4200);
  }

  SECTION("toJson")
  {
    auto entry = CallLogEntry{};
    entry.tool = "undo";
    entry.ok = false;
    entry.errorCode = "NO_TRANSACTION";
    const auto json = toJson(entry);
    CHECK(json["tool"] == "undo");
    CHECK(json["ok"] == false);
    CHECK(json["errorCode"] == "NO_TRANSACTION");
    CHECK(json["undoStep"].is_null());
  }
}

TEST_CASE("JsonlFileSink")
{
  const auto dir = std::filesystem::temp_directory_path() / "tb-mcp-calllog-test";
  std::filesystem::remove_all(dir);

  {
    auto sink = JsonlFileSink{dir / "log.jsonl", 200};
    for (int i = 0; i < 4; ++i)
    {
      sink.write(makeEntry("tool"));
    }
    CHECK(sink.currentPath() != dir / "log.jsonl");
  }

  auto stream = std::ifstream{dir / "log.jsonl"};
  auto line = std::string{};
  REQUIRE(std::getline(stream, line));
  const auto parsed = parseJson(line);
  REQUIRE(parsed.has_value());
  CHECK((*parsed)["tool"] == "tool");
  CHECK(std::filesystem::exists(dir / "log.1.jsonl"));

  std::filesystem::remove_all(dir);
}

} // namespace tb::mcp
