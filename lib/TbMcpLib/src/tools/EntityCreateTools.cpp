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

#include "mcp/tools/EntityCreateTools.h"

#include "EntityUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/Tag.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** Shrinks boxes for overlap tests and insets floor rays, like space_check. */
constexpr auto Epsilon = 0.01;
/** A point entity whose bounds are this close above a floor stands on it. */
constexpr auto SupportTolerance = 1.0;

using PropertyList = std::vector<std::pair<std::string, std::string>>;

ToolError invalidArgument(std::string message, std::string hint = {})
{
  return makeError(ErrorCode::InvalidArgument, std::move(message), std::move(hint));
}

/** Formats a number as a property value: integers without decimals. */
std::string formatNumber(const Json& value)
{
  if (value.is_number_integer())
  {
    return std::to_string(value.get<int64_t>());
  }
  const auto rounded = roundForOutput(value.get<double>());
  return rounded == std::floor(rounded) && std::abs(rounded) < 1e15
           ? std::to_string(int64_t(rounded))
           : fmt::format("{}", rounded);
}

/**
 * The property value given as JSON: strings as they are, numbers formatted, booleans as
 * "1" / "0" and arrays of numbers as space separated numbers ("255 128 0").
 */
std::optional<std::string> propertyValue(const Json& value)
{
  if (value.is_string())
  {
    return value.get<std::string>();
  }
  if (value.is_number())
  {
    return formatNumber(value);
  }
  if (value.is_boolean())
  {
    return value.get<bool>() ? "1" : "0";
  }
  if (value.is_array())
  {
    auto result = std::string{};
    for (const auto& element : value)
    {
      if (!element.is_number())
      {
        return std::nullopt;
      }
      result += (result.empty() ? "" : " ") + formatNumber(element);
    }
    return result;
  }
  return std::nullopt;
}

bool containsQuote(const std::string& str)
{
  return str.find('"') != std::string::npos;
}

Result<std::string, ToolError> classnameArgument(const Args& args)
{
  auto classname = args.get<std::string>("classname");
  if (containsQuote(classname) || classname.find_first_of(" \t\n") != std::string::npos)
  {
    return invalidArgument(
      "The classname '" + classname + "' must not contain quotes or whitespace.",
      "Use entity_classes_list to see the available classes.");
  }
  return classname;
}

/**
 * The properties of the argument `properties` as key-value strings. The classname is
 * given by its own argument; `origin` is rejected if `allowOrigin` is false (point
 * entities take `position`).
 */
Result<PropertyList, ToolError> propertiesArgument(
  const Args& args, const bool allowOrigin)
{
  auto result = PropertyList{};
  const auto properties = args.getOptional<Json>("properties");
  if (!properties)
  {
    return result;
  }

  for (const auto& item : properties->items())
  {
    const auto& key = item.key();
    if (key.empty() || containsQuote(key))
    {
      return invalidArgument(
        "Property key '" + key + "' is invalid: keys must be non-empty and must not "
        "contain quotes, which map files cannot store.");
    }
    if (key == mdl::EntityPropertyKeys::Classname)
    {
      return invalidArgument(
        "Set the classname with the argument 'classname', not in 'properties'.",
        "Remove 'classname' from 'properties'.");
    }
    if (!allowOrigin && key == mdl::EntityPropertyKeys::Origin)
    {
      return invalidArgument(
        "Set the origin of a point entity with the argument 'position', not in "
        "'properties'.",
        "Remove 'origin' from 'properties' and pass 'position': [x, y, z].");
    }

    auto value = propertyValue(item.value());
    if (!value)
    {
      return invalidArgument(
        "The value of property '" + key + "' must be a string, a number, a boolean or "
        "an array of numbers.",
        "Example: {\"light\": 300, \"_color\": [255, 128, 0], \"message\": \"Hello\"}");
    }
    if (containsQuote(*value))
    {
      return invalidArgument(
        "The value of property '" + key + "' contains a quote, which map files cannot "
        "store.",
        "Use single quotes instead.");
    }
    result.emplace_back(key, std::move(*value));
  }
  return result;
}

