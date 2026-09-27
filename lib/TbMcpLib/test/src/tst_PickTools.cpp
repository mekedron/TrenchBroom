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
#include "mcp/AgentCamera.h"
#include "mcp/CameraProjection.h"
#include "mcp/JsonVm.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Session.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameManager.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/result.h"

#include "vm/vec.h"

#include <functional>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::Node* findNode(
  mdl::Node& node, const std::function<bool(const mdl::Node&)>& predicate)
{
  if (predicate(node))
  {
    return &node;
  }
  for (auto* child : node.children())
  {
    if (auto* result = findNode(*child, predicate))
    {
      return result;
    }
  }
  return nullptr;
}

/** The brush whose bounds have the given min corner. */
mdl::BrushNode* findBrush(mdl::Map& map, const vm::vec3d& min)
{
  auto* node = findNode(map.worldNode(), [&](const mdl::Node& n) {
    return dynamic_cast<const mdl::BrushNode*>(&n) && n.logicalBounds().min == min;
  });
  REQUIRE(node);
  return static_cast<mdl::BrushNode*>(node);
}

ui::MapDocument& loadRooms(McpToolFixture& fixture)
{
  const auto* gameInfo = fixture.host().gameManager().gameInfo("Quake");
  REQUIRE(gameInfo);
  return fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = *gameInfo});
}

AgentCamera cameraFromJson(const Json& json)
{
  auto camera = AgentCamera{};
  camera.projection = json["projection"] == "perspective"
                        ? CameraProjection::Perspective
                        : CameraProjection::Orthographic;
  camera.position = *vec3FromJson(json["position"]);
  camera.direction = *vec3FromJson(json["direction"]);
  camera.up = *vec3FromJson(json["up"]);
  camera.fov = json.value("fov", 90.0);
  camera.zoom = json.value("zoom", 1.0);
  camera.nearPlane = json["near"].get<double>();
  camera.farPlane = json["far"].get<double>();
  return camera;
}

/** The pixel that the world point projects to in the image of a snapshot result. */
Json pixelOf(const Json& snapshot, const vm::vec3d& point)
{
  const auto projection = ImageProjection::create(
                            cameraFromJson(snapshot["camera"]),
                            snapshot["image"]["width"].get<size_t>(),
                            snapshot["image"]["height"].get<size_t>())
                          | kdl::value();
  const auto projected = projection.project(point);
  REQUIRE(projected.inFront);
  return Json{{"x", int64_t(projected.x)}, {"y", int64_t(projected.y)}};
}

/** Looks east (+x) at the west face of the first pillar (x = 192, y 192..224). */
Json pillarCamera()
{
  return Json{{"position", {100, 208, 128}}, {"direction", {1, 0, 0}}, {"fov", 90}};
}

} // namespace

