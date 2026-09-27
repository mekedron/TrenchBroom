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

#include "vm/bbox.h"
#include "vm/vec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class EntityNode;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::mcp
{

// Spatial analysis of the empty volume of a map (epic E12): a voxel grid of the empty
// space, leak prediction, spaces (rooms) with their openings, free spots and walkability.
// The analysis considers all objects, hidden ones included (like a compiler), except
// those in layers that are omitted from export.

// Brush roles

/** How a brush takes part in the analysis. Decided by its entity and its materials. */
struct BrushRole
{
  /**
   * Bounds spaces: world brushes and brushes of func_group and func_detail* entities
   * (not func_detail_illusionary), unless all faces have tool or liquid materials.
   */
  bool spaceSolid = false;
  /** Seals the map against leaks: spaceSolid brushes that are not func_detail*. */
  bool seals = false;
  /**
   * Blocks the player: spaceSolid brushes, clip brushes and the brushes of solid brush
   * entities (func_wall, func_plat, ...), but not doors, triggers, func_illusionary or
   * liquids.
   */
  bool blocksPlayer = false;
  /** A brush of a func_door* entity: an opening that the player can pass. */
  bool door = false;
  /**
   * Blocks placed objects: brushes that are not tool-only, liquid or trigger_* brushes
   * (doors and brush entities included).
   */
  bool blocksObjects = false;
  /** All faces have liquid materials, or the brush belongs to a func_water entity. */
  bool liquid = false;
};

BrushRole brushRole(const mdl::BrushNode& brushNode);

/**
 * Whether the material name denotes a compiler tool material that does not seal or
 * bound space (clip, trigger, hint, skip, origin, bevel, areaportal, ...).
 */
bool isToolMaterialName(std::string_view materialName);

/** Whether the material name denotes a clip material (clip, playerclip, ...). */
bool isClipMaterialName(std::string_view materialName);

/** Whether the material name denotes a liquid ('*' or '!' prefix, water, lava, slime). */
bool isLiquidMaterialName(std::string_view materialName);

// Voxel grid

using CellIndex = std::array<size_t, 3>;

/**
 * A regular grid of cubic cells aligned to multiples of the cell size. A cell is solid if
 * the interior of a brush of the rasterized set overlaps the interior of the cell
 * (conservative: a brush of any thickness blocks, so a sealed shell of brushes is sealed
 * in the grid as well; gaps narrower than a cell may be closed).
 */
struct VoxelGrid
{
  vm::vec3d origin = {0, 0, 0};
  double cellSize = 16.0;
  CellIndex dims = {0, 0, 0};
  std::vector<uint8_t> solid;

  size_t size() const { return dims[0] * dims[1] * dims[2]; }
  size_t index(const CellIndex& cell) const
  {
    return cell[0] + dims[0] * (cell[1] + dims[1] * cell[2]);
  }
  CellIndex cellOf(size_t index) const;
  vm::bbox3d cellBounds(const CellIndex& cell) const;
  vm::vec3d cellCenter(const CellIndex& cell) const;
  /** The cell containing the point, or nullopt if it is outside the grid. */
  std::optional<CellIndex> cellAt(const vm::vec3d& point) const;
  vm::bbox3d bounds() const;
};

/**
 * A grid over the region, aligned to multiples of the cell size and padded by the given
 * number of cells on every side. Fails if it would have more than maxCells cells.
 */
Result<VoxelGrid, ToolError> makeGrid(
  const vm::bbox3d& region, double cellSize, size_t padding, size_t maxCells);

/**
 * Marks the cells overlapped by the interior of the brushes accepted by the predicate.
 * Axis-aligned cuboids are rasterized by their bounds, other brushes cell by cell with
 * intersectsInterior.
 */
void rasterize(
  VoxelGrid& grid,
  const mdl::Map& map,
  const std::function<bool(const mdl::BrushNode&, const BrushRole&)>& accept);

/**
 * The bounds of the brushes accepted by the predicate (all brushes of the map that are
 * not in a layer omitted from export), or nullopt if there are none.
 */
std::optional<vm::bbox3d> brushBounds(
  const mdl::Map& map,
  const std::function<bool(const mdl::BrushNode&, const BrushRole&)>& accept);

/**
 * Half the player width of the map's game (16 for Quake and Half-Life), the default
 * cell size of the analysis.
 */
double defaultCellSize(const mdl::Map& map);

// Leak prediction (E12.8). The interface below is used by PlacementChecks; keep it
// stable (extend it, do not rename or remove members).

struct LeakOptions
{
  /** The edge length of the flood fill cells; 0 chooses it from the map size. */
  double cellSize = 0.0;
  /** The analysis is skipped when the cell grid would have more cells. */
  size_t maxCells = 4'000'000;
};

/** A point entity that the empty space outside the map reaches. */
struct LeakFinding
{
  const mdl::EntityNode* entity = nullptr;
  /** The entity's origin. */
  vm::vec3d position;
  /**
   * The nearest gap: where the flood fill path from the entity to the outside passes
   * the sealing brushes (none if the entity is outside all brushes).
   */
  std::optional<vm::bbox3d> gap;
  /** Sealing brushes next to the gap. */
  std::vector<const mdl::BrushNode*> gapBrushes;
};

struct LeakReport
{
  /** False if the analysis was skipped (no sealing brushes, too many cells). */
  bool analyzed = false;
  /** Why the analysis was skipped. */
  std::string skippedReason;
  double cellSize = 0.0;
  std::vector<LeakFinding> findings;
  /** The number of point entities that were checked. */
  size_t checkedEntities = 0;
};

/**
 * Finds point entities outside the sealed volume by a flood fill of the empty space
 * from outside the bounds of the sealing brushes.
 *
 * Sealing brushes are world brushes and func_group brushes whose faces are not all tool
 * or liquid materials (sky seals; clip, hint, skip, trigger, origin do not; func_detail
 * does not seal, as in ericw-tools and VHLT). Only point entities with an "origin"
 * property are checked. An entity whose origin cell is overlapped by a brush uses the
 * nearest free neighbour cell that a straight line from the origin reaches; entities
 * inside sealing brushes are not reported.
 *
 * The automatic cell size is the smallest power of two >= 8 that keeps the grid at
 * about a million cells, so gaps smaller than a cell may be missed.
 */
LeakReport predictLeaks(const mdl::Map& map, const LeakOptions& options = {});

// Spaces (E12.3)

struct SpaceOptions
{
  /** The region to analyze; default: the bounds of the space-solid brushes. */
  std::optional<vm::bbox3d> region = std::nullopt;
  /** The cell size; 0 uses defaultCellSize. */
  double cellSize = 0.0;
  /**
   * Openings (doorways, windows) whose smaller side is at most this size separate
   * spaces; wider openings join them. The size is rounded down to a multiple of twice
   * the cell size (the erosion distance is openingSize / cellSize / 2 cells, rounded
   * down). Rooms whose smallest inner dimension (usually the height) is not larger
   * than that have no core and merge with their neighbours.
   */
  double openingSize = 96.0;
  size_t maxCells = 4'000'000;
};

/** A place where two spaces, or a space and the void, touch. */
struct OpeningGeometry
{
  size_t spaceA = 0;
  /** The other space, or nullopt for the void outside the map. */
  std::optional<size_t> spaceB;
  /** The bounds of the cell faces between the spaces (flat along the normal axis). */
  vm::bbox3d bounds;
  /** The axis along which the opening is passed: 0, 1 or 2 (a hole in a floor). */
  size_t axis = 0;
  /** Unit vector along the axis, from spaceA to spaceB. */
  vm::vec3d normal = {1, 0, 0};
  /** Whether the cells below the opening's lowest faces are solid (a floor). */
  bool onFloor = false;
  size_t faceCount = 0;
};

struct SpaceGeometry
{
  /** `space:<hash of the cell bounds>`; unchanged as long as the bounds are. */
  std::string id;
  /**
   * The inner bounds: the cells that the room core reaches within the erosion distance,
   * i.e. the room up to its walls without the openings (all cells for passages).
   */
  vm::bbox3d bounds;
  size_t cellCount = 0;
  double volume = 0.0;
  /**
   * False if the space is connected to the void outside the map through a face of a
   * cell; air that touches the space only along an edge or a corner does not count, as
   * compilers do not leak through it.
   */
  bool sealed = true;
  /**
   * Sealed spaces only: where the space touches air connected to the outside only along
   * an edge or a corner (the bounds of the two cells), e.g. walls that meet only at an
   * edge; nullopt if it does not.
   */
  std::optional<vm::bbox3d> edgeGap;
  /**
   * Whether the space grew from a room core; false for passages and pockets, which
   * are narrower than openingSize in every direction.
   */
  bool hasCore = true;
  /** Indices into SpaceMap::openings. */
  std::vector<size_t> openings;
  /** Indices of neighbouring spaces. */
  std::vector<size_t> neighbours;
};

/** The result of the space segmentation; contains no node pointers. */
struct SpaceMap
{
  static constexpr int32_t Solid = -1;
  static constexpr int32_t Void = -2;
  /** Small isolated pockets of empty cells. */
  static constexpr int32_t Pocket = -3;

  VoxelGrid grid;
  /** Per cell: a space index, Solid, Void or Pocket. */
  std::vector<int32_t> labels;
  /** Per cell: whether it is connected to the outside of the grid. */
  std::vector<uint8_t> outside;
  std::vector<SpaceGeometry> spaces;
  std::vector<OpeningGeometry> openings;
  double openingSize = 0.0;

  /** The label of the cell containing the point, or Void outside the grid. */
  int32_t labelAt(const vm::vec3d& point) const;
  /**
   * The space containing the point; if the point's cell is solid, the space of the
   * nearest neighbouring cell within `searchCells` cells.
   */
  std::optional<size_t> spaceAt(const vm::vec3d& point, size_t searchCells = 1) const;
  std::optional<size_t> findSpace(std::string_view id) const;
};

/**
 * Segments the empty volume into spaces: the empty cells are eroded by
 * openingSize / 2 (chessboard distance to solid cells), the 26-connected parts of the
 * remainder are room cores, which grow back over the empty cells (first up to the erosion
 * distance, then narrow passages that are long enough become spaces of their own, then
 * everything). Cores and cells at the border of the grid that are connected to it form
 * the void. Growth never joins a cell connected to the outside (6-connected, like a BSP
 * compiler's flood fill) with one that is not. Boundaries between two labels are
 * openings.
 */
Result<SpaceMap, ToolError> analyzeSpaces(
  const mdl::Map& map, const SpaceOptions& options = {});

struct HeightStats
{
  double min = 0.0;
  double max = 0.0;
  /** The most common value. */
  double typical = 0.0;
};

/** The details of a space that need the map's nodes. */
struct SpaceDetails
{
  /** Floor heights from rays cast down in every column of the space. */
  std::optional<HeightStats> floor;
  std::optional<HeightStats> ceiling;
  /** The area of the columns with a floor. */
  double floorArea = 0.0;
  /**
   * The objects inside: point entities (by origin), brush entities, groups and patches
   * (by the center of their bounds, or the top center if the center is solid). World
   * brushes outside groups are the space's shell and not listed.
   */
  std::vector<const mdl::Node*> contents;
  std::vector<std::string> layers;
  std::vector<std::string> groups;
};

SpaceDetails describeSpace(mdl::Map& map, const SpaceMap& spaces, size_t spaceIndex);

/** The details of an opening, measured with rays against the space-solid brushes. */
struct OpeningDetails
{
  /** "doorway" (reaches the floor), "window", or "hole" (in a floor or ceiling). */
  std::string kind;
  vm::bbox3d bounds;
  vm::vec3d center;
  /** The horizontal size across the opening (for holes: the size along x). */
  double width = 0.0;
  /** The vertical size (for holes: the size along y). */
  double height = 0.0;
  /** The height (z) of the opening's bottom. */
  double bottom = 0.0;
  /** Unit vector along the axis from spaceA to spaceB. */
  vm::vec3d normal;
  /** func_door* entities in the opening. */
  std::vector<const mdl::EntityNode*> doors;
};

OpeningDetails describeOpening(
  mdl::Map& map, const SpaceMap& spaces, size_t openingIndex);

// Free spots (E12.5)

enum class Placement
{
  Floor,
  Wall,
  Ceiling,
  Any,
};

std::optional<Placement> placementFromString(std::string_view name);

enum class SpotSort
{
  /** Spread out: each next spot is the farthest from the spots chosen so far. */
  Spread,
  /** Nearest to FreeSpotOptions::near first. */
  Near,
};

struct FreeSpotOptions
{
  vm::vec3d size = {16, 16, 16};
  Placement placement = Placement::Floor;
  /** Only spots inside this space. */
  std::optional<size_t> space = std::nullopt;
  /** Only spots whose box lies in this region. */
  std::optional<vm::bbox3d> region = std::nullopt;
  /** Also spots outside all spaces (in the void). */
  bool includeOutside = false;
  /**
   * Free distance to walls: space-solid world and func_group brushes; brushes in a group
   * only if the group's bounds contain the candidate's center (a room built as a group),
   * otherwise they are objects.
   */
  double wallDistance = 0.0;
  /** Free distance to other objects (point entities, brush entities, groups). */
  double objectDistance = 0.0;
  /** Candidate positions are multiples of this step; 0 chooses it. */
  double step = 0.0;
  /**
   * Floor and ceiling placement: the fraction of the five support rays (center and
   * inset corners) that must find a surface within 1 unit.
   */
  double support = 1.0;
  /** Wall placement: swap x and y of the size for walls facing along x. */
  bool rotate = true;
  /** Wall placement: preferred height of the box bottom above the floor below it. */
  std::optional<double> heightAboveFloor = std::nullopt;
  size_t limit = 10;
  SpotSort sort = SpotSort::Spread;
  std::optional<vm::vec3d> near = std::nullopt;
  /** Extra bounds of point entities, e.g. their model bounds. */
  std::function<std::optional<vm::bbox3d>(const mdl::EntityNode&)> entityBounds = {};
  size_t maxCandidates = 20'000;
};

struct FreeSpot
{
  vm::bbox3d box;
  /** Where a point entity with this bounds size would have its origin: bottom center. */
  vm::vec3d origin;
  /** Free distance from each side of the box: -x, +x, -y, +y, -z, +z (nullopt: > 1024).
   */
  std::array<std::optional<double>, 6> clearance;
  std::optional<size_t> space = std::nullopt;
  /** Whether x and y of the size were swapped (wall placement). */
  bool rotated = false;
  // wall placement
  const mdl::BrushNode* wallBrush = nullptr;
  size_t wallFace = 0;
  vm::vec3d wallNormal = {0, 0, 0};
  /** The range of the box bottom z for which the box fits at this position. */
  std::optional<std::array<double, 2>> heightRange;
  /** The floor below the box, if any. */
  std::optional<double> floor;
};

struct FreeSpotResult
{
  std::vector<FreeSpot> spots;
  size_t candidates = 0;
  /** Whether the candidates were thinned out because there were too many. */
  bool coarsened = false;
  double step = 0.0;
};

/**
 * Finds free boxes of the given size. Candidates come from a lattice over the search
 * region (for walls: over the axis-aligned wall faces that face into the region) and
 * are verified against the real geometry with intersectsInterior (touching surfaces
 * do not count), not only against the grid. `spaces` is needed for `options.space` and
 * to exclude the void.
 */
Result<FreeSpotResult, ToolError> findFreeSpots(
  mdl::Map& map, const SpaceMap* spaces, const FreeSpotOptions& options);

// Walkability (E12.6)

/** Quake-family and Half-Life step height. */
constexpr auto DefaultStepHeight = 18.0;
/**
 * A standing jump: Quake, Quake 2, Quake 3 and Half-Life jump about 45 units
 * (270 u/s at 800 u/s^2 gravity); a Half-Life crouch jump reaches about 63.
 */
constexpr auto DefaultJumpHeight = 45.0;

struct WalkOptions
{
  std::optional<vm::bbox3d> region = std::nullopt;
  /** 0 uses defaultCellSize. */
  double cellSize = 0.0;
  /** 0 uses playerSize() of the game. */
  double playerWidth = 0.0;
  double playerHeight = 0.0;
  double stepHeight = DefaultStepHeight;
  double jumpHeight = DefaultJumpHeight;
  /** The start point; default: info_player_start, then info_player_deathmatch. */
  std::optional<vm::vec3d> start = std::nullopt;
  size_t maxCells = 4'000'000;
  size_t maxColumns = 250'000;
};

/** A position where the player can stand: a column of the plan and a floor height. */
struct WalkNode
{
  size_t column = 0;
  double z = 0.0;
  bool reachable = false;
  /** Whether the start can be reached again from here. */
  bool canReturn = false;
};

struct WalkPlan
{
  vm::vec2d origin;
  double cellSize = 0.0;
  size_t columns = 0;
  size_t rows = 0;
  double stepHeight = 0.0;
  double jumpHeight = 0.0;
  double playerWidth = 0.0;
  double playerHeight = 0.0;
  vm::bbox3d region;
  std::vector<WalkNode> nodes;
  /** Per column (x + y * columns): the indices of its nodes, lowest first. */
  std::vector<std::vector<size_t>> columnNodes;
  /** Per column: whether it has a floor without room for the player. */
  std::vector<uint8_t> cramped;
  /** Per column: whether a player-blocking brush fills the column at the start height. */
  std::vector<uint8_t> blocked;
  /** Per column: whether a door brush covers the column. */
  std::vector<uint8_t> door;
  std::optional<size_t> startNode;
  std::optional<vm::vec3d> start = std::nullopt;

  vm::vec3d position(const WalkNode& node) const;
};

/**
 * Finds where the player's center can stand (a free player box on a floor, the lowest
 * step height of the box ignored) and which of these positions can be reached from the
 * start by walking (height difference <= stepHeight), jumping up (<= jumpHeight) or
 * dropping down. The box rests on the highest floor under it: a floor with a standable
 * floor at most stepHeight higher under the box is not a position of its own. Doors are
 * passable. Floors are brush faces (walkable slope, normal z >= 0.7) and patches.
 */
Result<WalkPlan, ToolError> planWalk(mdl::Map& map, const WalkOptions& options = {});

} // namespace tb::mcp
