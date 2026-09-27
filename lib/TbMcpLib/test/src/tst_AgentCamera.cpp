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
#include "mcp/McpToolFixture.h"
#include "mdl/GameConfig.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "ui/MapDocument.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

constexpr auto Epsilon = 1e-9;

bool near(const vm::vec3d& lhs, const vm::vec3d& rhs, const double epsilon = 1e-6)
{
  return vm::length(lhs - rhs) < epsilon;
}

/** Whether every corner of the box is in front of the camera and inside its frustum. */
bool inFrustum(
  const AgentCamera& camera,
  const vm::bbox3d& box,
  const size_t width,
  const size_t height)
{
  const auto right = vm::normalize(vm::cross(camera.direction, camera.up));
  const auto tanVertical = std::tan(camera.fov * vm::Cd::pi() / 360.0) * 0.75;
  const auto tanHorizontal = tanVertical * double(width) / double(height);
  for (size_t i = 0; i < 8; ++i)
  {
    const auto corner = vm::vec3d{
      (i & 1) ? box.max.x() : box.min.x(),
      (i & 2) ? box.max.y() : box.min.y(),
      (i & 4) ? box.max.z() : box.min.z()};
    const auto v = corner - camera.position;
    const auto depth = vm::dot(v, camera.direction);
    if (
      depth <= camera.nearPlane || depth >= camera.farPlane
      || std::abs(vm::dot(v, right)) / depth > tanHorizontal + Epsilon
      || std::abs(vm::dot(v, camera.up)) / depth > tanVertical + Epsilon)
    {
      return false;
    }
  }
  return true;
}

} // namespace

