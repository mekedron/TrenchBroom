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

#include "mcp/tools/GroupTools.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/Object.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/flat_map.h"
#include "kd/vector_utils.h"

#include "vm/vec.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr double Epsilon = 1e-9;

const auto GroupableKinds = std::vector<ObjectKind>{
  ObjectKind::Group, ObjectKind::Entity, ObjectKind::Brush, ObjectKind::Patch};

// Shared helpers

void addUnique(std::vector<mdl::Node*>& nodes, mdl::Node* node)
{
  if (std::ranges::find(nodes, node) == nodes.end())
  {
    nodes.push_back(node);
  }
}

ToolError notEditableError(const std::string& id, std::string what = "Object")
{
  return makeError(
    ErrorCode::ObjectNotEditable,
    what + " " + id + " cannot be edited: it is hidden or locked.",
    "Show or unlock its layer (layer_set_state), or close the open group "
    "(group_close).",
    {id});
}

/** Whether the node is visible and not locked (it may be inside a closed group). */
bool isEditable(const mdl::Map& map, const mdl::Node& node)
{
  const auto& editorContext = map.editorContext();
  return editorContext.visible(node) && editorContext.editable(node);
}

/** The number of groups in the link set of the given group, including itself. */
size_t linkSetSize(const mdl::Map& map, const mdl::GroupNode& groupNode)
{
  return mdl::collectGroupsWithLinkId(
           {const_cast<mdl::WorldNode*>(&map.worldNode())}, groupNode.linkId())
    .size();
}

/** The groups in the link set of the given group, including itself. */
std::vector<mdl::GroupNode*> linkSet(mdl::Map& map, const mdl::GroupNode& groupNode)
{
  return mdl::collectGroupsWithLinkId({&map.worldNode()}, groupNode.linkId());
}

/**
 * Resolves a group argument. With `selectable`, the group must be selectable (visible,
 * unlocked, closed and not inside a closed group); otherwise it must be visible and
 * unlocked.
 */
Result<mdl::GroupNode*, ToolError> resolveGroup(
  CallContext& context, const std::string& id, const bool selectable)
{
  auto node = context.ids().resolve(id);
  if (node.is_error())
  {
    return errorOf(node);
  }

  auto* groupNode = dynamic_cast<mdl::GroupNode*>(node.value());
  if (!groupNode)
  {
    return makeError(
      ErrorCode::WrongObjectKind,
      "Object " + id + " is not a group.",
      "Pass a group id such as 'group:12'.",
      {id});
  }

  const auto& map = context.map();
  if (
    selectable ? !map.editorContext().selectable(*groupNode)
               : !isEditable(map, *groupNode))
  {
    return makeError(
      ErrorCode::ObjectNotEditable,
      "Group " + id
        + (selectable ? " cannot be edited: it is hidden, locked, open, or inside a "
                        "closed group."
                      : " cannot be edited: it is hidden or locked."),
      "Show or unlock its layer (layer_set_state), close it (group_close), or open the "
      "group that contains it (group_open).",
      {id});
  }

  return groupNode;
}

/**
 * Resolves the objects of tools that act on the members of a group: explicit ids may
 * name objects inside closed groups, but they must be visible and unlocked. Without ids,
 * the current selection is used.
 */
Result<std::vector<mdl::Node*>, ToolError> resolveMembers(
  CallContext& context, const Args& args)
{
  const auto explicitIds = args.getOptional<std::vector<std::string>>("ids");
  if (!explicitIds)
  {
    return resolveTargets(context, args, "ids", GroupableKinds);
  }

  auto nodes = std::vector<mdl::Node*>{};
  for (const auto& id : *explicitIds)
  {
    auto node = context.ids().resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    if (
      std::ranges::find(GroupableKinds, objectKindOf(*node.value()))
      == GroupableKinds.end())
    {
      return makeError(
        ErrorCode::WrongObjectKind,
        "Object " + id + " is not a group, entity, brush or patch.",
        "Pass only object ids.",
        {id});
    }
    if (!isEditable(context.map(), *node.value()))
    {
      return notEditableError(id);
    }
    addUnique(nodes, node.value());
  }
  return nodes;
}

/**
 * Replaces the brushes and patches of brush entities by their entities, like the
 * editor's "Add Objects to Group" and "Remove Objects from Group" do.
 */
