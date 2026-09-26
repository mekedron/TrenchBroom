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

#include "mcp/Errors.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("Errors")
{
  SECTION("toString and errorCodeFromString")
  {
    CHECK(toString(ErrorCode::InvalidArgument) == "INVALID_ARGUMENT");
    CHECK(toString(ErrorCode::DryRunUnsupported) == "DRY_RUN_UNSUPPORTED");
    CHECK(errorCodeFromString("BUSY_TIMEOUT") == ErrorCode::BusyTimeout);
    CHECK(errorCodeFromString("nope") == std::nullopt);
  }

  SECTION("toJson")
  {
    const auto error =
      makeError(ErrorCode::ObjectNotFound, "Gone.", "Undo it.", {"brush:1"});
    CHECK(
      toJson(error)
      == Json{
        {"code", "OBJECT_NOT_FOUND"},
        {"message", "Gone."},
        {"objectIds", {"brush:1"}},
        {"hint", "Undo it."},
      });

    CHECK(!toJson(makeError(ErrorCode::InternalError, "x")).contains("hint"));
  }

  SECTION("toText")
  {
    CHECK(
      toText(makeError(ErrorCode::NoSelection, "Nothing selected.", "Pass ids."))
      == "NO_SELECTION: Nothing selected. Hint: Pass ids.");
  }

  SECTION("Warning")
  {
    CHECK(
      toJson(Warning{"UNKNOWN_CLASSNAME", "Unknown class.", {"entity:1"}})
      == Json{
        {"code", "UNKNOWN_CLASSNAME"},
        {"message", "Unknown class."},
        {"objectIds", {"entity:1"}}});
  }
}

} // namespace tb::mcp
