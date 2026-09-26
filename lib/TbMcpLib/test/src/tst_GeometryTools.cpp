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
#include "mcp/tools/GeometryTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
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

void collectBrushes(mdl::Node& node, std::vector<mdl::BrushNode*>& result)
{
  if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
  {
    result.push_back(brushNode);
  }
  for (auto* child : node.children())
  {
    collectBrushes(*child, result);
  }
}

std::vector<mdl::BrushNode*> allBrushes(mdl::Map& map)
{
  auto result = std::vector<mdl::BrushNode*>{};
  collectBrushes(map.worldNode(), result);
  return result;
}

const mdl::BrushNode& brushNode(McpToolFixture& fixture, const Json& id)
{
  auto* node = dynamic_cast<mdl::BrushNode*>(fixture.node(id.get<std::string>()));
  REQUIRE(node);
  return *node;
}

std::vector<const mdl::BrushNode*> brushNodes(McpToolFixture& fixture, const Json& ids)
{
  auto result = std::vector<const mdl::BrushNode*>{};
  for (const auto& id : ids)
  {
    result.push_back(&brushNode(fixture, id));
  }
  return result;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

std::vector<std::string> selectedIds(McpToolFixture& fixture, mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto* node : map.selection().nodes)
  {
    result.push_back(fixture.id(*node));
  }
  return result;
}

size_t faceCount(const std::vector<const mdl::BrushNode*>& nodes)
{
  auto result = size_t{0};
  for (const auto* node : nodes)
  {
    result += node->brush().faceCount();
  }
  return result;
}

bool hasFaceWithNormal(const mdl::BrushNode& node, const vm::vec3d& normal)
{
  return std::ranges::any_of(node.brush().faces(), [&](const auto& face) {
    return vm::is_equal(face.normal(), normal, 0.0001);
  });
}

bool allFacesUse(const mdl::BrushNode& node, const std::string& material)
{
  return std::ranges::all_of(node.brush().faces(), [&](const auto& face) {
    return face.materialName() == material;
  });
}

double volume(const vm::bbox3d& box)
{
  const auto size = box.size();
  return size.x() * size.y() * size.z();
}

double overlapVolume(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  auto result = 1.0;
  for (size_t i = 0; i < 3; ++i)
  {
    result *=
      std::max(0.0, std::min(lhs.max[i], rhs.max[i]) - std::max(lhs.min[i], rhs.min[i]));
  }
  return result;
}

Json boxJson(const vm::vec3d& min, const vm::vec3d& max)
{
  return Json{{"min", {min.x(), min.y(), min.z()}}, {"max", {max.x(), max.y(), max.z()}}};
}

} // namespace

