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

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("Args")
{
  const auto args = Args{Json::parse(
    R"({"n":3,"s":"x","v":[1,2,3],"b":{"min":[0,0,0],"max":[1,2,3]},"null":null})")};

  CHECK(args.has("n"));
  CHECK(!args.has("null"));
  CHECK(!args.has("missing"));
  CHECK(args.get<int>("n") == 3);
  CHECK(args.get<std::string>("s") == "x");
  CHECK(args.get<vm::vec3d>("v") == vm::vec3d{1, 2, 3});
  CHECK(args.get<vm::bbox3d>("b") == vm::bbox3d{{0, 0, 0}, {1, 2, 3}});
  CHECK(args.getOptional<int>("missing") == std::nullopt);
  CHECK(args.getOr<int>("missing", 7) == 7);
}

} // namespace tb::mcp
