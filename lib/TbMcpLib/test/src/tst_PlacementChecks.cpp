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
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/PlacementChecks.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox_io.h"
#include "vm/plane.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const auto ModelCodes = Json::array(
  {"MODEL_BELOW_FLOOR", "MODEL_FLOATING", "MODEL_PENETRATES_BRUSHES", "MODEL_NO_FLOOR"});

std::string createBox(
  McpToolFixture& fixture,
  const Json& min,
  const Json& max,
  const std::string& material = {},
  const bool dryRun = false)
{
  auto args = Json{{"min", min}, {"max", max}};
  if (!material.empty())
  {
    args["material"] = material;
  }
  if (dryRun)
  {
    args["dryRun"] = true;
  }
  return fixture.call("brush_create_box", args)["result"]["brush"].get<std::string>();
}

/** The introduced issues of the given code. */
std::vector<Json> issuesOf(const Json& result, const std::string& code)
{
  auto issues = std::vector<Json>{};
  for (const auto& issue : result["issuesIntroduced"])
  {
    if (issue["code"] == code)
    {
      issues.push_back(issue);
    }
  }
  return issues;
}

bool hasWarning(const Json& result, const std::string& code)
{
  for (const auto& warning : result["warnings"])
  {
    if (warning["code"] == code)
    {
      return true;
    }
  }
  return false;
}

const mdl::BrushNode* brushNode(McpToolFixture& fixture, const std::string& id)
{
  return dynamic_cast<const mdl::BrushNode*>(fixture.node(id));
}

std::string brushOf(const Json& faceId)
{
  const auto id = faceId.get<std::string>();
  return id.substr(0, id.find("/face:"));
}

/** Six walls around the box (min, max) with the given thickness. */
void createRoom(
  McpToolFixture& fixture, const vm::vec3d& min, const vm::vec3d& max, double t = 16.0)
{
  const auto box = [&](const vm::vec3d& a, const vm::vec3d& b) {
    createBox(fixture, Json{a.x(), a.y(), a.z()}, Json{b.x(), b.y(), b.z()});
  };
  box({min.x() - t, min.y() - t, min.z() - t}, {max.x() + t, max.y() + t, min.z()});
  box({min.x() - t, min.y() - t, max.z()}, {max.x() + t, max.y() + t, max.z() + t});
  box({min.x() - t, min.y() - t, min.z()}, {min.x(), max.y() + t, max.z()});
  box({max.x(), min.y() - t, min.z()}, {max.x() + t, max.y() + t, max.z()});
  box({min.x(), min.y() - t, min.z()}, {max.x(), min.y(), max.z()});
  box({min.x(), max.y(), min.z()}, {max.x(), max.y() + t, max.z()});
}

void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

} // namespace

