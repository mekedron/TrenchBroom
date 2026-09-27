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

#include "TestEnvironment.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
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
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

ui::MapDocument& newDocument(McpToolFixture& fixture, const std::string& game)
{
  const auto result = fixture.call("document_new", Json{{"game", game}});
  const auto id = resultOf(result)["document"]["id"].get<std::string>();
  for (const auto& info : fixture.host().documentList)
  {
    if (info.id == id)
    {
      return *info.document;
    }
  }
  FAIL("document not found");
  return *fixture.host().documentList.front().document;
}

mdl::BrushNode* addBrush(
  mdl::Map& map, const vm::bbox3d& bounds, const std::string& material = "material")
{
  const auto builder = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto* brushNode =
    new mdl::BrushNode{builder.createCuboid(bounds, material) | kdl::value()};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  return brushNode;
}

mdl::BrushFaceHandle faceWithNormal(mdl::BrushNode& brushNode, const vm::vec3d& normal)
{
  const auto faceIndex = brushNode.brush().findFace(normal);
  REQUIRE(faceIndex);
  return mdl::BrushFaceHandle{&brushNode, *faceIndex};
}

std::string faceId(
  McpToolFixture& fixture, mdl::BrushNode& brushNode, const vm::vec3d& normal)
{
  return fixture.id(brushNode)
         + "/face:" + std::to_string(faceWithNormal(brushNode, normal).faceIndex());
}

const mdl::BrushFace& face(mdl::BrushNode& brushNode, const vm::vec3d& normal)
{
  return faceWithNormal(brushNode, normal).face();
}

bool hasWarning(const Json& result, const std::string& code)
{
  return result.contains("warnings")
         && std::ranges::any_of(result["warnings"], [&](const auto& warning) {
              return warning["code"] == code;
            });
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

/** The UV coordinates (in texels, without wrapping) of a point on the face. */
vm::vec2f uvAt(const mdl::BrushFace& face, const vm::vec3d& point)
{
  const auto uv = face.uvAttributes();
  return vm::vec2f{face.toUvCoordSystemMatrix(uv.offset, uv.scale) * point};
}

/** The UV coordinates of the face's vertices: the minimum and maximum per axis. */
std::pair<vm::vec2f, vm::vec2f> uvBounds(const mdl::BrushFace& face)
{
  auto min = vm::vec2f::fill(1e9f);
  auto max = vm::vec2f::fill(-1e9f);
  for (const auto& position : face.vertexPositions())
  {
    const auto uv = uvAt(face, position);
    min = vm::min(min, uv);
    max = vm::max(max, uv);
  }
  return {min, max};
}

bool isMultipleOf(const float value, const float size)
{
  const auto remainder = std::fmod(std::abs(value), size);
  return remainder < 0.01f || remainder > size - 0.01f;
}

} // namespace

