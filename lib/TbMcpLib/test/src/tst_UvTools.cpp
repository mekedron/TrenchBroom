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
#include "gl/MaterialManager.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/McpToolFixture.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/GameManager.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Selection.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
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

bool hasWarning(const Json& result, const std::string& code)
{
  return result.contains("warnings")
         && std::ranges::any_of(result["warnings"], [&](const auto& warning) {
              return warning["code"] == code;
            });
}

void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

void loadKnowledgeWad(McpToolFixture& fixture, mdl::Map& map)
{
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  processResources(map);
  REQUIRE(map.materialManager().materials().size() == 4);
}

/** A Quake document (Valve format) with the materials of knowledge.wad loaded. */
ui::MapDocument& knowledgeDocument(McpToolFixture& fixture)
{
  fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
  auto* document = fixture.host().documentList.back().document;
  loadKnowledgeWad(fixture, document->map());
  return *document;
}

/** Loads fixture/mcp/maps/uv_check.map with knowledge.wad. */
ui::MapDocument& uvCheckDocument(McpToolFixture& fixture)
{
  const auto* gameInfo = fixture.host().gameManager().gameInfo("Quake");
  REQUIRE(gameInfo);
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "uv_check.map",
    {.mapFormat = mdl::MapFormat::Valve, .gameInfo = *gameInfo});
  loadKnowledgeWad(fixture, document.map());
  return document;
}

std::vector<mdl::BrushNode*> brushesOf(mdl::Map& map)
{
  auto result = std::vector<mdl::BrushNode*>{};
  for (auto* node : map.worldNode().defaultLayer()->children())
  {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
    {
      result.push_back(brushNode);
    }
  }
  return result;
}

mdl::BrushNode* addCuboid(
  mdl::Map& map,
  const vm::bbox3d& box,
  const std::string& material,
  const mdl::UvAttributes& attributes = {})
{
  auto brush = brushBuilder(map).createCuboid(box, material).value();
  for (auto& face : brush.faces())
  {
    REQUIRE(face.setUvAttributes(attributes).is_success());
  }
  return static_cast<mdl::BrushNode*>(addBrushes(map, {std::move(brush)}).front());
}

mdl::BrushFaceHandle faceOf(mdl::BrushNode* brushNode, const vm::vec3d& normal)
{
  const auto index = brushNode->brush().findFace(normal);
  REQUIRE(index);
  return mdl::BrushFaceHandle{brushNode, *index};
}

std::string faceId(
  McpToolFixture& fixture, mdl::BrushNode* brushNode, const vm::vec3d& normal)
{
  return fixture.id(*brushNode)
         + "/face:" + std::to_string(faceOf(brushNode, normal).faceIndex());
}

std::vector<std::string> codesOf(const Json& items)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item["code"].get<std::string>());
  }
  return result;
}

vm::vec2d scaleOf(mdl::BrushNode* brushNode, const vm::vec3d& normal)
{
  return vm::vec2d{faceOf(brushNode, normal).face().uvAttributes().scale};
}

void setNote(McpToolFixture& fixture, Json note)
{
  fixture.call(
    "material_notes_set",
    Json{{"notes", Json::array({std::move(note)})}, {"scope", "game"}});
}

const auto Front = vm::vec3d{0, -1, 0};

} // namespace

