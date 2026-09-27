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

#include "mcp/AgentCamera.h"

#include "mcp/JsonVm.h"
#include "mcp/tools/CompileUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"

#include "kd/string_format.h"

#include "vm/constants.h"
#include "vm/ray.h"
#include "vm/vec.h"
#include "vm/vec_ext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace tb::mcp
{
namespace
{

constexpr auto Epsilon = 1e-9;

/** Distance of orthographic cameras from the bounds they are placed outside of. */
constexpr auto OrthoOffset = 16.0;

double toRadians(const double degrees)
{
  return degrees * vm::Cd::pi() / 180.0;
}

double toDegrees(const double radians)
{
  return radians * 180.0 / vm::Cd::pi();
}

std::array<vm::vec3d, 8> corners(const vm::bbox3d& box)
{
  auto result = std::array<vm::vec3d, 8>{};
  for (size_t i = 0; i < 8; ++i)
  {
    result[i] = vm::vec3d{
      (i & 1) ? box.max.x() : box.min.x(),
      (i & 2) ? box.max.y() : box.min.y(),
      (i & 4) ? box.max.z() : box.min.z()};
  }
  return result;
}

/** Cleans up values such as 6.123e-17 that should be 0. */
double clean(const double value)
{
  return std::abs(value) < 1e-12 ? 0.0 : value;
}

vm::vec3d clean(const vm::vec3d& v)
{
  return vm::vec3d{clean(v.x()), clean(v.y()), clean(v.z())};
}

} // namespace

vm::vec3d directionFromAngles(const double yaw, const double pitch)
{
  const auto y = toRadians(yaw);
  const auto p = toRadians(std::clamp(pitch, -90.0, 90.0));
  return clean(
    vm::vec3d{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)});
}

CameraAngles anglesFromCamera(const vm::vec3d& direction, const vm::vec3d& up)
{
  const auto dir = vm::normalize(direction);
  const auto pitch = toDegrees(std::asin(std::clamp(dir.z(), -1.0, 1.0)));
  const auto horizontal = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
  if (horizontal > Epsilon)
  {
    return {clean(toDegrees(std::atan2(dir.y(), dir.x()))), clean(pitch)};
  }

  // looking down, the up vector points forward; looking up, it points backward
  const auto forward = dir.z() < 0.0 ? up : -up;
  return {clean(toDegrees(std::atan2(forward.y(), forward.x()))), clean(pitch)};
}

vm::vec3d upVector(const vm::vec3d& direction, const double yaw)
{
  const auto dir = vm::normalize(direction);
  auto right = vm::cross(dir, vm::vec3d{0, 0, 1});
  if (vm::length(right) < 1e-6)
  {
    const auto forward = directionFromAngles(yaw, 0.0);
    right = vm::cross(forward, vm::vec3d{0, 0, 1});
  }
  return clean(vm::normalize(vm::cross(vm::normalize(right), dir)));
}

AgentCamera perspectiveCamera(
  const vm::vec3d& position, const vm::vec3d& direction, const double fov)
{
  auto camera = AgentCamera{};
  camera.projection = CameraProjection::Perspective;
  camera.position = position;
  camera.direction = clean(vm::normalize(direction));
  camera.up = upVector(camera.direction);
  camera.fov = fov;
  return camera;
}

AgentCamera lookAtCamera(
  const vm::vec3d& position, const vm::vec3d& target, const double fov)
{
  return perspectiveCamera(position, target - position, fov);
}

std::optional<OrthoView> parseOrthoView(const std::string_view name)
{
  if (name == "top" || name == "xy")
  {
    return OrthoView::Top;
  }
  if (name == "front" || name == "xz")
  {
    return OrthoView::Front;
  }
  if (name == "side" || name == "yz")
  {
    return OrthoView::Side;
  }
  return std::nullopt;
}

std::string_view toString(const OrthoView view)
{
  switch (view)
  {
  case OrthoView::Top:
    return "top";
  case OrthoView::Front:
    return "front";
  case OrthoView::Side:
    return "side";
  }
  return "top";
}

