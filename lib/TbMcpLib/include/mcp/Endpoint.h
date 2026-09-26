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

#include "mcp/Json.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tb::mcp
{

/**
 * Receives the messages that the server produces for one POSTed JSON-RPC payload.
 *
 * The endpoint keeps only a weak reference to the stream. The transport owns it and drops
 * it when the connection closes; any later output for it is discarded.
 */
class RequestStream
{
public:
  virtual ~RequestStream();

  /**
   * Sends a notification that belongs to the request, e.g. `notifications/progress`.
   * Transports that receive a notification before the final response switch the HTTP
   * response to `text/event-stream`.
   */
  virtual void notify(const Json& notification) = 0;

  /**
   * Completes the exchange with the final JSON-RPC response. For a batch, this is the
   * array of responses. Called exactly once per stream whose post returned
   * PostStatus::Pending.
   */
  virtual void complete(const Json& response) = 0;
};

/**
 * The standalone server-to-client stream of a session (the HTTP GET stream).
 */
class NotificationStream
{
public:
  virtual ~NotificationStream();

  virtual void send(const Json& notification) = 0;
};

enum class PostStatus
{
  /** The payload only contained notifications or responses: HTTP 202, no body. */
  Accepted,
  /**
   * The payload contained requests: HTTP 200. The response is delivered through the
   * request stream, possibly before `post` returns.
   */
  Pending,
  /** The payload was rejected: HTTP 400 with `body` (a JSON-RPC error) if not null. */
  BadRequest,
  /** The session id is unknown or expired: HTTP 404. */
  SessionNotFound,
};

struct PostResult
{
  PostStatus status;
  /** Set when the payload was an `initialize` request that created a new session. */
  std::string newSessionId = {};
  /** For BadRequest: the JSON-RPC error response to send as the body. */
  Json body = nullptr;
};

/**
 * The transport-facing interface of the MCP server. `McpServer` implements it; the
 * Streamable HTTP state machine (`StreamableHttpServer`) drives it. All functions must be
 * called on the thread that owns the server.
 */
class Endpoint
{
public:
  virtual ~Endpoint();

  /**
   * Handles one POSTed body (a JSON-RPC message or a batch).
   *
   * The endpoint may call the stream's functions before this function returns. The
   * transport must therefore buffer any output until `post` has returned so that it can
   * include the `Mcp-Session-Id` header of a new session in the response headers.
   *
   * @param sessionId the value of the `Mcp-Session-Id` header, if any
   * @param body the request body
   * @param stream receives notifications and the final response
   */
  virtual PostResult post(
    const std::optional<std::string>& sessionId,
    std::string_view body,
    std::shared_ptr<RequestStream> stream) = 0;

  /**
   * Returns the negotiated protocol version of the given session, or nullopt if there is
   * no such session.
   */
  virtual std::optional<std::string> sessionProtocolVersion(
    std::string_view sessionId) const = 0;

  /**
   * Attaches the standalone notification stream (HTTP GET) of the given session,
   * replacing any previous one. Returns false if there is no such session.
   */
  virtual bool openNotificationStream(
    std::string_view sessionId, std::shared_ptr<NotificationStream> stream) = 0;

  /**
   * Terminates the given session (HTTP DELETE). Returns false if there is no such
   * session.
   */
  virtual bool deleteSession(std::string_view sessionId) = 0;
};

} // namespace tb::mcp
