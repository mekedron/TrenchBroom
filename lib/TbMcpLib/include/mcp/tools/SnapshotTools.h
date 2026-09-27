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

#include "mcp/AgentCamera.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/Schema.h"
#include "mcp/Snapshot.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{
class BrushFace;
class BrushNode;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::mcp
{
class CallContext;
class IdRegistry;
class ToolRegistry;

/**
 * Registers the agent camera tools (agent_camera_set / get / list / delete) and the
 * snapshot tools (view_snapshot, view_snapshots_around, view_snapshot_compare,
 * view_snapshot_user).
 */
void registerSnapshotTools(ToolRegistry& registry);

/** A camera argument resolved to an agent camera. */
struct ResolvedCamera
{
  AgentCamera camera;
  /** The name of the agent camera, if one was used. */
  std::optional<std::string> name;
  /** Details of eye height placement, if used. */
  Json placement = nullptr;
};

/**
 * How `eyeHeight` cameras find the floor and the eye height when the scene is not the
 * document's map (e.g. a compiled BSP).
 */
struct CameraFloor
{
  std::function<std::optional<double>(const vm::vec3d&)> findFloor;
  double eyeHeight = 0.0;
  /** The game family reported with the placement. */
  std::string game;
};

/** The schema of a camera argument: an agent camera name or an inline camera. */
schema::Schema cameraArgumentSchema();

/**
 * Resolves a camera argument like view_snapshot: an agent camera name, an inline camera
 * (perspective, orthographic, frame, orbit, eyeHeight) or null (frame the default
 * bounds). `cameraFloor` replaces the document's map for eyeHeight placement.
 */
Result<ResolvedCamera, ToolError> resolveCameraArgument(
  CallContext& context,
  const Json& arg,
  size_t width,
  size_t height,
  const std::function<std::optional<vm::bbox3d>()>& defaultBounds,
  const CameraFloor* cameraFloor = nullptr);

/** The top-down image of map_plan_view (format "image" or "both"). */
struct PlanImageRequest
{
  /** The min x, y of the plan's first cell, the cell size and the number of cells. */
  double originX = 0.0;
  double originY = 0.0;
  double cellSize = 8.0;
  size_t columns = 1;
  size_t rows = 1;
  /** The height of the slice; geometry above it is cut away. */
  double height = 0.0;
  /** How far below the slice geometry is drawn. */
  double floorDepth = 1024.0;
  bool includeHidden = false;
  /** The image size; derived from the number of cells if not given. */
  std::optional<size_t> width;
  std::optional<size_t> imageHeight;
  /** Markers for the point entities. */
  std::vector<SnapshotMarker> markers;
};

/**
 * Renders the plan view image synchronously, adds it to the call's content and returns
 * `{image: {width, height, format, bytes, savedTo}, camera, counts}`. Fails with
 * UNSUPPORTED_IN_HOST if the host cannot render.
 */
ToolResult renderPlanImage(CallContext& context, const PlanImageRequest& request);

/** The objects that a snapshot draws. */
struct SnapshotVisibility
{
  /** The drawn brushes, patches, point entities, brush entities and groups. */
  std::unordered_set<const mdl::Node*> nodes;
  /** The face filter of the snapshot; null draws all faces. */
  std::function<bool(const mdl::BrushNode&, const mdl::BrushFace&)> faceFilter;

  bool drawsNode(const mdl::Node& node) const;
  bool drawsFace(const mdl::BrushNode& brushNode, size_t faceIndex) const;
};

/**
 * The objects that a snapshot with the given view arguments (options, isolate,
 * highlight; see SnapshotRecord::view) draws in the current state of the map, resolved
 * like view_snapshot does. Ids that no longer exist are skipped.
 */
SnapshotVisibility snapshotVisibility(
  mdl::Map& map, const IdRegistry& ids, const Json& view);

} // namespace tb::mcp
