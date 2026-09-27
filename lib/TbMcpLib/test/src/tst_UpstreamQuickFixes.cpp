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

#include "base/Color.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

// Tests the fixes to the editor's quick fixes and the selection they make that
// issue_fix relies on.
TEST_CASE("UpstreamQuickFixes")
{
  SECTION("selecting the world selects nothing")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create();

    // the world cannot be selected; it used to be listed in the selection anyway and
    // stayed there, so deleting the selection later failed
    mdl::selectNodes(map, {&map.worldNode()});
    CHECK(!map.worldNode().selected());
    CHECK(map.selection().nodes.empty());

    // the property quick fixes select the issue's node, here worldspawn
    mdl::setEntityProperty(map, "wad", "gfx\\base.wad");
    const auto validators = map.worldNode().registeredValidators();
    const auto validator = std::ranges::find_if(validators, [](const auto* v) {
      return v->description() == "Paths must use forward slashes";
    });
    REQUIRE(validator != validators.end());
    auto issues = map.worldNode().issues(validators);
    std::erase_if(
      issues, [&](const auto* issue) { return issue->type() != (*validator)->type(); });
    REQUIRE(issues.size() == 1);

    (*validator)->quickFixes().front()->apply(map, issues);
    CHECK(*map.worldNode().entity().property("wad") == "gfx/base.wad");
    CHECK(map.selection().nodes.empty());
  }

  SECTION("Move Brushes to World does not select the removed entity")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create();
    map.entityDefinitionManager().setDefinitions({
      {"point_entity",
       Color{},
       "a point entity",
       {},
       mdl::PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
    });

    auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "point_entity"}}}};
    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
    mdl::addNodes(map, {{entityNode, {brushNode}}});

    const auto validators = map.worldNode().registeredValidators();
    const auto validator = std::ranges::find_if(validators, [](const auto* v) {
      return v->description() == "Point entity with brushes";
    });
    REQUIRE(validator != validators.end());

    auto issues = entityNode->issues(validators);
    std::erase_if(
      issues, [&](const auto* issue) { return issue->type() != (*validator)->type(); });
    REQUIRE(issues.size() == 1);
    REQUIRE((*validator)->quickFixes().size() == 1);

    // the quick fix reparents the brush, which removes the empty entity; it used to
    // select the removed entity, which violated a precondition
    (*validator)->quickFixes().front()->apply(map, issues);

    CHECK(entityNode->parent() == nullptr);
    CHECK(brushNode->parent() == &mdl::parentForNodes(map));
    CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushNode});
  }
}

} // namespace tb::mcp
