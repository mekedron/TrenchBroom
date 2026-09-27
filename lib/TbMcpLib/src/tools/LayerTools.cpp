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

#include "mcp/tools/LayerTools.h"

#include "base/Color.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeQueries.h"
#include "mdl/Selection.h"
#include "mdl/VisibilityState.h"
#include "mdl/WorldNode.h"

#include "kd/vector_utils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** The kinds of objects that can be moved between layers or hidden and shown. */
const auto ObjectKinds = std::vector<ObjectKind>{
  ObjectKind::Group, ObjectKind::Entity, ObjectKind::Brush, ObjectKind::Patch};

// Describing layers

/** The position of the layer in the layer list: 0 for the default layer, 1.. after it. */
size_t layerPosition(const mdl::Map& map, const mdl::LayerNode& layerNode)
{
  const auto layers = map.worldNode().allLayersUserSorted();
  const auto it = std::ranges::find(layers, &layerNode);
  return size_t(std::distance(layers.begin(), it));
}

Json layerColorJson(const Color& color)
{
  const auto rgb = color.to<RgbB>();
  return Json{
    int(rgb.get<ColorChannel::r>()),
    int(rgb.get<ColorChannel::g>()),
    int(rgb.get<ColorChannel::b>()),
  };
}

/** Counts the objects of a layer by kind, including nested ones. */
Json layerCounts(const mdl::LayerNode& layerNode)
{
  auto brushes = size_t{0}, patches = size_t{0}, entities = size_t{0}, groups = size_t{0};
  for (const auto* node : mdl::collectDescendants(
         std::vector<mdl::Node*>{const_cast<mdl::LayerNode*>(&layerNode)}))
  {
    switch (objectKindOf(*node))
    {
    case ObjectKind::Brush:
      ++brushes;
      break;
    case ObjectKind::Patch:
      ++patches;
      break;
    case ObjectKind::Entity:
      ++entities;
      break;
    case ObjectKind::Group:
      ++groups;
      break;
    case ObjectKind::World:
    case ObjectKind::Layer:
      break;
    }
  }
  return Json{
    {"brushes", brushes},
    {"patches", patches},
    {"entities", entities},
    {"groups", groups},
    {"total", brushes + patches + entities + groups},
  };
}

Json layerJson(
  const mdl::Map& map, const mdl::LayerNode& layerNode, const IdRegistry& ids)
{
  const auto& layer = layerNode.layer();
  auto result = Json{
    {"id", ids.format(layerNode)},
    {"address", *nameAddressOf(layerNode)},
    {"name", layer.name()},
    {"default", layerNode.isDefaultLayer()},
    {"position", layerPosition(map, layerNode)},
    {"sortIndex", layer.sortIndex()},
    {"current", map.editorContext().currentLayer() == &layerNode},
    {"hidden", layerNode.hidden()},
    {"locked", layerNode.locked()},
    {"omitFromExport", layer.omitFromExport()},
    {"color", layer.color() ? layerColorJson(*layer.color()) : Json(nullptr)},
    {"counts", layerCounts(layerNode)},
  };
  return result;
}

Schema layerSchema()
{
  return object({
    field("id", objectId({ObjectKind::Layer})).required().describe("Layer id"),
    field("address", string())
      .required()
      .describe("Name address, e.g. 'layer:@Details'; survives editor restarts"),
    field("name", string()).required(),
    field("default", boolean()).required().describe("Whether this is the default layer"),
    field("position", integer())
      .required()
      .describe("Position in the layer list: 0 is the default layer, 1.. the others"),
    field("sortIndex", integer()).required().describe("The stored sort index"),
    field("current", boolean())
      .required()
      .describe("Whether new objects are added to this layer"),
    field("hidden", boolean()).required(),
    field("locked", boolean()).required(),
    field("omitFromExport", boolean())
      .required()
      .describe("Whether the layer is left out when compiling or exporting"),
    field("color", any()).describe("[r, g, b] (0-255), or null"),
    field("counts", object({}).allowAdditionalProperties())
      .required()
      .describe("Objects by kind: brushes, patches, entities, groups, total"),
  });
}

// Resolving layers

