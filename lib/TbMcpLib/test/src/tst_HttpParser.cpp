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

#include "mcp/HttpParser.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("HttpParser")
{
  const auto request = std::string{
    "POST /mcp?x=1 HTTP/1.1\r\n"
    "Host: localhost:47100\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 7\r\n"
    "\r\n"
    "{\"a\":1}"};

  SECTION("findHttpHeader")
  {
    const auto headers = HttpHeaders{{"Content-Type", "a"}, {"content-type", "b"}};
    CHECK(findHttpHeader(headers, "CONTENT-TYPE") == "a");
    CHECK(findHttpHeader(headers, "Accept") == std::nullopt);
  }

  SECTION("HttpRequest::keepAlive")
  {
    auto r = HttpRequest{};
    r.minorVersion = 1;
    CHECK(r.keepAlive());
    r.headers = {{"Connection", "Close"}};
    CHECK(!r.keepAlive());
    r.minorVersion = 0;
    r.headers = {};
    CHECK(!r.keepAlive());
    r.headers = {{"Connection", "Keep-Alive"}};
    CHECK(r.keepAlive());
  }

  SECTION("feed")
  {
    SECTION("complete request in one chunk")
    {
      auto parser = HttpParser{};
      parser.feed(request);
      REQUIRE(parser.hasRequest());

      const auto r = parser.nextRequest();
      REQUIRE(r);
      CHECK(r->method == "POST");
      CHECK(r->target == "/mcp?x=1");
      CHECK(r->path == "/mcp");
      CHECK(r->minorVersion == 1);
      CHECK(r->header("host") == "localhost:47100");
      CHECK(r->header("CONTENT-TYPE") == "application/json");
      CHECK(r->body == R"({"a":1})");
      CHECK(r->keepAlive());
      CHECK(!parser.hasRequest());
      CHECK(parser.error() == std::nullopt);
      CHECK(parser.bufferedSize() == 0);
    }

    SECTION("request split into single bytes")
    {
      auto parser = HttpParser{};
      for (size_t i = 0; i < request.size(); ++i)
      {
        CHECK(!parser.hasRequest());
        parser.feed(request.substr(i, 1));
      }
      const auto r = parser.nextRequest();
      REQUIRE(r);
      CHECK(r->body == R"({"a":1})");
    }

    SECTION("pipelined requests")
    {
      auto parser = HttpParser{};
      parser.feed(request + request + "GET /mcp HTTP/1.1\r\nHost: x\r\n\r\nDEL");
      CHECK(parser.nextRequest());
      CHECK(parser.nextRequest());
      const auto get = parser.nextRequest();
      REQUIRE(get);
      CHECK(get->method == "GET");
      CHECK(get->body.empty());
      CHECK(!parser.nextRequest());
      CHECK(parser.bufferedSize() == 3);
    }

    SECTION("bare LF line endings and leading empty lines")
    {
      auto parser = HttpParser{};
      parser.feed("\r\n\nGET /mcp HTTP/1.0\nHost: x\nX-Y:  a b \n\n");
      const auto r = parser.nextRequest();
      REQUIRE(r);
      CHECK(r->minorVersion == 0);
      CHECK(r->header("X-Y") == "a b");
      CHECK(!r->keepAlive());
    }

    SECTION("absolute-form target")
    {
      auto parser = HttpParser{};
      parser.feed("GET http://localhost:47100/mcp?a HTTP/1.1\r\n\r\n");
      const auto r = parser.nextRequest();
      REQUIRE(r);
      CHECK(r->path == "/mcp");
    }

    SECTION("chunked request body")
    {
      auto parser = HttpParser{};
      parser.feed("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
      REQUIRE(parser.error());
      CHECK(parser.error()->status == 411);
      CHECK(!parser.hasRequest());
    }

    SECTION("body too large")
    {
      auto parser = HttpParser{10};
      parser.feed("POST /mcp HTTP/1.1\r\nContent-Length: 11\r\n\r\n");
      REQUIRE(parser.error());
      CHECK(parser.error()->status == 413);

      auto defaultParser = HttpParser{};
      defaultParser.feed("POST /mcp HTTP/1.1\r\nContent-Length: 16777217\r\n\r\n");
      REQUIRE(defaultParser.error());
      CHECK(defaultParser.error()->status == 413);

      auto huge = HttpParser{};
      huge.feed(
        "POST /mcp HTTP/1.1\r\nContent-Length: 99999999999999999999999999\r\n\r\n");
      REQUIRE(huge.error());
      CHECK(huge.error()->status == 413);
    }

    SECTION("body at the limit")
    {
      auto parser = HttpParser{3};
      parser.feed("POST /mcp HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc");
      CHECK(parser.error() == std::nullopt);
      CHECK(parser.nextRequest());
    }

    SECTION("header too large")
    {
      auto parser = HttpParser{16, 32};
      parser.feed("GET /mcp HTTP/1.1\r\nX-Long: " + std::string(64, 'a'));
      REQUIRE(parser.error());
      CHECK(parser.error()->status == 431);
    }

    SECTION("malformed requests")
    {
      const auto status = [](const std::string& text) {
        auto parser = HttpParser{};
        parser.feed(text);
        return parser.error() ? parser.error()->status : 0;
      };

      CHECK(status("GET\r\n\r\n") == 400);
      CHECK(status("GET /mcp\r\n\r\n") == 400);
      CHECK(status("GET /mcp FOO/1.1\r\n\r\n") == 400);
      CHECK(status("G(T /mcp HTTP/1.1\r\n\r\n") == 400);
      CHECK(status("GET /mcp HTTP/2.0\r\n\r\n") == 505);
      CHECK(status("GET /mcp HTTP/1.1\r\nNoColon\r\n\r\n") == 400);
      CHECK(status("GET /mcp HTTP/1.1\r\nA: b\r\n folded\r\n\r\n") == 400);
      CHECK(status("POST /mcp HTTP/1.1\r\nContent-Length: x\r\n\r\n") == 400);
      CHECK(
        status("POST /mcp HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n")
        == 400);
      CHECK(
        status("POST /mcp HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx")
        == 0);
    }

    SECTION("input after an error is ignored")
    {
      auto parser = HttpParser{};
      parser.feed(request);
      parser.feed("GET / HTTP/3.0\r\n\r\n");
      parser.feed(request);
      CHECK(parser.error()->status == 505);
      CHECK(parser.nextRequest());
      CHECK(!parser.nextRequest());
    }
  }
}

} // namespace tb::mcp
