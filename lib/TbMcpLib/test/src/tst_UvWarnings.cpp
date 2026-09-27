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
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/UvAttributes.h"
#include "ui/MapDocument.h"

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

void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

/** A Quake map (Valve format) with the materials of knowledge.wad loaded. */
mdl::Map& knowledgeMap(McpToolFixture& fixture)
{
  fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
  auto* document = fixture.host().documentList.back().document;
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  processResources(document->map());
  REQUIRE(document->map().materialManager().materials().size() == 4);
  return document->map();
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

std::string faceId(
  McpToolFixture& fixture, mdl::BrushNode* brushNode, const vm::vec3d& normal)
{
  const auto index = brushNode->brush().findFace(normal);
  REQUIRE(index);
  return fixture.id(*brushNode) + "/face:" + std::to_string(*index);
}

std::vector<Json> warningsWith(const Json& result, const std::string& code)
{
  auto warnings = std::vector<Json>{};
  if (result.contains("warnings"))
  {
    for (const auto& warning : result["warnings"])
    {
      if (warning["code"] == code)
      {
        warnings.push_back(warning);
      }
    }
  }
  return warnings;
}

size_t uvWarningCount(const Json& result)
{
  if (!result.contains("warnings"))
  {
    return 0;
  }
  return size_t(std::ranges::count_if(result["warnings"], [](const auto& warning) {
    return warning["code"].template get<std::string>().starts_with("UV_");
  }));
}

const auto Front = vm::vec3d{0, -1, 0};

} // namespace

TEST_CASE("UvWarnings")
{
  auto fixture = McpToolFixture{};
  auto& map = knowledgeMap(fixture);

  SECTION("material_apply")
  {
    // the front face is 48 x 48: a 64 x 64 panel at scale 1 repeats 0.75 times
    auto* brush = addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_tile");
    auto* other = addCuboid(map, {{200, 0, 0}, {248, 16, 48}}, "k_panel");
    const auto front = faceId(fixture, brush, Front);

    const auto result =
      fixture.call("material_apply", Json{{"material", "k_panel"}, {"ids", {front}}});
    const auto warnings = warningsWith(result, "UV_FRACTIONAL_REPEAT");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front()["objectIds"] == Json{front});
    const auto message = warnings.front()["message"].get<std::string>();
    CHECK(message.find("k_panel") != std::string::npos);
    CHECK(message.find("Fix: uv_align") != std::string::npos);

    // only the changed faces are reported, not the other panel brush
    for (const auto& warning : result["warnings"])
    {
      for (const auto& id : warning["objectIds"])
      {
        CHECK(id.get<std::string>().starts_with(fixture.id(*brush)));
      }
    }
    CHECK(fixture.id(*other) != fixture.id(*brush));

    // a panel that fits has no UV warnings
    auto* fitting = addCuboid(map, {{384, 0, 0}, {448, 16, 64}}, "k_tile");
    const auto fits = fixture.call(
      "material_apply",
      Json{{"material", "k_panel"}, {"ids", {faceId(fixture, fitting, Front)}}});
    CHECK(uvWarningCount(fits) == 0);
  }

  SECTION("material_replace")
  {
    auto* brush = addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_tile");
    const auto result = fixture.call(
      "material_replace",
      Json{{"from", "k_tile"}, {"to", "k_panel"}, {"ids", {fixture.id(*brush)}}});
    CHECK(!warningsWith(result, "UV_FRACTIONAL_REPEAT").empty());
    // at most MaxUvWarnings findings plus the UV_CHECK_MORE summary
    CHECK(uvWarningCount(result) <= MaxUvWarnings + 1);
  }

  SECTION("face_attributes_set")
  {
    auto* brush = addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile");
    const auto front = faceId(fixture, brush, Front);

    const auto result =
      fixture.call("face_attributes_set", Json{{"ids", {front}}, {"scale", {1, 2}}});
    const auto warnings = warningsWith(result, "UV_ASPECT_DISTORTION");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front()["objectIds"] == Json{front});

    // a dry run reports the findings of the would-be state
    const auto dryRun = fixture.call(
      "face_attributes_set", Json{{"ids", {front}}, {"scale", {1, 1}}, {"dryRun", true}});
    CHECK(warningsWith(dryRun, "UV_ASPECT_DISTORTION").empty());

    const auto fixed =
      fixture.call("face_attributes_set", Json{{"ids", {front}}, {"scale", {1, 1}}});
    CHECK(uvWarningCount(fixed) == 0);
  }

  SECTION("seams name both faces")
  {
    auto* left = addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile");
    auto* right = addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile");
    const auto result = fixture.call(
      "face_attributes_set",
      Json{{"ids", {faceId(fixture, right, Front)}}, {"offset", {16, 0}}});
    const auto warnings = warningsWith(result, "UV_SEAM");
    REQUIRE(warnings.size() == 1);
    CHECK(
      warnings.front()["objectIds"]
      == Json{faceId(fixture, right, Front), faceId(fixture, left, Front)});
  }

  SECTION("uv_align")
  {
    auto* brush = addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile");
    const auto front = faceId(fixture, brush, Front);
    const auto result = fixture.call(
      "uv_align",
      Json{{"ids", {front}}, {"operation", "fit"}, {"repeatU", 1}, {"repeatV", 3}});
    CHECK(warningsWith(result, "UV_ASPECT_DISTORTION").size() == 1);

    const auto kept = fixture.call(
      "uv_align",
      Json{{"ids", {front}}, {"operation", "fit"}, {"repeatU", 1}, {"keepAspect", true}});
    CHECK(uvWarningCount(kept) == 0);
  }

  SECTION("at most 10 findings, then a summary")
  {
    auto ids = Json::array();
    for (size_t i = 0; i < 3; ++i)
    {
      auto* brush = addCuboid(
        map, {{double(i) * 100.0, 0, 0}, {double(i) * 100.0 + 64, 16, 64}}, "k_tile");
      ids.push_back(fixture.id(*brush));
    }
    const auto result =
      fixture.call("face_attributes_set", Json{{"ids", ids}, {"scale", {1, 2}}});
    CHECK(warningsWith(result, "UV_ASPECT_DISTORTION").size() == 10);
    const auto more = warningsWith(result, "UV_CHECK_MORE");
    REQUIRE(more.size() == 1);
    const auto message = more.front()["message"].get<std::string>();
    CHECK(message.find("8 more") != std::string::npos);
    CHECK(message.find("8 UV_ASPECT_DISTORTION") != std::string::npos);
    CHECK(message.find("uv_check") != std::string::npos);
  }
}

} // namespace tb::mcp
