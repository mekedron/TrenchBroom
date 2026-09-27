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

#include "mcp/tools/CsgUtils.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EditorContext.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::BrushNode* addBrush(mdl::Map& map, const vm::bbox3d& bounds)
{
  const auto builder = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto* brushNode =
    new mdl::BrushNode{builder.createCuboid(bounds, "material") | kdl::value()};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  return brushNode;
}

} // namespace

TEST_CASE("CsgUtils")
{
  SECTION("csgHollow")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create();

    const auto bounds = vm::bbox3d{{-112, -16, 0}, {-48, 64, 48}};
    auto* brushNode = addBrush(map, bounds);
    auto* layer = map.editorContext().currentLayer();
    REQUIRE(layer->childCount() == 1);

    const auto undoCommandsBefore = map.commandProcessor().undoCommandNames();

    SECTION("Walls have the given thickness")
    {
      mdl::selectNodes(map, {brushNode});
      CHECK(csgHollow(map, 4.0));
      CHECK(layer->childCount() == 6);
      CHECK(*map.undoCommandName() == "CSG Hollow");

      const auto brushes = map.selection().brushes;
      REQUIRE(brushes.size() == 6);
      for (const auto* wallNode : brushes)
      {
        const auto size = wallNode->logicalBounds().size();
        CHECK(vm::get_max_component(size, 2) == 4.0);
        CHECK(bounds.contains(wallNode->logicalBounds()));
      }
    }

    SECTION("A thickness that is too large doesn't hollow the brush")
    {
      mdl::selectNodes(map, {brushNode});
      const auto selectCommands = map.commandProcessor().undoCommandNames();

      CHECK(!csgHollow(map, vm::get_max_component(bounds.size())));
      CHECK(layer->childCount() == 1);
      CHECK(map.commandProcessor().undoCommandNames() == selectCommands);
    }

    SECTION("A non-positive thickness is rejected")
    {
      mdl::selectNodes(map, {brushNode});
      const auto selectCommands = map.commandProcessor().undoCommandNames();

      CHECK(!csgHollow(map, 0.0));
      CHECK(!csgHollow(map, -4.0));
      CHECK(layer->childCount() == 1);
      CHECK(map.commandProcessor().undoCommandNames() == selectCommands);
    }

    SECTION("Without selected brushes, nothing is hollowed")
    {
      CHECK(!csgHollow(map, 4.0));
      CHECK(map.commandProcessor().undoCommandNames() == undoCommandsBefore);
    }
  }
}

} // namespace tb::mcp
