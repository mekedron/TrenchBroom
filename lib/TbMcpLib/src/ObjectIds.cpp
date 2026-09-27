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

#include "mcp/ObjectIds.h"

#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Object.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"
#include "kd/overload.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace tb::mcp
{
namespace
{

constexpr auto ObjectKindNames = std::array<std::pair<ObjectKind, std::string_view>, 6>{{
  {ObjectKind::World, "world"},
  {ObjectKind::Layer, "layer"},
  {ObjectKind::Group, "group"},
  {ObjectKind::Entity, "entity"},
  {ObjectKind::Brush, "brush"},
  {ObjectKind::Patch, "patch"},
}};

std::optional<size_t> parseNumber(const std::string_view str)
{
  if (str.empty() || (str.size() > 1 && str.front() == '0'))
  {
    return std::nullopt;
  }
  auto result = size_t{0};
  const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), result);
  if (ec != std::errc{} || ptr != str.data() + str.size())
  {
    return std::nullopt;
  }
  return result;
}

std::optional<mdl::IdType> persistentIdOf(const mdl::Node& node)
{
  if (const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(&node))
  {
    return layerNode->persistentId();
  }
  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    return groupNode->persistentId();
  }
  return std::nullopt;
}

const std::string* linkIdOf(const mdl::Node& node)
{
  if (const auto* object = dynamic_cast<const mdl::Object*>(&node))
  {
    return &object->linkId();
  }
  return nullptr;
}

} // namespace

std::string_view toString(const ObjectKind kind)
{
  for (const auto& [k, name] : ObjectKindNames)
  {
    if (k == kind)
    {
      return name;
    }
  }
  contract_assert(false);
  return {};
}

std::optional<ObjectKind> objectKindFromString(const std::string_view str)
{
  for (const auto& [k, name] : ObjectKindNames)
  {
    if (name == str)
    {
      return k;
    }
  }
  return std::nullopt;
}

ObjectKind objectKindOf(const mdl::Node& node)
{
  return node.accept(kdl::overload(
    [](const mdl::WorldNode&) { return ObjectKind::World; },
    [](const mdl::LayerNode&) { return ObjectKind::Layer; },
    [](const mdl::GroupNode&) { return ObjectKind::Group; },
    [](const mdl::EntityNode&) { return ObjectKind::Entity; },
    [](const mdl::BrushNode&) { return ObjectKind::Brush; },
    [](const mdl::PatchNode&) { return ObjectKind::Patch; }));
}

std::optional<ObjectRef> parseObjectRef(std::string_view id)
{
  auto faceIndex = std::optional<size_t>{};
  if (const auto slash = id.find('/'); slash != std::string_view::npos)
  {
    const auto facePart = id.substr(slash + 1);
    if (!facePart.starts_with("face:"))
    {
      return std::nullopt;
    }
    faceIndex = parseNumber(facePart.substr(5));
    if (!faceIndex)
    {
      return std::nullopt;
    }
    id = id.substr(0, slash);
  }

  if (id == "world")
  {
    return faceIndex ? std::nullopt : std::optional{ObjectRef{ObjectKind::World}};
  }
  if (id == "layer:default")
  {
    return faceIndex ? std::nullopt
                     : std::optional{ObjectRef{ObjectKind::Layer, std::nullopt, true}};
  }

  const auto colon = id.find(':');
  if (colon == std::string_view::npos)
  {
    return std::nullopt;
  }

  const auto kind = objectKindFromString(id.substr(0, colon));
  const auto runtimeId = parseNumber(id.substr(colon + 1));
  if (!kind || !runtimeId || *kind == ObjectKind::World)
  {
    return std::nullopt;
  }
  if (faceIndex && *kind != ObjectKind::Brush)
  {
    return std::nullopt;
  }

  return ObjectRef{*kind, *runtimeId, false, faceIndex};
}

std::string formatObjectRef(const ObjectRef& ref)
{
  auto result = std::string{};
  if (ref.kind == ObjectKind::World)
  {
    result = "world";
  }
  else if (ref.isDefaultLayer)
  {
    result = "layer:default";
  }
  else
  {
    result =
      std::string{toString(ref.kind)} + ":" + std::to_string(ref.runtimeId.value_or(0));
  }

  if (ref.faceIndex)
  {
    result += "/face:" + std::to_string(*ref.faceIndex);
  }
  return result;
}