std::vector<mdl::Node*> preferEntities(
  const mdl::Map& map, const std::vector<mdl::Node*>& nodes)
{
  auto result = std::vector<mdl::Node*>{};
  for (auto* node : nodes)
  {
    auto* entity = mdl::findContainingEntity(node);
    if (entity && entity != &map.worldNode())
    {
      addUnique(result, entity);
    }
    else
    {
      addUnique(result, node);
    }
  }
  return result;
}

/** The open groups, outermost first. */
std::vector<mdl::GroupNode*> openGroups(const mdl::Map& map)
{
  auto result = std::vector<mdl::GroupNode*>{};
  for (auto* groupNode = map.editorContext().currentGroup(); groupNode;
       groupNode = groupNode->containingGroup())
  {
    result.insert(result.begin(), groupNode);
  }
  return result;
}

/** `{"openGroup": id | null, "openGroups": [outermost, ..., innermost]}` */
Json groupContextJson(const mdl::Map& map, const IdRegistry& ids)
{
  const auto* currentGroup = map.editorContext().currentGroup();
  auto open = Json::array();
  for (const auto* groupNode : openGroups(map))
  {
    open.push_back(ids.format(*groupNode));
  }
  return Json{
    {"openGroup", currentGroup ? Json(ids.format(*currentGroup)) : Json(nullptr)},
    {"openGroups", std::move(open)},
  };
}

/** Closes open groups until the given node is the current group or inside it. */
void closeGroupsOutside(mdl::Map& map, const mdl::Node& node)
{
  for (auto* current = map.editorContext().currentGroup();
       current && current != &node && !current->isAncestorOf(node);
       current = map.editorContext().currentGroup())
  {
    mdl::closeGroup(map);
  }
}

/**
 * Makes the given group the current group, like double-clicking it in the editor:
 * closes the open groups that do not contain it, then opens the closed groups from the
 * outermost one down to it.
 */
std::optional<ToolError> enterGroup(
  mdl::Map& map, const IdRegistry& ids, mdl::GroupNode& groupNode)
{
  closeGroupsOutside(map, groupNode);

  auto chain = std::vector<mdl::GroupNode*>{};
  for (auto* current = &groupNode;
       current && current != map.editorContext().currentGroup();
       current = current->containingGroup())
  {
    chain.insert(chain.begin(), current);
  }

  for (auto* current : chain)
  {
    if (!isEditable(map, *current))
    {
      return notEditableError(ids.format(*current), "Group");
    }
    mdl::openGroup(map, *current);
  }
  return std::nullopt;
}

/** Selects those of the given nodes that can be selected. */
void selectSelectable(mdl::Map& map, const std::vector<mdl::Node*>& nodes)
{
  auto selectable = std::vector<mdl::Node*>{};
  std::ranges::copy_if(nodes, std::back_inserter(selectable), [&](const auto* node) {
    return map.editorContext().selectable(*node);
  });
  mdl::deselectAll(map);
  if (!selectable.empty())
  {
    mdl::selectNodes(map, selectable);
  }
}

std::vector<std::string> groupIds(
  const std::vector<mdl::GroupNode*>& groupNodes, const IdRegistry& ids)
{
  auto result = std::vector<std::string>{};
  for (const auto* groupNode : groupNodes)
  {
    result.push_back(ids.format(*groupNode));
  }
  return result;
}

// group_create

ToolResult groupCreate(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", GroupableKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto name = args.get<std::string>("name");

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      auto* groupNode = mdl::groupSelectedNodes(map, name);
      if (!groupNode)
      {
        return context.operationFailed(
          "The objects could not be grouped.",
          "Check that the objects are not part of a linked group that would conflict.");
      }
      return Json{
        {"group", ids.format(*groupNode)},
        {"objects", formatIds(groupNode->children(), ids)},
      };
    },
    SelectionAfter::Result);
}

// group_ungroup

ToolResult groupUngroup(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Group});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto groups = formatIds(targets.value(), ids);
  auto children = std::vector<mdl::Node*>{};
  for (const auto* node : targets.value())
  {
    kdl::vec_append(children, node->children());
  }

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      mdl::ungroupSelectedNodes(map);
      if (std::ranges::any_of(
            groups, [&](const auto& id) { return ids.resolve(id).is_success(); }))
      {
        return context.operationFailed(
          "The groups could not be ungrouped.", "Check the editor messages.");
      }
      return Json{{"groups", groups}, {"objects", formatIds(children, ids)}};
    },
    SelectionAfter::Result);
}

