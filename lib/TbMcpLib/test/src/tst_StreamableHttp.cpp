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

#include "mcp/Endpoint.h"
#include "mcp/HttpParser.h"
#include "mcp/Json.h"
#include "mcp/SseParser.h"
#include "mcp/StreamableHttp.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

class FakeHttpConnection : public HttpConnection
{
public:
  std::string written;
  bool closed = false;
  std::function<void()> onClose;

  void write(const std::string_view bytes) override { written += bytes; }

  void close() override
  {
    closed = true;
    if (onClose)
    {
      onClose();
    }
  }

  std::string take() { return std::exchange(written, {}); }
};

using PostFunction = std::function<PostResult(
  const std::optional<std::string>&, std::string_view, std::shared_ptr<RequestStream>)>;

/**
 * A minimal endpoint: `initialize` creates the session "s1", other requests are
 * answered with an empty result, notifications are accepted. `onPost` overrides this.
 */
class FakeEndpoint : public Endpoint
{
public:
  std::map<std::string, std::string, std::less<>> sessions;
  std::vector<std::optional<std::string>> postedSessionIds;
  std::vector<std::string> postedBodies;
  std::weak_ptr<RequestStream> lastStream;
  std::map<std::string, std::weak_ptr<NotificationStream>, std::less<>>
    notificationStreams;
  std::vector<std::string> deletedSessions;
  PostFunction onPost;

  PostResult post(
    const std::optional<std::string>& sessionId,
    const std::string_view body,
    std::shared_ptr<RequestStream> stream) override
  {
    postedSessionIds.push_back(sessionId);
    postedBodies.emplace_back(body);
    lastStream = stream;

    if (onPost)
    {
      return onPost(sessionId, body, std::move(stream));
    }

    const auto message = parseJson(body);
    if (!message || !message->is_object())
    {
      return {
        PostStatus::BadRequest,
        {},
        Json{
          {"jsonrpc", "2.0"},
          {"id", nullptr},
          {"error", {{"code", -32700}, {"message", "Parse error"}}}}};
    }

    if (message->value("method", "") == "initialize")
    {
      sessions["s1"] = "2025-11-25";
      stream->complete(Json{
        {"jsonrpc", "2.0"},
        {"id", (*message)["id"]},
        {"result", {{"protocolVersion", "2025-11-25"}}}});
      return {PostStatus::Pending, "s1"};
    }

    if (!sessionId)
    {
      return {PostStatus::BadRequest};
    }
    if (!sessions.contains(*sessionId))
    {
      return {PostStatus::SessionNotFound};
    }

    if (message->contains("id"))
    {
      stream->complete(
        Json{{"jsonrpc", "2.0"}, {"id", (*message)["id"]}, {"result", Json::object()}});
      return {PostStatus::Pending};
    }
    return {PostStatus::Accepted};
  }

  std::optional<std::string> sessionProtocolVersion(
    const std::string_view sessionId) const override
  {
    const auto it = sessions.find(sessionId);
    return it != sessions.end() ? std::optional{it->second} : std::nullopt;
  }

  bool openNotificationStream(
    const std::string_view sessionId, std::shared_ptr<NotificationStream> stream) override
  {
    if (!sessions.contains(sessionId))
    {
      return false;
    }
    notificationStreams[std::string{sessionId}] = stream;
    return true;
  }

  bool deleteSession(const std::string_view sessionId) override
  {
    const auto it = sessions.find(sessionId);
    if (it == sessions.end())
    {
      return false;
    }
    sessions.erase(it);
    deletedSessions.emplace_back(sessionId);
    return true;
  }
};

struct ParsedResponse
{
  int status = 0;
  HttpHeaders headers;
  std::string body;
  bool chunked = false;
  /** False if a chunked body has not been terminated yet. */
  bool complete = false;

  std::optional<std::string_view> header(const std::string_view name) const
  {
    return findHttpHeader(headers, name);
  }

  std::vector<SseEvent> events() const
  {
    auto parser = SseParser{};
    return parser.feed(body);
  }

  Json json() const { return parseJson(body).value_or(Json{}); }
};

/**
 * Parses the responses in the given bytes. The last one may be an unterminated chunked
 * response.
 */
