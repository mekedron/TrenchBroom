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

#include "mcp/JsonRpc.h"

namespace tb::mcp::jsonrpc
{
namespace
{

bool isValidId(const Json& id)
{
  return id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

bool isValidParams(const Json& params)
{
  return params.is_null() || params.is_object() || params.is_array();
}

} // namespace

std::variant<Payload, ErrorResponse> parsePayload(const std::string_view text)
{
  auto value = parseJson(text);
  if (!value)
  {
    return ErrorResponse{nullptr, ErrorCode::ParseError, "Parse error", nullptr};
  }

  if (value->is_array())
  {
    if (value->empty())
    {
      return ErrorResponse{
        nullptr, ErrorCode::InvalidRequest, "Invalid request: empty batch", nullptr};
    }

    auto payload = Payload{true, {}};
    for (const auto& element : *value)
    {
      payload.messages.push_back(parseMessage(element));
    }
    return payload;
  }

  if (!value->is_object())
  {
    return ErrorResponse{
      nullptr,
      ErrorCode::InvalidRequest,
      "Invalid request: expected an object or an array",
      nullptr};
  }

  return Payload{false, {parseMessage(*value)}};
}

ParsedMessage parseMessage(const Json& value)
{
  if (!value.is_object())
  {
    return InvalidMessage{
      nullptr, ErrorCode::InvalidRequest, "Invalid request: expected an object"};
  }

  const auto* id = findMember(value, "id");
  const auto errorId = id && isValidId(*id) ? *id : Json(nullptr);

  const auto* version = findMember(value, "jsonrpc");
  if (!version || *version != "2.0")
  {
    return InvalidMessage{
      errorId, ErrorCode::InvalidRequest, "Invalid request: jsonrpc must be \"2.0\""};
  }

  if (const auto* method = findMember(value, "method"))
  {
    if (!method->is_string())
    {
      return InvalidMessage{
        errorId, ErrorCode::InvalidRequest, "Invalid request: method must be a string"};
    }

    const auto* params = findMember(value, "params");
    auto paramsValue = params ? *params : Json(nullptr);
    if (!isValidParams(paramsValue))
    {
      return InvalidMessage{
        errorId,
        ErrorCode::InvalidRequest,
        "Invalid request: params must be an object or an array"};
    }

    if (!id)
    {
      return Message{Notification{method->get<std::string>(), std::move(paramsValue)}};
    }
    if (!isValidId(*id))
    {
      return InvalidMessage{
        nullptr,
        ErrorCode::InvalidRequest,
        "Invalid request: id must be a string or an integer"};
    }
    return Message{Request{*id, method->get<std::string>(), std::move(paramsValue)}};
  }

  if (const auto* result = findMember(value, "result"))
  {
    return Message{Response{id ? *id : Json(nullptr), *result}};
  }

  if (const auto* error = findMember(value, "error"))
  {
    const auto* code = findMember(*error, "code");
    const auto* message = findMember(*error, "message");
    const auto* data = findMember(*error, "data");
    return Message{ErrorResponse{
      id ? *id : Json(nullptr),
      code && code->is_number_integer() ? code->get<int>() : ErrorCode::InternalError,
      message && message->is_string() ? message->get<std::string>() : std::string{},
      data ? *data : Json(nullptr)}};
  }

  return InvalidMessage{
    errorId,
    ErrorCode::InvalidRequest,
    "Invalid request: expected a method, a result or an error"};
}

Json toJson(const Message& message)
{
  return std::visit(
    [](const auto& m) -> Json {
      using T = std::decay_t<decltype(m)>;
      if constexpr (std::is_same_v<T, Request>)
      {
        return makeRequest(m.id, m.method, m.params);
      }
      else if constexpr (std::is_same_v<T, Notification>)
      {
        return makeNotification(m.method, m.params);
      }
      else if constexpr (std::is_same_v<T, Response>)
      {
        return makeResponse(m.id, m.result);
      }
      else
      {
        return makeError(m.id, m.code, m.message, m.data);
      }
    },
    message);
}

Json makeRequest(Json id, std::string method, Json params)
{
  auto result =
    Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", std::move(method)}};
  if (!params.is_null())
  {
    result["params"] = std::move(params);
  }
  return result;
}

Json makeNotification(std::string method, Json params)
{
  auto result = Json{{"jsonrpc", "2.0"}, {"method", std::move(method)}};
  if (!params.is_null())
  {
    result["params"] = std::move(params);
  }
  return result;
}

Json makeResponse(Json id, Json result)
{
  return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}

Json makeError(Json id, const int code, std::string message, Json data)
{
  auto error = Json{{"code", code}, {"message", std::move(message)}};
  if (!data.is_null())
  {
    error["data"] = std::move(data);
  }
  return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", std::move(error)}};
}

} // namespace tb::mcp::jsonrpc
