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
#include "mdl/Node.h"

#include "vm/bbox.h"

#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

// Tests the additions to mdl::Node that the MCP server relies on.
TEST_CASE("UpstreamNode")
{
  SECTION("runtimeId")
  {
    const auto worldBounds = vm::bbox3d{8192.0};

    auto nodeA = std::make_unique<mdl::EntityNode>(mdl::Entity{});
    auto nodeB = std::make_unique<mdl::EntityNode>(mdl::Entity{});

    CHECK(nodeA->runtimeId() != 0u);
    CHECK(nodeA->runtimeId() != nodeB->runtimeId());

    auto clone = std::unique_ptr<mdl::Node>{nodeA->clone(worldBounds)};
    CHECK(clone->runtimeId() != nodeA->runtimeId());
    CHECK(clone->runtimeId() != nodeB->runtimeId());

    const auto idA = nodeA->runtimeId();
    nodeA.reset();

    auto nodeC = std::make_unique<mdl::EntityNode>(mdl::Entity{});
    CHECK(nodeC->runtimeId() != idA);
  }
}

} // namespace tb::mcp
