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

#include "mcp/Targets.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/NodeQueries.h"
#include "mdl/Selection.h"

#include <algorithm>

namespace tb::mcp
{
namespace
{

bool isObject(const ObjectKind kind)
{
  return kind != ObjectKind::World && kind != ObjectKind::Layer;
}

ToolResult withSelection(
  CallContext& context,
  const bool alreadySelected,
  const std::function<void()>& select,
  const std::function<ToolResult()>& function,
  const SelectionAfter after)
{
  auto& map = context.map();
  auto& ids = context.ids();

  // remember the selection by id, since nodes may be removed by the operation
  auto savedNodes = std::vector<std::string>{};
  auto savedFaces = std::vector<ObjectRef>{};
  const auto& selection = map.selection();
  for (const auto* node : selection.nodes)
  {
    savedNodes.push_back(ids.format(*node));
  }
  for (const auto& handle : selection.brushFaces)
  {
    if (auto ref = parseObjectRef(ids.formatFace(*handle.node(), handle.faceIndex())))
    {
      savedFaces.push_back(*ref);
    }
  }

  if (!alreadySelected)
  {
    select();
  }

  auto result = function();

  if (after == SelectionAfter::Restore && !alreadySelected)
  {
    mdl::deselectAll(map);

    auto nodes = std::vector<mdl::Node*>{};
    for (const auto& id : savedNodes)
    {
      if (auto node = ids.resolve(id); node.is_success())
      {
        nodes.push_back(node.value());
      }
    }
    if (!nodes.empty())
    {
      mdl::selectNodes(map, nodes);
    }

    auto faces = std::vector<mdl::BrushFaceHandle>{};
    for (const auto& ref : savedFaces)
    {
      if (auto node = ids.resolve(ref); node.is_success())
      {
        faces.emplace_back(static_cast<mdl::BrushNode*>(node.value()), *ref.faceIndex);
      }
    }
    if (!faces.empty())
    {
      mdl::selectBrushFaces(map, faces);
    }
  }

  return result;
}

/**
 * The members that stand for a brush entity when it is passed to a tool accepting the
 * given kinds: its brushes and patches (those of the accepted kinds unless entities are
 * accepted). Empty for other nodes, or if the tool accepts neither entities nor any kind
 * of member.
 */
std::vector<mdl::Node*> brushEntityMembers(
  const mdl::Node& node, const std::vector<ObjectKind>& kinds)
{
  if (objectKindOf(node) != ObjectKind::Entity || !node.hasChildren())
  {
    return {};
  }

  const auto accepts = [&](const ObjectKind kind) {
    return kinds.empty() || std::ranges::find(kinds, kind) != kinds.end();
  };
  const auto acceptsAllMembers = accepts(ObjectKind::Entity);

  auto members = std::vector<mdl::Node*>{};
  for (auto* child : node.children())
  {
    if (acceptsAllMembers || accepts(objectKindOf(*child)))
    {
      members.push_back(child);
    }
  }
  return members;
}

} // namespace

schema::Field idsField(std::vector<ObjectKind> kinds, std::string description)
{
  using namespace schema;

  // brush entity ids stand for their brushes (see resolveTargets)
  const auto accepts = [&](const ObjectKind kind) {
    return std::ranges::find(kinds, kind) != kinds.end();
  };
  if (
    (accepts(ObjectKind::Brush) || accepts(ObjectKind::Patch))
    && !accepts(ObjectKind::Entity))
  {
    kinds.push_back(ObjectKind::Entity);
    description +=
      ". Brush entity ids stand for their "
      + std::string{accepts(ObjectKind::Patch) ? "brushes and patches" : "brushes"};
  }

  return field("ids", array(objectId(std::move(kinds))).nonEmpty())
    .describe(std::move(description));
}

Result<std::vector<mdl::Node*>, ToolError> resolveTargets(
  CallContext& context,
  const Args& args,
  const std::string_view key,
  const std::vector<ObjectKind>& kinds)
{
  auto& map = context.map();
  auto& ids = context.ids();

  auto nodes = std::vector<mdl::Node*>{};
  const auto addNode = [&](mdl::Node* node) {
    if (std::ranges::find(nodes, node) == nodes.end())
    {
      nodes.push_back(node);
    }
  };

  if (const auto explicitIds = args.getOptional<std::vector<std::string>>(key))
  {
    for (const auto& id : *explicitIds)
    {
      auto node = ids.resolve(id);
      if (node.is_error())
      {
        return errorOf(node);
      }

      // a brush entity is selected through its brushes and patches, as in the editor
      if (const auto members = brushEntityMembers(*node.value(), kinds); !members.empty())
      {
        for (auto* member : members)
        {
          if (!map.editorContext().selectable(*member))
          {
            return makeError(
              ErrorCode::ObjectNotEditable,
              "Brush entity " + id + " cannot be edited: its " + ids.format(*member)
                + " is hidden, locked, or inside a closed group.",
              "Show or unlock its layer (layer_set_state), or open its group "
              "(group_open).",
              {id});
          }
          addNode(member);
        }
        continue;
      }

      addNode(node.value());
    }
  }
  else
  {
    nodes = map.selection().nodes;
    if (nodes.empty())
    {
      return makeError(
        ErrorCode::NoSelection,
        "No " + std::string{key} + " were given and nothing is selected.",
        "Pass '" + std::string{key} + "' explicitly, or select objects first.");
    }
  }

  for (auto* node : nodes)
  {
    const auto kind = objectKindOf(*node);
    if (!kinds.empty() && std::ranges::find(kinds, kind) == kinds.end())
    {
      auto expected = std::string{};
      for (const auto k : kinds)
      {
        expected += (expected.empty() ? "" : " or ") + std::string{toString(k)};
      }
      return makeError(
        ErrorCode::WrongObjectKind,
        "Object " + ids.format(*node) + " is a " + std::string{toString(kind)}
          + "; this tool expects " + expected + " objects.",
        "Pass only " + expected + " ids.",
        {ids.format(*node)});
    }

    if (isObject(kind) && !map.editorContext().selectable(*node))
    {
      return makeError(
        ErrorCode::ObjectNotEditable,
        "Object " + ids.format(*node)
          + " cannot be edited: it is hidden, locked, or inside a closed group.",
        "Show or unlock its layer (layer_set_state), or open its group (group_open).",
        {ids.format(*node)});
    }
  }

  return nodes;
}

Result<mdl::BrushFaceHandle, ToolError> resolveFace(
  CallContext& context, const std::string_view id)
{
  const auto ref = parseObjectRef(id);
  if (!ref || !ref->faceIndex)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'" + std::string{id} + "' is not a valid face id.",
      "Face ids look like 'brush:1042/face:3'.");
  }

  return context.ids().resolve(*ref) | kdl::transform([&](mdl::Node* node) {
           return mdl::BrushFaceHandle{
             static_cast<mdl::BrushNode*>(node), *ref->faceIndex};
         });
}