// group_rename

ToolResult groupRename(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Group});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto name = args.get<std::string>("name");

  return withTargets(context, targets.value(), [&]() -> ToolResult {
    mdl::renameSelectedGroups(map, name);
    if (!std::ranges::all_of(
          targets.value(), [&](const auto* node) { return node->name() == name; }))
    {
      return context.operationFailed(
        "The groups could not be renamed.", "Check the editor messages.");
    }
    return Json{{"groups", formatIds(targets.value(), ids)}, {"name", name}};
  });
}

// groups_merge

ToolResult groupsMerge(CallContext& context, const Args& args)
{
  auto target = resolveGroup(context, args.get<std::string>("target"), true);
  if (target.is_error())
  {
    return errorOf(target);
  }
  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Group});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  auto* targetGroup = target.value();
  const auto targetId = ids.format(*targetGroup);

  auto sources = std::vector<mdl::Node*>{};
  auto objects = std::vector<mdl::Node*>{};
  for (auto* node : targets.value())
  {
    if (node == targetGroup)
    {
      continue;
    }
    if (node->isAncestorOf(*targetGroup))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Group " + ids.format(*node) + " contains the target group " + targetId + ".",
        "Pass a target group that is not inside one of the groups to merge.",
        {ids.format(*node), targetId});
    }
    sources.push_back(node);
    kdl::vec_append(objects, node->children());
  }

  if (sources.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "There are no groups to merge into " + targetId + ".",
      "Pass the groups to merge in 'ids' (the target may be among them).",
      {targetId});
  }

  const auto sourceIds = formatIds(sources, ids);
  auto selection = sources;
  selection.push_back(targetGroup);

  return withTargets(
    context,
    selection,
    [&]() -> ToolResult {
      mdl::mergeSelectedGroupsWithGroup(map, targetGroup);
      if (!std::ranges::all_of(
            objects, [&](const auto* node) { return node->parent() == targetGroup; }))
      {
        return context.operationFailed(
          "The groups could not be merged.",
          "Check that the groups are not linked in a way that would conflict.");
      }
      return Json{
        {"group", targetId},
        {"merged", sourceIds},
        {"objects", formatIds(objects, ids)},
      };
    },
    SelectionAfter::Result);
}

// group_add_objects

ToolResult groupAddObjects(CallContext& context, const Args& args)
{
  auto group = resolveGroup(context, args.get<std::string>("group"), false);
  if (group.is_error())
  {
    return errorOf(group);
  }
  auto targets = resolveTargets(context, args, "ids", GroupableKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  auto* groupNode = group.value();
  const auto groupId = ids.format(*groupNode);

  auto nodes = std::vector<mdl::Node*>{};
  auto alreadyInGroup = std::vector<mdl::Node*>{};
  for (auto* node : preferEntities(map, targets.value()))
  {
    if (node == groupNode || node->isAncestorOf(*groupNode))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Object " + ids.format(*node) + " is or contains the group " + groupId + ".",
        "A group cannot be added to itself or to one of its descendants.",
        {ids.format(*node), groupId});
    }
    if (node->parent() == groupNode)
    {
      alreadyInGroup.push_back(node);
    }
    else
    {
      nodes.push_back(node);
    }
  }

  if (nodes.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "All objects are already in group " + groupId + ".",
      "Pass objects that are not yet in the group.",
      formatIds(alreadyInGroup, ids));
  }

  mdl::deselectAll(map);
  if (!mdl::reparentNodes(map, {{groupNode, nodes}}))
  {
    return context.operationFailed(
      "The objects could not be added to group " + groupId + ".",
      "Check that adding them does not conflict with linked groups.");
  }
  selectSelectable(map, {groupNode});

  return Json{{"group", groupId}, {"objects", formatIds(nodes, ids)}};
}

// group_remove_objects

