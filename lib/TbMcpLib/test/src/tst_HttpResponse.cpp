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

#include "mcp/HttpResponse.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("HttpResponse")
{
  SECTION("httpReasonPhrase")
  {
    CHECK(httpReasonPhrase(200) == "OK");
    CHECK(httpReasonPhrase(404) == "Not Found");
    CHECK(httpReasonPhrase(411) == "Length Required");
    CHECK(httpReasonPhrase(999) == "Unknown");
  }

  SECTION("serializeHttpResponse")
  {
    CHECK(
      serializeHttpResponse(HttpResponse{
        200, {{"Content-Type", "application/json"}}, R"({"a":1})"})
      == "HTTP/1.1 200 OK\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: 7\r\n"
         "\r\n"
         R"({"a":1})");
    CHECK(
      serializeHttpResponse(HttpResponse{202})
      == "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n");
  }

  SECTION("serializeSseResponseHead")
  {
    CHECK(
      serializeSseResponseHead(200, {{"Mcp-Session-Id", "abc"}})
      == "HTTP/1.1 200 OK\r\n"
         "Content-Type: text/event-stream\r\n"
         "Cache-Control: no-cache\r\n"
         "Transfer-Encoding: chunked\r\n"
         "Mcp-Session-Id: abc\r\n"
         "\r\n");
  }

  SECTION("encodeHttpChunk")
  {
    CHECK(encodeHttpChunk("hello") == "5\r\nhello\r\n");
    CHECK(
      encodeHttpChunk(std::string(26, 'x')) == "1A\r\n" + std::string(26, 'x') + "\r\n");
    CHECK(encodeHttpChunk("").empty());
  }

  SECTION("lastHttpChunk")
  {
    CHECK(lastHttpChunk() == "0\r\n\r\n");
  }

  SECTION("encodeSseEvent")
  {
    CHECK(encodeSseEvent("{}") == "data: {}\n\n");
    CHECK(encodeSseEvent("{}", "message", "7") == "event: message\nid: 7\ndata: {}\n\n");
    CHECK(encodeSseEvent("a\nb\r\nc\rd") == "data: a\ndata: b\ndata: c\ndata: d\n\n");
    CHECK(encodeSseEvent("") == "data: \n\n");
  }

  SECTION("sseKeepAliveComment")
  {
    CHECK(sseKeepAliveComment() == ": keepalive\n\n");
  }
}

} // namespace tb::mcp
