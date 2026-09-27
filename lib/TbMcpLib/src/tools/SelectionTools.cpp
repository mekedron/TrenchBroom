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

#include "mcp/tools/SelectionTools.h"

#include "NodeJson.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ChangeCollector.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

std::string selectionMode(const mdl::Selection& selection)
{
  return selection.hasBrushFaces() ? "faces" : selection.hasNodes() ? "objects" : "none";
}

size_t selectionCount(const mdl::Selection& selection)
{
  return selection.hasBrushFaces() ? selection.brushFaces.size() : selection.nodes.size();
}

Json countsByKind(const mdl::Selection& selection)
{
  auto result = Json::object();
  if (selection.hasBrushFaces())
  {
    auto brushes = std::vector<const mdl::BrushNode*>{};
    for (const auto& handle : selection.brushFaces)
    {
      brushes.push_back(handle.node());
    }
    std::ranges::sort(brushes);
    const auto [first, last] = std::ranges::unique(brushes);
    brushes.erase(first, last);
    result["face"] = selection.brushFaces.size();
    result["brush"] = brushes.size();
    return result;
  }

  // stable order of kinds
  auto counts = std::map<ObjectKind, size_t>{};
  for (const auto* node : selection.nodes)
  {
    ++counts[objectKindOf(*node)];
  }
  for (const auto& [kind, count] : counts)
  {
    result[std::string{toString(kind)}] = count;
  }
  return result;
}

Json selectionBoundsJson(const mdl::Map& map)
{
  const auto& selection = map.selection();
  if (selection.hasBrushFaces())
  {
    auto bounds = std::optional<vm::bbox3d>{};
    for (const auto& handle : selection.brushFaces)
    {
      const auto faceBounds = handle.face().bounds();
      bounds = bounds ? vm::merge(*bounds, faceBounds) : faceBounds;
    }
    return bounds ? toJson(*bounds) : Json(nullptr);
  }
  if (const auto& bounds = map.selectionBounds())
  {
    return toJson(*bounds);
  }
  return nullptr;
}

Json selectionItem(
  const mdl::Map& map, const IdRegistry& ids, const size_t index, const Detail detail)
{
  const auto& selection = map.selection();
  if (selection.hasBrushFaces())
  {
    const auto& handle = selection.brushFaces[index];
    return faceJson(map, *handle.node(), handle.faceIndex(), ids, detail);
  }

  const auto& node = *selection.nodes[index];
  auto item = nodeSummary(node, ids);
  if (detail == Detail::Full)
  {
    item["state"] = nodeState(map, node);
  }
  return item;
}

Json selectionHeader(const mdl::Map& map)
{
  const auto& selection = map.selection();
  return Json{
    {"mode", selectionMode(selection)},
    {"count", selectionCount(selection)},
    {"countsByKind", countsByKind(selection)},
    {"bounds", selectionBoundsJson(map)},
  };
}

/** The tool-specific result of a selection change: mode and count of the new selection.
 */
Json selectionResult(const mdl::Map& map)
{
  const auto& selection = map.selection();
  return Json{
    {"mode", selectionMode(selection)},
    {"count", selectionCount(selection)},
  };
}

void warnIfEmpty(CallContext& context, const std::string& message)
{
  if (!context.map().selection().hasAny())
  {
    context.warn("NO_MATCH", message);
  }
}

ToolError notSelectableError(const std::string& id)
{
  return makeError(
    ErrorCode::ObjectNotEditable,
    "Object " + id
      + " cannot be selected: it is hidden, locked, or inside a closed group.",
    "Show or unlock its layer (layer_set_state), open its group (group_open), or select "
    "the containing group instead.",
    {id});
}

ToolResult selectionGet(CallContext& context, const Args& args)
{
  const auto& map = context.map();
  const auto& ids = context.ids();

  auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }
  const auto& page = request.value();

  const auto total = selectionCount(map.selection());
  auto items = Json::array();
  for (auto i = page.offset; i < total && items.size() < page.limit; ++i)
  {
    items.push_back(selectFields(selectionItem(map, ids, i, page.detail), page.fields));
  }

  const auto end = page.offset + items.size();
  auto result = selectionHeader(map);
  result["items"] = std::move(items);
  result["total"] = total;
  result["nextCursor"] =
    end < total ? Json(encodeCursor(end, map.modificationCount())) : Json(nullptr);
  if (page.stale)
  {
    result["stale"] = true;
  }
  return result;
}