TEST_CASE("PickTools")
{
  auto fixture = McpToolFixture{};
  auto& document = loadRooms(fixture);
  auto& map = document.map();

  auto* pillar = findBrush(map, vm::vec3d{192, 192, 0});
  auto* floor = findBrush(map, vm::vec3d{-16, -16, -16});
  auto* ceiling = findBrush(map, vm::vec3d{-16, -16, 256});
  const auto undoCount = map.commandProcessor().undoCommandNames().size();

  const auto snapshot = [&](Json args) {
    args["width"] = args.value("width", 320);
    args["height"] = args.value("height", 240);
    return fixture.call("view_snapshot", std::move(args));
  };

  SECTION("view_pick")
  {
    const auto shot = snapshot(Json{{"camera", pillarCamera()}});
    const auto id = shot["snapshotId"].get<std::string>();
    CHECK(id.starts_with("snap:"));

    SECTION("A face center")
    {
      const auto faceCenter = vm::vec3d{192, 208, 128};
      const auto pixel = pixelOf(shot, faceCenter);
      const auto result =
        fixture.call("view_pick", Json{{"snapshot", id}, {"pixel", pixel}});
      CHECK(result["snapshot"] == id);
      CHECK(result["width"] == 320);
      CHECK(result["height"] == 240);
      REQUIRE(result["picks"].size() == 1);
      CHECK(result["picks"][0]["pixel"] == pixel);

      const auto& hit = result["hit"];
      REQUIRE(hit.is_object());
      CHECK(fixture.node(hit["object"].get<std::string>()) == pillar);
      CHECK(hit["kind"] == "brush");
      CHECK(hit["normal"] == Json::array({-1, 0, 0}));
      CHECK(hit["material"] == "pillar_stone");
      const auto faceIndex = hit["faceIndex"].get<size_t>();
      CHECK(
        hit["face"]
        == hit["object"].get<std::string>() + "/face:" + std::to_string(faceIndex));
      CHECK(pillar->brush().face(faceIndex).normal() == vm::vec3d{-1, 0, 0});
      CHECK(hit["point"][0].get<double>() == Catch::Approx(192.0));
      CHECK(hit["point"][1].get<double>() == Catch::Approx(208.0).margin(1.0));
      CHECK(hit["point"][2].get<double>() == Catch::Approx(128.0).margin(1.0));
      CHECK(hit["distance"].get<double>() == Catch::Approx(92.0).margin(0.5));
      CHECK(hit["depth"].get<double>() == Catch::Approx(92.0).margin(0.1));
      CHECK(hit["group"].is_string());
      CHECK(fixture.node(hit["group"].get<std::string>()) == pillar->parent());
      CHECK(hit["layer"].is_string());
      CHECK(hit["bounds"] == toJson(pillar->logicalBounds()));
      CHECK(result["picks"][0]["ray"]["origin"] == Json::array({100, 208, 128}));
    }

    SECTION("Several pixels, floor and ceiling")
    {
      const auto result = fixture.call(
        "view_pick",
        Json{
          {"snapshot", id},
          {"pixels",
           {Json{{"x", 160}, {"y", 120}},
            Json{{"x", 5}, {"y", 235}},
            Json{{"x", 5}, {"y", 3}}}}});
      CHECK(!result.contains("hit"));
      REQUIRE(result["picks"].size() == 3);
      CHECK(
        fixture.node(result["picks"][0]["hit"]["object"].get<std::string>()) == pillar);
      const auto& floorHit = result["picks"][1]["hit"];
      CHECK(fixture.node(floorHit["object"].get<std::string>()) == floor);
      CHECK(floorHit["normal"] == Json::array({0, 0, 1}));
      CHECK(floorHit["group"].is_null());
      const auto& ceilingHit = result["picks"][2]["hit"];
      CHECK(fixture.node(ceilingHit["object"].get<std::string>()) == ceiling);
      CHECK(ceilingHit["normal"] == Json::array({0, 0, -1}));
    }

    SECTION("Pick options")
    {
      const auto pixel = Json{{"x", 160}, {"y", 120}};
      const auto pillarId = fixture.id(*pillar);
      // looking through the pillar: the second pillar is not in line, the east wall is
      const auto through = fixture.call(
        "view_pick", Json{{"snapshot", id}, {"pixel", pixel}, {"ignore", {pillarId}}});
      REQUIRE(through["hit"].is_object());
      CHECK(through["hit"]["object"] != pillarId);
      CHECK(through["hit"]["point"][0].get<double>() == Catch::Approx(512.0));

      const auto near = fixture.call(
        "view_pick", Json{{"snapshot", id}, {"pixel", pixel}, {"maxDistance", 50}});
      CHECK(near["hit"].is_null());

      const auto entities = fixture.call(
        "view_pick", Json{{"snapshot", id}, {"pixel", pixel}, {"kinds", {"entity"}}});
      CHECK(entities["hit"].is_null());
    }

    SECTION("Errors")
    {
      auto error = fixture.callExpectingError(
        "view_pick", Json{{"snapshot", "snap:999"}, {"pixel", {{"x", 1}, {"y", 1}}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.hint.find(id) != std::string::npos);

      error = fixture.callExpectingError(
        "view_pick", Json{{"snapshot", id}, {"pixel", {{"x", 320}, {"y", 1}}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      error = fixture.callExpectingError(
        "view_pick", Json{{"snapshot", id}, {"pixel", {{"x", 0}, {"y", 240}}}});
      CHECK(error.code == ErrorCode::InvalidArgument);

      error = fixture.callExpectingError("view_pick", Json{{"snapshot", id}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      error = fixture.callExpectingError(
        "view_pick",
        Json{
          {"snapshot", id},
          {"pixel", {{"x", 0}, {"y", 0}}},
          {"pixels", {Json{{"x", 0}, {"y", 0}}}}});
      CHECK(error.code == ErrorCode::InvalidArgument);

      // snapshots belong to the session
      const auto other = fixture.openSession();
      error = fixture.callExpectingErrorAs(
        other, "view_pick", Json{{"snapshot", id}, {"pixel", {{"x", 1}, {"y", 1}}}});
      CHECK(error.code == ErrorCode::InvalidArgument);
    }

    CHECK(map.commandProcessor().undoCommandNames().size() == undoCount);
  }

  SECTION("A miss")
  {
    const auto shot = snapshot(
      Json{{"camera", Json{{"position", {3000, 3000, 3000}}, {"direction", {1, 0, 0}}}}});
    const auto result = fixture.call(
      "view_pick",
      Json{{"snapshot", shot["snapshotId"]}, {"pixel", {{"x", 160}, {"y", 120}}}});
    CHECK(result["hit"].is_null());
    CHECK(result["picks"][0]["hit"].is_null());
  }

  SECTION("Only objects the snapshot drew")
  {
    auto* light = findNode(map.worldNode(), [](const mdl::Node& n) {
      const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&n);
      return entityNode && entityNode->entity().classname() == "light";
    });
    REQUIRE(light);
    const auto shot =
      snapshot(Json{{"camera", pillarCamera()}, {"isolate", {fixture.id(*light)}}});
    const auto pixel = Json{{"x", 160}, {"y", 120}};

    const auto isolated =
      fixture.call("view_pick", Json{{"snapshot", shot["snapshotId"]}, {"pixel", pixel}});
    CHECK(isolated["hit"].is_null());

    const auto all = fixture.call(
      "view_pick",
      Json{{"snapshot", shot["snapshotId"]}, {"pixel", pixel}, {"includeHidden", true}});
    CHECK(fixture.node(all["hit"]["object"].get<std::string>()) == pillar);
  }

  SECTION("Kept snapshots and other snapshot tools")
  {
    const auto shot = snapshot(Json{{"camera", pillarCamera()}, {"keepAs", "pillar"}});
    const auto kept = fixture.call(
      "view_pick", Json{{"snapshot", "pillar"}, {"pixel", {{"x", 160}, {"y", 120}}}});
    CHECK(fixture.node(kept["hit"]["object"].get<std::string>()) == pillar);

    // an orthographic view from the top sees the ceiling
    const auto top = snapshot(
      Json{{"camera", Json{{"view", "top"}, {"center", {256, 256, 0}}, {"zoom", 0.5}}}});
    const auto fromTop = fixture.call(
      "view_pick",
      Json{{"snapshot", top["snapshotId"]}, {"pixel", {{"x", 160}, {"y", 120}}}});
    CHECK(fixture.node(fromTop["hit"]["object"].get<std::string>()) == ceiling);
    CHECK(fromTop["hit"]["normal"] == Json::array({0, 0, 1}));

    // the plan view cuts at its height and sees the floor
    const auto plan = fixture.call(
      "map_plan_view",
      Json{
        {"format", "image"},
        {"region", Json{{"min", {0, 0, 0}}, {"max", {512, 512, 256}}}},
        {"height", 64},
        {"cellSize", 16}});
    REQUIRE(plan.contains("snapshotId"));
    const auto fromPlan = fixture.call(
      "view_pick",
      Json{
        {"snapshot", plan["snapshotId"]},
        {"pixel", {{"x", 20}, {"y", plan["image"]["height"].get<int>() - 20}}}});
    REQUIRE(fromPlan["hit"].is_object());
    CHECK(fixture.node(fromPlan["hit"]["object"].get<std::string>()) == floor);

    const auto around = fixture.call(
      "view_snapshots_around",
      Json{
        {"ids", {fixture.id(*pillar)}},
        {"views", {"north", "top"}},
        {"width", 64},
        {"height", 48}});
    REQUIRE(around["images"].size() == 2);
    const auto firstId = around["images"][0]["snapshotId"].get<std::string>();
    const auto secondId = around["images"][1]["snapshotId"].get<std::string>();
    CHECK(firstId != secondId);
    CHECK(Json(firstId) != shot["snapshotId"]);
    const auto fromAround = fixture.call(
      "view_pick", Json{{"snapshot", firstId}, {"pixel", {{"x", 32}, {"y", 24}}}});
    CHECK(fixture.node(fromAround["hit"]["object"].get<std::string>()) == pillar);
  }

  SECTION("The most recent snapshots are remembered")
  {
    const auto first = snapshot(Json{{"width", 16}, {"height", 16}})["snapshotId"];
    for (size_t i = 0; i < Session::MaxSnapshotRecords; ++i)
    {
      snapshot(Json{{"width", 16}, {"height", 16}});
    }
    CHECK(
      fixture
        .callExpectingError(
          "view_pick", Json{{"snapshot", first}, {"pixel", {{"x", 1}, {"y", 1}}}})
        .code
      == ErrorCode::InvalidArgument);
  }
}

} // namespace tb::mcp
