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

#pragma once

#include "mcp/HttpParser.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tb::mcp
{
class Endpoint;

/**
 * A byte stream connection to one HTTP client, e.g. a TCP socket. Implemented by the
 * transport adapter (ui::McpTcpTransport) and by fakes in tests.
 */
class HttpConnection
{
public:
  virtual ~HttpConnection();

  /**
   * Queues the given bytes for sending.
   */
  virtual void write(std::string_view bytes) = 0;

  /**
   * Closes the connection after all queued bytes were sent. The implementation may call
   * `StreamableHttpServer::connectionClosed` from within this function.
   */
  virtual void close() = 0;
};

/**
 * The MCP Streamable HTTP transport (server side) as a state machine over abstract
 * connections. It parses requests, enforces the security rules (Origin, Host, bearer
 * token), and drives an `Endpoint`:
 *
 * - `POST <path>`: calls `Endpoint::post`. The response is `202` without a body,
 *   `application/json` with the final response, or `text/event-stream` if the endpoint
 *   sent a notification before the final response.
 * - `GET <path>`: opens the standalone SSE notification stream of a session.
 * - `DELETE <path>`: terminates a session.
 *
 * Requests on one connection are answered in order (keep-alive and pipelining). While a
 * response is pending or an SSE stream is open, later requests on the same connection
 * wait.
 *
 * All functions must be called on the thread that owns the endpoint.
 */
class StreamableHttpServer
{
public:
  struct Config
  {
    /** The address the transport listens on; used for the Host check and for auth. */
    std::string bindAddress = "127.0.0.1";
    /** The bearer token; required iff the bind address is not a loopback address. */
    std::string accessToken = {};
    std::size_t maxBodySize = HttpParser::DefaultMaxBodySize;
    /** The path of the MCP endpoint. */
    std::string path = "/mcp";
  };

  struct ConnectionState;

private:
  Endpoint& m_endpoint;
  Config m_config;
  std::unordered_map<HttpConnection*, std::shared_ptr<ConnectionState>> m_connections;
  std::unordered_map<std::string, std::weak_ptr<ConnectionState>> m_notificationStreams;
  std::unordered_map<std::string, std::uint64_t> m_nextEventIds;

public:
  /**
   * Returns whether the given address is a loopback address (`127.0.0.0/8`, `::1` or
   * `localhost`).
   */
  static bool isLoopbackAddress(std::string_view address);

  /**
   * Returns an error message if the given configuration is invalid, i.e. the bind
   * address is not a loopback address and the access token is empty.
   */
  static std::optional<std::string> validateConfig(const Config& config);

  /**
   * Creates a server that drives the given endpoint. The configuration must be valid
   * (see validateConfig).
   */
  StreamableHttpServer(Endpoint& endpoint, Config config);
  ~StreamableHttpServer();

  StreamableHttpServer(const StreamableHttpServer&) = delete;
  StreamableHttpServer& operator=(const StreamableHttpServer&) = delete;

  const Config& config() const;

  /**
   * Registers a new connection. The connection must stay valid until
   * `connectionClosed` was called for it or the server is destroyed.
   */
  void openConnection(HttpConnection& connection);

  /**
   * Processes bytes received on the given connection.
   */
  void feed(HttpConnection& connection, std::string_view bytes);

  /**
   * Notifies the server that the given connection was closed (by either side). Drops
   * any pending request stream or notification stream of the connection. The server
   * does not use the connection afterwards. Calling this for an unknown connection has
   * no effect.
   */
  void connectionClosed(HttpConnection& connection);

  /**
   * Writes an SSE keep-alive comment to every open SSE stream.
   */
  void sendKeepAlives();

  /**
   * Closes all connections and drops all streams.
   */
  void closeAllConnections();

  std::size_t connectionCount() const;

  /**
   * Returns the next SSE event ID for the given session. IDs start at 1 and increase
   * monotonically per session.
   */
  std::uint64_t nextEventId(const std::string& sessionId);

private:
  void processRequests(const std::shared_ptr<ConnectionState>& state);
  void completeExchange(const std::shared_ptr<ConnectionState>& state);
  void closeConnection(const std::shared_ptr<ConnectionState>& state);

  void handleRequest(
    const std::shared_ptr<ConnectionState>& state, const HttpRequest& request);
  void handlePost(
    const std::shared_ptr<ConnectionState>& state, const HttpRequest& request);
  void handleGet(
    const std::shared_ptr<ConnectionState>& state, const HttpRequest& request);
  void handleDelete(
    const std::shared_ptr<ConnectionState>& state, const HttpRequest& request);

  bool checkSecurity(
    const std::shared_ptr<ConnectionState>& state, const HttpRequest& request);
  bool checkSession(
    const std::shared_ptr<ConnectionState>& state,
    const HttpRequest& request,
    const std::string& sessionId);

  void endNotificationStream(const std::string& sessionId);
};

} // namespace tb::mcp
