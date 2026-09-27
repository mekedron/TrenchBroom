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

#include "mcp/tools/SpaceAnalysis.h"

#include "mcp/AgentCamera.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "vm/ray.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace tb::mcp
{
namespace
{

constexpr auto Unassigned = int32_t(-4);

std::string lowerName(std::string_view name)
{
  if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos)
  {
    name = name.substr(slash + 1);
  }
  auto result = std::string{name};
  std::ranges::transform(
    result, result.begin(), [](const unsigned char c) { return char(std::tolower(c)); });
  return result;
}

bool isCuboid(const mdl::Brush& brush)
{
  if (brush.faceCount() != 6)
  {
    return false;
  }
  return std::ranges::all_of(brush.faces(), [](const auto& face) {
    const auto& n = face.normal();
    return std::abs(n.x()) > 0.999999 || std::abs(n.y()) > 0.999999
           || std::abs(n.z()) > 0.999999;
  });
}

bool omittedLayer(const mdl::Node& node)
{
  const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(&node);
  return layerNode && layerNode->layer().omitFromExport();
}

/** Visits every node below `node`, skipping layers that are omitted from export. */
template <typename F>
void visitNodes(const mdl::Node& node, const F& f)
{
  for (const auto* child : node.children())
  {
    if (omittedLayer(*child))
    {
      continue;
    }
    f(*child);
    visitNodes(*child, f);
  }
}

/** Caches brush roles for the duration of one analysis. */
class RoleCache
{
private:
  std::unordered_map<const mdl::BrushNode*, BrushRole> m_roles;

public:
  const BrushRole& operator()(const mdl::BrushNode& brushNode)
  {
    auto it = m_roles.find(&brushNode);
    if (it == m_roles.end())
    {
      it = m_roles.emplace(&brushNode, brushRole(brushNode)).first;
    }
    return it->second;
  }
};

vm::bbox3d shrink(const vm::bbox3d& box, const double amount)
{
  auto result = box;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto delta = std::min(amount, (box.max[i] - box.min[i]) / 4.0);
    result.min[i] += delta;
    result.max[i] -= delta;
  }
  return result;
}

bool boxesOverlap(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (lhs.max[i] <= rhs.min[i] || rhs.max[i] <= lhs.min[i])
    {
      return false;
    }
  }
  return true;
}

/** Whether the segment from p to q passes through the interior of the brush. */
bool segmentCrossesInterior(
  const mdl::Brush& brush, const vm::vec3d& p, const vm::vec3d& q)
{
  auto tMin = 0.0;
  auto tMax = 1.0;
  const auto d = q - p;
  for (const auto& face : brush.faces())
  {
    const auto& plane = face.boundary();
    const auto distance = plane.point_distance(p);
    const auto denominator = vm::dot(plane.normal, d);
    if (std::abs(denominator) < 1e-12)
    {
      if (distance >= 0.0)
      {
        return false;
      }
      continue;
    }
    const auto t = -distance / denominator;
    if (denominator < 0.0)
    {
      tMin = std::max(tMin, t);
    }
    else
    {
      tMax = std::min(tMax, t);
    }
    if (tMax - tMin <= 1e-6)
    {
      return false;
    }
  }
  return true;
}

constexpr auto Directions = std::array<std::array<int, 3>, 6>{{
  {-1, 0, 0},
  {1, 0, 0},
  {0, -1, 0},
  {0, 1, 0},
  {0, 0, -1},
  {0, 0, 1},
}};

std::optional<CellIndex> neighbour(
  const VoxelGrid& grid, const CellIndex& cell, const std::array<int, 3>& offset)
{
  auto result = cell;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto value = int64_t(cell[i]) + offset[i];
    if (value < 0 || value >= int64_t(grid.dims[i]))
    {
      return std::nullopt;
    }
    result[i] = size_t(value);
  }
  return result;
}

/**
 * Calls f(neighbourIndex) for the 6 face neighbours of the cell with the given index.
 * Faster than neighbour() in the flood fills.
 */
template <typename F>
void forEachFaceNeighbour(const VoxelGrid& grid, const size_t index, const F& f)
{
  const auto nx = grid.dims[0];
  const auto nxy = nx * grid.dims[1];
  const auto x = index % nx;
  const auto y = (index / nx) % grid.dims[1];
  const auto z = index / nxy;
  if (x > 0)
  {
    f(index - 1);
  }
  if (x + 1 < nx)
  {
    f(index + 1);
  }
  if (y > 0)
  {
    f(index - nx);
  }
  if (y + 1 < grid.dims[1])
  {
    f(index + nx);
  }
  if (z > 0)
  {
    f(index - nxy);
  }
  if (z + 1 < grid.dims[2])
  {
    f(index + nxy);
  }
}

/**
 * Calls f(neighbourIndex) for the cells in the 3 x 3 x 3 block around the cell with the
 * given index, including the cell itself if `includeSelf` is set.
 */
template <typename F>
void forEachBlockNeighbour(
  const VoxelGrid& grid, const size_t index, const bool includeSelf, const F& f)
{
  const auto nx = grid.dims[0];
  const auto ny = grid.dims[1];
  const auto nz = grid.dims[2];
  const auto x = index % nx;
  const auto y = (index / nx) % ny;
  const auto z = index / (nx * ny);
  const auto x0 = x > 0 ? x - 1 : x;
  const auto x1 = x + 1 < nx ? x + 1 : x;
  const auto y0 = y > 0 ? y - 1 : y;
  const auto y1 = y + 1 < ny ? y + 1 : y;
  const auto z0 = z > 0 ? z - 1 : z;
  const auto z1 = z + 1 < nz ? z + 1 : z;
  for (auto zz = z0; zz <= z1; ++zz)
  {
    for (auto yy = y0; yy <= y1; ++yy)
    {
      const auto rowStart = nx * (yy + ny * zz);
      for (auto xx = x0; xx <= x1; ++xx)
      {
        const auto neighbourIndex = rowStart + xx;
        if (includeSelf || neighbourIndex != index)
        {
          f(neighbourIndex);
        }
      }
    }
  }
}

template <typename F>
void forEachBorderCell(const VoxelGrid& grid, const F& f)
{
  const auto [nx, ny, nz] = grid.dims;
  for (size_t z = 0; z < nz; ++z)
  {
    for (size_t y = 0; y < ny; ++y)
    {
      for (size_t x = 0; x < nx; ++x)
      {
        if (x == 0 || y == 0 || z == 0 || x + 1 == nx || y + 1 == ny || z + 1 == nz)
        {
          f(grid.index({x, y, z}));
        }
        else if (x == 1)
        {
          // skip the interior of the row
          x = nx - 2;
        }
      }
    }
  }
}

double roundToPowerOfTwo(const double value)
{
  return std::exp2(std::round(std::log2(std::max(value, 1.0))));
}

std::string spaceHash(const VoxelGrid& grid, const vm::bbox3d& bounds)
{
  auto hash = uint64_t(14695981039346656037ull);
  const auto add = [&](const double value) {
    const auto quantized = int64_t(std::llround(value / grid.cellSize));
    for (size_t i = 0; i < 8; ++i)
    {
      hash ^= uint64_t((quantized >> (8 * i)) & 0xff);
      hash *= 1099511628211ull;
    }
  };
  for (size_t i = 0; i < 3; ++i)
  {
    add(bounds.min[i]);
    add(bounds.max[i]);
  }
  add(grid.cellSize * grid.cellSize);
  return fmt::format("space:{:08x}", uint32_t(hash ^ (hash >> 32)));
}

const mdl::Node* parentLayer(const mdl::Node& node)
{
  for (const auto* current = node.parent(); current; current = current->parent())
  {
    if (dynamic_cast<const mdl::LayerNode*>(current))
    {
      return current;
    }
  }
  return nullptr;
}

bool inGroup(const mdl::Node& node)
{
  for (const auto* current = node.parent(); current; current = current->parent())
  {
    if (dynamic_cast<const mdl::GroupNode*>(current))
    {
      return true;
    }
  }
  return false;
}

std::optional<double> firstHitDistance(
  mdl::Map& map,
  const vm::ray3d& ray,
  const std::function<bool(const mdl::Node&)>& accept,
  const double maxDistance)
{
  const auto hits = castRay(map, ray, accept, maxDistance);
  return hits.empty() ? std::nullopt : std::optional{hits.front().distance};
}

std::function<bool(const mdl::Node&)> acceptRole(
  RoleCache& roles, bool BrushRole::*member, const bool patches)
{
  return [&roles, member, patches](const mdl::Node& node) {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      return roles(*brushNode).*member;
    }
    return patches && dynamic_cast<const mdl::PatchNode*>(&node) != nullptr;
  };
}

} // namespace

// Brush roles

bool isToolMaterialName(const std::string_view materialName)
{
  const auto name = lowerName(materialName);
  return name.find("clip") != std::string::npos || name.starts_with("trigger")
         || name.starts_with("aaatrigger") || name.starts_with("hint") || name == "skip"
         || name == "origin" || name == "bevel" || name.starts_with("areaportal");
}

bool isClipMaterialName(const std::string_view materialName)
{
  return lowerName(materialName).find("clip") != std::string::npos;
}

bool isLiquidMaterialName(const std::string_view materialName)
{
  const auto name = lowerName(materialName);
  return (!name.empty() && (name.front() == '*' || name.front() == '!'))
         || name.find("water") != std::string::npos
         || name.find("lava") != std::string::npos
         || name.find("slime") != std::string::npos;
}