Json propertiesJson(const mdl::Entity& entity)
{
  auto result = Json::object();
  for (const auto& property : entity.properties())
  {
    result[property.key()] = property.value();
  }
  return result;
}

/**
 * Validates the properties against the definition (X14 warnings only) and sets them on
 * the given entity. With `applyDefaults`, the definition's defaults of the properties
 * that are still missing are set afterwards.
 */
ToolResult setProperties(
  CallContext& context,
  mdl::EntityNode& entityNode,
  const mdl::EntityDefinition* definition,
  const PropertyList& properties,
  const bool applyDefaults = false)
{
  if (properties.empty() && !applyDefaults)
  {
    return Json::object();
  }

  auto& map = context.map();
  const auto id = context.ids().format(entityNode);
  return withEntities(context, {&entityNode}, [&]() -> ToolResult {
    for (const auto& [key, value] : properties)
    {
      validateProperty(context, definition, key, value, {id});
      if (!mdl::setEntityProperty(map, key, value))
      {
        return context.operationFailed("Property '" + key + "' could not be set.");
      }
    }
    if (applyDefaults)
    {
      mdl::setDefaultEntityProperties(map, mdl::SetDefaultPropertyMode::SetMissing);
    }
    return Json::object();
  });
}

/**
 * The entities that become empty, and are therefore removed by the editor, when the
 * given brushes are moved out of them (to `target`, which is never counted).
 */
std::vector<std::string> entitiesEmptiedBy(
  const IdRegistry& ids, const std::vector<mdl::Node*>& nodes, const mdl::Node* target)
{
  auto result = std::vector<std::string>{};
  auto seen = std::vector<const mdl::Node*>{};
  for (const auto* node : nodes)
  {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node->parent());
    if (
      !entityNode || entityNode == target
      || std::ranges::find(seen, entityNode) != seen.end())
    {
      continue;
    }
    seen.push_back(entityNode);
    if (std::ranges::all_of(entityNode->children(), [&](const auto* child) {
          return std::ranges::find(nodes, child) != nodes.end();
        }))
    {
      result.push_back(ids.format(*entityNode));
    }
  }
  return result;
}

// entity_create_point

/**
 * The highest floor below the given bounds: vertical rays downwards from the height of
 * the bounds center at the center and the inset corners, against visible brushes and
 * patches that are not triggers.
 */
std::optional<RayHit> findFloor(mdl::Map& map, const vm::bbox3d& bounds)
{
  const auto& editorContext = map.editorContext();
  const auto accept = [&](const mdl::Node& node) {
    if (isPointEntity(node) || !editorContext.visible(node))
    {
      return false;
    }
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    return !brushNode || classifyBrush(*brushNode) != BrushClass::Trigger;
  };

  const auto inset = std::min(
    {Epsilon,
     (bounds.max.x() - bounds.min.x()) / 4.0,
     (bounds.max.y() - bounds.min.y()) / 4.0});
  const auto center = bounds.center();
  const auto z = center.z();
  const auto samples = std::array{
    vm::vec3d{center.x(), center.y(), z},
    vm::vec3d{bounds.min.x() + inset, bounds.min.y() + inset, z},
    vm::vec3d{bounds.max.x() - inset, bounds.min.y() + inset, z},
    vm::vec3d{bounds.min.x() + inset, bounds.max.y() - inset, z},
    vm::vec3d{bounds.max.x() - inset, bounds.max.y() - inset, z},
  };

  auto best = std::optional<RayHit>{};
  for (const auto& sample : samples)
  {
    const auto hits = castRay(map, vm::ray3d{sample, vm::vec3d{0, 0, -1}}, accept);
    if (!hits.empty() && (!best || hits.front().point.z() > best->point.z()))
    {
      best = hits.front();
    }
  }
  return best;
}

