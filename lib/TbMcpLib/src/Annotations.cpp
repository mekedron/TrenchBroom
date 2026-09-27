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

#include "mcp/Annotations.h"

#include "mcp/AgentCamera.h"
#include "mcp/CameraProjection.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"

#include "vm/ray.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <unordered_set>

namespace tb::mcp
{
namespace
{

// Font

struct Glyph
{
  char c;
  /** 7 rows of 5 columns, '#' is set. */
  std::string_view rows;
};

// clang-format off
constexpr auto Glyphs = std::array{
  Glyph{' ', "....." "....." "....." "....." "....." "....." "....."},
  Glyph{'0', ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###."},
  Glyph{'1', "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###."},
  Glyph{'2', ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####"},
  Glyph{'3', "#####" "...#." "..#.." "...#." "....#" "#...#" ".###."},
  Glyph{'4', "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#."},
  Glyph{'5', "#####" "#...." "####." "....#" "....#" "#...#" ".###."},
  Glyph{'6', "..##." ".#..." "#...." "####." "#...#" "#...#" ".###."},
  Glyph{'7', "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#..."},
  Glyph{'8', ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###."},
  Glyph{'9', ".###." "#...#" "#...#" ".####" "....#" "...#." ".##.."},
  Glyph{'A', ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
  Glyph{'B', "####." "#...#" "#...#" "####." "#...#" "#...#" "####."},
  Glyph{'C', ".###." "#...#" "#...." "#...." "#...." "#...#" ".###."},
  Glyph{'D', "###.." "#..#." "#...#" "#...#" "#...#" "#..#." "###.."},
  Glyph{'E', "#####" "#...." "#...." "####." "#...." "#...." "#####"},
  Glyph{'F', "#####" "#...." "#...." "####." "#...." "#...." "#...."},
  Glyph{'G', ".###." "#...#" "#...." "#.###" "#...#" "#...#" ".####"},
  Glyph{'H', "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
  Glyph{'I', ".###." "..#.." "..#.." "..#.." "..#.." "..#.." ".###."},
  Glyph{'J', "..###" "...#." "...#." "...#." "...#." "#..#." ".##.."},
  Glyph{'K', "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#"},
  Glyph{'L', "#...." "#...." "#...." "#...." "#...." "#...." "#####"},
  Glyph{'M', "#...#" "##.##" "#.#.#" "#.#.#" "#...#" "#...#" "#...#"},
  Glyph{'N', "#...#" "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#"},
  Glyph{'O', ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
  Glyph{'P', "####." "#...#" "#...#" "####." "#...." "#...." "#...."},
  Glyph{'Q', ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#"},
  Glyph{'R', "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#"},
  Glyph{'S', ".####" "#...." "#...." ".###." "....#" "....#" "####."},
  Glyph{'T', "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#.."},
  Glyph{'U', "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
  Glyph{'V', "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#.."},
  Glyph{'W', "#...#" "#...#" "#...#" "#.#.#" "#.#.#" "#.#.#" ".#.#."},
  Glyph{'X', "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#"},
  Glyph{'Y', "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." "..#.."},
  Glyph{'Z', "#####" "....#" "...#." "..#.." ".#..." "#...." "#####"},
  Glyph{'a', "....." "....." ".###." "....#" ".####" "#...#" ".####"},
  Glyph{'b', "#...." "#...." "#.##." "##..#" "#...#" "#...#" "####."},
  Glyph{'c', "....." "....." ".###." "#...." "#...." "#...#" ".###."},
  Glyph{'d', "....#" "....#" ".##.#" "#..##" "#...#" "#...#" ".####"},
  Glyph{'e', "....." "....." ".###." "#...#" "#####" "#...." ".###."},
  Glyph{'f', "..##." ".#..#" ".#..." "###.." ".#..." ".#..." ".#..."},
  Glyph{'g', "....." ".####" "#...#" "#...#" ".####" "....#" ".###."},
  Glyph{'h', "#...." "#...." "#.##." "##..#" "#...#" "#...#" "#...#"},
  Glyph{'i', "..#.." "....." ".##.." "..#.." "..#.." "..#.." ".###."},
  Glyph{'j', "...#." "....." "..##." "...#." "...#." "#..#." ".##.."},
  Glyph{'k', "#...." "#...." "#..#." "#.#.." "##..." "#.#.." "#..#."},
  Glyph{'l', ".##.." "..#.." "..#.." "..#.." "..#.." "..#.." ".###."},
  Glyph{'m', "....." "....." "##.#." "#.#.#" "#.#.#" "#...#" "#...#"},
  Glyph{'n', "....." "....." "#.##." "##..#" "#...#" "#...#" "#...#"},
  Glyph{'o', "....." "....." ".###." "#...#" "#...#" "#...#" ".###."},
  Glyph{'p', "....." "####." "#...#" "#...#" "####." "#...." "#...."},
  Glyph{'q', "....." ".####" "#...#" "#...#" ".####" "....#" "....#"},
  Glyph{'r', "....." "....." "#.##." "##..#" "#...." "#...." "#...."},
  Glyph{'s', "....." "....." ".###." "#...." ".###." "....#" "####."},
  Glyph{'t', ".#..." ".#..." "###.." ".#..." ".#..." ".#..#" "..##."},
  Glyph{'u', "....." "....." "#...#" "#...#" "#...#" "#..##" ".##.#"},
  Glyph{'v', "....." "....." "#...#" "#...#" "#...#" ".#.#." "..#.."},
  Glyph{'w', "....." "....." "#...#" "#...#" "#.#.#" "#.#.#" ".#.#."},
  Glyph{'x', "....." "....." "#...#" ".#.#." "..#.." ".#.#." "#...#"},
  Glyph{'y', "....." "#...#" "#...#" "#...#" ".####" "....#" ".###."},
  Glyph{'z', "....." "....." "#####" "...#." "..#.." ".#..." "#####"},
  Glyph{':', "....." ".##.." ".##.." "....." ".##.." ".##.." "....."},
  Glyph{'_', "....." "....." "....." "....." "....." "....." "#####"},
  Glyph{'-', "....." "....." "....." "#####" "....." "....." "....."},
  Glyph{'.', "....." "....." "....." "....." "....." ".##.." ".##.."},
  Glyph{'/', "....." "....#" "...#." "..#.." ".#..." "#...." "....."},
  Glyph{'#', ".#.#." ".#.#." "#####" ".#.#." "#####" ".#.#." ".#.#."},
  Glyph{'(', "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#."},
  Glyph{')', ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#..."},
  Glyph{',', "....." "....." "....." "....." ".##.." "..#.." ".#..."},
  Glyph{'+', "....." "..#.." "..#.." "#####" "..#.." "..#.." "....."},
  Glyph{'=', "....." "....." "#####" "....." "#####" "....." "....."},
  Glyph{'?', ".###." "#...#" "....#" "...#." "..#.." "....." "..#.."},
  Glyph{'*', "....." "..#.." "#.#.#" ".###." "#.#.#" "..#.." "....."},
};
// clang-format on

const Glyph* findGlyph(const char c)
{
  const auto it =
    std::ranges::find_if(Glyphs, [&](const auto& glyph) { return glyph.c == c; });
  return it != Glyphs.end() ? &*it : nullptr;
}

constexpr auto CharAdvance = GlyphWidth + 1;
constexpr auto LineGap = 2;
constexpr auto LabelPadding = 2;

// Colors

constexpr auto LabelText = Rgba8{255, 255, 255, 255};
constexpr auto LabelBackground = Rgba8{0, 0, 0, 170};
constexpr auto FloorGridColor = Rgba8{255, 214, 0, 210};
constexpr auto WallGridColor = Rgba8{0, 214, 255, 210};
constexpr auto GridLabelBackground = Rgba8{0, 0, 0, 150};
constexpr auto PlayerColor = Rgba8{60, 255, 60, 255};
constexpr auto PlayerFill = Rgba8{60, 255, 60, 70};
constexpr auto PlayerEyeColor = Rgba8{200, 255, 200, 255};
constexpr auto CompassBackground = Rgba8{0, 0, 0, 150};
constexpr auto CompassRing = Rgba8{255, 255, 255, 230};
constexpr auto CompassNorth = Rgba8{255, 50, 50, 255};
constexpr auto CompassSouth = Rgba8{220, 220, 220, 255};

// Limits

/** The most object labels that `labels: true` draws by default and at most. */
constexpr auto DefaultMaxLabels = size_t(30);
constexpr auto MaxLabels = size_t(100);
/** The most visibility rays for the labels of one image. */
constexpr auto LabelRayBudget = size_t(3000);
/** The most grid lines per axis; coarser steps are used above. */
constexpr auto MaxGridLinesPerAxis = size_t(32);
/** The most visibility rays for the grid of one image. */
constexpr auto GridRayBudget = size_t(12000);
/** The screen length of a grid line piece whose visibility is tested with one ray. */
constexpr auto GridPieceLength = 8.0;
constexpr auto DefaultGridStep = 64.0;

long roundToLong(const double value)
{
  return long(std::lround(value));
}

/** Clips the segment to the rectangle [minX, maxX] x [minY, maxY] (Liang-Barsky). */
bool clipSegment(
  vm::vec2d& a,
  vm::vec2d& b,
  const double minX,
  const double minY,
  const double maxX,
  const double maxY)
{
  const auto d = b - a;
  auto t0 = 0.0;
  auto t1 = 1.0;
  const auto clip = [&](const double p, const double q) {
    if (std::abs(p) < 1e-12)
    {
      return q >= 0.0;
    }
    const auto r = q / p;
    if (p < 0.0)
    {
      if (r > t1)
      {
        return false;
      }
      t0 = std::max(t0, r);
    }
    else
    {
      if (r < t0)
      {
        return false;
      }
      t1 = std::min(t1, r);
    }
    return true;
  };
  if (
    !clip(-d.x(), a.x() - minX) || !clip(d.x(), maxX - a.x())
    || !clip(-d.y(), a.y() - minY) || !clip(d.y(), maxY - a.y()))
  {
    return false;
  }
  const auto start = a;
  a = start + t0 * d;
  b = start + t1 * d;
  return true;
}

bool insideImage(const ProjectedPoint& point, const ImageProjection& projection)
{
  return point.inFront && point.x >= 0.0 && point.y >= 0.0
         && point.x < double(projection.width()) && point.y < double(projection.height());
}

/** The convex hull of the points (monotone chain), counterclockwise. */
std::vector<vm::vec2d> convexHull(std::vector<vm::vec2d> points)
{
  std::ranges::sort(points, [](const auto& lhs, const auto& rhs) {
    return lhs.x() < rhs.x() || (lhs.x() == rhs.x() && lhs.y() < rhs.y());
  });
  if (points.size() < 3)
  {
    return points;
  }
  const auto cross = [](const auto& o, const auto& a, const auto& b) {
    return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
  };
  auto hull = std::vector<vm::vec2d>(2 * points.size());
  auto k = size_t(0);
  for (const auto& point : points)
  {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], point) <= 0)
    {
      --k;
    }
    hull[k++] = point;
  }
  for (auto i = points.size() - 1, t = k + 1; i > 0; --i)
  {
    while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0)
    {
      --k;
    }
    hull[k++] = points[i - 1];
  }
  hull.resize(k - 1);
  return hull;
}

std::string formatNumber(const double value)
{
  const auto rounded = std::round(value);
  return std::abs(value - rounded) < 0.05
           ? fmt::format("{}", static_cast<long long>(rounded))
           : fmt::format("{:.1f}", value);
}

std::string sizeText(const vm::bbox3d& bounds)
{
  const auto size = bounds.size();
  return fmt::format(
    "{}x{}x{}", formatNumber(size.x()), formatNumber(size.y()), formatNumber(size.z()));
}

// Placing labels

struct LabelPlacer
{
  size_t width;
  size_t height;
  std::vector<PixelBox> placed = {};

  /**
   * Finds a position for a label box centered at (x, y), trying positions below and
   * above if it overlaps an earlier label. Returns nullopt if there is none.
   */
  std::optional<PixelBox> place(
    const double x,
    const double y,
    const std::vector<std::string>& lines,
    const int scale)
  {
    const auto box = labelBox(x, y, lines, scale);
    for (const auto offset : {0L, 1L, -1L, 2L, -2L})
    {
      auto candidate = box;
      candidate.y += offset * (box.height + scale);
      if (
        candidate.inside(width, height)
        && std::ranges::none_of(
          placed, [&](const auto& other) { return candidate.intersects(other); }))
      {
        placed.push_back(candidate);
        return candidate;
      }
    }
    return std::nullopt;
  }
};

// Grid

struct GridLine
{
  vm::vec3d start;
  vm::vec3d end;
  Rgba8 color;
  /** The coordinate label, empty if the line has none. */
  std::string label;
};

/** The multiples of step within [min, max]. */
std::vector<double> gridCoordinates(const double min, const double max, const double step)
{
  auto result = std::vector<double>{};
  for (auto k = std::ceil(min / step - 1e-9); k * step <= max + 1e-9; k += 1.0)
  {
    result.push_back(k * step);
  }
  return result;
}

bool labelled(const double coordinate, const double step, const size_t labelEvery)
{
  const auto index = std::llround(coordinate / step);
  return labelEvery > 0 && index % static_cast<long long>(labelEvery) == 0;
}

size_t autoLabelEvery(const size_t lines)
{
  auto result = size_t(1);
  while (lines / result > 6)
  {
    result *= 2;
  }
  return result;
}

/** Whether the camera sees the inner side of a plane through point with normal. */
bool facesCamera(
  const ImageProjection& projection, const vm::vec3d& point, const vm::vec3d& normal)
{
  const auto& camera = projection.camera();
  if (camera.projection == CameraProjection::Orthographic)
  {
    return vm::dot(normal, camera.direction) < -1e-6;
  }
  return vm::dot(normal, camera.position - point) > 1e-6;
}

std::vector<GridLine> gridLines(
  const ImageProjection& projection, const GridAnnotation& grid)
{
  auto result = std::vector<GridLine>{};
  const auto& box = grid.box;
  const auto step = grid.step;
  const auto xs = gridCoordinates(box.min.x(), box.max.x(), step);
  const auto ys = gridCoordinates(box.min.y(), box.max.y(), step);
  const auto zs = gridCoordinates(box.min.z(), box.max.z(), step);
  const auto every = [&](const std::vector<double>& coordinates) {
    return grid.labelEvery > 0 ? grid.labelEvery : autoLabelEvery(coordinates.size());
  };
  const auto label =
    [&](
      const char axis, const double coordinate, const std::vector<double>& coordinates) {
      return labelled(coordinate, step, every(coordinates))
               ? fmt::format("{}={}", axis, formatNumber(coordinate))
               : std::string{};
    };

  if (grid.floor && facesCamera(projection, box.min, vm::vec3d{0, 0, 1}))
  {
    const auto z = box.min.z();
    for (const auto x : xs)
    {
      result.push_back(
        {{x, box.min.y(), z}, {x, box.max.y(), z}, FloorGridColor, label('x', x, xs)});
    }
    for (const auto y : ys)
    {
      result.push_back(
        {{box.min.x(), y, z}, {box.max.x(), y, z}, FloorGridColor, label('y', y, ys)});
    }
  }

  if (grid.walls)
  {
    // the walls at x = min / max: lines along y (at heights z) and along z (at y)
    for (const auto wallX : {box.min.x(), box.max.x()})
    {
      const auto normal = vm::vec3d{wallX == box.min.x() ? 1.0 : -1.0, 0, 0};
      if (!facesCamera(projection, vm::vec3d{wallX, box.min.y(), box.min.z()}, normal))
      {
        continue;
      }
      for (const auto z : zs)
      {
        if (z > box.min.z() + 1e-9)
        {
          result.push_back(
            {{wallX, box.min.y(), z},
             {wallX, box.max.y(), z},
             WallGridColor,
             label('z', z, zs)});
        }
      }
      for (const auto y : ys)
      {
        result.push_back(
          {{wallX, y, box.min.z()},
           {wallX, y, box.max.z()},
           WallGridColor,
           label('y', y, ys)});
      }
    }
    for (const auto wallY : {box.min.y(), box.max.y()})
    {
      const auto normal = vm::vec3d{0, wallY == box.min.y() ? 1.0 : -1.0, 0};
      if (!facesCamera(projection, vm::vec3d{box.min.x(), wallY, box.min.z()}, normal))
      {
        continue;
      }
      for (const auto z : zs)
      {
        if (z > box.min.z() + 1e-9)
        {
          result.push_back(
            {{box.min.x(), wallY, z},
             {box.max.x(), wallY, z},
             WallGridColor,
             label('z', z, zs)});
        }
      }
      for (const auto x : xs)
      {
        result.push_back(
          {{x, wallY, box.min.z()},
           {x, wallY, box.max.z()},
           WallGridColor,
           label('x', x, xs)});
      }
    }
  }
  return result;
}

struct GridLabel
{
  std::string text;
  vm::vec2d position;
  double depth;
  Rgba8 color;
};

/** Draws the grid lines; returns the number of lines drawn and their label positions. */
std::pair<size_t, std::vector<GridLabel>> drawGrid(
  RgbaImage& image,
  const ImageProjection& projection,
  const GridAnnotation& grid,
  const int thickness)
{
  const auto lines = gridLines(projection, grid);
  auto drawn = size_t(0);
  auto labels = std::vector<GridLabel>{};
  if (lines.empty())
  {
    return {drawn, labels};
  }

  const auto width = double(projection.width());
  const auto height = double(projection.height());
  const auto maxPieces = std::max(size_t(4), GridRayBudget / lines.size());

  for (const auto& line : lines)
  {
    const auto projected = projection.projectSegment(line.start, line.end);
    if (!projected)
    {
      continue;
    }
    auto a = projected->first;
    auto b = projected->second;
    if (!clipSegment(a, b, 0.0, 0.0, width, height))
    {
      continue;
    }
    const auto screenLength = vm::length(projected->second - projected->first);
    const auto pieces =
      std::clamp(size_t(std::ceil(screenLength / GridPieceLength)), size_t(1), maxPieces);

    auto any = false;
    auto best = std::optional<GridLabel>{};
    for (size_t i = 0; i < pieces; ++i)
    {
      const auto p0 = line.start + (line.end - line.start) * (double(i) / double(pieces));
      const auto p1 =
        line.start + (line.end - line.start) * (double(i + 1) / double(pieces));
      const auto mid = (p0 + p1) / 2.0;
      const auto projectedMid = projection.project(mid);
      if (!insideImage(projectedMid, projection))
      {
        continue;
      }
      if (grid.visible && !grid.visible(mid))
      {
        continue;
      }
      if (const auto piece = projection.projectSegment(p0, p1))
      {
        drawLine(image, piece->first, piece->second, line.color, thickness);
        any = true;
        if (!line.label.empty() && (!best || projectedMid.depth < best->depth))
        {
          best =
            GridLabel{line.label, projectedMid.pixel(), projectedMid.depth, line.color};
        }
      }
    }
    if (any)
    {
      ++drawn;
      if (best)
      {
        labels.push_back(*best);
      }
    }
  }
  return {drawn, labels};
}

// Player

bool drawPlayer(
  RgbaImage& image,
  const ImageProjection& projection,
  const PlayerAnnotation& player,
  const int thickness,
  LabelPlacer& placer,
  const int scale)
{
  const auto h = player.width / 2.0;
  const auto& f = player.feet;
  const auto corner = [&](const int i) {
    return vm::vec3d{
      f.x() + ((i & 1) ? h : -h),
      f.y() + ((i & 2) ? h : -h),
      f.z() + ((i & 4) ? player.height : 0.0)};
  };

  auto points = std::vector<vm::vec2d>{};
  auto allInFront = true;
  for (int i = 0; i < 8; ++i)
  {
    const auto projected = projection.project(corner(i));
    allInFront = allInFront && projected.inFront;
    points.push_back(projected.pixel());
  }
  if (allInFront)
  {
    fillPolygon(image, convexHull(points), PlayerFill);
  }

  auto any = false;
  for (int i = 0; i < 8; ++i)
  {
    for (const auto bit : {1, 2, 4})
    {
      if (!(i & bit))
      {
        any = drawLine3d(
                image, projection, corner(i), corner(i | bit), PlayerColor, thickness)
              || any;
      }
    }
  }

  // eye height
  const auto eye = [&](const int i) {
    auto point = corner(i);
    point[2] = f.z() + player.eyeHeight;
    return point;
  };
  drawLine3d(image, projection, eye(0), eye(1), PlayerEyeColor, thickness);
  drawLine3d(image, projection, eye(1), eye(3), PlayerEyeColor, thickness);
  drawLine3d(image, projection, eye(3), eye(2), PlayerEyeColor, thickness);
  drawLine3d(image, projection, eye(2), eye(0), PlayerEyeColor, thickness);

  const auto top = projection.project(f + vm::vec3d{0, 0, player.height});
  if (top.inFront)
  {
    const auto lines = std::vector<std::string>{
      fmt::format(
        "player {}x{}x{}",
        formatNumber(player.width),
        formatNumber(player.width),
        formatNumber(player.height)),
      fmt::format("eye {}", formatNumber(player.eyeHeight)),
    };
    const auto box = labelBox(top.x, top.y, lines, scale);
    if (
      auto placed =
        placer.place(top.x, top.y - double(box.height) / 2.0 - scale, lines, scale))
    {
      drawLabel(image, *placed, lines, PlayerColor, LabelBackground, scale);
    }
  }
  return any;
}

// Compass

void drawCompass(
  RgbaImage& image,
  const ImageProjection& projection,
  const int scale,
  LabelPlacer& placer)
{
  const auto radius = double(18 * scale);
  const auto margin = double(4 * scale);
  const auto center =
    vm::vec2d{double(projection.width()) - radius - margin, radius + margin};
  placer.placed.push_back(PixelBox{
    roundToLong(center.x() - radius),
    roundToLong(center.y() - radius),
    roundToLong(2.0 * radius),
    roundToLong(2.0 * radius)});

  // the top of the compass is the camera's horizontal forward direction
  const auto& camera = projection.camera();
  auto forward = vm::vec3d{camera.direction.x(), camera.direction.y(), 0.0};
  if (vm::length(forward) < 1e-3)
  {
    // looking straight down or up: the top of the image
    forward = vm::vec3d{camera.up.x(), camera.up.y(), 0.0};
    if (camera.direction.z() > 0.0)
    {
      forward = -forward;
    }
  }
  forward = vm::normalize(forward);
  const auto right = vm::cross(forward, vm::vec3d{0, 0, 1});
  const auto screen = [&](const vm::vec3d& world) {
    // image y grows downwards
    return vm::vec2d{vm::dot(world, right), -vm::dot(world, forward)};
  };

  const auto north = screen(vm::vec3d{0, 1, 0});
  const auto east = screen(vm::vec3d{1, 0, 0});
  const auto perpendicular = vm::vec2d{-north.y(), north.x()};

  auto disc = std::vector<vm::vec2d>{};
  for (int i = 0; i < 32; ++i)
  {
    const auto angle = 2.0 * std::numbers::pi * double(i) / 32.0;
    disc.push_back(center + radius * vm::vec2d{std::cos(angle), std::sin(angle)});
  }
  fillPolygon(image, disc, CompassBackground);
  drawCircle(image, center, radius, CompassRing, std::max(1, scale / 2));

  const auto arrow = radius * 0.5;
  const auto half = radius * 0.18;
  fillPolygon(
    image,
    {center + arrow * north,
     center + half * perpendicular,
     center - half * perpendicular},
    CompassNorth);
  fillPolygon(
    image,
    {center - arrow * north,
     center + half * perpendicular,
     center - half * perpendicular},
    CompassSouth);

  const auto letter = [&](const char c, const vm::vec2d& direction, const Rgba8& color) {
    const auto position = center + direction * (radius - 5.0 * scale);
    const auto text = std::string(1, c);
    drawText(
      image,
      roundToLong(position.x() - double(textWidth(text, scale)) / 2.0),
      roundToLong(position.y() - double(textHeight(scale)) / 2.0),
      text,
      color,
      scale);
  };
  letter('N', north, CompassNorth);
  letter('S', -north, CompassSouth);
  letter('E', east, CompassSouth);
  letter('W', -east, CompassSouth);
}

// Building annotations from a map

bool isContainerNode(const mdl::Node& node)
{
  if (dynamic_cast<const mdl::GroupNode*>(&node))
  {
    return true;
  }
  const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
  return entityNode && entityNode->hasChildren();
}

bool isNodeOrDescendant(const mdl::Node& node, const mdl::Node& ancestor)
{
  return &node == &ancestor || node.isDescendantOf(ancestor);
}

/** The label priority: groups, brush entities, point entities, brushes, patches. */
int labelPriority(const mdl::Node& node)
{
  if (dynamic_cast<const mdl::GroupNode*>(&node))
  {
    return 0;
  }
  if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node))
  {
    return entityNode->hasChildren() ? 1 : 2;
  }
  return dynamic_cast<const mdl::BrushNode*>(&node) ? 3 : 4;
}

std::vector<std::string> labelLines(const mdl::Node& node, const IdRegistry& ids)
{
  auto lines = std::vector<std::string>{ids.format(node)};
  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    lines.push_back(groupNode->group().name());
  }
  else if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node))
  {
    lines.push_back(entityNode->entity().classname());
  }
  lines.push_back(sizeText(node.logicalBounds()));
  return lines;
}

/** The first hit of the drawn objects on the way to the point, if it is before it. */
bool pointVisible(
  mdl::Map& map,
  const ImageProjection& projection,
  const std::function<bool(const mdl::Node&)>& drawn,
  const vm::vec3d& point,
  const mdl::Node* target)
{
  const auto& camera = projection.camera();
  auto ray = vm::ray3d{};
  auto distance = 0.0;
  if (camera.projection == CameraProjection::Perspective)
  {
    distance = vm::length(point - camera.position);
    if (distance < 1e-6)
    {
      return false;
    }
    ray = vm::ray3d{camera.position, (point - camera.position) / distance};
  }
  else
  {
    distance = projection.depth(point);
    ray = vm::ray3d{point - distance * camera.direction, camera.direction};
  }
  const auto tolerance = std::max(0.5, distance * 1e-3);
  const auto hits = castRay(map, ray, drawn, distance + tolerance);
  for (const auto& hit : hits)
  {
    if (!projection.withinClipRange(projection.depth(hit.point)))
    {
      continue;
    }
    if (target && isNodeOrDescendant(*hit.node, *target))
    {
      return true;
    }
    return hit.distance >= distance - tolerance;
  }
  return true;
}

/** The points to anchor a label of the given bounds at, best first. */
std::vector<vm::vec3d> anchorCandidates(
  const ImageProjection& projection, const vm::bbox3d& bounds)
{
  const auto center = bounds.center();
  auto result = std::vector<vm::vec3d>{center};
  auto faces = std::vector<std::pair<double, vm::vec3d>>{};
  for (size_t axis = 0; axis < 3; ++axis)
  {
    for (const auto sign : {-1.0, 1.0})
    {
      auto normal = vm::vec3d{0, 0, 0};
      normal[axis] = sign;
      auto point = center;
      point[axis] = sign < 0 ? bounds.min[axis] : bounds.max[axis];
      const auto& camera = projection.camera();
      const auto toCamera = camera.projection == CameraProjection::Perspective
                              ? vm::normalize(camera.position - point)
                              : -camera.direction;
      const auto facing = vm::dot(normal, toCamera);
      if (facing > 0.0)
      {
        faces.emplace_back(facing, point);
      }
    }
  }
  std::ranges::sort(
    faces, [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
  for (const auto& [facing, point] : faces)
  {
    result.push_back(point);
  }
  return result;
}

vm::bbox3d boundsOf(const std::vector<mdl::Node*>& nodes)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto* node : nodes)
  {
    if (!isContainerNode(*node))
    {
      result = result ? vm::merge(*result, node->logicalBounds()) : node->logicalBounds();
    }
  }
  return result.value_or(vm::bbox3d{});
}

/**
 * The space around the point that the image center shows: rays from just in front of
 * the surface there find the floor, the ceiling and the walls.
 */
std::optional<vm::bbox3d> spaceAtImageCenter(
  mdl::Map& map,
  const ImageProjection& projection,
  const std::function<bool(const mdl::Node&)>& solid,
  const vm::bbox3d& fallback)
{
  const auto ray = projection.pickRay(
    double(projection.width()) / 2.0, double(projection.height()) / 2.0);
  const auto hits = castRay(map, ray, solid);
  const auto it = std::ranges::find_if(hits, [&](const auto& hit) {
    return projection.withinClipRange(projection.depth(hit.point));
  });
  if (it == hits.end())
  {
    return std::nullopt;
  }

  auto normal = -ray.direction;
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(it->node);
      brushNode && it->faceIndex)
  {
    normal = brushNode->brush().face(*it->faceIndex).normal();
  }
  const auto start = it->point + normal;