BrushRole brushRole(const mdl::BrushNode& brushNode)
{
  auto toolOnly = true;
  auto liquidOnly = true;
  auto anyClip = false;
  for (const auto& face : brushNode.brush().faces())
  {
    const auto& material = face.materialName();
    const auto tool = isToolMaterialName(material);
    toolOnly = toolOnly && tool;
    liquidOnly = liquidOnly && isLiquidMaterialName(material);
    anyClip = anyClip || (tool && isClipMaterialName(material));
  }

  auto role = BrushRole{};
  const auto* entityNode = owningBrushEntity(brushNode);
  const auto classname =
    entityNode ? entityNode->entity().classname() : std::string{"worldspawn"};
  const auto detail = classname.starts_with("func_detail");
  const auto structural = !entityNode || classname == "func_group"
                          || (detail && classname != "func_detail_illusionary");
  role.liquid = liquidOnly || classname == "func_water";

  if (structural)
  {
    role.spaceSolid = !toolOnly && !liquidOnly;
    role.seals = role.spaceSolid && !detail;
    role.blocksPlayer = role.spaceSolid || anyClip;
    role.blocksObjects = role.spaceSolid;
  }
  else if (classname.starts_with("func_door"))
  {
    role.door = true;
    role.blocksObjects = !toolOnly;
  }
  else if (
    classname.starts_with("trigger_") || classname == "func_illusionary"
    || classname == "func_detail_illusionary" || role.liquid)
  {
    // not solid
  }
  else
  {
    role.blocksPlayer = !toolOnly || anyClip;
    role.blocksObjects = !toolOnly;
  }
  return role;
}

// Voxel grid

CellIndex VoxelGrid::cellOf(const size_t index) const
{
  return {index % dims[0], (index / dims[0]) % dims[1], index / (dims[0] * dims[1])};
}

vm::bbox3d VoxelGrid::cellBounds(const CellIndex& cell) const
{
  const auto min =
    origin + vm::vec3d{double(cell[0]), double(cell[1]), double(cell[2])} * cellSize;
  return {min, min + vm::vec3d{cellSize, cellSize, cellSize}};
}

vm::vec3d VoxelGrid::cellCenter(const CellIndex& cell) const
{
  return cellBounds(cell).center();
}

std::optional<CellIndex> VoxelGrid::cellAt(const vm::vec3d& point) const
{
  auto result = CellIndex{};
  for (size_t i = 0; i < 3; ++i)
  {
    const auto value = std::floor((point[i] - origin[i]) / cellSize);
    if (value < 0.0 || value >= double(dims[i]))
    {
      return std::nullopt;
    }
    result[i] = size_t(value);
  }
  return result;
}

vm::bbox3d VoxelGrid::bounds() const
{
  return {
    origin,
    origin + vm::vec3d{double(dims[0]), double(dims[1]), double(dims[2])} * cellSize};
}

Result<VoxelGrid, ToolError> makeGrid(
  const vm::bbox3d& region,
  const double cellSize,
  const size_t padding,
  const size_t maxCells)
{
  if (!(cellSize > 0.0))
  {
    return makeError(ErrorCode::InvalidArgument, "The cell size must be positive.");
  }
  auto grid = VoxelGrid{};
  grid.cellSize = cellSize;
  auto total = 1.0;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto first = std::floor(region.min[i] / cellSize) - double(padding);
    const auto last = std::ceil(region.max[i] / cellSize) + double(padding);
    grid.origin[i] = first * cellSize;
    const auto count = std::max(1.0, last - first);
    grid.dims[i] = size_t(count);
    total *= count;
  }
  if (total > double(maxCells))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The analysis grid would have {} cells of size {}; at most {} are allowed.",
        size_t(total),
        cellSize,
        maxCells),
      fmt::format(
        "Pass a larger cellSize (e.g. {}) or a smaller region.",
        cellSize * std::ceil(std::cbrt(total / double(maxCells)) + 0.01)));
  }
  grid.solid.assign(grid.size(), 0);
  return grid;
}

void rasterize(
  VoxelGrid& grid,
  const mdl::Map& map,
  const std::function<bool(const mdl::BrushNode&, const BrushRole&)>& accept)
{
  constexpr auto Eps = 1e-6;
  visitNodes(map.worldNode(), [&](const mdl::Node& node) {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    if (!brushNode || !accept(*brushNode, brushRole(*brushNode)))
    {
      return;
    }
    const auto& brush = brushNode->brush();
    const auto& bounds = brush.bounds();
    auto lo = CellIndex{};
    auto hi = CellIndex{};
    for (size_t i = 0; i < 3; ++i)
    {
      const auto first =
        std::floor((bounds.min[i] - grid.origin[i]) / grid.cellSize + Eps);
      const auto last =
        std::ceil((bounds.max[i] - grid.origin[i]) / grid.cellSize - Eps) - 1.0;
      if (last < 0.0 || first >= double(grid.dims[i]) || last < first)
      {
        return;
      }
      lo[i] = size_t(std::max(0.0, first));
      hi[i] = size_t(std::min(double(grid.dims[i] - 1), last));
    }

    const auto cuboid = isCuboid(brush);
    for (auto z = lo[2]; z <= hi[2]; ++z)
    {
      for (auto y = lo[1]; y <= hi[1]; ++y)
      {
        for (auto x = lo[0]; x <= hi[0]; ++x)
        {
          const auto index = grid.index({x, y, z});
          if (grid.solid[index])
          {
            continue;
          }
          if (cuboid)
          {
            grid.solid[index] = 1;
            continue;
          }
          const auto cell = grid.cellBounds({x, y, z});
          const auto center = cell.center();
          const auto centerInside =
            std::ranges::all_of(brush.faces(), [&](const auto& face) {
              return face.boundary().point_distance(center) < 0.0;
            });
          if (centerInside || intersectsInterior(brush, cell))
          {
            grid.solid[index] = 1;
          }
        }
      }
    }
  });
}

std::optional<vm::bbox3d> brushBounds(
  const mdl::Map& map,
  const std::function<bool(const mdl::BrushNode&, const BrushRole&)>& accept)
{
  auto result = std::optional<vm::bbox3d>{};
  visitNodes(map.worldNode(), [&](const mdl::Node& node) {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
        brushNode && accept(*brushNode, brushRole(*brushNode)))
    {
      result = result ? vm::merge(*result, node.logicalBounds()) : node.logicalBounds();
    }
  });
  return result;
}

double defaultCellSize(const mdl::Map& map)
{
  const auto width = playerSize(map.gameInfo().gameConfig).width;
  return roundToPowerOfTwo(width > 0.0 ? width / 2.0 : 16.0);
}

// Leak prediction

namespace
{

/** The number of axis directions in which a march from the cell hits a solid cell. */
size_t enclosure(const VoxelGrid& grid, const CellIndex& cell)
{
  auto count = size_t(0);
  for (const auto& direction : Directions)
  {
    auto current = neighbour(grid, cell, direction);
    while (current && !grid.solid[grid.index(*current)])
    {
      current = neighbour(grid, *current, direction);
    }
    if (current)
    {
      ++count;
    }
  }
  return count;
}

std::optional<CellIndex> entityCell(
  const mdl::Map& map,
  const VoxelGrid& grid,
  const vm::vec3d& position,
  const std::function<bool(const mdl::BrushNode&)>& blocks)
{
  const auto cell = grid.cellAt(position);
  if (!cell)
  {
    return std::nullopt;
  }
  if (!grid.solid[grid.index(*cell)])
  {
    return cell;
  }

  // the origin's cell is overlapped by a brush; use the nearest free neighbour that a
  // straight line from the origin reaches
  auto best = std::optional<CellIndex>{};
  auto bestDistance = std::numeric_limits<double>::max();
  for (int dz = -1; dz <= 1; ++dz)
  {
    for (int dy = -1; dy <= 1; ++dy)
    {
      for (int dx = -1; dx <= 1; ++dx)
      {
        const auto candidate = neighbour(grid, *cell, {dx, dy, dz});
        if (!candidate || grid.solid[grid.index(*candidate)])
        {
          continue;
        }
        const auto center = grid.cellCenter(*candidate);
        const auto distance = vm::squared_distance(center, position);
        if (distance >= bestDistance)
        {
          continue;
        }
        const auto segmentBounds =
          vm::bbox3d{vm::min(center, position), vm::max(center, position)};
        const auto blocked = std::ranges::any_of(
          map.worldNode().nodeTree().find_intersectors(segmentBounds),
          [&](const auto* node) {
            const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
            return brushNode && blocks(*brushNode) && !inOmittedLayer(*brushNode)
                   && segmentCrossesInterior(brushNode->brush(), position, center);
          });
        if (!blocked)
        {
          best = candidate;
          bestDistance = distance;
        }
      }
    }
  }
  return best;
}

} // namespace

