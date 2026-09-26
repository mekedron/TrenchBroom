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

#include "mcp/Args.h"
#include "mcp/Pagination.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("Pagination")
{
  SECTION("base64")
  {
    for (const auto* str : {"", "a", "ab", "abc", "abcd", "{\"o\":10,\"m\":3}"})
    {
      CHECK(base64Decode(base64Encode(str)) == std::string{str});
    }
    CHECK(base64Encode("abc") == "YWJj");
    CHECK(base64Decode("YWJj!") == std::nullopt);
    CHECK(base64Decode("Y=Jj") == std::nullopt);
  }

  SECTION("cursor")
  {
    const auto cursor = encodeCursor(10, 3);
    const auto decoded = decodeCursor(cursor);
    REQUIRE(decoded.has_value());
    CHECK(decoded->offset == 10);
    CHECK(decoded->modificationCount == 3);
    CHECK(decodeCursor("garbage") == std::nullopt);
  }

  SECTION("pageRequest and makePage")
  {
    auto items = std::vector<Json>{};
    for (int i = 0; i < 5; ++i)
    {
      items.push_back(Json{{"i", i}, {"nested", {{"a", i}, {"b", -i}}}});
    }

    const auto first = pageRequest(Args{Json{{"limit", 2}}}, 7);
    REQUIRE(first.is_success());
    const auto page1 = makePage(items, first.value(), 7);
    CHECK(page1["items"].size() == 2);
    CHECK(page1["total"] == 5);
    REQUIRE(page1["nextCursor"].is_string());

    const auto second =
      pageRequest(Args{Json{{"limit", 10}, {"cursor", page1["nextCursor"]}}}, 7);
    REQUIRE(second.is_success());
    CHECK(!second.value().stale);
    const auto page2 = makePage(items, second.value(), 7);
    CHECK(page2["items"].size() == 3);
    CHECK(page2["items"][0]["i"] == 2);
    CHECK(page2["nextCursor"].is_null());

    const auto stale =
      pageRequest(Args{Json{{"limit", 10}, {"cursor", page1["nextCursor"]}}}, 8);
    CHECK(stale.value().stale);
    CHECK(makePage(items, stale.value(), 8)["stale"] == true);

    CHECK(pageRequest(Args{Json{{"cursor", "bad"}}}, 0).is_error());
  }

  SECTION("selectFields")
  {
    const auto item = Json::parse(
      R"({"id":"brush:1","bounds":{"min":[0,0,0]},"faces":[{"material":"a","index":0},{"material":"b","index":1}]})");
    CHECK(selectFields(item, {}) == item);
    CHECK(selectFields(item, {"id"}) == Json{{"id", "brush:1"}});
    CHECK(
      selectFields(item, {"id", "faces.material"})
      == Json::parse(R"({"id":"brush:1","faces":[{"material":"a"},{"material":"b"}]})"));
    CHECK(selectFields(item, {"missing"}) == Json::object());
  }
}

} // namespace tb::mcp