ToolResult selectionSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& editorContext = map.editorContext();
  const auto mode = args.get<std::string>("mode");

  auto nodes = std::vector<mdl::Node*>{};
  auto faces = std::vector<mdl::BrushFaceHandle>{};
  auto firstObjectId = std::optional<std::string>{};
  auto firstFaceId = std::optional<std::string>{};

  for (const auto& id : args.get<std::vector<std::string>>("ids"))
  {
    const auto ref = parseObjectRef(id);
    if (!ref)
    {
      return makeError(ErrorCode::InvalidArgument, "'" + id + "' is not a valid id.");
    }

    auto resolved = ids.resolve(*ref);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    auto* node = resolved.value();

    if (ref->faceIndex)
    {
      firstFaceId = firstFaceId.value_or(id);
      auto* brushNode = static_cast<mdl::BrushNode*>(node);
      const auto handle = mdl::BrushFaceHandle{brushNode, *ref->faceIndex};
      if (mode != "remove" && !editorContext.selectable(*brushNode, handle.face()))
      {
        return notSelectableError(id);
      }
      if (std::ranges::find(faces, handle) == faces.end())
      {
        faces.push_back(handle);
      }
    }
    else
    {
      firstObjectId = firstObjectId.value_or(id);
      const auto kind = objectKindOf(*node);
      if (kind == ObjectKind::World || kind == ObjectKind::Layer)
      {
        return makeError(
          ErrorCode::WrongObjectKind,
          "Object " + id + " is a " + std::string{toString(kind)}
            + " and cannot be selected itself.",
          "To select the contents of layers, use select_by with 'layers'.",
          {id});
      }
      // like a click in the editor, a brush entity selects its brushes and patches
      const auto members = kind == ObjectKind::Entity && node->hasChildren()
                             ? node->children()
                             : std::vector<mdl::Node*>{node};
      for (auto* member : members)
      {
        if (mode != "remove" && !editorContext.selectable(*member))
        {
          return notSelectableError(id);
        }
        if (std::ranges::find(nodes, member) == nodes.end())
        {
          nodes.push_back(member);
        }
      }
    }
  }

  if (!nodes.empty() && !faces.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Objects and faces cannot be selected at the same time (e.g. " + *firstObjectId
        + " and " + *firstFaceId + ").",
      "Pass either object ids or face ids. To select all faces of brushes, use "
      "select_faces_of.",
      {*firstObjectId, *firstFaceId});
  }

  const auto& selection = map.selection();
  if (mode == "replace")
  {
    mdl::deselectAll(map);
    if (!nodes.empty())
    {
      mdl::selectNodes(map, nodes);
    }
    else
    {
      mdl::selectBrushFaces(map, faces);
    }
  }
  else if (mode == "add")
  {
    if (!nodes.empty() && selection.hasBrushFaces())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Objects cannot be added to a face selection.",
        "Use mode 'replace' to replace the face selection with objects.");
    }
    if (!faces.empty() && selection.hasNodes())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Faces cannot be added to an object selection.",
        "Use mode 'replace' to replace the object selection with faces.");
    }
    if (!nodes.empty())
    {
      mdl::selectNodes(map, nodes);
    }
    else
    {
      mdl::selectBrushFaces(map, faces);
    }
  }
  else
  {
    if (!nodes.empty())
    {
      mdl::deselectNodes(map, nodes);
    }
    else
    {
      mdl::deselectBrushFaces(map, faces);
    }
  }

  return selectionResult(map);
}

ToolResult selectionClear(CallContext& context, const Args&)
{
  auto& map = context.map();
  const auto previousCount = selectionCount(map.selection());
  mdl::deselectAll(map);
  return Json{{"cleared", previousCount}};
}