TEST_CASE("FaceTools")
{
  auto fixture = McpToolFixture{};

  SECTION("face_attributes_get")
  {
    SECTION("Standard format: brush ids, selection and pagination")
    {
      auto& map = fixture.create().map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      auto* other = addBrush(map, {{128, 0, 0}, {192, 64, 64}});
      const auto brushId = fixture.id(*brush);

      const auto result = fixture.call("face_attributes_get", Json{{"ids", {brushId}}});
      CHECK(result["total"] == 6);
      CHECK(result["format"]["mapFormat"] == "Standard");
      CHECK(result["format"]["uvFormat"] == "standard");
      CHECK(result["format"]["storesSurfaceAttributes"] == false);
      CHECK(result["format"]["storesColor"] == false);

      const auto& item = result["items"][0];
      CHECK(item["brush"] == brushId);
      CHECK(item["material"] == "material");
      CHECK(item["materialSize"].is_null());
      CHECK(item["offset"] == Json{0, 0});
      CHECK(item["scale"] == Json{1, 1});
      CHECK(item["rotation"] == 0);
      CHECK(item.contains("uAxis"));
      CHECK(item.contains("vAxis"));
      CHECK(!item.contains("surfaceFlags"));
      CHECK(!item.contains("normal"));

      // one face id, full detail
      const auto topId = faceId(fixture, *brush, {0, 0, 1});
      const auto full =
        fixture.call("face_attributes_get", Json{{"ids", {topId}}, {"detail", "full"}});
      CHECK(full["total"] == 1);
      CHECK(full["items"][0]["id"] == topId);
      CHECK(full["items"][0]["normal"] == Json{0, 0, 1});
      CHECK(full["items"][0]["vertices"].size() == 4);

      // the selected objects
      mdl::selectNodes(map, {other});
      const auto selected = fixture.call("face_attributes_get");
      CHECK(selected["total"] == 6);
      CHECK(selected["items"][0]["brush"] == fixture.id(*other));

      // the selected faces
      mdl::deselectAll(map);
      mdl::selectBrushFaces(map, {faceWithNormal(*brush, {1, 0, 0})});
      const auto selectedFaces = fixture.call("face_attributes_get");
      CHECK(selectedFaces["total"] == 1);
      CHECK(selectedFaces["items"][0]["id"] == faceId(fixture, *brush, {1, 0, 0}));

      // pagination
      const auto page =
        fixture.call("face_attributes_get", Json{{"ids", {brushId}}, {"limit", 4}});
      CHECK(page["items"].size() == 4);
      CHECK(page["total"] == 6);
      CHECK(page["nextCursor"].is_string());
    }

    SECTION("Valve format reports the UV axes")
    {
      auto& map = fixture.create({.mapFormat = mdl::MapFormat::Valve}).map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      const auto result = fixture.call(
        "face_attributes_get", Json{{"ids", {faceId(fixture, *brush, {0, 0, 1})}}});
      CHECK(result["format"]["uvFormat"] == "valve220");
      const auto& item = result["items"][0];
      CHECK(item["uAxis"] == Json{1, 0, 0});
      CHECK(item["vAxis"] == Json{0, -1, 0});
    }

    SECTION("Quake 2 flags by name and material defaults")
    {
      auto& map = newDocument(fixture, "Quake 2").map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "lavatest");
      const auto result = fixture.call(
        "face_attributes_get", Json{{"ids", {faceId(fixture, *brush, {0, 0, 1})}}});
      CHECK(result["format"]["mapFormat"] == "Quake2");
      CHECK(result["format"]["storesSurfaceAttributes"] == true);
      CHECK(!result["format"]["surfaceFlags"].empty());
      CHECK(!result["format"]["contentFlags"].empty());

      const auto& item = result["items"][0];
      CHECK(item["materialSize"] == Json{64, 64});
      CHECK(item["surfaceFlags"]["bits"] == 9);
      CHECK(item["surfaceFlags"]["names"] == Json{"light", "warp"});
      CHECK(item["surfaceFlags"]["unknownBits"] == Json::array());
      CHECK(item["surfaceFlags"]["fromMaterial"] == true);
      CHECK(item["contentFlags"]["names"] == Json{"lava"});
      CHECK(item["surfaceValue"] == 700);
      CHECK(item["surfaceValueFromMaterial"] == true);
    }

    SECTION("invalid input")
    {
      auto& map = fixture.create().map();
      addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      auto* entity = new mdl::EntityNode{mdl::Entity{{{"classname", "info_null"}}}};
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {entity}}});
      const auto entityId = fixture.id(*entity);
      CHECK(
        fixture.callExpectingError("face_attributes_get").code == ErrorCode::NoSelection);
      CHECK(
        fixture.callExpectingError("face_attributes_get", Json{{"ids", {"brush:999999"}}})
          .code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("face_attributes_get", Json{{"ids", {entityId}}}).code
        == ErrorCode::WrongObjectKind);
    }
  }

  SECTION("face_attributes_set")
  {
    SECTION("Quake 2: alignment, flags by name, value")
    {
      auto& map = newDocument(fixture, "Quake 2").map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "lavatest");
      auto* other = addBrush(map, {{128, 0, 0}, {192, 64, 64}}, "lavatest");
      const auto topId = faceId(fixture, *brush, {0, 0, 1});
      mdl::selectNodes(map, {other});

      const auto result = fixture.call(
        "face_attributes_set",
        Json{
          {"ids", {topId}},
          {"offset", {8, 4}},
          {"scale", {0.5, 2}},
          {"rotation", 30},
          {"surfaceFlags", {{"add", {"light", "SLICK"}}}},
          {"contentFlags", {{"set", {"water", "detail"}}}},
          {"surfaceValue", 300},
        });
      CHECK(result["undoStep"] == "AI: Set Face Attributes");
      CHECK(result["changes"]["modified"] == Json{fixture.id(*brush)});
      CHECK(resultOf(result)["count"] == 1);
      CHECK(
        resultOf(result)["faces"][0]["surfaceFlags"]["names"]
        == Json{"light", "slick", "warp"});
      CHECK(!hasWarning(result, "ATTRIBUTE_NOT_SAVED"));

      const auto& top = face(*brush, {0, 0, 1});
      CHECK(top.uvAttributes().offset == vm::vec2f{8, 4});
      CHECK(top.uvAttributes().scale == vm::vec2f{0.5f, 2});
      CHECK(top.uvAttributes().rotation == Catch::Approx(30));
      CHECK(top.surfaceAttributes().flags == 11); // light | slick | warp (material)
      CHECK(top.surfaceAttributes().contents == (32 | 134217728));
      CHECK(top.surfaceAttributes().value == 300);

      // other faces are unchanged and the selection is restored
      CHECK(face(*brush, {0, 0, -1}).uvAttributes().scale == vm::vec2f{1, 1});
      CHECK(selectedIds(fixture, map) == std::vector{fixture.id(*other)});

      SECTION("add and remove, raw bits, unknown names")
      {
        const auto changed = fixture.call(
          "face_attributes_set",
          Json{
            {"ids", {topId}},
            {"surfaceFlags", {{"add", {256, "nonsense"}}, {"remove", {"light"}}}},
          });
        CHECK(hasWarning(changed, "UNKNOWN_FLAG"));
        CHECK(face(*brush, {0, 0, 1}).surfaceAttributes().flags == (2 | 8 | 256));
      }

      SECTION("relative changes")
      {
        fixture.call(
          "face_attributes_set",
          Json{
            {"ids", {topId}},
            {"offsetBy", {2, -4}},
            {"scaleBy", {2, 0.5}},
            {"rotateBy", 15},
          });
        const auto& changed = face(*brush, {0, 0, 1});
        CHECK(changed.uvAttributes().offset == vm::vec2f{10, 0});
        CHECK(changed.uvAttributes().scale == vm::vec2f{1, 1});
        CHECK(changed.uvAttributes().rotation == Catch::Approx(45));
      }

      SECTION("unset returns to the material's defaults")
      {
        const auto changed = fixture.call(
          "face_attributes_set",
          Json{{"ids", {topId}}, {"unset", {"surfaceFlags", "surfaceValue"}}});
        const auto& top2 = face(*brush, {0, 0, 1});
        CHECK(!top2.surfaceAttributes().flags);
        CHECK(!top2.surfaceAttributes().value);
        CHECK(top2.resolvedSurfaceFlags() == 9);
        CHECK(resultOf(changed)["faces"][0]["surfaceFlags"]["fromMaterial"] == true);
      }

      SECTION("material, selected faces")
      {
        mdl::deselectAll(map);
        mdl::selectBrushFaces(map, {faceWithNormal(*other, {1, 0, 0})});
        const auto changed =
          fixture.call("face_attributes_set", Json{{"material", "no_such_material"}});
        CHECK(hasWarning(changed, "UNKNOWN_MATERIAL"));
        CHECK(resultOf(changed)["count"] == 1);
        CHECK(face(*other, {1, 0, 0}).materialName() == "no_such_material");
        CHECK(face(*other, {-1, 0, 0}).materialName() == "lavatest");
      }
    }

    SECTION("Standard format warns about attributes it does not store")
    {
      auto& map = fixture.create().map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      const auto result = fixture.call(
        "face_attributes_set",
        Json{
          {"ids", {fixture.id(*brush)}},
          {"surfaceFlags", {{"set", {4}}}},
          {"color", {10, 20, 30}},
        });
      CHECK(hasWarning(result, "ATTRIBUTE_NOT_SAVED"));
      CHECK(resultOf(result)["count"] == 6);
      CHECK(resultOf(result)["faces"][0]["color"] == Json{10, 20, 30});
      CHECK(face(*brush, {0, 0, 1}).surfaceAttributes().flags == 4);

      fixture.call(
        "face_attributes_set", Json{{"ids", {fixture.id(*brush)}}, {"unset", {"color"}}});
      CHECK(!face(*brush, {0, 0, 1}).surfaceAttributes().color);
    }

    SECTION("dry run")
    {
      auto& map = fixture.create().map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      const auto result = fixture.call(
        "face_attributes_set",
        Json{{"ids", {fixture.id(*brush)}}, {"scale", {2, 2}}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(resultOf(result)["faces"][0]["scale"] == Json{2, 2});
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().scale == vm::vec2f{1, 1});
    }

    SECTION("invalid input")
    {
      auto& map = fixture.create().map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      const auto ids = Json{fixture.id(*brush)};
      const auto error = [&](Json arguments) {
        arguments["ids"] = ids;
        return fixture.callExpectingError("face_attributes_set", arguments).code;
      };
      CHECK(error(Json::object()) == ErrorCode::InvalidArgument);
      CHECK(
        error(Json{{"offset", {0, 0}}, {"offsetBy", {1, 1}}})
        == ErrorCode::InvalidArgument);
      CHECK(error(Json{{"scale", {0, 1}}}) == ErrorCode::InvalidArgument);
      CHECK(
        error(Json{{"surfaceFlags", {{"set", {1}}, {"add", {2}}}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        error(Json{{"surfaceFlags", {{"add", {1}}, {"remove", {1}}}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        error(Json{{"surfaceValue", 1}, {"unset", {"surfaceValue"}}})
        == ErrorCode::InvalidArgument);
      CHECK(error(Json{{"color", {1, 2}}}) == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("face_attributes_set", Json{{"rotation", 10}}).code
        == ErrorCode::NoSelection);
    }
  }

  SECTION("face_attributes_copy")
  {
    SECTION("Valve format: project, rotate and material")
    {
      auto& map = fixture.create({.mapFormat = mdl::MapFormat::Valve}).map();
      auto* a = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "source_mat");
      auto* b = addBrush(map, {{64, 0, 0}, {128, 64, 64}}, "target_mat");
      const auto sourceId = faceId(fixture, *a, {0, 0, 1});
      fixture.call(
        "face_attributes_set",
        Json{
          {"ids", {sourceId}}, {"offset", {5, 7}}, {"scale", {2, 2}}, {"rotation", 30}});
      mdl::selectNodes(map, {a});

      SECTION("project to a coplanar face continues the texture")
      {
        const auto targetId = faceId(fixture, *b, {0, 0, 1});
        const auto result = fixture.call(
          "face_attributes_copy", Json{{"source", sourceId}, {"ids", {targetId}}});
        CHECK(resultOf(result)["count"] == 1);
        CHECK(resultOf(result)["mode"] == "project");

        const auto& source = face(*a, {0, 0, 1});
        const auto& target = face(*b, {0, 0, 1});
        CHECK(target.materialName() == "source_mat");
        CHECK(target.uvAttributes().scale == vm::vec2f{2, 2});
        CHECK(target.uvAttributes().rotation == Catch::Approx(30));
        const auto point = vm::vec3d{64, 32, 64};
        CHECK(vm::is_equal(uvAt(target, point), uvAt(source, point), 0.01f));
        CHECK(selectedIds(fixture, map) == std::vector{fixture.id(*a)});
      }

      SECTION("rotate wraps around the edge")
      {
        // the side face of the source brush shares the edge x = 64, z = 64
        const auto targetId = faceId(fixture, *a, {1, 0, 0});
        const auto result = fixture.call(
          "face_attributes_copy",
          Json{{"source", sourceId}, {"ids", {targetId}}, {"mode", "rotate"}});
        CHECK(!hasWarning(result, "ROTATION_NEEDS_VALVE_FORMAT"));
        const auto& source = face(*a, {0, 0, 1});
        const auto& target = face(*a, {1, 0, 0});
        CHECK(target.materialName() == "source_mat");
        // the texture continues across the edge; without a loaded material, offsets
        // wrap at 1 texel, so the coordinates agree up to whole texels
        const auto d1 = uvAt(target, {64, 0, 64}) - uvAt(source, {64, 0, 64});
        const auto d2 = uvAt(target, {64, 64, 64}) - uvAt(source, {64, 64, 64});
        CHECK(vm::is_equal(d1, d2, 0.01f));
        CHECK(isMultipleOf(d1.x(), 1.0f));
        CHECK(isMultipleOf(d1.y(), 1.0f));
      }

      SECTION("material only")
      {
        const auto result = fixture.call(
          "face_attributes_copy",
          Json{{"source", sourceId}, {"ids", {fixture.id(*b)}}, {"mode", "material"}});
        CHECK(resultOf(result)["count"] == 6);
        CHECK(face(*b, {0, 0, 1}).materialName() == "source_mat");
        CHECK(face(*b, {0, 0, 1}).uvAttributes().scale == vm::vec2f{1, 1});
      }

      SECTION("the source is not a target; the selection is the default")
      {
        mdl::selectNodes(map, {a, b});
        const auto result =
          fixture.call("face_attributes_copy", Json{{"source", sourceId}});
        CHECK(resultOf(result)["count"] == 11);
        CHECK(face(*a, {0, 0, -1}).materialName() == "source_mat");
        CHECK(face(*b, {0, 0, -1}).materialName() == "source_mat");
      }

      SECTION("dry run")
      {
        const auto result = fixture.call(
          "face_attributes_copy",
          Json{{"source", sourceId}, {"ids", {fixture.id(*b)}}, {"dryRun", true}});
        CHECK(result["dryRun"] == true);
        CHECK(face(*b, {0, 0, 1}).materialName() == "target_mat");
      }

      SECTION("invalid input")
      {
        CHECK(
          fixture
            .callExpectingError(
              "face_attributes_copy", Json{{"source", sourceId}, {"ids", {sourceId}}})
            .code
          == ErrorCode::InvalidArgument);
        CHECK(
          fixture
            .callExpectingError(
              "face_attributes_copy",
              Json{{"source", fixture.id(*a) + "/face:99"}, {"ids", {fixture.id(*b)}}})
            .code
          != ErrorCode::InternalError);
        CHECK(
          fixture.callExpectingError("face_attributes_copy", Json{{"ids", {sourceId}}})
            .code
          == ErrorCode::InvalidArgument);
      }
    }

    SECTION("Standard format warns for rotate")
    {
      auto& map = fixture.create().map();
      auto* a = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "source_mat");
      auto* b = addBrush(map, {{64, 0, 0}, {128, 64, 64}}, "target_mat");
      const auto result = fixture.call(
        "face_attributes_copy",
        Json{
          {"source", faceId(fixture, *a, {0, 0, 1})},
          {"ids", {faceId(fixture, *b, {1, 0, 0})}},
          {"mode", "rotate"},
        });
      CHECK(hasWarning(result, "ROTATION_NEEDS_VALVE_FORMAT"));
      CHECK(face(*b, {1, 0, 0}).materialName() == "source_mat");
    }

    SECTION("Quake 2 content flags are copied on request")
    {
      auto& map = newDocument(fixture, "Quake 2").map();
      auto* a = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "b_pv_v1a1");
      auto* b = addBrush(map, {{64, 0, 0}, {128, 64, 64}}, "b_pv_v1a1");
      const auto sourceId = faceId(fixture, *a, {0, 0, 1});
      const auto targetId = faceId(fixture, *b, {0, 0, 1});
      fixture.call(
        "face_attributes_set",
        Json{
          {"ids", {sourceId}},
          {"surfaceFlags", {{"set", {"light"}}}},
          {"contentFlags", {{"set", {"water"}}}},
        });

      fixture.call(
        "face_attributes_copy", Json{{"source", sourceId}, {"ids", {targetId}}});
      CHECK(face(*b, {0, 0, 1}).surfaceAttributes().flags == 1);
      CHECK(!face(*b, {0, 0, 1}).surfaceAttributes().contents);

      fixture.call(
        "face_attributes_copy",
        Json{{"source", sourceId}, {"ids", {targetId}}, {"contentFlags", true}});
      CHECK(face(*b, {0, 0, 1}).surfaceAttributes().contents == 32);
    }
  }

  SECTION("uv_align")
  {
    SECTION("fit N x M with a loaded material")
    {
      auto& map = newDocument(fixture, "Quake 2").map();
      // b_pv_v1a1 is 128 x 256 texels
      auto* brush = addBrush(map, {{0, 0, 0}, {128, 128, 64}}, "b_pv_v1a1");
      const auto topId = faceId(fixture, *brush, {0, 0, 1});
      fixture.call("face_attributes_set", Json{{"ids", {topId}}, {"offset", {13, 17}}});

      const auto result = fixture.call(
        "uv_align",
        Json{{"ids", {topId}}, {"operation", "fit"}, {"repeatU", 2}, {"repeatV", 3}});
      CHECK(result["undoStep"] == "AI: Align UV");
      CHECK(resultOf(result)["operation"] == "fit");

      const auto& top = face(*brush, {0, 0, 1});
      CHECK(top.uvAttributes().scale.x() == Catch::Approx(0.5f));
      CHECK(top.uvAttributes().scale.y() == Catch::Approx(128.0f / (3.0f * 256.0f)));
      const auto [min, max] = uvBounds(top);
      CHECK(max.x() - min.x() == Catch::Approx(256.0f));
      CHECK(max.y() - min.y() == Catch::Approx(768.0f));
      CHECK(isMultipleOf(min.x(), 128.0f));
      CHECK(isMultipleOf(min.y(), 256.0f));

      SECTION("one axis only")
      {
        fixture.call(
          "uv_align", Json{{"ids", {topId}}, {"operation", "fit"}, {"repeatV", 1}});
        const auto& changed = face(*brush, {0, 0, 1});
        CHECK(changed.uvAttributes().scale.x() == Catch::Approx(0.5f));
        CHECK(changed.uvAttributes().scale.y() == Catch::Approx(0.5f));
      }

      SECTION("center")
      {
        fixture.call(
          "uv_align",
          Json{{"ids", {topId}}, {"operation", "justify"}, {"edge", "center"}});
        const auto& changed = face(*brush, {0, 0, 1});
        const auto center = uvAt(changed, {64, 64, 64});
        CHECK(isMultipleOf(center.x() - 64.0f, 128.0f));
        CHECK(isMultipleOf(center.y() - 128.0f, 256.0f));
      }

      SECTION("justify, align, autoFit and trim sheet fit run the editor's operations")
      {
        for (const auto& arguments :
             {Json{{"operation", "justify"}, {"edge", "left"}, {"policy", "next"}},
              Json{{"operation", "align"}},
              Json{{"operation", "autoFit"}},
              Json{{"operation", "fit"}, {"trimSheet", true}}})
        {
          auto withIds = arguments;
          withIds["ids"] = Json{topId};
          const auto aligned = fixture.call("uv_align", withIds);
          CHECK(resultOf(aligned)["count"] == 1);
        }
        const auto [min2, max2] = uvBounds(face(*brush, {0, 0, 1}));
        CHECK(max2.x() - min2.x() > 0.0f);
      }
    }

    SECTION("fit skips faces whose material is not loaded")
    {
      auto& map = fixture.create().map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      const auto result = fixture.call(
        "uv_align", Json{{"ids", {fixture.id(*brush)}}, {"operation", "fit"}});
      CHECK(hasWarning(result, "MATERIAL_NOT_LOADED"));
      CHECK(resultOf(result)["count"] == 0);
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().scale == vm::vec2f{1, 1});
    }

    SECTION("flip, rotate90, reset and resetToWorld in Valve format")
    {
      auto& map = fixture.create({.mapFormat = mdl::MapFormat::Valve}).map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      auto* other = addBrush(map, {{128, 0, 0}, {192, 64, 64}});
      const auto topId = faceId(fixture, *brush, {0, 0, 1});
      mdl::selectNodes(map, {other});

      fixture.call(
        "uv_align", Json{{"ids", {topId}}, {"operation", "flip"}, {"axis", "u"}});
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().scale == vm::vec2f{-1, 1});
      fixture.call(
        "uv_align", Json{{"ids", {topId}}, {"operation", "flip"}, {"axis", "v"}});
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().scale == vm::vec2f{-1, -1});

      fixture.call(
        "uv_align",
        Json{{"ids", {topId}}, {"operation", "rotate90"}, {"direction", "ccw"}});
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().rotation == Catch::Approx(90));
      CHECK(selectedIds(fixture, map) == std::vector{fixture.id(*other)});

      fixture.call("uv_align", Json{{"ids", {topId}}, {"operation", "reset"}});
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().rotation == Catch::Approx(0));
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().scale == vm::vec2f{1, 1});

      // a wall face: rotate the axes, then reset them to world axes
      const auto wallId = faceId(fixture, *brush, {1, 0, 0});
      fixture.call("face_attributes_set", Json{{"ids", {wallId}}, {"rotation", 45}});
      fixture.call("uv_align", Json{{"ids", {wallId}}, {"operation", "resetToWorld"}});
      const auto& wall = face(*brush, {1, 0, 0});
      CHECK(wall.uvAttributes().rotation == Catch::Approx(0));
      CHECK(vm::is_equal(wall.uAxis(), vm::vec3d{0, 1, 0}, 0.001));
    }

    SECTION("fit on copies made by objects_array keeps the Valve rotation")
    {
      // a Quake map in Valve 220 format with the materials of materials.wad
      auto& map = newDocument(fixture, "Quake").map();
      REQUIRE(map.worldNode().mapFormat() == mdl::MapFormat::Valve);
      const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad";
      fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
      auto gl = gl::TestGl{};
      gl::processResourcesSync(
        map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});

      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}}, "wall_old_a");
      REQUIRE(brush->brush().faces().front().material());

      const auto faceItems = [&](const std::string& brushId) {
        return fixture.call("face_attributes_get", Json{{"ids", {brushId}}})["items"];
      };
      const auto original = faceItems(fixture.id(*brush));

      const auto array = fixture.call(
        "objects_array",
        Json{
          {"ids", {fixture.id(*brush)}},
          {"pattern", "line"},
          {"count", 3},
          {"offset", {128, 0, 0}},
          {"alignmentLock", true},
        });
      const auto copyId = resultOf(array)["instances"][2][0].get<std::string>();
      const auto copied = faceItems(copyId);
      REQUIRE(copied.size() == original.size());
      for (size_t i = 0; i < copied.size(); ++i)
      {
        CHECK(copied[i]["rotation"] == original[i]["rotation"]);
      }

      const auto fitted =
        resultOf(fixture.call("uv_align", Json{{"ids", {copyId}}, {"operation", "fit"}}));
      REQUIRE(fitted["count"] == original.size());
      const auto after = faceItems(copyId);
      for (size_t i = 0; i < after.size(); ++i)
      {
        CAPTURE(i, original[i], fitted["faces"][i], after[i]);
        CHECK(fitted["faces"][i]["rotation"] == original[i]["rotation"]);
        CHECK(after[i]["rotation"] == original[i]["rotation"]);
        CHECK(after[i]["material"] == "wall_old_a");
        CHECK(after[i]["uAxis"] == copied[i]["uAxis"]);
        CHECK(after[i]["vAxis"] == copied[i]["vAxis"]);

        // the texture covers the face exactly once
        const auto faceHandle = after[i]["id"].get<std::string>();
        auto* copy = dynamic_cast<mdl::BrushNode*>(fixture.node(copyId));
        const auto& face = copy->brush().face(
          std::stoul(faceHandle.substr(faceHandle.find("/face:") + 6)));
        const auto [min, max] = uvBounds(face);
        CHECK(max.x() - min.x() == Catch::Approx(face.textureSize().x()));
        CHECK(max.y() - min.y() == Catch::Approx(face.textureSize().y()));
      }
    }

    SECTION("dry run, ignored and invalid arguments")
    {
      auto& map = fixture.create().map();
      auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
      const auto ids = Json{fixture.id(*brush)};

      const auto dryRun = fixture.call(
        "uv_align",
        Json{
          {"ids", ids},
          {"operation", "rotate90"},
          {"direction", "cw"},
          {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(face(*brush, {0, 0, 1}).uvAttributes().rotation == 0);

      const auto ignored = fixture.call(
        "uv_align",
        Json{{"ids", ids}, {"operation", "flip"}, {"axis", "u"}, {"edge", "left"}});
      CHECK(hasWarning(ignored, "IGNORED_ARGUMENT"));

      CHECK(
        fixture
          .callExpectingError("uv_align", Json{{"ids", ids}, {"operation", "justify"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("uv_align", Json{{"ids", ids}, {"operation", "flip"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("uv_align", Json{{"ids", ids}, {"operation", "spin"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("uv_align", Json{{"operation", "reset"}}).code
        == ErrorCode::NoSelection);
    }
  }

  SECTION("uv_nudge")
  {
    auto& map = fixture.create().map();
    auto* brush = addBrush(map, {{0, 0, 0}, {64, 64, 64}});
    const auto topId = faceId(fixture, *brush, {0, 0, 1});
    const auto gridSize = float(map.grid().actualSize());
    const auto gridAngle = float(vm::to_degrees(map.grid().angle()));
    const auto top = [&]() { return face(*brush, {0, 0, 1}).uvAttributes(); };

    SECTION("direction and distance")
    {
      fixture.call("uv_nudge", Json{{"ids", {topId}}, {"direction", "left"}});
      CHECK(top().offset == vm::vec2f{gridSize, 0});
      fixture.call(
        "uv_nudge", Json{{"ids", {topId}}, {"direction", "down"}, {"distance", 4}});
      CHECK(top().offset == vm::vec2f{gridSize, -4});

      // with a negative scale, the image still moves in the given direction
      fixture.call("face_attributes_set", Json{{"ids", {topId}}, {"scale", {-1, 1}}});
      fixture.call(
        "uv_nudge", Json{{"ids", {topId}}, {"direction", "right"}, {"distance", 2}});
      CHECK(top().offset == vm::vec2f{gridSize + 2, -4});
    }

    SECTION("offsetBy and rotate, the selection is the default")
    {
      mdl::selectBrushFaces(map, {faceWithNormal(*brush, {0, 0, 1})});
      const auto result =
        fixture.call("uv_nudge", Json{{"offsetBy", {3, 5}}, {"rotate", "ccw"}});
      CHECK(resultOf(result)["count"] == 1);
      CHECK(top().offset == vm::vec2f{3, 5});
      CHECK(top().rotation == Catch::Approx(gridAngle));

      fixture.call("uv_nudge", Json{{"rotate", "cw"}, {"angle", 5}});
      CHECK(top().rotation == Catch::Approx(gridAngle - 5));
      CHECK(map.selection().brushFaces.size() == 1);
    }

    SECTION("all faces of a brush")
    {
      const auto result = fixture.call(
        "uv_nudge", Json{{"ids", {fixture.id(*brush)}}, {"offsetBy", {1, 1}}});
      CHECK(resultOf(result)["count"] == 6);
      CHECK(face(*brush, {1, 0, 0}).uvAttributes().offset == vm::vec2f{1, 1});
    }

    SECTION("dry run, ignored and invalid arguments")
    {
      const auto dryRun = fixture.call(
        "uv_nudge", Json{{"ids", {topId}}, {"direction", "up"}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(top().offset == vm::vec2f{0, 0});

      const auto ignored = fixture.call(
        "uv_nudge", Json{{"ids", {topId}}, {"rotate", "cw"}, {"distance", 8}});
      CHECK(hasWarning(ignored, "IGNORED_ARGUMENT"));

      CHECK(
        fixture.callExpectingError("uv_nudge", Json{{"ids", {topId}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "uv_nudge", Json{{"ids", {topId}}, {"direction", "up"}, {"offsetBy", {1, 1}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("uv_nudge", Json{{"direction", "up"}}).code
        == ErrorCode::NoSelection);
    }
  }
}

} // namespace tb::mcp