LeakReport predictLeaks(const mdl::Map& map, const LeakOptions& options)
{
  auto report = LeakReport{};
  const auto seals = [](const mdl::BrushNode&, const BrushRole& role) {
    return role.seals;
  };
  const auto bounds = brushBounds(map, seals);
  if (!bounds)
  {
    report.skippedReason = "The map has no sealing brushes.";
    return report;
  }

  auto cellSize = options.cellSize;
  if (!(cellSize > 0.0))
  {
    const auto budget = double(std::min(options.maxCells, size_t(1'000'000)));
    cellSize = 8.0;
    const auto size = bounds->size();
    while ((size.x() / cellSize + 2) * (size.y() / cellSize + 2)
               * (size.z() / cellSize + 2)
             > budget
           && cellSize < 4096.0)
    {
      cellSize *= 2.0;
    }
  }
  report.cellSize = cellSize;

  auto gridResult = makeGrid(*bounds, cellSize, 1, options.maxCells);
  if (gridResult.is_error())
  {
    report.skippedReason = errorOf(gridResult).message;
    return report;
  }
  auto grid = std::move(gridResult).value();
  rasterize(grid, map, seals);

  // flood fill from the border; parent holds 1 + the direction to the parent cell, or 7
  // for the border cells
  constexpr auto Seed = uint8_t(7);
  auto parent = std::vector<uint8_t>(grid.size(), 0);
  auto queue = std::vector<size_t>{};
  queue.reserve(grid.size() / 4);
  forEachBorderCell(grid, [&](const size_t index) {
    if (!grid.solid[index] && !parent[index])
    {
      parent[index] = Seed;
      queue.push_back(index);
    }
  });
  const auto nx = grid.dims[0];
  const auto nxy = nx * grid.dims[1];
  for (size_t head = 0; head < queue.size(); ++head)
  {
    const auto index = queue[head];
    const auto x = index % nx;
    const auto y = (index / nx) % grid.dims[1];
    const auto z = index / nxy;
    const auto visit = [&](const size_t next, const uint8_t directionToParent) {
      if (!parent[next] && !grid.solid[next])
      {
        parent[next] = directionToParent;
        queue.push_back(next);
      }
    };
    // direction codes: 1 -x, 2 +x, 3 -y, 4 +y, 5 -z, 6 +z (towards the parent)
    if (x > 0)
    {
      visit(index - 1, 2);
    }
    if (x + 1 < nx)
    {
      visit(index + 1, 1);
    }
    if (y > 0)
    {
      visit(index - nx, 4);
    }
    if (y + 1 < grid.dims[1])
    {
      visit(index + nx, 3);
    }
    if (z > 0)
    {
      visit(index - nxy, 6);
    }
    if (z + 1 < grid.dims[2])
    {
      visit(index + nxy, 5);
    }
  }

  const auto sealsBrush = [](const mdl::BrushNode& brushNode) {
    return brushRole(brushNode).seals;
  };

  visitNodes(map.worldNode(), [&](const mdl::Node& node) {
    if (!isPointEntity(node))
    {
      return;
    }
    const auto& entityNode = static_cast<const mdl::EntityNode&>(node);
    if (!entityNode.entity().hasProperty("origin"))
    {
      return;
    }
    ++report.checkedEntities;

    const auto position = entityNode.entity().origin();
    if (!grid.cellAt(position))
    {
      report.findings.push_back(LeakFinding{&entityNode, position, std::nullopt, {}});
      return;
    }
    const auto cell = entityCell(map, grid, position, sealsBrush);
    if (!cell || !parent[grid.index(*cell)])
    {
      return;
    }

    auto finding = LeakFinding{&entityNode, position, std::nullopt, {}};
    if (enclosure(grid, *cell) >= 4)
    {
      // follow the path to the outside until it leaves the enclosed region
      auto previous = *cell;
      auto current = *cell;
      while (true)
      {
        const auto code = parent[grid.index(current)];
        if (code == Seed)
        {
          break;
        }
        const auto next = neighbour(grid, current, Directions[code - 1]);
        if (!next)
        {
          break;
        }
        previous = current;
        current = *next;
        if (enclosure(grid, current) < 4)
        {
          break;
        }
      }
      auto gap = vm::merge(grid.cellBounds(previous), grid.cellBounds(current));
      finding.gap = gap;

      const auto search = gap.expand(grid.cellSize);
      auto brushes = std::vector<const mdl::BrushNode*>{};
      for (const auto* candidate : map.worldNode().nodeTree().find_intersectors(search))
      {
        const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(candidate);
        if (brushNode && sealsBrush(*brushNode) && !inOmittedLayer(*brushNode))
        {
          brushes.push_back(brushNode);
        }
      }
      const auto center = gap.center();
      std::ranges::sort(brushes, [&](const auto* lhs, const auto* rhs) {
        return vm::squared_distance(lhs->logicalBounds().center(), center)
               < vm::squared_distance(rhs->logicalBounds().center(), center);
      });
      if (brushes.size() > 8)
      {
        brushes.resize(8);
      }
      finding.gapBrushes = std::move(brushes);
    }
    report.findings.push_back(std::move(finding));
  });

  report.analyzed = true;
  return report;
}

// Spaces

int32_t SpaceMap::labelAt(const vm::vec3d& point) const
{
  const auto cell = grid.cellAt(point);
  return cell ? labels[grid.index(*cell)] : Void;
}

std::optional<size_t> SpaceMap::spaceAt(
  const vm::vec3d& point, const size_t searchCells) const
{
  const auto cell = grid.cellAt(point);
  if (!cell)
  {
    return std::nullopt;
  }
  if (const auto label = labels[grid.index(*cell)]; label >= 0)
  {
    return size_t(label);
  }
  if (labels[grid.index(*cell)] != Solid)
  {
    return std::nullopt;
  }

  auto best = std::optional<size_t>{};
  auto bestDistance = std::numeric_limits<double>::max();
  const auto reach = int(searchCells);
  for (int dz = -reach; dz <= reach; ++dz)
  {
    for (int dy = -reach; dy <= reach; ++dy)
    {
      for (int dx = -reach; dx <= reach; ++dx)
      {
        const auto candidate = neighbour(grid, *cell, {dx, dy, dz});
        if (!candidate)
        {
          continue;
        }
        const auto label = labels[grid.index(*candidate)];
        const auto distance = vm::squared_distance(grid.cellCenter(*candidate), point);
        if (label >= 0 && distance < bestDistance)
        {
          best = size_t(label);
          bestDistance = distance;
        }
      }
    }
  }
  return best;
}

std::optional<size_t> SpaceMap::findSpace(const std::string_view id) const
{
  for (size_t i = 0; i < spaces.size(); ++i)
  {
    if (spaces[i].id == id)
    {
      return i;
    }
  }
  return std::nullopt;
}

namespace
{

/**
 * Grows the labels over unassigned empty cells by BFS, up to maxDepth steps, over the
 * 26 neighbours of a cell (the chessboard metric of the erosion) or its 6 face
 * neighbours.
 */
void growLabels(
  const VoxelGrid& grid,
  std::vector<int32_t>& labels,
  const size_t maxDepth,
  const bool diagonal)
{
  auto frontier = std::vector<size_t>{};
  for (size_t i = 0; i < labels.size(); ++i)
  {
    if (labels[i] >= 0 || labels[i] == SpaceMap::Void)
    {
      frontier.push_back(i);
    }
  }
  auto next = std::vector<size_t>{};
  for (size_t depth = 0; depth < maxDepth && !frontier.empty(); ++depth)
  {
    next.clear();
    for (const auto index : frontier)
    {
      const auto label = labels[index];
      const auto visit = [&](const size_t neighbourIndex) {
        if (labels[neighbourIndex] == Unassigned)
        {
          labels[neighbourIndex] = label;
          next.push_back(neighbourIndex);
        }
      };
      if (diagonal)
      {
        forEachBlockNeighbour(grid, index, false, visit);
      }
      else
      {
        forEachFaceNeighbour(grid, index, visit);
      }
    }
    std::swap(frontier, next);
  }
}

/** Connected components (face neighbours) of the cells with the given label. */
std::vector<std::vector<size_t>> components(
  const VoxelGrid& grid, const std::vector<int32_t>& labels, const int32_t label)
{
  auto result = std::vector<std::vector<size_t>>{};
  auto visited = std::vector<uint8_t>(labels.size(), 0);
  for (size_t start = 0; start < labels.size(); ++start)
  {
    if (labels[start] != label || visited[start])
    {
      continue;
    }
    auto component = std::vector<size_t>{start};
    visited[start] = 1;
    for (size_t head = 0; head < component.size(); ++head)
    {
      forEachFaceNeighbour(grid, component[head], [&](const size_t next) {
        if (labels[next] == label && !visited[next])
        {
          visited[next] = 1;
          component.push_back(next);
        }
      });
    }
    result.push_back(std::move(component));
  }
  return result;
}

struct OpeningFace
{
  size_t cell;
  size_t axis;
  /** Whether the lower cell (cell) belongs to the first label of the pair. */
  bool firstBelow;
};

} // namespace