IdRegistry::IdRegistry(ui::MapDocument& document)
  : m_document{document}
{
  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect(this, &IdRegistry::documentWasLoaded);
  m_notifierConnection +=
    m_document.nodesWereAddedNotifier.connect(this, &IdRegistry::nodesWereAdded);
  m_notifierConnection +=
    m_document.nodesWillBeRemovedNotifier.connect(this, &IdRegistry::nodesWillBeRemoved);
  m_notifierConnection +=
    m_document.nodesWereRemovedNotifier.connect(this, &IdRegistry::nodesWereRemoved);

  rebuild();
}

IdRegistry::~IdRegistry() = default;

std::string IdRegistry::format(const mdl::Node& node) const
{
  const auto kind = objectKindOf(node);
  if (kind == ObjectKind::World)
  {
    return "world";
  }
  if (
    kind == ObjectKind::Layer
    && &node
         == static_cast<const mdl::Node*>(m_document.map().worldNode().defaultLayer()))
  {
    return "layer:default";
  }
  return formatObjectRef(ObjectRef{kind, canonicalId(node)});
}

std::string IdRegistry::formatFace(
  const mdl::BrushNode& brushNode, const size_t faceIndex) const
{
  return formatObjectRef(
    ObjectRef{ObjectKind::Brush, canonicalId(brushNode), false, faceIndex});
}

Result<mdl::Node*, ToolError> IdRegistry::resolve(const std::string_view id) const
{
  if (const auto ref = parseObjectRef(id))
  {
    return resolve(*ref);
  }

  return makeError(
    ErrorCode::InvalidArgument,
    "'" + std::string{id} + "' is not a valid object id.",
    "Object ids look like 'brush:1042', 'entity:7', 'layer:default', 'world' or "
    "'brush:1042/face:3'. Use ids returned by other tools.");
}

Result<mdl::Node*, ToolError> IdRegistry::resolve(const ObjectRef& ref) const
{
  auto& worldNode = m_document.map().worldNode();
  if (ref.kind == ObjectKind::World)
  {
    return &worldNode;
  }
  if (ref.isDefaultLayer)
  {
    return worldNode.defaultLayer();
  }

  const auto id = formatObjectRef(ObjectRef{ref.kind, ref.runtimeId});
  const auto runtimeId = *ref.runtimeId;

  auto* node = findCurrent(runtimeId);
  if (!node)
  {
    // layers and groups survive reloads through their persistent ids
    if (const auto it = m_persistentIds.find(runtimeId); it != m_persistentIds.end())
    {
      const auto& [kind, persistentId] = it->second;
      for (const auto& [otherRuntimeId, otherNode] : m_nodes)
      {
        if (
          objectKindOf(*otherNode) == kind && persistentIdOf(*otherNode) == persistentId)
        {
          node = otherNode;
          break;
        }
      }
    }
  }

  if (!node)
  {
    if (runtimeId < m_reloadThreshold)
    {
      return makeError(
        ErrorCode::ObjectNotFound,
        "Object " + id + " does not exist because the document was reloaded.",
        "Object ids do not survive reloading a document. Query the map again to get "
        "the current ids.",
        {id});
    }
    return makeError(
      ErrorCode::ObjectNotFound,
      "Object " + id + " does not exist.",
      "It may have been deleted; 'undo' may restore it. Query the map again to get the "
      "current ids.",
      {id});
  }

  const auto actualKind = objectKindOf(*node);
  if (actualKind != ref.kind)
  {
    return makeError(
      ErrorCode::WrongObjectKind,
      "Object " + id + " is a " + std::string{toString(actualKind)} + ", not a "
        + std::string{toString(ref.kind)} + ".",
      "Use the id " + format(*node) + ".",
      {id});
  }

  if (ref.faceIndex)
  {
    const auto* brushNode = static_cast<const mdl::BrushNode*>(node);
    if (*ref.faceIndex >= brushNode->brush().faceCount())
    {
      return makeError(
        ErrorCode::ObjectNotFound,
        "Brush " + id + " has no face " + std::to_string(*ref.faceIndex) + " (it has "
          + std::to_string(brushNode->brush().faceCount()) + " faces).",
        "Face indices change when a brush's geometry changes. Read the brush again to "
        "get its current faces.",
        {formatObjectRef(ref)});
    }
  }

  return node;
}

