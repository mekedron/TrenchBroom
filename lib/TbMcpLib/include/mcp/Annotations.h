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
#include "mcp/Errors.h"
#include "mcp/Image.h"
#include "mcp/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;
} // namespace tb::mdl

namespace tb::mcp
{
class IdRegistry;
class ImageProjection;

// Annotations (E12.2) are drawn by the MCP core onto a rendered image, so they work with
// every snapshot renderer. Pixel coordinates have their origin in the top left corner.

// Drawing primitives. Colors are blended with their alpha; pixels outside the image are
// ignored.

/** Blends the color into the pixel (x, y). */
void blendPixel(RgbaImage& image, long x, long y, const Rgba8& color);

/** Draws a line of the given thickness in pixels, clipped to the image. */
void drawLine(
  RgbaImage& image,
  const vm::vec2d& start,
  const vm::vec2d& end,
  const Rgba8& color,
  int thickness = 1);

/** Draws the outline of a rectangle (x, y is its top left pixel). */
void drawRect(
  RgbaImage& image, long x, long y, long width, long height, const Rgba8& color);

/** Fills a rectangle (x, y is its top left pixel). */
void fillRect(
  RgbaImage& image, long x, long y, long width, long height, const Rgba8& color);

/** Fills a polygon (even-odd rule; pixel centers inside the polygon are filled). */
void fillPolygon(
  RgbaImage& image, const std::vector<vm::vec2d>& points, const Rgba8& color);

/** Draws the outline of a circle. */
void drawCircle(
  RgbaImage& image,
  const vm::vec2d& center,
  double radius,
  const Rgba8& color,
  int thickness = 1);

// A built-in 5x7 bitmap font: letters, digits, space and : _ - . / # ( ) , + = ? *.
// Other characters are drawn as '?'.

constexpr auto GlyphWidth = 5;
constexpr auto GlyphHeight = 7;

/** Whether the font has a glyph for the character. */
bool hasGlyph(char c);

/** The width of the text in pixels: 6 * scale per character, minus the last gap. */
long textWidth(std::string_view text, int scale = 1);
long textHeight(int scale = 1);

/** Draws the text with its top left corner at (x, y). */
void drawText(
  RgbaImage& image,
  long x,
  long y,
  std::string_view text,
  const Rgba8& color,
  int scale = 1);

/** A rectangle in pixels. */
struct PixelBox
{
  long x = 0;
  long y = 0;
  long width = 0;
  long height = 0;

  bool intersects(const PixelBox& other) const;
  bool inside(size_t imageWidth, size_t imageHeight) const;
};

/** The box of a label with the given lines of text centered on (x, y). */
PixelBox labelBox(double x, double y, const std::vector<std::string>& lines, int scale);

/** Draws lines of text on a background box (see labelBox). */
void drawLabel(
  RgbaImage& image,
  const PixelBox& box,
  const std::vector<std::string>& lines,
  const Rgba8& textColor,
  const Rgba8& background,
  int scale);

/**
 * Draws a world line segment with the given projection, clipped to the near plane.
 * Returns whether any part of it lies in front of the camera.
 */
bool drawLine3d(
  RgbaImage& image,
  const ImageProjection& projection,
  const vm::vec3d& start,
  const vm::vec3d& end,
  const Rgba8& color,
  int thickness = 1);

/** The text scale for an image: 1 below 400 pixels (the shorter side), 2 from 400, 3 from
 * 1200. */
int annotationScale(size_t width, size_t height);

// Annotations

/** A label for an object, placed at the projection of `anchor`. */
struct LabelAnnotation
{
  std::string objectId;
  std::vector<std::string> lines;
  vm::vec3d anchor;
};

/** Coordinate lines on the floor and the walls of a box. */
struct GridAnnotation
{
  double step = 64.0;
  bool floor = true;
  bool walls = true;
  /** The floor is the bottom of the box, the walls are its sides. */
  vm::bbox3d box;
  /** Every labelEvery-th line gets a coordinate label (0: choose automatically). */
  size_t labelEvery = 0;
  /**
   * Whether a world point is visible, i.e. not behind other geometry; line pieces that
   * are not visible are not drawn. Null draws everything.
   */
  std::function<bool(const vm::vec3d&)> visible;
};

/** A player-sized box standing with its feet at `feet`, as a scale reference. */
struct PlayerAnnotation
{
  vm::vec3d feet;
  double width = 32.0;
  double height = 56.0;
  double eyeHeight = 46.0;
};

struct AnnotationSpec
{
  std::vector<LabelAnnotation> labels;
  std::optional<GridAnnotation> grid;
  bool compass = false;
  std::optional<PlayerAnnotation> player;
  /** The text scale; 0 chooses annotationScale. */
  int scale = 0;
};

/** What drawAnnotations drew. */
struct AnnotationReport
{
  /** The ids of the labelled objects. */
  std::vector<std::string> labelled;
  /** Labels that were not drawn because they would overlap others or leave the image. */
  size_t labelsSkipped = 0;
  size_t gridLines = 0;
  size_t gridLabels = 0;
  bool compass = false;
  /** Whether the player box is in front of the camera. */
  bool player = false;
};

/** Draws the annotations onto the image. Precondition: the image has the projection's
 * size */
AnnotationReport drawAnnotations(
  RgbaImage& image, const ImageProjection& projection, const AnnotationSpec& spec);

/**
 * The annotations requested by the `annotations` argument of view_snapshot for an image
 * of the given objects:
 *
 * - `labels`: true (the drawn groups, entities and brushes, most important first) or
 *   {ids, max}; objects whose label anchor is hidden behind other geometry or outside the
 *   image are skipped.
 * - `grid`: true or {step, planes: floor | walls | both, box, labelEvery}. Without a box,
 *   the grid covers the space around the point in the image center: rays from there find
 *   the floor, the ceiling and the walls.
 * - `compass`: true.
 * - `player`: {point, onFloor}: the game's player box standing at the point (on the floor
 *   below it unless onFloor is false).
 *
 * `drawn` accepts the objects the image shows (for occlusion); `sceneNodes` lists them.
 * The player size comes from the map's game (playerSize). Labels test at most 3000
 * anchor points for occlusion. Warnings (e.g. a coarser grid) are added to `warnings`.
 */
Result<AnnotationSpec, ToolError> buildAnnotations(
  mdl::Map& map,
  const IdRegistry& ids,
  const ImageProjection& projection,
  const std::vector<mdl::Node*>& sceneNodes,
  const std::function<bool(const mdl::Node&)>& drawn,
  const Json& annotations,
  std::vector<Warning>& warnings);

/**
 * The summary of the drawn annotations: `{labels, labelled, labelsSkipped, grid: {step,
 * box, floor, walls, lines, labels} | null, compass, player: {feet, width, height,
 * eyeHeight, visible} | null}`.
 */
Json annotationsJson(const AnnotationSpec& spec, const AnnotationReport& report);

} // namespace tb::mcp