Result<SpaceMap, ToolError> analyzeSpaces(
  const mdl::Map& map, const SpaceOptions& options)
{
  const auto cellSize = options.cellSize > 0.0 ? options.cellSize : defaultCellSize(map);
  const auto spaceSolid = [](const mdl::BrushNode&, const BrushRole& role) {
    return role.spaceSolid;
  };
  auto region = options.region;
  if (!region)
  {
    region = brushBounds(map, spaceSolid);
  }
  if (!region)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The map has no solid brushes that could enclose spaces.");
  }

  auto gridResult = makeGrid(*region, cellSize, 1, options.maxCells);
  if (gridResult.is_error())
  {
    return errorOf(gridResult);
  }

  auto result = SpaceMap{};
  result.grid = std::move(gridResult).value();
  result.openingSize = options.openingSize;
  auto& grid = result.grid;
  rasterize(grid, map, spaceSolid);

  const auto count = grid.size();
  const auto erosion =
    size_t(std::max(1.0, std::floor(options.openingSize / cellSize / 2.0)));

  // 1. chessboard distance to the solid cells, up to the erosion distance
  auto distance = std::vector<uint8_t>(count, 255);
  auto frontier = std::vector<size_t>{};
  for (size_t i = 0; i < count; ++i)
  {
    if (grid.solid[i])
    {
      distance[i] = 0;
      frontier.push_back(i);
    }
  }
  {
    auto next = std::vector<size_t>{};
    for (size_t depth = 1; depth <= erosion && !frontier.empty(); ++depth)
    {
      next.clear();
      for (const auto index : frontier)
      {
        forEachBlockNeighbour(grid, index, false, [&](const size_t neighbourIndex) {
          if (distance[neighbourIndex] == 255)
          {
            distance[neighbourIndex] = uint8_t(depth);
            next.push_back(neighbourIndex);
          }
        });
      }
      std::swap(frontier, next);
    }
  }

  // 2. the cells connected to the border
  result.outside.assign(count, 0);
  {
    auto queue = std::vector<size_t>{};
    forEachBorderCell(grid, [&](const size_t index) {
      if (!grid.solid[index] && !result.outside[index])
      {
        result.outside[index] = 1;
        queue.push_back(index);
      }
    });
    for (size_t head = 0; head < queue.size(); ++head)
    {
      forEachFaceNeighbour(grid, queue[head], [&](const size_t next) {
        if (!grid.solid[next] && !result.outside[next])
        {
          result.outside[next] = 1;
          queue.push_back(next);
        }
      });
    }
  }

  // 3. room cores: the connected parts of the eroded empty cells
  const auto& outside = result.outside;
  auto& labels = result.labels;
  labels.assign(count, Unassigned);
  constexpr auto Core = int32_t(-5);
  for (size_t i = 0; i < count; ++i)
  {
    if (grid.solid[i])
    {
      labels[i] = SpaceMap::Solid;
    }
    else if (distance[i] == 255)
    {
      labels[i] = Core;
    }
  }
  // the border cells (outside the analyzed region) are the void; cores touching them are
  // spaces that are open to the outside
  forEachBorderCell(grid, [&](const size_t index) {
    if (labels[index] != SpaceMap::Solid)
    {
      labels[index] = SpaceMap::Void;
    }
  });
  auto nextLabel = int32_t(0);
  for (const auto& component : components(grid, labels, Core))
  {
    // a core connected to the outside is a leaking room if it is mostly enclosed (most
    // of its cells see solid cells in at least 5 of the 6 axis directions), otherwise it
    // is an outdoor area that belongs to the void
    auto label = SpaceMap::Void;
    if (!outside[component.front()])
    {
      label = nextLabel++;
    }
    else
    {
      const auto stride = std::max(size_t(1), component.size() / 200);
      auto samples = size_t(0);
      auto enclosed = size_t(0);
      for (size_t i = 0; i < component.size(); i += stride)
      {
        ++samples;
        if (enclosure(grid, grid.cellOf(component[i])) >= 5)
        {
          ++enclosed;
        }
      }
      if (enclosed * 2 > samples)
      {
        label = nextLabel++;
      }
    }
    for (const auto index : component)
    {
      labels[index] = label;
    }
  }

  struct Accumulator
  {
    CellIndex lo = {
      std::numeric_limits<size_t>::max(),
      std::numeric_limits<size_t>::max(),
      std::numeric_limits<size_t>::max()};
    CellIndex hi = {0, 0, 0};
    size_t cells = 0;
    bool sealed = true;
  };
  const auto accumulate = [&](const size_t labelCount) {
    auto perLabel = std::vector<Accumulator>(labelCount);
    for (size_t i = 0; i < count; ++i)
    {
      if (labels[i] < 0)
      {
        continue;
      }
      auto& accumulator = perLabel[size_t(labels[i])];
      const auto cell = grid.cellOf(i);
      for (size_t a = 0; a < 3; ++a)
      {
        accumulator.lo[a] = std::min(accumulator.lo[a], cell[a]);
        accumulator.hi[a] = std::max(accumulator.hi[a], cell[a]);
      }
      ++accumulator.cells;
      if (outside[i])
      {
        accumulator.sealed = false;
      }
    }
    return perLabel;
  };

  // 4. grow the cores back up to the erosion distance; this reconstructs the rooms up to
  // their walls, which give the inner bounds; one more step enters the openings
  growLabels(grid, labels, erosion, true);
  const auto innerBounds = accumulate(size_t(nextLabel));
  growLabels(grid, labels, 1, true);

  // 5. long narrow passages that no core reached become spaces of their own
  const auto minPassageLength = 2 * (erosion + 1);
  for (const auto& component : components(grid, labels, Unassigned))
  {
    auto lo = grid.cellOf(component.front());
    auto hi = lo;
    for (const auto index : component)
    {
      const auto cell = grid.cellOf(index);
      for (size_t i = 0; i < 3; ++i)
      {
        lo[i] = std::min(lo[i], cell[i]);
        hi[i] = std::max(hi[i], cell[i]);
      }
    }
    const auto length = std::max(hi[0] - lo[0], hi[1] - lo[1]) + 1;
    const auto isOutside =
      std::ranges::any_of(component, [&](const auto index) { return outside[index]; });
    if (isOutside)
    {
      // narrow outside areas, e.g. between the outer walls of rooms
      for (const auto index : component)
      {
        labels[index] = SpaceMap::Void;
      }
    }
    else if (length >= minPassageLength)
    {
      const auto label = nextLabel++;
      for (const auto index : component)
      {
        labels[index] = label;
      }
    }
  }

  // 6. grow everything
  growLabels(grid, labels, std::numeric_limits<size_t>::max(), false);

  // 7. isolated pockets without a core
  const auto minPocketCells = size_t(16);
  for (const auto& component : components(grid, labels, Unassigned))
  {
    const auto label =
      component.size() >= minPocketCells ? nextLabel++ : SpaceMap::Pocket;
    for (const auto index : component)
    {
      labels[index] = label;
    }
  }

  // spaces: bounds, sizes, sealed
  auto accumulators = accumulate(size_t(nextLabel));
  for (size_t label = 0; label < innerBounds.size(); ++label)
  {
    if (innerBounds[label].cells > 0)
    {
      accumulators[label].lo = innerBounds[label].lo;
      accumulators[label].hi = innerBounds[label].hi;
    }
  }

  auto spaces = std::vector<SpaceGeometry>{};
  for (size_t label = 0; label < accumulators.size(); ++label)
  {
    const auto& accumulator = accumulators[label];
    auto space = SpaceGeometry{};
    space.hasCore = label < innerBounds.size() && innerBounds[label].cells > 0;
    if (accumulator.cells > 0)
    {
      space.bounds =
        vm::merge(grid.cellBounds(accumulator.lo), grid.cellBounds(accumulator.hi));
    }
    space.cellCount = accumulator.cells;
    space.volume = double(accumulator.cells) * cellSize * cellSize * cellSize;
    space.sealed = accumulator.sealed;
    spaces.push_back(std::move(space));
  }

  // order the spaces by position and assign ids
  auto order = std::vector<size_t>(spaces.size());
  for (size_t i = 0; i < order.size(); ++i)
  {
    order[i] = i;
  }
  std::ranges::sort(order, [&](const auto lhs, const auto rhs) {
    const auto& l = spaces[lhs].bounds.min;
    const auto& r = spaces[rhs].bounds.min;
    return std::tuple{l[2], l[1], l[0]} < std::tuple{r[2], r[1], r[0]};
  });
  auto newIndex = std::vector<int32_t>(spaces.size());
  for (size_t i = 0; i < order.size(); ++i)
  {
    newIndex[order[i]] = int32_t(i);
    result.spaces.push_back(std::move(spaces[order[i]]));
  }
  for (auto& label : labels)
  {
    if (label >= 0)
    {
      label = newIndex[size_t(label)];
    }
  }
  auto usedIds = std::unordered_map<std::string, size_t>{};
  for (auto& space : result.spaces)
  {
    space.id = spaceHash(grid, space.bounds);
    if (const auto n = ++usedIds[space.id]; n > 1)
    {
      space.id += fmt::format("-{}", n);
    }
  }

  // openings: faces between cells of different labels
  auto faces = std::map<std::pair<int32_t, int32_t>, std::vector<OpeningFace>>{};
  for (size_t i = 0; i < count; ++i)
  {
    const auto la = labels[i];
    if (la < 0 && la != SpaceMap::Void)
    {
      continue;
    }
    const auto cell = grid.cellOf(i);
    for (size_t axis = 0; axis < 3; ++axis)
    {
      if (cell[axis] + 1 >= grid.dims[axis])
      {
        continue;
      }
      auto other = cell;
      ++other[axis];
      const auto lb = labels[grid.index(other)];
      if (lb == la || (lb < 0 && lb != SpaceMap::Void))
      {
        continue;
      }
      if (la < 0 && lb < 0)
      {
        continue;
      }
      // spaces first, the void (negative) last
      const auto first = la >= 0 && (lb < 0 || la < lb) ? la : lb;
      const auto second = first == la ? lb : la;
      faces[{first, second}].push_back(OpeningFace{i, axis, first == la});
    }
  }

  for (const auto& [pair, pairFaces] : faces)
  {
    auto lookup = std::unordered_map<size_t, size_t>{};
    for (size_t f = 0; f < pairFaces.size(); ++f)
    {
      lookup.emplace(pairFaces[f].cell * 3 + pairFaces[f].axis, f);
    }
    auto visited = std::vector<uint8_t>(pairFaces.size(), 0);
    for (size_t start = 0; start < pairFaces.size(); ++start)
    {
      if (visited[start])
      {
        continue;
      }
      auto cluster = std::vector<size_t>{start};
      visited[start] = 1;
      for (size_t head = 0; head < cluster.size(); ++head)
      {
        forEachBlockNeighbour(
          grid, pairFaces[cluster[head]].cell, true, [&](const size_t neighbourIndex) {
            for (size_t axis = 0; axis < 3; ++axis)
            {
              const auto it = lookup.find(neighbourIndex * 3 + axis);
              if (it != lookup.end() && !visited[it->second])
              {
                visited[it->second] = 1;
                cluster.push_back(it->second);
              }
            }
          });
      }

      auto opening = OpeningGeometry{};
      opening.spaceA = size_t(pair.first);
      if (pair.second >= 0)
      {
        opening.spaceB = size_t(pair.second);
      }
      auto axisCounts = std::array<size_t, 3>{0, 0, 0};
      auto signSum = std::array<int64_t, 3>{0, 0, 0};
      auto bounds = std::optional<vm::bbox3d>{};
      for (const auto f : cluster)
      {
        const auto& face = pairFaces[f];
        auto cellBounds = grid.cellBounds(grid.cellOf(face.cell));
        cellBounds.min[face.axis] = cellBounds.max[face.axis];
        bounds = bounds ? vm::merge(*bounds, cellBounds) : cellBounds;
        ++axisCounts[face.axis];
        signSum[face.axis] += face.firstBelow ? 1 : -1;
      }
      opening.axis =
        size_t(std::distance(axisCounts.begin(), std::ranges::max_element(axisCounts)));
      opening.normal = vm::vec3d{0, 0, 0};
      opening.normal[opening.axis] = signSum[opening.axis] >= 0 ? 1.0 : -1.0;
      opening.bounds = *bounds;
      opening.faceCount = cluster.size();

      // on the floor: a solid cell below one of the lowest faces
      if (opening.axis != 2)
      {
        for (const auto f : cluster)
        {
          const auto& face = pairFaces[f];
          const auto cell = grid.cellOf(face.cell);
          if (
            grid.cellBounds(cell).min.z() > opening.bounds.min.z() + 0.5 || cell[2] == 0)
          {
            continue;
          }
          auto other = cell;
          ++other[face.axis];
          auto below = cell;
          --below[2];
          auto otherBelow = other;
          --otherBelow[2];
          if (grid.solid[grid.index(below)] || grid.solid[grid.index(otherBelow)])
          {
            opening.onFloor = true;
            break;
          }
        }
      }

      const auto index = result.openings.size();
      result.spaces[opening.spaceA].openings.push_back(index);
      if (opening.spaceB)
      {
        result.spaces[*opening.spaceB].openings.push_back(index);
        auto& neighboursA = result.spaces[opening.spaceA].neighbours;
        auto& neighboursB = result.spaces[*opening.spaceB].neighbours;
        if (std::ranges::find(neighboursA, *opening.spaceB) == neighboursA.end())
        {
          neighboursA.push_back(*opening.spaceB);
          neighboursB.push_back(opening.spaceA);
        }
      }
      result.openings.push_back(std::move(opening));
    }
  }
  for (auto& space : result.spaces)
  {
    std::ranges::sort(space.neighbours);
  }

  return result;
}

