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

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp::jsonrpc
{

TEST_CASE("JsonRpc")
{
  SECTION("parsePayload")
  {
    SECTION("request with a numeric id")
    {
      const auto parsed =
        parsePayload(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"a":1}})");
      const auto& payload = std::get<Payload>(parsed);
      CHECK(!payload.isBatch);
      REQUIRE(payload.messages.size() == 1);
      const auto& request = std::get<Request>(std::get<Message>(payload.messages[0]));
      CHECK(request.id == 1);
      CHECK(request.method == "ping");
      CHECK(request.params == Json{{"a", 1}});
    }

    SECTION("request with a string id")
    {
      const auto parsed = parsePayload(R"({"jsonrpc":"2.0","id":"x","method":"ping"})");
      const auto& request =
        std::get<Request>(std::get<Message>(std::get<Payload>(parsed).messages[0]));
      CHECK(request.id == "x");
      CHECK(request.params.is_null());
    }

    SECTION("notification")
    {
      const auto parsed =
        parsePayload(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
      const auto& notification =
        std::get<Notification>(std::get<Message>(std::get<Payload>(parsed).messages[0]));
      CHECK(notification.method == "notifications/initialized");
    }

    SECTION("response and error")
    {
      const auto response = parsePayload(R"({"jsonrpc":"2.0","id":3,"result":{}})");
      CHECK(std::holds_alternative<Response>(
        std::get<Message>(std::get<Payload>(response).messages[0])));

      const auto error = parsePayload(
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"nope"}})");
      const auto& errorResponse =
        std::get<ErrorResponse>(std::get<Message>(std::get<Payload>(error).messages[0]));
      CHECK(errorResponse.code == ErrorCode::MethodNotFound);
      CHECK(errorResponse.message == "nope");
    }

    SECTION("parse error")
    {
      const auto parsed = parsePayload("{not json");
      REQUIRE(std::holds_alternative<ErrorResponse>(parsed));
      CHECK(std::get<ErrorResponse>(parsed).code == ErrorCode::ParseError);
    }

    SECTION("invalid requests")
    {
      CHECK(
        std::get<ErrorResponse>(parsePayload("[]")).code == ErrorCode::InvalidRequest);
      CHECK(
        std::get<ErrorResponse>(parsePayload("42")).code == ErrorCode::InvalidRequest);

      const auto noVersion = parsePayload(R"({"id":1,"method":"ping"})");
      const auto& invalid =
        std::get<InvalidMessage>(std::get<Payload>(noVersion).messages[0]);
      CHECK(invalid.code == ErrorCode::InvalidRequest);
      CHECK(invalid.id == 1);

      const auto badParams =
        parsePayload(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":3})");
      CHECK(
        std::holds_alternative<InvalidMessage>(std::get<Payload>(badParams).messages[0]));

      const auto badId =
        parsePayload(R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})");
      CHECK(std::get<InvalidMessage>(std::get<Payload>(badId).messages[0]).id.is_null());
    }

    SECTION("batch")
    {
      const auto parsed = parsePayload(
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"x"},5])");
      const auto& payload = std::get<Payload>(parsed);
      CHECK(payload.isBatch);
      REQUIRE(payload.messages.size() == 3);
      CHECK(std::holds_alternative<Message>(payload.messages[0]));
      CHECK(std::holds_alternative<Message>(payload.messages[1]));
      CHECK(std::holds_alternative<InvalidMessage>(payload.messages[2]));
    }
  }

  SECTION("toJson")
  {
    CHECK(
      toJson(Request{1, "ping", nullptr})
      == Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}});
    CHECK(
      toJson(Notification{"n", Json{{"a", 1}}})
      == Json{{"jsonrpc", "2.0"}, {"method", "n"}, {"params", {{"a", 1}}}});
    CHECK(
      toJson(Response{"x", Json::object()})
      == Json{{"jsonrpc", "2.0"}, {"id", "x"}, {"result", Json::object()}});
    CHECK(
      toJson(ErrorResponse{2, -1, "m", nullptr})
      == Json{
        {"jsonrpc", "2.0"}, {"id", 2}, {"error", {{"code", -1}, {"message", "m"}}}});
  }

  SECTION("makeError")
  {
    CHECK(
      makeError(nullptr, ErrorCode::ParseError, "Parse error", Json{{"x", 1}})
      == Json{
        {"jsonrpc", "2.0"},
        {"id", nullptr},
        {"error", {{"code", -32700}, {"message", "Parse error"}, {"data", {{"x", 1}}}}}});
  }
}

} // namespace tb::mcp::jsonrpc
