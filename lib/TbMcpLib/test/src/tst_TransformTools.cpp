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

#include "mcp/JsonVm.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

mdl::BrushNode* addBox(mdl::Map& map, const vm::bbox3d& bounds)
{
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, "stone")
                 .value();
  auto* brushNode = new mdl::BrushNode{std::move(brush)};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  return brushNode;
}

mdl::EntityNode* addPlayerStart(mdl::Map& map)
{
  auto* entityNode = new mdl::EntityNode{mdl::Entity{{
    {"classname", "info_player_start"},
    {"origin", "256 0 32"},
    {"angle", "0"},
  }}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

bool boundsEqual(const vm::bbox3d& actual, const vm::bbox3d& expected)
{
  return vm::is_equal(actual.min, expected.min, 0.001)
         && vm::is_equal(actual.max, expected.max, 0.001);
}

/** Whether two JSON vectors are equal within 0.001. */
bool vecEqual(const Json& actual, const Json& expected)
{
  return vm::is_equal(*vec3FromJson(actual), *vec3FromJson(expected), 0.001);
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

std::string angleOf(const mdl::EntityNode& entityNode)
{
  const auto* value = entityNode.entity().property("angle");
  return value ? *value : "";
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

size_t brushCount(const mdl::Map& map)
{
  return brushCount(map.worldNode());
}

} // namespace

TEST_CASE("TransformTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  const auto boundsA = vm::bbox3d{{0, 0, 0}, {64, 32, 16}};
  const auto boundsB = vm::bbox3d{{128, 0, 0}, {192, 64, 64}};
  auto* brushA = addBox(map, boundsA);
  auto* brushB = addBox(map, boundsB);
  auto* playerStart = addPlayerStart(map);
  mdl::deselectAll(map);

  const auto idA = fixture.id(*brushA);
  const auto idB = fixture.id(*brushB);
  const auto idStart = fixture.id(*playerStart);
  const auto& selection = map.selection();

  SECTION("objects_move")
  {
    SECTION("explicit ids")
    {
      const auto moved =
        fixture.call("objects_move", Json{{"ids", {idA}}, {"vector", {16, 0, 0}}});
      CHECK(moved["undoStep"] == "AI: Move Objects");
      REQUIRE(resultOf(moved)["objects"].size() == 1);
      CHECK(resultOf(moved)["objects"][0]["id"] == idA);
      CHECK(
        resultOf(moved)["bounds"] == Json{{"min", {16, 0, 0}}, {"max", {80, 32, 16}}});
      CHECK(moved["changes"]["modified"] == Json{idA});
      CHECK(boundsEqual(brushA->logicalBounds(), {{16, 0, 0}, {80, 32, 16}}));
      CHECK(boundsEqual(brushB->logicalBounds(), boundsB));
      // the selection is restored
      CHECK_FALSE(selection.hasAny());

      fixture.call("undo");
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushB});
      fixture.call("objects_move", Json{{"vector", {0, 0, 32}}});
      CHECK(boundsEqual(brushB->logicalBounds(), {{128, 0, 32}, {192, 64, 96}}));
      CHECK(selection.nodes == std::vector<mdl::Node*>{brushB});
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "objects_move", Json{{"ids", {idA}}, {"vector", {16, 0, 0}}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(dryRun["changes"]["modified"] == Json{idA});
      CHECK(resultOf(dryRun)["bounds"]["min"] == Json{16, 0, 0});
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }

    SECTION("alignment lock override is restored")
    {
      const auto alignmentLock = map.editorContext().alignmentLock();
      fixture.call(
        "objects_move",
        Json{{"ids", {idA}}, {"vector", {16, 0, 0}}, {"alignmentLock", !alignmentLock}});
      CHECK(map.editorContext().alignmentLock() == alignmentLock);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError("objects_move", Json{{"ids", {idA}}, {"vector", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_move", Json{{"vector", {16, 0, 0}}}).code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "objects_move", Json{{"ids", {"brush:999999"}}, {"vector", {16, 0, 0}}})
          .code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("objects_move", Json{{"ids", {idA}}}).code
        == ErrorCode::InvalidArgument);

      const auto outside = fixture.callExpectingError(
        "objects_move", Json{{"ids", {idA}}, {"vector", {20000, 0, 0}}});
      CHECK(outside.code == ErrorCode::OutOfWorldBounds);
      CHECK(outside.objectIds == std::vector<std::string>{idA});
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }
  }

  SECTION("objects_rotate")
  {
    SECTION("brush around its center")
    {
      const auto rotated =
        fixture.call("objects_rotate", Json{{"ids", {idA}}, {"angle", 90}});
      CHECK(rotated["undoStep"] == "AI: Rotate Objects");
      CHECK(boundsEqual(brushA->logicalBounds(), {{16, -16, 0}, {48, 48, 16}}));
      CHECK_FALSE(hasWarning(rotated, "NON_INTEGER_VERTICES"));
    }

    SECTION("around a center and an axis vector")
    {
      fixture.call(
        "objects_rotate",
        Json{{"ids", {idA}}, {"angle", 90}, {"axis", {0, 0, 1}}, {"center", {0, 0, 0}}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{-32, 0, 0}, {0, 64, 16}}));

      fixture.call(
        "objects_rotate",
        Json{{"ids", {idA}}, {"angle", -90}, {"axis", "z"}, {"center", {0, 0, 0}}});
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushA});
      fixture.call("objects_rotate", Json{{"angle", 90}, {"axis", "x"}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{0, 8, -8}, {64, 24, 24}}));
      CHECK(selection.nodes == std::vector<mdl::Node*>{brushA});
    }

    SECTION("entity angles")
    {
      const auto& config = map.worldNode().entityPropertyConfig();
      REQUIRE(config.updateAnglePropertyAfterTransform);

      fixture.call("objects_rotate", Json{{"ids", {idStart}}, {"angle", 90}});
      CHECK(angleOf(*playerStart) == "90");

      fixture.call(
        "objects_rotate",
        Json{{"ids", {idStart}}, {"angle", 90}, {"updateEntityAngles", false}});
      CHECK(angleOf(*playerStart) == "90");
      CHECK(config.updateAnglePropertyAfterTransform);
    }

    SECTION("non-integer vertices")
    {
      const auto rotated =
        fixture.call("objects_rotate", Json{{"ids", {idA}}, {"angle", 45}});
      CHECK(hasWarning(rotated, "NON_INTEGER_VERTICES"));
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "objects_rotate", Json{{"ids", {idA, idStart}}, {"angle", 90}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
      CHECK(angleOf(*playerStart) == "0");
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("objects_rotate", Json{{"ids", {idA}}, {"angle", 0}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_rotate", Json{{"ids", {idA}}, {"angle", 360}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_rotate", Json{{"ids", {idA}}, {"angle", 90}, {"axis", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_rotate", Json{{"ids", {idA}}, {"angle", 90}, {"axis", "w"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_rotate", Json{{"angle", 90}}).code
        == ErrorCode::NoSelection);
    }
  }

  SECTION("objects_scale")
  {
    SECTION("factors")
    {
      const auto scaled = fixture.call(
        "objects_scale", Json{{"ids", {idA}}, {"factors", {2, 1, 1}}, {"anchor", "min"}});
      CHECK(scaled["undoStep"] == "AI: Scale Objects");
      CHECK(boundsEqual(brushA->logicalBounds(), {{0, 0, 0}, {128, 32, 16}}));

      fixture.call("objects_scale", Json{{"ids", {idA}}, {"factors", {0.5, 1, 1}}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{32, 0, 0}, {96, 32, 16}}));

      fixture.call(
        "objects_scale",
        Json{{"ids", {idA}}, {"factors", {1, 2, 1}}, {"anchor", {0, 0, 0}}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{32, 0, 0}, {96, 64, 16}}));

      fixture.call(
        "objects_scale", Json{{"ids", {idA}}, {"factors", {1, 1, 2}}, {"anchor", "max"}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{32, 0, -16}, {96, 64, 16}}));
    }

    SECTION("box")
    {
      mdl::selectNodes(map, {brushA});
      const auto box = Json{{"min", {0, 0, 0}}, {"max", {256, 128, 32}}};
      fixture.call("objects_scale", Json{{"box", box}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{0, 0, 0}, {256, 128, 32}}));
    }

    SECTION("dry run")
    {
      fixture.call(
        "objects_scale", Json{{"ids", {idA}}, {"factors", {2, 2, 2}}, {"dryRun", true}});
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }

    SECTION("invalid input")
    {
      const auto box = Json{{"min", {0, 0, 0}}, {"max", {256, 128, 32}}};
      CHECK(
        fixture.callExpectingError("objects_scale", Json{{"ids", {idA}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_scale", Json{{"ids", {idA}}, {"factors", {2, 2, 2}}, {"box", box}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_scale", Json{{"ids", {idA}}, {"factors", {0, 1, 1}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_scale", Json{{"ids", {idA}}, {"factors", {-1, 1, 1}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_scale", Json{{"ids", {idA}}, {"box", box}, {"anchor", "min"}})
          .code
        == ErrorCode::InvalidArgument);

      const auto flatBox = Json{{"min", {0, 0, 0}}, {"max", {256, 128, 0}}};
      CHECK(
        fixture
          .callExpectingError("objects_scale", Json{{"ids", {idA}}, {"box", flatBox}})
          .code
        == ErrorCode::InvalidGeometry);
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }
  }

  SECTION("objects_shear")
  {
    const auto sheared = fixture.call(
      "objects_shear", Json{{"ids", {idB}}, {"side", "+z"}, {"offset", {32, 0, 0}}});
    CHECK(sheared["undoStep"] == "AI: Shear Objects");
    CHECK(boundsEqual(brushB->logicalBounds(), {{128, 0, 0}, {224, 64, 64}}));

    SECTION("dry run")
    {
      fixture.call(
        "objects_shear",
        Json{{"ids", {idA}}, {"side", "+y"}, {"offset", {16, 0, 0}}, {"dryRun", true}});
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushA});
      fixture.call("objects_shear", Json{{"side", "-x"}, {"offset", {0, 0, 16}}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{0, 0, 0}, {64, 32, 32}}));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "objects_shear", Json{{"ids", {idA}}, {"side", "+z"}, {"offset", {0, 0, 8}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_shear", Json{{"ids", {idA}}, {"side", "+z"}, {"offset", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_shear", Json{{"ids", {idA}}, {"side", "z"}, {"offset", {8, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_flip")
  {
    SECTION("around the bounds center")
    {
      const auto flipped =
        fixture.call("objects_flip", Json{{"ids", {idA, idStart}}, {"axis", "x"}});
      CHECK(flipped["undoStep"] == "AI: Flip Objects");
      CHECK(angleOf(*playerStart) == "180");
    }

    SECTION("around a center")
    {
      mdl::selectNodes(map, {brushA});
      fixture.call("objects_flip", Json{{"axis", "x"}, {"center", {0, 0, 0}}});
      CHECK(boundsEqual(brushA->logicalBounds(), {{-64, 0, 0}, {0, 32, 16}}));
    }

    SECTION("dry run")
    {
      fixture.call(
        "objects_flip", Json{{"ids", {idStart}}, {"axis", "x"}, {"dryRun", true}});
      CHECK(angleOf(*playerStart) == "0");
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("objects_flip", Json{{"ids", {idA}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("objects_flip", Json{{"ids", {idA}}, {"axis", "w"}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_duplicate")
  {
    SECTION("explicit ids with offset")
    {
      const auto duplicated = fixture.call(
        "objects_duplicate", Json{{"ids", {idA, idStart}}, {"offset", {0, 64, 0}}});
      CHECK(duplicated["undoStep"] == "AI: Duplicate Objects");
      const auto& copies = resultOf(duplicated)["copies"];
      REQUIRE(copies.size() == 2);
      CHECK(copies[0]["original"] == idA);
      CHECK(copies[1]["original"] == idStart);
      CHECK(copies[0]["copy"] != idA);
      CHECK(duplicated["changes"]["created"].size() == 2);

      auto* copyA = fixture.node(copies[0]["copy"].get<std::string>());
      REQUIRE(copyA);
      CHECK(boundsEqual(copyA->logicalBounds(), {{0, 64, 0}, {64, 96, 16}}));
      CHECK(boundsEqual(brushA->logicalBounds(), boundsA));
      // the copies are selected
      CHECK(selection.nodes.size() == 2);
      CHECK(copyA->selected());

      fixture.call("undo");
      CHECK(brushCount(map) == 2);
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushB});
      const auto duplicated = fixture.call("objects_duplicate");
      REQUIRE(resultOf(duplicated)["copies"].size() == 1);
      CHECK(brushCount(map) == 3);
    }

    SECTION("dry run")
    {
      const auto dryRun =
        fixture.call("objects_duplicate", Json{{"ids", {idA}}, {"dryRun", true}});
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(brushCount(map) == 2);
      CHECK_FALSE(selection.hasAny());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("objects_duplicate").code == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "objects_duplicate", Json{{"ids", {idA}}, {"offset", {20000, 0, 0}}})
          .code
        == ErrorCode::OutOfWorldBounds);
      CHECK(brushCount(map) == 2);
    }
  }

  SECTION("objects_delete")
  {
    SECTION("explicit ids")
    {
      const auto deleted = fixture.call("objects_delete", Json{{"ids", {idA, idStart}}});
      CHECK(deleted["undoStep"] == "AI: Delete Objects");
      CHECK(resultOf(deleted)["removed"] == Json{idA, idStart});
      CHECK(resultOf(deleted)["count"] == 2);
      CHECK(deleted["changes"]["removed"].size() == 2);
      CHECK(brushCount(map) == 1);
      CHECK(
        fixture.callExpectingError("object_get", Json{{"ids", {idA}}}).code
        == ErrorCode::ObjectNotFound);

      fixture.call("undo");
      CHECK(brushCount(map) == 2);
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {brushB});
      fixture.call("objects_delete");
      CHECK(brushCount(map) == 1);
      CHECK_FALSE(selection.hasAny());
    }

    SECTION("dry run")
    {
      const auto dryRun =
        fixture.call("objects_delete", Json{{"ids", {idA}}, {"dryRun", true}});
      CHECK(dryRun["changes"]["removed"] == Json{idA});
      CHECK(brushCount(map) == 2);
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("objects_delete").code == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "objects_delete",
            Json{{"ids", {fixture.id(*map.worldNode().defaultLayer())}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_array")
  {
    SECTION("line")
    {
      const auto array = fixture.call(
        "objects_array",
        Json{{"ids", {idA}}, {"pattern", "line"}, {"count", 4}, {"offset", {0, 64, 0}}});
      CHECK(array["undoStep"] == "AI: Array Objects");
      const auto& result = resultOf(array);
      CHECK(result["count"] == 4);
      REQUIRE(result["instances"].size() == 4);
      CHECK(result["instances"][0] == Json{idA});
      CHECK(result["created"].size() == 3);
      CHECK(brushCount(map) == 5);

      auto* last = fixture.node(result["instances"][3][0].get<std::string>());
      REQUIRE(last);
      CHECK(boundsEqual(last->logicalBounds(), {{0, 192, 0}, {64, 224, 16}}));
      // the whole array is selected
      CHECK(selection.nodes.size() == 4);

      // one undo step
      fixture.call("undo");
      CHECK(brushCount(map) == 2);
    }

    SECTION("grid")
    {
      mdl::selectNodes(map, {brushA});
      const auto array = fixture.call(
        "objects_array",
        Json{{"pattern", "grid"}, {"counts", {2, 3, 1}}, {"spacing", {-128, -64, 0}}});
      const auto& result = resultOf(array);
      CHECK(result["count"] == 6);
      REQUIRE(result["instances"].size() == 6);
      auto* last = fixture.node(result["instances"][5][0].get<std::string>());
      REQUIRE(last);
      CHECK(boundsEqual(last->logicalBounds(), {{-128, -128, 0}, {-64, -96, 16}}));
      CHECK(brushCount(map) == 7);
    }

    SECTION("circle without rotation")
    {
      const auto array = fixture.call(
        "objects_array",
        Json{
          {"ids", {idStart}},
          {"pattern", "circle"},
          {"count", 4},
          {"center", {0, 0, 32}},
          {"rotate", false}});
      const auto& result = resultOf(array);
      REQUIRE(result["instances"].size() == 4);
      auto* second = static_cast<mdl::EntityNode*>(
        fixture.node(result["instances"][1][0].get<std::string>()));
      REQUIRE(second);
      CHECK(vm::is_equal(second->entity().origin(), vm::vec3d{0, 256, 32}, 0.001));
      CHECK(angleOf(*second) == "0");
    }

    SECTION("circle with rotation")
    {
      const auto array = fixture.call(
        "objects_array",
        Json{
          {"ids", {idStart}},
          {"pattern", "circle"},
          {"count", 4},
          {"center", {0, 0, 32}}});
      const auto& result = resultOf(array);
      auto* second = static_cast<mdl::EntityNode*>(
        fixture.node(result["instances"][1][0].get<std::string>()));
      REQUIRE(second);
      CHECK(vm::is_equal(second->entity().origin(), vm::vec3d{0, 256, 32}, 0.001));
      CHECK(angleOf(*second) == "90");
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "objects_array",
        Json{
          {"ids", {idA}},
          {"pattern", "line"},
          {"count", 3},
          {"offset", {0, 64, 0}},
          {"dryRun", true}});
      CHECK(resultOf(dryRun)["created"].size() == 2);
      CHECK(brushCount(map) == 2);
    }

    SECTION("invalid input")
    {
      const auto call = [&](Json arguments) {
        arguments["ids"] = Json{idA};
        return fixture.callExpectingError("objects_array", std::move(arguments)).code;
      };
      CHECK(
        call(Json{{"pattern", "line"}, {"count", 1}, {"offset", {0, 64, 0}}})
        == ErrorCode::InvalidArgument);
      CHECK(call(Json{{"pattern", "line"}, {"count", 3}}) == ErrorCode::InvalidArgument);
      CHECK(
        call(Json{{"pattern", "line"}, {"count", 3}, {"offset", {0, 0, 0}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        call(Json{{"pattern", "grid"}, {"counts", {1, 1, 1}}, {"spacing", {64, 64, 64}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        call(Json{{"pattern", "grid"}, {"counts", {2, 1, 1}}, {"spacing", {0, 64, 64}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        call(Json{{"pattern", "circle"}, {"count", 4}}) == ErrorCode::InvalidArgument);
      CHECK(
        call(Json{{"pattern", "line"}, {"count", 3}, {"offset", {8000, 0, 0}}})
        == ErrorCode::OutOfWorldBounds);
      CHECK(brushCount(map) == 2);
    }
  }

  SECTION("command_repeat")
  {
    SECTION("nothing to repeat")
    {
      const auto nothing = fixture.call("command_repeat");
      CHECK(resultOf(nothing)["times"] == 0);
      CHECK(hasWarning(nothing, "NOTHING_TO_REPEAT"));
      CHECK(nothing["undoStep"].is_null());
    }

    SECTION("duplicate and move")
    {
      fixture.call("objects_duplicate", Json{{"ids", {idA}}});
      fixture.call("objects_move", Json{{"vector", {0, 64, 0}}});
      CHECK(brushCount(map) == 3);

      const auto repeated = fixture.call("command_repeat", Json{{"times", 2}});
      CHECK(repeated["undoStep"] == "AI: Repeat Last Commands");
      CHECK(resultOf(repeated)["times"] == 2);
      CHECK(repeated["changes"]["created"].size() == 2);
      CHECK(brushCount(map) == 5);
      REQUIRE(selection.nodes.size() == 1);
      CHECK(
        boundsEqual(selection.nodes[0]->logicalBounds(), {{0, 192, 0}, {64, 224, 16}}));

      SECTION("dry run")
      {
        const auto dryRun = fixture.call("command_repeat", Json{{"dryRun", true}});
        CHECK(dryRun["dryRun"] == true);
        CHECK(resultOf(dryRun)["objects"].size() == 1);
        CHECK(brushCount(map) == 5);
      }

      SECTION("one undo step")
      {
        fixture.call("undo");
        CHECK(brushCount(map) == 3);
        // undo clears the repeat list
        CHECK(hasWarning(fixture.call("command_repeat"), "NOTHING_TO_REPEAT"));
      }

      SECTION("command_repeat_clear")
      {
        const auto dryRun = fixture.call("command_repeat_clear", Json{{"dryRun", true}});
        CHECK(resultOf(dryRun)["cleared"] == true);
        CHECK(map.canRepeatCommands());

        const auto cleared = fixture.call("command_repeat_clear");
        CHECK(resultOf(cleared)["cleared"] == true);
        CHECK_FALSE(map.canRepeatCommands());
        CHECK(hasWarning(fixture.call("command_repeat"), "NOTHING_TO_REPEAT"));
      }

      SECTION("not inside a transaction")
      {
        fixture.call("transaction_begin", Json{{"name", "Test"}});
        CHECK(
          fixture.callExpectingError("command_repeat").code
          == ErrorCode::TransactionActive);
        fixture.call("transaction_rollback");
      }
    }
  }
}

TEST_CASE("TransformTools with Valve 220 UVs")
{
  auto fixture = McpToolFixture{};
  auto& map = fixture.create({.mapFormat = mdl::MapFormat::Valve}).map();
  auto* brush = addBox(map, {{0, 0, 0}, {64, 64, 64}});
  const auto id = fixture.id(*brush);

  // face_attributes_get of the top face of a brush
  const auto topFace = [&](const std::string& brushId) {
    const auto result =
      fixture.call("face_attributes_get", Json{{"ids", {brushId}}, {"detail", "full"}});
    for (const auto& item : result["items"])
    {
      if (item["normal"] == Json{0, 0, 1})
      {
        return item;
      }
    }
    FAIL("no top face");
    return Json{};
  };

  const auto topId = topFace(id)["id"].get<std::string>();
  fixture.call("face_attributes_set", Json{{"ids", {topId}}, {"rotation", 30}});
  const auto original = topFace(id);
  CHECK(original["rotation"] == 30);

  SECTION("translated copies report the rotation of the original")
  {
    const auto array = fixture.call(
      "objects_array",
      Json{
        {"ids", {id}},
        {"pattern", "line"},
        {"count", 3},
        {"offset", {128, 0, 0}},
        {"alignmentLock", true},
      });
    for (const auto& instance : resultOf(array)["instances"])
    {
      const auto top = topFace(instance[0].get<std::string>());
      CHECK(top["rotation"] == 30);
      CHECK(vecEqual(top["uAxis"], original["uAxis"]));
      CHECK(vecEqual(top["vAxis"], original["vAxis"]));
    }

    fixture.call(
      "objects_move",
      Json{{"ids", {id}}, {"vector", {0, 64, 0}}, {"alignmentLock", true}});
    CHECK(topFace(id)["rotation"] == 30);
    CHECK(vecEqual(topFace(id)["uAxis"], original["uAxis"]));
  }

  SECTION("setting the reported rotation keeps the texture")
  {
    fixture.call(
      "objects_move",
      Json{{"ids", {id}}, {"vector", {0, 64, 0}}, {"alignmentLock", true}});
    const auto moved = topFace(id);
    fixture.call("face_attributes_set", Json{{"ids", {topId}}, {"rotation", 30}});
    CHECK(vecEqual(topFace(id)["uAxis"], moved["uAxis"]));

    fixture.call("face_attributes_set", Json{{"ids", {topId}}, {"rotation", 0}});
    CHECK(topFace(id)["rotation"] == 0);
    CHECK(vecEqual(topFace(id)["uAxis"], Json{1, 0, 0}));
  }

  SECTION("a rotated brush reports the rotated texture")
  {
    fixture.call(
      "objects_rotate",
      Json{{"ids", {id}}, {"angle", 90}, {"axis", "z"}, {"alignmentLock", true}});
    const auto rotated = topFace(id);
    CHECK(rotated["rotation"] != 30);
    CHECK_FALSE(vecEqual(rotated["uAxis"], original["uAxis"]));

    // setting the reported rotation changes nothing
    fixture.call(
      "face_attributes_set", Json{{"ids", {topId}}, {"rotation", rotated["rotation"]}});
    CHECK(vecEqual(topFace(id)["uAxis"], rotated["uAxis"]));
  }
}

} // namespace tb::mcp
