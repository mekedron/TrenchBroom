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

#include "mcp/CameraProjection.h"

#include "base/Macros.h"
#include "gl/Camera.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"

#include "kd/result.h"

#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <cmath>

namespace tb::mcp
{
namespace
{

/** Points closer to the near plane than this are treated as lying on it when clipping. */
constexpr auto ClipEpsilon = 1e-6;

} // namespace

Result<std::unique_ptr<gl::Camera>> makeGlCamera(
  const AgentCamera& camera, const size_t width, const size_t height)
{
  if (
    width == 0 || height == 0 || width > MaxCameraImageSize
    || height > MaxCameraImageSize)
  {
    return Error{fmt::format("Invalid image size {}x{}", width, height)};
  }
  // orthographic cameras may start at the near plane (the plan view does)
  const auto minNear = camera.projection == CameraProjection::Orthographic ? 0.0 : 1e-9;
  if (
    !std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane)
    || camera.nearPlane < minNear || camera.farPlane <= camera.nearPlane)
  {
    return Error{fmt::format(
      "Invalid clipping planes: near {}, far {}", camera.nearPlane, camera.farPlane)};
  }
  if (
    !vm::is_finite(camera.position) || !vm::is_finite(camera.direction)
    || !vm::is_finite(camera.up))
  {
    return Error{"Invalid camera position or orientation"};
  }

  const auto direction = vm::normalize(vm::vec3f{camera.direction});
  const auto up = vm::normalize(vm::vec3f{camera.up});
  if (
    !vm::is_unit(direction, vm::Cf::almost_zero())
    || !vm::is_unit(up, vm::Cf::almost_zero())
    || vm::is_zero(vm::cross(direction, up), vm::Cf::almost_zero()))
  {
    return Error{
      "Invalid camera orientation: direction and up must not be zero or parallel"};
  }

  const auto viewport = gl::Camera::Viewport{0, 0, int(width), int(height)};
  const auto position = vm::vec3f{camera.position};
  const auto nearPlane = float(camera.nearPlane);
  const auto farPlane = float(camera.farPlane);

  switch (camera.projection)
  {
  case CameraProjection::Perspective:
    if (!(camera.fov >= 1.0 && camera.fov <= 150.0))
    {
      return Error{fmt::format(
        "Invalid field of view {}, must be between 1 and 150 degrees", camera.fov)};
    }
    return std::make_unique<gl::PerspectiveCamera>(
      float(camera.fov), nearPlane, farPlane, viewport, position, direction, up);
  case CameraProjection::Orthographic: {
    if (!(camera.zoom >= 0.02 && camera.zoom <= 100.0))
    {
      return Error{fmt::format(
        "Invalid zoom {}, must be between 0.02 and 100 pixels per unit", camera.zoom)};
    }
    auto result = std::make_unique<gl::OrthographicCamera>(
      nearPlane, farPlane, viewport, position, direction, up);
    result->setZoom(float(camera.zoom));
    return result;
  }
    switchDefault();
  }
}

ImageProjection::ImageProjection(
  AgentCamera camera,
  const size_t width,
  const size_t height,
  const vm::mat4x4d& matrix,
  const vm::mat4x4d& inverse)
  : m_camera{std::move(camera)}
  , m_width{width}
  , m_height{height}
  , m_matrix{matrix}
  , m_inverse{inverse}
{
  m_camera.direction = vm::normalize(m_camera.direction);
  m_camera.up = vm::normalize(m_camera.up);
}