Json floorJson(
  CallContext& context, const RayHit& hit, const double movedDown, const bool model)
{
  const auto& ids = context.ids();
  auto result = Json{
    {"z", roundForOutput(hit.point.z())},
    {"object", ids.format(*hit.node)},
    {"face", nullptr},
    {"distance", roundForOutput(movedDown)},
    {"bounds", model ? "model" : "definition"},
  };
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(hit.node);
      brushNode && hit.faceIndex)
  {
    result["face"] = ids.formatFace(*brushNode, *hit.faceIndex);
  }
  return result;
}

/** The solid and brush entity brushes whose interior intersects the given bounds. */
std::vector<std::string> overlappingBrushes(
  CallContext& context, const vm::bbox3d& bounds)
{
  auto& map = context.map();
  const auto inner = bounds.expand(-Epsilon);
  auto result = std::vector<std::string>{};
  for (auto* node : map.worldNode().nodeTree().find_intersectors(inner))
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (
      brushNode && classifyBrush(*brushNode) != BrushClass::Trigger
      && intersectsInterior(brushNode->brush(), inner))
    {
      result.push_back(context.ids().format(*brushNode));
    }
  }
  std::ranges::sort(result);
  return result;
}

/** Adds a point entity of a class without definition, like mdl::createPointEntity. */
mdl::EntityNode* createUndefinedPointEntity(
  mdl::Map& map, const std::string& classname, const vm::vec3d& origin)
{
  auto entity = mdl::Entity{{{mdl::EntityPropertyKeys::Classname, classname}}};
  entity.setOrigin(origin);
  auto* entityNode = new mdl::EntityNode{std::move(entity)};

  auto transaction = mdl::Transaction{map, "Create " + classname};
  mdl::deselectAll(map);
  if (mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}}).empty())
  {
    transaction.cancel();
    return nullptr;
  }
  mdl::selectNodes(map, {entityNode});
  return transaction.commit() ? entityNode : nullptr;
}