ToolResult selectAll(CallContext& context, const Args&)
{
  auto& map = context.map();
  mdl::selectAllNodes(map);
  warnIfEmpty(context, "There are no visible, unlocked objects to select.");
  return selectionResult(map);
}

ToolResult selectInvert(CallContext& context, const Args&)
{
  auto& map = context.map();
  mdl::invertNodeSelection(map);
  warnIfEmpty(context, "Nothing was selected: all selectable objects were selected.");
  return selectionResult(map);
}

Result<std::vector<mdl::Node*>, ToolError> resolveIdList(
  CallContext& context, const std::vector<std::string>& idList, const ObjectKind kind)
{
  auto nodes = std::vector<mdl::Node*>{};
  for (const auto& id : idList)
  {
    auto node = context.ids().resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    if (objectKindOf(*node.value()) != kind)
    {
      return makeError(
        ErrorCode::WrongObjectKind,
        "Object " + id + " is not a " + std::string{toString(kind)} + ".",
        "Pass only " + std::string{toString(kind)} + " ids.",
        {id});
    }
    if (std::ranges::find(nodes, node.value()) == nodes.end())
    {
      nodes.push_back(node.value());
    }
  }
  return nodes;
}

ToolResult selectByLinkedGroups(CallContext& context, const Json& value)
{
  auto& map = context.map();
  const auto idList = value.is_string()
                        ? std::vector<std::string>{value.get<std::string>()}
                        : value.get<std::vector<std::string>>();

  auto groups = resolveIdList(context, idList, ObjectKind::Group);
  if (groups.is_error())
  {
    return errorOf(groups);
  }

  for (auto* node : groups.value())
  {
    const auto& groupNode = static_cast<const mdl::GroupNode&>(*node);
    const auto linked =
      mdl::collectNodesWithLinkId({&map.worldNode()}, groupNode.linkId());
    if (linked.size() < 2)
    {
      const auto id = context.ids().format(*node);
      return makeError(
        ErrorCode::InvalidArgument,
        "Group " + id + " is not linked to any other group.",
        "Pass a group that has linked duplicates (see group_list or object details).",
        {id});
    }
    if (!map.editorContext().selectable(*node))
    {
      return notSelectableError(context.ids().format(*node));
    }
  }

  mdl::deselectAll(map);
  mdl::selectNodes(map, groups.value());
  if (!mdl::canSelectLinkedGroups(map))
  {
    return context.operationFailed("The linked groups could not be selected.");
  }
  mdl::selectLinkedGroups(map);
  return selectionResult(map);
}

ToolResult selectBy(CallContext& context, const Args& args)
{
  auto& map = context.map();

  auto criteria = std::vector<std::string>{};
  for (const auto* key : {"classname", "material", "layers", "linkedGroup"})
  {
    if (args.has(key))
    {
      criteria.emplace_back(key);
    }
  }
  if (criteria.size() != 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      criteria.empty() ? "No selection criterion was given."
                       : "Only one criterion can be given, but got "
                           + std::to_string(criteria.size()) + ".",
      "Pass exactly one of 'classname', 'material', 'layers' or 'linkedGroup'.");
  }
  if (args.has("target") && criteria.front() != "material")
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'target' only applies to the 'material' criterion.",
      "Remove 'target', or select by 'material'.");
  }

  const auto& criterion = criteria.front();
  if (criterion == "classname")
  {
    const auto classname = args.get<std::string>("classname");
    mdl::selectEntitiesWithClassname(map, classname);
    warnIfEmpty(
      context,
      "No visible, editable entity has the classname '" + classname
        + "' (the comparison ignores case).");
  }
  else if (criterion == "material")
  {
    const auto material = args.get<std::string>("material");
    if (args.getOr<std::string>("target", "brushes") == "faces")
    {
      mdl::selectBrushFacesWithMaterial(map, material);
    }
    else
    {
      mdl::selectBrushesWithMaterial(map, material);
    }
    warnIfEmpty(
      context, "No selectable brush face uses the material '" + material + "'.");
  }
  else if (criterion == "layers")
  {
    auto layers = resolveIdList(
      context, args.get<std::vector<std::string>>("layers"), ObjectKind::Layer);
    if (layers.is_error())
    {
      return errorOf(layers);
    }
    auto layerNodes = std::vector<mdl::LayerNode*>{};
    for (auto* node : layers.value())
    {
      layerNodes.push_back(static_cast<mdl::LayerNode*>(node));
    }
    if (!mdl::canSelectAllInLayers(map, layerNodes))
    {
      return makeError(
        ErrorCode::OperationFailed,
        "The selection cannot be changed right now.",
        "Try again when the user has finished the current interaction.");
    }
    mdl::selectAllInLayers(map, layerNodes);
    warnIfEmpty(
      context,
      "The layers contain no selectable objects (they may be empty, hidden or locked).");
  }
  else
  {
    return selectByLinkedGroups(context, args.get<Json>("linkedGroup"));
  }

  return selectionResult(map);
}

