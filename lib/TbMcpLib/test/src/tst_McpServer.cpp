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
#include "mcp/CallContext.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/PromptRegistry.h"
#include "mcp/ProtocolVersion.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
using namespace schema;

namespace
{

Json request(const int id, const std::string& method, Json params = Json::object())
{
  return jsonrpc::makeRequest(id, method, std::move(params));
}

Json initializeRequest(const std::string& version)
{
  return request(
    1,
    "initialize",
    Json{
      {"protocolVersion", version},
      {"capabilities", Json::object()},
      {"clientInfo", Json{{"name", "c"}, {"version", "1"}}},
    });
}

} // namespace

TEST_CASE("McpServer")
{
  auto fixture = McpToolFixture{};
  auto& server = fixture.server();

  SECTION("initialize")
  {
    SECTION("negotiates the protocol version")
    {
      for (const auto& [requested, expected] :
           std::vector<std::pair<std::string, std::string>>{
             {"2025-11-25", "2025-11-25"},
             {"2025-06-18", "2025-06-18"},
             {"2025-03-26", "2025-03-26"},
             {"2024-11-05", "2025-11-25"},
           })
      {
        auto stream = std::make_shared<CapturingRequestStream>();
        const auto result =
          server.post(std::nullopt, dumpJson(initializeRequest(requested)), stream);
        REQUIRE(result.status == PostStatus::Pending);
        CHECK(result.newSessionId.size() == 32);
        REQUIRE(stream->response);
        CHECK((*stream->response)["result"]["protocolVersion"] == expected);
        CHECK(server.sessionProtocolVersion(result.newSessionId) == expected);
      }
    }

    SECTION("returns capabilities and server info")
    {
      auto stream = std::make_shared<CapturingRequestStream>();
      server.post(std::nullopt, dumpJson(initializeRequest("2025-11-25")), stream);
      const auto& result = (*stream->response)["result"];
      CHECK(result["capabilities"]["tools"]["listChanged"] == true);
      CHECK(result["capabilities"]["resources"]["subscribe"] == true);
      CHECK(result["capabilities"].contains("prompts"));
      CHECK(result["capabilities"].contains("logging"));
      CHECK(result["serverInfo"]["name"] == "trenchbroom");
      CHECK(result["serverInfo"]["title"] == "TrenchBroom");
      CHECK(result["serverInfo"]["version"] == "test-version");
      CHECK(result["instructions"].is_string());
    }

    SECTION("requires protocolVersion")
    {
      auto stream = std::make_shared<CapturingRequestStream>();
      const auto result = server.post(
        std::nullopt, dumpJson(request(1, "initialize", Json::object())), stream);
      CHECK(result.status == PostStatus::BadRequest);
      CHECK(result.body["error"]["code"] == jsonrpc::ErrorCode::InvalidParams);
    }

    SECTION("is rejected on an existing session")
    {
      const auto response =
        fixture.rpc("initialize", Json{{"protocolVersion", "2025-11-25"}});
      CHECK(response["error"]["code"] == jsonrpc::ErrorCode::InvalidRequest);
    }

    SECTION("fires sessionsDidChangeNotifier")
    {
      auto count = 0;
      auto connection = server.sessionsDidChangeNotifier.connect([&]() { ++count; });
      fixture.openSession();
      CHECK(count == 1);
      CHECK(server.sessionCount() == 2);
    }
  }

  SECTION("post")
  {
    SECTION("requires a session for anything but initialize")
    {
      const auto result = server.post(
        std::nullopt,
        dumpJson(request(1, "ping")),
        std::make_shared<CapturingRequestStream>());
      CHECK(result.status == PostStatus::BadRequest);
      CHECK(result.body["error"]["code"] == jsonrpc::ErrorCode::InvalidRequest);
    }

    SECTION("rejects unknown sessions")
    {
      const auto result = server.post(
        std::string{"unknown"},
        dumpJson(request(1, "ping")),
        std::make_shared<CapturingRequestStream>());
      CHECK(result.status == PostStatus::SessionNotFound);
    }

    SECTION("rejects invalid JSON")
    {
      const auto result =
        server.post(fixture.sessionId(), "{", std::make_shared<CapturingRequestStream>());
      CHECK(result.status == PostStatus::BadRequest);
      CHECK(result.body["error"]["code"] == jsonrpc::ErrorCode::ParseError);
    }

    SECTION("accepts notifications with 202")
    {
      const auto result = server.post(
        fixture.sessionId(),
        dumpJson(jsonrpc::makeNotification("notifications/roots/list_changed")),
        std::make_shared<CapturingRequestStream>());
      CHECK(result.status == PostStatus::Accepted);
    }

    SECTION("answers invalid messages with an error response")
    {
      auto stream = std::make_shared<CapturingRequestStream>();
      const auto result =
        server.post(fixture.sessionId(), R"({"id":5,"method":"ping"})", stream);
      CHECK(result.status == PostStatus::Pending);
      REQUIRE(stream->response);
      CHECK((*stream->response)["id"] == 5);
      CHECK((*stream->response)["error"]["code"] == jsonrpc::ErrorCode::InvalidRequest);
    }

    SECTION("batches")
    {
      const auto batch = dumpJson(Json::array({
        request(1, "ping"),
        jsonrpc::makeNotification("notifications/roots/list_changed"),
        request(2, "tools/list"),
      }));

      SECTION("are rejected for protocol versions after 2025-03-26")
      {
        const auto result = server.post(
          fixture.sessionId(), batch, std::make_shared<CapturingRequestStream>());
        CHECK(result.status == PostStatus::BadRequest);
      }

      SECTION("are answered with an array for 2025-03-26")
      {
        const auto sessionId = fixture.openSession("old", "2025-03-26");
        auto stream = std::make_shared<CapturingRequestStream>();
        const auto result = server.post(sessionId, batch, stream);
        CHECK(result.status == PostStatus::Pending);
        REQUIRE(stream->response);
        REQUIRE(stream->response->is_array());
        REQUIRE(stream->response->size() == 2);
        CHECK((*stream->response)[0]["id"] == 1);
        CHECK((*stream->response)[1]["id"] == 2);
      }
    }
  }

  SECTION("methods")
  {
    SECTION("ping")
    {
      CHECK(fixture.rpc("ping")["result"] == Json::object());
    }

    SECTION("unknown method")
    {
      CHECK(fixture.rpc("nope")["error"]["code"] == jsonrpc::ErrorCode::MethodNotFound);
    }

    SECTION("logging/setLevel")
    {
      CHECK(
        fixture.rpc("logging/setLevel", Json{{"level", "debug"}})["result"]
        == Json::object());
      CHECK(server.state().findSession(fixture.sessionId())->logLevel == "debug");
    }

    SECTION("tools/list")
    {
      const auto response = fixture.rpc("tools/list");
      const auto& tools = response["result"]["tools"];
      REQUIRE(tools.is_array());
      auto names = std::vector<std::string>{};
      for (const auto& tool : tools)
      {
        names.push_back(tool["name"].get<std::string>());
        CHECK(tool.contains("inputSchema"));
        CHECK(tool.contains("outputSchema"));
      }
      for (const auto* name :
           {"editor_status",
            "document_list",
            "document_activate",
            "session_log",
            "undo",
            "redo",
            "history_get",
            "transaction_begin",
            "transaction_commit",
            "transaction_rollback"})
      {
        CHECK(std::ranges::find(names, name) != names.end());
      }
    }

    SECTION("tools/call")
    {
      SECTION("unknown tool")
      {
        const auto response = fixture.rpc("tools/call", Json{{"name", "nope"}});
        CHECK(response["error"]["code"] == jsonrpc::ErrorCode::InvalidParams);
      }

      SECTION("missing name")
      {
        const auto response = fixture.rpc("tools/call", Json::object());
        CHECK(response["error"]["code"] == jsonrpc::ErrorCode::InvalidParams);
      }

      SECTION("invalid arguments are a tool error")
      {
        const auto error = fixture.callExpectingError("session_log", Json{{"bogus", 1}});
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.details["errors"][0]["path"] == "/bogus");
      }

      SECTION("structuredContent is omitted for 2025-03-26")
      {
        const auto sessionId = fixture.openSession("old", "2025-03-26");
        const auto result = fixture.callRawAs(sessionId, "editor_status", Json::object());
        CHECK(!result.contains("structuredContent"));
        CHECK(result["content"][0]["type"] == "text");
      }

      SECTION("progress")
      {
        server.tools().add(ToolDef{"test_progress"}.handler(
          [](CallContext& context, const Args&) -> ToolResult {
            context.progress(0.5, 1.0, "halfway");
            return Json::object();
          }));

        auto stream = fixture.post(
          fixture.sessionId(),
          request(
            9,
            "tools/call",
            Json{{"name", "test_progress"}, {"_meta", {{"progressToken", "tok"}}}}));
        REQUIRE(stream->notifications.size() == 1);
        CHECK(
          stream->notifications[0]
          == jsonrpc::makeNotification(
            "notifications/progress",
            Json{
              {"progressToken", "tok"},
              {"progress", 0.5},
              {"total", 1.0},
              {"message", "halfway"}}));
        CHECK(stream->response.has_value());
      }
    }

    SECTION("notifications/cancelled")
    {
      auto& document = fixture.create();
      (void)document;
      fixture.host().busy = BusyState::Busy;

      auto stream = fixture.post(
        fixture.sessionId(),
        request(
          42,
          "tools/call",
          Json{{"name", "transaction_begin"}, {"arguments", {{"name", "x"}}}}));
      CHECK(!stream->response.has_value());

      server.post(
        fixture.sessionId(),
        dumpJson(jsonrpc::makeNotification(
          "notifications/cancelled", Json{{"requestId", 42}, {"reason", "user"}})),
        std::make_shared<CapturingRequestStream>());

      REQUIRE(stream->response.has_value());
      CHECK((*stream->response)["result"]["isError"] == true);
      CHECK(
        (*stream->response)["result"]["structuredContent"]["error"]["code"]
        == "CANCELLED");
    }

    SECTION("resources")
    {
      SECTION("resources/list")
      {
        const auto response = fixture.rpc("resources/list");
        auto uris = std::vector<std::string>{};
        for (const auto& resource : response["result"]["resources"])
        {
          uris.push_back(resource["uri"].get<std::string>());
        }
        CHECK(std::ranges::find(uris, "trenchbroom://editor/status") != uris.end());
        CHECK(std::ranges::find(uris, "trenchbroom://guide") != uris.end());
      }

      SECTION("resources/templates/list")
      {
        CHECK(fixture.rpc("resources/templates/list")["result"].contains(
          "resourceTemplates"));
      }

      SECTION("resources/read")
      {
        const auto response =
          fixture.rpc("resources/read", Json{{"uri", "trenchbroom://editor/status"}});
        const auto& contents = response["result"]["contents"];
        REQUIRE(contents.size() == 1);
        const auto status = parseJson(contents[0]["text"].get<std::string>());
        REQUIRE(status.has_value());
        CHECK((*status)["version"] == "test-version");

        CHECK(
          fixture.rpc(
            "resources/read", Json{{"uri", "trenchbroom://nope"}})["error"]["code"]
          == jsonrpc::ErrorCode::ResourceNotFound);
      }

      SECTION("subscribe and notifications")
      {
        auto notifications = std::make_shared<CapturingNotificationStream>();
        REQUIRE(server.openNotificationStream(fixture.sessionId(), notifications));

        CHECK(
          fixture.rpc(
            "resources/subscribe", Json{{"uri", "trenchbroom://editor/status"}})["result"]
          == Json::object());
        CHECK(
          fixture.rpc(
            "resources/subscribe", Json{{"uri", "trenchbroom://nope"}})["error"]["code"]
          == jsonrpc::ErrorCode::ResourceNotFound);

        fixture.create();
        REQUIRE(notifications->notifications.size() == 1);
        CHECK(
          notifications->notifications[0]
          == jsonrpc::makeNotification(
            "notifications/resources/updated",
            Json{{"uri", "trenchbroom://editor/status"}}));

        fixture.rpc(
          "resources/unsubscribe", Json{{"uri", "trenchbroom://editor/status"}});
        fixture.create();
        CHECK(notifications->notifications.size() == 1);
      }
    }

    SECTION("prompts")
    {
      server.prompts().add(PromptDef{
        "test_prompt",
        "Test",
        "A test prompt",
        {PromptArgument{"topic", "The topic", true}},
        [](const PromptArguments& arguments) -> Result<Json, ToolError> {
          return userTextMessage("About " + arguments.at("topic"));
        },
      });

      CHECK(fixture.rpc("prompts/list")["result"]["prompts"].size() == 1);
      CHECK(
        fixture.rpc(
          "prompts/get",
          Json{
            {"name", "test_prompt"},
            {"arguments", {{"topic", "x"}}}})["result"]["messages"][0]["content"]["text"]
        == "About x");
      CHECK(
        fixture.rpc("prompts/get", Json{{"name", "test_prompt"}})["error"]["code"]
        == jsonrpc::ErrorCode::InvalidParams);
    }
  }

  SECTION("deleteSession")
  {
    const auto sessionId = fixture.openSession();
    CHECK(server.deleteSession(sessionId));
    CHECK(!server.deleteSession(sessionId));
    CHECK(!server.sessionProtocolVersion(sessionId));
    CHECK(
      server
        .post(
          sessionId,
          dumpJson(request(1, "ping")),
          std::make_shared<CapturingRequestStream>())
        .status
      == PostStatus::SessionNotFound);
  }

  SECTION("openNotificationStream")
  {
    CHECK(!server.openNotificationStream(
      "unknown", std::make_shared<CapturingNotificationStream>()));

    auto notifications = std::make_shared<CapturingNotificationStream>();
    REQUIRE(server.openNotificationStream(fixture.sessionId(), notifications));
    server.broadcast(jsonrpc::makeNotification("notifications/tools/list_changed"));
    CHECK(notifications->notifications.size() == 1);

    SECTION("uninitialized sessions get no notifications")
    {
      auto stream = std::make_shared<CapturingRequestStream>();
      const auto result =
        server.post(std::nullopt, dumpJson(initializeRequest("2025-11-25")), stream);
      auto other = std::make_shared<CapturingNotificationStream>();
      REQUIRE(server.openNotificationStream(result.newSessionId, other));
      server.broadcast(jsonrpc::makeNotification("notifications/tools/list_changed"));
      CHECK(other->notifications.empty());
    }
  }

  SECTION("stopAgents")
  {
    auto& document = fixture.create();
    fixture.call("transaction_begin", Json{{"name", "work"}});
    CHECK(document.map().transactionDepth() == 1);

    fixture.host().busy = BusyState::Busy;
    auto pending = fixture.post(
      fixture.sessionId(),
      request(7, "tools/call", Json{{"name", "undo"}, {"arguments", Json::object()}}));
    CHECK(!pending->response);

    server.stopAgents();

    CHECK(server.sessionCount() == 0);
    CHECK(document.map().transactionDepth() == 0);
    REQUIRE(pending->response);
    CHECK(
      (*pending->response)["result"]["structuredContent"]["error"]["code"]
      == "CANCELLED");
    CHECK(server.activity().state == ServerActivity::State::Idle);
  }
}

} // namespace tb::mcp
