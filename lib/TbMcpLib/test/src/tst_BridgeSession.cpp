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

#include "mcp/BridgeSession.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

// the version of McpToolFixture's server
constexpr auto Version = "test-version";

Json request(const Json& id, const std::string& method, Json params = Json::object())
{
  return jsonrpc::makeRequest(id, method, std::move(params));
}

Json initializeRequest(const std::string& protocolVersion)
{
  return request(
    1,
    "initialize",
    Json{
      {"protocolVersion", protocolVersion},
      {"capabilities", Json{{"roots", Json::object()}}},
      {"clientInfo", Json{{"name", "client"}, {"version", "1.0"}}},
    });
}

/** Initializes the bridge session like a client does. */
void initialize(BridgeSession& session, const std::string& protocolVersion = "2025-11-25")
{
  REQUIRE(session.answer(initializeRequest(protocolVersion)));
  REQUIRE(!session.answer(jsonrpc::makeNotification("notifications/initialized")));
}

Json resultOf(const std::optional<Json>& response)
{
  REQUIRE(response);
  REQUIRE(response->contains("result"));
  return (*response)["result"];
}

std::vector<std::string> methodsOf(const std::vector<Json>& messages)
{
  auto result = std::vector<std::string>{};
  for (const auto& message : messages)
  {
    result.push_back(message["method"].get<std::string>());
  }
  return result;
}

} // namespace

