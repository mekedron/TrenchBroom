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

#include "mcp/Json.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("Json")
{
  SECTION("roundForOutput")
  {
    CHECK(roundForOutput(63.99999999997) == 64.0);
    CHECK(roundForOutput(0.1234567) == 0.123457);
    CHECK(roundForOutput(-0.0000001) == 0.0);
  }

  SECTION("parseJson")
  {
    CHECK(parseJson(R"({"a":1})") == Json{{"a", 1}});
    CHECK(parseJson("{") == std::nullopt);
  }

  SECTION("dumpJson")
  {
    CHECK(dumpJson(Json{{"b", 1}, {"a", 2}}) == R"({"b":1,"a":2})");
  }

  SECTION("findMember")
  {
    const auto value = Json{{"a", 1}};
    REQUIRE(findMember(value, "a") != nullptr);
    CHECK(*findMember(value, "a") == 1);
    CHECK(findMember(value, "b") == nullptr);
    CHECK(findMember(Json::array(), "a") == nullptr);
  }
}

} // namespace tb::mcp