TEST_CASE("PlacementChecks")
{
  auto fixture = McpToolFixture{};

  SECTION("isToolMaterial")
  {
    for (const auto* name :
         {"clip",
          "CLIP",
          "AAATRIGGER",
          "ORIGIN",
          "SKIP",
          "NULL",
          "HINT",
          "BEVEL",
          "trigger",
          "e1u1/clip",
          "e1u1/trigger",
          "common/caulk",
          "common/nodraw",
          "common/weapclip",
          "CLIPBEVEL"})
    {
      CAPTURE(name);
      CHECK(isToolMaterial(name));
    }
    for (const auto* name : {"k_tile", "sky1", "*water0", "base_wall/concrete", ""})
    {
      CAPTURE(name);
      CHECK_FALSE(isToolMaterial(name));
    }
  }

  SECTION("findZFighting")
  {
    auto& document = fixture.create();
    auto& map = document.map();
    const auto slab = createBox(fixture, {0, 0, 0}, {128, 128, 16});

    SECTION("coplanar overlapping faces facing the same way")
    {
      const auto inset = createBox(fixture, {32, 32, 8}, {96, 96, 16});

      const auto all = findZFighting(map);
      REQUIRE(all.size() == 1);
      CHECK(all[0].area == vm::approx{4096.0});
      CHECK(all[0].center == vm::approx{vm::vec3d{64, 64, 16}});
      CHECK(all[0].plane.normal == vm::approx{vm::vec3d{0, 0, 1}});
      CHECK(all[0].plane.distance == vm::approx{16.0});

      const auto brushes = std::vector{brushNode(fixture, inset)};
      const auto some = findZFighting(map, &brushes);
      REQUIRE(some.size() == 1);
      CHECK(some[0].first.node() == brushes[0]);
      CHECK(fixture.id(*some[0].second.node()) == slab);

      auto& ids = fixture.server().state().documentState(document).ids;
      const auto issues = zFightingIssues(some, ids);
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].code == "Z_FIGHTING");
      CHECK(brushOf(issues[0].details["faces"][0]) == inset);
      CHECK(brushOf(issues[0].details["faces"][1]) == slab);
      CHECK(issues[0].details["area"] == 4096);
      CHECK(issues[0].details["center"] == Json{64, 64, 16});
    }

    SECTION("a touching face of another brush facing the other way hides the overlap")
    {
      createBox(fixture, {32, 32, 8}, {96, 96, 16});
      createBox(fixture, {0, 0, 16}, {128, 128, 32});
      CHECK(findZFighting(map).empty());
    }

    SECTION("a partly hiding face does not hide it")
    {
      createBox(fixture, {32, 32, 8}, {96, 96, 16});
      createBox(fixture, {0, 0, 16}, {64, 128, 32});
      const auto found = findZFighting(map);
      REQUIRE(found.size() == 1);
      CHECK(found[0].area == vm::approx{4096.0});
    }

    SECTION("tool materials are ignored")
    {
      createBox(fixture, {32, 32, 8}, {96, 96, 16}, "clip");
      createBox(fixture, {32, 32, 8}, {96, 96, 16}, "common/caulk");
      CHECK(findZFighting(map).empty());
    }

    SECTION("trigger brushes are ignored")
    {
      const auto trigger = createBox(fixture, {32, 32, 8}, {96, 96, 16});
      fixture.call(
        "entity_create_brush", Json{{"classname", "trigger_once"}, {"ids", {trigger}}});
      CHECK(findZFighting(map).empty());
    }

    SECTION("other planes, faces facing each other and touching edges are no z-fighting")
    {
      createBox(fixture, {32, 32, 4}, {96, 96, 12});
      createBox(fixture, {0, 0, -16}, {128, 128, 0});
      createBox(fixture, {128, 0, 8}, {192, 64, 16});
      createBox(fixture, {256, 0, 8}, {320, 64, 16});
      CHECK(findZFighting(map).empty());
    }
  }

  SECTION("issuesIntroduced reports z-fighting")
  {
    fixture.create();
    const auto slab = createBox(fixture, {0, 0, 0}, {128, 128, 16});

    // a call that creates a brush z-fighting with an existing one
    auto result =
      fixture.call("brush_create_box", Json{{"min", {32, 32, 8}}, {"max", {96, 96, 16}}});
    const auto inset = result["result"]["brush"].get<std::string>();
    auto issues = issuesOf(result, "Z_FIGHTING");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["source"] == "mcp");
    CHECK(issues[0]["type"] == "Z-fighting");
    CHECK(brushOf(issues[0]["objectId"]) == inset);
    CHECK(brushOf(issues[0]["details"]["faces"][0]) == inset);
    CHECK(brushOf(issues[0]["details"]["faces"][1]) == slab);
    CHECK(issues[0]["details"]["area"] == 4096);

    // an unrelated call does not report it again
    result = fixture.call(
      "brush_create_box", Json{{"min", {512, 0, 0}}, {"max", {576, 64, 64}}});
    const auto other = result["result"]["brush"].get<std::string>();
    CHECK(issuesOf(result, "Z_FIGHTING").empty());

    // changing one of the brushes keeps the pair: not introduced
    result = fixture.call("objects_move", Json{{"ids", {other}}, {"vector", {64, 0, 0}}});
    CHECK(issuesOf(result, "Z_FIGHTING").empty());
    result =
      fixture.call("material_apply", Json{{"ids", {inset}}, {"material", "other"}});
    CHECK(issuesOf(result, "Z_FIGHTING").empty());

    // a dry run reports it
    result = fixture.call(
      "brush_create_box",
      Json{{"min", {16, 16, 8}}, {"max", {32, 32, 16}}, {"dryRun", true}});
    CHECK(result["dryRun"] == true);
    CHECK(issuesOf(result, "Z_FIGHTING").size() == 1);

    // moving an existing brush into z-fighting reports it
    result =
      fixture.call("objects_move", Json{{"ids", {other}}, {"vector", {-576, 0, -48}}});
    issues = issuesOf(result, "Z_FIGHTING");
    REQUIRE_FALSE(issues.empty());
    CHECK(brushOf(issues[0]["objectId"]) == other);
  }

  SECTION("issuesIntroduced reports model placement")
  {
    fixture.create({.mapFormat = mdl::MapFormat::Valve, .gameInfo = mdl::QuakeGameInfo});
    fixture.call(
      "entity_definitions_set",
      {{"type", "external"},
       {"path", (getFixtureRoot() / "test" / "mcp" / "models.fgd").string()}});
    createBox(fixture, {-128, -128, -16}, {128, 128, 0});

    auto result = fixture.call(
      "entity_create_point",
      {{"classname", "monster_person"},
       {"position", {0, 0, 24}},
       {"snapToGrid", false},
       {"properties", {{"sequence", 0}}}});
    const auto person = result["result"]["entity"].get<std::string>();
    CHECK(result["issuesIntroduced"] == Json::array());

    // floating after a property change
    result = fixture.call(
      "entity_properties_set",
      Json{{"ids", {person}}, {"properties", {{"origin", "0 0 100"}}}});
    auto issues = issuesOf(result, "MODEL_FLOATING");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["objectId"] == person);
    CHECK(issues[0]["source"] == "mcp");
    CHECK(
      issues[0]["details"]["modelBounds"]
      == Json{{"min", {-16, -16, 76}}, {"max", {16, 16, 140}}});
    CHECK(issues[0]["details"]["distance"] == 76);

    result = fixture.call(
      "entity_properties_set",
      Json{{"ids", {person}}, {"properties", {{"origin", "0 0 24"}}}});
    CHECK(issuesOf(result, "MODEL_FLOATING").empty());

    // objects_move warns itself, so the issue is not repeated
    result =
      fixture.call("objects_move", Json{{"ids", {person}}, {"vector", {0, 0, 32}}});
    CHECK(hasWarning(result, "MODEL_FLOATING"));
    CHECK(issuesOf(result, "MODEL_FLOATING").empty());
    fixture.call("objects_move", Json{{"ids", {person}}, {"vector", {0, 0, -32}}});

    // a new brush through the model
    result =
      fixture.call("brush_create_box", Json{{"min", {-8, -8, 16}}, {"max", {8, 8, 32}}});
    const auto pillar = result["result"]["brush"].get<std::string>();
    issues = issuesOf(result, "MODEL_PENETRATES_BRUSHES");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["objectId"] == person);
    CHECK(issues[0]["details"]["relatedIds"] == Json{pillar});

    // unrelated calls do not report it again
    result = fixture.call(
      "brush_create_box", Json{{"min", {512, 0, 0}}, {"max", {576, 64, 64}}});
    CHECK(issuesOf(result, "MODEL_PENETRATES_BRUSHES").empty());

    // lights may hang anywhere, like map_check says: a light shown with a model high
    // above the floor and reaching into a brush is fine
    result = fixture.call(
      "entity_create_point",
      {{"classname", "light_lamp"}, {"position", {64, 64, 100}}, {"snapToGrid", false}});
    const auto lamp = result["result"]["entity"].get<std::string>();
    const auto noModelIssues = [&](const Json& callResult) {
      for (const auto& code : ModelCodes)
      {
        CAPTURE(code);
        CHECK(issuesOf(callResult, code.get<std::string>()).empty());
      }
    };
    noModelIssues(result);
    result = fixture.call(
      "brush_create_box", Json{{"min", {48, 48, 120}}, {"max", {80, 80, 136}}});
    noModelIssues(result);
    CHECK(
      fixture.call("issues_list", {{"codes", ModelCodes}, {"ids", {lamp}}})["total"]
      == 0);
  }

  SECTION("sameIssue")
  {
    const auto uvIssue = [](const std::string& brush, const vm::plane3d& plane) {
      auto issue = McpIssue{};
      issue.code = std::string{UvDistortionCode};
      issue.details = Json{{"brush", brush}, {"material", "k_tile"}};
      issue.signature = fmt::format(
        "UV|{}|{},{},{}|{}",
        brush,
        plane.normal.x(),
        plane.normal.y(),
        plane.normal.z(),
        plane.distance);
      issue.plane = plane;
      return issue;
    };
    const auto up = vm::vec3d{0, 0, 1};
    const auto tilted = vm::normalize(vm::vec3d{0, 0.1, 1});
    const auto before = uvIssue("brush:1", {16, up});
    // snapped vertices move the plane or split the face: the same issue
    CHECK(sameIssue(before, uvIssue("brush:1", {16.4, up})));
    CHECK(sameIssue(before, uvIssue("brush:1", {16, tilted})));
    // another brush, material or direction
    CHECK_FALSE(sameIssue(before, uvIssue("brush:2", {16, up})));
    auto otherMaterial = uvIssue("brush:1", {16, up});
    otherMaterial.details["material"] = "k_stone";
    otherMaterial.signature += "|k_stone";
    CHECK_FALSE(sameIssue(before, otherMaterial));
    CHECK_FALSE(sameIssue(before, uvIssue("brush:1", {16, vm::vec3d{1, 0, 0}})));

    const auto zIssue = [](const Json& brushes, const double distance) {
      auto issue = McpIssue{};
      issue.code = std::string{ZFightingCode};
      issue.details = Json{{"brushes", brushes}};
      issue.signature = fmt::format("Z|{}|{}", brushes.dump(), distance);
      issue.plane = vm::plane3d{distance, vm::vec3d{0, 0, 1}};
      return issue;
    };
    const auto pair = zIssue({"brush:1", "brush:2"}, 16);
    CHECK(sameIssue(pair, zIssue({"brush:2", "brush:1"}, 16.5)));
    CHECK_FALSE(sameIssue(pair, zIssue({"brush:1", "brush:2"}, 32)));
    CHECK_FALSE(sameIssue(pair, zIssue({"brush:1", "brush:3"}, 16)));

    // other issues match by signature
    auto model = McpIssue{};
    model.code = "MODEL_FLOATING";
    model.signature = "MODEL_FLOATING|entity:1|brush:1";
    auto other = model;
    CHECK(sameIssue(model, other));
    other.signature = "MODEL_FLOATING|entity:1|brush:2";
    CHECK_FALSE(sameIssue(model, other));
  }

  SECTION("issuesIntroduced ignores existing face issues whose plane moved slightly")
  {
    fixture.create();
    const auto slab = createBox(fixture, {0, 0, 0}, {128, 128, 16});
    const auto inset = createBox(fixture, {32, 32, 8}, {96, 96, 16});

    // like Snap Vertices, a call moves both brushes by less than a unit: the pair still
    // z-fights, but it is not new
    const auto result =
      fixture.call("objects_move", Json{{"ids", {slab, inset}}, {"vector", {0, 0, 0.5}}});
    CHECK(issuesOf(result, "Z_FIGHTING").empty());
  }

  SECTION("issuesIntroduced reports stretched textures")
  {
    fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
    auto* document = fixture.host().documentList.back().document;
    const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad";
    fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
    processResources(document->map());

    auto result = fixture.call(
      "brush_create_box",
      Json{{"min", {0, 0, 0}}, {"max", {128, 128, 128}}, {"material", "k_tile"}});
    const auto box = result["result"]["brush"].get<std::string>();
    CHECK(issuesOf(result, "UV_ASPECT_DISTORTION").empty());

    result = fixture.call(
      "objects_scale",
      Json{{"ids", {box}}, {"factors", {1, 1, 2}}, {"alignmentLock", true}});
    const auto issues = issuesOf(result, "UV_ASPECT_DISTORTION");
    REQUIRE(!issues.empty());
    CHECK(issues[0]["source"] == "mcp");
    CHECK(brushOf(issues[0]["objectId"]) == box);
    CHECK(issues[0]["details"]["material"] == "k_tile");
    CHECK(issues[0]["details"].contains("fix"));

    // moving the brush by half a unit keeps the distortion: not introduced again
    result = fixture.call("objects_move", Json{{"ids", {box}}, {"vector", {0, 0, 0.5}}});
    CHECK(issuesOf(result, "UV_ASPECT_DISTORTION").empty());
  }

  SECTION("issuesIntroduced reports entities outside the hull")
  {
    fixture.create();
    createRoom(fixture, {-256, -256, 0}, {256, 256, 192});

    auto result = fixture.call(
      "entity_create_point", Json{{"classname", "light"}, {"position", {0, 0, 64}}});
    const auto light = result["result"]["entity"].get<std::string>();
    CHECK(issuesOf(result, "ENTITY_OUTSIDE_HULL").empty());

    // moved outside the hull
    result =
      fixture.call("objects_move", Json{{"ids", {light}}, {"vector", {1024, 0, 0}}});
    auto issues = issuesOf(result, "ENTITY_OUTSIDE_HULL");
    REQUIRE(issues.size() == 1);
    CHECK(issues[0]["objectId"] == light);
    CHECK(issues[0]["details"]["position"] == Json{1024, 0, 64});

    // a later unrelated call does not report it again
    result =
      fixture.call("brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {32, 32, 32}}});
    CHECK(issuesOf(result, "ENTITY_OUTSIDE_HULL").empty());

    // back inside, then a dry run that removes a wall
    fixture.call("objects_move", Json{{"ids", {light}}, {"vector", {-1024, 0, 0}}});
    result = fixture.call(
      "entity_create_point",
      Json{{"classname", "light"}, {"position", {2048, 0, 64}}, {"dryRun", true}});
    CHECK(issuesOf(result, "ENTITY_OUTSIDE_HULL").size() == 1);
  }
}