TEST_CASE("BridgeSession")
{
  auto fixture = McpToolFixture{};
  auto session = BridgeSession{Version};

  SECTION("route")
  {
    SECTION("before initialize, the bridge answers everything")
    {
      CHECK(session.route(request(1, "tools/call"), false) == BridgeRoute::Bridge);
      CHECK(session.route(request(1, "tools/list"), false) == BridgeRoute::Bridge);
      CHECK(
        session.route(jsonrpc::makeNotification("notifications/cancelled"), false)
        == BridgeRoute::Bridge);
    }

    initialize(session);

    SECTION("without an editor session")
    {
      for (const auto* method :
           {"initialize",
            "ping",
            "tools/list",
            "resources/list",
            "resources/templates/list",
            "prompts/list",
            "prompts/get",
            "logging/setLevel"})
      {
        CAPTURE(method);
        CHECK(session.route(request(2, method), false) == BridgeRoute::Bridge);
      }

      for (const auto* method :
           {"tools/call",
            "resources/read",
            "resources/subscribe",
            "resources/unsubscribe",
            "completion/complete",
            "unknown/method"})
      {
        CAPTURE(method);
        CHECK(session.route(request(2, method), false) == BridgeRoute::Editor);
      }

      CHECK(
        session.route(jsonrpc::makeNotification("notifications/initialized"), false)
        == BridgeRoute::Bridge);
      CHECK(
        session.route(
          jsonrpc::makeNotification("notifications/cancelled", Json{{"requestId", 2}}),
          false)
        == BridgeRoute::EditorIfConnected);
      CHECK(
        session.route(jsonrpc::makeResponse(7, Json::object()), false)
        == BridgeRoute::EditorIfConnected);
      CHECK(session.route(Json{{"foo", 1}}, false) == BridgeRoute::Bridge);
    }

    SECTION("with an editor session, everything but initialize goes to the editor")
    {
      CHECK(session.route(initializeRequest("2025-11-25"), true) == BridgeRoute::Bridge);
      CHECK(session.route(request(2, "tools/list"), true) == BridgeRoute::Editor);
      CHECK(session.route(request(2, "ping"), true) == BridgeRoute::Editor);
      CHECK(
        session.route(jsonrpc::makeNotification("notifications/cancelled"), true)
        == BridgeRoute::Editor);
    }

    SECTION("batches")
    {
      const auto offline =
        Json::array({request(2, "tools/list"), request(3, "prompts/list")});
      const auto mixed =
        Json::array({request(2, "tools/list"), request(3, "tools/call")});
      CHECK(session.route(offline, false) == BridgeRoute::Bridge);
      CHECK(session.route(mixed, false) == BridgeRoute::Editor);
      CHECK(session.route(offline, true) == BridgeRoute::Editor);
    }
  }

  SECTION("answer")
  {
    SECTION("initialize answers like the editor's server")
    {
      for (const auto* protocolVersion :
           {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"})
      {
        CAPTURE(protocolVersion);
        auto bridge = BridgeSession{Version};
        const auto response = bridge.answer(initializeRequest(protocolVersion));

        auto stream = std::make_shared<CapturingRequestStream>();
        fixture.server().post(
          std::nullopt, dumpJson(initializeRequest(protocolVersion)), stream);

        REQUIRE(stream->response);
        CHECK(response == stream->response);
        CHECK(bridge.clientInitialized());
      }
    }

    SECTION("before initialize, requests fail like on the editor's server")
    {
      const auto response = session.answer(request(5, "tools/list"));
      REQUIRE(response);
      CHECK((*response)["id"] == 5);
      CHECK((*response)["error"]["code"] == jsonrpc::ErrorCode::InvalidRequest);
      CHECK(!session.clientInitialized());
    }

    initialize(session);

    SECTION("the lists equal the editor's without documents")
    {
      for (const auto* method :
           {"tools/list", "resources/list", "resources/templates/list", "prompts/list"})
      {
        CAPTURE(method);
        CHECK(
          resultOf(session.answer(request(2, method))) == fixture.rpc(method)["result"]);
      }
    }

    SECTION("prompts/get answers like the editor's server")
    {
      const auto prompts = fixture.rpc("prompts/list")["result"]["prompts"];
      for (const auto& prompt : prompts)
      {
        const auto params = Json{{"name", prompt["name"]}, {"arguments", Json::object()}};
        CHECK(
          session.answer(request(2, "prompts/get", params))->value("result", Json{})
          == fixture.rpc("prompts/get", params).value("result", Json{}));
      }

      const auto unknown = session.answer(request(2, "prompts/get", {{"name", "nope"}}));
      REQUIRE(unknown);
      CHECK((*unknown)["error"]["code"] == jsonrpc::ErrorCode::InvalidParams);
    }

    SECTION("ping")
    {
      CHECK(resultOf(session.answer(request("p", "ping"))) == Json::object());
    }

    SECTION("batches")
    {
      // batches need protocol version 2025-03-26
      initialize(session, "2025-03-26");
      const auto response = session.answer(Json::array(
        {request(2, "ping"), jsonrpc::makeNotification("x"), request(3, "ping")}));
      REQUIRE(response);
      REQUIRE(response->is_array());
      CHECK(response->size() == 2);
    }

    SECTION("notifications and client responses have no response")
    {
      CHECK(!session.answer(jsonrpc::makeNotification("notifications/initialized")));
      CHECK(!session.answer(jsonrpc::makeResponse(3, Json::object())));
    }

    SECTION("a new initialize starts a new session")
    {
      const auto response = session.answer(initializeRequest("2025-03-26"));
      CHECK(resultOf(response)["protocolVersion"] == "2025-03-26");
      CHECK(session.handshake().front()["params"]["protocolVersion"] == "2025-03-26");
    }
  }

  SECTION("handshake")
  {
    initialize(session, "2025-06-18");

    SECTION("without client state")
    {
      const auto messages = session.handshake();
      CHECK(
        methodsOf(messages)
        == std::vector<std::string>{
          "initialize", "notifications/initialized", "tools/list", "resources/list"});
      CHECK(messages[0]["params"] == initializeRequest("2025-06-18")["params"]);
      for (const auto& message : messages)
      {
        CHECK((!message.contains("id") || BridgeSession::isHandshakeId(message["id"])));
      }
    }

    SECTION("replays the log level and the subscriptions")
    {
      CHECK(
        resultOf(session.answer(request(2, "logging/setLevel", {{"level", "info"}})))
        == Json::object());
      session.forwarded(request(3, "resources/subscribe", {{"uri", "trenchbroom://a"}}));
      session.forwarded(request(4, "resources/subscribe", {{"uri", "trenchbroom://b"}}));
      session.forwarded(
        request(5, "resources/unsubscribe", {{"uri", "trenchbroom://a"}}));
      session.forwarded(request(6, "logging/setLevel", {{"level", "debug"}}));

      const auto messages = session.handshake();
      CHECK(
        methodsOf(messages)
        == std::vector<std::string>{
          "initialize",
          "notifications/initialized",
          "logging/setLevel",
          "resources/subscribe",
          "tools/list",
          "resources/list"});
      CHECK(messages[2]["params"]["level"] == "debug");
      CHECK(messages[3]["params"]["uri"] == "trenchbroom://b");
    }

    SECTION("a failed setLevel is not replayed")
    {
      session.answer(request(2, "logging/setLevel", Json::object()));
      CHECK(methodsOf(session.handshake()).size() == 4);
    }
  }

  SECTION("isHandshakeId")
  {
    CHECK(
      BridgeSession::isHandshakeId(Json(std::string{"trenchbroom-bridge:initialize"})));
    CHECK(!BridgeSession::isHandshakeId(Json(std::string{"initialize"})));
    CHECK(!BridgeSession::isHandshakeId(1));
  }

  SECTION("editorConnected")
  {
    initialize(session);
    const auto tools = fixture.rpc("tools/list")["result"];
    const auto resources = fixture.rpc("resources/list")["result"];

    SECTION("nothing changed")
    {
      CHECK(session.editorConnected(tools, resources).empty());
    }

    SECTION("the editor has other tools and resources")
    {
      auto otherTools = tools;
      otherTools["tools"].erase(0);
      auto otherResources = resources;
      otherResources["resources"].push_back(
        Json{{"uri", "trenchbroom://documents/doc:1"}});

      CHECK(
        methodsOf(session.editorConnected(otherTools, resources))
        == std::vector<std::string>{"notifications/tools/list_changed"});
      CHECK(
        methodsOf(session.editorConnected(otherTools, otherResources))
        == std::vector<std::string>{"notifications/resources/list_changed"});
      CHECK(session.editorConnected(otherTools, otherResources).empty());
    }

    SECTION("unknown lists count as changed")
    {
      CHECK(session.editorConnected(nullptr, nullptr).size() == 2);
    }

    SECTION("after a lost connection, the resources count as changed")
    {
      CHECK(session.editorConnected(tools, resources).empty());
      CHECK(
        methodsOf(session.editorDisconnected())
        == std::vector<std::string>{"notifications/resources/list_changed"});
      CHECK(
        methodsOf(session.editorConnected(tools, resources))
        == std::vector<std::string>{"notifications/resources/list_changed"});
    }
  }

  SECTION("editorDisconnected")
  {
    initialize(session);
    auto tools = fixture.rpc("tools/list")["result"];
    const auto resources = fixture.rpc("resources/list")["result"];

    SECTION("reports the tools if the editor had other tools")
    {
      tools["tools"].erase(0);
      session.editorConnected(tools, resources);
      CHECK(
        methodsOf(session.editorDisconnected())
        == std::vector<std::string>{
          "notifications/tools/list_changed", "notifications/resources/list_changed"});
      CHECK(
        methodsOf(session.editorDisconnected())
        == std::vector<std::string>{"notifications/resources/list_changed"});
    }
  }
}

} // namespace tb::mcp
