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

#include "base/Result.h"
#include "mcp/AgentCamera.h"

#include "vm/mat.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace tb::gl
{
class Camera;
}

namespace tb::mcp
{

/** The largest image side that a camera can render. */
constexpr auto MaxCameraImageSize = size_t(16384);

/**
 * Returns an editor camera for the given agent camera that renders an image of the given
 * size: a perspective camera with the agent camera's field of view, or an orthographic
 * camera whose zoom is the agent camera's pixels per map unit. Fails if the camera or the
 * size is invalid. The snapshot renderer draws with this camera, and ImageProjection
 * uses its matrices, so that picking and annotations match the rendered image exactly.
 */
Result<std::unique_ptr<gl::Camera>> makeGlCamera(
  const AgentCamera& camera, size_t width, size_t height);

/** A world point projected into an image. */
struct ProjectedPoint
{
  /** Pixel coordinates; the top left corner of the image is (0, 0), y grows downwards. */
  double x = 0.0;
  double y = 0.0;
  /** The distance of the point along the view direction (from the camera plane). */
  double depth = 0.0;
  /** Whether the point is in front of the near plane (only then are x and y valid). */
  bool inFront = false;

  vm::vec2d pixel() const { return vm::vec2d{x, y}; }
};

/**
 * The mapping between an agent camera's image and the world, computed with the matrices
 * of the renderer's camera (makeGlCamera) in double precision.
 *
 * Pixel coordinates have their origin in the top left corner of the image; the center of
 * pixel (i, j) is (i + 0.5, j + 0.5).
 */
class ImageProjection
{
private:
  AgentCamera m_camera;
  size_t m_width = 0;
  size_t m_height = 0;
  vm::mat4x4d m_matrix;
  vm::mat4x4d m_inverse;

  ImageProjection(
    AgentCamera camera,
    size_t width,
    size_t height,
    const vm::mat4x4d& matrix,
    const vm::mat4x4d& inverse);

public:
  /** Fails like makeGlCamera if the camera or the size is invalid. */
  static Result<ImageProjection> create(
    const AgentCamera& camera, size_t width, size_t height);

  const AgentCamera& camera() const;
  size_t width() const;
  size_t height() const;

  /**
   * The ray through the given image position (pixel coordinates, see above). Perspective
   * rays start at the camera position; orthographic rays start on the camera plane (the
   * plane through the position, orthogonal to the view direction). The direction is
   * normalized.
   */
  vm::ray3d pickRay(double x, double y) const;

  /** The ray through the center of the pixel (x, y). */
  vm::ray3d pixelRay(size_t x, size_t y) const;

  /** Projects a world point into the image. */
  ProjectedPoint project(const vm::vec3d& point) const;

  /** The distance of the point along the view direction from the camera plane. */
  double depth(const vm::vec3d& point) const;

  /** Whether a point at the given depth lies between the near and the far plane. */
  bool withinClipRange(double depth) const;

  /**
   * Projects a world line segment into the image after clipping it to the near plane.
   * Returns nullopt if the segment is entirely behind the near plane. The result may lie
   * partly or entirely outside the image.
   */
  std::optional<std::pair<vm::vec2d, vm::vec2d>> projectSegment(
    const vm::vec3d& start, const vm::vec3d& end) const;
};

} // namespace tb::mcp
