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
#include "mcp/Pagination.h"

#include <string>
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
class IdRegistry;

// Shared JSON descriptions of map objects, used by the scene, spatial and selection tools
// and the resources. Keep these compact: they appear in lists.

/**
 * A short human-readable label: the classname of an entity ("monster_ogre"), the name
 * of a group or layer, "worldspawn" for the world, "brush" or "patch" otherwise.
 */
std::string nodeLabel(const mdl::Node& node);

/** The distinct material names of a brush or patch, sorted; empty for other nodes. */
std::vector<std::string> nodeMaterials(const mdl::Node& node);

/** The names of the smart tags of a node (e.g. "trigger", "detail"). */
std::vector<std::string> nodeTagNames(const mdl::Map& map, const mdl::Node& node);

/** The names of the smart tags of a face (e.g. "clip", "skip"). */
std::vector<std::string> faceTagNames(const mdl::Map& map, const mdl::BrushFace& face);

/**
 * The id of the layer containing the node (canonical, e.g. "layer:default"), or null for
 * the world and nodes outside a layer.
 */
Json layerIdOf(const mdl::Node& node, const IdRegistry& ids);

/** The id of the innermost group containing the node, or null. */
Json groupIdOf(const mdl::Node& node, const IdRegistry& ids);

/**
 * A compact description of a node for lists:
 * `{"id", "kind", "label", "bounds"}` plus `"classname"` for entities, `"name"` for
 * layers and groups, `"materials"` for brushes and patches, `"entity"` (the owning brush
 * entity id) for brushes of brush entities, and `"layer"`.
 */
Json nodeSummary(const mdl::Node& node, const IdRegistry& ids);

/**
 * The visibility and lock state of a node as seen by the editor:
 * `{"visible", "hidden", "locked", "selected", "selectable"}`.
 */
Json nodeState(const mdl::Map& map, const mdl::Node& node);

/**
 * Describes one face of a brush. Summary: `{"id", "index", "normal", "center",
 * "material"}`. Full additionally has `"offset", "scale", "rotation", "area", "tags"`,
 * the surface attributes (`"surfaceContents"`, `"surfaceFlags"`, `"surfaceValue"`, if
 * set) and `"vertices"`.
 */
Json faceJson(
  const mdl::Map& map,
  const mdl::BrushNode& brushNode,
  size_t faceIndex,
  const IdRegistry& ids,
  Detail detail = Detail::Summary);

} // namespace tb::mcp
