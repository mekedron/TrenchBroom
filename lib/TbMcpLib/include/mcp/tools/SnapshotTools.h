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

#include "mcp/Errors.h"
#include "mcp/Snapshot.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace tb::mcp
{
class CallContext;
class ToolRegistry;

/**
 * Registers the agent camera tools (agent_camera_set / get / list / delete) and the
 * snapshot tools (view_snapshot, view_snapshots_around, view_snapshot_compare,
 * view_snapshot_user).
 */
void registerSnapshotTools(ToolRegistry& registry);

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

} // namespace tb::mcp
