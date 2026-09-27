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

#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include "mcp/Endpoint.h"
#include "mcp/FakeHost.h"
#include "mcp/Json.h"
#include "mcp/McpServer.h"
#include "mcp/RegisterAll.h"
#include "ui/McpTcpTransport.h"
#include "ui/QtScheduler.h"

#include <map>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/**
 * `initialize` creates session "s1" and answers synchronously; method "slow" sends a
 * progress notification during post and answers 50 ms later; other requests are
 * answered synchronously; notifications are accepted.
 */
class FakeEndpoint : public mcp::Endpoint
{
public:
  std::map<std::string, std::string, std::less<>> sessions;
  std::map<std::string, std::weak_ptr<mcp::NotificationStream>, std::less<>>
    notificationStreams;
  QtScheduler scheduler;

  mcp::PostResult post(
    const std::optional<std::string>& sessionId,
    const std::string_view body,
    std::shared_ptr<mcp::RequestStream> stream) override
  {
    const auto message = mcp::parseJson(body).value_or(mcp::Json{});
    const auto method = message.value("method", "");
    const auto id = message.contains("id") ? message["id"] : mcp::Json{};
    const auto response =
      mcp::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"method", method}}}};

    if (method == "initialize")
    {
      sessions["s1"] = "2025-11-25";
      stream->complete(response);
      return {mcp::PostStatus::Pending, "s1"};
    }

    if (!sessionId || !sessions.contains(*sessionId))
    {
      return {mcp::PostStatus::SessionNotFound};
    }

    if (id.is_null())
    {
      return {mcp::PostStatus::Accepted};
    }

    if (method == "slow")
    {
      stream->notify(mcp::Json{
        {"jsonrpc", "2.0"},
        {"method", "notifications/progress"},
        {"params", {{"progress", 1}}}});
      scheduler.postDelayed(
        std::chrono::milliseconds{50}, [weakStream = std::weak_ptr{stream}, response]() {
          if (auto s = weakStream.lock())
          {
            s->complete(response);
          }
        });
      return {mcp::PostStatus::Pending};
    }

    stream->complete(response);
    return {mcp::PostStatus::Pending};
  }

  std::optional<std::string> sessionProtocolVersion(
    const std::string_view sessionId) const override
  {
    const auto it = sessions.find(sessionId);
    return it != sessions.end() ? std::optional{it->second} : std::nullopt;
  }

  bool openNotificationStream(
    const std::string_view sessionId,
    std::shared_ptr<mcp::NotificationStream> stream) override
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
    return sessions.erase(std::string{sessionId}) > 0;
  }
};

QByteArray postRequest(
  const quint16 port, const QByteArray& body, const QByteArray& extraHeaders = {})
{
  return "POST /mcp HTTP/1.1\r\n"
         "Host: 127.0.0.1:"
         + QByteArray::number(port)
         + "\r\n"
           "Content-Type: application/json\r\n"
           "Accept: application/json, text/event-stream\r\n"
         + extraHeaders + "Content-Length: " + QByteArray::number(body.size())
         + "\r\n\r\n" + body;
}

/**
 * Reads from the socket until the accumulated data contains the given text.
 */
bool readUntil(QTcpSocket& socket, QByteArray& data, const QByteArray& text)
{
  return QTest::qWaitFor(
    [&]() {
      data += socket.readAll();
      return data.contains(text);
    },
    5000);
}

} // namespace