ToolResult groupRemoveObjects(CallContext& context, const Args& args)
{
  auto targets = resolveMembers(context, args);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto nodes = preferEntities(map, targets.value());

  auto moved = Json::array();
  auto nodesToMove = kdl::flat_map<mdl::Node*, std::vector<mdl::Node*>>{};
  for (auto* node : nodes)
  {
    auto* parentGroup = dynamic_cast<mdl::GroupNode*>(node->parent());
    if (!parentGroup)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Object " + ids.format(*node) + " is not in a group.",
        "Pass objects that are inside a group.",
        {ids.format(*node)});
    }
    if (node->isDescendantOf(nodes))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Object " + ids.format(*node) + " is inside another of the given objects.",
        "Pass either a group or objects inside it, not both.",
        {ids.format(*node)});
    }

    auto* newParent = parentGroup->parent();
    nodesToMove[newParent].push_back(node);
    moved.push_back(Json{
      {"id", ids.format(*node)},
      {"from", ids.format(*parentGroup)},
      {"to", ids.format(*newParent)},
    });
  }

  mdl::deselectAll(map);
  if (!mdl::reparentNodes(map, nodesToMove))
  {
    return context.operationFailed(
      "The objects could not be removed from their groups.",
      "Check that removing them does not conflict with linked groups.");
  }

  // keep the objects editable: leave open groups that no longer contain them
  for (const auto& [newParent, children] : nodesToMove)
  {
    closeGroupsOutside(map, *newParent);
  }
  selectSelectable(map, nodes);

  return Json{
    {"objects", formatIds(nodes, ids)},
    {"moved", std::move(moved)},
    {"context", groupContextJson(map, ids)},
  };
}

// group_open / group_close

ToolResult groupOpen(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  const auto id = args.get<std::string>("group");
  auto node = ids.resolve(id);
  if (node.is_error())
  {
    return errorOf(node);
  }
  auto* groupNode = dynamic_cast<mdl::GroupNode*>(node.value());
  if (!groupNode)
  {
    return makeError(
      ErrorCode::WrongObjectKind,
      "Object " + id + " is not a group.",
      "Pass a group id such as 'group:12'.",
      {id});
  }

  if (auto error = enterGroup(map, ids, *groupNode))
  {
    return *error;
  }
  return groupContextJson(map, ids);
}

ToolResult groupClose(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  auto closed = std::vector<std::string>{};
  if (!map.editorContext().currentGroup())
  {
    context.warn("NO_OPEN_GROUP", "No group is open, so nothing was closed.");
  }
  else
  {
    do
    {
      closed.push_back(ids.format(*map.editorContext().currentGroup()));
      mdl::closeGroup(map);
    } while (args.get<bool>("all") && map.editorContext().currentGroup());
  }

  auto result = groupContextJson(map, ids);
  result["closed"] = closed;
  return result;
}

// linked_group_duplicate

ToolResult linkedGroupDuplicate(CallContext& context, const Args& args)
{
  auto group = resolveGroup(context, args.get<std::string>("group"), true);
  if (group.is_error())
  {
    return errorOf(group);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  auto* groupNode = group.value();
  const auto groupId = ids.format(*groupNode);
  const auto offset = args.getOptional<vm::vec3d>("offset");
  const auto count = args.get<size_t>("count");

  const auto hasOffset = offset && !vm::is_zero(*offset, Epsilon);
  if (count > 1 && !hasOffset)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Several duplicates without an offset would lie on top of each other.",
      "Pass an 'offset', e.g. [256, 0, 0].",
      {groupId});
  }

  auto duplicates = std::vector<mdl::Node*>{};
  for (size_t i = 1; i <= count; ++i)
  {
    mdl::deselectAll(map);
    mdl::selectNodes(map, {groupNode});
    auto* duplicate = mdl::createLinkedDuplicate(map);
    if (!duplicate)
    {
      return context.operationFailed(
        "The linked duplicate of " + groupId + " could not be created.",
        "Check the editor messages.");
    }
    duplicates.push_back(duplicate);

    if (hasOffset)
    {
      mdl::deselectAll(map);
      mdl::selectNodes(map, {duplicate});
      if (!mdl::translateSelection(map, *offset * double(i)))
      {
        return geometryOperationFailed(
          context,
          "The linked duplicate could not be moved.",
          {groupId},
          "Use a smaller offset or count.");
      }
    }
  }

  if (
    auto error =
      checkInsideWorldBounds(duplicates, map, ids, "Use a smaller offset or count."))
  {
    return *error;
  }

  selectSelectable(map, duplicates);
  return Json{
    {"groups", formatIds(duplicates, ids)},
    {"linkedGroups", groupIds(linkSet(map, *groupNode), ids)},
  };
}

// linked_group_select