  auto min = fallback.min;
  auto max = fallback.max;
  for (size_t axis = 0; axis < 3; ++axis)
  {
    for (const auto sign : {-1.0, 1.0})
    {
      auto direction = vm::vec3d{0, 0, 0};
      direction[axis] = sign;
      const auto axisHits = castRay(map, vm::ray3d{start, direction}, solid);
      if (!axisHits.empty())
      {
        (sign < 0 ? min : max)[axis] = axisHits.front().point[axis];
      }
    }
    min[axis] = std::min(min[axis], start[axis]);
    max[axis] = std::max(max[axis], start[axis]);
  }
  return vm::bbox3d{min, max};
}

} // namespace

// Primitives

void blendPixel(RgbaImage& image, const long x, const long y, const Rgba8& color)
{
  if (x < 0 || y < 0 || x >= long(image.width) || y >= long(image.height))
  {
    return;
  }
  auto* pixel = &image.pixels[(size_t(y) * image.width + size_t(x)) * 4];
  const auto alpha = unsigned(color[3]);
  for (size_t i = 0; i < 3; ++i)
  {
    pixel[i] = static_cast<unsigned char>(
      (unsigned(color[i]) * alpha + unsigned(pixel[i]) * (255 - alpha) + 127) / 255);
  }
  pixel[3] = std::max(pixel[3], color[3]);
}

