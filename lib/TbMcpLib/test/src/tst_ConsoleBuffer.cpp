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


#include "mcp/ConsoleBuffer.h"

#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::vector<uint64_t> seqs(const auto& messages)
{
  auto result = std::vector<uint64_t>{};
  for (const auto& message : messages)
  {
    result.push_back(message.seq);
  }
  return result;
}

std::vector<std::string> texts(const auto& messages)
{
  auto result = std::vector<std::string>{};
  for (const auto& message : messages)
  {
    result.push_back(message.text);
  }
  return result;
}

} // namespace

TEST_CASE("ConsoleBuffer")
{
  auto buffer = ConsoleBuffer{3};
  auto added = 0;
  auto cleared = 0;
  auto connection = buffer.messagesAddedNotifier.connect([&]() { ++added; });
  connection += buffer.clearedNotifier.connect([&]() { ++cleared; });

  SECTION("add")
  {
    const auto* document = reinterpret_cast<const ui::MapDocument*>(&buffer);
    buffer.add(LogLevel::Warn, "first", document, "first.map");
    buffer.add(LogLevel::Info, "second");

    REQUIRE(buffer.messages().size() == 2);
    const auto& first = buffer.messages().front();
    CHECK(first.seq == 1);
    CHECK(first.level == LogLevel::Warn);
    CHECK(first.text == "first");
    CHECK(first.document == document);
    CHECK(first.documentName == "first.map");
    CHECK(first.time.time_since_epoch().count() > 0);
    CHECK(buffer.messages().back().document == nullptr);
    CHECK(buffer.lastSeq() == 2);
    CHECK(added == 2);

    SECTION("ignores empty messages")
    {
      buffer.add(LogLevel::Error, "");
      CHECK(buffer.messages().size() == 2);
      CHECK(buffer.lastSeq() == 2);
      CHECK(added == 2);
    }
  }

  SECTION("drops the oldest messages when full")
  {
    for (const auto* text : {"a", "b", "c", "d", "e"})
    {
      buffer.add(LogLevel::Info, text);
    }
    CHECK(buffer.capacity() == 3);
    CHECK(texts(buffer.messages()) == std::vector<std::string>{"c", "d", "e"});
    CHECK(seqs(buffer.messages()) == std::vector<uint64_t>{3, 4, 5});
    CHECK(buffer.droppedCount() == 2);
    CHECK(buffer.lastSeq() == 5);
  }

  SECTION("capacity is at least 1")
  {
    auto tiny = ConsoleBuffer{0};
    tiny.add(LogLevel::Info, "a");
    tiny.add(LogLevel::Info, "b");
    CHECK(tiny.capacity() == 1);
    CHECK(texts(tiny.messages()) == std::vector<std::string>{"b"});
  }

  SECTION("messagesAfter")
  {
    for (const auto* text : {"a", "b", "c", "d"})
    {
      buffer.add(LogLevel::Info, text);
    }
    CHECK(seqs(buffer.messagesAfter(0)) == std::vector<uint64_t>{2, 3, 4});
    CHECK(seqs(buffer.messagesAfter(2)) == std::vector<uint64_t>{3, 4});
    CHECK(buffer.messagesAfter(4).empty());
    CHECK(buffer.messagesAfter(10).empty());
  }

  SECTION("clear")
  {
    buffer.add(LogLevel::Info, "a");
    buffer.add(LogLevel::Info, "b");
    buffer.clear();

    CHECK(buffer.messages().empty());
    CHECK(buffer.lastSeq() == 2);
    CHECK(buffer.droppedCount() == 0);
    CHECK(cleared == 1);

    // sequence numbers continue
    buffer.add(LogLevel::Info, "c");
    CHECK(seqs(buffer.messages()) == std::vector<uint64_t>{3});
    CHECK(seqs(buffer.messagesAfter(1)) == std::vector<uint64_t>{3});
  }
}

} // namespace tb::mcp
