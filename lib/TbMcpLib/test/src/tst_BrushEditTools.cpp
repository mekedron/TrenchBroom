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
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
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

mdl::BrushNode* addBrush(mdl::Map& map, const vm::bbox3d& bounds)
{
  const auto builder = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto* brushNode =
    new mdl::BrushNode{builder.createCuboid(bounds, "material") | kdl::value()};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  return brushNode;
}

std::string faceIdWithNormal(
  McpToolFixture& fixture, const mdl::BrushNode& brushNode, const vm::vec3d& normal)
{
  const auto faceIndex = brushNode.brush().findFace(normal);
  REQUIRE(faceIndex);
  return fixture.id(brushNode) + "/face:" + std::to_string(*faceIndex);
}

vm::bbox3d boundsOf(const Json& summary)
{
  return *boxFromJson(summary["bounds"]);
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

bool contains(const std::vector<std::string>& ids, const std::string& id)
{
  return std::ranges::find(ids, id) != ids.end();
}

std::vector<std::string> selectedIds(McpToolFixture& fixture, const mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto* node : map.selection().nodes)
  {
    result.push_back(fixture.id(*node));
  }
  return result;
}

bool hasIntegerVertices(const mdl::BrushNode& brushNode)
{
  return std::ranges::all_of(
    brushNode.brush().vertexPositions(), [](const auto& p) { return p == vm::round(p); });
}

} // namespace