SpaceDetails describeSpace(mdl::Map& map, const SpaceMap& spaces, const size_t spaceIndex)
{
  const auto& grid = spaces.grid;
  const auto& space = spaces.spaces[spaceIndex];
  const auto label = int32_t(spaceIndex);
  auto roles = RoleCache{};
  const auto accept = acceptRole(roles, &BrushRole::spaceSolid, true);

  auto details = SpaceDetails{};
  auto floors = std::vector<double>{};
  auto ceilings = std::vector<double>{};
  const auto lo = grid.cellAt(space.bounds.min + vm::vec3d{0.5, 0.5, 0.5});
  const auto hi = grid.cellAt(space.bounds.max - vm::vec3d{0.5, 0.5, 0.5});
  if (lo && hi)
  {
    for (auto y = (*lo)[1]; y <= (*hi)[1]; ++y)
    {
      for (auto x = (*lo)[0]; x <= (*hi)[0]; ++x)
      {
        auto lowest = std::optional<size_t>{};
        auto highest = std::optional<size_t>{};
        for (auto z = (*lo)[2]; z <= (*hi)[2]; ++z)
        {
          if (spaces.labels[grid.index({x, y, z})] == label)
          {
            lowest = lowest ? lowest : z;
            highest = z;
          }
        }
        if (!lowest)
        {
          continue;
        }
        if (*lowest > 0 && grid.solid[grid.index({x, y, *lowest - 1})])
        {
          const auto center = grid.cellCenter({x, y, *lowest});
          if (
            const auto d = firstHitDistance(
              map, vm::ray3d{center, {0, 0, -1}}, accept, grid.cellSize * 2.0))
          {
            floors.push_back(center.z() - *d);
          }
        }
        if (*highest + 1 < grid.dims[2] && grid.solid[grid.index({x, y, *highest + 1})])
        {
          const auto center = grid.cellCenter({x, y, *highest});
          if (
            const auto d = firstHitDistance(
              map, vm::ray3d{center, {0, 0, 1}}, accept, grid.cellSize * 2.0))
          {
            ceilings.push_back(center.z() + *d);
          }
        }
      }
    }
  }

  const auto stats = [](const std::vector<double>& values) -> std::optional<HeightStats> {
    if (values.empty())
    {
      return std::nullopt;
    }
    auto histogram = std::map<int64_t, size_t>{};
    for (const auto value : values)
    {
      ++histogram[std::llround(value)];
    }
    const auto typical = std::ranges::max_element(
      histogram, [](const auto& l, const auto& r) { return l.second < r.second; });
    return HeightStats{
      *std::ranges::min_element(values),
      *std::ranges::max_element(values),
      double(typical->first)};
  };
  details.floor = stats(floors);
  details.ceiling = stats(ceilings);
  details.floorArea = double(floors.size()) * grid.cellSize * grid.cellSize;

  // contents
  const auto insideSpace = [&](const vm::bbox3d& bounds) {
    if (!boxesOverlap(bounds.expand(1.0), space.bounds))
    {
      return false;
    }
    const auto center = bounds.center();
    const auto centerLabel = spaces.labelAt(center);
    if (centerLabel == label)
    {
      return true;
    }
    return centerLabel == SpaceMap::Solid
           && spaces.labelAt({center.x(), center.y(), bounds.max.z() + 1.0}) == label;
  };
  auto layers = std::set<std::string>{};
  auto groups = std::set<std::string>{};
  const auto addLayer = [&](const mdl::Node& node) {
    if (const auto* layer = dynamic_cast<const mdl::LayerNode*>(parentLayer(node)))
    {
      layers.insert(layer->layer().name());
    }
  };
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    for (const auto* child : node.children())
    {
      if (omittedLayer(*child))
      {
        continue;
      }
      if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(child))
      {
        if (insideSpace(child->logicalBounds()))
        {
          if (!inGroup(*child))
          {
            details.contents.push_back(child);
          }
          groups.insert(groupNode->group().name());
          addLayer(*child);
        }
        self(self, *child);
      }
      else if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(child))
      {
        const auto inside =
          entityNode->hasChildren()
            ? insideSpace(child->logicalBounds())
            : spaces.spaceAt(entityNode->entity().origin()) == spaceIndex;
        if (inside)
        {
          details.contents.push_back(child);
          addLayer(*child);
        }
      }
      else if (dynamic_cast<const mdl::PatchNode*>(child))
      {
        if (insideSpace(child->logicalBounds()) && !inGroup(*child))
        {
          details.contents.push_back(child);
          addLayer(*child);
        }
      }
      else if (!dynamic_cast<const mdl::BrushNode*>(child))
      {
        self(self, *child);
      }
    }
  };
  visit(visit, map.worldNode());
  details.layers = {layers.begin(), layers.end()};
  details.groups = {groups.begin(), groups.end()};
  return details;
}

OpeningDetails describeOpening(
  mdl::Map& map, const SpaceMap& spaces, const size_t openingIndex)
{
  const auto& grid = spaces.grid;
  const auto& opening = spaces.openings[openingIndex];
  auto roles = RoleCache{};
  const auto accept = acceptRole(roles, &BrushRole::spaceSolid, true);

  auto details = OpeningDetails{};
  details.bounds = opening.bounds;
  details.normal = opening.normal;
  details.kind = opening.axis == 2 ? "hole" : opening.onFloor ? "doorway" : "window";

  if (opening.axis == 2)
  {
    const auto size = opening.bounds.size();
    details.width = size.x();
    details.height = size.y();
    details.bottom = opening.bounds.min.z();
  }
  else
  {
    // measure the narrowest cross section with rays on both sides of the faces
    const auto widthAxis = opening.axis == 0 ? size_t(1) : size_t(0);
    auto bounds = opening.bounds;
    auto bestWidth = std::numeric_limits<double>::max();
    const auto reach = grid.cellSize * 2.0;
    for (const auto offset : {-0.5, 0.5})
    {
      auto center = opening.bounds.center();
      center[opening.axis] += offset * grid.cellSize;
      const auto extent = opening.bounds.size();
      const auto ray = [&](const size_t axis, const double sign, const double size) {
        auto direction = vm::vec3d{0, 0, 0};
        direction[axis] = sign;
        return firstHitDistance(
          map, vm::ray3d{center, direction}, accept, size / 2 + reach);
      };
      const auto left = ray(widthAxis, -1.0, extent[widthAxis]);
      const auto right = ray(widthAxis, 1.0, extent[widthAxis]);
      const auto down = ray(2, -1.0, extent.z());
      const auto up = ray(2, 1.0, extent.z());
      if (!left || !right || !down || !up)
      {
        continue;
      }
      const auto width = *left + *right;
      if (width < bestWidth)
      {
        bestWidth = width;
        bounds.min[widthAxis] = center[widthAxis] - *left;
        bounds.max[widthAxis] = center[widthAxis] + *right;
        bounds.min[2] = center.z() - *down;
        bounds.max[2] = center.z() + *up;
      }
    }
    details.bounds = bounds;
    const auto size = bounds.size();
    details.width = size[widthAxis];
    details.height = size.z();
    details.bottom = bounds.min.z();
  }
  details.center = details.bounds.center();

  // doors in the opening: door brushes that touch the opening's box, which is widened by
  // one cell along the axis to reach doors inside the wall; the octree returns every
  // node in the cells it visits, so the bounds are checked
  auto search = details.bounds.expand(1.0);
  search.min[opening.axis] -= grid.cellSize;
  search.max[opening.axis] += grid.cellSize;
  auto doors = std::vector<const mdl::EntityNode*>{};
  for (const auto* node : map.worldNode().nodeTree().find_intersectors(search))
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node))
    {
      const auto* entityNode = owningBrushEntity(*brushNode);
      if (
        entityNode && entityNode->entity().classname().starts_with("func_door")
        && brushNode->logicalBounds().intersects(search)
        && std::ranges::find(doors, entityNode) == doors.end())
      {
        doors.push_back(entityNode);
      }
    }
  }
  details.doors = std::move(doors);
  return details;
}

// Free spots

std::optional<Placement> placementFromString(const std::string_view name)
{
  if (name == "floor")
  {
    return Placement::Floor;
  }
  if (name == "wall")
  {
    return Placement::Wall;
  }
  if (name == "ceiling")
  {
    return Placement::Ceiling;
  }
  if (name == "any")
  {
    return Placement::Any;
  }
  return std::nullopt;
}

namespace
{

class SpotChecker
{
private:
  mdl::Map& m_map;
  const SpaceMap* m_spaces;
  const FreeSpotOptions& m_options;
  RoleCache m_roles;

  /**
   * Walls are space-solid world and func_group brushes. A brush in a group is a wall if
   * its innermost group encloses the box's center (e.g. a room built as a group); a
   * group that does not enclose it is an object (e.g. furniture).
   */
  bool isWall(const mdl::BrushNode& brushNode, const vm::bbox3d& box)
  {
    if (!m_roles(brushNode).spaceSolid)
    {
      return false;
    }
    if (const auto* entityNode = owningBrushEntity(brushNode);
        entityNode && entityNode->entity().classname() != "func_group")
    {
      return false;
    }
    const auto* group = mdl::findContainingGroup(&brushNode);
    return !group || group->logicalBounds().contains(box.center());
  }