TEST_CASE("AgentCamera")
{
  SECTION("directionFromAngles")
  {
    CHECK(near(directionFromAngles(0, 0), vm::vec3d{1, 0, 0}));
    CHECK(near(directionFromAngles(90, 0), vm::vec3d{0, 1, 0}));
    CHECK(near(directionFromAngles(180, 0), vm::vec3d{-1, 0, 0}));
    CHECK(near(directionFromAngles(0, -90), vm::vec3d{0, 0, -1}));
    CHECK(near(directionFromAngles(0, 90), vm::vec3d{0, 0, 1}));
    CHECK(near(
      directionFromAngles(45, 30),
      vm::vec3d{
        std::cos(vm::Cd::pi() / 6.0) / std::sqrt(2.0),
        std::cos(vm::Cd::pi() / 6.0) / std::sqrt(2.0),
        0.5}));
  }

  SECTION("anglesFromCamera")
  {
    for (const auto [yaw, pitch] :
         {std::pair{0.0, 0.0},
          std::pair{90.0, -30.0},
          std::pair{-135.0, 60.0},
          std::pair{180.0, 10.0}})
    {
      const auto direction = directionFromAngles(yaw, pitch);
      const auto angles = anglesFromCamera(direction, upVector(direction));
      CHECK(angles.yaw == Catch::Approx(yaw));
      CHECK(angles.pitch == Catch::Approx(pitch));
    }

    // vertical: the yaw comes from the up vector
    const auto down = anglesFromCamera(vm::vec3d{0, 0, -1}, vm::vec3d{0, 1, 0});
    CHECK(down.yaw == Catch::Approx(90.0));
    CHECK(down.pitch == Catch::Approx(-90.0));
    const auto up = anglesFromCamera(vm::vec3d{0, 0, 1}, vm::vec3d{-1, 0, 0});
    CHECK(up.yaw == Catch::Approx(0.0).margin(1e-9));
    CHECK(up.pitch == Catch::Approx(90.0));
  }

  SECTION("upVector")
  {
    CHECK(near(upVector(vm::vec3d{1, 0, 0}), vm::vec3d{0, 0, 1}));
    CHECK(near(
      upVector(directionFromAngles(0, -45)),
      vm::vec3d{std::sqrt(0.5), 0, std::sqrt(0.5)}));
    // looking down, the yaw direction is the top of the image
    CHECK(near(upVector(vm::vec3d{0, 0, -1}), vm::vec3d{0, 1, 0}));
    CHECK(near(upVector(vm::vec3d{0, 0, -1}, 0), vm::vec3d{1, 0, 0}));
    // looking up, it is the bottom
    CHECK(near(upVector(vm::vec3d{0, 0, 1}, 0), vm::vec3d{-1, 0, 0}));
  }

  SECTION("lookAtCamera")
  {
    const auto camera = lookAtCamera(vm::vec3d{0, 0, 0}, vm::vec3d{100, 0, 100}, 60);
    CHECK(camera.projection == CameraProjection::Perspective);
    CHECK(near(camera.direction, vm::normalize(vm::vec3d{1, 0, 1})));
    CHECK(vm::dot(camera.direction, camera.up) == Catch::Approx(0.0).margin(1e-9));
    CHECK(camera.up.z() > 0.0);
    CHECK(camera.fov == 60);

    SECTION("straight down")
    {
      const auto down = lookAtCamera(vm::vec3d{0, 0, 100}, vm::vec3d{0, 0, 0});
      CHECK(near(down.direction, vm::vec3d{0, 0, -1}));
      CHECK(near(down.up, vm::vec3d{0, 1, 0}));
    }

    SECTION("straight up")
    {
      const auto up = lookAtCamera(vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 100});
      CHECK(near(up.direction, vm::vec3d{0, 0, 1}));
      CHECK(vm::dot(up.direction, up.up) == Catch::Approx(0.0).margin(1e-9));
    }
  }

  SECTION("orthographic views")
  {
    CHECK(parseOrthoView("top") == OrthoView::Top);
    CHECK(parseOrthoView("xy") == OrthoView::Top);
    CHECK(parseOrthoView("front") == OrthoView::Front);
    CHECK(parseOrthoView("xz") == OrthoView::Front);
    CHECK(parseOrthoView("side") == OrthoView::Side);
    CHECK(parseOrthoView("yz") == OrthoView::Side);
    CHECK(parseOrthoView("3d") == std::nullopt);

    const auto bounds = vm::bbox3d{{-1000, -2000, -500}, {1000, 2000, 500}};

    // like ui::MapView2D::initializeCamera
    const auto top = orthographicCamera(OrthoView::Top, vm::vec3d{10, 20, 30}, 2, bounds);
    CHECK(top.projection == CameraProjection::Orthographic);
    CHECK(top.direction == vm::vec3d{0, 0, -1});
    CHECK(top.up == vm::vec3d{0, 1, 0});
    CHECK(top.position == vm::vec3d{10, 20, 516});
    CHECK(top.zoom == 2);
    CHECK(top.farPlane > 1000);
    CHECK(orthoViewOf(top) == OrthoView::Top);

    const auto front =
      orthographicCamera(OrthoView::Front, vm::vec3d{10, 20, 30}, 1, bounds);
    CHECK(front.direction == vm::vec3d{0, 1, 0});
    CHECK(front.up == vm::vec3d{0, 0, 1});
    CHECK(front.position == vm::vec3d{10, -2016, 30});
    CHECK(front.farPlane > 4000);
    CHECK(orthoViewOf(front) == OrthoView::Front);

    const auto side =
      orthographicCamera(OrthoView::Side, vm::vec3d{10, 20, 30}, 1, bounds);
    CHECK(side.direction == vm::vec3d{-1, 0, 0});
    CHECK(side.up == vm::vec3d{0, 0, 1});
    CHECK(side.position == vm::vec3d{1016, 20, 30});
    CHECK(orthoViewOf(side) == OrthoView::Side);

    CHECK(
      orthoViewOf(lookAtCamera(vm::vec3d{0, 0, 10}, vm::vec3d{0, 0, 0})) == std::nullopt);
  }

  SECTION("frameBox")
  {
    const auto box = vm::bbox3d{{0, 0, 0}, {512, 256, 128}};

    SECTION("perspective")
    {
      for (const auto [yaw, pitch] :
           {std::pair{45.0, -30.0}, std::pair{-90.0, 0.0}, std::pair{0.0, -90.0}})
      {
        const auto direction = directionFromAngles(yaw, pitch);
        auto camera = perspectiveCamera(vm::vec3d{0, 0, 0}, direction);
        camera.up = upVector(direction, yaw);
        const auto framed = frameBox(camera, box, 1024, 768);
        CHECK(framed.direction == camera.direction);
        CHECK(inFrustum(framed, box, 1024, 768));
        // the camera looks at the center of the box
        const auto toCenter = vm::normalize(box.center() - framed.position);
        CHECK(near(toCenter, framed.direction));
        // a tall image needs a larger distance
        const auto tall = frameBox(camera, box, 300, 1000);
        CHECK(inFrustum(tall, box, 300, 1000));
        CHECK(
          vm::length(tall.position - box.center())
          > vm::length(framed.position - box.center()));
      }

      CHECK(
        framingDistance(box, 90, 1024, 768, 1.0)
        < framingDistance(box, 90, 1024, 768, 1.5));
    }

    SECTION("orthographic")
    {
      const auto top =
        orthographicCamera(OrthoView::Top, vm::vec3d{0, 0, 0}, 1, vm::bbox3d{4096.0});
      const auto framed = frameBox(top, box, 1024, 768, 1.0);
      CHECK(framed.direction == top.direction);
      CHECK(framed.position.x() == Catch::Approx(256));
      CHECK(framed.position.y() == Catch::Approx(128));
      CHECK(framed.position.z() == top.position.z());
      // 512 x 256 units into 1024 x 768 pixels: 2 pixels per unit
      CHECK(framed.zoom == Catch::Approx(2.0));
      CHECK(frameBox(top, box, 1024, 768, 2.0).zoom == Catch::Approx(1.0));

      // a camera inside the box is moved out of it
      auto inside = top;
      inside.position = vm::vec3d{0, 0, 64};
      const auto moved = frameBox(inside, box, 1024, 768);
      CHECK(moved.position.z() > 128);
      CHECK(moved.farPlane > moved.position.z());
    }
  }

  SECTION("orbitCamera")
  {
    const auto target = vm::vec3d{100, 200, 50};
    const auto camera = orbitCamera(target, 90, -45, 200, 70);
    CHECK(near(camera.direction, directionFromAngles(90, -45)));
    CHECK(near(camera.position + camera.direction * 200.0, target));
    CHECK(camera.position.y() < target.y());
    CHECK(camera.position.z() > target.z());
    CHECK(camera.fov == 70);

    const auto down = orbitCamera(target, 90, -90, 100);
    CHECK(near(down.position, vm::vec3d{100, 200, 150}));
    CHECK(near(down.up, vm::vec3d{0, 1, 0}));
  }

  SECTION("playerSize")
  {
    auto config = mdl::GameConfig{};
    const auto sizeOf = [&](const std::string& name) {
      config.name = name;
      return playerSize(config);
    };
    CHECK(sizeOf("Quake").family == "quake");
    CHECK(sizeOf("Quake").eyeHeight == 46);
    CHECK(sizeOf("Quake 2").family == "quake2");
    CHECK(sizeOf("Quake 2").eyeHeight == 46);
    CHECK(sizeOf("Half-Life").family == "halflife");
    CHECK(sizeOf("Half-Life").eyeHeight == 64);
    CHECK(sizeOf("Half-Life").height == 72);
    CHECK(sizeOf("Quake 3").family == "quake3");
    CHECK(sizeOf("Quake 3").eyeHeight == 50);
    CHECK(sizeOf("Some Mod").family == "generic");
    CHECK(sizeOf("Some Mod").eyeHeight == 48);
  }

  SECTION("findFloor")
  {
    auto fixture = McpToolFixture{};
    auto& document = fixture.load(
      getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
      {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
    auto& map = document.map();

    // the floor of the west room is at z = 0
    CHECK(findFloor(map, vm::vec3d{256, 256, 100}) == 0.0);
    // inside the floor brush: the ray passes through it
    CHECK(findFloor(map, vm::vec3d{256, 256, -8}) == std::nullopt);
    // outside the rooms
    CHECK(findFloor(map, vm::vec3d{700, 150, 64}) == std::nullopt);
    // too deep
    CHECK(findFloor(map, vm::vec3d{256, 256, 100}, 50) == std::nullopt);
  }

  SECTION("toJson")
  {
    const auto perspective = orbitCamera(vm::vec3d{0, 0, 0}, 90, -30, 100, 75);
    const auto json = toJson(perspective);
    CHECK(json["projection"] == "perspective");
    CHECK(json["yaw"] == 90);
    CHECK(json["pitch"] == -30);
    CHECK(json["fov"] == 75);
    CHECK(!json.contains("zoom"));
    CHECK(json["near"] == 1);
    CHECK(json["position"].size() == 3);

    const auto top =
      orthographicCamera(OrthoView::Top, vm::vec3d{0, 0, 0}, 0.5, vm::bbox3d{1024.0});
    const auto orthoJson = toJson(top);
    CHECK(orthoJson["projection"] == "orthographic");
    CHECK(orthoJson["view"] == "top");
    CHECK(orthoJson["zoom"] == 0.5);
    CHECK(!orthoJson.contains("fov"));
    CHECK(orthoJson["direction"] == Json::array({0, 0, -1}));
  }
}

} // namespace tb::mcp