void drawLine(
  RgbaImage& image,
  const vm::vec2d& start,
  const vm::vec2d& end,
  const Rgba8& color,
  const int thickness)
{
  auto a = start;
  auto b = end;
  const auto margin = double(thickness);
  if (!clipSegment(
        a,
        b,
        -margin,
        -margin,
        double(image.width) + margin,
        double(image.height) + margin))
  {
    return;
  }
  const auto steps =
    std::max(1L, roundToLong(std::max(std::abs(b.x() - a.x()), std::abs(b.y() - a.y()))));
  const auto offset = (thickness - 1) / 2;
  auto lastX = std::numeric_limits<long>::min();
  auto lastY = std::numeric_limits<long>::min();
  for (long i = 0; i <= steps; ++i)
  {
    const auto point = a + (b - a) * (double(i) / double(steps));
    const auto x = long(std::floor(point.x())) - offset;
    const auto y = long(std::floor(point.y())) - offset;
    if (x == lastX && y == lastY)
    {
      continue;
    }
    lastX = x;
    lastY = y;
    for (int dy = 0; dy < thickness; ++dy)
    {
      for (int dx = 0; dx < thickness; ++dx)
      {
        // avoid blending the same pixel twice where squares of neighbouring points
        // overlap
        const auto px = x + dx;
        const auto py = y + dy;
        if (i > 0)
        {
          const auto previous = a + (b - a) * (double(i - 1) / double(steps));
          const auto prevX = long(std::floor(previous.x())) - offset;
          const auto prevY = long(std::floor(previous.y())) - offset;
          if (
            px >= prevX && px < prevX + thickness && py >= prevY
            && py < prevY + thickness)
          {
            continue;
          }
        }
        blendPixel(image, px, py, color);
      }
    }
  }
}