TEST_CASE("McpTcpTransport")
{
  auto endpoint = FakeEndpoint{};
  auto transport = McpTcpTransport{endpoint};

  SECTION("listen")
  {
    REQUIRE(transport.listen("127.0.0.1", 0));
    CHECK(transport.isListening());
    CHECK(transport.serverPort() != 0);

    SECTION("port in use")
    {
      auto other = McpTcpTransport{endpoint};
      CHECK(!other.listen("127.0.0.1", transport.serverPort()));
      CHECK(!other.errorString().isEmpty());
      CHECK(!other.isListening());
      CHECK(other.serverPort() == 0);
    }

    SECTION("non-loopback address without token")
    {
      auto other = McpTcpTransport{endpoint};
      CHECK(!other.listen("0.0.0.0", 0));
      CHECK(other.errorString().contains("access token"));
    }

    SECTION("non-loopback address with token")
    {
      auto other = McpTcpTransport{endpoint, {.accessToken = "secret"}};
      CHECK(other.listen("0.0.0.0", 0));
    }

    SECTION("invalid address")
    {
      auto other = McpTcpTransport{endpoint};
      CHECK(!other.listen("not an address", 0));
    }
  }

  SECTION("close")
  {
    REQUIRE(transport.listen("localhost", 0));
    transport.close();
    CHECK(!transport.isListening());
    CHECK(transport.serverPort() == 0);
  }

  SECTION("requests")
  {
    REQUIRE(transport.listen("127.0.0.1", 0));
    const auto port = transport.serverPort();

    auto spy = QSignalSpy{&transport, &McpTcpTransport::connectionCountChanged};

    auto socket = QTcpSocket{};
    socket.connectToHost(QHostAddress::LocalHost, port);
    REQUIRE(socket.waitForConnected(5000));
    REQUIRE(QTest::qWaitFor([&]() { return transport.connectionCount() == 1; }, 5000));
    CHECK(spy.count() == 1);

    auto data = QByteArray{};

    SECTION("POST returns JSON with a session id, keep-alive works")
    {
      socket.write(postRequest(
        port, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
      REQUIRE(readUntil(socket, data, R"("method":"initialize"}})"));
      CHECK(data.startsWith("HTTP/1.1 200 OK\r\n"));
      CHECK(data.contains("Mcp-Session-Id: s1\r\n"));
      CHECK(data.contains("Content-Type: application/json\r\n"));

      data.clear();
      socket.write(postRequest(
        port,
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})",
        "Mcp-Session-Id: s1\r\nMCP-Protocol-Version: 2025-11-25\r\n"));
      REQUIRE(readUntil(socket, data, R"("method":"ping"}})"));
      CHECK(data.startsWith("HTTP/1.1 200 OK\r\n"));
      CHECK(socket.state() == QAbstractSocket::ConnectedState);
      CHECK(transport.connectionCount() == 1);
    }

    SECTION("notification before the response switches to SSE")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      socket.write(postRequest(
        port, R"({"jsonrpc":"2.0","id":3,"method":"slow"})", "Mcp-Session-Id: s1\r\n"));
      REQUIRE(readUntil(socket, data, "0\r\n\r\n"));
      CHECK(data.startsWith("HTTP/1.1 200 OK\r\n"));
      CHECK(data.contains("Content-Type: text/event-stream\r\n"));
      CHECK(data.contains("Transfer-Encoding: chunked\r\n"));
      CHECK(data.contains("notifications/progress"));
      CHECK(data.contains(R"("method":"slow"}})"));
      CHECK(data.indexOf("notifications/progress") < data.indexOf(R"("id":3)"));
    }

    SECTION("GET stream receives events")
    {
      endpoint.sessions["s1"] = "2025-11-25";
      socket.write(
        "GET /mcp HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/event-stream\r\n"
        "Mcp-Session-Id: s1\r\n\r\n");
      REQUIRE(readUntil(socket, data, "\r\n\r\n"));
      CHECK(data.startsWith("HTTP/1.1 200 OK\r\n"));
      CHECK(data.contains("Content-Type: text/event-stream\r\n"));

      auto stream = endpoint.notificationStreams["s1"].lock();
      REQUIRE(stream);
      stream->send(
        mcp::Json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
      REQUIRE(readUntil(socket, data, "notifications/tools/list_changed"));
      CHECK(data.contains("id: 1\n"));
    }

    SECTION("closeAllConnections")
    {
      transport.closeAllConnections();
      CHECK(transport.connectionCount() == 0);
      CHECK(spy.count() == 2);
      CHECK(QTest::qWaitFor(
        [&]() { return socket.state() == QAbstractSocket::UnconnectedState; }, 5000));
    }

    SECTION("client disconnect")
    {
      socket.disconnectFromHost();
      CHECK(QTest::qWaitFor([&]() { return transport.connectionCount() == 0; }, 5000));
      CHECK(spy.count() == 2);
    }
  }
}

TEST_CASE("McpTcpTransport with McpServer")
{
  auto host = mcp::FakeHost{};
  auto scheduler = QtScheduler{};
  auto server = mcp::McpServer{host, scheduler, mcp::ServerInfo{.version = "e2e"}};
  mcp::registerAll(server);

  auto transport = McpTcpTransport{server};
  REQUIRE(transport.listen("127.0.0.1", 0));
  const auto port = transport.serverPort();

  auto socket = QTcpSocket{};
  socket.connectToHost(QHostAddress::LocalHost, port);
  REQUIRE(socket.waitForConnected(5000));

  auto data = QByteArray{};

  // initialize
  socket.write(postRequest(
    port,
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"e2e","version":"1"}}})"));
  REQUIRE(readUntil(socket, data, R"("instructions")"));
  REQUIRE(readUntil(socket, data, "}}"));
  CHECK(data.startsWith("HTTP/1.1 200 OK\r\n"));

  const auto sessionHeader = QByteArray{"Mcp-Session-Id: "};
  const auto headerStart = data.indexOf(sessionHeader);
  REQUIRE(headerStart >= 0);
  const auto headerEnd = data.indexOf("\r\n", headerStart);
  const auto sessionId = data.mid(
    headerStart + sessionHeader.size(), headerEnd - headerStart - sessionHeader.size());
  CHECK(sessionId.size() == 32);
  CHECK(server.sessionCount() == 1);

  const auto sessionHeaders =
    "Mcp-Session-Id: " + sessionId + "\r\nMCP-Protocol-Version: 2025-11-25\r\n";

  // notifications/initialized
  data.clear();
  socket.write(postRequest(
    port, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", sessionHeaders));
  REQUIRE(readUntil(socket, data, "\r\n\r\n"));
  CHECK(data.startsWith("HTTP/1.1 202"));

  // tools/list
  data.clear();
  socket.write(postRequest(
    port, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", sessionHeaders));
  REQUIRE(readUntil(socket, data, R"("name":"transaction_rollback")"));
  CHECK(data.contains(R"("name":"editor_status")"));

  // tools/call editor_status
  data.clear();
  socket.write(postRequest(
    port,
    R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"editor_status","arguments":{}}})",
    sessionHeaders));
  REQUIRE(readUntil(socket, data, R"("isError":false)"));
  CHECK(data.contains(R"(\"version\":\"test-version\")"));
  CHECK(data.contains(R"("structuredContent")"));

  // an unknown session gets 404
  data.clear();
  socket.write(postRequest(
    port, R"({"jsonrpc":"2.0","id":4,"method":"ping"})", "Mcp-Session-Id: 0123\r\n"));
  REQUIRE(readUntil(socket, data, "\r\n\r\n"));
  CHECK(data.startsWith("HTTP/1.1 404"));
}

} // namespace tb::ui