  std::optional<vm::bbox3d> entityBounds(const mdl::EntityNode& entityNode) const
  {
    auto bounds = entityNode.physicalBounds();
    if (m_options.entityBounds)
    {
      if (const auto extra = m_options.entityBounds(entityNode))
      {
        bounds = vm::merge(bounds, *extra);
      }
    }
    return bounds;
  }

public:
  SpotChecker(mdl::Map& map, const SpaceMap* spaces, const FreeSpotOptions& options)
    : m_map{map}
    , m_spaces{spaces}
    , m_options{options}
  {
  }

  RoleCache& roles() { return m_roles; }

  /**
   * Whether nothing overlaps the box, and walls and objects keep their distance within
   * the given margin boxes.
   */
  bool isFree(
    const vm::bbox3d& box, const vm::bbox3d& wallMargin, const vm::bbox3d& objectMargin)
  {
    const auto shrunk = shrink(box, 0.01);
    const auto search = vm::merge(vm::merge(box, wallMargin), objectMargin);
    for (const auto* node : m_map.worldNode().nodeTree().find_intersectors(search))
    {
      if (inOmittedLayer(*node))
      {
        continue;
      }
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node))
      {
        const auto& role = m_roles(*brushNode);
        if (!role.blocksObjects)
        {
          continue;
        }
        const auto& brush = brushNode->brush();
        if (intersectsInterior(brush, shrunk))
        {
          return false;
        }
        const auto& margin = isWall(*brushNode, box) ? wallMargin : objectMargin;
        if (margin != box && intersectsInterior(brush, shrink(margin, 0.01)))
        {
          return false;
        }
      }
      else if (isPointEntity(*node))
      {
        const auto bounds = entityBounds(static_cast<const mdl::EntityNode&>(*node));
        if (
          bounds
          && (boxesOverlap(*bounds, shrunk) || boxesOverlap(*bounds, objectMargin)))
        {
          return false;
        }
      }
      else if (dynamic_cast<const mdl::PatchNode*>(node))
      {
        if (boxesOverlap(node->logicalBounds(), shrunk))
        {
          return false;
        }
      }
    }
    return true;
  }

  /** Whether the box is in the requested space (or in any space). */
  std::optional<size_t> spaceOf(const vm::bbox3d& box, bool& accepted) const
  {
    accepted = true;
    if (!m_spaces)
    {
      return std::nullopt;
    }
    const auto space = m_spaces->spaceAt(box.center(), 1);
    if (m_options.space)
    {
      accepted = space == m_options.space;
    }
    else if (!m_options.includeOutside)
    {
      accepted = space.has_value();
    }
    return space;
  }

  /** The number of the five rays (center, inset corners) that hit within 1 unit. */
  size_t support(const vm::bbox3d& box, const double direction)
  {
    const auto accept = acceptRole(m_roles, &BrushRole::blocksObjects, true);
    const auto inset = std::min(1.0, std::min(box.size().x(), box.size().y()) / 4.0);
    const auto z = direction < 0.0 ? box.min.z() + 1.0 : box.max.z() - 1.0;
    const auto points = std::array<vm::vec2d, 5>{{
      {box.center().x(), box.center().y()},
      {box.min.x() + inset, box.min.y() + inset},
      {box.max.x() - inset, box.min.y() + inset},
      {box.min.x() + inset, box.max.y() - inset},
      {box.max.x() - inset, box.max.y() - inset},
    }};
    auto count = size_t(0);
    for (const auto& point : points)
    {
      if (firstHitDistance(
            m_map, vm::ray3d{{point.x(), point.y(), z}, {0, 0, direction}}, accept, 2.0))
      {
        ++count;
      }
    }
    return count;
  }

  std::array<std::optional<double>, 6> clearance(const vm::bbox3d& box)
  {
    const auto accept = [&](const mdl::Node& node) {
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
      {
        return m_roles(*brushNode).blocksObjects;
      }
      return dynamic_cast<const mdl::PatchNode*>(&node) || isPointEntity(node);
    };
    auto result = std::array<std::optional<double>, 6>{};
    const auto center = box.center();
    for (size_t d = 0; d < 6; ++d)
    {
      const auto axis = d / 2;
      auto direction = vm::vec3d{0, 0, 0};
      direction[axis] = d % 2 == 0 ? -1.0 : 1.0;
      auto origin = center;
      origin[axis] = d % 2 == 0 ? box.min[axis] : box.max[axis];
      if (
        const auto distance =
          firstHitDistance(m_map, vm::ray3d{origin, direction}, accept, 1024.0))
      {
        result[d] = std::max(0.0, *distance);
      }
    }
    return result;
  }

  std::optional<double> floorBelow(const vm::vec3d& point)
  {
    const auto accept = acceptRole(m_roles, &BrushRole::blocksObjects, true);
    const auto distance =
      firstHitDistance(m_map, vm::ray3d{point, {0, 0, -1}}, accept, 4096.0);
    return distance ? std::optional{point.z() - *distance} : std::nullopt;
  }
};

vm::bbox3d marginBox(
  const vm::bbox3d& box, const double margin, const std::array<bool, 6>& sides)
{
  auto result = box;
  for (size_t d = 0; d < 6; ++d)
  {
    if (sides[d])
    {
      if (d % 2 == 0)
      {
        result.min[d / 2] -= margin;
      }
      else
      {
        result.max[d / 2] += margin;
      }
    }
  }
  return result;
}

std::vector<double> lattice(const double from, const double to, const double step)
{
  auto result = std::vector<double>{};
  for (auto value = std::ceil(from / step - 1e-9) * step; value <= to + 1e-9;
       value += step)
  {
    result.push_back(value);
  }
  return result;
}

/** Whether the point lies in the convex polygon of the face (on its plane). */
bool pointInFace(const mdl::BrushFace& face, const vm::vec3d& point)
{
  const auto vertices = face.vertexPositions();
  const auto& normal = face.normal();
  for (size_t i = 0; i < vertices.size(); ++i)
  {
    const auto& a = vertices[i];
    const auto& b = vertices[(i + 1) % vertices.size()];
    if (vm::dot(vm::cross(b - a, point - a), normal) < -1e-6)
    {
      return false;
    }
  }
  return true;
}

} // namespace