Result<mdl::LayerNode*, ToolError> resolveLayer(
  CallContext& context, const std::string& id)
{
  auto node = context.ids().resolve(id);
  if (node.is_error())
  {
    return errorOf(node);
  }
  if (auto* layerNode = dynamic_cast<mdl::LayerNode*>(node.value()))
  {
    return layerNode;
  }
  return makeError(
    ErrorCode::WrongObjectKind,
    "Object " + id + " is not a layer.",
    "Pass a layer id from layers_list, e.g. 'layer:default'.",
    {id});
}

Result<mdl::LayerNode*, ToolError> layerArgument(
  CallContext& context, const Args& args, const std::string& key = "layer")
{
  return resolveLayer(context, args.get<std::string>(key));
}

ToolError defaultLayerError(const std::string& what)
{
  return makeError(
    ErrorCode::InvalidArgument,
    "The default layer cannot be " + what + ".",
    "Pass a custom layer id from layers_list.",
    {"layer:default"});
}

/** Warns if new objects would end up in a hidden or locked layer. */
void warnAboutCurrentLayer(CallContext& context)
{
  const auto& map = context.map();
  const auto* currentLayer = map.editorContext().currentLayer();
  if (!currentLayer)
  {
    return;
  }
  const auto id = context.ids().format(*currentLayer);
  if (currentLayer->hidden())
  {
    context.warn(
      "CURRENT_LAYER_HIDDEN",
      "The current layer '" + currentLayer->name()
        + "' is hidden; new objects are added to it and will not be visible. Make "
          "another layer current or show it with layer_set_state.",
      {id});
  }
  if (currentLayer->locked())
  {
    context.warn(
      "CURRENT_LAYER_LOCKED",
      "The current layer '" + currentLayer->name()
        + "' is locked; new objects are added to it and cannot be edited. Make "
          "another layer current or unlock it with layer_set_state.",
      {id});
  }
}

/** The editor only removes a layer while another layer is visible and unlocked. */
bool hasOtherVisibleAndUnlockedLayer(const mdl::Map& map, const mdl::LayerNode& except)
{
  const auto layers = map.worldNode().allLayers();
  return std::ranges::any_of(layers, [&](const auto* layerNode) {
    return layerNode != &except && !layerNode->hidden() && !layerNode->locked();
  });
}

// Tool handlers

ToolResult layersList(CallContext& context, const Args&)
{
  const auto& map = context.map();
  const auto& ids = context.ids();

  auto layers = Json::array();
  for (const auto* layerNode : map.worldNode().allLayersUserSorted())
  {
    layers.push_back(layerJson(map, *layerNode, ids));
  }

  const auto* currentLayer = map.editorContext().currentLayer();
  return Json{
    {"layers", std::move(layers)},
    {"current", currentLayer ? Json(ids.format(*currentLayer)) : Json(nullptr)},
  };
}

ToolResult layerCreate(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& worldNode = map.worldNode();
  const auto name = args.get<std::string>("name");

  if (args.has("position") && args.has("after"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'position' and 'after' cannot be combined.",
      "Pass either 'position' (1 = first after the default layer) or 'after' (a layer "
      "id).");
  }

  const auto customLayers = worldNode.customLayersUserSorted();
  const auto bottom = customLayers.size() + 1;

  auto targetPosition = bottom;
  if (const auto position = args.getOptional<int64_t>("position"))
  {
    if (*position < 1 || size_t(*position) > bottom)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Position " + std::to_string(*position)
          + " is out of range; it must be between 1 and " + std::to_string(bottom) + ".",
        "Position 0 is the default layer, which always comes first. Omit 'position' to "
        "add the layer at the end.");
    }
    targetPosition = size_t(*position);
  }
  else if (const auto after = args.getOptional<std::string>("after"))
  {
    auto afterLayer = resolveLayer(context, *after);
    if (afterLayer.is_error())
    {
      return errorOf(afterLayer);
    }
    targetPosition = layerPosition(map, *afterLayer.value()) + 1;
  }

  if (std::ranges::any_of(
        worldNode.allLayers(), [&](const auto* l) { return l->name() == name; }))
  {
    context.warn(
      "DUPLICATE_LAYER_NAME",
      "Another layer is already named '" + name + "'; use layer ids to tell them apart.");
  }

  // like ui::LayerEditor::onAddLayer: sort the new layer at the bottom of the list
  auto layer = mdl::Layer{name};
  layer.setSortIndex(
    !customLayers.empty() ? customLayers.back()->layer().sortIndex() + 1 : 0);
  auto* layerNode = new mdl::LayerNode{std::move(layer)};

  if (mdl::addNodes(map, {{&worldNode, {layerNode}}}).empty())
  {
    return context.operationFailed("The layer '" + name + "' could not be created.");
  }

  if (targetPosition != bottom)
  {
    mdl::moveLayer(map, layerNode, int(targetPosition) - int(bottom));
  }

  if (args.get<bool>("makeCurrent"))
  {
    mdl::setCurrentLayer(map, layerNode);
  }

  return Json{{"layer", layerJson(map, *layerNode, context.ids())}};
}

