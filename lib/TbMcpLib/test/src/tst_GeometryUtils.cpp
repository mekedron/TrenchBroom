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

#include "mcp/tools/GeometryUtils.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/bbox_io.h"
#include "vm/ray.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::Brush cuboid(const mdl::Map& map, const vm::bbox3d& box)
{
  return brushBuilder(map).createCuboid(box, "material").value();
}

mdl::BrushNode* addCuboid(mdl::Map& map, const vm::bbox3d& box)
{
  const auto added = addBrushes(map, {cuboid(map, box)});
  REQUIRE(added.size() == 1);
  return static_cast<mdl::BrushNode*>(added.front());
}

mdl::EntityNode* entityNode(std::vector<mdl::EntityProperty> properties)
{
  return new mdl::EntityNode{mdl::Entity{std::move(properties)}};
}

} // namespace

TEST_CASE("GeometryUtils")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();

  SECTION("intersectsInterior")
  {
    const auto cube = cuboid(map, {{0, 0, 0}, {64, 64, 64}});
    CHECK(intersectsInterior(cube, {{32, 32, 32}, {96, 96, 96}}));
    CHECK(intersectsInterior(cube, {{-16, -16, -16}, {80, 80, 80}}));
    // touching surfaces do not count
    CHECK_FALSE(intersectsInterior(cube, {{64, 0, 0}, {128, 64, 64}}));
    CHECK_FALSE(intersectsInterior(cube, {{128, 128, 128}, {192, 192, 192}}));
    // flat boxes
    CHECK(intersectsInterior(cube, {{16, 16, 32}, {48, 48, 32}}));
    CHECK_FALSE(intersectsInterior(cube, {{16, 16, 64}, {48, 48, 64}}));

    // the box overlaps the bounds of the tetrahedron, but not the tetrahedron
    const auto tetrahedron =
      brushBuilder(map)
        .createBrush(
          std::vector<vm::vec3d>{{0, 0, 0}, {64, 0, 0}, {0, 64, 0}, {0, 0, 64}},
          "material")
        .value();
    CHECK(intersectsInterior(tetrahedron, {{1, 1, 1}, {10, 10, 10}}));
    CHECK_FALSE(intersectsInterior(tetrahedron, {{40, 40, 40}, {60, 60, 60}}));
  }

  SECTION("owningBrushEntity")
  {
    auto* worldBrush = addCuboid(map, {{0, 0, 0}, {64, 64, 64}});
    CHECK(owningBrushEntity(*worldBrush) == nullptr);

    auto* door = entityNode({{"classname", "func_door"}});
    auto* doorBrush = new mdl::BrushNode{cuboid(map, {{0, 0, 0}, {64, 64, 64}})};
    door->addChild(doorBrush);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {door}}});
    CHECK(owningBrushEntity(*doorBrush) == door);
    CHECK(owningBrushEntity(*door) == nullptr);
  }

  SECTION("classifyBrush")
  {
    auto* worldBrush = addCuboid(map, {{0, 0, 0}, {64, 64, 64}});
    CHECK(classifyBrush(*worldBrush) == BrushClass::Solid);

    const auto classify = [&](const std::string& classname) {
      auto* entity = entityNode({{"classname", classname}});
      auto* brushNode = new mdl::BrushNode{cuboid(map, {{0, 0, 0}, {64, 64, 64}})};
      entity->addChild(brushNode);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {entity}}});
      return classifyBrush(*brushNode);
    };

    CHECK(classify("func_group") == BrushClass::Solid);
    CHECK(classify("func_detail") == BrushClass::Solid);
    CHECK(classify("func_detail_wall") == BrushClass::Solid);
    CHECK(classify("func_door") == BrushClass::Entity);
    CHECK(classify("trigger_once") == BrushClass::Trigger);
  }

  SECTION("isPointEntity")
  {
    auto* point = entityNode({{"classname", "info_player_start"}});
    auto* brushEntity = entityNode({{"classname", "func_door"}});
    auto* brushNode = new mdl::BrushNode{cuboid(map, {{0, 0, 0}, {64, 64, 64}})};
    brushEntity->addChild(brushNode);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {point, brushEntity}}});

    CHECK(isPointEntity(*point));
    CHECK_FALSE(isPointEntity(*brushEntity));
    CHECK_FALSE(isPointEntity(*brushNode));
    CHECK_FALSE(isPointEntity(map.worldNode()));
  }

  SECTION("castRay")
  {
    auto* near = addCuboid(map, {{64, 0, 0}, {128, 64, 64}});
    auto* far = addCuboid(map, {{256, 0, 0}, {320, 64, 64}});
    auto* point =
      entityNode({{"classname", "info_player_start"}, {"origin", "512 32 32"}});
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {point}}});

    const auto acceptAll = [](const mdl::Node&) { return true; };
    const auto ray = vm::ray3d{{0, 32, 32}, {1, 0, 0}};

    SECTION("hits sorted by distance")
    {
      const auto hits = castRay(map, ray, acceptAll);
      REQUIRE(hits.size() == 3);

      CHECK(hits[0].node == near);
      CHECK(hits[0].distance == vm::approx{64.0});
      CHECK(hits[0].point == vm::approx{vm::vec3d{64, 32, 32}});
      REQUIRE(hits[0].faceIndex);
      CHECK(
        near->brush().face(*hits[0].faceIndex).normal()
        == vm::approx{vm::vec3d{-1, 0, 0}});

      CHECK(hits[1].node == far);
      CHECK(hits[1].distance == vm::approx{256.0});

      CHECK(hits[2].node == point);
      CHECK(hits[2].distance < 512.0);
      CHECK_FALSE(hits[2].faceIndex);
    }

    SECTION("max distance")
    {
      const auto hits = castRay(map, ray, acceptAll, 200.0);
      REQUIRE(hits.size() == 1);
      CHECK(hits[0].node == near);
    }

    SECTION("predicate")
    {
      const auto hits =
        castRay(map, ray, [&](const mdl::Node& node) { return &node != near; });
      REQUIRE(hits.size() == 2);
      CHECK(hits[0].node == far);
      CHECK(hits[1].node == point);
    }

    SECTION("ray starting inside a brush does not hit it")
    {
      const auto hits = castRay(map, vm::ray3d{{96, 32, 32}, {1, 0, 0}}, acceptAll);
      REQUIRE(hits.size() == 2);
      CHECK(hits[0].node == far);
      CHECK(hits[0].distance == vm::approx{160.0});
    }

    SECTION("ray missing everything")
    {
      CHECK(castRay(map, vm::ray3d{{0, 32, 32}, {-1, 0, 0}}, acceptAll).empty());
    }
  }

  SECTION("checkBox")
  {
    CHECK_FALSE(checkBox(map, {{0, 0, 0}, {64, 64, 64}}));

    const auto degenerate = checkBox(map, {{0, 0, 0}, {64, 0, 64}}, "The opening");
    REQUIRE(degenerate);
    CHECK(degenerate->code == ErrorCode::InvalidGeometry);
    CHECK(degenerate->message.starts_with("The opening is degenerate"));

    const auto outside = checkBox(map, {{0, 0, 0}, {64, 64, 99999}});
    REQUIRE(outside);
    CHECK(outside->code == ErrorCode::OutOfWorldBounds);
  }

  SECTION("geometryError")
  {
    const auto invalid =
      geometryError("The brush could not be built.", "Brush is empty", {"brush:1"}, "");
    CHECK(invalid.code == ErrorCode::InvalidGeometry);
    CHECK(invalid.message == "The brush could not be built. Editor said: Brush is empty");
    CHECK(invalid.objectIds == std::vector<std::string>{"brush:1"});
    CHECK(invalid.details.at("editorMessages") == Json::array({"Brush is empty"}));

    const auto outside = geometryError(
      "The brush could not be moved.", "Brush exceeds World Bounds", {}, "");
    CHECK(outside.code == ErrorCode::OutOfWorldBounds);
  }

  SECTION("addBrushes")
  {
    SECTION("to the parent for new nodes")
    {
      const auto added = addBrushes(
        map,
        {cuboid(map, {{0, 0, 0}, {64, 64, 64}}),
         cuboid(map, {{64, 0, 0}, {128, 64, 64}})});
      REQUIRE(added.size() == 2);
      for (const auto* node : added)
      {
        CHECK(dynamic_cast<const mdl::BrushNode*>(node));
        CHECK(node->parent() == &mdl::parentForNodes(map));
      }
      CHECK(added[1]->logicalBounds() == vm::bbox3d{{64, 0, 0}, {128, 64, 64}});
    }

    SECTION("to the given parent")
    {
      auto* group = new mdl::GroupNode{mdl::Group{"group"}};
      group->addChild(new mdl::BrushNode{cuboid(map, {{0, 0, 0}, {64, 64, 64}})});
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {group}}});

      const auto added =
        addBrushes(map, {cuboid(map, {{64, 0, 0}, {128, 64, 64}})}, group);
      REQUIRE(added.size() == 1);
      CHECK(added.front()->parent() == group);
      CHECK(group->childCount() == 2);
    }

    SECTION("no brushes")
    {
      CHECK(addBrushes(map, {}).empty());
    }
  }

  SECTION("ScopedLockOverride")
  {
    auto& editorContext = map.editorContext();
    editorContext.setAlignmentLock(true);
    editorContext.setUvLock(false);

    SECTION("overrides and restores both locks")
    {
      {
        const auto lockOverride = ScopedLockOverride{map, false, true};
        CHECK_FALSE(editorContext.alignmentLock());
        CHECK(editorContext.uvLock());
      }
      CHECK(editorContext.alignmentLock());
      CHECK_FALSE(editorContext.uvLock());
    }

    SECTION("absent values keep the current settings")
    {
      {
        const auto lockOverride = ScopedLockOverride{map, std::nullopt};
        CHECK(editorContext.alignmentLock());
        CHECK_FALSE(editorContext.uvLock());

        // changes made inside the scope are reverted as well
        editorContext.setUvLock(true);
      }
      CHECK(editorContext.alignmentLock());
      CHECK_FALSE(editorContext.uvLock());
    }
  }
}

} // namespace tb::mcp