void drawRect(
  RgbaImage& image,
  const long x,
  const long y,
  const long width,
  const long height,
  const Rgba8& color)
{
  if (width <= 0 || height <= 0)
  {
    return;
  }
  fillRect(image, x, y, width, 1, color);
  if (height > 1)
  {
    fillRect(image, x, y + height - 1, width, 1, color);
  }
  if (height > 2)
  {
    fillRect(image, x, y + 1, 1, height - 2, color);
    if (width > 1)
    {
      fillRect(image, x + width - 1, y + 1, 1, height - 2, color);
    }
  }
}

void fillRect(
  RgbaImage& image,
  const long x,
  const long y,
  const long width,
  const long height,
  const Rgba8& color)
{
  const auto x0 = std::max(0L, x);
  const auto y0 = std::max(0L, y);
  const auto x1 = std::min(long(image.width), x + width);
  const auto y1 = std::min(long(image.height), y + height);
  for (auto py = y0; py < y1; ++py)
  {
    for (auto px = x0; px < x1; ++px)
    {
      blendPixel(image, px, py, color);
    }
  }
}

void fillPolygon(
  RgbaImage& image, const std::vector<vm::vec2d>& points, const Rgba8& color)
{
  if (points.size() < 3)
  {
    return;
  }
  auto minY = points.front().y();
  auto maxY = minY;
  for (const auto& point : points)
  {
    minY = std::min(minY, point.y());
    maxY = std::max(maxY, point.y());
  }
  const auto y0 = std::max(0L, long(std::floor(minY)));
  const auto y1 = std::min(long(image.height) - 1, long(std::ceil(maxY)));
  auto crossings = std::vector<double>{};
  for (auto y = y0; y <= y1; ++y)
  {
    const auto scanY = double(y) + 0.5;
    crossings.clear();
    for (size_t i = 0; i < points.size(); ++i)
    {
      const auto& a = points[i];
      const auto& b = points[(i + 1) % points.size()];
      if ((a.y() <= scanY && b.y() > scanY) || (b.y() <= scanY && a.y() > scanY))
      {
        crossings.push_back(a.x() + (scanY - a.y()) / (b.y() - a.y()) * (b.x() - a.x()));
      }
    }
    std::ranges::sort(crossings);
    for (size_t i = 0; i + 1 < crossings.size(); i += 2)
    {
      const auto x0 = std::max(0L, long(std::ceil(crossings[i] - 0.5)));
      const auto x1 =
        std::min(long(image.width) - 1, long(std::ceil(crossings[i + 1] - 0.5)) - 1);
      for (auto x = x0; x <= x1; ++x)
      {
        blendPixel(image, x, y, color);
      }
    }
  }
}