ToolResult linkedGroupSelect(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  // reading the link set does not require editable objects
  auto nodes = std::vector<mdl::Node*>{};
  if (const auto explicitIds = args.getOptional<std::vector<std::string>>("ids"))
  {
    for (const auto& id : *explicitIds)
    {
      auto node = ids.resolve(id);
      if (node.is_error())
      {
        return errorOf(node);
      }
      addUnique(nodes, node.value());
    }
  }
  else
  {
    auto targets = resolveTargets(context, args, "ids", GroupableKinds);
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    nodes = targets.value();
  }

  auto linkedGroups = std::vector<mdl::GroupNode*>{};
  for (auto* node : nodes)
  {
    auto* groupNode = dynamic_cast<mdl::GroupNode*>(node);
    if (!groupNode)
    {
      groupNode = mdl::findContainingGroup(node);
    }
    while (groupNode && linkSetSize(map, *groupNode) < 2)
    {
      groupNode = groupNode->containingGroup();
    }
    if (!groupNode)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Object " + ids.format(*node) + " is not in a linked group.",
        "Pass linked groups or objects inside them; create linked groups with "
        "linked_group_duplicate.",
        {ids.format(*node)});
    }
    for (auto* linkedGroup : linkSet(map, *groupNode))
    {
      if (std::ranges::find(linkedGroups, linkedGroup) == linkedGroups.end())
      {
        linkedGroups.push_back(linkedGroup);
      }
    }
  }

  auto selectable = std::vector<mdl::Node*>{};
  auto notSelectable = std::vector<mdl::Node*>{};
  for (auto* linkedGroup : linkedGroups)
  {
    (map.editorContext().selectable(*linkedGroup) ? selectable : notSelectable)
      .push_back(linkedGroup);
  }

  if (selectable.empty())
  {
    return makeError(
      ErrorCode::ObjectNotEditable,
      "None of the linked groups can be selected: they are hidden, locked, open or "
      "inside a closed group.",
      "Show or unlock their layers (layer_set_state), or close the open group "
      "(group_close).",
      formatIds(notSelectable, ids));
  }
  if (!notSelectable.empty())
  {
    context.warn(
      "NOT_SELECTABLE",
      "Some linked groups are hidden, locked, open or inside a closed group and were not "
      "selected.",
      formatIds(notSelectable, ids));
  }

  mdl::deselectAll(map);
  mdl::selectNodes(map, selectable);
  return Json{{"groups", formatIds(selectable, ids)}, {"count", selectable.size()}};
}

// linked_group_separate

ToolResult linkedGroupSeparate(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Group});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto& groupNodes = targets.value();

  for (auto* node : groupNodes)
  {
    const auto& groupNode = static_cast<const mdl::GroupNode&>(*node);
    const auto members = linkSet(map, groupNode);
    if (members.size() < 2)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Group " + ids.format(groupNode) + " is not linked to any other group.",
        "Pass linked groups; use object_get to find them.",
        {ids.format(groupNode)});
    }
    if (std::ranges::all_of(members, [&](const auto* member) {
          return std::ranges::find(groupNodes, member) != groupNodes.end();
        }))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "All groups linked with " + ids.format(groupNode)
          + " were given, so there is nothing to separate them from.",
        "Pass only the groups that should form a separate link set.",
        groupIds(members, ids));
    }
  }

  return withTargets(context, groupNodes, [&]() -> ToolResult {
    if (!mdl::canSeparateSelectedLinkedGroups(map))
    {
      return context.operationFailed("The linked groups could not be separated.");
    }
    mdl::separateSelectedLinkedGroups(map);

    auto groups = Json::array();
    for (const auto* node : groupNodes)
    {
      const auto& groupNode = static_cast<const mdl::GroupNode&>(*node);
      auto linkedWith = std::vector<std::string>{};
      for (const auto* member : linkSet(map, groupNode))
      {
        if (member != &groupNode)
        {
          linkedWith.push_back(ids.format(*member));
        }
      }
      groups.push_back(Json{{"id", ids.format(groupNode)}, {"linkedWith", linkedWith}});
    }
    return Json{{"groups", std::move(groups)}};
  });
}

// linked_group_extract

