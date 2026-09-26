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
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::BrushNode* addBox(mdl::Map& map, const vm::bbox3d& bounds)
{
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, "stone")
                 .value();
  auto* brushNode = new mdl::BrushNode{std::move(brush)};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  mdl::deselectAll(map);
  return brushNode;
}

size_t brushCount(const mdl::Node& node)
{
  auto count = size_t(dynamic_cast<const mdl::BrushNode*>(&node) ? 1 : 0);
  for (const auto* child : node.children())
  {
    count += brushCount(*child);
  }
  return count;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

} // namespace

TEST_CASE("Scenario S7")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  const auto roomCenter = vm::vec3d{512, 512, 0};

  SECTION("12 columns in a circle facing the room center")
  {
    // a column that is thin radially (16) and wide tangentially (48), off the room center
    const auto columnBounds = vm::bbox3d{{696, 488, 0}, {712, 536, 128}};
    auto* column = addBox(map, columnBounds);
    fixture.call("selection_set", Json{{"ids", {fixture.id(*column)}}});

    const auto array = fixture.call(
      "objects_array",
      Json{
        {"pattern", "circle"},
        {"count", 12},
        {"center", {512, 512, 0}},
        {"radius", 384},
      });
    CHECK(array["undoStep"] == "AI: Array Objects");
    CHECK(hasWarning(array, "NON_INTEGER_VERTICES"));

    const auto& result = array["result"];
    REQUIRE(result["instances"].size() == 12);
    CHECK(result["created"].size() == 11);
    CHECK(brushCount(map.worldNode()) == 12);

    // evenly spaced at distance 384, starting at the original's direction (+x)
    for (size_t i = 0; i < 12; ++i)
    {
      auto* node = fixture.node(result["instances"][i][0].get<std::string>());
      REQUIRE(node);
      const auto center = node->logicalBounds().center() - roomCenter;
      CHECK(
        vm::length(vm::vec2d{center.x(), center.y()}) == Catch::Approx(384).margin(0.01));
      const auto expectedAngle = vm::to_radians(30.0 * double(i));
      CHECK(center.x() == Catch::Approx(384 * std::cos(expectedAngle)).margin(0.01));
      CHECK(center.y() == Catch::Approx(384 * std::sin(expectedAngle)).margin(0.01));
      CHECK(center.z() == Catch::Approx(64).margin(0.01));
    }

    // the original was moved to radius 384
    CHECK(vm::is_equal(column->logicalBounds().min, vm::vec3d{888, 488, 0}, 0.001));

    // the copy at 90 degrees is rotated to face the center: thin along y
    auto* quarter = fixture.node(result["instances"][3][0].get<std::string>());
    REQUIRE(quarter);
    CHECK(vm::is_equal(quarter->logicalBounds().size(), vm::vec3d{48, 16, 128}, 0.01));

    // the whole ring is selected, e.g. to group it
    CHECK(map.selection().nodes.size() == 12);

    // one undo step restores the single column
    fixture.call("undo");
    CHECK(brushCount(map.worldNode()) == 1);
    CHECK(vm::is_equal(column->logicalBounds().min, columnBounds.min, 0.001));
    CHECK(vm::is_equal(column->logicalBounds().max, columnBounds.max, 0.001));
  }

  SECTION("a spiral staircase with 20 steps")
  {
    // a step reaching 128 units out from the center of the stairs
    auto* step = addBox(map, {{512, 496, 0}, {640, 528, 16}});
    const auto stepId = fixture.id(*step);

    const auto array = fixture.call(
      "objects_array",
      Json{
        {"ids", {stepId}},
        {"pattern", "circle"},
        {"count", 20},
        {"center", {512, 512, 0}},
        {"angleStep", 18},
        {"rise", 16},
      });
    CHECK(array["undoStep"] == "AI: Array Objects");

    const auto& result = array["result"];
    REQUIRE(result["instances"].size() == 20);
    CHECK(brushCount(map.worldNode()) == 20);

    for (size_t i = 0; i < 20; ++i)
    {
      auto* node = fixture.node(result["instances"][i][0].get<std::string>());
      REQUIRE(node);
      const auto& bounds = node->logicalBounds();
      CHECK(bounds.min.z() == Catch::Approx(16.0 * double(i)).margin(0.001));
      CHECK(bounds.max.z() == Catch::Approx(16.0 * double(i) + 16).margin(0.001));
    }

    // the step at 180 degrees points towards -x
    auto* half = fixture.node(result["instances"][10][0].get<std::string>());
    REQUIRE(half);
    CHECK(vm::is_equal(half->logicalBounds().min, vm::vec3d{384, 496, 160}, 0.001));
    CHECK(vm::is_equal(half->logicalBounds().max, vm::vec3d{512, 528, 176}, 0.001));

    fixture.call("undo");
    CHECK(brushCount(map.worldNode()) == 1);
  }
}

} // namespace tb::mcp