Result<FreeSpotResult, ToolError> findFreeSpots(
  mdl::Map& map, const SpaceMap* spaces, const FreeSpotOptions& options)
{
  if (!(options.size.x() > 0.0 && options.size.y() > 0.0 && options.size.z() > 0.0))
  {
    return makeError(
      ErrorCode::InvalidArgument, "The size must be positive on every axis.");
  }
  if (options.space && (!spaces || *options.space >= spaces->spaces.size()))
  {
    return makeError(ErrorCode::InvalidArgument, "Unknown space.");
  }

  auto region = std::optional<vm::bbox3d>{};
  if (options.space)
  {
    region = spaces->spaces[*options.space].bounds;
  }
  if (options.region)
  {
    if (region)
    {
      const auto min = vm::max(region->min, options.region->min);
      const auto max = vm::min(region->max, options.region->max);
      if (min.x() >= max.x() || min.y() >= max.y() || min.z() >= max.z())
      {
        return FreeSpotResult{};
      }
      region = vm::bbox3d{min, max};
    }
    else
    {
      region = options.region;
    }
  }
  if (!region)
  {
    region =
      spaces
        ? std::optional{spaces->grid.bounds()}
        : brushBounds(map, [](const auto&, const auto& role) { return role.spaceSolid; });
  }
  if (!region)
  {
    return makeError(ErrorCode::InvalidArgument, "The map is empty; pass a region.");
  }

  auto checker = SpotChecker{map, spaces, options};
  auto result = FreeSpotResult{};
  auto step = options.step > 0.0 ? options.step : 8.0;
  const auto size = options.size;
  const auto extent = region->size();

  // estimate the candidate count and coarsen the lattice if needed
  const auto estimate = [&](const double s) {
    switch (options.placement)
    {
    case Placement::Any:
      return (extent.x() / s + 1) * (extent.y() / s + 1) * (extent.z() / s + 1);
    case Placement::Wall:
      return 2.0 * (extent.x() + extent.y()) / s * (extent.z() / s + 1);
    case Placement::Floor:
    case Placement::Ceiling:
      return (extent.x() / s + 1) * (extent.y() / s + 1);
    }
    return 0.0;
  };
  while (estimate(step) > double(options.maxCandidates))
  {
    step *= 2.0;
    result.coarsened = true;
  }
  result.step = step;

  auto candidates = std::vector<FreeSpot>{};

  const auto check =
    [&](
      const vm::bbox3d& box,
      const std::array<bool, 6>& wallSides,
      const std::array<bool, 6>& objectSides) -> std::optional<FreeSpot> {
    if (
      box.min.x() < region->min.x() - 1e-6 || box.min.y() < region->min.y() - 1e-6
      || box.min.z() < region->min.z() - 1e-6 || box.max.x() > region->max.x() + 1e-6
      || box.max.y() > region->max.y() + 1e-6 || box.max.z() > region->max.z() + 1e-6)
    {
      return std::nullopt;
    }
    auto accepted = true;
    const auto space = checker.spaceOf(box, accepted);
    if (!accepted)
    {
      return std::nullopt;
    }
    const auto wallMargin =
      options.wallDistance > 0.0 ? marginBox(box, options.wallDistance, wallSides) : box;
    const auto objectMargin = options.objectDistance > 0.0
                                ? marginBox(box, options.objectDistance, objectSides)
                                : box;
    if (!checker.isFree(box, wallMargin, objectMargin))
    {
      return std::nullopt;
    }
    auto spot = FreeSpot{};
    spot.box = box;
    spot.origin = {box.center().x(), box.center().y(), box.min.z()};
    spot.space = space;
    return spot;
  };

  const auto requiredSupport =
    size_t(std::ceil(std::clamp(options.support, 0.0, 1.0) * 5.0 - 1e-9));
  const auto accept = acceptRole(checker.roles(), &BrushRole::blocksObjects, true);

  if (options.placement == Placement::Floor || options.placement == Placement::Ceiling)
  {
    const auto down = options.placement == Placement::Floor;
    for (const auto x : lattice(region->min.x(), region->max.x() - size.x(), step))
    {
      for (const auto y : lattice(region->min.y(), region->max.y() - size.y(), step))
      {
        const auto cx = x + size.x() / 2.0;
        const auto cy = y + size.y() / 2.0;
        const auto start = vm::vec3d{cx, cy, down ? region->max.z() : region->min.z()};
        const auto hits =
          castRay(map, vm::ray3d{start, {0, 0, down ? -1.0 : 1.0}}, accept, extent.z());
        for (const auto& hit : hits)
        {
          const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(hit.node);
          if (brushNode && hit.faceIndex)
          {
            const auto nz = brushNode->brush().face(*hit.faceIndex).normal().z();
            if (down ? nz < 0.7 : nz > -0.7)
            {
              continue;
            }
          }
          const auto z = hit.point.z();
          const auto box =
            down ? vm::bbox3d{{x, y, z}, {x + size.x(), y + size.y(), z + size.z()}}
                 : vm::bbox3d{{x, y, z - size.z()}, {x + size.x(), y + size.y(), z}};
          ++result.candidates;
          auto spot = check(
            box,
            down ? std::array<bool, 6>{true, true, true, true, false, true}
                 : std::array<bool, 6>{true, true, true, true, true, false},
            down ? std::array<bool, 6>{true, true, true, true, false, true}
                 : std::array<bool, 6>{true, true, true, true, true, false});
          if (!spot || checker.support(box, down ? -1.0 : 1.0) < requiredSupport)
          {
            continue;
          }
          spot->floor = down ? std::optional{z} : checker.floorBelow(box.center());
          candidates.push_back(std::move(*spot));
        }
      }
    }
  }
  else if (options.placement == Placement::Any)
  {
    for (const auto z : lattice(region->min.z(), region->max.z() - size.z(), step))
    {
      for (const auto y : lattice(region->min.y(), region->max.y() - size.y(), step))
      {
        for (const auto x : lattice(region->min.x(), region->max.x() - size.x(), step))
        {
          const auto box = vm::bbox3d{{x, y, z}, vm::vec3d{x, y, z} + size};
          ++result.candidates;
          const auto all = std::array<bool, 6>{true, true, true, true, true, true};
          if (auto spot = check(box, all, all))
          {
            spot->floor = checker.floorBelow(box.center());
            candidates.push_back(std::move(*spot));
          }
        }
      }
    }
  }
  else
  {
    // wall placement over the axis-aligned vertical faces of walls
    auto walls = std::vector<const mdl::BrushNode*>{};
    for (const auto* node :
         map.worldNode().nodeTree().find_intersectors(region->expand(1.0)))
    {
      const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
      if (brushNode && checker.roles()(*brushNode).spaceSolid && !inOmittedLayer(*node))
      {
        walls.push_back(brushNode);
      }
    }
    for (const auto* brushNode : walls)
    {
      const auto& brush = brushNode->brush();
      for (size_t faceIndex = 0; faceIndex < brush.faceCount(); ++faceIndex)
      {
        const auto& face = brush.face(faceIndex);
        const auto& normal = face.normal();
        auto axis = size_t(0);
        if (std::abs(normal.x()) > 0.999)
        {
          axis = 0;
        }
        else if (std::abs(normal.y()) > 0.999)
        {
          axis = 1;
        }
        else
        {
          continue;
        }
        const auto sign = normal[axis] > 0.0 ? 1.0 : -1.0;
        const auto widthAxis = 1 - axis;
        auto boxSize = size;
        const auto rotated = options.rotate && axis == 0;
        if (rotated)
        {
          std::swap(boxSize[0], boxSize[1]);
        }
        const auto vertices = face.vertexPositions();
        auto faceBounds = vm::bbox3d{vertices.front(), vertices.front()};
        for (const auto& vertex : vertices)
        {
          faceBounds = vm::merge(faceBounds, vm::bbox3d{vertex, vertex});
        }
        const auto plane = faceBounds.min[axis];
        if (plane < region->min[axis] - 1e-6 || plane > region->max[axis] + 1e-6)
        {
          continue;
        }

        const auto wallSides = [&]() {
          auto sides = std::array<bool, 6>{false, false, false, false, false, false};
          sides[widthAxis * 2] = true;
          sides[widthAxis * 2 + 1] = true;
          sides[axis * 2 + (sign > 0.0 ? 1 : 0)] = true;
          return sides;
        }();

        for (const auto u : lattice(
               std::max(faceBounds.min[widthAxis], region->min[widthAxis]),
               std::min(faceBounds.max[widthAxis], region->max[widthAxis])
                 - boxSize[widthAxis],
               step))
        {
          auto valid = std::vector<FreeSpot>{};
          const auto zFrom = std::max(faceBounds.min.z(), region->min.z());
          const auto zTo = std::min(faceBounds.max.z(), region->max.z()) - boxSize.z();
          const auto makeBox = [&](const double z) {
            auto min = vm::vec3d{0, 0, z};
            min[widthAxis] = u;
            min[axis] = sign > 0.0 ? plane : plane - boxSize[axis];
            return vm::bbox3d{min, min + boxSize};
          };
          const auto tryZ = [&](const double z) -> std::optional<FreeSpot> {
            const auto box = makeBox(z);
            ++result.candidates;
            // the back side must lie on the face
            auto corner = box.min;
            corner[axis] = plane;
            const auto u1 = box.max[widthAxis];
            auto corners = std::array<vm::vec3d, 4>{corner, corner, corner, corner};
            corners[1][widthAxis] = u1;
            corners[2][2] = box.max.z();
            corners[3][widthAxis] = u1;
            corners[3][2] = box.max.z();
            if (!std::ranges::all_of(
                  corners, [&](const auto& c) { return pointInFace(face, c); }))
            {
              return std::nullopt;
            }
            return check(box, wallSides, wallSides);
          };
          for (const auto z : lattice(zFrom, zTo, step))
          {
            if (auto spot = tryZ(z))
            {
              valid.push_back(std::move(*spot));
            }
          }
          if (valid.empty())
          {
            continue;
          }

          const auto lowest = valid.front().box.min.z();
          const auto highest = valid.back().box.min.z();
          auto front = valid.front().box.center();
          front[axis] = plane + sign * std::min(1.0, boxSize[axis] / 2.0);
          const auto floor = checker.floorBelow(front);

          auto chosen = std::optional<FreeSpot>{};
          if (options.heightAboveFloor && floor)
          {
            const auto preferred = *floor + *options.heightAboveFloor;
            chosen = tryZ(preferred);
            if (!chosen)
            {
              chosen =
                *std::ranges::min_element(valid, [&](const auto& l, const auto& r) {
                  return std::abs(l.box.min.z() - preferred)
                         < std::abs(r.box.min.z() - preferred);
                });
            }
          }
          else
          {
            chosen = valid.front();
          }
          chosen->wallBrush = brushNode;
          chosen->wallFace = faceIndex;
          chosen->wallNormal = normal;
          chosen->rotated = rotated;
          chosen->heightRange = std::array<double, 2>{lowest, highest};
          chosen->floor = floor;
          candidates.push_back(std::move(*chosen));
        }
      }
    }
  }

  // select the spots
  const auto overlapsChosen = [&](const FreeSpot& spot) {
    return std::ranges::any_of(result.spots, [&](const auto& chosen) {
      return boxesOverlap(chosen.box, shrink(spot.box, 0.01));
    });
  };
  if (options.sort == SpotSort::Near && options.near)
  {
    std::ranges::stable_sort(candidates, [&](const auto& lhs, const auto& rhs) {
      return vm::squared_distance(lhs.box.center(), *options.near)
             < vm::squared_distance(rhs.box.center(), *options.near);
    });
    for (auto& candidate : candidates)
    {
      if (result.spots.size() >= options.limit)
      {
        break;
      }
      if (!overlapsChosen(candidate))
      {
        result.spots.push_back(std::move(candidate));
      }
    }
  }
  else
  {
    auto minDistance =
      std::vector<double>(candidates.size(), std::numeric_limits<double>::max());
    auto used = std::vector<uint8_t>(candidates.size(), 0);
    const auto target = region->center();
    while (result.spots.size() < options.limit)
    {
      auto best = std::optional<size_t>{};
      for (size_t i = 0; i < candidates.size(); ++i)
      {
        if (used[i])
        {
          continue;
        }
        if (!best)
        {
          best = i;
          continue;
        }
        if (result.spots.empty())
        {
          if (
            vm::squared_distance(candidates[i].box.center(), target)
            < vm::squared_distance(candidates[*best].box.center(), target))
          {
            best = i;
          }
        }
        else if (minDistance[i] > minDistance[*best])
        {
          best = i;
        }
      }
      if (!best)
      {
        break;
      }
      used[*best] = 1;
      if (overlapsChosen(candidates[*best]))
      {
        continue;
      }
      result.spots.push_back(candidates[*best]);
      const auto center = candidates[*best].box.center();
      for (size_t i = 0; i < candidates.size(); ++i)
      {
        minDistance[i] = std::min(
          minDistance[i], vm::squared_distance(candidates[i].box.center(), center));
      }
    }
  }

  for (auto& spot : result.spots)
  {
    spot.clearance = checker.clearance(spot.box);
  }
  return result;
}

// Walkability

vm::vec3d WalkPlan::position(const WalkNode& node) const
{
  const auto x = node.column % columns;
  const auto y = node.column / columns;
  return {
    origin.x() + (double(x) + 0.5) * cellSize,
    origin.y() + (double(y) + 0.5) * cellSize,
    node.z};
}

