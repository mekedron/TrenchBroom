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

#include "gl/Camera.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "mcp/AgentCamera.h"
#include "mcp/CameraProjection.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/distance.h"
#include "vm/vec.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

AgentCamera perspective()
{
  return lookAtCamera(vm::vec3d{-200, -300, 150}, vm::vec3d{256, 256, 64}, 75.0);
}

AgentCamera orthographic()
{
  auto camera = orthographicCamera(
    OrthoView::Top,
    vm::vec3d{128, 64, 0},
    0.5,
    vm::bbox3d{vm::vec3d{-4096, -4096, -4096}, vm::vec3d{4096, 4096, 4096}});
  return camera;
}

/** The distance of the point from the ray's line. */
double distanceToRay(const vm::ray3d& ray, const vm::vec3d& point)
{
  const auto v = point - ray.origin;
  return vm::length(v - vm::dot(v, ray.direction) * ray.direction);
}

} // namespace

TEST_CASE("CameraProjection")
{
  SECTION("makeGlCamera")
  {
    const auto camera = makeGlCamera(perspective(), 640, 480) | kdl::value();
    CHECK(dynamic_cast<const gl::PerspectiveCamera*>(camera.get()) != nullptr);
    CHECK(camera->viewport() == gl::Camera::Viewport{0, 0, 640, 480});

    const auto ortho = makeGlCamera(orthographic(), 640, 480) | kdl::value();
    CHECK(dynamic_cast<const gl::OrthographicCamera*>(ortho.get()) != nullptr);
    CHECK(ortho->zoom() == 0.5f);

    // orthographic cameras may start at their near plane, perspective ones may not
    auto plan = orthographic();
    plan.nearPlane = 0.0;
    CHECK(makeGlCamera(plan, 64, 64).is_success());
    auto invalid = perspective();
    invalid.nearPlane = 0.0;
    CHECK(makeGlCamera(invalid, 64, 64).is_error());
    CHECK(makeGlCamera(perspective(), 0, 64).is_error());
    CHECK(makeGlCamera(perspective(), MaxCameraImageSize + 1, 64).is_error());
    CHECK(ImageProjection::create(invalid, 64, 64).is_error());
  }

  SECTION("project and pickRay")
  {
    for (const auto& camera : {perspective(), orthographic()})
    {
      CAPTURE(camera.projection);
      const auto projection = ImageProjection::create(camera, 640, 480) | kdl::value();
      CHECK(projection.width() == 640);
      CHECK(projection.height() == 480);

      for (const auto& point :
           {vm::vec3d{256, 256, 64},
            vm::vec3d{0, 0, 0},
            vm::vec3d{300, 100, 0},
            vm::vec3d{200, 400, 128}})
      {
        CAPTURE(point);
        const auto projected = projection.project(point);
        REQUIRE(projected.inFront);
        CHECK(projected.depth == Catch::Approx(projection.depth(point)));

        // the pick ray through the projected position passes through the point
        const auto ray = projection.pickRay(projected.x, projected.y);
        CHECK(vm::length(ray.direction) == Catch::Approx(1.0));
        CHECK(distanceToRay(ray, point) < 0.05);
        CHECK(vm::dot(point - ray.origin, ray.direction) > 0.0);
      }
    }
  }

  SECTION("image center and corners")
  {
    const auto camera = perspective();
    const auto projection = ImageProjection::create(camera, 800, 600) | kdl::value();

    // the image center looks along the view direction
    const auto center = projection.pickRay(400, 300);
    CHECK(center.origin == vm::approx{camera.position});
    CHECK(center.direction == vm::approx{vm::normalize(camera.direction), 1e-6});

    const auto target = camera.position + 100.0 * camera.direction;
    const auto projected = projection.project(target);
    CHECK(projected.x == Catch::Approx(400).margin(1e-3));
    CHECK(projected.y == Catch::Approx(300).margin(1e-3));

    // top left: up and left of the view direction; y grows downwards
    const auto right = vm::normalize(vm::cross(camera.direction, camera.up));
    const auto topLeft = projection.pickRay(0, 0);
    CHECK(vm::dot(topLeft.direction, camera.up) > 0.0);
    CHECK(vm::dot(topLeft.direction, right) < 0.0);
    const auto bottomRight = projection.pickRay(800, 600);
    CHECK(vm::dot(bottomRight.direction, camera.up) < 0.0);
    CHECK(vm::dot(bottomRight.direction, right) > 0.0);

    // pixel rays go through pixel centers
    const auto pixel = projection.pixelRay(0, 0);
    const auto point = pixel.origin + 100.0 * pixel.direction;
    CHECK(projection.project(point).x == Catch::Approx(0.5).margin(1e-3));
    CHECK(projection.project(point).y == Catch::Approx(0.5).margin(1e-3));

    // the editor's projection: the vertical half extent is tan(fov / 2) * 0.75
    const auto top = projection.pickRay(400, 0);
    const auto angle = std::atan2(
      vm::dot(top.direction, vm::normalize(camera.up)),
      vm::dot(top.direction, camera.direction));
    CHECK(std::tan(angle) == Catch::Approx(std::tan(vm::to_radians(75.0) / 2.0) * 0.75));
  }

  SECTION("orthographic")
  {
    const auto camera = orthographic();
    const auto projection = ImageProjection::create(camera, 640, 480) | kdl::value();

    // 0.5 pixels per unit, centered on (128, 64), +y is up in the image
    const auto projected = projection.project(vm::vec3d{128, 64, 0});
    CHECK(projected.x == Catch::Approx(320));
    CHECK(projected.y == Catch::Approx(240));
    const auto corner = projection.project(vm::vec3d{128 - 640, 64 + 480, 0});
    CHECK(corner.x == Catch::Approx(0).margin(1e-6));
    CHECK(corner.y == Catch::Approx(0).margin(1e-6));

    // rays are parallel and start on the camera plane
    const auto a = projection.pickRay(0, 0);
    const auto b = projection.pickRay(640, 480);
    CHECK(a.direction == vm::approx{vm::vec3d{0, 0, -1}});
    CHECK(b.direction == vm::approx{vm::vec3d{0, 0, -1}});
    CHECK(a.origin.z() == Catch::Approx(camera.position.z()));
    CHECK(a.origin.x() == Catch::Approx(128 - 640));
    CHECK(a.origin.y() == Catch::Approx(64 + 480));
  }

  SECTION("depth, clip range and segments")
  {
    const auto camera = perspectiveCamera(vm::vec3d{0, 0, 0}, vm::vec3d{1, 0, 0}, 90.0);
    const auto projection = ImageProjection::create(camera, 100, 100) | kdl::value();

    CHECK(projection.depth(vm::vec3d{10, 5, 0}) == Catch::Approx(10));
    CHECK(!projection.project(vm::vec3d{-10, 0, 0}).inFront);
    CHECK(!projection.project(vm::vec3d{0.5, 0, 0}).inFront);
    CHECK(projection.withinClipRange(10.0));
    CHECK(!projection.withinClipRange(0.5));
    CHECK(!projection.withinClipRange(camera.farPlane + 1.0));

    // entirely behind
    CHECK(
      projection.projectSegment(vm::vec3d{-10, 0, 0}, vm::vec3d{-20, 5, 0})
      == std::nullopt);
    // in front: unchanged
    const auto front =
      projection.projectSegment(vm::vec3d{10, 0, 0}, vm::vec3d{20, 0, 0});
    REQUIRE(front);
    CHECK(front->first == vm::approx{vm::vec2d{50, 50}});
    CHECK(front->second == vm::approx{vm::vec2d{50, 50}});
    // crossing the near plane: clipped, the far end is unchanged
    const auto crossing =
      projection.projectSegment(vm::vec3d{-10, 0, -10}, vm::vec3d{10, 0, -10});
    REQUIRE(crossing);
    CHECK(
      crossing->second == vm::approx{projection.project(vm::vec3d{10, 0, -10}).pixel()});
    CHECK(crossing->first.y() > 100.0);
  }
}

} // namespace tb::mcp
