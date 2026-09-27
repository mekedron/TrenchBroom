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
#include "mcp/tools/UvTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const auto Front = vm::vec3d{0, -1, 0};

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

/** A Quake document (Valve format) with the materials of knowledge.wad loaded. */
mdl::Map& knowledgeMap(McpToolFixture& fixture)
{
  fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
  auto& map = fixture.host().documentList.back().document->map();
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
  REQUIRE(map.materialManager().materials().size() == 4);
  return map;
}

mdl::BrushNode* addCuboid(
  mdl::Map& map, const vm::bbox3d& box, const std::string& material)
{
  auto brush = brushBuilder(map).createCuboid(box, material).value();
  for (auto& face : brush.faces())
  {
    REQUIRE(
      face.setUvAttributes({.offset = {7.0f, 3.0f}, .scale = {0.8f, 0.9f}}).is_success());
  }
  return static_cast<mdl::BrushNode*>(addBrushes(map, {std::move(brush)}).front());
}

/** Copies the brush as map text and deletes it. */
std::string cutText(McpToolFixture& fixture, mdl::BrushNode* brushNode)
{
  const auto id = fixture.id(*brushNode);
  return fixture.call("clipboard_cut", Json{{"ids", {id}}})["result"]["text"]
    .get<std::string>();
}

const mdl::BrushFace& frontOf(McpToolFixture& fixture, const Json& pasteResult)
{
  const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(
    fixture.node(pasteResult["result"]["ids"][0].get<std::string>()));
  REQUIRE(brushNode);
  const auto index = brushNode->brush().findFace(Front);
  REQUIRE(index);
  return brushNode->brush().face(*index);
}

vm::vec2d scaleOf(const mdl::BrushFace& face)
{
  return vm::vec2d{face.uvAttributes().scale};
}

} // namespace