mdl::Node* IdRegistry::findByRuntimeId(const mdl::IdType runtimeId) const
{
  const auto it = m_nodes.find(runtimeId);
  return it != m_nodes.end() ? it->second : nullptr;
}

size_t IdRegistry::size() const
{
  return m_nodes.size();
}

mdl::IdType IdRegistry::canonicalId(const mdl::Node& node) const
{
  const auto it = m_canonicalIds.find(node.runtimeId());
  return it != m_canonicalIds.end() ? it->second : node.runtimeId();
}

mdl::Node* IdRegistry::findCurrent(const mdl::IdType canonicalId) const
{
  if (auto* node = findByRuntimeId(canonicalId))
  {
    return node;
  }
  if (const auto it = m_currentIds.find(canonicalId); it != m_currentIds.end())
  {
    return findByRuntimeId(it->second);
  }
  return nullptr;
}

void IdRegistry::rebuild()
{
  for (const auto& [runtimeId, node] : m_nodes)
  {
    m_reloadThreshold = std::max(m_reloadThreshold, runtimeId + 1);
  }

  m_map = &m_document.map();
  m_nodes.clear();
  m_canonicalIds.clear();
  m_currentIds.clear();
  m_recentlyRemoved.clear();
  registerRecursively(m_document.map().worldNode());
}

void IdRegistry::registerRecursively(mdl::Node& node)
{
  m_nodes[node.runtimeId()] = &node;
  if (const auto persistentId = persistentIdOf(node))
  {
    m_persistentIds[node.runtimeId()] = {objectKindOf(node), *persistentId};
  }

  for (auto* child : node.children())
  {
    registerRecursively(*child);
  }
}

void IdRegistry::unregisterRecursively(const mdl::Node& node)
{
  m_nodes.erase(node.runtimeId());
  for (const auto* child : node.children())
  {
    unregisterRecursively(*child);
  }
}

void IdRegistry::collectLinkKeys(
  const mdl::Node& node,
  const mdl::Node* anchor,
  std::vector<std::string> path,
  std::vector<std::tuple<LinkKey, const mdl::Node*>>& result) const
{
  const auto* linkId = linkIdOf(node);
  if (!linkId || linkId->empty())
  {
    return;
  }

  path.push_back(*linkId);
  result.emplace_back(LinkKey{anchor, path}, &node);
  for (const auto* child : node.children())
  {
    collectLinkKeys(*child, anchor, path, result);
  }
}

void IdRegistry::documentWasLoaded()
{
  // A new map window (document_show) notifies without replacing the map; the ids stay
  if (&m_document.map() != m_map)
  {
    rebuild();
  }
}

void IdRegistry::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  for (auto* node : nodes)
  {
    registerRecursively(*node);
  }

  if (!m_recentlyRemoved.empty())
  {
    for (const auto* node : nodes)
    {
      const auto* parent = node->parent();
      if (!dynamic_cast<const mdl::GroupNode*>(parent))
      {
        continue;
      }

      auto keys = std::vector<std::tuple<LinkKey, const mdl::Node*>>{};
      collectLinkKeys(*node, parent, {}, keys);
      for (const auto& [key, addedNode] : keys)
      {
        const auto it = std::ranges::find_if(m_recentlyRemoved, [&](const auto& entry) {
          return std::get<0>(entry) == key;
        });
        if (it != m_recentlyRemoved.end())
        {
          const auto canonical = std::get<1>(*it);
          if (canonical != addedNode->runtimeId())
          {
            m_canonicalIds[addedNode->runtimeId()] = canonical;
            m_currentIds[canonical] = addedNode->runtimeId();
          }
        }
      }
    }
    m_recentlyRemoved.clear();
  }
}

void IdRegistry::nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes)
{
  m_recentlyRemoved.clear();
  for (const auto* node : nodes)
  {
    const auto* parent = node->parent();
    if (!dynamic_cast<const mdl::GroupNode*>(parent))
    {
      continue;
    }

    auto keys = std::vector<std::tuple<LinkKey, const mdl::Node*>>{};
    collectLinkKeys(*node, parent, {}, keys);
    for (const auto& [key, removedNode] : keys)
    {
      m_recentlyRemoved.emplace_back(key, canonicalId(*removedNode));
    }
  }
}

void IdRegistry::nodesWereRemoved(const std::vector<mdl::Node*>& nodes)
{
  for (const auto* node : nodes)
  {
    unregisterRecursively(*node);
  }
}

} // namespace tb::mcp