Result<WalkPlan, ToolError> planWalk(mdl::Map& map, const WalkOptions& options)
{
  const auto player = playerSize(map.gameInfo().gameConfig);
  auto plan = WalkPlan{};
  plan.playerWidth = options.playerWidth > 0.0 ? options.playerWidth : player.width;
  plan.playerHeight = options.playerHeight > 0.0 ? options.playerHeight : player.height;
  plan.stepHeight = options.stepHeight;
  plan.jumpHeight = options.jumpHeight;
  plan.cellSize = options.cellSize > 0.0 ? options.cellSize : defaultCellSize(map);

  auto roles = RoleCache{};
  const auto blocks = [](const mdl::BrushNode&, const BrushRole& role) {
    return role.blocksPlayer;
  };
  auto region = options.region ? options.region : brushBounds(map, blocks);
  if (!region)
  {
    return makeError(ErrorCode::InvalidArgument, "The map has no solid brushes.");
  }
  plan.region = *region;

  auto gridResult = makeGrid(*region, plan.cellSize, 0, options.maxCells);
  if (gridResult.is_error())
  {
    return errorOf(gridResult);
  }
  auto grid = std::move(gridResult).value();
  plan.origin = {grid.origin.x(), grid.origin.y()};
  plan.columns = grid.dims[0];
  plan.rows = grid.dims[1];
  const auto columnCount = plan.columns * plan.rows;
  if (columnCount > options.maxColumns)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The plan would have {} x {} columns; at most {} columns are allowed.",
        plan.columns,
        plan.rows,
        options.maxColumns),
      "Pass a larger cellSize or a smaller region.");
  }
  rasterize(grid, map, blocks);

  const auto halfWidth = plan.playerWidth / 2.0;
  const auto blocking = [&](const mdl::Node& node) {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      return roles(*brushNode).blocksPlayer && !inOmittedLayer(node);
    }
    return false;
  };
  const auto isFree = [&](const vm::bbox3d& box) {
    for (const auto* node : map.worldNode().nodeTree().find_intersectors(box))
    {
      if (blocking(*node))
      {
        if (intersectsInterior(static_cast<const mdl::BrushNode*>(node)->brush(), box))
        {
          return false;
        }
      }
    }
    return true;
  };
  // the player's box without its lowest stepHeight units, which may overlap steps
  const auto fits = [&](const double x, const double y, const double z) {
    const auto lift = std::min(plan.stepHeight, plan.playerHeight / 2.0);
    return isFree(vm::bbox3d{
      {x - halfWidth + 0.01, y - halfWidth + 0.01, z + lift},
      {x + halfWidth - 0.01, y + halfWidth - 0.01, z + plan.playerHeight - 0.01}});
  };
  // and a thin column above the floor point, so that floors inside brushes do not count
  const auto standable = [&](const double x, const double y, const double z) {
    return fits(x, y, z)
           && isFree(vm::bbox3d{
             {x - 0.5, y - 0.5, z + 0.1},
             {x + 0.5, y + 0.5, z + plan.playerHeight - 0.01}});
  };

  // floors: upward facing surfaces hit by a ray down each column
  const auto acceptFloor = [&](const mdl::Node& node) {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      return roles(*brushNode).blocksPlayer && !inOmittedLayer(node);
    }
    return dynamic_cast<const mdl::PatchNode*>(&node) != nullptr;
  };
  plan.columnNodes.resize(columnCount);
  plan.cramped.assign(columnCount, 0);
  plan.blocked.assign(columnCount, 0);
  plan.door.assign(columnCount, 0);
  const auto top = grid.bounds().max.z() + 1.0;
  const auto depth = grid.bounds().size().z() + 2.0;
  for (size_t row = 0; row < plan.rows; ++row)
  {
    for (size_t column = 0; column < plan.columns; ++column)
    {
      const auto index = column + row * plan.columns;
      const auto x = plan.origin.x() + (double(column) + 0.5) * plan.cellSize;
      const auto y = plan.origin.y() + (double(row) + 0.5) * plan.cellSize;
      // the player stands on the highest surface under its box, so the rays at the
      // inset corners of the box find floors, too
      auto zs = std::vector<double>{};
      const auto inset = std::max(0.0, halfWidth - 1.0);
      for (const auto& [dx, dy] : std::array<std::array<double, 2>, 5>{
             {{0, 0},
              {-inset, -inset},
              {inset, -inset},
              {-inset, inset},
              {inset, inset}}})
      {
        const auto hits =
          castRay(map, vm::ray3d{{x + dx, y + dy, top}, {0, 0, -1}}, acceptFloor, depth);
        for (const auto& hit : hits)
        {
          if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(hit.node);
              brushNode && hit.faceIndex
              && brushNode->brush().face(*hit.faceIndex).normal().z() < 0.7)
          {
            continue;
          }
          const auto z = hit.point.z();
          if (std::ranges::none_of(
                zs, [&](const auto other) { return std::abs(other - z) < 1.0; }))
          {
            zs.push_back(z);
          }
        }
      }
      std::ranges::sort(zs);
      for (const auto z : zs)
      {
        if (standable(x, y, z))
        {
          plan.columnNodes[index].push_back(plan.nodes.size());
          plan.nodes.push_back(WalkNode{index, z, false, false});
        }
        else
        {
          plan.cramped[index] = 1;
        }
      }
    }
  }

  // doors
  visitNodes(map.worldNode(), [&](const mdl::Node& node) {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    if (!brushNode || !roles(*brushNode).door)
    {
      return;
    }
    const auto& bounds = node.logicalBounds();
    for (size_t row = 0; row < plan.rows; ++row)
    {
      for (size_t column = 0; column < plan.columns; ++column)
      {
        const auto x = plan.origin.x() + (double(column) + 0.5) * plan.cellSize;
        const auto y = plan.origin.y() + (double(row) + 0.5) * plan.cellSize;
        if (
          x > bounds.min.x() && x < bounds.max.x() && y > bounds.min.y()
          && y < bounds.max.y())
        {
          plan.door[column + row * plan.columns] = 1;
        }
      }
    }
  });

  // the start
  plan.start = options.start;
  if (!plan.start)
  {
    for (const auto* classname : {"info_player_start", "info_player_deathmatch"})
    {
      visitNodes(map.worldNode(), [&](const mdl::Node& node) {
        if (plan.start || !isPointEntity(node))
        {
          return;
        }
        const auto& entity = static_cast<const mdl::EntityNode&>(node).entity();
        if (entity.classname() == classname)
        {
          plan.start = entity.origin();
        }
      });
      if (plan.start)
      {
        break;
      }
    }
  }
  if (plan.start)
  {
    const auto startColumn = vm::vec2d{
      std::floor((plan.start->x() - plan.origin.x()) / plan.cellSize),
      std::floor((plan.start->y() - plan.origin.y()) / plan.cellSize)};
    auto bestDistance = std::numeric_limits<double>::max();
    for (int dy = -2; dy <= 2; ++dy)
    {
      for (int dx = -2; dx <= 2; ++dx)
      {
        const auto cx = startColumn.x() + dx;
        const auto cy = startColumn.y() + dy;
        if (cx < 0 || cy < 0 || cx >= double(plan.columns) || cy >= double(plan.rows))
        {
          continue;
        }
        const auto index = size_t(cx) + size_t(cy) * plan.columns;
        for (const auto nodeIndex : plan.columnNodes[index])
        {
          const auto& node = plan.nodes[nodeIndex];
          if (node.z > plan.start->z() + plan.stepHeight)
          {
            continue;
          }
          const auto position = plan.position(node);
          const auto distance = vm::squared_distance(
                                  vm::vec2d{position.x(), position.y()},
                                  vm::vec2d{plan.start->x(), plan.start->y()})
                                + (plan.start->z() - node.z) * 0.01;
          if (distance < bestDistance)
          {
            bestDistance = distance;
            plan.startNode = nodeIndex;
          }
        }
      }
    }

    const auto referenceZ =
      plan.startNode ? plan.nodes[*plan.startNode].z : plan.start->z();
    if (
      const auto layer = grid.cellAt(
        {grid.origin.x() + 1.0,
         grid.origin.y() + 1.0,
         referenceZ + plan.playerHeight / 2.0}))
    {
      for (size_t index = 0; index < columnCount; ++index)
      {
        const auto cell =
          CellIndex{index % plan.columns, index / plan.columns, (*layer)[2]};
        plan.blocked[index] = grid.solid[grid.index(cell)];
      }
    }
  }

  // edges
  auto edges = std::vector<std::vector<size_t>>(plan.nodes.size());
  auto reverse = std::vector<std::vector<size_t>>(plan.nodes.size());
  const auto offsets =
    std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
  for (size_t a = 0; a < plan.nodes.size(); ++a)
  {
    const auto& node = plan.nodes[a];
    const auto column = node.column % plan.columns;
    const auto row = node.column / plan.columns;
    const auto positionA = plan.position(node);
    for (const auto& offset : offsets)
    {
      const auto cx = int64_t(column) + offset[0];
      const auto cy = int64_t(row) + offset[1];
      if (cx < 0 || cy < 0 || cx >= int64_t(plan.columns) || cy >= int64_t(plan.rows))
      {
        continue;
      }
      const auto& targets = plan.columnNodes[size_t(cx) + size_t(cy) * plan.columns];
      auto walkTarget = std::optional<size_t>{};
      for (const auto b : targets)
      {
        const auto zb = plan.nodes[b].z;
        if (zb <= node.z + plan.stepHeight)
        {
          walkTarget = b;
        }
        else if (zb <= node.z + plan.jumpHeight)
        {
          // jump up: needs head room above the start of the jump
          if (fits(positionA.x(), positionA.y(), zb))
          {
            edges[a].push_back(b);
            reverse[b].push_back(a);
          }
        }
      }
      if (walkTarget)
      {
        const auto zb = plan.nodes[*walkTarget].z;
        const auto positionB = plan.position(plan.nodes[*walkTarget]);
        if (zb >= node.z - plan.stepHeight || fits(positionB.x(), positionB.y(), node.z))
        {
          edges[a].push_back(*walkTarget);
          reverse[*walkTarget].push_back(a);
        }
      }
    }
  }

  if (plan.startNode)
  {
    const auto flood =
      [&](const std::vector<std::vector<size_t>>& graph, bool WalkNode::*flag) {
        auto queue = std::vector<size_t>{*plan.startNode};
        plan.nodes[*plan.startNode].*flag = true;
        for (size_t head = 0; head < queue.size(); ++head)
        {
          for (const auto next : graph[queue[head]])
          {
            if (!(plan.nodes[next].*flag))
            {
              plan.nodes[next].*flag = true;
              queue.push_back(next);
            }
          }
        }
      };
    flood(edges, &WalkNode::reachable);
    flood(reverse, &WalkNode::canReturn);
  }

  return plan;
}

} // namespace tb::mcp