ToolResult entityCreatePoint(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& grid = map.grid();

  const auto classnameResult = classnameArgument(args);
  if (classnameResult.is_error())
  {
    return errorOf(classnameResult);
  }
  const auto& classname = classnameResult.value();

  auto propertiesResult = propertiesArgument(args, false);
  if (propertiesResult.is_error())
  {
    return errorOf(propertiesResult);
  }
  auto properties = std::move(propertiesResult).value();
  if (const auto angle = args.getOptional<double>("angle"))
  {
    if (std::ranges::any_of(properties, [](const auto& property) {
          return property.first == mdl::EntityPropertyKeys::Angle;
        }))
    {
      return invalidArgument(
        "The angle is given twice, as 'angle' and in 'properties'.",
        "Pass only one of them.");
    }
    properties.emplace_back(mdl::EntityPropertyKeys::Angle, formatNumber(Json(*angle)));
  }

  const auto* definition = findEntityDefinition(map, classname);
  if (definition && mdl::getType(*definition) != mdl::EntityDefinitionType::Point)
  {
    return invalidArgument(
      "'" + classname + "' is a brush entity class; it needs brushes.",
      "Use entity_create_brush with the brushes that should make up the entity.");
  }
  auto models = EntityModelLoader{map};
  const auto dropToFloor = args.get<bool>("dropToFloor");
  auto position = args.get<vm::vec3d>("position");
  if (args.get<bool>("snapToGrid"))
  {
    position[0] = grid.snap(position.x());
    position[1] = grid.snap(position.y());
    if (!dropToFloor)
    {
      position[2] = grid.snap(position.z());
    }
  }

  auto floor = std::optional<RayHit>{};
  auto movedDown = 0.0;
  auto dropBounds = DropBounds{};
  if (dropToFloor)
  {
    auto bounds = dropToFloorBounds(
      definition,
      classname,
      properties,
      args.get<bool>("applyDefaults"),
      args.get<std::string>("dropUsing"),
      models);
    if (bounds.is_error())
    {
      return errorOf(bounds);
    }
    dropBounds = std::move(bounds).value();
    const auto& localBounds = dropBounds.bounds;
    floor = findFloor(map, localBounds.translate(position));
    if (!floor)
    {
      return invalidArgument(
        "There is no floor below " + toJson(position).dump() + ".",
        "Move the position above a floor, or find one with ray_pick (direction [0, 0, "
        "-1]).");
    }
    const auto z = floor->point.z() - localBounds.min.z();
    movedDown = position.z() - z;
    position[2] = z;
  }

  auto* entityNode = definition ? mdl::createPointEntity(map, *definition, position)
                                : createUndefinedPointEntity(map, classname, position);
  if (!entityNode)
  {
    return context.operationFailed("The entity could not be created.");
  }

  const auto id = context.ids().format(*entityNode);
  if (!definition)
  {
    warnUnknownClassname(context, classname, {id});
  }

  if (auto result = setProperties(
        context, *entityNode, definition, properties, args.get<bool>("applyDefaults"));
      result.is_error())
  {
    return result;
  }

  const auto bounds = entityNode->logicalBounds();
  const auto overlaps = overlappingBrushes(context, bounds);
  if (!overlaps.empty())
  {
    auto brushIds = std::string{};
    for (const auto& brushId : overlaps)
    {
      brushIds += (brushIds.empty() ? "" : ", ") + brushId;
    }
    auto warningIds = overlaps;
    warningIds.insert(warningIds.begin(), id);
    context.warn(
      "ENTITY_OVERLAPS_BRUSHES",
      "The entity " + id + " intersects the brushes " + brushIds
        + "; move it into open space or use dropToFloor.",
      std::move(warningIds));
  }
  warnModelPlacement(context, {entityNode}, models);
  if (!map.worldBounds().contains(bounds))
  {
    context.warn(
      "OUTSIDE_WORLD_BOUNDS",
      "The entity " + id + " lies (partly) outside the world bounds.",
      {id});
  }

  const auto support = findFloor(map, bounds);
  const auto onFloor =
    support && std::abs(bounds.min.z() - support->point.z()) <= SupportTolerance;

  return Json{
    {"entity", id},
    {"classname", classname},
    {"origin", toJson(entityNode->entity().origin())},
    {"bounds", toJson(bounds)},
    {"floor",
     floor ? floorJson(context, *floor, movedDown, dropBounds.model) : Json(nullptr)},
    {"onFloor", bool(onFloor)},
    {"overlaps", overlaps},
    {"properties", propertiesJson(entityNode->entity())},
  };
}

// entity_create_brush

/**
 * Turns the selected brushes into an entity of a class without definition, like
 * mdl::createBrushEntity.
 */
mdl::EntityNode* createUndefinedBrushEntity(mdl::Map& map, const std::string& classname)
{
  const auto nodes = map.selection().nodes;

  // like the editor: keep the properties of the entity that contains all brushes
  const auto* containingEntity = mdl::findContainingEntity(nodes.front());
  auto entity = containingEntity && containingEntity != &map.worldNode()
                    && std::ranges::all_of(
                      nodes,
                      [&](const auto* node) {
                        return mdl::findContainingEntity(node) == containingEntity;
                      })
                  ? containingEntity->entity()
                  : mdl::Entity{};
  entity.addOrUpdateProperty(mdl::EntityPropertyKeys::Classname, classname);
  auto* entityNode = new mdl::EntityNode{std::move(entity)};

  auto transaction = mdl::Transaction{map, "Create " + classname};
  mdl::deselectAll(map);
  if (
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}}).empty()
    || !mdl::reparentNodes(map, {{entityNode, nodes}}))
  {
    transaction.cancel();
    return nullptr;
  }
  mdl::selectNodes(map, nodes);
  return transaction.commit() ? entityNode : nullptr;
}