std::vector<ParsedResponse> parseResponses(std::string_view bytes)
{
  auto result = std::vector<ParsedResponse>{};
  while (!bytes.empty())
  {
    auto response = ParsedResponse{};
    const auto headEnd = bytes.find("\r\n\r\n");
    REQUIRE(headEnd != std::string_view::npos);

    auto head = bytes.substr(0, headEnd);
    bytes.remove_prefix(headEnd + 4);

    const auto statusLineEnd = head.find("\r\n");
    const auto statusLine = head.substr(0, statusLineEnd);
    REQUIRE(statusLine.starts_with("HTTP/1.1 "));
    response.status = std::stoi(std::string{statusLine.substr(9, 3)});

    head.remove_prefix(
      statusLineEnd == std::string_view::npos ? head.size() : statusLineEnd + 2);
    while (!head.empty())
    {
      const auto lineEnd = head.find("\r\n");
      const auto line = head.substr(0, lineEnd);
      const auto colon = line.find(": ");
      response.headers.emplace_back(line.substr(0, colon), line.substr(colon + 2));
      head.remove_prefix(lineEnd == std::string_view::npos ? head.size() : lineEnd + 2);
    }

    if (response.header("Transfer-Encoding") == "chunked")
    {
      response.chunked = true;
      while (!bytes.empty())
      {
        const auto sizeEnd = bytes.find("\r\n");
        const auto size = std::stoul(std::string{bytes.substr(0, sizeEnd)}, nullptr, 16);
        bytes.remove_prefix(sizeEnd + 2);
        if (size == 0)
        {
          bytes.remove_prefix(2);
          response.complete = true;
          break;
        }
        response.body += bytes.substr(0, size);
        bytes.remove_prefix(size + 2);
      }
    }
    else
    {
      const auto length = std::stoul(std::string{*response.header("Content-Length")});
      response.body = bytes.substr(0, length);
      bytes.remove_prefix(length);
      response.complete = true;
    }

    result.push_back(std::move(response));
  }
  return result;
}

ParsedResponse parseResponse(const std::string_view bytes)
{
  auto responses = parseResponses(bytes);
  REQUIRE(responses.size() == 1);
  return responses.front();
}

std::string request(
  const std::string_view method,
  const std::string_view body,
  const HttpHeaders& headers = {},
  const std::string_view target = "/mcp")
{
  auto result = std::string{method} + " " + std::string{target} + " HTTP/1.1\r\n";
  if (!findHttpHeader(headers, "Host"))
  {
    result += "Host: 127.0.0.1:47100\r\n";
  }
  for (const auto& [name, value] : headers)
  {
    result += name + ": " + value + "\r\n";
  }
  if (method == "POST")
  {
    if (!findHttpHeader(headers, "Content-Type"))
    {
      result += "Content-Type: application/json\r\n";
    }
    result += "Accept: application/json, text/event-stream\r\n";
    result += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  result += "\r\n";
  result += body;
  return result;
}

std::string post(const std::string_view body, const HttpHeaders& headers = {})
{
  return request("POST", body, headers);
}

const auto initializeBody =
  std::string{R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"};
const auto session = HttpHeaders{{"Mcp-Session-Id", "s1"}};

Json response(const int id)
{
  return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};
}

Json progress(const int value)
{
  return Json{
    {"jsonrpc", "2.0"},
    {"method", "notifications/progress"},
    {"params", {{"progress", value}}}};
}

} // namespace