vm::vec3d orthoDirection(const OrthoView view)
{
  switch (view)
  {
  case OrthoView::Top:
    return vm::vec3d{0, 0, -1};
  case OrthoView::Front:
    return vm::vec3d{0, 1, 0};
  case OrthoView::Side:
    return vm::vec3d{-1, 0, 0};
  }
  return vm::vec3d{0, 0, -1};
}

vm::vec3d orthoUp(const OrthoView view)
{
  return view == OrthoView::Top ? vm::vec3d{0, 1, 0} : vm::vec3d{0, 0, 1};
}

std::optional<OrthoView> orthoViewOf(const AgentCamera& camera)
{
  if (camera.projection != CameraProjection::Orthographic)
  {
    return std::nullopt;
  }
  for (const auto view : {OrthoView::Top, OrthoView::Front, OrthoView::Side})
  {
    if (
      vm::dot(camera.direction, orthoDirection(view)) > 1.0 - 1e-9
      && vm::dot(camera.up, orthoUp(view)) > 1.0 - 1e-9)
    {
      return view;
    }
  }
  return std::nullopt;
}

AgentCamera orthographicCamera(
  const OrthoView view,
  const vm::vec3d& center,
  const double zoom,
  const vm::bbox3d& bounds)
{
  auto camera = AgentCamera{};
  camera.projection = CameraProjection::Orthographic;
  camera.direction = orthoDirection(view);
  camera.up = orthoUp(view);
  camera.zoom = zoom;
  camera.position = center;
  camera.nearPlane = 1.0;

  const auto size = bounds.size();
  switch (view)
  {
  case OrthoView::Top:
    camera.position[2] = bounds.max.z() + OrthoOffset;
    camera.farPlane = size.z() + 2.0 * OrthoOffset;
    break;
  case OrthoView::Front:
    camera.position[1] = bounds.min.y() - OrthoOffset;
    camera.farPlane = size.y() + 2.0 * OrthoOffset;
    break;
  case OrthoView::Side:
    camera.position[0] = bounds.max.x() + OrthoOffset;
    camera.farPlane = size.x() + 2.0 * OrthoOffset;
    break;
  }
  return camera;
}

double framingDistance(
  const vm::bbox3d& box,
  const double fov,
  const size_t width,
  const size_t height,
  const double margin)
{
  const auto radius = std::max(vm::length(box.size()) / 2.0, 1.0) * margin;
  const auto aspect =
    double(std::max(width, size_t(1))) / double(std::max(height, size_t(1)));
  const auto tanVertical = std::tan(toRadians(fov) / 2.0) * 0.75;
  const auto tanHorizontal = tanVertical * aspect;
  const auto halfAngle = std::atan(std::min(tanVertical, tanHorizontal));
  return radius / std::sin(halfAngle);
}

AgentCamera frameBox(
  AgentCamera camera,
  const vm::bbox3d& box,
  const size_t width,
  const size_t height,
  const double margin)
{
  const auto center = box.center();
  const auto boxCorners = corners(box);

  if (camera.projection == CameraProjection::Perspective)
  {
    const auto distance = framingDistance(box, camera.fov, width, height, margin);
    camera.position = clean(center - camera.direction * distance);
    const auto radius = vm::length(box.size()) / 2.0;
    camera.farPlane = std::max(camera.farPlane, distance + radius + 16.0);
    return camera;
  }

  const auto right = vm::normalize(vm::cross(camera.direction, camera.up));
  auto extentRight = 0.0;
  auto extentUp = 0.0;
  for (const auto& corner : boxCorners)
  {
    extentRight = std::max(extentRight, std::abs(vm::dot(corner - center, right)));
    extentUp = std::max(extentUp, std::abs(vm::dot(corner - center, camera.up)));
  }
  const auto zoomRight = double(width) / std::max(2.0 * extentRight * margin, 1.0);
  const auto zoomUp = double(height) / std::max(2.0 * extentUp * margin, 1.0);
  camera.zoom = std::min(zoomRight, zoomUp);

  // center on the box, keeping the distance along the view axis
  camera.position =
    center + camera.direction * vm::dot(camera.position - center, camera.direction);

  // make sure the box is in front of the camera and within the far plane
  auto nearest = std::numeric_limits<double>::max();
  auto farthest = std::numeric_limits<double>::lowest();
  for (const auto& corner : boxCorners)
  {
    const auto depth = vm::dot(corner - camera.position, camera.direction);
    nearest = std::min(nearest, depth);
    farthest = std::max(farthest, depth);
  }
  if (nearest < camera.nearPlane + OrthoOffset)
  {
    const auto shift = camera.nearPlane + OrthoOffset - nearest;
    camera.position = camera.position - camera.direction * shift;
    farthest += shift;
  }
  camera.position = clean(camera.position);
  camera.farPlane = std::max(camera.farPlane, farthest + OrthoOffset);
  return camera;
}