Result<ImageProjection> ImageProjection::create(
  const AgentCamera& camera, const size_t width, const size_t height)
{
  return makeGlCamera(camera, width, height)
         | kdl::and_then([&](const auto& glCamera) -> Result<ImageProjection> {
             // the matrices of the renderer's camera, computed in double precision
             const auto direction = vm::normalize(camera.direction);
             const auto right =
               vm::normalize(vm::cross(direction, vm::normalize(camera.up)));
             const auto up = vm::cross(right, direction);
             const auto view =
               vm::view_matrix(direction, up) * vm::translation_matrix(-camera.position);

             auto projectionMatrix = vm::mat4x4d{};
             if (camera.projection == CameraProjection::Perspective)
             {
               projectionMatrix = vm::perspective_matrix(
                 camera.fov, camera.nearPlane, camera.farPlane, int(width), int(height));
             }
             else
             {
               // like gl::OrthographicCamera, with its viewport rounded to whole units
               const auto& zoomed =
                 static_cast<const gl::OrthographicCamera&>(*glCamera).zoomedViewport();
               const auto w2 = double(zoomed.width) / 2.0;
               const auto h2 = double(zoomed.height) / 2.0;
               projectionMatrix =
                 vm::ortho_matrix(camera.nearPlane, camera.farPlane, -w2, h2, w2, -h2);
             }

             const auto matrix = projectionMatrix * view;
             const auto inverse = vm::invert(matrix);
             if (!inverse)
             {
               return Error{"The camera's projection cannot be inverted"};
             }
             return ImageProjection{camera, width, height, matrix, *inverse};
           });
}

const AgentCamera& ImageProjection::camera() const
{
  return m_camera;
}

size_t ImageProjection::width() const
{
  return m_width;
}

size_t ImageProjection::height() const
{
  return m_height;
}

vm::ray3d ImageProjection::pickRay(const double x, const double y) const
{
  const auto ndc = vm::vec3d{
    2.0 * x / double(m_width) - 1.0,
    1.0 - 2.0 * y / double(m_height),
    0.0,
  };
  const auto point = m_inverse * ndc;

  if (m_camera.projection == CameraProjection::Perspective)
  {
    return vm::ray3d{m_camera.position, vm::normalize(point - m_camera.position)};
  }
  // move the point back onto the camera plane
  const auto distance = vm::dot(point - m_camera.position, m_camera.direction);
  return vm::ray3d{point - distance * m_camera.direction, m_camera.direction};
}

vm::ray3d ImageProjection::pixelRay(const size_t x, const size_t y) const
{
  return pickRay(double(x) + 0.5, double(y) + 0.5);
}

double ImageProjection::depth(const vm::vec3d& point) const
{
  return vm::dot(point - m_camera.position, m_camera.direction);
}

bool ImageProjection::withinClipRange(const double depth) const
{
  return depth >= m_camera.nearPlane - ClipEpsilon
         && depth <= m_camera.farPlane + ClipEpsilon;
}

ProjectedPoint ImageProjection::project(const vm::vec3d& point) const
{
  auto result = ProjectedPoint{};
  result.depth = depth(point);
  result.inFront =
    result.depth >= m_camera.nearPlane - ClipEpsilon
    && (m_camera.projection == CameraProjection::Orthographic || result.depth > ClipEpsilon);
  if (result.inFront)
  {
    const auto ndc = m_matrix * point;
    result.x = (ndc.x() + 1.0) / 2.0 * double(m_width);
    result.y = (1.0 - ndc.y()) / 2.0 * double(m_height);
  }
  return result;
}

std::optional<std::pair<vm::vec2d, vm::vec2d>> ImageProjection::projectSegment(
  const vm::vec3d& start, const vm::vec3d& end) const
{
  // clip against a plane slightly in front of the near plane
  const auto clipDepth =
    std::max(m_camera.nearPlane, 0.0)
    + (m_camera.projection == CameraProjection::Perspective ? 1e-3 : 0.0);
  const auto startDepth = depth(start);
  const auto endDepth = depth(end);
  if (startDepth < clipDepth && endDepth < clipDepth)
  {
    return std::nullopt;
  }

  auto a = start;
  auto b = end;
  if (startDepth < clipDepth)
  {
    const auto t = (clipDepth - startDepth) / (endDepth - startDepth);
    a = start + t * (end - start);
  }
  else if (endDepth < clipDepth)
  {
    const auto t = (clipDepth - endDepth) / (startDepth - endDepth);
    b = end + t * (start - end);
  }

  const auto pa = m_matrix * a;
  const auto pb = m_matrix * b;
  const auto toPixel = [&](const vm::vec3d& ndc) {
    return vm::vec2d{
      (ndc.x() + 1.0) / 2.0 * double(m_width), (1.0 - ndc.y()) / 2.0 * double(m_height)};
  };
  return std::pair{toPixel(pa), toPixel(pb)};
}

} // namespace tb::mcp
