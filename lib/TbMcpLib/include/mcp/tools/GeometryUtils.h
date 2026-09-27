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
#include "mcp/Json.h"

#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Brush;
class BrushBuilder;
class BrushNode;
class EntityNode;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::mcp
{
class Args;
class CallContext;
class IdRegistry;

// Shared helpers of the geometry, brush editing and transform tools (epic E4). Keep this
// small; it must not grow into a second tool file.

/**
 * Whether the interior of the convex brush intersects the given box (separating axis
 * test over the brush face normals, the box axes and the cross products of the brush
 * edges with the box axes). Surfaces that merely touch do not count. The box may be
 * degenerate (e.g. flat). Used by space_check, map_plan_view and opening_cut.
 */
bool intersectsInterior(const mdl::Brush& brush, const vm::bbox3d& box);

/** The brush entity that owns the given node, or nullptr (world brushes). */
const mdl::EntityNode* owningBrushEntity(const mdl::Node& node);

/** Classification of brushes by their owning entity. */
enum class BrushClass
{
  /** World brushes and brushes of func_group / func_detail* entities. */
  Solid,
  /** Brushes of other brush entities (doors, platforms, ...). */
  Entity,
  /** Brushes of trigger_* entities. */
  Trigger,
};

BrushClass classifyBrush(const mdl::BrushNode& brushNode);

/** Whether the node is an entity without children (a point entity). */
bool isPointEntity(const mdl::Node& node);

/**
 * Whether the node is in a layer omitted from export: the compiler never sees it. Hidden
 * and locked objects are compiled.
 */
bool inOmittedLayer(const mdl::Node& node);

struct RayHit
{
  double distance;
  vm::vec3d point;
  mdl::Node* node;
  std::optional<size_t> faceIndex;
};

/**
 * Casts the ray against brushes, patches and point entities accepted by the given
 * predicate. Like the editor's picking, brush faces are only hit from the front, so a ray
 * starting inside a brush does not hit that brush, and a ray starting inside the bounds
 * of a point entity does not hit that entity. Returns the hits sorted by distance. Used
 * by ray_pick, space_check and entity placement.
 */
std::vector<RayHit> castRay(
  mdl::Map& map,
  const vm::ray3d& ray,
  const std::function<bool(const mdl::Node&)>& accept,
  std::optional<double> maxDistance = std::nullopt);

/** A brush builder that uses the map format, world bounds and the game's face defaults.
 */
mdl::BrushBuilder brushBuilder(const mdl::Map& map);

/**
 * The material named by the argument `key`, or the current material if it is absent.
 * Warns with UNKNOWN_MATERIAL (X14) if the material is not loaded; the name is still
 * used, as the editor does.
 */
std::string materialArgument(
  CallContext& context, const Args& args, std::string_view key = "material");

/**
 * Checks that a box that should become a brush has a positive size along every axis
 * (INVALID_GEOMETRY) and lies inside the world bounds (OUT_OF_WORLD_BOUNDS). `what`
 * names the box in the message, e.g. "The box" or "The opening".
 */
std::optional<ToolError> checkBox(
  const mdl::Map& map, const vm::bbox3d& box, std::string_view what = "The box");

/**
 * Checks that the given nodes lie strictly inside the world bounds after an operation.
 * Brushes that reach the world bounds were clipped by them, so touching the bounds is
 * reported as well. Returns OUT_OF_WORLD_BOUNDS naming all offending ids.
 */
std::optional<ToolError> checkInsideWorldBounds(
  const std::vector<mdl::Node*>& nodes,
  const mdl::Map& map,
  const IdRegistry& ids,
  std::string hint = {});

/**
 * Turns an mdl error message (e.g. "Brush is empty") about the given objects into an
 * actionable INVALID_GEOMETRY error (E4.16). Messages that mention the world bounds
 * become OUT_OF_WORLD_BOUNDS.
 */
ToolError geometryError(
  std::string message,
  const std::string& mdlMessage,
  std::vector<std::string> objectIds,
  std::string hint);

/**
 * Like CallContext::operationFailed, for a Map_* geometry function that returned false:
 * classifies the failure as INVALID_GEOMETRY (or OUT_OF_WORLD_BOUNDS if the editor
 * logged a world bounds problem) and names the involved objects.
 */
ToolError geometryOperationFailed(
  const CallContext& context,
  std::string message,
  std::vector<std::string> objectIds,
  std::string hint);

/**
 * Adds the brushes to the parent for new nodes (the open group or the current layer),
 * or to `parent` if given. Returns the added nodes (empty if adding failed).
 */
std::vector<mdl::Node*> addBrushes(
  mdl::Map& map, std::vector<mdl::Brush> brushes, mdl::Node* parent = nullptr);

/** `[nodeSummary, ...]` of the given nodes. */
Json nodeSummaries(const std::vector<mdl::Node*>& nodes, const IdRegistry& ids);

/** The ids of the given nodes. */
std::vector<std::string> formatIds(
  const std::vector<mdl::Node*>& nodes, const IdRegistry& ids);

/**
 * Warns with NON_INTEGER_VERTICES (scenario S7) about brushes among the given nodes
 * (and their descendants) whose vertices do not have integer coordinates, since such
 * vertices are rounded when the map is saved.
 */
void warnNonIntegerVertices(CallContext& context, const std::vector<mdl::Node*>& nodes);

/**
 * Overrides the alignment lock (texture lock) and UV lock of the map's editor context for
 * the lifetime of this object, e.g. for a tool argument `alignmentLock: false`. An
 * absent value keeps the current setting. The previous settings are restored on
 * destruction.
 */
class ScopedLockOverride
{
private:
  mdl::Map& m_map;
  bool m_previousAlignmentLock;
  bool m_previousUvLock;

public:
  ScopedLockOverride(
    mdl::Map& map, std::optional<bool> alignmentLock, std::optional<bool> uvLock = {});
  ~ScopedLockOverride();

  ScopedLockOverride(const ScopedLockOverride&) = delete;
  ScopedLockOverride& operator=(const ScopedLockOverride&) = delete;
};

} // namespace tb::mcp
