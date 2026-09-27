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


#include "fs/TestEnvironment.h"
#include "mcp/ConsoleBuffer.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Pagination.h"
#include "ui/MapDocument.h"

#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

using namespace std::chrono_literals;

const auto ConsoleUri = std::string{"trenchbroom://console"};

std::vector<std::string> texts(const Json& items)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item["text"].get<std::string>());
  }
  return result;
}

size_t consoleUpdates(const CapturingNotificationStream& stream)
{
  auto count = size_t{0};
  for (const auto& notification : stream.notifications)
  {
    if (
      notification["method"] == "notifications/resources/updated"
      && notification["params"]["uri"] == ConsoleUri)
    {
      ++count;
    }
  }
  return count;
}

Json readConsoleResource(McpToolFixture& fixture)
{
  const auto read = fixture.rpc("resources/read", Json{{"uri", ConsoleUri}});
  REQUIRE(read.contains("result"));
  const auto& contents = read["result"]["contents"];
  REQUIRE(contents.size() == 1);
  CHECK(contents[0]["mimeType"] == "application/json");
  return *parseJson(contents[0]["text"].get<std::string>());
}

} // namespace

TEST_CASE("ConsoleTools")
{
  auto fixture = McpToolFixture{};
  auto& console = fixture.host().console;

  auto& document = fixture.create();
  const auto documentId = fixture.documentId(document);
  // drop the messages logged while the document was created
  console.clear();

  SECTION("console_read")
  {
    console.add(LogLevel::Debug, "debug message");
    console.add(LogLevel::Info, "Loaded materials", &document, "unnamed1");
    console.add(LogLevel::Warn, "Missing texture wall_a", &document, "unnamed1");
    console.add(LogLevel::Error, "Could not load model", nullptr, "closed.map");
    console.add(LogLevel::Info, "no document");
    const auto firstSeq = console.messages().front().seq;

    SECTION("returns the messages of level info or higher, oldest first")
    {
      const auto result = fixture.call("console_read");
      CHECK(
        texts(result["items"])
        == std::vector<std::string>{
          "Loaded materials",
          "Missing texture wall_a",
          "Could not load model",
          "no document"});
      CHECK(result["total"] == 4);
      CHECK(result["nextCursor"].is_null());
      CHECK(result["lastSeq"] == console.lastSeq());
      CHECK(!result.contains("dropped"));

      const auto& warning = result["items"][1];
      CHECK(warning["seq"] == firstSeq + 2);
      CHECK(warning["level"] == "warning");
      CHECK(warning["time"].get<std::string>().ends_with("Z"));
      CHECK(warning["document"] == documentId);
      CHECK(!warning.contains("documentName"));

      const auto& error = result["items"][2];
      CHECK(error["level"] == "error");
      CHECK(!error.contains("document"));
      CHECK(error["documentName"] == "closed.map");

      CHECK(!result["items"][3].contains("document"));
      CHECK(!result["items"][3].contains("documentName"));
    }

    SECTION("filters by minimum level")
    {
      CHECK(
        fixture.call("console_read", Json{{"minLevel", "debug"}})["items"].size() == 5);
      CHECK(
        texts(fixture.call("console_read", Json{{"minLevel", "warning"}})["items"])
        == std::vector<std::string>{"Missing texture wall_a", "Could not load model"});
      CHECK(
        texts(fixture.call("console_read", Json{{"minLevel", "error"}})["items"])
        == std::vector<std::string>{"Could not load model"});
    }

    SECTION("filters by text")
    {
      CHECK(
        texts(fixture.call("console_read", Json{{"text", "MISSING texture"}})["items"])
        == std::vector<std::string>{"Missing texture wall_a"});
      CHECK(
        texts(fixture.call(
          "console_read",
          Json{{"text", "^(could not|missing)"}, {"regex", true}})["items"])
        == std::vector<std::string>{"Missing texture wall_a", "Could not load model"});
      // without regex, the pattern is plain text
      CHECK(fixture.call("console_read", Json{{"text", "^(could not|missing)"}})["items"]
              .empty());

      const auto error = fixture.callExpectingError(
        "console_read", Json{{"text", "(unclosed"}, {"regex", true}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("regular expression") != std::string::npos);
    }

    SECTION("filters by document")
    {
      CHECK(
        texts(fixture.call("console_read", Json{{"document", documentId}})["items"])
        == std::vector<std::string>{"Loaded materials", "Missing texture wall_a"});
      CHECK(
        texts(fixture.call("console_read", Json{{"document", "closed.map"}})["items"])
        == std::vector<std::string>{"Could not load model"});
      CHECK(
        texts(fixture.call(
          "console_read", Json{{"document", "none"}, {"minLevel", "debug"}})["items"])
        == std::vector<std::string>{"debug message", "no document"});

      // the filter does not default to the active document
      CHECK(fixture.call("console_read")["items"].size() == 4);

      CHECK(
        fixture.callExpectingError("console_read", Json{{"document", "doc:99"}}).code
        == ErrorCode::DocumentNotFound);
    }

    SECTION("pages and reads only newer messages")
    {
      const auto first = fixture.call("console_read", Json{{"limit", 3}});
      CHECK(
        texts(first["items"])
        == std::vector<std::string>{
          "Loaded materials", "Missing texture wall_a", "Could not load model"});
      CHECK(first["total"] == 4);
      REQUIRE(first["nextCursor"].is_string());
      CHECK(first["lastSeq"] == firstSeq + 3);

      const auto second =
        fixture.call("console_read", Json{{"limit", 3}, {"cursor", first["nextCursor"]}});
      CHECK(texts(second["items"]) == std::vector<std::string>{"no document"});
      CHECK(second["total"] == 1);
      CHECK(second["nextCursor"].is_null());
      CHECK(second["lastSeq"] == console.lastSeq());
      CHECK(!second.contains("dropped"));

      // after the last read, only new messages are returned
      CHECK(fixture.call("console_read", Json{{"after", second["lastSeq"]}})["items"]
              .empty());
      console.add(LogLevel::Info, "new message");
      const auto third = fixture.call("console_read", Json{{"after", second["lastSeq"]}});
      CHECK(texts(third["items"]) == std::vector<std::string>{"new message"});
      CHECK(third["lastSeq"] == console.lastSeq());

      // messages that do not match still advance lastSeq
      console.add(LogLevel::Debug, "ignored");
      const auto fourth = fixture.call("console_read", Json{{"after", third["lastSeq"]}});
      CHECK(fourth["items"].empty());
      CHECK(fourth["lastSeq"] == console.lastSeq());
    }

    SECTION("newest")
    {
      const auto result =
        fixture.call("console_read", Json{{"limit", 2}, {"newest", true}});
      CHECK(
        texts(result["items"])
        == std::vector<std::string>{"Could not load model", "no document"});
      CHECK(result["total"] == 4);
      CHECK(result["nextCursor"].is_null());
      CHECK(result["lastSeq"] == console.lastSeq());
    }

    SECTION("reports dropped messages")
    {
      const auto lastSeq = console.lastSeq();

      SECTION("cleared")
      {
        console.clear();
        console.add(LogLevel::Info, "after clear");
        const auto result = fixture.call("console_read", Json{{"after", firstSeq}});
        CHECK(texts(result["items"]) == std::vector<std::string>{"after clear"});
        // the messages after firstSeq up to lastSeq are gone
        CHECK(result["dropped"] == lastSeq - firstSeq);

        CHECK(!fixture.call("console_read").contains("dropped"));
      }

      SECTION("buffer limit")
      {
        for (size_t i = 0; i < console.capacity(); ++i)
        {
          console.add(LogLevel::Info, "filler");
        }
        const auto result =
          fixture.call("console_read", Json{{"after", lastSeq - 1}, {"limit", 1}});
        CHECK(result["dropped"] == 1);
        CHECK(result["total"] == console.capacity());
        CHECK(!fixture.call("console_read", Json{{"after", lastSeq}, {"limit", 1}})
                 .contains("dropped"));
      }
    }

    SECTION("invalid arguments")
    {
      CHECK(
        fixture.callExpectingError("console_read", Json{{"minLevel", "verbose"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("console_read", Json{{"limit", 0}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("console_read", Json{{"after", -1}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("console_read", Json{{"cursor", "garbage"}}).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("host without console")
    {
      fixture.host().supportsConsole = false;
      CHECK(
        fixture.callExpectingError("console_read").code == ErrorCode::UnsupportedInHost);
    }
  }

  SECTION("console_clear")
  {
    console.add(LogLevel::Info, "a");
    console.add(LogLevel::Warn, "b");
    const auto lastSeq = console.lastSeq();

    SECTION("dry run")
    {
      const auto result = fixture.call("console_clear", Json{{"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["cleared"] == 2);
      CHECK(result["result"]["wouldDo"].is_string());
      CHECK(console.messages().size() == 2);
      CHECK(fixture.host().clearConsoleViewsCount == 0);
    }

    SECTION("clears the buffer and the console views")
    {
      const auto result = fixture.call("console_clear");
      CHECK(result["dryRun"] == false);
      CHECK(result["undoStep"].is_null());
      CHECK(result["result"]["cleared"] == 2);
      CHECK(result["result"]["lastSeq"] == lastSeq);
      CHECK(console.messages().empty());
      CHECK(fixture.host().clearConsoleViewsCount == 1);

      const auto read = fixture.call("console_read", Json{{"after", lastSeq - 2}});
      CHECK(read["items"].empty());
      CHECK(read["dropped"] == 2);

      // new messages continue the sequence
      console.add(LogLevel::Info, "c");
      CHECK(fixture.call("console_read")["items"][0]["seq"] == lastSeq + 1);
    }

    SECTION("host without console")
    {
      fixture.host().supportsConsole = false;
      CHECK(
        fixture.callExpectingError("console_clear").code == ErrorCode::UnsupportedInHost);
      CHECK(fixture.host().clearConsoleViewsCount == 0);
    }
  }

  SECTION("console resource")
  {
    const auto list = fixture.rpc("resources/list");
    CHECK(std::ranges::any_of(list["result"]["resources"], [](const auto& entry) {
      return entry["uri"] == ConsoleUri;
    }));

    SECTION("read")
    {
      console.add(LogLevel::Debug, "debug");
      console.add(LogLevel::Warn, "warning", &document, "unnamed1");
      const auto content = readConsoleResource(fixture);
      CHECK(texts(content["messages"]) == std::vector<std::string>{"warning"});
      CHECK(content["messages"][0]["document"] == documentId);
      CHECK(content["lastSeq"] == console.lastSeq());
      CHECK(content["buffered"] == 2);
      CHECK(content["capacity"] == console.capacity());
    }

    SECTION("lists the newest 200 messages")
    {
      for (auto i = 0; i < 250; ++i)
      {
        console.add(LogLevel::Info, std::to_string(i));
      }
      const auto content = readConsoleResource(fixture);
      REQUIRE(content["messages"].size() == 200);
      CHECK(content["messages"].front()["text"] == "50");
      CHECK(content["messages"].back()["text"] == "249");
    }

    SECTION("subscribers are notified once per burst")
    {
      auto notifications = std::make_shared<CapturingNotificationStream>();
      REQUIRE(
        fixture.server().openNotificationStream(fixture.sessionId(), notifications));
      REQUIRE(fixture.rpc("resources/subscribe", Json{{"uri", ConsoleUri}})["result"]
                .is_object());

      console.add(LogLevel::Info, "a");
      console.add(LogLevel::Warn, "b");
      fixture.scheduler().runPending();
      // delayed so that messages logged shortly after are reported together
      CHECK(consoleUpdates(*notifications) == 0);

      fixture.scheduler().advance(100ms);
      console.add(LogLevel::Error, "c");
      fixture.scheduler().advance(200ms);
      CHECK(consoleUpdates(*notifications) == 1);

      fixture.scheduler().advance(1000ms);
      CHECK(consoleUpdates(*notifications) == 1);

      SECTION("the agent call log lines do not notify")
      {
        console.add(LogLevel::Info, "[AI] map_summary ok 1 ms (+0 created)");
        fixture.scheduler().advance(1000ms);
        CHECK(consoleUpdates(*notifications) == 1);
      }

      SECTION("clearing notifies")
      {
        console.clear();
        fixture.scheduler().advance(1000ms);
        CHECK(consoleUpdates(*notifications) == 2);
      }

      SECTION("the next burst notifies again")
      {
        console.add(LogLevel::Info, "d");
        fixture.scheduler().advance(1000ms);
        CHECK(consoleUpdates(*notifications) == 2);
      }
    }

    SECTION("nothing is scheduled without subscriptions")
    {
      const auto pending = fixture.scheduler().pendingTaskCount();
      console.add(LogLevel::Info, "a");
      CHECK(fixture.scheduler().pendingTaskCount() == pending);
    }

    SECTION("host without console")
    {
      fixture.host().supportsConsole = false;
      CHECK(fixture.rpc("resources/read", Json{{"uri", ConsoleUri}}).contains("error"));
    }
  }

  SECTION("call results report console warnings and errors")
  {
    SECTION("messages logged during the call")
    {
      auto env = fs::TestEnvironment{};
      env.createFile(
        "missing_wad.map",
        R"(// Game: Quake
// Format: Standard
// entity 0
{
"classname" "worldspawn"
"wad" "/does/not/exist/missing.wad"
}
)");

      const auto result = fixture.call(
        "document_open", Json{{"path", (env.dir() / "missing_wad.map").string()}});
      const auto openedId = result["result"]["document"]["id"];
      REQUIRE(result.contains("console"));

      CAPTURE(result["console"]);
      const auto& messages = result["console"];
      CHECK(std::ranges::any_of(messages, [&](const auto& message) {
        return message["text"].template get<std::string>().find("missing.wad")
                 != std::string::npos
               && (message["level"] == "warning" || message["level"] == "error")
               && message["document"] == openedId;
      }));

      // the same messages are in the console
      const auto read = fixture.call(
        "console_read", Json{{"document", openedId}, {"minLevel", "warning"}});
      CHECK(!read["items"].empty());
    }

    SECTION("only warnings and errors logged while the call ran")
    {
      console.add(LogLevel::Error, "before the call", &document, "unnamed1");
      CHECK(!fixture.call("document_list").contains("console"));
      CHECK(!fixture.call("console_read").contains("console"));
    }
  }
}

} // namespace tb::mcp
