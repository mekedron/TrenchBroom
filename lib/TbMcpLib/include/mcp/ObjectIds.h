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

#include "base/NotifierConnection.h"
#include "base/Result.h"
#include "mcp/Errors.h"
#include "mdl/IdType.h"

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace tb
{
namespace mdl
{
class BrushNode;
class GroupNode;
class LayerNode;
class Map;
class Node;
} // namespace mdl

namespace ui
{
class MapDocument;
}

namespace mcp
{

enum class ObjectKind
{
  World,
  Layer,
  Group,
  Entity,
  Brush,
  Patch,
};

std::string_view toString(ObjectKind kind);
std::optional<ObjectKind> objectKindFromString(std::string_view str);
ObjectKind objectKindOf(const mdl::Node& node);

/**
 * A parsed object id. External ids look like `brush:1042`, `brush:1042/face:3`, `world`
 * or `layer:default`.
 */
struct ObjectRef
{
  ObjectKind kind = ObjectKind::World;
  /** Unset for `world` and `layer:default`. */
  std::optional<mdl::IdType> runtimeId;
  bool isDefaultLayer = false;
  std::optional<size_t> faceIndex;

  bool operator==(const ObjectRef&) const = default;
};

std::optional<ObjectRef> parseObjectRef(std::string_view id);
std::string formatObjectRef(const ObjectRef& ref);

/**
 * Maps object ids to nodes for one document.
 *
 * Ids are based on `Node::runtimeId()`, so they survive undo and redo because undo
 * re-adds the same node objects. The registry only knows nodes that are currently in the
 * tree, so it never hands out a pointer to a node that may have been freed.
 *
 * When linked groups are updated, the children of the target groups are replaced by fresh
 * clones. The registry detects this (removal followed by addition of nodes with the same
 * link ids under the same group) and keeps the old ids for the clones (aliasing), so that
 * an agent's id for a node in a linked copy stays valid.
 *
 * When the document is reloaded, node ids become invalid, except for layers and groups,
 * which are remapped by their persistent ids.
 */
class IdRegistry
{
private:
  using LinkKey = std::tuple<const mdl::Node*, std::vector<std::string>>;

  ui::MapDocument& m_document;
  /**
   * The map the ids were built for. A document replaces its map when it is created or
   * loaded in place; a new map window only notifies that the document was loaded.
   */
  const mdl::Map* m_map = nullptr;
  std::unordered_map<mdl::IdType, mdl::Node*> m_nodes;
  /** runtime id of a clone -> canonical (first seen) id */
  std::unordered_map<mdl::IdType, mdl::IdType> m_canonicalIds;
  /** canonical id -> runtime id of the node that currently represents it */
  std::unordered_map<mdl::IdType, mdl::IdType> m_currentIds;
  /** runtime id of a layer or group -> its persistent id, kept after removal */
  std::unordered_map<mdl::IdType, std::tuple<ObjectKind, mdl::IdType>> m_persistentIds;
  /** nodes removed in the current notification burst, for linked group aliasing */
  std::vector<std::tuple<LinkKey, mdl::IdType>> m_recentlyRemoved;
  /** all runtime ids below this value were assigned before the last reload */
  mdl::IdType m_reloadThreshold = 0;

  NotifierConnection m_notifierConnection;

public:
  explicit IdRegistry(ui::MapDocument& document);
  ~IdRegistry();

  IdRegistry(const IdRegistry&) = delete;
  IdRegistry& operator=(const IdRegistry&) = delete;

  /** Returns the canonical external id of the given node, e.g. `brush:1042`. */
  std::string format(const mdl::Node& node) const;

  /** Returns the canonical external id of the given face, e.g. `brush:1042/face:3`. */
  std::string formatFace(const mdl::BrushNode& brushNode, size_t faceIndex) const;

  /**
   * Resolves the given external id to a node that is currently in the tree. Face ids
   * resolve to their brush.
   */
  Result<mdl::Node*, ToolError> resolve(std::string_view id) const;

  /** Resolves a parsed reference. */
  Result<mdl::Node*, ToolError> resolve(const ObjectRef& ref) const;

  /** Returns the node with the given runtime id if it is in the tree. */
  mdl::Node* findByRuntimeId(mdl::IdType runtimeId) const;

  /** The number of registered nodes. */
  size_t size() const;

private:
  mdl::IdType canonicalId(const mdl::Node& node) const;
  mdl::Node* findCurrent(mdl::IdType canonicalId) const;

  void rebuild();
  void registerRecursively(mdl::Node& node);
  void unregisterRecursively(const mdl::Node& node);

  void collectLinkKeys(
    const mdl::Node& node,
    const mdl::Node* anchor,
    std::vector<std::string> path,
    std::vector<std::tuple<LinkKey, const mdl::Node*>>& result) const;

  void documentWasLoaded();
  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
};

} // namespace mcp
} // namespace tb