ToolResult layerRename(CallContext& context, const Args& args)
{
  auto layerNode = layerArgument(context, args);
  if (layerNode.is_error())
  {
    return errorOf(layerNode);
  }
  if (layerNode.value()->isDefaultLayer())
  {
    return defaultLayerError("renamed");
  }

  auto& map = context.map();
  const auto name = args.get<std::string>("name");
  if (layerNode.value()->name() != name)
  {
    mdl::renameLayer(map, layerNode.value(), name);
  }
  return Json{{"layer", layerJson(map, *layerNode.value(), context.ids())}};
}

ToolResult layerRemove(CallContext& context, const Args& args)
{
  auto resolved = layerArgument(context, args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  auto* layerNode = resolved.value();
  if (layerNode->isDefaultLayer())
  {
    return defaultLayerError("removed");
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto layerId = ids.format(*layerNode);
  if (!hasOtherVisibleAndUnlockedLayer(map, *layerNode))
  {
    return makeError(
      ErrorCode::OperationFailed,
      "Layer " + layerId
        + " cannot be removed because no other layer is visible and unlocked.",
      "Show or unlock another layer first, e.g. layer_set_state {\"layer\": "
      "\"layer:default\", \"hidden\": false, \"locked\": false}.",
      {layerId});
  }

  auto* defaultLayerNode = map.worldNode().defaultLayer();

  // like ui::LayerEditor::onRemoveLayer
  mdl::deselectAll(map);
  auto moved = std::vector<std::string>{};
  if (layerNode->hasChildren())
  {
    const auto children = layerNode->children();
    for (const auto* child : children)
    {
      moved.push_back(ids.format(*child));
    }
    if (!mdl::reparentNodes(map, {{defaultLayerNode, children}}))
    {
      return context.operationFailed(
        "The objects of layer " + layerId + " could not be moved to the default layer.");
    }
  }

  if (map.editorContext().currentLayer() == layerNode)
  {
    mdl::setCurrentLayer(map, defaultLayerNode);
  }

  mdl::removeNodes(map, {layerNode});

  const auto* currentLayer = map.editorContext().currentLayer();
  return Json{
    {"removed", layerId},
    {"movedToDefaultLayer", moved},
    {"current", currentLayer ? Json(ids.format(*currentLayer)) : Json(nullptr)},
  };
}

ToolResult layerReorder(CallContext& context, const Args& args)
{
  auto resolved = layerArgument(context, args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  auto* layerNode = resolved.value();
  if (layerNode->isDefaultLayer())
  {
    return defaultLayerError("moved; it always comes first");
  }

  if (args.has("position") == args.has("offset"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass exactly one of 'position' and 'offset'.",
      "'position' is the target position (1 = first after the default layer), "
      "'offset' moves the layer up (negative) or down (positive).");
  }

  auto& map = context.map();
  const auto customCount = map.worldNode().customLayers().size();
  const auto position = int(layerPosition(map, *layerNode));
  const auto offset = args.has("offset") ? int(args.get<int64_t>("offset"))
                                         : int(args.get<int64_t>("position")) - position;

  if (!mdl::canMoveLayer(map, layerNode, offset))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Layer " + context.ids().format(*layerNode) + " at position "
        + std::to_string(position) + " cannot move to position "
        + std::to_string(position + offset) + "; custom layers occupy positions 1 to "
        + std::to_string(customCount) + ".",
      "Use layers_list to see the positions.",
      {context.ids().format(*layerNode)});
  }

  if (offset != 0)
  {
    mdl::moveLayer(map, layerNode, offset);
  }
  return Json{{"layer", layerJson(map, *layerNode, context.ids())}};
}

ToolResult layerSetState(CallContext& context, const Args& args)
{
  auto resolved = layerArgument(context, args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  auto* layerNode = resolved.value();

  auto& map = context.map();
  const auto layerId = context.ids().format(*layerNode);

  const auto current = args.getOptional<bool>("current");
  const auto hidden = args.getOptional<bool>("hidden");
  const auto locked = args.getOptional<bool>("locked");
  const auto omitFromExport = args.getOptional<bool>("omitFromExport");
  const auto isolate = args.getOptional<bool>("isolate");

  if (!current && !hidden && !locked && !omitFromExport && !isolate)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "No state was given.",
      "Pass at least one of 'current', 'hidden', 'locked', 'omitFromExport' or "
      "'isolate'.");
  }
  if (isolate == false)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'isolate' can only be true.",
      "To show the other layers again, set 'hidden': false on them, or pass "
      "'isolate': true for another layer.");
  }
  if (isolate && hidden == true)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "A layer cannot be isolated and hidden at the same time.",
      "Pass either 'isolate': true or 'hidden': true.");
  }
  if (current == false && map.editorContext().currentLayer() == layerNode)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Layer " + layerId
        + " is the current layer; there is always a current layer, so it cannot be "
          "unset.",
      "Make another layer current instead, e.g. layer_set_state {\"layer\": "
      "\"layer:default\", \"current\": true}.",
      {layerId});
  }

  const auto nodes = std::vector<mdl::Node*>{layerNode};

  if (isolate && mdl::canIsolateLayers(map, {layerNode}))
  {
    mdl::isolateLayers(map, {layerNode});
  }

  // like ui::LayerEditor::toggleLayerVisible and toggleLayerLocked
  if (hidden == true && !layerNode->hidden())
  {
    mdl::hideNodes(map, nodes);
  }
  else if (hidden == false && !layerNode->visible())
  {
    mdl::resetNodeVisibility(map, nodes);
  }

  if (locked == true && !layerNode->locked())
  {
    mdl::lockNodes(map, nodes);
  }
  else if (locked == false && layerNode->locked())
  {
    mdl::resetNodeLockingState(map, nodes);
  }

  if (omitFromExport && *omitFromExport != layerNode->layer().omitFromExport())
  {
    mdl::setOmitLayerFromExport(map, layerNode, *omitFromExport);
  }

  if (current == true && mdl::canSetCurrentLayer(map, layerNode))
  {
    mdl::setCurrentLayer(map, layerNode);
  }

  warnAboutCurrentLayer(context);
  return Json{{"layer", layerJson(map, *layerNode, context.ids())}};
}