ToolResult entityCreateBrush(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();

  const auto classnameResult = classnameArgument(args);
  if (classnameResult.is_error())
  {
    return errorOf(classnameResult);
  }
  const auto& classname = classnameResult.value();

  const auto properties = propertiesArgument(args, true);
  if (properties.is_error())
  {
    return errorOf(properties);
  }

  const auto* definition = findEntityDefinition(map, classname);
  if (definition && mdl::getType(*definition) != mdl::EntityDefinitionType::Brush)
  {
    return invalidArgument(
      "'" + classname + "' is a point entity class; it cannot contain brushes.",
      "Use entity_create_point with a position.");
  }

  const auto nodes =
    resolveTargets(context, args, "ids", {ObjectKind::Brush, ObjectKind::Patch});
  if (nodes.is_error())
  {
    return errorOf(nodes);
  }

  const auto removedEntities = entitiesEmptiedBy(ids, nodes.value(), nullptr);
  const auto brushIds = formatIds(nodes.value(), ids);

  auto* entityNode = static_cast<mdl::EntityNode*>(nullptr);
  if (auto result = withTargets(
        context,
        nodes.value(),
        [&]() -> ToolResult {
          entityNode = definition ? mdl::createBrushEntity(map, *definition)
                                  : createUndefinedBrushEntity(map, classname);
          if (!entityNode)
          {
            return context.operationFailed(
              "The brush entity could not be created.", "Check the editor messages.");
          }
          return Json::object();
        },
        SelectionAfter::Result);
      result.is_error())
  {
    return result;
  }

  const auto id = ids.format(*entityNode);
  if (!definition)
  {
    warnUnknownClassname(context, classname, {id});
  }

  if (auto result = setProperties(context, *entityNode, definition, properties.value());
      result.is_error())
  {
    return result;
  }

  return Json{
    {"entity", id},
    {"classname", classname},
    {"brushes", brushIds},
    {"bounds", toJson(entityNode->logicalBounds())},
    {"removedEntities", removedEntities},
    {"properties", propertiesJson(entityNode->entity())},
  };
}

// entity_move_brushes

/** Make Structural never enables tags, so it never asks for an option. */
class FirstOptionCallback : public mdl::TagMatcherCallback
{
public:
  size_t selectOption(const std::vector<std::string>&) override { return 0; }
};

ToolResult entityMoveBrushes(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();

  const auto nodes =
    resolveTargets(context, args, "ids", {ObjectKind::Brush, ObjectKind::Patch});
  if (nodes.is_error())
  {
    return errorOf(nodes);
  }

  const auto targetId = args.get<std::string>("entity");
  const auto target = ids.resolve(targetId);
  if (target.is_error())
  {
    return errorOf(target);
  }

  const auto toWorld = target.value() == &map.worldNode();
  auto* targetEntity = dynamic_cast<mdl::EntityNode*>(target.value());
  if (!toWorld && (!targetEntity || !targetEntity->hasChildren()))
  {
    return makeError(
      ErrorCode::WrongObjectKind,
      "Object " + targetId + " is not a brush entity; brushes can only be moved into "
        + "brush entities or to the world.",
      "Pass the id of a brush entity (e.g. a func_door), or \"world\"; create a new "
      "brush entity with entity_create_brush.",
      {targetId});
  }

  const auto toMove = [&]() {
    auto result = std::vector<mdl::Node*>{};
    std::ranges::copy_if(
      nodes.value(), std::back_inserter(result), [&](const auto* node) {
        return toWorld ? owningBrushEntity(*node) != nullptr
                       : node->parent() != targetEntity;
      });
    return result;
  }();

  const auto entityId = toWorld ? Json("world") : Json(ids.format(*targetEntity));
  if (toMove.empty())
  {
    context.warn(
      "NOTHING_MOVED",
      "All given brushes already belong to " + entityId.get<std::string>() + ".",
      formatIds(nodes.value(), ids));
    return Json{
      {"entity", entityId},
      {"moved", Json::array()},
      {"removedEntities", Json::array()},
    };
  }

  const auto removedEntities = entitiesEmptiedBy(ids, toMove, targetEntity);
  const auto movedIds = formatIds(toMove, ids);

  if (auto result = withTargets(
        context,
        nodes.value(),
        [&]() -> ToolResult {
          auto callback = FirstOptionCallback{};
          const auto success = toWorld
                                 ? mdl::makeStructural(map, nodes.value(), callback)
                                 : mdl::moveToEntity(map, nodes.value(), *targetEntity);
          if (!success)
          {
            return context.operationFailed(
              "The brushes could not be moved.", "Check the editor messages.");
          }
          return Json::object();
        });
      result.is_error())
  {
    return result;
  }

  return Json{
    {"entity", entityId},
    {"moved", movedIds},
    {"removedEntities", removedEntities},
  };
}