TEST_CASE("BrushEditTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();
  const auto& worldBounds = map.worldBounds();

  // a: 0..64 cube, b: the cube next to it along x (shares the face x = 64),
  // far: a cube far away along x
  auto* a = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
  auto* b = addBrush(map, {{64, 0, 0}, {128, 64, 64}});
  auto* far = addBrush(map, {{256, 0, 0}, {320, 64, 64}});
  const auto aId = fixture.id(*a);
  const auto bId = fixture.id(*b);
  const auto farId = fixture.id(*far);
  const auto aTop = faceIdWithNormal(fixture, *a, {0, 0, 1});
  const auto aRight = faceIdWithNormal(fixture, *a, {1, 0, 0});

  auto* entity = new mdl::EntityNode{mdl::Entity{{{"classname", "info_null"}}}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entity}}});
  const auto entityId = fixture.id(*entity);

  SECTION("brush_clip")
  {
    SECTION("2 points split a brush; the normal is cross(b - a, axis)")
    {
      const auto result = fixture.call(
        "brush_clip",
        Json{
          {"ids", {aId}},
          {"points", {{32, 0, 0}, {32, 64, 0}}},
          {"keep", "back"},
        });
      CHECK(result["undoStep"] == "AI: Clip Brushes");
      const auto& clip = resultOf(result);
      CHECK(clip["normal"] == Json{1, 0, 0});
      CHECK(clip["split"] == Json{aId});
      REQUIRE(clip["brushes"].size() == 1);
      CHECK(boundsOf(clip["brushes"][0]) == vm::bbox3d{{0, 0, 0}, {32, 64, 64}});
      CHECK(fixture.node(aId) == nullptr);
      CHECK(result["changes"]["removed"] == Json{aId});

      // the result is selected, as in the editor
      CHECK(
        selectedIds(fixture, map)
        == std::vector<std::string>{clip["brushes"][0]["id"].get<std::string>()});

      fixture.call("undo");
      CHECK(fixture.node(aId) != nullptr);
    }

    SECTION("keep both, 3 points and the selection")
    {
      mdl::selectNodes(map, {a, far});
      const auto result = fixture.call(
        "brush_clip", Json{{"points", {{0, 0, 16}, {0, 64, 16}, {64, 0, 16}}}});
      const auto& clip = resultOf(result);
      CHECK(clip["normal"] == Json{0, 0, -1});
      CHECK(clip["split"] == Json{aId, farId});
      CHECK(clip["brushes"].size() == 4);
      CHECK(map.selection().nodes.size() == 4);
    }

    SECTION("brushes on the discarded side are removed, others stay unchanged")
    {
      const auto result = fixture.call(
        "brush_clip",
        Json{
          {"ids", {aId, farId}},
          {"points", {{200, 0, 0}, {200, 64, 0}}},
          {"keep", "front"},
        });
      const auto& clip = resultOf(result);
      CHECK(clip["discarded"] == Json{aId});
      CHECK(clip["unchanged"] == Json{farId});
      CHECK(clip["split"].empty());
      CHECK(fixture.node(aId) == nullptr);
      CHECK(fixture.node(farId) == far);
    }

    SECTION("plane of a face")
    {
      const auto bottom = faceIdWithNormal(fixture, *a, {0, 0, -1});
      const auto result = fixture.call(
        "brush_clip", Json{{"ids", {farId}}, {"face", bottom}, {"keep", "back"}});
      CHECK(resultOf(result)["unchanged"] == Json{farId});
      CHECK(hasWarning(result, "NOTHING_CLIPPED"));
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "brush_clip",
        Json{{"ids", {aId}}, {"points", {{32, 0, 0}, {32, 64, 0}}}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["changes"]["ephemeral"] == true);
      CHECK(result["changes"]["created"].size() == 2);
      CHECK(fixture.node(aId) == a);
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "brush_clip",
            Json{{"ids", {aId}}, {"points", {{0, 0, 0}, {1, 0, 0}}}, {"face", aTop}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("brush_clip", Json{{"ids", {aId}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "brush_clip",
            Json{{"ids", {aId}}, {"points", {{0, 0, 0}, {0, 0, 64}}}, {"axis", "z"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "brush_clip", Json{{"ids", {entityId}}, {"points", {{0, 0, 0}, {0, 64, 0}}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("brush_clip", Json{{"points", {{0, 0, 0}, {0, 64, 0}}}})
          .code
        == ErrorCode::NoSelection);

      const auto outside = fixture.callExpectingError(
        "brush_clip",
        Json{{"ids", {aId}}, {"points", {{0, 0, 0}, {0, worldBounds.max.y() + 1, 0}}}});
      CHECK(outside.code == ErrorCode::OutOfWorldBounds);
    }
  }

  SECTION("face_extrude")
  {
    SECTION("explicit faces with different normals")
    {
      mdl::selectNodes(map, {far});
      const auto result =
        fixture.call("face_extrude", Json{{"faces", {aTop, aRight}}, {"distance", 16}});
      CHECK(result["undoStep"] == "AI: Extrude Faces");
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {80, 64, 80}});
      CHECK(resultOf(result)["faces"].size() == 2);
      CHECK(resultOf(result)["brushes"][0]["id"] == aId);
      CHECK(result["changes"]["modified"] == Json{aId});

      // the human's selection is restored
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{farId});

      fixture.call("undo");
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
    }

    SECTION("selected faces, inward")
    {
      mdl::selectBrushFaces(map, {{a, *a->brush().findFace(vm::vec3d{0, 0, 1})}});
      fixture.call("face_extrude", Json{{"distance", -16}});
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 48}});
      CHECK(map.selection().brushFaces.size() == 1);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "face_extrude", Json{{"faces", {aTop}}, {"distance", 16}, {"dryRun", true}});
      CHECK(result["changes"]["modified"] == Json{aId});
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
    }

    SECTION("degenerate and out of bounds results")
    {
      const auto degenerate = fixture.callExpectingError(
        "face_extrude", Json{{"faces", {aTop}}, {"distance", -64}});
      CHECK(degenerate.code == ErrorCode::InvalidGeometry);
      CHECK(contains(degenerate.objectIds, aId));
      CHECK(contains(degenerate.objectIds, aTop));
      CHECK(!degenerate.hint.empty());

      const auto outside = fixture.callExpectingError(
        "face_extrude", Json{{"faces", {aTop}}, {"distance", worldBounds.max.z()}});
      CHECK(outside.code == ErrorCode::OutOfWorldBounds);
      CHECK(contains(outside.objectIds, aId));
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("face_extrude", Json{{"distance", 16}}).code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError("face_extrude", Json{{"faces", {aTop}}, {"distance", 0}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "face_extrude", Json{{"faces", {aId + "/face:99"}}, {"distance", 16}})
          .code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("face_extrude", Json{{"faces", {aTop}}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("face_extrude_new")
  {
    SECTION("split outward")
    {
      const auto result =
        fixture.call("face_extrude_new", Json{{"faces", {aTop}}, {"distance", 32}});
      CHECK(result["undoStep"] == "AI: Extrude Faces to New Brushes");
      const auto& brushes = resultOf(result)["brushes"];
      REQUIRE(brushes.size() == 1);
      CHECK(boundsOf(brushes[0]) == vm::bbox3d{{0, 0, 64}, {64, 64, 96}});
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
      CHECK(
        selectedIds(fixture, map)
        == std::vector<std::string>{brushes[0]["id"].get<std::string>()});
    }

    SECTION("split inward")
    {
      const auto result =
        fixture.call("face_extrude_new", Json{{"faces", {aTop}}, {"distance", -16}});
      const auto& brushes = resultOf(result)["brushes"];
      REQUIRE(brushes.size() == 1);
      CHECK(boundsOf(brushes[0]) == vm::bbox3d{{0, 0, 0}, {64, 64, 48}});
      CHECK(a->logicalBounds() == vm::bbox3d{{0, 0, 48}, {64, 64, 64}});
      CHECK(resultOf(result)["resized"] == Json{aId});
    }

    SECTION("stamp")
    {
      const auto result = fixture.call(
        "face_extrude_new",
        Json{{"faces", {aRight}}, {"distance", 16}, {"mode", "stamp"}, {"dryRun", true}});
      CHECK(result["changes"]["ephemeral"] == true);
      CHECK(
        boundsOf(resultOf(result)["brushes"][0]) == vm::bbox3d{{64, 0, 0}, {80, 64, 64}});
      CHECK(map.selection().nodes.empty());
    }

    SECTION("errors")
    {
      CHECK(
        fixture
          .callExpectingError(
            "face_extrude_new",
            Json{{"faces", {aTop}}, {"distance", -16}, {"mode", "stamp"}})
          .code
        == ErrorCode::InvalidArgument);

      const auto tooDeep = fixture.callExpectingError(
        "face_extrude_new", Json{{"faces", {aTop}}, {"distance", -64}});
      CHECK(tooDeep.code == ErrorCode::InvalidGeometry);
      CHECK(contains(tooDeep.objectIds, aId));

      const auto outside = fixture.callExpectingError(
        "face_extrude_new",
        Json{{"faces", {aTop}}, {"distance", worldBounds.max.z()}, {"mode", "stamp"}});
      CHECK(outside.code == ErrorCode::OutOfWorldBounds);
    }
  }

  SECTION("vertices_move")
  {
    SECTION("a shared edge moves in all brushes that have it")
    {
      const auto result = fixture.call(
        "vertices_move",
        Json{{"edges", {{{64, 0, 64}, {64, 64, 64}}}}, {"vector", {0, 0, 16}}});
      CHECK(result["undoStep"] == "AI: Move Vertices");
      CHECK(resultOf(result)["brushes"].size() == 2);
      CHECK(a->logicalBounds().max.z() == 80);
      CHECK(b->logicalBounds().max.z() == 80);
      CHECK(far->logicalBounds().max.z() == 64);
    }

    SECTION("explicit ids restrict the brushes")
    {
      fixture.call(
        "vertices_move",
        Json{
          {"ids", {aId}},
          {"vertices", {{64, 64, 64}}},
          {"vector", {0, 0, 16}},
        });
      CHECK(a->logicalBounds().max.z() == 80);
      CHECK(b->logicalBounds().max.z() == 64);
    }

    SECTION("a face, given in any vertex order, with the selection")
    {
      mdl::selectNodes(map, {a});
      const auto result = fixture.call(
        "vertices_move",
        Json{
          {"faces", {{{0, 0, 64}, {64, 64, 64}, {64, 0, 64}, {0, 64, 64}}}},
          {"vector", {0, 0, 8.5}},
        });
      CHECK(a->logicalBounds().max.z() == 72.5);
      CHECK(b->logicalBounds().max.z() == 64);
      CHECK(hasWarning(result, "NON_INTEGER_VERTICES"));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{aId});
    }

    SECTION("dry run")
    {
      fixture.call(
        "vertices_move",
        Json{{"vertices", {{0, 0, 0}}}, {"vector", {0, 0, -16}}, {"dryRun", true}});
      CHECK(a->logicalBounds().min.z() == 0);
    }

    SECTION("handles outside the selection are found in the brushes that have them")
    {
      mdl::selectNodes(map, {far});
      const auto result = fixture.call(
        "vertices_move",
        Json{{"edges", {{{64, 0, 64}, {64, 64, 64}}}}, {"vector", {0, 0, 16}}});
      CHECK(a->logicalBounds().max.z() == 80);
      CHECK(b->logicalBounds().max.z() == 80);
      CHECK(far->logicalBounds().max.z() == 64);
      CHECK(hasWarning(result, "HANDLES_OUTSIDE_SELECTION"));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{farId});
    }

    SECTION("a selection that has the handles restricts the brushes")
    {
      mdl::selectNodes(map, {b});
      const auto result = fixture.call(
        "vertices_move", Json{{"vertices", {{64, 64, 64}}}, {"vector", {0, 0, 16}}});
      CHECK(a->logicalBounds().max.z() == 64);
      CHECK(b->logicalBounds().max.z() == 80);
      CHECK_FALSE(hasWarning(result, "HANDLES_OUTSIDE_SELECTION"));
    }

    SECTION("errors for handles that are not found say where they were searched")
    {
      const auto inIds = fixture.callExpectingError(
        "vertices_move",
        Json{{"ids", {farId}}, {"vertices", {{0, 0, 0}}}, {"vector", {0, 0, 16}}});
      CHECK(inIds.code == ErrorCode::InvalidArgument);
      CHECK(inIds.message.find("given in 'ids' (" + farId + ")") != std::string::npos);
      CHECK(inIds.hint.find(aId) != std::string::npos);
      CHECK(inIds.hint.find("omit 'ids'") != std::string::npos);
      CHECK(inIds.objectIds == std::vector<std::string>{aId});

      mdl::selectNodes(map, {far});
      const auto withSelection = fixture.callExpectingError(
        "vertices_move", Json{{"vertices", {{1, 2, 3}}}, {"vector", {0, 0, 16}}});
      CHECK(withSelection.code == ErrorCode::InvalidArgument);
      CHECK(
        withSelection.message.find("neither the selection (1 brush) nor the 3 visible")
        != std::string::npos);
      CHECK(withSelection.hint.find("No brush in the map") != std::string::npos);

      mdl::deselectAll(map);
      mdl::hideNodes(map, {a});
      const auto hidden = fixture.callExpectingError(
        "vertices_move", Json{{"vertices", {{0, 0, 0}}}, {"vector", {0, 0, 16}}});
      CHECK(hidden.code == ErrorCode::ObjectNotEditable);
      CHECK(
        hidden.message.find("the 2 visible, unlocked brushes of the whole map")
        != std::string::npos);
      CHECK(hidden.objectIds == std::vector<std::string>{aId});
      CHECK(hidden.hint.find("layer_set_state") != std::string::npos);
    }

    SECTION("non-convex, out of bounds and unknown handles")
    {
      const auto nonConvex = fixture.callExpectingError(
        "vertices_move",
        Json{{"ids", {aId}}, {"vertices", {{64, 64, 64}}}, {"vector", {-96, -32, -48}}});
      CHECK(nonConvex.code == ErrorCode::InvalidGeometry);
      CHECK(nonConvex.objectIds == std::vector<std::string>{aId});
      CHECK(nonConvex.message.find(aId) != std::string::npos);
      CHECK(nonConvex.hint.find("smaller vector") != std::string::npos);

      const auto outside = fixture.callExpectingError(
        "vertices_move",
        Json{{"vertices", {{0, 0, 0}}}, {"vector", {0, 0, worldBounds.min.z() - 64}}});
      CHECK(outside.code == ErrorCode::OutOfWorldBounds);
      CHECK(outside.objectIds == std::vector<std::string>{aId});

      const auto unknown = fixture.callExpectingError(
        "vertices_move", Json{{"vertices", {{1, 2, 3}}}, {"vector", {0, 0, 16}}});
      CHECK(unknown.code == ErrorCode::InvalidArgument);
      CHECK(unknown.message.find("[1.0,2.0,3.0]") != std::string::npos);

      CHECK(
        fixture
          .callExpectingError(
            "vertices_move",
            Json{
              {"vertices", {{0, 0, 0}}},
              {"edges", {{{64, 0, 64}, {64, 64, 64}}}},
              {"vector", {0, 0, 16}},
            })
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "vertices_move", Json{{"vertices", {{0, 0, 0}}}, {"vector", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "vertices_move",
            Json{{"ids", {entityId}}, {"vertices", {{0, 0, 0}}}, {"vector", {0, 0, 8}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("vertex_add")
  {
    mdl::selectNodes(map, {far});
    const auto result =
      fixture.call("vertex_add", Json{{"brush", aId}, {"position", {32, 32, 96}}});
    CHECK(result["undoStep"] == "AI: Add Vertex");
    CHECK(resultOf(result)["vertexCount"] == 9);
    CHECK(a->logicalBounds().max.z() == 96);
    CHECK(selectedIds(fixture, map) == std::vector<std::string>{farId});

    const auto inside = fixture.callExpectingError(
      "vertex_add", Json{{"brush", bId}, {"position", {96, 32, 32}}});
    CHECK(inside.code == ErrorCode::InvalidGeometry);
    CHECK(inside.objectIds == std::vector<std::string>{bId});

    CHECK(
      fixture
        .callExpectingError(
          "vertex_add",
          Json{{"brush", bId}, {"position", {96, 32, worldBounds.max.z() + 8}}})
        .code
      == ErrorCode::OutOfWorldBounds);
    CHECK(
      fixture
        .callExpectingError(
          "vertex_add", Json{{"brush", bId}, {"position", {128, 64, 64}}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "vertex_add", Json{{"brush", entityId}, {"position", {0, 0, 128}}})
        .code
      == ErrorCode::InvalidArgument);

    fixture.call(
      "vertex_add", Json{{"brush", bId}, {"position", {96, 32, 96}}, {"dryRun", true}});
    CHECK(b->brush().vertexCount() == 8);
  }

  SECTION("vertices_remove")
  {
    const auto result =
      fixture.call("vertices_remove", Json{{"ids", {aId}}, {"vertices", {{64, 64, 64}}}});
    CHECK(result["undoStep"] == "AI: Remove Vertices");
    CHECK(a->brush().vertexCount() == 7);
    CHECK(b->brush().vertexCount() == 8);

    const auto degenerate = fixture.callExpectingError(
      "vertices_remove",
      Json{
        {"ids", {bId}},
        {"faces", {{{64, 0, 64}, {128, 0, 64}, {128, 64, 64}, {64, 64, 64}}}}});
    CHECK(degenerate.code == ErrorCode::InvalidGeometry);
    CHECK(degenerate.objectIds == std::vector<std::string>{bId});

    fixture.call(
      "vertices_remove",
      Json{{"edges", {{{256, 0, 64}, {256, 64, 64}}}}, {"dryRun", true}});
    CHECK(far->brush().vertexCount() == 8);

    // a vertex of an unselected brush
    mdl::selectNodes(map, {far});
    const auto outside = fixture.call("vertices_remove", Json{{"vertices", {{0, 0, 0}}}});
    CHECK(hasWarning(outside, "HANDLES_OUTSIDE_SELECTION"));
    CHECK(a->brush().vertexCount() == 6);
    CHECK(far->brush().vertexCount() == 8);

    const auto unknown = fixture.callExpectingError(
      "vertices_remove", Json{{"ids", {bId}}, {"vertices", {{256, 0, 0}}}});
    CHECK(unknown.code == ErrorCode::InvalidArgument);
    CHECK(unknown.objectIds == std::vector<std::string>{farId});
  }

  SECTION("vertices_snap")
  {
    auto* odd = addBrush(map, {{0.5, 0.25, 128}, {64.5, 64, 192.75}});
    const auto oddId = fixture.id(*odd);
    REQUIRE(!hasIntegerVertices(*odd));

    mdl::selectNodes(map, {odd});
    const auto result = fixture.call("vertices_snap", Json{{"mode", "integer"}});
    CHECK(result["undoStep"] == "AI: Snap Vertices");
    CHECK(resultOf(result)["snapTo"] == 1.0);
    CHECK(resultOf(result)["snapped"] == Json{oddId});
    CHECK(hasIntegerVertices(*odd));

    const auto grid =
      fixture.call("vertices_snap", Json{{"ids", {oddId}}, {"dryRun", true}});
    CHECK(resultOf(grid)["snapTo"] == map.grid().actualSize());

    auto* small = addBrush(map, {{0, 0, 256}, {8, 8, 264}});
    const auto degenerate = fixture.callExpectingError(
      "vertices_snap", Json{{"ids", {fixture.id(*small)}}, {"snapTo", 64}});
    CHECK(degenerate.code == ErrorCode::InvalidGeometry);

    CHECK(
      fixture
        .callExpectingError(
          "vertices_snap", Json{{"ids", {aId}}, {"mode", "grid"}, {"snapTo", 8}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("csg_merge")
  {
    SECTION("brushes")
    {
      const auto result = fixture.call("csg_merge", Json{{"ids", {aId, farId}}});
      CHECK(result["undoStep"] == "AI: CSG Convex Merge");
      const auto& merged = resultOf(result)["brush"];
      CHECK(boundsOf(merged) == vm::bbox3d{{0, 0, 0}, {320, 64, 64}});
      CHECK(fixture.node(aId) == nullptr);
      CHECK(fixture.node(farId) == nullptr);
      CHECK(
        selectedIds(fixture, map)
        == std::vector<std::string>{merged["id"].get<std::string>()});
    }

    SECTION("faces keep the brushes")
    {
      const auto farBottom = faceIdWithNormal(fixture, *far, {0, 0, -1});
      const auto result =
        fixture.call("csg_merge", Json{{"faces", {aTop, farBottom}}, {"dryRun", true}});
      CHECK(boundsOf(resultOf(result)["brush"]) == vm::bbox3d{{0, 0, 0}, {320, 64, 64}});
      CHECK(result["changes"]["removed"].empty());
    }

    SECTION("errors")
    {
      CHECK(
        fixture.callExpectingError("csg_merge", Json{{"ids", {aId}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("csg_merge", Json{{"ids", {aId, entityId}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.callExpectingError("csg_merge").code == ErrorCode::NoSelection);

      // coplanar faces do not span a volume
      const auto farTop = faceIdWithNormal(fixture, *far, {0, 0, 1});
      const auto flat =
        fixture.callExpectingError("csg_merge", Json{{"faces", {aTop, farTop}}});
      CHECK(flat.code == ErrorCode::InvalidGeometry);
      CHECK(contains(flat.objectIds, aTop));
    }
  }

  SECTION("csg_subtract")
  {
    auto* cutter = addBrush(map, {{32, 32, 32}, {96, 96, 96}});
    const auto cutterId = fixture.id(*cutter);

    SECTION("cuts touching brushes and removes the cutters")
    {
      const auto result = fixture.call("csg_subtract", Json{{"ids", {cutterId}}});
      CHECK(result["undoStep"] == "AI: CSG Subtract");
      const auto& subtract = resultOf(result);
      CHECK(subtract["cutters"] == Json{cutterId});
      CHECK(subtract["cut"].size() == 2);
      CHECK(contains(subtract["cut"].get<std::vector<std::string>>(), aId));
      CHECK(contains(subtract["cut"].get<std::vector<std::string>>(), bId));
      CHECK(!subtract["fragments"].empty());
      CHECK(fixture.node(cutterId) == nullptr);
      CHECK(fixture.node(aId) == nullptr);
      CHECK(map.selection().nodes.size() == subtract["fragments"].size());
    }

    SECTION("cutters touching nothing are removed with a warning")
    {
      auto* lonely = addBrush(map, {{1024, 1024, 1024}, {1088, 1088, 1088}});
      mdl::selectNodes(map, {lonely});
      const auto result = fixture.call("csg_subtract");
      CHECK(hasWarning(result, "NOTHING_SUBTRACTED"));
      CHECK(resultOf(result)["cut"].empty());
    }

    SECTION("dry run")
    {
      fixture.call("csg_subtract", Json{{"ids", {cutterId}}, {"dryRun", true}});
      CHECK(fixture.node(cutterId) == cutter);
      CHECK(fixture.node(aId) == a);
    }

    CHECK(
      fixture.callExpectingError("csg_subtract", Json{{"ids", {entityId}}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("csg_intersect")
  {
    auto* overlap = addBrush(map, {{32, 32, 32}, {96, 96, 96}});
    const auto overlapId = fixture.id(*overlap);

    const auto result = fixture.call("csg_intersect", Json{{"ids", {aId, overlapId}}});
    CHECK(result["undoStep"] == "AI: CSG Intersect");
    CHECK(boundsOf(resultOf(result)["brush"]) == vm::bbox3d{{32, 32, 32}, {64, 64, 64}});
    CHECK(fixture.node(aId) == nullptr);

    const auto disjoint = fixture.call("csg_intersect", Json{{"ids", {bId, farId}}});
    CHECK(resultOf(disjoint)["brush"].is_null());
    CHECK(hasWarning(disjoint, "EMPTY_INTERSECTION"));
    CHECK(fixture.node(bId) == nullptr);

    CHECK(
      fixture
        .callExpectingError(
          "csg_intersect", Json{{"ids", {resultOf(result)["brush"]["id"]}}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("csg_hollow")
  {
    mdl::selectNodes(map, {far});
    const auto result =
      fixture.call("csg_hollow", Json{{"ids", {aId, bId}}, {"thickness", 8}});
    CHECK(result["undoStep"] == "AI: CSG Hollow");
    const auto& hollow = resultOf(result);
    CHECK(hollow["thickness"] == 8.0);
    CHECK(hollow["walls"].size() == 12);
    CHECK(hollow["notHollowed"].empty());
    for (const auto& wall : hollow["walls"])
    {
      const auto size = boundsOf(wall).size();
      CHECK(vm::get_max_component(size, 2) == 8.0);
    }
    CHECK(map.selection().nodes.size() == 12);

    const auto partial =
      fixture.call("csg_hollow", Json{{"ids", {farId}}, {"dryRun", true}});
    CHECK(resultOf(partial)["thickness"] == map.grid().actualSize());

    auto* small = addBrush(map, {{0, 0, 256}, {16, 16, 272}});
    const auto smallId = fixture.id(*small);
    const auto mixed = fixture.call(
      "csg_hollow", Json{{"ids", {farId, smallId}}, {"thickness", 8}, {"dryRun", true}});
    CHECK(resultOf(mixed)["notHollowed"] == Json{smallId});
    CHECK(hasWarning(mixed, "NOT_HOLLOWED"));

    const auto tooThick =
      fixture.callExpectingError("csg_hollow", Json{{"ids", {farId}}, {"thickness", 32}});
    CHECK(tooThick.code == ErrorCode::InvalidGeometry);
    CHECK(tooThick.objectIds == std::vector<std::string>{farId});

    CHECK(
      fixture.callExpectingError("csg_hollow", Json{{"ids", {farId}}, {"thickness", 0}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("csg_hollow", Json{{"ids", {entityId}}}).code
      == ErrorCode::InvalidArgument);

    // explicit ids of the wrong kind are rejected by the schema, a selection with the
    // wrong kinds by the tool
    mdl::deselectAll(map);
    mdl::selectNodes(map, {far, entity});
    const auto wrongKind = fixture.callExpectingError("csg_hollow");
    CHECK(wrongKind.code == ErrorCode::WrongObjectKind);
    CHECK(wrongKind.objectIds == std::vector<std::string>{entityId});
  }
}

} // namespace tb::mcp