/** Resolves explicit ids without requiring the objects to be visible or unlocked. */
Result<std::vector<mdl::Node*>, ToolError> resolveAnyObjects(
  CallContext& context, const std::vector<std::string>& idList)
{
  auto nodes = std::vector<mdl::Node*>{};
  for (const auto& id : idList)
  {
    auto node = context.ids().resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    if (!kdl::vec_contains(nodes, node.value()))
    {
      nodes.push_back(node.value());
    }
  }
  return nodes;
}

/** An error for the first object that is inside a group; they cannot change layers. */
std::optional<ToolError> groupedObjectError(
  const IdRegistry& ids, const std::vector<mdl::Node*>& nodes)
{
  for (const auto* node : nodes)
  {
    if (const auto* groupNode = mdl::findContainingGroup(node))
    {
      const auto id = ids.format(*node);
      const auto groupId = ids.format(*groupNode);
      return makeError(
        ErrorCode::InvalidArgument,
        "Object " + id + " is inside group " + groupId
          + "; objects inside groups belong to the group's layer.",
        "Move the group " + groupId
          + " instead, or take the object out of the group first "
            "(group_remove_objects).",
        {id, groupId});
    }
  }
  return std::nullopt;
}

/** The nodes that moveSelectedNodesToLayer moves: brushes of brush entities move with
 * their entity. */
std::vector<mdl::Node*> nodesToMove(const std::vector<mdl::Node*>& targets)
{
  auto result = std::vector<mdl::Node*>{};
  for (auto* node : targets)
  {
    auto* moved = node;
    const auto kind = objectKindOf(*node);
    if (kind == ObjectKind::Brush || kind == ObjectKind::Patch)
    {
      if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node->parent()))
      {
        moved = entityNode;
      }
    }
    if (!kdl::vec_contains(result, moved))
    {
      result.push_back(moved);
    }
  }
  return result;
}