void drawCircle(
  RgbaImage& image,
  const vm::vec2d& center,
  const double radius,
  const Rgba8& color,
  const int thickness)
{
  const auto segments = std::clamp(int(radius), 16, 128);
  auto previous = center + vm::vec2d{radius, 0.0};
  for (int i = 1; i <= segments; ++i)
  {
    const auto angle = 2.0 * std::numbers::pi * double(i) / double(segments);
    const auto point = center + radius * vm::vec2d{std::cos(angle), std::sin(angle)};
    drawLine(image, previous, point, color, thickness);
    previous = point;
  }
}

// Text

bool hasGlyph(const char c)
{
  return findGlyph(c) != nullptr;
}

long textWidth(const std::string_view text, const int scale)
{
  return text.empty() ? 0 : long(text.size()) * CharAdvance * scale - scale;
}

long textHeight(const int scale)
{
  return GlyphHeight * scale;
}

void drawText(
  RgbaImage& image,
  const long x,
  const long y,
  const std::string_view text,
  const Rgba8& color,
  const int scale)
{
  auto penX = x;
  for (const auto c : text)
  {
    const auto* glyph = findGlyph(c);
    if (!glyph)
    {
      glyph = findGlyph('?');
    }
    for (int row = 0; row < GlyphHeight; ++row)
    {
      for (int column = 0; column < GlyphWidth; ++column)
      {
        if (glyph->rows[size_t(row * GlyphWidth + column)] == '#')
        {
          fillRect(image, penX + column * scale, y + row * scale, scale, scale, color);
        }
      }
    }
    penX += CharAdvance * scale;
  }
}