schema::Field faceTargetsField(std::string description)
{
  using namespace schema;
  return field(
           "ids",
           array(objectId({ObjectKind::Brush, ObjectKind::Group, ObjectKind::Entity}))
             .nonEmpty())
    .describe(std::move(description));
}

Result<std::vector<mdl::BrushFaceHandle>, ToolError> resolveFaceTargets(
  CallContext& context, const Args& args, const std::string_view key)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& editorContext = map.editorContext();

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  const auto addFace = [&](const mdl::BrushFaceHandle& handle) {
    if (std::ranges::find(faces, handle) == faces.end())
    {
      faces.push_back(handle);
    }
  };

  if (const auto explicitIds = args.getOptional<std::vector<std::string>>(key))
  {
    for (const auto& id : *explicitIds)
    {
      const auto ref = parseObjectRef(id);
      if (!ref)
      {
        return makeError(ErrorCode::InvalidArgument, "'" + id + "' is not a valid id.");
      }

      auto node = ids.resolve(*ref);
      if (node.is_error())
      {
        return errorOf(node);
      }

      if (ref->faceIndex)
      {
        const auto handle = mdl::BrushFaceHandle{
          static_cast<mdl::BrushNode*>(node.value()), *ref->faceIndex};
        if (!editorContext.selectable(*handle.node(), handle.face()))
        {
          return makeError(
            ErrorCode::ObjectNotEditable,
            "Face " + id
              + " cannot be edited: its brush is hidden, locked, or inside a closed "
                "group.",
            "Show or unlock its layer (layer_set_state), or open its group "
            "(group_open).",
            {id});
        }
        addFace(handle);
      }
      else
      {
        const auto nodeFaces = mdl::collectBrushFaces(
          std::vector{node.value()}, [&](const auto& brushNode, const auto& face) {
            return editorContext.selectable(brushNode, face);
          });
        if (nodeFaces.empty())
        {
          return makeError(
            ErrorCode::ObjectNotEditable,
            "Object " + id + " has no editable brush faces.",
            "Pass brushes, faces, or groups and brush entities that contain visible, "
            "unlocked brushes.",
            {id});
        }
        for (const auto& handle : nodeFaces)
        {
          addFace(handle);
        }
      }
    }
    return faces;
  }

  const auto& selection = map.selection();
  faces = selection.hasBrushFaces() ? selection.brushFaces : selection.allBrushFaces();
  if (faces.empty())
  {
    return makeError(
      ErrorCode::NoSelection,
      "No " + std::string{key} + " were given and no brush faces are selected.",
      "Pass face or brush ids in '" + std::string{key}
        + "', or select brushes or faces first.");
  }
  return faces;
}

ToolResult withTargets(
  CallContext& context,
  const std::vector<mdl::Node*>& targets,
  const std::function<ToolResult()>& function,
  const SelectionAfter after)
{
  auto& map = context.map();
  const auto& selection = map.selection();
  const auto alreadySelected =
    std::ranges::is_permutation(selection.nodes, targets) && selection.brushFaces.empty();
  return withSelection(
    context,
    alreadySelected,
    [&]() {
      mdl::deselectAll(map);
      mdl::selectNodes(map, targets);
    },
    function,
    after);
}

ToolResult withFaces(
  CallContext& context,
  const std::vector<mdl::BrushFaceHandle>& faces,
  const std::function<ToolResult()>& function)
{
  auto& map = context.map();
  const auto& selection = map.selection();
  const auto alreadySelected =
    selection.nodes.empty() && std::ranges::is_permutation(selection.brushFaces, faces);
  return withSelection(
    context,
    alreadySelected,
    [&]() {
      mdl::deselectAll(map);
      mdl::selectBrushFaces(map, faces);
    },
    function,
    SelectionAfter::Restore);
}

} // namespace tb::mcp
