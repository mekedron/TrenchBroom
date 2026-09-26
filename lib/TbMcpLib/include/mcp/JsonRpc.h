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

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tb::mcp::jsonrpc
{

/** JSON-RPC 2.0 and MCP error codes. */
namespace ErrorCode
{
constexpr int ParseError = -32700;
constexpr int InvalidRequest = -32600;
constexpr int MethodNotFound = -32601;
constexpr int InvalidParams = -32602;
constexpr int InternalError = -32603;
/** Generic server error, e.g. the editor did not start (stdio bridge). */
constexpr int ServerError = -32000;
/** MCP: the requested resource does not exist. */
constexpr int ResourceNotFound = -32002;
/** The request was cancelled by the client. */
constexpr int RequestCancelled = -32800;
} // namespace ErrorCode

struct Request
{
  /** A string or an integer. */
  Json id;
  std::string method;
  /** An object, an array, or null if absent. */
  Json params;
};

struct Notification
{
  std::string method;
  Json params;
};

struct Response
{
  Json id;
  Json result;
};

struct ErrorResponse
{
  /** null if the id of the request could not be determined. */
  Json id;
  int code;
  std::string message;
  Json data;
};

using Message = std::variant<Request, Notification, Response, ErrorResponse>;

/**
 * An invalid element of a payload. It must be answered with an error response.
 */
struct InvalidMessage
{
  Json id;
  int code;
  std::string message;
};

using ParsedMessage = std::variant<Message, InvalidMessage>;

struct Payload
{
  bool isBatch = false;
  std::vector<ParsedMessage> messages;
};

/**
 * Parses a JSON-RPC payload: a single message or a batch.
 *
 * Returns an ErrorResponse (parse error or invalid request) if the payload as a whole is
 * unusable, e.g. not JSON or an empty batch. Individual invalid batch elements are
 * returned as InvalidMessage.
 */
std::variant<Payload, ErrorResponse> parsePayload(std::string_view text);

/** Converts a JSON value to a message. */
ParsedMessage parseMessage(const Json& value);

Json toJson(const Message& message);

Json makeRequest(Json id, std::string method, Json params = nullptr);
Json makeNotification(std::string method, Json params = nullptr);
Json makeResponse(Json id, Json result);
Json makeError(Json id, int code, std::string message, Json data = nullptr);

} // namespace tb::mcp::jsonrpc