bool PixelBox::intersects(const PixelBox& other) const
{
  return x < other.x + other.width && other.x < x + width && y < other.y + other.height
         && other.y < y + height;
}

bool PixelBox::inside(const size_t imageWidth, const size_t imageHeight) const
{
  return x >= 0 && y >= 0 && x + width <= long(imageWidth)
         && y + height <= long(imageHeight);
}

PixelBox labelBox(
  const double x, const double y, const std::vector<std::string>& lines, const int scale)
{
  auto width = 0L;
  for (const auto& line : lines)
  {
    width = std::max(width, textWidth(line, scale));
  }
  const auto count = long(lines.size());
  const auto height =
    count * textHeight(scale) + std::max(0L, count - 1) * LineGap * scale;
  const auto boxWidth = width + 2 * LabelPadding * scale;
  const auto boxHeight = height + 2 * LabelPadding * scale;
  return PixelBox{
    roundToLong(x - double(boxWidth) / 2.0),
    roundToLong(y - double(boxHeight) / 2.0),
    boxWidth,
    boxHeight};
}

void drawLabel(
  RgbaImage& image,
  const PixelBox& box,
  const std::vector<std::string>& lines,
  const Rgba8& textColor,
  const Rgba8& background,
  const int scale)
{
  fillRect(image, box.x, box.y, box.width, box.height, background);
  auto y = box.y + LabelPadding * scale;
  for (const auto& line : lines)
  {
    drawText(image, box.x + LabelPadding * scale, y, line, textColor, scale);
    y += textHeight(scale) + LineGap * scale;
  }
}

