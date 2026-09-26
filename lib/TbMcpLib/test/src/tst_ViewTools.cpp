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

#include "mcp/McpToolFixture.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("ViewTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& grid = document.map().grid();
  REQUIRE(grid.size() == 4);

  SECTION("grid_get")
  {
    const auto result = fixture.call("grid_get");
    CHECK(result["size"] == 16);
    CHECK(result["exponent"] == 4);
    CHECK(result["effectiveSize"] == 16);
    CHECK(result["visible"] == grid.visible());
    CHECK(result["snap"] == grid.snap());
    CHECK(result["angle"] == 15);

    grid.toggleSnap();
    const auto noSnap = fixture.call("grid_get");
    CHECK(noSnap["snap"] == false);
    CHECK(noSnap["size"] == 16);
    CHECK(noSnap["effectiveSize"] == 1);
  }

  SECTION("grid_set")
  {
    SECTION("size")
    {
      const auto result = fixture.call("grid_set", Json{{"size", 32}});
      CHECK(result["undoStep"].is_null());
      CHECK(result["result"]["size"] == 32);
      CHECK(result["result"]["exponent"] == 5);
      CHECK(result["result"]["previous"]["size"] == 16);
      CHECK(result["grid"] == 32);
      CHECK(grid.size() == 5);

      fixture.call("grid_set", Json{{"size", 0.125}});
      CHECK(grid.size() == -3);
    }

    SECTION("exponent, visible and snap")
    {
      const auto visible = grid.visible();
      const auto result = fixture.call(
        "grid_set", Json{{"exponent", 3}, {"visible", !visible}, {"snap", false}});
      CHECK(result["result"]["size"] == 8);
      CHECK(result["result"]["effectiveSize"] == 1);
      CHECK(grid.size() == 3);
      CHECK(grid.visible() == !visible);
      CHECK_FALSE(grid.snap());

      fixture.call("grid_set", Json{{"snap", true}});
      CHECK(grid.snap());
      CHECK(grid.size() == 3);
    }

    SECTION("dry run")
    {
      const auto result =
        fixture.call("grid_set", Json{{"size", 64}, {"snap", false}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["size"] == 64);
      CHECK(grid.size() == 4);
      CHECK(grid.snap());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 24}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 512}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 0}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"exponent", 9}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 16}, {"exponent", 4}}).code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.callExpectingError("grid_set").code == ErrorCode::InvalidArgument);

      const auto error = fixture.callExpectingError("grid_set", Json{{"size", 24}});
      CHECK(error.hint.find("0.125") != std::string::npos);
      CHECK(grid.size() == 4);
    }
  }
}

} // namespace tb::mcp
