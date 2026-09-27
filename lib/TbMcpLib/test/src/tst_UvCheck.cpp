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
#include "mcp/ServerState.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mcp/tools/UvCheck.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/UvAttributes.h"
#include "ui/MapDocument.h"

#include "kd/string_format.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <map>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::string knowledgeWad()
{
  return (getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad").string();
}

void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

/** A Quake document (Valve format) with the materials of knowledge.wad loaded. */
ui::MapDocument& knowledgeDocument(McpToolFixture& fixture)
{
  fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
  auto* document = fixture.host().documentList.back().document;
  fixture.call("materials_collections_set", Json{{"wads", Json{knowledgeWad()}}});
  processResources(document->map());
  REQUIRE(document->map().materialManager().materials().size() == 4);
  return *document;
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
  const auto added = addBrushes(map, {std::move(brush)});
  REQUIRE(added.size() == 1);
  return static_cast<mdl::BrushNode*>(added.front());
}

mdl::BrushFaceHandle faceOf(mdl::BrushNode* brushNode, const vm::vec3d& normal)
{
  const auto index = brushNode->brush().findFace(normal);
  REQUIRE(index);
  return mdl::BrushFaceHandle{brushNode, *index};
}

MaterialProfile makeProfile(
  const std::string& name,
  const MaterialKind kind,
  const std::optional<vm::vec2d>& textureSize = vm::vec2d{64, 64})
{
  auto profile = MaterialProfile{};
  profile.name = name;
  profile.loaded = textureSize.has_value();
  profile.textureSize = textureSize;
  profile.kind = {kind, "notes", 0};
  profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{1, 1}, "config", 0};
  return profile;
}

/** Profiles given by the test, keyed by lower-case name. */
struct Profiles
{
  std::map<std::string, MaterialProfile> profiles;

  Profiles()
  {
    add(makeProfile("k_tile", MaterialKind::Tile));
    add(makeProfile("k_panel", MaterialKind::Panel));
    add(makeProfile("k_trim", MaterialKind::Trim, vm::vec2d{64, 16}));
    add(makeProfile("{k_decal", MaterialKind::Decal));
    add(makeProfile("missing", MaterialKind::Panel, std::nullopt));
    add(makeProfile("clip", MaterialKind::Tool, std::nullopt));
  }

  MaterialProfile& add(MaterialProfile profile)
  {
    auto key = kdl::str_to_lower(profile.name);
    return profiles[key] = std::move(profile);
  }

  MaterialProfile& operator[](const std::string& name) { return profiles.at(name); }

  ProfileProvider provider()
  {
    return [this](const std::string& name) -> const MaterialProfile& {
      return profiles.at(kdl::str_to_lower(name));
    };
  }
};

std::vector<UvIssue> issuesOf(const std::vector<UvFinding>& findings)
{
  auto result = std::vector<UvIssue>{};
  for (const auto& finding : findings)
  {
    result.push_back(finding.issue);
  }
  return result;
}

size_t countOf(const std::vector<UvFinding>& findings, const UvIssue issue)
{
  return size_t(std::ranges::count_if(
    findings, [&](const auto& finding) { return finding.issue == issue; }));
}

const vm::vec3d Front = vm::vec3d{0, -1, 0};

} // namespace

