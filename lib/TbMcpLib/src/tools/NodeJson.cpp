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

#include "NodeJson.h"

#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/ModelUtils.h"
#include "mdl/PatchNode.h"
#include "mdl/SurfaceAttributes.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"

#include <algorithm>

namespace tb::mcp
{

std::string nodeLabel(const mdl::Node& node)
{
  if (dynamic_cast<const mdl::WorldNode*>(&node))
  {
    return "worldspawn";
  }
  if (const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(&node))
  {
    return layerNode->layer().name();
  }
  if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    return groupNode->group().name();
  }
  if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node))
  {
    return entityNode->entity().classname();
  }
  if (dynamic_cast<const mdl::PatchNode*>(&node))
  {
    return "patch";
  }
  return "brush";
}

std::vector<std::string> nodeMaterials(const mdl::Node& node)
{
  auto result = std::vector<std::string>{};
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    for (const auto& face : brushNode->brush().faces())
    {
      result.push_back(face.materialName());
    }
  }
  else if (const auto* patchNode = dynamic_cast<const mdl::PatchNode*>(&node))
  {
    result.push_back(patchNode->patch().materialName());
  }
  std::ranges::sort(result);
  const auto [first, last] = std::ranges::unique(result);
  result.erase(first, last);
  return result;
}

namespace
{

template <typename T>
std::vector<std::string> tagNames(const mdl::Map& map, const T& taggable)
{
  auto result = std::vector<std::string>{};
  for (const auto& tag : map.tagManager().smartTags())
  {
    if (taggable.hasTag(tag))
    {
      result.push_back(tag.name());
    }
  }
  return result;
}

} // namespace

std::vector<std::string> nodeTagNames(const mdl::Map& map, const mdl::Node& node)
{
  return tagNames(map, node);
}

std::vector<std::string> faceTagNames(const mdl::Map& map, const mdl::BrushFace& face)
{
  return tagNames(map, face);
}

Json layerIdOf(const mdl::Node& node, const IdRegistry& ids)
{
  const auto* layerNode = mdl::findContainingLayer(const_cast<mdl::Node*>(&node));
  return layerNode ? Json(ids.format(*layerNode)) : Json(nullptr);
}

Json groupIdOf(const mdl::Node& node, const IdRegistry& ids)
{
  const auto* groupNode = mdl::findContainingGroup(&node);
  return groupNode ? Json(ids.format(*groupNode)) : Json(nullptr);
}

Json nodeSummary(const mdl::Node& node, const IdRegistry& ids)
{
  const auto kind = objectKindOf(node);
  auto result = Json{
    {"id", ids.format(node)},
    {"kind", std::string{toString(kind)}},
    {"label", nodeLabel(node)},
  };

  switch (kind)
  {
  case ObjectKind::Entity:
    result["classname"] = nodeLabel(node);
    break;
  case ObjectKind::Layer:
  case ObjectKind::Group:
    result["name"] = nodeLabel(node);
    break;
  case ObjectKind::Brush:
  case ObjectKind::Patch:
    result["materials"] = nodeMaterials(node);
    if (
      const auto* entityNode =
        dynamic_cast<const mdl::EntityNode*>(mdl::findContainingEntity(&node)))
    {
      result["entity"] = ids.format(*entityNode);
    }
    break;
  case ObjectKind::World:
    break;
  }

  if (kind != ObjectKind::World)
  {
    result["bounds"] = toJson(node.logicalBounds());
  }
  if (kind != ObjectKind::World && kind != ObjectKind::Layer)
  {
    result["layer"] = layerIdOf(node, ids);
  }
  return result;
}

Json nodeState(const mdl::Map& map, const mdl::Node& node)
{
  const auto& editorContext = map.editorContext();
  return Json{
    {"visible", editorContext.visible(node)},
    {"hidden", node.hidden()},
    {"locked", node.locked()},
    {"selected", node.selected()},
    {"selectable", editorContext.selectable(node)},
  };
}

Json faceJson(
  const mdl::Map& map,
  const mdl::BrushNode& brushNode,
  const size_t faceIndex,
  const IdRegistry& ids,
  const Detail detail)
{
  const auto& face = brushNode.brush().face(faceIndex);
  const auto uv = face.uvAttributes();
  const auto& surface = face.surfaceAttributes();

  auto result = Json{
    {"id", ids.formatFace(brushNode, faceIndex)},
    {"index", faceIndex},
    {"normal", toJson(face.normal())},
    {"center", toJson(face.center())},
    {"material", face.materialName()},
  };

  if (detail == Detail::Full)
  {
    result["offset"] = toJson(vm::vec2d{uv.offset});
    result["scale"] = toJson(vm::vec2d{uv.scale});
    result["rotation"] = roundForOutput(double(uv.rotation));
    result["area"] = roundForOutput(face.area());
    result["tags"] = faceTagNames(map, face);
    if (const auto contents = surface.contents)
    {
      result["surfaceContents"] = *contents;
    }
    if (const auto flags = surface.flags)
    {
      result["surfaceFlags"] = *flags;
    }
    if (const auto value = surface.value)
    {
      result["surfaceValue"] = roundForOutput(double(*value));
    }

    auto vertices = Json::array();
    for (const auto& position : face.vertexPositions())
    {
      vertices.push_back(toJson(position));
    }
    result["vertices"] = std::move(vertices);
  }
  return result;
}

} // namespace tb::mcp
