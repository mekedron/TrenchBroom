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

#include "mcp/JsonVm.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("JsonVm")
{
  CHECK(
    toJson(vm::vec3d{1, 63.99999999997, -0.0000001}) == Json::array({1.0, 64.0, 0.0}));
  CHECK(toJson(vm::vec2d{1, 2}) == Json::array({1.0, 2.0}));
  CHECK(
    toJson(vm::bbox3d{{0, 0, 0}, {1, 1, 1}})
    == Json{{"min", {0.0, 0.0, 0.0}}, {"max", {1.0, 1.0, 1.0}}});
  CHECK(vec3FromJson(Json::array({1, 2})) == std::nullopt);
  CHECK(vec2FromJson(Json::array({1, 2})) == vm::vec2d{1, 2});
  CHECK(boxFromJson(Json::object()) == std::nullopt);
}

} // namespace tb::mcp
