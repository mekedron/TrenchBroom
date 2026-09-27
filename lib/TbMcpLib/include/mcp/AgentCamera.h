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

#pragma once

#include "mcp/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tb::mdl
{
struct GameConfig;
class Map;
} // namespace tb::mdl

namespace tb::mcp
{

enum class CameraProjection
{
  Perspective,
  Orthographic,
};

/**
 * A camera owned by an agent. It is independent of the editor's cameras and is never
 * shown to the user. Directions are unit vectors; `up` is orthogonal to `direction`.
 */
struct AgentCamera
{
  CameraProjection projection = CameraProjection::Perspective;
  vm::vec3d position = vm::vec3d{0, 0, 0};
  vm::vec3d direction = vm::vec3d{1, 0, 0};
  vm::vec3d up = vm::vec3d{0, 0, 1};
  /** Perspective only: the field of view in degrees, as the editor's camera uses it. */
  double fov = 90.0;
  /** Orthographic only: image pixels per map unit. */
  double zoom = 1.0;
  double nearPlane = 1.0;
  double farPlane = 32768.0;

  bool operator==(const AgentCamera&) const = default;
};

// Camera math of the agent cameras (E10.4 / E10.5). Angles are in degrees: yaw turns
// counterclockwise from +X around +Z, pitch is positive when looking up.

/** The field of view of new perspective cameras, like the editor's default. */
constexpr auto DefaultFov = 90.0;

/** The image size that framing assumes when no image size is known. */
constexpr auto DefaultImageWidth = size_t(1024);
constexpr auto DefaultImageHeight = size_t(768);

struct CameraAngles
{
  double yaw = 0.0;
  double pitch = 0.0;

  bool operator==(const CameraAngles&) const = default;
};

/** The unit direction for the given yaw and pitch. */
vm::vec3d directionFromAngles(double yaw, double pitch);

/**
 * The yaw and pitch of the given direction. For vertical directions, the yaw is taken
 * from the up vector (the direction the top of the image points to when looking down).
 */
CameraAngles anglesFromCamera(const vm::vec3d& direction, const vm::vec3d& up);

/**
 * The up vector of a perspective camera looking in the given direction: orthogonal to
 * the direction and in the vertical plane that contains it. For vertical directions, the
 * horizontal direction `yaw` becomes the top of the image when looking down (and the
 * bottom when looking up).
 */
vm::vec3d upVector(const vm::vec3d& direction, double yaw = 90.0);

/** A perspective camera at the given position looking in the given direction. */
AgentCamera perspectiveCamera(
  const vm::vec3d& position, const vm::vec3d& direction, double fov = DefaultFov);

/**
 * A perspective camera at the given position looking at the target. When looking
 * straight up or down, north (+Y) is the top of the image. Precondition: position !=
 * target
 */
AgentCamera lookAtCamera(
  const vm::vec3d& position, const vm::vec3d& target, double fov = DefaultFov);

/** The axis-aligned orthographic views of the editor's 2D views. */
enum class OrthoView
{
  /** The XY view: looking down -Z, +Y up. */
  Top,
  /** The XZ view: looking along +Y, +Z up. */
  Front,
  /** The YZ view: looking along -X, +Z up. */
  Side,
};

/** "top" / "xy", "front" / "xz", "side" / "yz". */
std::optional<OrthoView> parseOrthoView(std::string_view name);
/** "top", "front" or "side". */
std::string_view toString(OrthoView view);
/** The view direction and up vector of the view, like ui::MapView2D::initializeCamera. */
vm::vec3d orthoDirection(OrthoView view);
vm::vec3d orthoUp(OrthoView view);
/** The ortho view whose direction and up vector the camera has, if any. */
std::optional<OrthoView> orthoViewOf(const AgentCamera& camera);

/**
 * An orthographic camera for the given view, centered on `center` (the coordinate along
 * the view axis is ignored), with the given zoom (image pixels per map unit). Like the
 * editor, the camera is placed outside the given bounds (usually the world bounds) so
 * that everything inside them is in front of it and within the far plane.
 */
AgentCamera orthographicCamera(
  OrthoView view, const vm::vec3d& center, double zoom, const vm::bbox3d& bounds);

/**
 * Moves the camera so that the box fits into an image of the given size, keeping its
 * direction and up vector. Perspective cameras are moved back along their direction
 * until the bounding sphere of the box fits into the field of view (the editor's
 * projection: the vertical half extent is tan(fov / 2) * 0.75); orthographic cameras are
 * centered on the box (keeping their position along the view axis) and zoomed so that
 * the box fits. `margin` scales the fitted extent (1 = tight). The far plane is extended
 * if needed.
 */
AgentCamera frameBox(
  AgentCamera camera,
  const vm::bbox3d& box,
  size_t width,
  size_t height,
  double margin = 1.1);

/**
 * The distance at which a perspective camera with the given field of view sees the
 * whole bounding sphere of the box in an image of the given size.
 */
double framingDistance(
  const vm::bbox3d& box, double fov, size_t width, size_t height, double margin = 1.1);

/**
 * A perspective camera that looks at the target in the direction given by yaw and pitch
 * from the given distance (orbiting the target).
 */
AgentCamera orbitCamera(
  const vm::vec3d& target,
  double yaw,
  double pitch,
  double distance,
  double fov = DefaultFov);

/** The player's size in a game, for placing cameras at eye height. */
struct PlayerSize
{
  /** The game family, e.g. "quake". */
  std::string family;
  /** The height of the eyes above the floor. */
  double eyeHeight = 0.0;
  /** The height and width of the player's bounding box. */
  double height = 0.0;
  double width = 0.0;
};

/**
 * The player size of the game (Quake, Quake 2, Half-Life, Quake 3), or a generic size
 * for other games. The game is recognized by its name or its compilation tools.
 */
PlayerSize playerSize(const mdl::GameConfig& gameConfig);

/**
 * The height (z) of the floor below the given point: the first brush or patch surface
 * that a ray cast straight down from the point hits within maxDepth. Trigger brushes and
 * point entities are ignored; hidden objects count. Returns nullopt if there is no
 * floor.
 */
std::optional<double> findFloor(
  mdl::Map& map, const vm::vec3d& point, double maxDepth = 4096.0);

/**
 * `{projection, position, direction, up, yaw, pitch, fov | zoom, view, near, far}`;
 * `view` is "top", "front", "side" or null for orthographic cameras.
 */
Json toJson(const AgentCamera& camera);

} // namespace tb::mcp