TEST_CASE("UvTools")
{
  auto fixture = McpToolFixture{};

  SECTION("uv_check")
  {
    auto& document = uvCheckDocument(fixture);
    auto& map = document.map();
    const auto brushes = brushesOf(map);
    REQUIRE(brushes.size() == 2);
    auto* stretched = brushes[0];
    auto* panel = brushes[1];
    const auto panelFace = faceId(fixture, panel, Front);

    SECTION("finds a stretched texture and a fractional panel repeat in the map")
    {
      const auto result = fixture.call("uv_check", Json{{"scope", "map"}});
      CHECK(result["scope"] == "map");
      CHECK(result["facesChecked"] == 12);
      CHECK(result["total"] == 7);
      CHECK(result["counts"]["UV_ASPECT_DISTORTION"] == 6);
      CHECK(result["counts"]["UV_FRACTIONAL_REPEAT"] == 1);
      CHECK(result["counts"]["UV_SEAM"] == 0);
      CHECK(result["counts"].size() == 6);

      const auto& items = result["items"];
      const auto fractional = std::ranges::find_if(
        items, [](const auto& item) { return item["code"] == "UV_FRACTIONAL_REPEAT"; });
      REQUIRE(fractional != items.end());
      CHECK((*fractional)["face"] == panelFace);
      CHECK((*fractional)["brush"] == fixture.id(*panel));
      CHECK((*fractional)["material"] == "k_panel");
      CHECK((*fractional)["measured"]["repeats"] == Json{0.75, 0.75});
      CHECK((*fractional)["fix"]["tool"] == "uv_align");
      CHECK((*fractional)["fix"]["arguments"]["ids"] == Json{panelFace});
      CHECK((*fractional)["alternatives"][0]["tool"] == "material_fit_geometry");
      CHECK((*fractional)["alternatives"][0]["arguments"]["face"] == panelFace);

      const auto aspect = std::ranges::find_if(
        items, [](const auto& item) { return item["code"] == "UV_ASPECT_DISTORTION"; });
      REQUIRE(aspect != items.end());
      CHECK((*aspect)["brush"] == fixture.id(*stretched));
      CHECK((*aspect)["measured"]["aspect"] == 0.5);
      CHECK((*aspect)["fix"]["tool"] == "face_attributes_set");
    }

    SECTION("the fix removes the finding")
    {
      const auto before = fixture.call("uv_check", Json{{"ids", {panelFace}}});
      REQUIRE(before["total"] == 1);
      const auto& fix = before["items"][0]["fix"];
      fixture.call(fix["tool"].get<std::string>(), fix["arguments"]);
      const auto after = fixture.call("uv_check", Json{{"ids", {panelFace}}});
      CHECK(after["total"] == 0);
    }

    SECTION("ids and selection")
    {
      const auto byBrush = fixture.call("uv_check", Json{{"ids", {fixture.id(*panel)}}});
      CHECK(byBrush["scope"] == "ids");
      CHECK(byBrush["facesChecked"] == 6);
      CHECK(
        codesOf(byBrush["items"]) == std::vector<std::string>{"UV_FRACTIONAL_REPEAT"});

      mdl::selectNodes(map, {stretched});
      const auto selected = fixture.call("uv_check");
      CHECK(selected["scope"] == "selection");
      CHECK(selected["facesChecked"] == 6);
      CHECK(selected["counts"]["UV_ASPECT_DISTORTION"] == 6);
      CHECK(selected["counts"]["UV_FRACTIONAL_REPEAT"] == 0);

      mdl::deselectAll(map);
      mdl::selectBrushFaces(map, {faceOf(panel, Front)});
      const auto face = fixture.call("uv_check");
      CHECK(face["facesChecked"] == 1);
      CHECK(face["total"] == 1);
    }

    SECTION("codes and pagination")
    {
      const auto filtered = fixture.call(
        "uv_check", Json{{"scope", "map"}, {"codes", {"UV_FRACTIONAL_REPEAT"}}});
      CHECK(
        codesOf(filtered["items"]) == std::vector<std::string>{"UV_FRACTIONAL_REPEAT"});
      CHECK(filtered["counts"] == Json{{"UV_FRACTIONAL_REPEAT", 1}});

      const auto page = fixture.call("uv_check", Json{{"scope", "map"}, {"limit", 2}});
      CHECK(page["items"].size() == 2);
      CHECK(page["total"] == 7);
      REQUIRE(page["nextCursor"].is_string());
      const auto next = fixture.call(
        "uv_check",
        Json{{"scope", "map"}, {"limit", 10}, {"cursor", page["nextCursor"]}});
      CHECK(next["items"].size() == 5);
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("uv_check").code == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "uv_check", Json{{"scope", "map"}, {"ids", {fixture.id(*panel)}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("uv_check", Json{{"codes", {"UV_NOPE"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("uv_check", Json{{"ids", {"brush:999999"}}}).code
        == ErrorCode::ObjectNotFound);
    }

    SECTION("notes change what is expected")
    {
      setNote(fixture, Json{{"material", "k_tile"}, {"scale", {1, 2}}});
      const auto result = fixture.call("uv_check", Json{{"scope", "map"}});
      CHECK(result["counts"]["UV_ASPECT_DISTORTION"] == 5);
      // the panel brush's tiles at scale 1 x 1 are now distorted and unusual
      CHECK(result["counts"]["UV_UNUSUAL_SCALE"] == 5);
    }
  }

  SECTION("material_fit_geometry")
  {
    auto& document = uvCheckDocument(fixture);
    auto& map = document.map();
    auto* panel = brushesOf(map)[1];
    const auto panelFace = faceId(fixture, panel, Front);

    SECTION("a panel at scale 1")
    {
      const auto result =
        fixture.call("material_fit_geometry", Json{{"face", panelFace}});
      CHECK(result["face"] == panelFace);
      CHECK(result["material"] == "k_panel");
      CHECK(result["kind"]["value"] == "panel");
      CHECK(result["textureSize"] == Json{64, 64});
      CHECK(result["typicalScale"]["value"] == Json{1, 1});
      CHECK(result["panelSize"] == Json{64, 64});
      CHECK(result["faceSize"] == Json{48, 48});
      CHECK(result["fits"] == false);

      const auto& u = result["axes"][0];
      CHECK(u["axis"] == "u");
      CHECK(u["relevant"] == true);
      CHECK(u["repeats"] == 0.75);
      CHECK(u["smaller"].is_null());
      CHECK(u["larger"]["size"] == 64);
      CHECK(u["target"]["size"] == 64);
      CHECK(u["delta"] == 16);
      CHECK(u["resize"]["face"] == faceId(fixture, panel, {1, 0, 0}));
      CHECK(u["resize"]["distance"] == 16);
      CHECK(u["resize"]["call"]["tool"] == "face_extrude");
      CHECK(
        u["resize"]["call"]["arguments"]
        == Json{{"faces", {faceId(fixture, panel, {1, 0, 0})}}, {"distance", 16}});
      CHECK(u["resize"]["opposite"]["face"] == faceId(fixture, panel, {-1, 0, 0}));

      // the V axis points down: its far edge is the bottom face
      const auto& v = result["axes"][1];
      CHECK(v["resize"]["face"] == faceId(fixture, panel, {0, 0, -1}));
      CHECK(v["resize"]["distance"] == 16);
      CHECK(result["then"]["tool"] == "uv_align");
      CHECK(result["then"]["arguments"]["operation"] == "typical");

      // following the advice makes the face fit
      fixture.call(
        u["resize"]["call"]["tool"].get<std::string>(), u["resize"]["call"]["arguments"]);
      fixture.call(
        v["resize"]["call"]["tool"].get<std::string>(), v["resize"]["call"]["arguments"]);
      const auto fitted = fixture.call(
        "material_fit_geometry", Json{{"face", faceId(fixture, panel, Front)}});
      CHECK(fitted["faceSize"] == Json{64, 64});
      CHECK(fitted["fits"] == true);
      CHECK(fitted["axes"][0]["resize"].is_null());
    }

    SECTION("the typical scale from notes")
    {
      setNote(fixture, Json{{"material", "k_panel"}, {"scale", 0.5}});
      const auto result =
        fixture.call("material_fit_geometry", Json{{"face", panelFace}});
      CHECK(result["typicalScale"]["source"] == "notes");
      CHECK(result["panelSize"] == Json{32, 32});
      const auto& u = result["axes"][0];
      CHECK(u["repeats"] == 1.5);
      CHECK(u["smaller"] == Json{{"size", 32}, {"repeats", 1}});
      CHECK(u["larger"] == Json{{"size", 64}, {"repeats", 2}});
      CHECK(u["delta"] == -16);
      CHECK(u["resize"]["distance"] == -16);
      CHECK(!hasWarning(
        fixture.call("material_fit_geometry", Json{{"face", panelFace}}),
        "TYPICAL_SCALE_DEFAULT"));
    }

    SECTION("another material, a trim")
    {
      const auto result = fixture.call(
        "material_fit_geometry", Json{{"face", panelFace}, {"material", "k_trim"}});
      CHECK(result["material"] == "k_trim");
      CHECK(result["textureSize"] == Json{64, 16});
      CHECK(result["axes"][0]["relevant"] == false);
      CHECK(result["axes"][1]["relevant"] == true);
      CHECK(result["axes"][1]["fits"] == true);
      CHECK(result["fits"] == true);
      CHECK(result["then"]["tool"] == "material_apply");
    }

    SECTION("tiles warn")
    {
      const auto result = fixture.call(
        "material_fit_geometry", Json{{"face", faceId(fixture, panel, {1, 0, 0})}});
      CHECK(hasWarning(result, "MATERIAL_IS_TILE"));
    }

    SECTION("selection")
    {
      mdl::selectBrushFaces(map, {faceOf(panel, Front)});
      CHECK(fixture.call("material_fit_geometry")["face"] == panelFace);

      mdl::selectBrushFaces(map, {faceOf(panel, {1, 0, 0})});
      CHECK(
        fixture.callExpectingError("material_fit_geometry").code
        == ErrorCode::InvalidArgument);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("material_fit_geometry").code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "material_fit_geometry", Json{{"face", panelFace}, {"material", "nope"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("material_fit_geometry", Json{{"face", fixture.id(*panel)}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "material_fit_geometry", Json{{"face", fixture.id(*panel) + "/face:99"}})
          .code
        == ErrorCode::ObjectNotFound);
    }
  }

  SECTION("uv_align fit with keepAspect and round")
  {
    auto& map = knowledgeDocument(fixture).map();
    // the front face is 128 x 64
    auto* brush = addCuboid(map, {{0, 0, 0}, {128, 16, 64}}, "k_tile");
    const auto front = faceId(fixture, brush, Front);

    SECTION("repeatU only keeps square texels")
    {
      const auto result = resultOf(fixture.call(
        "uv_align",
        Json{
          {"ids", {front}}, {"operation", "fit"}, {"repeatU", 3}, {"keepAspect", true}}));
      const auto scale = scaleOf(brush, Front);
      CHECK(scale.x() == Catch::Approx(128.0 / 192.0));
      CHECK(scale.y() == Catch::Approx(scale.x()));
      REQUIRE(result["fits"].size() == 1);
      CHECK(result["fits"][0]["id"] == front);
      CHECK(result["fits"][0]["repeats"] == Json{3, 1.5});
      CHECK(!result["fits"][0].contains("typicalScale"));
    }

    SECTION("round makes the following axis whole")
    {
      const auto result = resultOf(fixture.call(
        "uv_align",
        Json{
          {"ids", {front}},
          {"operation", "fit"},
          {"repeatU", 2.6},
          {"keepAspect", true},
          {"round", true}}));
      CHECK(result["fits"][0]["repeats"] == Json{3, 2});
      CHECK(scaleOf(brush, Front).y() == Catch::Approx(0.5));
      const auto sample = sampleFace(faceOf(brush, Front).face(), vm::vec2d{64, 64});
      REQUIRE(sample);
      CHECK(sample->aligned());
    }

    SECTION("repeatV only")
    {
      const auto result = resultOf(fixture.call(
        "uv_align",
        Json{
          {"ids", {front}}, {"operation", "fit"}, {"repeatV", 1}, {"keepAspect", true}}));
      CHECK(result["fits"][0]["repeats"] == Json{2, 1});
      CHECK(scaleOf(brush, Front) == vm::vec2d{1, 1});
    }

    SECTION("the typical aspect ratio from notes")
    {
      setNote(fixture, Json{{"material", "k_tile"}, {"scale", {1, 0.5}}});
      resultOf(fixture.call(
        "uv_align",
        Json{
          {"ids", {front}}, {"operation", "fit"}, {"repeatU", 1}, {"keepAspect", true}}));
      CHECK(scaleOf(brush, Front) == vm::vec2d{2, 1});
    }

    SECTION("negative scales keep their sign")
    {
      fixture.call("face_attributes_set", Json{{"ids", {front}}, {"scale", {-1, 1}}});
      fixture.call(
        "uv_align",
        Json{
          {"ids", {front}}, {"operation", "fit"}, {"repeatU", 1}, {"keepAspect", true}});
      CHECK(scaleOf(brush, Front) == vm::vec2d{-2, 2});
    }

    SECTION("round without keepAspect rounds the given repeats")
    {
      const auto result = resultOf(fixture.call(
        "uv_align",
        Json{
          {"ids", {front}},
          {"operation", "fit"},
          {"repeatU", 1.4},
          {"repeatV", 0.4},
          {"round", true}}));
      CHECK(result["fits"][0]["repeats"] == Json{1, 1});
    }

    SECTION("selection")
    {
      mdl::selectBrushFaces(map, {faceOf(brush, Front)});
      const auto result = resultOf(fixture.call(
        "uv_align", Json{{"operation", "fit"}, {"repeatU", 1}, {"keepAspect", true}}));
      CHECK(result["count"] == 1);
      CHECK(scaleOf(brush, Front) == vm::vec2d{2, 2});
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "uv_align",
        Json{
          {"ids", {front}},
          {"operation", "fit"},
          {"repeatU", 1},
          {"keepAspect", true},
          {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(scaleOf(brush, Front) == vm::vec2d{1, 1});
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "uv_align",
            Json{
              {"ids", {front}},
              {"operation", "fit"},
              {"repeatU", 1},
              {"repeatV", 1},
              {"keepAspect", true}})
          .code
        == ErrorCode::InvalidArgument);

      const auto ignored = fixture.call(
        "uv_align",
        Json{
          {"ids", {front}},
          {"operation", "rotate90"},
          {"direction", "cw"},
          {"keepAspect", true}});
      CHECK(hasWarning(ignored, "IGNORED_ARGUMENT"));
    }
  }

  SECTION("uv_align typical")
  {
    auto& map = knowledgeDocument(fixture).map();
    auto* brush = addCuboid(
      map,
      {{0, 0, 0}, {64, 16, 64}},
      "k_panel",
      {.offset = {7.0f, 3.0f}, .scale = {0.8f, 0.9f}});
    const auto front = faceId(fixture, brush, Front);

    SECTION("the scale from notes, justified")
    {
      setNote(fixture, Json{{"material", "k_panel"}, {"scale", 0.5}});
      const auto result =
        fixture.call("uv_align", Json{{"ids", {front}}, {"operation", "typical"}});
      CHECK(scaleOf(brush, Front) == vm::vec2d{0.5, 0.5});
      const auto& fits = resultOf(result)["fits"];
      REQUIRE(fits.size() == 1);
      CHECK(fits[0]["repeats"] == Json{2, 2});
      CHECK(fits[0]["typicalScale"]["source"] == "notes");
      CHECK(fits[0]["typicalScale"]["value"] == Json{0.5, 0.5});
      CHECK(!hasWarning(result, "TYPICAL_SCALE_DEFAULT"));
      // justified: the panel's edges lie on the face's edges
      CHECK(fixture.call("uv_check", Json{{"ids", {front}}})["total"] == 0);
    }

    SECTION("the scale of other faces in the map")
    {
      addCuboid(map, {{200, 0, 0}, {264, 16, 64}}, "k_panel", {.scale = {0.25f, 0.25f}});
      addCuboid(map, {{300, 0, 0}, {364, 16, 64}}, "k_panel", {.scale = {0.25f, 0.25f}});
      const auto result = resultOf(
        fixture.call("uv_align", Json{{"ids", {front}}, {"operation", "typical"}}));
      CHECK(scaleOf(brush, Front) == vm::vec2d{0.25, 0.25});
      CHECK(result["fits"][0]["typicalScale"]["source"] == "map");
    }

    SECTION("without data, the game's default scale")
    {
      const auto result = fixture.call(
        "uv_align", Json{{"ids", {fixture.id(*brush)}}, {"operation", "typical"}});
      CHECK(hasWarning(result, "TYPICAL_SCALE_DEFAULT"));
      CHECK(resultOf(result)["fits"][0]["typicalScale"]["source"] == "config");
      CHECK(scaleOf(brush, Front) == vm::vec2d{1, 1});
    }

    SECTION("dry run and selection")
    {
      fixture.call(
        "uv_align", Json{{"ids", {front}}, {"operation", "typical"}, {"dryRun", true}});
      CHECK(scaleOf(brush, Front) == vm::vec2d{vm::vec2f{0.8f, 0.9f}});

      // the whole brush: no other faces use the material, so the default applies
      mdl::selectNodes(map, {brush});
      const auto result = fixture.call("uv_align", Json{{"operation", "typical"}});
      CHECK(resultOf(result)["count"] == 6);
      CHECK(scaleOf(brush, Front) == vm::vec2d{1, 1});
    }

    SECTION("materials that are not loaded are skipped")
    {
      auto* missing = addCuboid(map, {{200, 0, 0}, {264, 16, 64}}, "missing");
      const auto result = fixture.call(
        "uv_align", Json{{"ids", {fixture.id(*missing)}}, {"operation", "typical"}});
      CHECK(hasWarning(result, "MATERIAL_NOT_LOADED"));
    }
  }
}

} // namespace tb::mcp