AgentCamera orbitCamera(
  const vm::vec3d& target,
  const double yaw,
  const double pitch,
  const double distance,
  const double fov)
{
  const auto direction = directionFromAngles(yaw, pitch);
  auto camera = perspectiveCamera(target - direction * distance, direction, fov);
  camera.up = upVector(direction, yaw);
  camera.position = clean(camera.position);
  camera.farPlane = std::max(camera.farPlane, distance * 2.0);
  return camera;
}

PlayerSize playerSize(const mdl::GameConfig& gameConfig)
{
  // eye height = the height of the player's origin above the floor + the view offset
  const auto quake = PlayerSize{"quake", 46.0, 56.0, 32.0};
  const auto quake2 = PlayerSize{"quake2", 46.0, 56.0, 32.0};
  const auto halfLife = PlayerSize{"halflife", 64.0, 72.0, 32.0};
  const auto quake3 = PlayerSize{"quake3", 50.0, 56.0, 30.0};

  const auto name = kdl::str_to_lower(gameConfig.name);
  if (name == "quake")
  {
    return quake;
  }
  if (name == "quake 2" || name == "quake2")
  {
    return quake2;
  }
  if (name == "half-life" || name == "halflife")
  {
    return halfLife;
  }
  if (name == "quake 3" || name == "quake3" || name == "quake 3 arena")
  {
    return quake3;
  }

  if (const auto family = compileFamily(gameConfig))
  {
    switch (*family)
    {
    case CompileFamily::Quake:
      return quake;
    case CompileFamily::Quake2:
      return quake2;
    case CompileFamily::HalfLife:
      return halfLife;
    case CompileFamily::Quake3:
      return quake3;
    }
  }
  return PlayerSize{"generic", 48.0, 56.0, 32.0};
}

std::optional<double> findFloor(
  mdl::Map& map, const vm::vec3d& point, const double maxDepth)
{
  const auto accept = [](const mdl::Node& node) {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      return classifyBrush(*brushNode) != BrushClass::Trigger;
    }
    return dynamic_cast<const mdl::PatchNode*>(&node) != nullptr;
  };
  const auto hits = castRay(map, vm::ray3d{point, vm::vec3d{0, 0, -1}}, accept, maxDepth);
  if (hits.empty())
  {
    return std::nullopt;
  }
  return hits.front().point.z();
}

Json toJson(const AgentCamera& camera)
{
  const auto angles = anglesFromCamera(camera.direction, camera.up);
  auto result = Json{
    {"projection",
     camera.projection == CameraProjection::Perspective ? "perspective" : "orthographic"},
    {"position", toJson(camera.position)},
    {"direction", toJson(camera.direction)},
    {"up", toJson(camera.up)},
    {"yaw", roundForOutput(angles.yaw)},
    {"pitch", roundForOutput(angles.pitch)},
  };
  if (camera.projection == CameraProjection::Perspective)
  {
    result["fov"] = roundForOutput(camera.fov);
  }
  else
  {
    const auto view = orthoViewOf(camera);
    result["view"] = view ? Json(std::string{toString(*view)}) : Json(nullptr);
    result["zoom"] = roundForOutput(camera.zoom);
  }
  result["near"] = roundForOutput(camera.nearPlane);
  result["far"] = roundForOutput(camera.farPlane);
  return result;
}

} // namespace tb::mcp