TEST_CASE("PlacementChecks per-call overhead", "[.][benchmark]")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  // 2,000 brushes: 5 sealed rooms with a grid of pillars
  auto brushes = std::vector<mdl::Brush>{};
  const auto add = [&](const vm::bbox3d& box) {
    brushes.push_back(brushBuilder(map).createCuboid(box, "wall").value());
  };
  for (int room = 0; room < 5; ++room)
  {
    const auto x0 = double(room) * 1200.0;
    const auto min = vm::vec3d{x0, 0, 0};
    const auto max = vm::vec3d{x0 + 1024, 1024, 256};
    const auto t = 16.0;
    add({{min.x() - t, min.y() - t, min.z() - t}, {max.x() + t, max.y() + t, min.z()}});
    add({{min.x() - t, min.y() - t, max.z()}, {max.x() + t, max.y() + t, max.z() + t}});
    add({{min.x() - t, min.y() - t, min.z()}, {min.x(), max.y() + t, max.z()}});
    add({{max.x(), min.y() - t, min.z()}, {max.x() + t, max.y() + t, max.z()}});
    add({{min.x(), min.y() - t, min.z()}, {max.x(), min.y(), max.z()}});
    add({{min.x(), max.y(), min.z()}, {max.x(), max.y() + t, max.z()}});
  }
  for (int room = 0; room < 5; ++room)
  {
    for (int i = 0; i < 20; ++i)
    {
      for (int j = 0; j < 20 && brushes.size() < 2000; ++j)
      {
        const auto x = double(room) * 1200.0 + 16.0 + double(i) * 48.0;
        const auto y = 16.0 + double(j) * 48.0;
        add({{x, y, 0}, {x + 16, y + 16, 64}});
      }
    }
  }
  addBrushes(map, std::move(brushes));
  fixture.call(
    "entity_create_point", Json{{"classname", "light"}, {"position", {512, 512, 128}}});

  const auto measure = [](const auto& f) {
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
  };

  const auto create = measure([&]() {
    fixture.call(
      "brush_create_box", Json{{"min", {100, 100, 100}}, {"max", {132, 132, 132}}});
  });
  const auto move = measure([&]() {
    fixture.call(
      "objects_move",
      Json{
        {"ids", {fixture.id(*map.worldNode().defaultLayer()->children().back())}},
        {"vector", {0, 0, 16}}});
  });
  auto pillars = Json::array();
  for (const auto* node : map.worldNode().defaultLayer()->children())
  {
    if (pillars.size() < 400)
    {
      pillars.push_back(fixture.id(*node));
    }
  }
  const auto moveMany = measure([&]() {
    fixture.call("objects_move", Json{{"ids", pillars}, {"vector", {0, 0, 16}}});
  });
  const auto zFightingMap = measure([&]() { findZFighting(map); });
  const auto leaks = measure([&]() { predictLeaks(map); });
  const auto list =
    measure([&]() { fixture.call("issues_list", Json{{"sources", {"mcp"}}}); });

  WARN(
    "2,000 brushes: brush_create_box "
    << create << " ms, objects_move " << move << " ms, objects_move (400 brushes) "
    << moveMany << " ms, findZFighting (map) " << zFightingMap << " ms, predictLeaks "
    << leaks << " ms, issues_list (mcp) " << list << " ms");
}

} // namespace tb::mcp