bool drawLine3d(
  RgbaImage& image,
  const ImageProjection& projection,
  const vm::vec3d& start,
  const vm::vec3d& end,
  const Rgba8& color,
  const int thickness)
{
  const auto projected = projection.projectSegment(start, end);
  if (!projected)
  {
    return false;
  }
  drawLine(image, projected->first, projected->second, color, thickness);
  return true;
}

int annotationScale(const size_t width, const size_t height)
{
  const auto size = std::min(width, height);
  return size >= 1200 ? 3 : size >= 400 ? 2 : 1;
}

AnnotationReport drawAnnotations(
  RgbaImage& image, const ImageProjection& projection, const AnnotationSpec& spec)
{
  auto report = AnnotationReport{};
  const auto scale = spec.scale > 0
                       ? spec.scale
                       : annotationScale(projection.width(), projection.height());
  const auto thickness = std::max(1, scale - 1);
  auto placer = LabelPlacer{projection.width(), projection.height()};

  if (spec.compass)
  {
    // reserve the corner before labels are placed; drawn last, on top
    placer.placed.push_back(
      PixelBox{long(projection.width()) - 44 * scale, 0, 44 * scale, 44 * scale});
  }

  auto gridLabels = std::vector<GridLabel>{};
  if (spec.grid)
  {
    auto [lines, labels] = drawGrid(image, projection, *spec.grid, thickness);
    report.gridLines = lines;
    gridLabels = std::move(labels);
  }

  if (spec.player)
  {
    report.player =
      drawPlayer(image, projection, *spec.player, thickness + 1, placer, scale);
  }

  for (const auto& label : spec.labels)
  {
    const auto projected = projection.project(label.anchor);
    if (!insideImage(projected, projection))
    {
      ++report.labelsSkipped;
      continue;
    }
    if (const auto box = placer.place(projected.x, projected.y, label.lines, scale))
    {
      drawLabel(image, *box, label.lines, LabelText, LabelBackground, scale);
      report.labelled.push_back(label.objectId);
    }
    else
    {
      ++report.labelsSkipped;
    }
  }

  for (const auto& label : gridLabels)
  {
    const auto lines = std::vector<std::string>{label.text};
    if (
      const auto box = placer.place(label.position.x(), label.position.y(), lines, scale))
    {
      drawLabel(image, *box, lines, label.color, GridLabelBackground, scale);
      ++report.gridLabels;
    }
  }

  if (spec.compass)
  {
    placer.placed.clear();
    drawCompass(image, projection, scale, placer);
    report.compass = true;
  }
  return report;
}

