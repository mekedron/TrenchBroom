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

#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

// Tests the fixes to mdl::Map that the MCP server relies on.
TEST_CASE("UpstreamMap")
{
  SECTION("canRedoCommand")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create();

    CHECK(!map.canRedoCommand());

    auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});

    REQUIRE(map.canUndoCommand());
    CHECK(!map.canRedoCommand());

    map.undoCommand();
    CHECK(!map.canUndoCommand());
    CHECK(map.canRedoCommand());
    CHECK(map.redoCommandName() != nullptr);
  }
}

} // namespace tb::mcp