TEST_CASE("StreamableHttpServer")
{
  auto endpoint = FakeEndpoint{};
  auto server = StreamableHttpServer{endpoint, {}};
  auto connection = FakeHttpConnection{};
  connection.onClose = [&]() { server.connectionClosed(connection); };
  server.openConnection(connection);

  SECTION("isLoopbackAddress")
  {
    CHECK(StreamableHttpServer::isLoopbackAddress("127.0.0.1"));
    CHECK(StreamableHttpServer::isLoopbackAddress("127.1.2.3"));
    CHECK(StreamableHttpServer::isLoopbackAddress("::1"));
    CHECK(StreamableHttpServer::isLoopbackAddress("[::1]"));
    CHECK(StreamableHttpServer::isLoopbackAddress("localhost"));
    CHECK(StreamableHttpServer::isLoopbackAddress("::ffff:127.0.0.1"));
    CHECK(!StreamableHttpServer::isLoopbackAddress("0.0.0.0"));
    CHECK(!StreamableHttpServer::isLoopbackAddress("127.0.0.1.evil.com"));
    CHECK(!StreamableHttpServer::isLoopbackAddress("127.0.0"));
    CHECK(!StreamableHttpServer::isLoopbackAddress("192.168.1.2"));
    CHECK(!StreamableHttpServer::isLoopbackAddress("::"));
  }

  SECTION("validateConfig")
  {
    CHECK(StreamableHttpServer::validateConfig({}) == std::nullopt);
    CHECK(StreamableHttpServer::validateConfig({"0.0.0.0", ""}) != std::nullopt);
    CHECK(StreamableHttpServer::validateConfig({"0.0.0.0", "secret"}) == std::nullopt);
  }

  SECTION("config")
  {
    CHECK(server.config().bindAddress == "127.0.0.1");
    CHECK(server.config().path == "/mcp");
  }

  SECTION("feed")
  {
    SECTION("initialize returns JSON with a session id")
    {
      server.feed(connection, post(initializeBody));

      const auto r = parseResponse(connection.take());
      CHECK(r.status == 200);
      CHECK(r.header("Content-Type") == "application/json");
      CHECK(r.header("Mcp-Session-Id") == "s1");
      CHECK(r.json()["result"]["protocolVersion"] == "2025-11-25");
      CHECK(
        endpoint.postedSessionIds
        == std::vector<std::optional<std::string>>{std::nullopt});
      CHECK(!connection.closed);
    }

    SECTION("request split across arbitrary chunks")
    {
      const auto bytes = post(initializeBody);
      for (size_t i = 0; i < bytes.size(); i += 7)
      {
        CHECK(connection.written.empty());
        server.feed(connection, bytes.substr(i, 7));
      }
      CHECK(parseResponse(connection.take()).status == 200);
    }

    SECTION("notification returns 202")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      server.feed(
        connection,
        post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session));

      const auto r = parseResponse(connection.take());
      CHECK(r.status == 202);
      CHECK(r.body.empty());
      CHECK(r.header("Mcp-Session-Id") == std::nullopt);
      CHECK(endpoint.postedSessionIds.back() == "s1");
    }

    SECTION("bad request returns 400 with body")
    {
      server.feed(connection, post("{"));
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 400);
      CHECK(r.header("Content-Type") == "application/json");
      CHECK(r.json()["error"]["code"] == -32700);
    }

    SECTION("bad request without body")
    {
      server.feed(connection, post(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"));
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 400);
      CHECK(r.body.empty());
    }

    SECTION("unknown session returns 404")
    {
      server.feed(
        connection,
        post(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", {{"Mcp-Session-Id", "x"}}));
      CHECK(parseResponse(connection.take()).status == 404);
      CHECK(endpoint.postedBodies.empty());
    }

    SECTION("session not found reported by the endpoint returns 404")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      endpoint.onPost = [](const auto&, const auto&, const auto&) {
        return PostResult{PostStatus::SessionNotFound};
      };
      server.feed(connection, post("{}", session));
      CHECK(parseResponse(connection.take()).status == 404);
    }

    SECTION("protocol version header")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      const auto ping = std::string{R"({"jsonrpc":"2.0","id":2,"method":"ping"})"};

      server.feed(
        connection,
        post(ping, {{"Mcp-Session-Id", "s1"}, {"MCP-Protocol-Version", "2025-11-25"}}));
      CHECK(parseResponse(connection.take()).status == 200);

      server.feed(connection, post(ping, session));
      CHECK(parseResponse(connection.take()).status == 200);

      server.feed(
        connection,
        post(ping, {{"Mcp-Session-Id", "s1"}, {"MCP-Protocol-Version", "2025-06-18"}}));
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 400);
      CHECK(r.json()["error"]["code"] == -32600);
      CHECK(endpoint.postedBodies.size() == 2);
    }

    SECTION("content type must be JSON")
    {
      server.feed(connection, post("{}", {{"Content-Type", "text/plain"}}));
      CHECK(parseResponse(connection.take()).status == 415);

      server.feed(
        connection,
        post(initializeBody, {{"Content-Type", "Application/JSON; charset=utf-8"}}));
      CHECK(parseResponse(connection.take()).status == 200);
    }

    SECTION("other paths return 404")
    {
      server.feed(connection, request("POST", "{}", {}, "/other"));
      CHECK(parseResponse(connection.take()).status == 404);
      CHECK(endpoint.postedBodies.empty());
    }

    SECTION("other methods return 405")
    {
      server.feed(connection, request("PUT", ""));
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 405);
      CHECK(r.header("Allow") == "GET, POST, DELETE");
    }

    SECTION("parse errors are answered and close the connection")
    {
      server.feed(connection, "POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 411);
      CHECK(r.header("Connection") == "close");
      CHECK(connection.closed);
      CHECK(server.connectionCount() == 0);
    }

    SECTION("body too large returns 413")
    {
      server.feed(
        connection,
        "POST /mcp HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16777217\r\n\r\n");
      CHECK(parseResponse(connection.take()).status == 413);
      CHECK(connection.closed);
    }

    SECTION("security")
    {
      SECTION("allowed origins")
      {
        for (const auto* origin :
             {"http://localhost",
              "http://localhost:3000",
              "https://127.0.0.1:8080",
              "http://[::1]:5000"})
        {
          CAPTURE(origin);
          server.feed(connection, post(initializeBody, {{"Origin", origin}}));
          CHECK(parseResponse(connection.take()).status == 200);
        }
      }

      SECTION("rejected origins")
      {
        for (const auto* origin :
             {"http://evil.com",
              "http://localhost.evil.com",
              "null",
              "file://",
              "http://127.0.0.1.evil.com"})
        {
          CAPTURE(origin);
          server.feed(connection, post(initializeBody, {{"Origin", origin}}));
          CHECK(parseResponse(connection.take()).status == 403);
        }
        CHECK(endpoint.postedBodies.empty());
      }

      SECTION("allowed hosts")
      {
        for (const auto* host :
             {"localhost", "localhost:47100", "127.0.0.1", "[::1]:47100", "LOCALHOST"})
        {
          CAPTURE(host);
          server.feed(connection, post(initializeBody, {{"Host", host}}));
          CHECK(parseResponse(connection.take()).status == 200);
        }
      }

      SECTION("rejected hosts (DNS rebinding)")
      {
        for (const auto* host : {"evil.com", "evil.com:47100", "192.168.0.1"})
        {
          CAPTURE(host);
          server.feed(connection, post(initializeBody, {{"Host", host}}));
          CHECK(parseResponse(connection.take()).status == 403);
        }
        CHECK(endpoint.postedBodies.empty());
      }

      SECTION("missing host")
      {
        server.feed(connection, "GET /mcp HTTP/1.1\r\n\r\n");
        CHECK(parseResponse(connection.take()).status == 400);
      }

      SECTION("non-loopback bind requires a bearer token")
      {
        auto remoteServer = StreamableHttpServer{endpoint, {"192.168.0.10", "secret"}};
        auto remote = FakeHttpConnection{};
        remoteServer.openConnection(remote);

        const auto host = HttpHeaders{{"Host", "192.168.0.10:47100"}};
        remoteServer.feed(remote, post(initializeBody, host));
        auto r = parseResponse(remote.take());
        CHECK(r.status == 401);
        CHECK(r.header("WWW-Authenticate") == "Bearer");

        remoteServer.feed(
          remote,
          post(
            initializeBody,
            {{"Host", "192.168.0.10:47100"}, {"Authorization", "Bearer wrong"}}));
        CHECK(parseResponse(remote.take()).status == 401);

        remoteServer.feed(
          remote,
          post(
            initializeBody,
            {{"Host", "192.168.0.10:47100"}, {"Authorization", "bearer secret"}}));
        CHECK(parseResponse(remote.take()).status == 200);

        remoteServer.feed(
          remote,
          post(
            initializeBody,
            {{"Host", "other.host"}, {"Authorization", "Bearer secret"}}));
        CHECK(parseResponse(remote.take()).status == 403);
      }

      SECTION("wildcard bind accepts any host but requires a token")
      {
        auto remoteServer = StreamableHttpServer{endpoint, {"0.0.0.0", "secret"}};
        auto remote = FakeHttpConnection{};
        remoteServer.openConnection(remote);

        remoteServer.feed(
          remote,
          post(
            initializeBody,
            {{"Host", "my-pc.lan:47100"}, {"Authorization", "Bearer secret"}}));
        CHECK(parseResponse(remote.take()).status == 200);

        remoteServer.feed(remote, post(initializeBody, {{"Host", "my-pc.lan:47100"}}));
        CHECK(parseResponse(remote.take()).status == 401);
      }
    }

    SECTION("deferred JSON response")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      auto pending = std::shared_ptr<RequestStream>{};
      endpoint.onPost = [&](const auto&, const auto&, auto stream) {
        pending = stream;
        return PostResult{PostStatus::Pending};
      };
      server.feed(connection, post(R"({"jsonrpc":"2.0","id":2,"method":"x"})", session));
      CHECK(connection.written.empty());

      pending->complete(response(2));
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 200);
      CHECK(r.header("Content-Type") == "application/json");
      CHECK(r.json() == response(2));

      // later calls are ignored
      pending->complete(response(3));
      pending->notify(progress(1));
      CHECK(connection.written.empty());
    }

    SECTION("notification before the response switches to SSE")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      auto pending = std::shared_ptr<RequestStream>{};
      endpoint.onPost = [&](const auto&, const auto&, auto stream) {
        pending = stream;
        return PostResult{PostStatus::Pending};
      };
      server.feed(connection, post(R"({"jsonrpc":"2.0","id":2,"method":"x"})", session));
      CHECK(connection.written.empty());

      pending->notify(progress(1));
      auto r = parseResponse(connection.written);
      CHECK(r.status == 200);
      CHECK(r.header("Content-Type") == "text/event-stream");
      CHECK(r.header("Transfer-Encoding") == "chunked");
      CHECK(!r.complete);

      pending->notify(progress(2));
      pending->complete(response(2));
      r = parseResponse(connection.take());
      CHECK(r.complete);

      const auto events = r.events();
      REQUIRE(events.size() == 3);
      CHECK(parseJson(events[0].data) == progress(1));
      CHECK(parseJson(events[1].data) == progress(2));
      CHECK(parseJson(events[2].data) == response(2));
      CHECK(events[0].id == "1");
      CHECK(events[1].id == "2");
      CHECK(events[2].id == "3");
      CHECK(!connection.closed);
    }

    SECTION("output during post is buffered until post returns")
    {
      SECTION("JSON response for a new session")
      {
        endpoint.onPost = [&](const auto&, const auto&, auto stream) {
          stream->complete(response(1));
          CHECK(connection.written.empty());
          return PostResult{PostStatus::Pending, "new"};
        };
        server.feed(connection, post(initializeBody));

        const auto r = parseResponse(connection.take());
        CHECK(r.status == 200);
        CHECK(r.header("Mcp-Session-Id") == "new");
        CHECK(r.json() == response(1));
      }

      SECTION("SSE response for a new session")
      {
        endpoint.onPost = [&](const auto&, const auto&, auto stream) {
          stream->notify(progress(1));
          stream->complete(response(1));
          CHECK(connection.written.empty());
          return PostResult{PostStatus::Pending, "new"};
        };
        server.feed(connection, post(initializeBody));

        const auto r = parseResponse(connection.take());
        CHECK(r.status == 200);
        CHECK(r.header("Content-Type") == "text/event-stream");
        CHECK(r.header("Mcp-Session-Id") == "new");
        CHECK(r.complete);
        REQUIRE(r.events().size() == 2);
        CHECK(r.events()[0].id == "1");
      }

      SECTION("notification during post, response later")
      {
        auto pending = std::shared_ptr<RequestStream>{};
        endpoint.onPost = [&](const auto&, const auto&, auto stream) {
          stream->notify(progress(1));
          pending = stream;
          return PostResult{PostStatus::Pending, "new"};
        };
        server.feed(connection, post(initializeBody));

        auto r = parseResponse(connection.written);
        CHECK(r.header("Mcp-Session-Id") == "new");
        CHECK(!r.complete);
        REQUIRE(r.events().size() == 1);

        pending->complete(response(1));
        r = parseResponse(connection.take());
        CHECK(r.complete);
        CHECK(r.events().size() == 2);
      }

      SECTION("output is discarded if post does not return Pending")
      {
        endpoint.onPost = [&](const auto&, const auto&, auto stream) {
          stream->notify(progress(1));
          stream->complete(response(1));
          return PostResult{PostStatus::Accepted};
        };
        server.feed(connection, post("{}"));
        const auto r = parseResponse(connection.take());
        CHECK(r.status == 202);
        CHECK(r.body.empty());
      }
    }

    SECTION("keep-alive across requests")
    {
      server.feed(connection, post(initializeBody));
      server.feed(
        connection, post(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", session));
      const auto responses = parseResponses(connection.take());
      REQUIRE(responses.size() == 2);
      CHECK(responses[0].status == 200);
      CHECK(responses[1].json() == response(2));
      CHECK(!connection.closed);
    }

    SECTION("Connection: close")
    {
      server.feed(connection, post(initializeBody, {{"Connection", "close"}}));
      const auto r = parseResponse(connection.take());
      CHECK(r.status == 200);
      CHECK(r.header("Connection") == "close");
      CHECK(connection.closed);
    }

    SECTION("pipelined requests wait for the pending response")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      auto pending = std::vector<std::shared_ptr<RequestStream>>{};
      endpoint.onPost = [&](const auto&, const auto&, auto stream) {
        pending.push_back(stream);
        return PostResult{PostStatus::Pending};
      };

      server.feed(
        connection,
        post(R"({"jsonrpc":"2.0","id":1,"method":"x"})", session)
          + post(R"({"jsonrpc":"2.0","id":2,"method":"x"})", session));
      REQUIRE(pending.size() == 1);

      pending[0]->complete(response(1));
      REQUIRE(pending.size() == 2);
      pending[1]->complete(response(2));

      const auto responses = parseResponses(connection.take());
      REQUIRE(responses.size() == 2);
      CHECK(responses[0].json() == response(1));
      CHECK(responses[1].json() == response(2));
    }

    SECTION("GET opens the notification stream")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      server.feed(
        connection,
        request("GET", "", {{"Accept", "text/event-stream"}, {"Mcp-Session-Id", "s1"}}));

      auto r = parseResponse(connection.written);
      CHECK(r.status == 200);
      CHECK(r.header("Content-Type") == "text/event-stream");
      CHECK(!r.complete);

      auto stream = endpoint.notificationStreams["s1"].lock();
      REQUIRE(stream);
      stream->send(Json{{"jsonrpc", "2.0"}, {"method", "a"}});
      stream->send(Json{{"jsonrpc", "2.0"}, {"method", "b"}});

      r = parseResponse(connection.written);
      const auto events = r.events();
      REQUIRE(events.size() == 2);
      CHECK(parseJson(events[0].data).value()["method"] == "a");
      CHECK(events[0].id == "1");
      CHECK(events[1].id == "2");
    }

    SECTION("event IDs are per session and shared by GET and POST streams")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      server.feed(
        connection,
        request("GET", "", {{"Accept", "text/event-stream"}, {"Mcp-Session-Id", "s1"}}));
      endpoint.notificationStreams["s1"].lock()->send(progress(0));

      auto other = FakeHttpConnection{};
      server.openConnection(other);
      endpoint.onPost = [&](const auto&, const auto&, auto stream) {
        stream->notify(progress(1));
        stream->complete(response(1));
        return PostResult{PostStatus::Pending};
      };
      server.feed(other, post("{}", session));

      const auto events = parseResponse(other.take()).events();
      REQUIRE(events.size() == 2);
      CHECK(events[0].id == "2");
      CHECK(events[1].id == "3");
    }

    SECTION("GET errors")
    {
      endpoint.sessions["s1"] = "2025-11-25";

      server.feed(connection, request("GET", "", {{"Accept", "text/event-stream"}}));
      CHECK(parseResponse(connection.take()).status == 400);

      server.feed(
        connection,
        request("GET", "", {{"Accept", "text/event-stream"}, {"Mcp-Session-Id", "x"}}));
      CHECK(parseResponse(connection.take()).status == 404);

      server.feed(
        connection,
        request("GET", "", {{"Accept", "application/json"}, {"Mcp-Session-Id", "s1"}}));
      CHECK(parseResponse(connection.take()).status == 406);
      CHECK(endpoint.notificationStreams.empty());
    }

    SECTION("a second GET replaces the first stream")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      const auto get =
        request("GET", "", {{"Accept", "text/event-stream"}, {"Mcp-Session-Id", "s1"}});
      server.feed(connection, get);
      auto first = endpoint.notificationStreams["s1"].lock();

      auto other = FakeHttpConnection{};
      other.onClose = [&]() { server.connectionClosed(other); };
      server.openConnection(other);
      server.feed(other, get);

      CHECK(connection.closed);
      CHECK(parseResponse(connection.take()).complete);

      first->send(progress(1));
      CHECK(connection.written.empty());

      endpoint.notificationStreams["s1"].lock()->send(progress(2));
      CHECK(parseResponse(other.take()).events().size() == 1);
    }

    SECTION("DELETE")
    {
      endpoint.sessions["s1"] = "2025-11-25";

      auto other = FakeHttpConnection{};
      other.onClose = [&]() { server.connectionClosed(other); };
      server.openConnection(other);
      server.feed(
        other,
        request("GET", "", {{"Accept", "text/event-stream"}, {"Mcp-Session-Id", "s1"}}));

      server.feed(connection, request("DELETE", "", session));
      CHECK(parseResponse(connection.take()).status == 200);
      CHECK(endpoint.deletedSessions == std::vector<std::string>{"s1"});
      CHECK(other.closed);

      server.feed(connection, request("DELETE", "", session));
      CHECK(parseResponse(connection.take()).status == 404);

      server.feed(connection, request("DELETE", ""));
      CHECK(parseResponse(connection.take()).status == 400);
    }
  }

  SECTION("connectionClosed")
  {
    endpoint.sessions["s1"] = "2025-11-25";
    endpoint.onPost = [&](const auto&, const auto&, auto) {
      return PostResult{PostStatus::Pending};
    };
    server.feed(connection, post(R"({"jsonrpc":"2.0","id":2,"method":"x"})", session));
    CHECK(!endpoint.lastStream.expired());

    server.connectionClosed(connection);
    CHECK(endpoint.lastStream.expired());
    CHECK(server.connectionCount() == 0);

    // unknown connections are ignored
    server.connectionClosed(connection);
    server.feed(connection, post("{}"));
    CHECK(connection.written.empty());
  }

  SECTION("sendKeepAlives")
  {
    endpoint.sessions["s1"] = "2025-11-25";
    auto idle = FakeHttpConnection{};
    server.openConnection(idle);

    server.feed(
      connection,
      request("GET", "", {{"Accept", "text/event-stream"}, {"Mcp-Session-Id", "s1"}}));
    connection.take();

    server.sendKeepAlives();
    CHECK(connection.take() == "D\r\n: keepalive\n\n\r\n");
    CHECK(idle.written.empty());
  }

  SECTION("closeAllConnections")
  {
    auto other = FakeHttpConnection{};
    other.onClose = [&]() { server.connectionClosed(other); };
    server.openConnection(other);
    CHECK(server.connectionCount() == 2);

    server.closeAllConnections();
    CHECK(connection.closed);
    CHECK(other.closed);
    CHECK(server.connectionCount() == 0);
  }

  SECTION("connectionCount")
  {
    CHECK(server.connectionCount() == 1);
    auto other = FakeHttpConnection{};
    server.openConnection(other);
    CHECK(server.connectionCount() == 2);
    server.connectionClosed(other);
    CHECK(server.connectionCount() == 1);
  }

  SECTION("nextEventId")
  {
    CHECK(server.nextEventId("a") == 1);
    CHECK(server.nextEventId("a") == 2);
    CHECK(server.nextEventId("b") == 1);
  }
}

} // namespace tb::mcp