TEST_CASE("UvCheck")
{
  auto fixture = McpToolFixture{};
  auto& document = knowledgeDocument(fixture);
  auto& map = document.map();
  auto profiles = Profiles{};

  SECTION("codes")
  {
    CHECK(toString(UvIssue::AspectDistortion) == "UV_ASPECT_DISTORTION");
    CHECK(toString(UvIssue::Seam) == "UV_SEAM");
    CHECK(uvIssueFromString("UV_PANEL_NOT_ALIGNED") == UvIssue::PanelNotAligned);
    CHECK(uvIssueFromString("UV_NOPE") == std::nullopt);
    CHECK(uvIssueCodes().size() == 6);
  }

  SECTION("expectedAspect")
  {
    auto profile = makeProfile("k_tile", MaterialKind::Tile);
    CHECK(expectedAspect(profile).ratio == 1.0);
    CHECK(expectedAspect(profile).source == "default");

    profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{2, 1}, "map", 50};
    CHECK(expectedAspect(profile).ratio == 1.0);

    profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{2, 1}, "corpus", 50};
    CHECK(expectedAspect(profile).ratio == 2.0);
    CHECK(expectedAspect(profile).source == "corpus");

    profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{0.5, 1}, "notes", 0};
    CHECK(expectedAspect(profile).ratio == 0.5);
  }

  SECTION("checkedKind")
  {
    auto profile = makeProfile("k_panel", MaterialKind::Tile);
    profile.kind = {MaterialKind::Tile, "map", 5};
    CHECK(checkedKind(profile) == MaterialKind::Tile);

    profile.image = ImageAnalysis{};
    profile.image->suggestedKind = MaterialKind::Panel;
    CHECK(checkedKind(profile) == MaterialKind::Panel);

    profile.kind = {MaterialKind::Tile, "map", MinMapSamples};
    CHECK(checkedKind(profile) == MaterialKind::Tile);

    profile.kind = {MaterialKind::Tile, "corpus", 5};
    CHECK(checkedKind(profile) == MaterialKind::Tile);
  }

  SECTION("trimAcrossAxis")
  {
    auto profile = makeProfile("k_trim", MaterialKind::Trim, vm::vec2d{64, 16});
    CHECK(trimAcrossAxis(profile) == 1);
    profile.textureSize = vm::vec2d{16, 64};
    CHECK(trimAcrossAxis(profile) == 0);
    profile.textureSize = vm::vec2d{64, 64};
    CHECK(trimAcrossAxis(profile) == std::nullopt);
    profile.image = ImageAnalysis{};
    profile.image->tilesU = true;
    CHECK(trimAcrossAxis(profile) == 1);
  }

  SECTION("UV_ASPECT_DISTORTION")
  {
    auto* stretched =
      addCuboid(map, {{0, 0, 0}, {128, 16, 128}}, "k_tile", {.scale = {1.0f, 2.0f}});
    auto* slight =
      addCuboid(map, {{0, 64, 0}, {128, 80, 128}}, "k_tile", {.scale = {1.0f, 1.05f}});

    const auto findings = checkUv(
      mdl::toHandles(stretched), map, profiles.provider(), {UvIssue::AspectDistortion});
    REQUIRE(findings.size() == 6);
    const auto& finding = findings.front();
    CHECK(finding.issue == UvIssue::AspectDistortion);
    CHECK(finding.material == "k_tile");
    CHECK(finding.measured["aspect"] == 0.5);
    CHECK(finding.measured["expectedAspect"] == 1.0);
    CHECK(finding.measured["expectedFrom"] == "default");
    CHECK(finding.measured["deviation"] == 1.0);
    REQUIRE(!finding.fixes.empty());
    CHECK(finding.fixes.front().tool == "face_attributes_set");
    CHECK(finding.fixes.front().arguments["scale"] == Json{1.0, 1.0});

    CHECK(checkUv(
            mdl::toHandles(slight), map, profiles.provider(), {UvIssue::AspectDistortion})
            .empty());

    // the expected ratio comes from notes or the corpus
    profiles["k_tile"].typicalScale = Sourced<vm::vec2d>{vm::vec2d{1, 2}, "notes", 0};
    CHECK(
      checkUv(
        mdl::toHandles(stretched), map, profiles.provider(), {UvIssue::AspectDistortion})
        .empty());
    CHECK(
      checkUv(
        mdl::toHandles(slight), map, profiles.provider(), {UvIssue::AspectDistortion})
        .size()
      == 6);
  }

  SECTION("aspect distortion of a panel suggests an aspect-preserving fit")
  {
    auto* panel =
      addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_panel", {.scale = {1.0f, 0.5f}});
    const auto findings = checkUv(
      {faceOf(panel, Front)}, map, profiles.provider(), {UvIssue::AspectDistortion});
    REQUIRE(findings.size() == 1);
    const auto& fix = findings.front().fixes.front();
    CHECK(fix.tool == "uv_align");
    CHECK(fix.arguments["operation"] == "fit");
    CHECK(fix.arguments["keepAspect"] == true);
    CHECK(fix.arguments["repeatU"] == 1.0);
  }

  SECTION("UV_FRACTIONAL_REPEAT")
  {
    SECTION("panel with less than one repeat")
    {
      auto* panel = addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_panel");
      const auto findings = checkUv({faceOf(panel, Front)}, map, profiles.provider());
      REQUIRE(issuesOf(findings) == std::vector{UvIssue::FractionalRepeat});
      const auto& finding = findings.front();
      CHECK(finding.measured["repeats"] == Json{0.75, 0.75});
      CHECK(finding.measured["fractionalAxes"] == Json{"u", "v"});
      CHECK(finding.fixes.front().tool == "uv_align");
      CHECK(finding.fixes.front().arguments["repeatU"] == 1.0);
      CHECK(finding.fixes.front().arguments["repeatV"] == 1.0);
      CHECK(finding.fixes[1].tool == "material_fit_geometry");
    }

    SECTION("panel with 1.5 repeats")
    {
      auto* panel =
        addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_panel", {.scale = {0.5f, 0.5f}});
      const auto findings = checkUv({faceOf(panel, Front)}, map, profiles.provider());
      REQUIRE(issuesOf(findings) == std::vector{UvIssue::FractionalRepeat});
      CHECK(findings.front().measured["repeats"] == Json{1.5, 1.5});
    }

    SECTION("a panel that fits has no findings")
    {
      auto* panel =
        addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_panel", {.scale = {0.75f, 0.75f}});
      CHECK(checkUv({faceOf(panel, Front)}, map, profiles.provider()).empty());
    }

    SECTION("a panel that only fits with distorted texels suggests resizing first")
    {
      auto* panel = addCuboid(map, {{0, 0, 0}, {80, 16, 48}}, "k_panel");
      const auto findings = checkUv(
        {faceOf(panel, Front)}, map, profiles.provider(), {UvIssue::FractionalRepeat});
      REQUIRE(findings.size() == 1);
      CHECK(findings.front().fixes.front().tool == "material_fit_geometry");
      CHECK(findings.front().fixes[1].arguments["keepAspect"] == true);
    }

    SECTION("trims are checked across the strip only")
    {
      auto* across = addCuboid(map, {{0, 0, 0}, {100, 16, 24}}, "k_trim");
      const auto findings = checkUv(
        {faceOf(across, Front)}, map, profiles.provider(), {UvIssue::FractionalRepeat});
      REQUIRE(findings.size() == 1);
      CHECK(findings.front().measured["fractionalAxes"] == Json{"v"});
      CHECK(findings.front().fixes.front().arguments["repeatV"] == 2.0);
      CHECK(findings.front().fixes.front().arguments["keepAspect"] == true);

      auto* along = addCuboid(map, {{0, 64, 0}, {100, 80, 16}}, "k_trim");
      CHECK(
        checkUv(
          {faceOf(along, Front)}, map, profiles.provider(), {UvIssue::FractionalRepeat})
          .empty());
    }

    SECTION("tiles may repeat fractionally")
    {
      auto* tile = addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_tile");
      CHECK(checkUv({faceOf(tile, Front)}, map, profiles.provider()).empty());
    }
  }

  SECTION("UV_PANEL_NOT_ALIGNED")
  {
    auto* panel = addCuboid(
      map,
      {{0, 0, 0}, {32, 16, 32}},
      "k_panel",
      {.offset = {10.0f, 0.0f}, .scale = {0.5f, 0.5f}});
    const auto findings = checkUv({faceOf(panel, Front)}, map, profiles.provider());
    REQUIRE(issuesOf(findings) == std::vector{UvIssue::PanelNotAligned});
    const auto& finding = findings.front();
    CHECK(finding.measured["texelStart"] == Json{10.0, 0.0});
    CHECK(finding.measured["unalignedAxes"] == Json{"u"});
    CHECK(finding.fixes.front().tool == "face_attributes_set");
    CHECK(finding.fixes.front().arguments["offsetBy"] == Json{-10.0, 0.0});
  }

  SECTION("UV_UNUSUAL_SCALE")
  {
    auto* tile =
      addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile", {.scale = {2.0f, 2.0f}});
    const auto face = std::vector{faceOf(tile, Front)};
    const auto only = std::vector{UvIssue::UnusualScale};
    auto& profile = profiles["k_tile"];

    // no data: nothing is unusual
    CHECK(checkUv(face, map, profiles.provider(), only).empty());

    const auto range = ScaleRange{{0.5, 0.5}, {0.75, 0.75}, {1, 1}, {1.5, 1.5}};
    profile.scaleRange = Sourced<ScaleRange>{range, "corpus", 10};
    profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{1, 1}, "corpus", 10};
    auto findings = checkUv(face, map, profiles.provider(), only);
    REQUIRE(findings.size() == 1);
    CHECK(findings.front().measured["factor"] == 2.0);
    CHECK(findings.front().measured["direction"] == "above");
    CHECK(findings.front().measured["source"] == "corpus");
    CHECK(findings.front().fixes.front().arguments["scale"] == Json{1.0, 1.0});

    // too few corpus samples
    profile.scaleRange = Sourced<ScaleRange>{range, "corpus", MinCorpusScaleSamples - 1};
    CHECK(checkUv(face, map, profiles.provider(), only).empty());

    // map statistics need more samples
    profile.scaleRange = Sourced<ScaleRange>{range, "map", 10};
    CHECK(checkUv(face, map, profiles.provider(), only).empty());
    profile.scaleRange = Sourced<ScaleRange>{range, "map", MinMapSamples};
    CHECK(checkUv(face, map, profiles.provider(), only).size() == 1);

    // a scale from notes is the range
    profile.scaleRange.reset();
    profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{4, 4}, "notes", 0};
    findings = checkUv(face, map, profiles.provider(), only);
    REQUIRE(findings.size() == 1);
    CHECK(findings.front().measured["direction"] == "below");
    profile.typicalScale = Sourced<vm::vec2d>{vm::vec2d{2.2, 2.2}, "notes", 0};
    CHECK(checkUv(face, map, profiles.provider(), only).empty());
  }

  SECTION("UV_TEXEL_DENSITY_MISMATCH")
  {
    SECTION("faces of the same brush")
    {
      auto cuboid =
        brushBuilder(map).createCuboid({{0, 0, 0}, {64, 64, 64}}, "k_tile").value();
      const auto topIndex = cuboid.findFace(vm::vec3d{0, 0, 1});
      REQUIRE(topIndex);
      REQUIRE(cuboid.face(*topIndex)
                .setUvAttributes(mdl::UvAttributes{.scale = {2.0f, 2.0f}})
                .is_success());
      auto* brush =
        static_cast<mdl::BrushNode*>(addBrushes(map, {std::move(cuboid)}).front());
      const auto top = faceOf(brush, {0, 0, 1});

      const auto findings = checkUv(
        mdl::toHandles(brush), map, profiles.provider(), {UvIssue::TexelDensityMismatch});
      REQUIRE(findings.size() == 1);
      CHECK(findings.front().face == top);
      CHECK(findings.front().neighbour);
      CHECK(findings.front().measured["ratio"] == 2.0);
      CHECK(findings.front().fixes.front().arguments["scale"] == Json{1.0, 1.0});
    }

    SECTION("coplanar faces of other brushes")
    {
      auto* left = addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile");
      auto* right =
        addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile", {.scale = {2.0f, 2.0f}});

      const auto findings = checkUv(
        {faceOf(left, Front)}, map, profiles.provider(), {UvIssue::TexelDensityMismatch});
      REQUIRE(findings.size() == 1);
      CHECK(findings.front().neighbour == faceOf(right, Front));

      // both checked: reported once, on the face further from the typical scale
      const auto both = checkUv(
        {faceOf(left, Front), faceOf(right, Front)},
        map,
        profiles.provider(),
        {UvIssue::TexelDensityMismatch});
      REQUIRE(both.size() == 1);
      CHECK(both.front().face == faceOf(right, Front));
    }

    SECTION("panels are not compared")
    {
      addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile");
      auto* panel =
        addCuboid(map, {{64, 0, 0}, {96, 16, 32}}, "k_panel", {.scale = {0.5f, 0.5f}});
      CHECK(checkUv(
              {faceOf(panel, Front)},
              map,
              profiles.provider(),
              {UvIssue::TexelDensityMismatch})
              .empty());
    }
  }

  SECTION("UV_SEAM")
  {
    auto* left = addCuboid(map, {{0, 0, 0}, {64, 16, 64}}, "k_tile");

    SECTION("continuous texture")
    {
      auto* right = addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile");
      CHECK(checkUv(
              {faceOf(left, Front), faceOf(right, Front)},
              map,
              profiles.provider(),
              {UvIssue::Seam})
              .empty());
    }

    SECTION("offset by a whole texture")
    {
      auto* right =
        addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile", {.offset = {64.0f, 0.0f}});
      CHECK(checkUv({faceOf(right, Front)}, map, profiles.provider(), {UvIssue::Seam})
              .empty());
    }

    SECTION("offset mismatch")
    {
      auto* right =
        addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile", {.offset = {16.0f, 0.0f}});
      const auto findings = checkUv(
        {faceOf(left, Front), faceOf(right, Front)},
        map,
        profiles.provider(),
        {UvIssue::Seam});
      REQUIRE(findings.size() == 1);
      const auto& finding = findings.front();
      CHECK(finding.face == faceOf(left, Front));
      CHECK(finding.neighbour == faceOf(right, Front));
      CHECK(finding.measured["mismatch"] == "offset");
      CHECK(finding.measured["offsetDifference"] == Json{-16.0, 0.0});
      CHECK(finding.fixes.front().tool == "face_attributes_copy");
      CHECK(finding.fixes.front().neighbourAsSource);
      CHECK(finding.fixes[1].arguments["offsetBy"] == Json{16.0, 0.0});

      const auto& ids = fixture.server().state().documentState(document).ids;
      const auto json = toJson(finding, ids);
      const auto faceId = [&](mdl::BrushNode* node) {
        return fixture.id(*node)
               + "/face:" + std::to_string(faceOf(node, Front).faceIndex());
      };
      CHECK(json["code"] == "UV_SEAM");
      CHECK(json["face"] == faceId(left));
      CHECK(json["brush"] == fixture.id(*left));
      CHECK(json["material"] == "k_tile");
      CHECK(json["neighbour"] == faceId(right));
      CHECK(json["fix"]["arguments"]["source"] == json["neighbour"]);
      CHECK(json["fix"]["arguments"]["ids"] == Json{json["face"]});
      CHECK(json["fix"]["arguments"]["mode"] == "project");
      CHECK(json["alternatives"].size() == 1);
    }

    SECTION("different scale")
    {
      auto* right =
        addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile", {.scale = {0.5f, 0.5f}});
      const auto findings =
        checkUv({faceOf(right, Front)}, map, profiles.provider(), {UvIssue::Seam});
      REQUIRE(findings.size() == 1);
      CHECK(findings.front().measured["mismatch"] == "scaleOrRotation");
      CHECK(
        findings.front().message
        == "'k_tile' does not continue across the edge to its coplanar neighbour: the "
           "scale differs (0.5 x 0.5 vs 1 x 1).");
    }

    SECTION("different rotation")
    {
      auto* right =
        addCuboid(map, {{64, 0, 0}, {128, 16, 64}}, "k_tile", {.rotation = 90.0f});
      const auto findings =
        checkUv({faceOf(right, Front)}, map, profiles.provider(), {UvIssue::Seam});
      REQUIRE(findings.size() == 1);
      CHECK(
        findings.front().message
        == "'k_tile' does not continue across the edge to its coplanar neighbour: the "
           "rotation differs (90 vs 0 degrees).");
      CHECK(findings.front().measured["rotation"] == 90.0);
      CHECK(findings.front().measured["neighbourRotation"] == 0.0);
    }

    SECTION("different texture axes with the same scale and rotation")
    {
      // a sheared Valve 220 face keeps its scale and rotation but not its axes
      auto brush =
        brushBuilder(map).createCuboid({{64, 0, 0}, {128, 16, 64}}, "k_tile").value();
      const auto frontIndex = brush.findFace(Front);
      REQUIRE(frontIndex);
      brush.face(*frontIndex).shearUv({0.5f, 0.0f});
      auto* right =
        static_cast<mdl::BrushNode*>(addBrushes(map, {std::move(brush)}).front());
      REQUIRE(right->brush().face(*frontIndex).uvAttributes().scale == vm::vec2f{1, 1});

      const auto findings =
        checkUv({faceOf(right, Front)}, map, profiles.provider(), {UvIssue::Seam});
      REQUIRE(findings.size() == 1);
      const auto& message = findings.front().message;
      CHECK(message.find("the texture axes differ") != std::string::npos);
      CHECK(message.find("1 x 1 vs 1 x 1") == std::string::npos);
    }

    SECTION("different materials have no seam")
    {
      auto* right = addCuboid(
        map, {{64, 0, 0}, {128, 16, 64}}, "{k_decal", {.offset = {16.0f, 0.0f}});
      CHECK(checkUv({faceOf(right, Front)}, map, profiles.provider(), {UvIssue::Seam})
              .empty());
    }
  }

  SECTION("skipped faces")
  {
    SECTION("tool materials")
    {
      auto* clip =
        addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "clip", {.scale = {1.0f, 3.0f}});
      CHECK(checkUv(mdl::toHandles(clip), map, profiles.provider()).empty());
    }

    SECTION("size-dependent checks need a loaded material")
    {
      auto* missing =
        addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "missing", {.scale = {1.0f, 3.0f}});
      const auto findings = checkUv({faceOf(missing, Front)}, map, profiles.provider());
      CHECK(issuesOf(findings) == std::vector{UvIssue::AspectDistortion});
    }
  }

  SECTION("findings are grouped by face and deduplicated")
  {
    auto* panel =
      addCuboid(map, {{0, 0, 0}, {48, 16, 48}}, "k_panel", {.offset = {5.0f, 0.0f}});
    const auto front = faceOf(panel, Front);
    const auto findings = checkUv({front, front}, map, profiles.provider());
    CHECK(
      issuesOf(findings)
      == std::vector{UvIssue::FractionalRepeat, UvIssue::PanelNotAligned});
    CHECK(countOf(findings, UvIssue::FractionalRepeat) == 1);
  }

  SECTION("profileProvider analyzes the image when few map samples decide the kind")
  {
    // twelve k_panel faces with 2.5 repeats make the map call it a tile
    for (size_t i = 0; i < 2; ++i)
    {
      addCuboid(
        map,
        {{double(i) * 100.0, 0, 0}, {double(i) * 100.0 + 48, 16, 48}},
        "k_panel",
        {.scale = {0.3f, 0.3f}});
    }
    auto knowledge = MaterialKnowledge{map, std::nullopt};
    const auto provider = profileProvider(knowledge);
    const auto& profile = provider("k_panel");
    CHECK(profile.kind.source == "map");
    REQUIRE(profile.image);
    CHECK(checkedKind(profile) == MaterialKind::Panel);
  }
}

} // namespace tb::mcp
