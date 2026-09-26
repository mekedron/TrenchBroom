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

#include "base/PreferenceManager.h"
#include "mcp/McpToolFixture.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include "kd/invoke.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("MaterialTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  const auto& editorContext = document.map().editorContext();

  const auto alignmentLock = pref(Preferences::AlignmentLock);
  const auto uvLock = pref(Preferences::UvLock);
  const auto restorePreferences = kdl::invoke_later{[&]() {
    setPref(Preferences::AlignmentLock, alignmentLock);
    setPref(Preferences::UvLock, uvLock);
  }};

  SECTION("locks_get")
  {
    CHECK(
      fixture.call("locks_get")
      == Json{{"alignmentLock", alignmentLock}, {"uvLock", uvLock}});

    setPref(Preferences::UvLock, !uvLock);
    CHECK(fixture.call("locks_get")["uvLock"] == !uvLock);
  }

  SECTION("locks_set")
  {
    SECTION("alignment lock")
    {
      const auto result =
        fixture.call("locks_set", Json{{"alignmentLock", !alignmentLock}});
      CHECK(result["undoStep"].is_null());
      CHECK(result["result"]["alignmentLock"] == !alignmentLock);
      CHECK(result["result"]["uvLock"] == uvLock);
      CHECK(result["result"]["previous"]["alignmentLock"] == alignmentLock);
      CHECK(pref(Preferences::AlignmentLock) == !alignmentLock);
      CHECK(pref(Preferences::UvLock) == uvLock);
      // the document applies the preference to its editor context
      CHECK(editorContext.alignmentLock() == !alignmentLock);
    }

    SECTION("both")
    {
      fixture.call("locks_set", Json{{"alignmentLock", false}, {"uvLock", true}});
      CHECK_FALSE(pref(Preferences::AlignmentLock));
      CHECK(pref(Preferences::UvLock));
      CHECK_FALSE(editorContext.alignmentLock());
      CHECK(editorContext.uvLock());
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "locks_set", Json{{"alignmentLock", !alignmentLock}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["alignmentLock"] == !alignmentLock);
      CHECK(pref(Preferences::AlignmentLock) == alignmentLock);
      CHECK(editorContext.alignmentLock() == alignmentLock);
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("locks_set").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("locks_set", Json{{"uvLock", "yes"}}).code
        == ErrorCode::InvalidArgument);
    }
  }
}

} // namespace tb::mcp