ToolResult objectsMoveToLayer(CallContext& context, const Args& args)
{
  auto resolved = layerArgument(context, args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  auto* layerNode = resolved.value();

  auto& map = context.map();
  auto& ids = context.ids();
  const auto layerId = ids.format(*layerNode);

  // objects in groups are not selectable while the group is closed, so check this first
  if (const auto explicitIds = args.getOptional<std::vector<std::string>>("ids"))
  {
    auto nodes = resolveAnyObjects(context, *explicitIds);
    if (nodes.is_error())
    {
      return errorOf(nodes);
    }
    if (auto error = groupedObjectError(ids, nodes.value()))
    {
      return *error;
    }
  }

  auto targets = resolveTargets(context, args, "ids", ObjectKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  if (auto error = groupedObjectError(ids, targets.value()))
  {
    return *error;
  }

  const auto moved = nodesToMove(targets.value());
  auto movedIds = std::vector<std::string>{};
  for (auto* node : moved)
  {
    if (mdl::findContainingLayer(node) != layerNode)
    {
      movedIds.push_back(ids.format(*node));
    }
  }

  if (movedIds.empty())
  {
    context.warn("NO_CHANGE", "All objects are already in layer " + layerId + ".");
    return Json{{"layer", layerId}, {"moved", Json::array()}};
  }

  auto result = withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      mdl::moveSelectedNodesToLayer(map, layerNode);
      return Json{{"layer", layerId}, {"moved", movedIds}};
    },
    SelectionAfter::Result);

  if (result.is_success() && (layerNode->hidden() || layerNode->locked()))
  {
    context.warn(
      layerNode->hidden() ? "LAYER_HIDDEN" : "LAYER_LOCKED",
      "Layer " + layerId + " is " + (layerNode->hidden() ? "hidden" : "locked")
        + ", so the moved objects were deselected and cannot be edited until it is "
          "shown and unlocked (layer_set_state).",
      {layerId});
  }
  return result;
}

size_t hiddenObjectCount(mdl::Map& map)
{
  const auto nodes = mdl::collectDescendants(
    kdl::vec_static_cast<mdl::Node*>(map.worldNode().allLayers()));
  return size_t(
    std::ranges::count_if(nodes, [](const auto* node) { return !node->visible(); }));
}

ToolResult visibilitySet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto mode = args.get<std::string>("mode");
  const auto explicitIds = args.getOptional<std::vector<std::string>>("ids");

  const auto finish = [&](const std::vector<mdl::Node*>& nodes) -> ToolResult {
    auto affected = std::vector<std::string>{};
    for (const auto* node : nodes)
    {
      affected.push_back(ids.format(*node));
    }
    return Json{
      {"mode", mode},
      {"ids", affected},
      {"hiddenObjects", hiddenObjectCount(map)},
    };
  };

  if (mode == "show_all")
  {
    if (explicitIds)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "'show_all' takes no ids.",
        "Remove 'ids', or use mode 'show' to show specific objects.");
    }
    mdl::showAllNodes(map);

    auto hiddenLayers = std::vector<std::string>{};
    for (const auto* layerNode : map.worldNode().allLayersUserSorted())
    {
      if (layerNode->hidden())
      {
        hiddenLayers.push_back(ids.format(*layerNode));
      }
    }
    if (!hiddenLayers.empty())
    {
      context.warn(
        "HIDDEN_LAYERS",
        "show_all shows all objects but not hidden layers; these layers stay hidden. "
        "Show them with layer_set_state {\"hidden\": false}.",
        hiddenLayers);
    }
    return finish({});
  }

  if (mode == "isolate")
  {
    auto targets = resolveTargets(context, args, "ids", ObjectKinds);
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    // like the editor, the isolated objects stay selected
    return withTargets(
      context,
      targets.value(),
      [&]() -> ToolResult {
        mdl::isolateSelectedNodes(map);
        return finish(targets.value());
      },
      SelectionAfter::Result);
  }

  if (mode == "show" && !explicitIds)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Mode 'show' needs ids; hidden objects cannot be selected.",
      "Pass the ids of the hidden objects (objects_find {\"visible\": false}), or use "
      "mode 'show_all'.");
  }

  auto nodes = std::vector<mdl::Node*>{};
  if (explicitIds)
  {
    auto resolved = resolveAnyObjects(context, *explicitIds);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    nodes = std::move(resolved).value();
  }
  else
  {
    auto targets = resolveTargets(context, args, "ids", ObjectKinds);
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    nodes = std::move(targets).value();
  }

  if (mode == "hide")
  {
    mdl::hideNodes(map, nodes);
  }
  else
  {
    // undo explicit hiding, then force objects in hidden containers visible
    auto hiddenNodes = std::vector<mdl::Node*>{};
    std::ranges::copy_if(nodes, std::back_inserter(hiddenNodes), [](const auto* node) {
      return node->visibilityState() == mdl::VisibilityState::Hidden;
    });
    if (!hiddenNodes.empty())
    {
      mdl::resetNodeVisibility(map, hiddenNodes);
    }
    mdl::ensureNodesVisible(map, nodes);
  }
  return finish(nodes);
}