vm::axis::type axisFromString(const std::string& axis)
{
  return axis == "x" ? vm::axis::x : axis == "y" ? vm::axis::y : vm::axis::z;
}

ToolResult selectSpatial(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  const auto mode = args.get<std::string>("mode");
  const auto deleteSelectors = args.get<bool>("deleteSelectors");

  auto selectorIds = std::vector<std::string>{};
  for (const auto* node : targets.value())
  {
    selectorIds.push_back(context.ids().format(*node));
  }

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      if (mode == "touching")
      {
        mdl::selectTouchingNodes(map, deleteSelectors);
      }
      else if (mode == "inside")
      {
        mdl::selectContainedNodes(map, deleteSelectors);
      }
      else
      {
        mdl::selectTouchingNodes(
          map, axisFromString(args.get<std::string>("axis")), deleteSelectors);
      }

      warnIfEmpty(context, "No selectable object matched.");
      auto result = selectionResult(map);
      result["selectors"] = selectorIds;
      result["selectorsDeleted"] = deleteSelectors;
      return result;
    },
    SelectionAfter::Result);
}

ToolResult selectSiblings(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      mdl::selectSiblingNodes(map);
      return selectionResult(map);
    },
    SelectionAfter::Result);
}

ToolResult selectByLine(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto lines = std::vector<size_t>{};
  for (const auto line : args.get<std::vector<int64_t>>("lines"))
  {
    lines.push_back(size_t(line));
  }

  mdl::selectNodesWithFilePosition(map, lines);
  warnIfEmpty(
    context,
    "No selectable object is defined at these lines. Line numbers refer to the file as "
    "last loaded or saved.");
  return selectionResult(map);
}

ToolResult selectFacesOf(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& editorContext = map.editorContext();

  if (args.has("ids") && args.has("face"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'ids' and 'face' cannot be combined.",
      "Pass brush ids in 'ids' to select all their faces, or one face id in 'face' to "
      "select its connected coplanar faces.");
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  if (const auto faceId = args.getOptional<std::string>("face"))
  {
    auto handle = resolveFace(context, *faceId);
    if (handle.is_error())
    {
      return errorOf(handle);
    }
    const auto& startFace = handle.value();
    if (!editorContext.selectable(*startFace.node(), startFace.face()))
    {
      return notSelectableError(*faceId);
    }

    faces = args.getOr<bool>("coplanar", true)
              ? mdl::collectConnectedCoplanarFaces(
                  startFace, editorContext, map.worldNode().nodeTree())
              : std::vector<mdl::BrushFaceHandle>{startFace};
  }
  else
  {
    if (args.has("coplanar"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "'coplanar' only applies together with 'face'.",
        "Pass a face id in 'face', or remove 'coplanar'.");
    }

    auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    faces = mdl::collectSelectableBrushFaces(targets.value(), editorContext);
  }

  mdl::deselectAll(map);
  mdl::selectBrushFaces(map, faces);
  return selectionResult(map);
}

Schema selectionResultSchema(std::vector<Field> extraFields = {})
{
  auto fields = std::vector<Field>{
    field("mode", enumOf({"none", "objects", "faces"}))
      .required()
      .describe("What is selected now"),
    field("count", integer()).required().describe("Number of selected objects or faces"),
  };
  for (auto& extra : extraFields)
  {
    fields.push_back(std::move(extra));
  }
  return object(std::move(fields));
}

} // namespace

void registerSelectionTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"selection_get"}
      .title("Get Selection")
      .description(
        "Returns the current selection: mode ('none', 'objects' or 'faces'), count, "
        "counts per kind, bounds and a page of the selected objects (id, kind, label, "
        "bounds, layer, ...) or faces (id, normal, center, material). detail 'full' adds "
        "each object's state or each face's UV attributes and vertices. TrenchBroom "
        "selects either objects or faces, never both. "
        "Example: {\"limit\": 20, \"fields\": [\"id\", \"label\"]}")
      .input(object({}))
      .output(object({
        field("mode", enumOf({"none", "objects", "faces"})).required(),
        field("count", integer()).required(),
        field("countsByKind", object({}).allowAdditionalProperties())
          .required()
          .describe(
            "e.g. {\"brush\": 3, \"entity\": 1}; for faces {\"face\", \"brush\"}"),
        field("bounds", any()).describe("Bounds of the selection, or null"),
        field("items", array(any())).required(),
        field("total", integer()).required(),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .paginated()
      .idempotent()
      .handler(selectionGet));

  registry.add(
    ToolDef{"selection_set"}
      .title("Set Selection")
      .description(
        "Replaces, extends or reduces the selection with object ids or face ids (like "
        "'brush:12/face:3'); objects and faces cannot be mixed. As in the editor, a "
        "brush entity id selects its brushes and patches. Selecting is undoable. "
        "Hidden, locked objects and objects inside closed groups cannot be selected. To "
        "deselect everything use selection_clear. "
        "Example: {\"ids\": [\"brush:12\", \"entity:40\"], \"mode\": \"add\"}")
      .input(object({
        field("ids", array(objectId()).nonEmpty())
          .required()
          .describe("Object ids or face ids"),
        field("mode", enumOf({"replace", "add", "remove"}).defaultsTo("replace"))
          .describe("'replace' the selection, 'add' to it or 'remove' from it"),
      }))
      .output(selectionResultSchema())
      .mutation(Mutation::Map)
      .handler(selectionSet));

  registry.add(ToolDef{"selection_clear"}
                 .title("Clear Selection")
                 .description("Deselects all objects and faces. Example: {}")
                 .input(object({}))
                 .output(object({
                   field("cleared", integer())
                     .required()
                     .describe("Number of objects or faces that were selected before"),
                 }))
                 .mutation(Mutation::Map)
                 .handler(selectionClear));

  registry.add(
    ToolDef{"select_all"}
      .title("Select All")
      .description(
        "Selects all visible, unlocked objects, like Edit > Select All. Inside an open "
        "group, only that group's contents are selected. Example: {}")
      .input(object({}))
      .output(selectionResultSchema())
      .mutation(Mutation::Map)
      .handler(selectAll));

  registry.add(
    ToolDef{"select_invert"}
      .title("Invert Selection")
      .description(
        "Selects all visible, unlocked objects that are not selected and deselects the "
        "selected ones, like Edit > Select Inverse. Example: {}")
      .input(object({}))
      .output(selectionResultSchema())
      .mutation(Mutation::Map)
      .handler(selectInvert));

  registry.add(
    ToolDef{"select_by"}
      .title("Select By")
      .description(
        "Replaces the selection with everything matching exactly one criterion: "
        "'classname' (entities, case-insensitive), 'material' (brushes using it, or its "
        "faces with target 'faces'), 'layers' (all selectable objects in these layers) "
        "or "
        "'linkedGroup' (all groups linked with the given groups). Only visible, unlocked "
        "objects are selected. No match gives count 0 and a NO_MATCH warning. "
        "Example: {\"classname\": \"monster_ogre\"} or {\"material\": \"wall_metal\", "
        "\"target\": \"faces\"}")
      .input(object({
        field("classname", string().nonEmpty()).describe("Entity classname"),
        field("material", string().nonEmpty()).describe("Material name"),
        field("target", enumOf({"brushes", "faces"}))
          .describe("With 'material': select brushes (default) or faces"),
        field("layers", array(objectId({ObjectKind::Layer})).nonEmpty())
          .describe("Layer ids"),
        field(
          "linkedGroup",
          oneOf({
            objectId({ObjectKind::Group}),
            array(objectId({ObjectKind::Group})).nonEmpty(),
          }))
          .describe("Group id or ids; selects all groups linked with them"),
      }))
      .output(selectionResultSchema())
      .mutation(Mutation::Map)
      .handler(selectBy));

  registry.add(
    ToolDef{"select_spatial"}
      .title("Select Spatially")
      .description(
        "Uses brushes as selectors and selects the objects that touch them ('touching'), "
        "lie completely inside them ('inside') or lie inside them when the selectors are "
        "extended infinitely along an axis ('tall'), like Edit > Select Touching / "
        "Inside "
        "/ Tall. The selectors themselves are not selected; deleteSelectors removes them "
        "in the same undo step. "
        "Example: {\"mode\": \"inside\", \"ids\": [\"brush:12\"]}")
      .input(object({
        field("mode", enumOf({"touching", "inside", "tall"})).required(),
        idsField({ObjectKind::Brush}, "Selector brushes. Default: the selected brushes"),
        field("axis", enumOf({"x", "y", "z"}).defaultsTo("z"))
          .describe("For 'tall': the axis along which the selectors are extended"),
        field("deleteSelectors", boolean().defaultsTo(false))
          .describe("Delete the selector brushes"),
      }))
      .output(selectionResultSchema({
        field("selectors", array(string())).describe("The selector brushes"),
        field("selectorsDeleted", boolean()),
      }))
      .mutation(Mutation::Map)
      .handler(selectSpatial));

  registry.add(ToolDef{"select_siblings"}
                 .title("Select Siblings")
                 .description("Selects all selectable objects that share a parent "
                              "(entity, group or layer) with "
                              "the given objects, like Edit > Select Siblings. "
                              "Example: {\"ids\": [\"brush:12\"]}")
                 .input(object({idsField()}))
                 .output(selectionResultSchema())
                 .mutation(Mutation::Map)
                 .handler(selectSiblings));

  registry.add(
    ToolDef{"select_by_line"}
      .title("Select by Line Number")
      .description(
        "Selects the objects defined at the given lines (1-based) of the map file, e.g. "
        "lines reported by a compiler. Line numbers refer to the file as last loaded or "
        "saved; objects created since then have no line. "
        "Example: {\"lines\": [231, 1042]}")
      .input(object({
        field("lines", array(integer().min(1)).nonEmpty())
          .required()
          .describe("1-based line numbers"),
      }))
      .output(selectionResultSchema())
      .mutation(Mutation::Map)
      .handler(selectByLine));

  registry.add(
    ToolDef{"select_faces_of"}
      .title("Select Faces Of")
      .description(
        "Selects faces: all faces of the given brushes ('ids', default: the selected "
        "brushes), or, with 'face', all faces that are coplanar with it and connected to "
        "it through shared edges (like shift+double-click in the editor; coplanar: false "
        "selects just that face). "
        "Example: {\"face\": \"brush:12/face:4\"} or {\"ids\": [\"brush:12\"]}")
      .input(object({
        idsField({ObjectKind::Brush}, "Brush ids. Default: the selected brushes"),
        field("face", objectId({ObjectKind::Brush}))
          .describe("A face id, e.g. 'brush:12/face:4'"),
        field("coplanar", boolean())
          .describe("With 'face': select connected coplanar faces (default true)"),
      }))
      .output(selectionResultSchema())
      .mutation(Mutation::Map)
      .handler(selectFacesOf));
}

Json selectionDetails(const mdl::Map& map, const IdRegistry& ids, const size_t limit)
{
  const auto total = selectionCount(map.selection());
  auto items = Json::array();
  for (size_t i = 0; i < total && i < limit; ++i)
  {
    items.push_back(selectionItem(map, ids, i, Detail::Summary));
  }

  auto result = selectionHeader(map);
  result["items"] = std::move(items);
  result["truncated"] = total > limit;
  return result;
}

} // namespace tb::mcp