ToolResult linkedGroupExtract(CallContext& context, const Args& args)
{
  auto targets = resolveMembers(context, args);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto& nodes = targets.value();

  const auto containingGroups = mdl::collectContainingGroups(nodes);
  if (containingGroups.size() != 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      containingGroups.empty() ? "The objects are not in a group."
                               : "The objects are in different groups.",
      "Pass objects that are all directly inside the same linked group.",
      formatIds(nodes, ids));
  }

  auto* groupNode = containingGroups.front();
  const auto groupId = ids.format(*groupNode);
  const auto oldLinkedGroups = linkSet(map, *groupNode);
  if (oldLinkedGroups.size() < 2)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Group " + groupId + " is not linked to any other group.",
      "Use group_remove_objects to move objects out of an unlinked group.",
      {groupId});
  }

  const auto groupsOpenBefore = openGroups(map);
  if (auto error = enterGroup(map, ids, *groupNode))
  {
    return *error;
  }

  mdl::deselectAll(map);
  mdl::selectNodes(map, nodes);
  if (!mdl::canExtractLinkedGroups(map))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The objects are all objects of group " + groupId
        + ", so extracting them would leave it empty.",
      "Use linked_group_separate to unlink the whole group instead.",
      {groupId});
  }

  const auto newLinkedGroups = mdl::extractLinkedGroups(map);
  if (newLinkedGroups.size() != oldLinkedGroups.size())
  {
    return context.operationFailed(
      "The objects could not be extracted.", "Check the editor messages.");
  }

  // restore the open groups (the extraction closed the source group)
  for (auto* current = map.editorContext().currentGroup();
       current && std::ranges::find(groupsOpenBefore, current) == groupsOpenBefore.end();
       current = map.editorContext().currentGroup())
  {
    mdl::closeGroup(map);
  }

  const auto index =
    size_t(std::ranges::find(oldLinkedGroups, groupNode) - oldLinkedGroups.begin());
  auto* extracted = newLinkedGroups[index];
  selectSelectable(map, {extracted});

  // Only the first new group has its final contents yet. The others are updated from it
  // when the call's transaction is committed; their objects that correspond to the
  // first group's objects (same link ids) are replaced by clones that keep their ids.
  auto linkIds = std::vector<std::string>{};
  for (const auto* child : newLinkedGroups.front()->children())
  {
    if (const auto* object = dynamic_cast<const mdl::Object*>(child))
    {
      linkIds.push_back(object->linkId());
    }
  }
  auto objects = std::vector<mdl::Node*>{};
  for (auto* child : extracted->children())
  {
    const auto* object = dynamic_cast<const mdl::Object*>(child);
    if (object && std::ranges::find(linkIds, object->linkId()) != linkIds.end())
    {
      objects.push_back(child);
    }
  }

  auto groups = Json::array();
  for (size_t i = 0; i < oldLinkedGroups.size(); ++i)
  {
    groups.push_back(Json{
      {"source", ids.format(*oldLinkedGroups[i])},
      {"extracted", ids.format(*newLinkedGroups[i])},
    });
  }

  return Json{
    {"group", ids.format(*extracted)},
    {"objects", formatIds(objects, ids)},
    {"groups", std::move(groups)},
  };
}

// Schemas

Field nameField(std::string description)
{
  return field("name", string().nonEmpty()).required().describe(std::move(description));
}

Schema idListSchema()
{
  return array(objectId());
}

Schema contextOutput(std::vector<Field> fields)
{
  fields.push_back(field("openGroup", any())
                     .required()
                     .describe("The current (innermost open) group, or null"));
  fields.push_back(field("openGroups", idListSchema())
                     .required()
                     .describe("The open groups, outermost first"));
  return object(std::move(fields));
}

} // namespace

void registerGroupTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"group_create"}
      .title("Create Group")
      .description(
        "Groups objects like Edit > Group and selects the new group. Brushes of brush "
        "entities are grouped with their entity. The group is created where the first "
        "object is (its group or layer). "
        "Example: {\"ids\": [\"brush:12\", \"entity:15\"], \"name\": \"house\"}")
      .input(object({
        idsField(GroupableKinds),
        nameField("Name of the new group"),
      }))
      .output(object({
        field("group", objectId({ObjectKind::Group}))
          .required()
          .describe("The new group"),
        field("objects", idListSchema()).required().describe("The objects in the group"),
      }))
      .mutation(Mutation::Map)
      .handler(groupCreate));

  registry.add(
    ToolDef{"group_ungroup"}
      .title("Ungroup")
      .description(
        "Dissolves groups like Edit > Ungroup: their objects move to the groups' parents "
        "and keep their ids, and are selected. Ungrouping a linked group unlinks it "
        "first. Example: {\"ids\": [\"group:7\"]}")
      .input(object({idsField(
        {ObjectKind::Group},
        "Groups to dissolve. Default: the "
        "selected groups")}))
      .output(object({
        field("groups", idListSchema()).required().describe("The dissolved groups"),
        field("objects", idListSchema()).required().describe("Their former objects"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(groupUngroup));

  registry.add(
    ToolDef{"group_rename"}
      .title("Rename Groups")
      .description(
        "Renames groups. Example: {\"ids\": [\"group:7\"], \"name\": \"tower\"}")
      .input(object({
        idsField({ObjectKind::Group}, "Groups to rename. Default: the selected groups"),
        nameField("New name"),
      }))
      .output(object({
        field("groups", idListSchema()).required(),
        field("name", string()).required(),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(groupRename));

  registry.add(
    ToolDef{"groups_merge"}
      .title("Merge Groups")
      .description(
        "Merges groups into a target group like 'Merge Groups into <name>': the objects "
        "of the other groups move into the target (keeping their ids), the emptied "
        "groups "
        "are removed, and the target is selected. "
        "Example: {\"ids\": [\"group:7\", \"group:9\"], \"target\": \"group:7\"}")
      .input(object({
        idsField(
          {ObjectKind::Group},
          "Groups to merge into the target (the target may be among them). Default: "
          "the selected groups"),
        field("target", objectId({ObjectKind::Group}))
          .required()
          .describe("The group that receives the objects"),
      }))
      .output(object({
        field("group", objectId({ObjectKind::Group})).required().describe("The target"),
        field("merged", idListSchema()).required().describe("The removed groups"),
        field("objects", idListSchema()).required().describe("The moved objects"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(groupsMerge));

  registry.add(
    ToolDef{"group_add_objects"}
      .title("Add Objects to Group")
      .description(
        "Moves objects into a group like 'Add Objects to Group' (the objects keep their "
        "ids) and selects the group. Brushes of brush entities move with their entity. "
        "Objects already in the group are skipped. If the group is linked, its linked "
        "copies get the objects too. "
        "Example: {\"group\": \"group:7\", \"ids\": [\"brush:12\"]}")
      .input(object({
        field("group", objectId({ObjectKind::Group}))
          .required()
          .describe("The group that receives the objects"),
        idsField(GroupableKinds, "Objects to add. Default: the current selection"),
      }))
      .output(object({
        field("group", objectId({ObjectKind::Group})).required(),
        field("objects", idListSchema()).required().describe("The moved objects"),
      }))
      .mutation(Mutation::Map)
      .handler(groupAddObjects));

  registry.add(
    ToolDef{"group_remove_objects"}
      .title("Remove Objects from Group")
      .description(
        "Moves objects out of their group into the group's parent (the enclosing group "
        "or layer), like 'Remove Objects from Group'. The objects keep their ids and are "
        "selected; the group does not need to be open. A group that becomes empty is "
        "removed, and open groups that no longer contain the objects are closed. Brushes "
        "of brush entities move with their entity. "
        "Example: {\"ids\": [\"brush:12\", \"brush:13\"]}")
      .input(object({
        idsField(GroupableKinds, "Objects inside groups. Default: the current selection"),
      }))
      .output(contextOutput({
        field("objects", idListSchema()).required().describe("The moved objects"),
        field(
          "moved",
          array(object({
            field("id", objectId()).required(),
            field("from", objectId()).required().describe("The old group"),
            field("to", objectId()).required().describe("The new parent"),
          })))
          .required(),
      }))
      .mutation(Mutation::Map)
      .handler(groupRemoveObjects));

  registry.add(
    ToolDef{"group_open"}
      .title("Open Group")
      .description(
        "Opens a group for editing, like double-clicking it: open groups that do not "
        "contain it are closed, enclosing closed groups are opened, and the selection is "
        "cleared. Objects outside the open group cannot be edited until group_close. "
        "Most tools can edit objects in closed groups only through the group; open it "
        "to edit its members individually. Example: {\"group\": \"group:7\"}")
      .input(object({
        field("group", objectId({ObjectKind::Group}))
          .required()
          .describe("Group to open"),
      }))
      .output(contextOutput({}))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(groupOpen));

  registry.add(
    ToolDef{"group_close"}
      .title("Close Group")
      .description(
        "Closes the current (innermost open) group, or all open groups, and clears the "
        "selection. Changes to a linked group are applied to its linked copies. Warns "
        "with NO_OPEN_GROUP if no group is open. Example: {\"all\": true}")
      .input(object({
        field("all", boolean().defaultsTo(false)).describe("Close all open groups"),
      }))
      .output(contextOutput({
        field("closed", idListSchema()).required().describe("The closed groups"),
      }))
      .mutation(Mutation::Map)
      .handler(groupClose));

  registry.add(
    ToolDef{"linked_group_duplicate"}
      .title("Create Linked Duplicate")
      .description(
        "Creates linked duplicates of a group, like Edit > Create Linked Duplicate: "
        "changes to the contents of any linked copy are applied to all of them, while "
        "each copy keeps its own position. Copy i is moved by i * offset. Selects the "
        "new groups. Example: {\"group\": \"group:7\", \"offset\": [512, 0, 0], "
        "\"count\": 3}")
      .input(object({
        field("group", objectId({ObjectKind::Group}))
          .required()
          .describe("Group to copy"),
        field("offset", vec3()).describe("Offset between consecutive copies"),
        field("count", integer().min(1).max(64).defaultsTo(1))
          .describe("Number of copies; more than one needs an offset"),
      }))
      .output(object({
        field("groups", idListSchema()).required().describe("The new linked groups"),
        field("linkedGroups", idListSchema())
          .required()
          .describe("All groups of the link set"),
      }))
      .mutation(Mutation::Map)
      .handler(linkedGroupDuplicate));

  registry.add(
    ToolDef{"linked_group_select"}
      .title("Select Linked Groups")
      .description(
        "Selects all groups linked with the given groups, or with the linked groups that "
        "contain the given objects. Groups that cannot be selected (hidden, locked, "
        "inside a closed group) are skipped with a NOT_SELECTABLE warning. "
        "Example: {\"ids\": [\"group:7\"]}")
      .input(object({
        field("ids", array(objectId(GroupableKinds)).nonEmpty())
          .describe(
            "Linked groups or objects inside them (they need not be editable). Default: "
            "the current selection"),
      }))
      .output(object({
        field("groups", idListSchema()).required().describe("The selected groups"),
        field("count", integer()).required(),
      }))
      .mutation(Mutation::Map)
      .handler(linkedGroupSelect));

  registry.add(
    ToolDef{"linked_group_separate"}
      .title("Separate Linked Groups")
      .description(
        "Unlinks the given groups from the other groups of their link set, like "
        "Edit > Separate Linked Groups. Given groups of the same link set stay linked "
        "with each other. The groups and their objects keep their ids. "
        "Example: {\"ids\": [\"group:9\"]}")
      .input(object({
        idsField(
          {ObjectKind::Group},
          "Linked groups to separate. Default: the selected "
          "groups"),
      }))
      .output(object({
        field(
          "groups",
          array(object({
            field("id", objectId({ObjectKind::Group})).required(),
            field("linkedWith", idListSchema())
              .required()
              .describe("The groups it is still linked with"),
          })))
          .required(),
      }))
      .mutation(Mutation::Map)
      .handler(linkedGroupSeparate));

  registry.add(
    ToolDef{"linked_group_extract"}
      .title("Extract Linked Groups")
      .description(
        "Extracts objects out of their linked group, like Edit > Extract Linked Groups: "
        "the objects and their counterparts are removed from every group of the link set "
        "and put into new groups that are linked with each other and keep the positions "
        "of the old ones. The extracted objects get new ids (listed in 'objects'). The "
        "group does not need to be open; it is closed afterwards and the new group of "
        "the "
        "source is selected. Example: {\"ids\": [\"brush:12\"]}")
      .input(object({
        idsField(
          GroupableKinds,
          "Objects inside one linked group (not all of its objects). Default: the "
          "current selection"),
      }))
      .output(object({
        field("group", objectId({ObjectKind::Group}))
          .required()
          .describe("The new group that holds the extracted objects"),
        field("objects", idListSchema())
          .required()
          .describe("The extracted objects in the new group (new ids)"),
        field(
          "groups",
          array(object({
            field("source", objectId({ObjectKind::Group})).required(),
            field("extracted", objectId({ObjectKind::Group})).required(),
          })))
          .required()
          .describe("For each old linked group, the new group of its extracted copy"),
      }))
      .mutation(Mutation::Map)
      .handler(linkedGroupExtract));
}

} // namespace tb::mcp