// Schemas

Schema layerResultSchema()
{
  return object({field("layer", layerSchema()).required()});
}

Schema visibilityResultSchema()
{
  return object({
    field("mode", enumOf({"hide", "show", "isolate", "show_all"}))
      .required()
      .describe("The mode that was applied"),
    field("ids", array(objectId())).required().describe("The objects acted on"),
    field("hiddenObjects", integer())
      .required()
      .describe("Number of objects in the map that are not visible now"),
  });
}

} // namespace

void registerLayerTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"layers_list"}
      .title("List Layers")
      .description(
        "Lists all layers (read-only) in the order of the layer list, the default layer "
        "('layer:default', position 0) first: id, name, position, sortIndex, whether it "
        "is the default or current layer (where new objects go), hidden, locked, "
        "omitFromExport, color and object counts by kind, plus the current layer's id. "
        "objects_find with 'layer' lists a layer's objects. Example: {}")
      .input(object({}))
      .output(object({
        field("layers", array(layerSchema())).required(),
        field("current", any()).describe("The id of the current layer"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(layersList));

  registry.add(
    ToolDef{"layer_create"}
      .title("Create Layer")
      .description(
        "Creates an empty layer (one undo step), by default at the end of the layer "
        "list, and makes it the current layer (new objects are added to it) like the "
        "editor's Add Layer button; makeCurrent: false keeps the current layer. "
        "'position' (1 = first after the default layer) or 'after' (a layer id) place "
        "it elsewhere; pass at most one. A duplicate name only warns. Returns the new "
        "layer. Example: {\"name\": \"Lighting\", \"after\": \"layer:default\"}")
      .input(object({
        field("name", string().nonEmpty()).required().describe("Layer name"),
        field("position", integer().min(1))
          .describe("Position in the layer list (1 = first after the default layer)"),
        field("after", objectId({ObjectKind::Layer}))
          .describe("Insert after this layer; not with 'position'"),
        field("makeCurrent", boolean().defaultsTo(true))
          .describe("Make the new layer the current layer"),
      }))
      .output(layerResultSchema())
      .mutation(Mutation::Map)
      .handler(layerCreate));

  registry.add(
    ToolDef{"layer_rename"}
      .title("Rename Layer")
      .description("Renames a layer (one undo step) and returns it. The default layer "
                   "cannot be renamed. "
                   "Example: {\"layer\": \"layer:12\", \"name\": \"Upper Floor\"}")
      .input(object({
        field("layer", objectId({ObjectKind::Layer}))
          .required()
          .describe("Layer id from layers_list"),
        field("name", string().nonEmpty()).required().describe("New name"),
      }))
      .output(layerResultSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(layerRename));

  registry.add(
    ToolDef{"layer_remove"}
      .title("Remove Layer")
      .description(
        "Removes a layer and moves its objects to the default layer, like the editor's "
        "Remove Layer button (one undo step). Clears the selection. If the layer was "
        "current, the default layer becomes current. The default layer cannot be "
        "removed, and like in the editor another layer must be visible and unlocked. "
        "Example: {\"layer\": \"layer:12\"}")
      .input(object({
        field("layer", objectId({ObjectKind::Layer}))
          .required()
          .describe("Layer id from layers_list"),
      }))
      .output(object({
        field("removed", objectId({ObjectKind::Layer}))
          .required()
          .describe("Id of the removed layer"),
        field("movedToDefaultLayer", array(objectId()))
          .required()
          .describe("The objects that were moved to the default layer"),
        field("current", any()).describe("The id of the current layer"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(layerRemove));

  registry.add(
    ToolDef{"layer_reorder"}
      .title("Reorder Layer")
      .description(
        "Moves a layer in the layer list (one undo step), either to 'position' (1 = "
        "first after the default layer) or by 'offset' (negative moves up, positive "
        "down); pass exactly one. The default layer always stays first. Returns the "
        "layer with its new position. Example: {\"layer\": \"layer:12\", \"offset\": "
        "-1}")
      .input(object({
        field("layer", objectId({ObjectKind::Layer}))
          .required()
          .describe("Custom layer id from layers_list"),
        field("position", integer().min(1))
          .describe("Target position (1 = first after the default layer)"),
        field("offset", integer()).describe("Positions to move; negative moves up"),
      }))
      .output(layerResultSchema())
      .mutation(Mutation::Map)
      .handler(layerReorder));

  registry.add(
    ToolDef{"layer_set_state"}
      .title("Set Layer State")
      .description(
        "Sets any of a layer's states in one undo step: current (new objects are added "
        "to the current layer; it can only be set to true), hidden, locked, "
        "omitFromExport and isolate (true: show this layer and hide all others). Pass "
        "at least one. Hiding or locking a layer deselects its objects. Hiding or "
        "locking the current layer is allowed, like in the editor, but warns. Returns "
        "the layer. Example: {\"layer\": \"layer:12\", \"hidden\": false, "
        "\"locked\": true}")
      .input(object({
        field("layer", objectId({ObjectKind::Layer}))
          .required()
          .describe("Layer id from layers_list"),
        field("current", boolean())
          .describe("true: make this the current layer (false is refused)"),
        field("hidden", boolean()).describe("Hide (true) or show (false) the layer"),
        field("locked", boolean()).describe("Lock (true) or unlock (false) the layer"),
        field("omitFromExport", boolean())
          .describe("Leave the layer out when compiling or exporting"),
        field("isolate", boolean())
          .describe("true: show only this layer (false is refused)"),
      }))
      .output(layerResultSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(layerSetState));

  registry.add(
    ToolDef{"objects_move_to_layer"}
      .title("Move Objects to Layer")
      .description(
        "Moves groups, entities, brushes and patches (ids, default: the current "
        "selection) to a layer, like the editor's Move Selection to Layer (one undo "
        "step). A brush of a brush entity moves the whole entity; objects inside groups "
        "cannot be moved on their own (move the group). The moved objects are left "
        "selected unless the layer is hidden or locked. "
        "Example: {\"ids\": [\"brush:12\", \"entity:40\"], \"layer\": \"layer:7\"}")
      .input(object({
        idsField(ObjectKinds),
        field("layer", objectId({ObjectKind::Layer})).required().describe("Target layer"),
      }))
      .output(object({
        field("layer", objectId({ObjectKind::Layer})).required().describe("Target layer"),
        field("moved", array(objectId()))
          .required()
          .describe("The objects that were moved (brush entities for their brushes)"),
      }))
      .mutation(Mutation::Map)
      .handler(objectsMoveToLayer));

  registry.add(
    ToolDef{"visibility_set"}
      .title("Set Visibility")
      .description(
        "Hides or shows objects (one undo step). 'hide' hides the objects (ids, "
        "default: the current selection) and deselects them. 'show' shows hidden "
        "objects (ids required), even inside a hidden layer or group. 'isolate' hides "
        "all other objects (ids, default: the current selection; they stay selected), "
        "like View > Isolate. 'show_all' takes no ids and shows all objects, like View "
        "> Show All; hidden layers stay hidden (use layer_set_state). Returns the number "
        "of hidden objects. Examples: {\"mode\": \"hide\", \"ids\": [\"brush:12\"]}; "
        "{\"mode\": \"show_all\"}")
      .input(object({
        field("mode", enumOf({"hide", "show", "isolate", "show_all"}))
          .required()
          .describe("'hide', 'show', 'isolate' (hide all others) or 'show_all'"),
        idsField(
          ObjectKinds, "Object ids. Default: the current selection (not for show)"),
      }))
      .output(visibilityResultSchema())
      .mutation(Mutation::Map)
      .handler(visibilitySet));
}

} // namespace tb::mcp