TEST_CASE("UV modes of new objects")
{
  auto fixture = McpToolFixture{};
  auto& map = knowledgeMap(fixture);
  fixture.call(
    "material_notes_set",
    Json{
      {"notes", Json::array({Json{{"material", "k_panel"}, {"scale", 0.5}}})},
      {"scope", "game"}});

  SECTION("clipboard_paste keep leaves the UVs")
  {
    const auto text =
      cutText(fixture, addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_panel"));
    const auto result = fixture.call("clipboard_paste", Json{{"text", text}});
    CHECK_FALSE(result["result"].contains("uv"));
    CHECK(scaleOf(frontOf(fixture, result)) == vm::vec2d{vm::vec2f{0.8f, 0.9f}});
  }

  SECTION("clipboard_paste typical")
  {
    const auto text =
      cutText(fixture, addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_panel"));
    const auto result =
      fixture.call("clipboard_paste", Json{{"text", text}, {"uv", "typical"}});
    const auto& uv = result["result"]["uv"];
    CHECK(uv["mode"] == "typical");
    CHECK(uv["faces"] == 6);
    CHECK(uv["skipped"] == 0);
    CHECK(uv["typicalScales"]["k_panel"]["source"] == "notes");
    CHECK(scaleOf(frontOf(fixture, result)) == vm::vec2d{0.5, 0.5});
    CHECK(result["undoStep"] == "AI: Paste");
    // justified: the panel fits the face
    const auto brushId = result["result"]["ids"][0].get<std::string>();
    const auto& brush =
      static_cast<const mdl::BrushNode*>(fixture.node(brushId))->brush();
    const auto front = brushId + "/face:" + std::to_string(*brush.findFace(Front));
    CHECK(fixture.call("uv_check", Json{{"ids", {front}}})["total"] == 0);
  }

  SECTION("map_import fit rounds to whole repeats")
  {
    // 72 units at scale 0.5 are 2.25 repeats of the 64 texel panel: 2 repeats
    const auto text =
      cutText(fixture, addCuboid(map, {{0, 0, 0}, {72, 16, 64}}, "k_panel"));
    const auto path = std::filesystem::temp_directory_path()
                      / (
                        "tb_mcp_uv_modes_"
                        + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count())
                        + ".map");
    {
      auto stream = std::ofstream{path};
      stream << text;
    }
    const auto result =
      fixture.call("map_import", Json{{"path", path.string()}, {"uv", "fit"}});
    std::filesystem::remove(path);

    CHECK(result["result"]["uv"]["mode"] == "fit");
    const auto scale = scaleOf(frontOf(fixture, result));
    CHECK(scale.x() == vm::approx{72.0 / 128.0});
    CHECK(scale.y() == vm::approx{0.5});
  }

  SECTION("clipboard_paste world")
  {
    const auto text =
      cutText(fixture, addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_panel"));
    const auto result =
      fixture.call("clipboard_paste", Json{{"text", text}, {"uv", "world"}});
    const auto& face = frontOf(fixture, result);
    CHECK(scaleOf(face) == vm::vec2d{0.5, 0.5});
    CHECK(face.uvAttributes().offset == vm::vec2f{0, 0});
    CHECK(face.uvAttributes().rotation == 0.0f);
    CHECK(vm::abs(face.uAxis()) == vm::vec3d{1, 0, 0});
  }

  SECTION("faces whose material is not loaded keep their UVs")
  {
    const auto text =
      cutText(fixture, addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "not_in_wad"));
    const auto result =
      fixture.call("clipboard_paste", Json{{"text", text}, {"uv", "typical"}});
    CHECK(result["result"]["uv"]["faces"] == 0);
    CHECK(result["result"]["uv"]["skipped"] == 6);
    CHECK(hasWarning(result, "MATERIAL_NOT_LOADED"));
    CHECK(scaleOf(frontOf(fixture, result)) == vm::vec2d{vm::vec2f{0.8f, 0.9f}});
  }

  SECTION("a dry run leaves the map unchanged")
  {
    const auto text =
      cutText(fixture, addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_panel"));
    const auto before = map.worldNode().defaultLayer()->childCount();
    const auto result = fixture.call(
      "clipboard_paste", Json{{"text", text}, {"uv", "typical"}, {"dryRun", true}});
    CHECK(result["result"]["uv"]["faces"] == 6);
    CHECK(map.worldNode().defaultLayer()->childCount() == before);
  }

  SECTION("face text ignores uv")
  {
    auto* brushNode = addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_panel");
    const auto face = fixture.id(*brushNode) + "/face:0";
    const auto text =
      fixture.call("clipboard_copy", Json{{"faces", {face}}})["text"].get<std::string>();
    const auto result = fixture.call(
      "clipboard_paste", Json{{"text", text}, {"faces", {face}}, {"uv", "typical"}});
    CHECK(hasWarning(result, "IGNORED_ARGUMENT"));
  }

  SECTION("uv_align on a whole layer in one undo step")
  {
    for (size_t i = 0; i < 40; ++i)
    {
      const auto x = double(i) * 80.0;
      addCuboid(map, {{x, 0, 0}, {x + 64, 16, 64}}, "k_panel");
    }
    const auto result = fixture.call(
      "uv_align",
      Json{{"ids", {"layer:default"}}, {"operation", "typical"}, {"detail", "summary"}});
    // the faces of the initial brush have no loaded material
    CHECK(result["result"]["count"] == 240);
    CHECK(result["undoStep"] == "AI: Align UV");
    CHECK(result["changes"]["counts"]["modified"] == 40);
    for (const auto* brushNode : map.worldNode().defaultLayer()->children())
    {
      const auto& brush = static_cast<const mdl::BrushNode*>(brushNode)->brush();
      const auto& face = brush.face(*brush.findFace(Front));
      if (face.materialName() == "k_panel")
      {
        CHECK(scaleOf(face) == vm::vec2d{0.5, 0.5});
      }
    }

    const auto history = fixture.call("history_get", Json{{"limit", 1}});
    CHECK(history["undo"][0]["name"] == "AI: Align UV");
  }
}

TEST_CASE("uvModeFromString")
{
  CHECK(uvModeFromString("keep") == UvMode::Keep);
  CHECK(uvModeFromString("typical") == UvMode::Typical);
  CHECK(uvModeFromString("fit") == UvMode::Fit);
  CHECK(uvModeFromString("world") == UvMode::World);
  CHECK(uvModeFromString("stretch") == std::nullopt);
}

} // namespace tb::mcp