TEST_CASE("GeometryTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("brush_create_box")
  {
    const auto created = fixture.call(
      "brush_create_box",
      Json{{"min", {0, 0, 0}}, {"max", {128, 64, 16}}, {"material", "floor_stone"}});
    const auto& result = resultOf(created);
    const auto& node = brushNode(fixture, result["brush"]);

    CHECK(node.logicalBounds() == vm::bbox3d{{0, 0, 0}, {128, 64, 16}});
    CHECK(allFacesUse(node, "floor_stone"));
    CHECK(result["object"]["id"] == result["brush"]);
    CHECK(result["object"]["kind"] == "brush");
    CHECK(created["undoStep"] == "AI: Create Box Brush");
    CHECK(created["changes"]["created"] == Json{result["brush"]});
    CHECK(hasWarning(created, "UNKNOWN_MATERIAL"));
    CHECK(node.parent() == map.editorContext().currentLayer());
    CHECK(selectedIds(fixture, map) == std::vector{result["brush"].get<std::string>()});

    SECTION("uses the current material by default")
    {
      map.setCurrentMaterialName("current_mat");
      const auto other = fixture.call(
        "brush_create_box", Json{{"min", {0, 0, 32}}, {"max", {16, 16, 48}}});
      CHECK(allFacesUse(brushNode(fixture, resultOf(other)["brush"]), "current_mat"));
      CHECK(!hasWarning(other, "UNKNOWN_MATERIAL"));
      CHECK(
        selectedIds(fixture, map)
        == std::vector{resultOf(other)["brush"].get<std::string>()});
    }

    SECTION("undo removes the brush")
    {
      fixture.call("undo");
      CHECK(allBrushes(map).empty());
    }
  }

  SECTION("brush_create_box errors")
  {
    const auto degenerate = fixture.callExpectingError(
      "brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {128, 0, 16}}});
    CHECK(degenerate.code == ErrorCode::InvalidGeometry);
    CHECK(degenerate.message.find("along y") != std::string::npos);
    CHECK(!degenerate.hint.empty());

    const auto outside = fixture.callExpectingError(
      "brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {128, 64, 10000}}});
    CHECK(outside.code == ErrorCode::OutOfWorldBounds);

    CHECK(allBrushes(map).empty());
  }

  SECTION("brush_create_box dry run")
  {
    const auto dryRun = fixture.call(
      "brush_create_box",
      Json{{"min", {0, 0, 0}}, {"max", {128, 64, 16}}, {"dryRun", true}});
    CHECK(dryRun["dryRun"] == true);
    CHECK(dryRun["changes"]["created"].size() == 1);
    CHECK(dryRun["changes"]["ephemeral"] == true);
    CHECK(allBrushes(map).empty());
    CHECK(!map.canUndoCommand());
  }

  SECTION("brush_create_shape")
  {
    const auto shape = [&](Json arguments) {
      const auto created = fixture.call("brush_create_shape", std::move(arguments));
      CHECK(created["undoStep"] == "AI: Create Shape");
      CHECK(resultOf(created)["count"] == resultOf(created)["brushes"].size());
      return created;
    };

    SECTION("cuboid")
    {
      const auto created = shape(Json{
        {"shape", "cuboid"},
        {"min", {0, 0, 0}},
        {"max", {64, 64, 64}},
        {"material", "crate"}});
      const auto& result = resultOf(created);
      REQUIRE(result["brushes"].size() == 1);
      const auto& node = brushNode(fixture, result["brushes"][0]);
      CHECK(node.logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
      CHECK(allFacesUse(node, "crate"));
      CHECK(result["group"].is_null());
      CHECK(result["objects"].size() == 1);
      CHECK(
        selectedIds(fixture, map) == result["brushes"].get<std::vector<std::string>>());
    }

    SECTION("stairs")
    {
      const auto created = shape(Json{
        {"shape", "stairs"},
        {"min", {0, 0, 0}},
        {"max", {128, 64, 64}},
        {"stepHeight", 16},
        {"stairDirection", "-x"}});
      const auto nodes = brushNodes(fixture, resultOf(created)["brushes"]);
      REQUIRE(nodes.size() == 4);

      // ascending towards -x: the tallest step is at the minimum x
      const auto tallest = std::ranges::max_element(nodes, [](auto* lhs, auto* rhs) {
        return lhs->logicalBounds().max.z() < rhs->logicalBounds().max.z();
      });
      CHECK((*tallest)->logicalBounds() == vm::bbox3d{{0, 0, 0}, {32, 64, 64}});

      const auto single = shape(Json{
        {"shape", "stairs"},
        {"min", {0, 0, 128}},
        {"max", {128, 64, 136}},
        {"stepHeight", 16}});
      CHECK(resultOf(single)["count"] == 1);
      CHECK(hasWarning(single, "SINGLE_STEP"));

      const auto error = fixture.callExpectingError(
        "brush_create_shape",
        Json{
          {"shape", "stairs"},
          {"min", {0, 0, 0}},
          {"max", {128, 64, 64}},
          {"stepHeight", 0}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("stepHeight") != std::string::npos);
    }

    SECTION("arch")
    {
      const auto arguments = Json{
        {"shape", "arch"},
        {"min", {0, 0, 0}},
        {"max", {32, 128, 64}},
        {"thickness", 16},
        {"sides", 8}};
      const auto plain = shape(arguments);
      const auto plainCount = resultOf(plain)["count"].get<size_t>();
      CHECK(plainCount > 1);

      // the default axis x makes an upright arch spanning y
      for (const auto* node : brushNodes(fixture, resultOf(plain)["brushes"]))
      {
        CHECK(node->logicalBounds().min.x() == 0.0);
        CHECK(node->logicalBounds().max.x() == 32.0);
      }

      auto withSpandrel = arguments;
      withSpandrel["spandrel"] = true;
      withSpandrel["min"] = {0, 0, 256};
      withSpandrel["max"] = {32, 128, 320};
      CHECK(resultOf(shape(withSpandrel))["count"].get<size_t>() > plainCount);

      auto tooThick = arguments;
      tooThick["thickness"] = 64;
      const auto error = fixture.callExpectingError("brush_create_shape", tooThick);
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("thickness") != std::string::npos);
    }

    SECTION("cylinder")
    {
      const auto edgeAligned = shape(Json{
        {"shape", "cylinder"},
        {"min", {0, 0, 0}},
        {"max", {64, 64, 64}},
        {"sides", 8},
        {"circleMode", "edgeAligned"}});
      const auto vertexAligned = shape(Json{
        {"shape", "cylinder"},
        {"min", {128, 0, 0}},
        {"max", {192, 64, 64}},
        {"sides", 8},
        {"circleMode", "vertexAligned"}});
      const auto& edgeNode = brushNode(fixture, resultOf(edgeAligned)["brushes"][0]);
      const auto& vertexNode = brushNode(fixture, resultOf(vertexAligned)["brushes"][0]);
      CHECK(edgeNode.brush().faceCount() == 10);
      CHECK(vertexNode.brush().faceCount() == 10);
      CHECK(hasFaceWithNormal(edgeNode, {1, 0, 0}));
      CHECK(!hasFaceWithNormal(vertexNode, {1, 0, 0}));

      const auto horizontal = shape(Json{
        {"shape", "cylinder"},
        {"min", {0, 256, 0}},
        {"max", {128, 320, 64}},
        {"axis", "x"}});
      const auto& horizontalNode = brushNode(fixture, resultOf(horizontal)["brushes"][0]);
      CHECK(hasFaceWithNormal(horizontalNode, {1, 0, 0}));
      CHECK(hasFaceWithNormal(horizontalNode, {-1, 0, 0}));

      const auto scalable = shape(Json{
        {"shape", "cylinder"},
        {"min", {0, 512, 0}},
        {"max", {64, 576, 64}},
        {"circleMode", "scalable"},
        {"precision", 1}});
      CHECK(
        brushNode(fixture, resultOf(scalable)["brushes"][0]).brush().faceCount() == 26);

      const auto hollow = shape(Json{
        {"shape", "cylinder"},
        {"min", {0, 0, 128}},
        {"max", {128, 128, 256}},
        {"sides", 12},
        {"hollow", true},
        {"thickness", 16},
        {"group", "Tower"}});
      CHECK(resultOf(hollow)["count"] == 12);
      REQUIRE(resultOf(hollow)["group"].is_string());
      const auto groupId = resultOf(hollow)["group"].get<std::string>();
      CHECK(dynamic_cast<mdl::GroupNode*>(fixture.node(groupId)));
      CHECK(selectedIds(fixture, map) == std::vector{groupId});

      const auto tooThick = fixture.callExpectingError(
        "brush_create_shape",
        Json{
          {"shape", "cylinder"},
          {"min", {0, 0, 0}},
          {"max", {64, 128, 64}},
          {"hollow", true},
          {"thickness", 32}});
      CHECK(tooThick.code == ErrorCode::InvalidArgument);
      CHECK(tooThick.message.find("half") != std::string::npos);

      const auto tooFewSides = fixture.callExpectingError(
        "brush_create_shape",
        Json{
          {"shape", "cylinder"},
          {"min", {0, 0, 0}},
          {"max", {64, 64, 64}},
          {"sides", 2}});
      CHECK(tooFewSides.code == ErrorCode::InvalidArgument);
    }

    SECTION("cone")
    {
      const auto created = shape(Json{
        {"shape", "cone"}, {"min", {0, 0, 0}}, {"max", {64, 64, 128}}, {"sides", 6}});
      const auto& node = brushNode(fixture, resultOf(created)["brushes"][0]);
      CHECK(node.brush().faceCount() == 7);
      CHECK(node.logicalBounds().max.z() == 128.0);
    }

    SECTION("uvSphere")
    {
      const auto fewRings = shape(Json{
        {"shape", "uvSphere"}, {"min", {0, 0, 0}}, {"max", {64, 64, 64}}, {"rings", 4}});
      const auto manyRings = shape(Json{
        {"shape", "uvSphere"},
        {"min", {128, 0, 0}},
        {"max", {192, 64, 64}},
        {"rings", 8}});
      CHECK(
        faceCount(brushNodes(fixture, resultOf(fewRings)["brushes"]))
        < faceCount(brushNodes(fixture, resultOf(manyRings)["brushes"])));
    }

    SECTION("icoSphere")
    {
      const auto coarse = shape(Json{
        {"shape", "icoSphere"},
        {"min", {0, 0, 0}},
        {"max", {64, 64, 64}},
        {"subdivision", 1}});
      const auto fine = shape(Json{
        {"shape", "icoSphere"},
        {"min", {128, 0, 0}},
        {"max", {192, 64, 64}},
        {"subdivision", 2}});
      CHECK(
        faceCount(brushNodes(fixture, resultOf(coarse)["brushes"]))
        < faceCount(brushNodes(fixture, resultOf(fine)["brushes"])));
      CHECK(hasWarning(fine, "NON_INTEGER_VERTICES"));
    }

    SECTION("ignored arguments are warned about")
    {
      const auto created = shape(Json{
        {"shape", "cuboid"}, {"min", {0, 0, 0}}, {"max", {64, 64, 64}}, {"sides", 12}});
      CHECK(hasWarning(created, "IGNORED_ARGUMENT"));
    }

    SECTION("invalid bounds")
    {
      const auto error = fixture.callExpectingError(
        "brush_create_shape",
        Json{{"shape", "cone"}, {"min", {0, 0, 0}}, {"max", {64, 64, 0}}});
      CHECK(error.code == ErrorCode::InvalidGeometry);
      CHECK(allBrushes(map).empty());
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "brush_create_shape",
        Json{
          {"shape", "stairs"},
          {"min", {0, 0, 0}},
          {"max", {128, 64, 64}},
          {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(dryRun["changes"]["created"].size() == 4);
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(allBrushes(map).empty());
    }
  }

  SECTION("brush_create_hull")
  {
    const auto created = fixture.call(
      "brush_create_hull",
      Json{
        {"points", {{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {0, 0, 64}, {8, 8, 8}}},
        {"material", "rock"}});
    const auto& result = resultOf(created);
    const auto& node = brushNode(fixture, result["brush"]);
    CHECK(result["vertexCount"] == 4);
    CHECK(node.brush().faceCount() == 4);
    CHECK(node.logicalBounds() == vm::bbox3d{{0, 0, 0}, {128, 128, 64}});
    CHECK(allFacesUse(node, "rock"));
    CHECK(hasWarning(created, "POINTS_INSIDE_HULL"));
    CHECK(!hasWarning(created, "NON_INTEGER_VERTICES"));
    CHECK(created["undoStep"] == "AI: Create Hull Brush");
    CHECK(selectedIds(fixture, map) == std::vector{result["brush"].get<std::string>()});

    const auto fractional = fixture.call(
      "brush_create_hull",
      Json{{"points", {{0, 0, 256}, {64.5, 0, 256}, {0, 64, 256}, {0, 0, 320}}}});
    CHECK(hasWarning(fractional, "NON_INTEGER_VERTICES"));

    const auto tooFew = fixture.callExpectingError(
      "brush_create_hull", Json{{"points", {{0, 0, 0}, {64, 0, 0}, {0, 64, 0}}}});
    CHECK(tooFew.code == ErrorCode::InvalidGeometry);
    CHECK(tooFew.message.find("at least 4") != std::string::npos);

    const auto coplanar = fixture.callExpectingError(
      "brush_create_hull",
      Json{{"points", {{0, 0, 0}, {64, 0, 0}, {0, 64, 0}, {64, 64, 0}}}});
    CHECK(coplanar.code == ErrorCode::InvalidGeometry);
    CHECK(coplanar.message.find("one plane") != std::string::npos);

    const auto collinear = fixture.callExpectingError(
      "brush_create_hull",
      Json{{"points", {{0, 0, 0}, {16, 16, 16}, {32, 32, 32}, {64, 64, 64}}}});
    CHECK(collinear.code == ErrorCode::InvalidGeometry);
    CHECK(collinear.message.find("one line") != std::string::npos);

    const auto outside = fixture.callExpectingError(
      "brush_create_hull",
      Json{{"points", {{0, 0, 0}, {64, 0, 0}, {0, 64, 0}, {0, 0, 9000}}}});
    CHECK(outside.code == ErrorCode::OutOfWorldBounds);
    CHECK(outside.details["pointIndices"] == Json{3});

    SECTION("dry run")
    {
      const auto brushCount = allBrushes(map).size();
      const auto dryRun = fixture.call(
        "brush_create_hull",
        Json{
          {"points", {{0, 0, 512}, {64, 0, 512}, {0, 64, 512}, {0, 0, 576}}},
          {"dryRun", true}});
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(allBrushes(map).size() == brushCount);
    }
  }

  SECTION("room_create")
  {
    const auto created = fixture.call(
      "room_create",
      Json{
        {"min", {0, 0, 0}},
        {"max", {256, 192, 128}},
        {"thickness", 16},
        {"material", "base"},
        {"floor", "floor_stone"},
        {"walls", "wall_brick"}});
    const auto& result = resultOf(created);
    CHECK(created["undoStep"] == "AI: Create Room");
    CHECK(result["outer"] == boxJson({-16, -16, -16}, {272, 208, 144}));

    const auto& roles = result["brushes"];
    REQUIRE(roles.size() == 6);
    for (const auto* role :
         {"floor", "ceiling", "wallWest", "wallEast", "wallSouth", "wallNorth"})
    {
      CHECK(roles.contains(role));
    }
    CHECK(allBrushes(map).size() == 6);
    CHECK(map.selection().nodes.size() == 6);

    CHECK(allFacesUse(brushNode(fixture, roles["floor"]), "floor_stone"));
    CHECK(allFacesUse(brushNode(fixture, roles["ceiling"]), "base"));
    CHECK(allFacesUse(brushNode(fixture, roles["wallNorth"]), "wall_brick"));
    CHECK(
      brushNode(fixture, roles["floor"]).logicalBounds()
      == vm::bbox3d{{-16, -16, -16}, {272, 208, 0}});

    // sealed: the brushes do not overlap and fill the shell between inner and outer box
    const auto inner = vm::bbox3d{{0, 0, 0}, {256, 192, 128}};
    const auto outer = vm::bbox3d{{-16, -16, -16}, {272, 208, 144}};
    auto bounds = std::vector<vm::bbox3d>{};
    auto total = 0.0;
    for (const auto* node : allBrushes(map))
    {
      CHECK(overlapVolume(node->logicalBounds(), inner) == 0.0);
      for (const auto& other : bounds)
      {
        CHECK(overlapVolume(node->logicalBounds(), other) == 0.0);
      }
      bounds.push_back(node->logicalBounds());
      total += volume(node->logicalBounds());
    }
    CHECK(total == volume(outer) - volume(inner));

    const auto space =
      fixture.call("space_check", Json{{"box", boxJson({0, 0, 0}, {256, 192, 128})}});
    CHECK(space["free"] == true);

    SECTION("undo removes the whole room in one step")
    {
      fixture.call("undo");
      CHECK(allBrushes(map).empty());
    }
  }

  SECTION("room_create options and errors")
  {
    SECTION("default thickness is the grid size and group")
    {
      const auto created = fixture.call(
        "room_create",
        Json{{"min", {0, 0, 0}}, {"max", {128, 128, 128}}, {"group", "Room"}});
      const auto grid = map.grid().actualSize();
      CHECK(resultOf(created)["thickness"] == grid);
      REQUIRE(resultOf(created)["group"].is_string());
      auto* group = dynamic_cast<mdl::GroupNode*>(
        fixture.node(resultOf(created)["group"].get<std::string>()));
      REQUIRE(group);
      CHECK(group->name() == "Room");
      CHECK(group->children().size() == 6);
    }

    const auto degenerate = fixture.callExpectingError(
      "room_create", Json{{"min", {0, 0, 0}}, {"max", {128, 0, 128}}});
    CHECK(degenerate.code == ErrorCode::InvalidGeometry);

    const auto outside = fixture.callExpectingError(
      "room_create",
      Json{{"min", {0, 0, 0}}, {"max", {128, 128, 8190}}, {"thickness", 16}});
    CHECK(outside.code == ErrorCode::OutOfWorldBounds);

    const auto thickness = fixture.callExpectingError(
      "room_create",
      Json{{"min", {0, 0, 0}}, {"max", {128, 128, 128}}, {"thickness", 0}});
    CHECK(thickness.code == ErrorCode::InvalidArgument);

    const auto dryRun = fixture.call(
      "room_create",
      Json{{"min", {0, 0, 0}}, {"max", {128, 128, 128}}, {"dryRun", true}});
    CHECK(dryRun["changes"]["created"].size() == 6);
    CHECK(dryRun["changes"]["ephemeral"] == true);
  }

  SECTION("opening_cut")
  {
    const auto room = fixture.call(
      "room_create",
      Json{
        {"min", {0, 0, 0}},
        {"max", {256, 256, 128}},
        {"thickness", 16},
        {"walls", "wall_brick"}});
    const auto& roles = resultOf(room)["brushes"];
    const auto southId = roles["wallSouth"].get<std::string>();
    const auto opening = boxJson({96, -16, 0}, {160, 0, 96});

    SECTION("explicit wall")
    {
      auto arguments = opening;
      arguments["ids"] = {southId};
      arguments["material"] = "trim";
      const auto cut = fixture.call("opening_cut", arguments);
      const auto& result = resultOf(cut);
      CHECK(cut["undoStep"] == "AI: Cut Opening");
      REQUIRE(result["cuts"].size() == 1);
      CHECK(result["cuts"][0]["removed"] == southId);
      CHECK(cut["changes"]["removed"] == Json{southId});
      CHECK(fixture.node(southId) == nullptr);

      const auto fragments = brushNodes(fixture, result["cuts"][0]["fragments"]);
      CHECK(fragments.size() == 3);
      CHECK(allBrushes(map).size() == 8);
      auto reveals = size_t{0};
      for (const auto* fragment : fragments)
      {
        CHECK(fragment->parent() == map.editorContext().currentLayer());
        CHECK(
          overlapVolume(fragment->logicalBounds(), {{96, -16, 0}, {160, 0, 96}}) == 0.0);
        reveals +=
          size_t(std::ranges::count_if(fragment->brush().faces(), [](const auto& f) {
            return f.materialName() == "trim";
          }));
        // the wall's own faces keep their material although the opening is flush
        for (const auto& face : fragment->brush().faces())
        {
          if (std::abs(face.normal().y()) > 0.99)
          {
            CHECK(face.materialName() == "wall_brick");
          }
        }
      }
      CHECK(reveals >= 3);
      CHECK(
        selectedIds(fixture, map)
        == result["cuts"][0]["fragments"].get<std::vector<std::string>>());

      const auto space = fixture.call("space_check", Json{{"box", opening}});
      CHECK(space["free"] == true);

      SECTION("undo restores the wall")
      {
        fixture.call("undo");
        CHECK(fixture.node(southId) != nullptr);
        CHECK(allBrushes(map).size() == 6);
      }
    }

    SECTION("finds the intersecting walls")
    {
      // a window through the south-west corner cuts two walls; the floor only touches
      const auto cut = fixture.call("opening_cut", boxJson({-16, -16, 32}, {32, 32, 64}));
      const auto& cuts = resultOf(cut)["cuts"];
      REQUIRE(cuts.size() == 2);
      auto removed = std::vector<std::string>{};
      for (const auto& entry : cuts)
      {
        removed.push_back(entry["removed"].get<std::string>());
      }
      std::ranges::sort(removed);
      auto expected = std::vector{roles["wallWest"].get<std::string>(), southId};
      std::ranges::sort(expected);
      CHECK(removed == expected);

      const auto space =
        fixture.call("space_check", Json{{"box", boxJson({-16, -16, 32}, {32, 32, 64})}});
      CHECK(space["free"] == true);
    }

    SECTION("errors")
    {
      auto missing = boxJson({96, 64, 32}, {160, 128, 96});
      const auto nothing = fixture.callExpectingError("opening_cut", missing);
      CHECK(nothing.code == ErrorCode::InvalidArgument);
      CHECK(!nothing.hint.empty());

      missing["ids"] = {southId};
      const auto notThisWall = fixture.callExpectingError("opening_cut", missing);
      CHECK(notThisWall.code == ErrorCode::InvalidArgument);
      CHECK(notThisWall.objectIds == std::vector{southId});

      const auto degenerate =
        fixture.callExpectingError("opening_cut", boxJson({96, -16, 0}, {96, 0, 96}));
      CHECK(degenerate.code == ErrorCode::InvalidGeometry);

      CHECK(allBrushes(map).size() == 6);
    }

    SECTION("dry run")
    {
      auto arguments = opening;
      arguments["dryRun"] = true;
      const auto dryRun = fixture.call("opening_cut", arguments);
      CHECK(dryRun["changes"]["removed"] == Json{southId});
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(fixture.node(southId) != nullptr);
      CHECK(allBrushes(map).size() == 6);
    }
  }
}

} // namespace tb::mcp
