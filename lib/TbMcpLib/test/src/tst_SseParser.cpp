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

#include "mcp/HttpResponse.h"
#include "mcp/SseParser.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("SseParser")
{
  SECTION("feed")
  {
    SECTION("single event")
    {
      auto parser = SseParser{};
      CHECK(
        parser.feed("event: message\nid: 1\ndata: {}\n\n")
        == std::vector<SseEvent>{{"message", "{}", "1", std::nullopt}});
    }

    SECTION("default event type and no id")
    {
      auto parser = SseParser{};
      const auto events = parser.feed("data: x\n\n");
      REQUIRE(events.size() == 1);
      CHECK(events[0].event == "message");
      CHECK(events[0].id == std::nullopt);
    }

    SECTION("multi-line data")
    {
      auto parser = SseParser{};
      const auto events = parser.feed("data: a\ndata:b\ndata:  c\n\n");
      REQUIRE(events.size() == 1);
      CHECK(events[0].data == "a\nb\n c");
    }

    SECTION("CRLF and CR line endings")
    {
      auto parser = SseParser{};
      const auto events = parser.feed("data: a\r\n\r\ndata: b\r\rdata: c\n\n");
      REQUIRE(events.size() == 3);
      CHECK(events[0].data == "a");
      CHECK(events[1].data == "b");
      CHECK(events[2].data == "c");
    }

    SECTION("CRLF split across feeds")
    {
      auto parser = SseParser{};
      CHECK(parser.feed("data: a\r").empty());
      CHECK(parser.feed("\n").empty());
      const auto events = parser.feed("\r\n");
      REQUIRE(events.size() == 1);
      CHECK(events[0].data == "a");
    }

    SECTION("comments, unknown fields, and events without data")
    {
      auto parser = SseParser{};
      CHECK(parser.feed(": keepalive\n\n").empty());
      CHECK(parser.feed("foo: bar\nevent: x\n\n").empty());
      const auto events = parser.feed("data\n\n");
      REQUIRE(events.size() == 1);
      CHECK(events[0].data.empty());
      CHECK(events[0].event == "message");
    }

    SECTION("id persists and retry is parsed")
    {
      auto parser = SseParser{};
      const auto events =
        parser.feed("id: 5\nretry: 1000\ndata: a\n\ndata: b\n\nretry: x\ndata: c\n\n");
      REQUIRE(events.size() == 3);
      CHECK(events[0].id == "5");
      CHECK(events[0].retry == 1000);
      CHECK(events[1].id == "5");
      CHECK(events[1].retry == std::nullopt);
      CHECK(events[2].retry == std::nullopt);
    }

    SECTION("byte order mark")
    {
      auto parser = SseParser{};
      const auto events = parser.feed(
        "\xEF\xBB\xBF"
        "data: a\n\n");
      REQUIRE(events.size() == 1);
      CHECK(events[0].data == "a");
    }

    SECTION("split into single bytes, round trip with encodeSseEvent")
    {
      const auto stream = encodeSseEvent(R"({"a":1})", "message", "1")
                          + std::string{sseKeepAliveComment()}
                          + encodeSseEvent("x\ny", "", "2");
      auto parser = SseParser{};
      auto events = std::vector<SseEvent>{};
      for (const auto c : stream)
      {
        for (auto& event : parser.feed(std::string(1, c)))
        {
          events.push_back(std::move(event));
        }
      }
      CHECK(
        events
        == std::vector<SseEvent>{
          {"message", R"({"a":1})", "1", std::nullopt},
          {"message", "x\ny", "2", std::nullopt},
        });
    }
  }
}

} // namespace tb::mcp