Result<AnnotationSpec, ToolError> buildAnnotations(
  mdl::Map& map,
  const IdRegistry& ids,
  const ImageProjection& projection,
  const std::vector<mdl::Node*>& sceneNodes,
  const std::function<bool(const mdl::Node&)>& drawn,
  const Json& annotations,
  std::vector<Warning>& warnings)
{
  auto spec = AnnotationSpec{};
  const auto sceneBounds = boundsOf(sceneNodes);

  // labels
  if (const auto* labels = findMember(annotations, "labels");
      labels && !(labels->is_boolean() && !labels->get<bool>()))
  {
    const auto options = labels->is_object() ? *labels : Json::object();
    const auto max =
      std::min(MaxLabels, size_t(options.value("max", int64_t(DefaultMaxLabels))));
    auto candidates = std::vector<const mdl::Node*>{};
    auto explicitIds = false;
    if (const auto* idList = findMember(options, "ids"))
    {
      explicitIds = true;
      for (const auto& id : *idList)
      {
        auto node = ids.resolve(id.get<std::string>());
        if (node.is_error())
        {
          return errorOf(node);
        }
        candidates.push_back(node.value());
      }
    }
    else
    {
      const auto inScene =
        std::unordered_set<const mdl::Node*>{sceneNodes.begin(), sceneNodes.end()};
      for (const auto* node : sceneNodes)
      {
        // members of drawn groups and brush entities are covered by their container
        const auto* parent = node->parent();
        auto covered = false;
        for (; parent; parent = parent->parent())
        {
          if (inScene.contains(parent) && isContainerNode(*parent))
          {
            covered = true;
            break;
          }
        }
        if (!covered)
        {
          candidates.push_back(node);
        }
      }
      // most important first, then the larger ones
      std::ranges::stable_sort(candidates, [](const auto* lhs, const auto* rhs) {
        const auto lp = labelPriority(*lhs);
        const auto rp = labelPriority(*rhs);
        if (lp != rp)
        {
          return lp < rp;
        }
        return vm::length(lhs->logicalBounds().size())
               > vm::length(rhs->logicalBounds().size());
      });
    }

    auto rays = size_t(0);
    for (const auto* node : candidates)
    {
      if (spec.labels.size() >= max || rays >= LabelRayBudget)
      {
        break;
      }
      const auto& bounds = node->logicalBounds();
      for (const auto& anchor : anchorCandidates(projection, bounds))
      {
        const auto projected = projection.project(anchor);
        if (!insideImage(projected, projection))
        {
          continue;
        }
        if (!explicitIds)
        {
          ++rays;
          if (!pointVisible(map, projection, drawn, anchor, node))
          {
            continue;
          }
        }
        spec.labels.push_back(
          LabelAnnotation{ids.format(*node), labelLines(*node, ids), anchor});
        break;
      }
    }
  }

  // grid
  if (const auto* grid = findMember(annotations, "grid");
      grid && !(grid->is_boolean() && !grid->get<bool>()))
  {
    const auto options = grid->is_object() ? *grid : Json::object();
    auto annotation = GridAnnotation{};
    annotation.step = options.value("step", DefaultGridStep);
    const auto planes = options.value("planes", std::string{"both"});
    annotation.floor = planes != "walls";
    annotation.walls = planes != "floor";
    annotation.labelEvery = size_t(options.value("labelEvery", int64_t(0)));

    auto box = std::optional<vm::bbox3d>{};
    if (const auto* boxJson = findMember(options, "box"))
    {
      box = *boxFromJson(*boxJson);
    }
    else
    {
      const auto solid = [&](const mdl::Node& node) {
        if (!drawn(node) || isPointEntity(node))
        {
          return false;
        }
        const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
        return !brushNode || classifyBrush(*brushNode) != BrushClass::Trigger;
      };
      box = spaceAtImageCenter(map, projection, solid, sceneBounds);
      if (!box && !sceneNodes.empty())
      {
        box = sceneBounds;
      }
    }

    if (!box)
    {
      warnings.push_back(Warning{
        "GRID_NOT_DRAWN",
        "The image shows nothing to put a grid on; pass annotations.grid.box."});
    }
    else
    {
      annotation.box = *box;
      const auto size = box->size();
      const auto longest = std::max({size.x(), size.y(), size.z()});
      const auto requested = annotation.step;
      while (longest / annotation.step > double(MaxGridLinesPerAxis))
      {
        annotation.step *= 2.0;
      }
      if (annotation.step != requested && findMember(options, "step"))
      {
        warnings.push_back(Warning{
          "GRID_STEP_INCREASED",
          fmt::format(
            "A grid step of {} would draw too many lines; the step is {}.",
            formatNumber(requested),
            formatNumber(annotation.step))});
      }
      annotation.visible = [&map, &projection, drawn](const vm::vec3d& point) {
        return pointVisible(map, projection, drawn, point, nullptr);
      };
      spec.grid = std::move(annotation);
    }
  }

  // compass
  if (const auto* compass = findMember(annotations, "compass"))
  {
    spec.compass = compass->is_boolean() && compass->get<bool>();
  }

  // player
  if (const auto* player = findMember(annotations, "player"))
  {
    const auto point = *vec3FromJson((*player)["point"]);
    auto feet = point;
    if (player->value("onFloor", true))
    {
      if (const auto floor = findFloor(map, point + vm::vec3d{0, 0, 1}))
      {
        feet[2] = *floor;
      }
      else
      {
        warnings.push_back(Warning{
          "NO_FLOOR",
          fmt::format(
            "There is no floor below ({}, {}, {}); the player stands at the point.",
            formatNumber(point.x()),
            formatNumber(point.y()),
            formatNumber(point.z()))});
      }
    }
    const auto size = playerSize(map.gameInfo().gameConfig);
    spec.player = PlayerAnnotation{feet, size.width, size.height, size.eyeHeight};
  }
  return spec;
}

Json annotationsJson(const AnnotationSpec& spec, const AnnotationReport& report)
{
  auto grid = Json(nullptr);
  if (spec.grid)
  {
    grid = Json{
      {"step", roundForOutput(spec.grid->step)},
      {"box", toJson(spec.grid->box)},
      {"floor", spec.grid->floor},
      {"walls", spec.grid->walls},
      {"lines", report.gridLines},
      {"labels", report.gridLabels},
    };
  }
  auto player = Json(nullptr);
  if (spec.player)
  {
    player = Json{
      {"feet", toJson(spec.player->feet)},
      {"width", roundForOutput(spec.player->width)},
      {"height", roundForOutput(spec.player->height)},
      {"eyeHeight", roundForOutput(spec.player->eyeHeight)},
      {"visible", report.player},
    };
  }
  return Json{
    {"labels", report.labelled.size()},
    {"labelled", report.labelled},
    {"labelsSkipped", report.labelsSkipped},
    {"grid", std::move(grid)},
    {"compass", report.compass},
    {"player", std::move(player)},
  };
}

} // namespace tb::mcp