Field propertiesField()
{
  return field("properties", object({}).allowAdditionalProperties())
    .describe(
      "Properties to set, key -> value. Values may be strings, numbers, booleans (1 / "
      "0) or arrays of numbers (written space separated, e.g. [255, 128, 0] -> \"255 "
      "128 0\"). Values are checked against the entity definition; problems are "
      "warnings (UNKNOWN_PROPERTY, INVALID_CHOICE, ...), never errors");
}

} // namespace

void registerEntityCreateTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"entity_create_point"}
      .title("Create Point Entity")
      .description(
        "Creates a point entity (monster, light, item, player start, ...) with its "
        "origin at 'position' in the open group or the current layer, and selects it. "
        "The position is snapped to the grid (snapToGrid). With dropToFloor, the entity "
        "is lowered (or raised) so that the bottom of its bounds rests on the highest "
        "floor below it (visible brushes and patches, triggers ignored; rays start at "
        "the height of the bounds center); no floor fails with INVALID_ARGUMENT. "
        "'angle' sets the yaw in degrees. Properties are validated against the entity "
        "definition (warnings only); applyDefaults sets the definition's defaults of "
        "all other properties. A brush entity class fails (use entity_create_brush); an "
        "unknown class is created anyway with an UNKNOWN_CLASSNAME warning. Warnings: "
        "ENTITY_OVERLAPS_BRUSHES if the entity intersects solid brushes, "
        "OUTSIDE_WORLD_BOUNDS, and for entities whose model can be loaded the model "
        "placement findings of entity_placement_check (MODEL_BELOW_FLOOR, "
        "MODEL_FLOATING, MODEL_PENETRATES_BRUSHES, MODEL_NO_FLOOR). dropUsing chooses "
        "the bounds that rest on the floor: \"model\" (the model bounds of the "
        "animation the properties select, e.g. a sitting pose), \"definition\" (the "
        "class size) or \"auto\" (the model if it can be loaded). 'onFloor' tells "
        "whether the entity stands on a floor (within 1 unit). Example: {\"classname\": "
        "\"monster_ogre\", \"position\": [256, 128, 64], \"angle\": 90, "
        "\"dropToFloor\": true, \"properties\": {\"spawnflags\": 256}}")
      .input(object({
        field("classname", string().nonEmpty())
          .required()
          .describe("Entity class, e.g. 'info_player_start' (see entity_classes_list)"),
        field("position", vec3()).required().describe("Origin [x, y, z] of the entity"),
        propertiesField(),
        field("angle", angle())
          .describe("Yaw in degrees (0 = east / +x, 90 = north / +y); sets 'angle'"),
        field("dropToFloor", boolean().defaultsTo(false))
          .describe("Place the entity on the floor below the position"),
        field("dropUsing", enumOf({"auto", "model", "definition"}).defaultsTo("auto"))
          .describe(
            "Bounds that dropToFloor rests on the floor: the model's bounds in its "
            "current animation, the class size, or auto (model if loadable)"),
        field("applyDefaults", boolean().defaultsTo(false))
          .describe("Set the definition's default values of the missing properties"),
        field("snapToGrid", boolean().defaultsTo(true))
          .describe("Snap the position to the grid (x and y; z too unless dropToFloor)"),
      }))
      .output(object({
        field("entity", objectId()).required().describe("Id of the new entity"),
        field("classname", string()).required(),
        field("origin", vec3()).required(),
        field("bounds", box()).required(),
        field("floor", any())
          .required()
          .describe(
            "With dropToFloor: {z, object, face, distance, bounds} (distance the "
            "entity moved down; bounds \"model\" or \"definition\"); otherwise null"),
        field("onFloor", boolean())
          .required()
          .describe("Whether a floor is within 1 unit below the entity"),
        field("overlaps", array(objectId()))
          .required()
          .describe("Brushes that intersect the entity"),
        field("properties", object({}).allowAdditionalProperties())
          .required()
          .describe("All properties of the new entity"),
      }))
      .mutation(Mutation::Map)
      .handler(entityCreatePoint));

  registry.add(
    ToolDef{"entity_create_brush"}
      .title("Create Brush Entity")
      .description(
        "Turns brushes (and patches) into a brush entity (func_door, trigger_once, "
        "func_detail, ...), like the editor's Create Entity menu, and selects them. "
        "Brushes that belong to another brush entity are moved out of it; entities that "
        "become empty are removed (removedEntities). If all brushes belong to one "
        "entity, its properties are kept. A point entity class fails (use "
        "entity_create_point); an unknown class is created anyway with an "
        "UNKNOWN_CLASSNAME warning. Properties are validated against the entity "
        "definition (warnings only). Example: {\"classname\": \"func_door\", \"ids\": "
        "[\"brush:1042\"], \"properties\": {\"angle\": -1, \"speed\": 200}}")
      .input(object({
        field("classname", string().nonEmpty())
          .required()
          .describe("Brush entity class, e.g. 'func_door' (see entity_classes_list)"),
        idsField(
          {ObjectKind::Brush, ObjectKind::Patch},
          "Brushes and patches of the new entity. Default: the selection (which must "
          "contain only brushes and patches)"),
        propertiesField(),
      }))
      .output(object({
        field("entity", objectId()).required().describe("Id of the new entity"),
        field("classname", string()).required(),
        field("brushes", array(objectId())).required(),
        field("bounds", box()).required(),
        field("removedEntities", array(objectId()))
          .required()
          .describe("Brush entities removed because they became empty"),
        field("properties", object({}).allowAdditionalProperties())
          .required()
          .describe("All properties of the new entity"),
      }))
      .mutation(Mutation::Map)
      .handler(entityCreateBrush));

  registry.add(
    ToolDef{"entity_move_brushes"}
      .title("Move Brushes to Entity")
      .description(
        "Moves brushes (and patches) into an existing brush entity, or with entity "
        "\"world\" back to the world (the editor's Make Structural: the brushes move to "
        "the group or layer of the first moved brush, and smart tags matching them are "
        "turned off, e.g. Quake 2 detail content flags are cleared; materials are kept). "
        "Brush entities that become empty are removed (removedEntities). Brushes that "
        "are already there are skipped; if none is moved, the call succeeds with a "
        "NOTHING_MOVED warning. Example: {\"ids\": [\"brush:1042\"], \"entity\": "
        "\"entity:77\"}")
      .input(object({
        idsField(
          {ObjectKind::Brush, ObjectKind::Patch},
          "Brushes and patches to move. Default: the selection"),
        field("entity", objectId())
          .required()
          .describe("Id of the brush entity to move them into, or \"world\""),
      }))
      .output(object({
        field("entity", any()).required().describe("The target entity id or \"world\""),
        field("moved", array(objectId()))
          .required()
          .describe("The brushes that were moved"),
        field("removedEntities", array(objectId()))
          .required()
          .describe("Brush entities removed because they became empty"),
      }))
      .mutation(Mutation::Map)
      .handler(entityMoveBrushes));
}

} // namespace tb::mcp
