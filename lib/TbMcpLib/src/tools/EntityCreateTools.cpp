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
#include "NodeJson.h"
#include "ToolUtils.h"
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
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
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
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
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
/** A dropped origin this close to an integer height is rounded to it. */
constexpr auto DropRoundingEpsilon = 0.01;

/**
 * Makes the height of a dropped origin an integer, so that no fractions from model
 * bounds end up in the map: a height within DropRoundingEpsilon of an integer is
 * rounded, any other height is rounded up so that the bounds do not sink into the
 * floor (they float less than 1 unit above it instead).
 */
double cleanDropHeight(const double z)
{
  const auto rounded = std::round(z);
  return std::abs(z - rounded) <= DropRoundingEpsilon ? rounded : std::ceil(z);
}

ToolError invalidArgument(std::string message, std::string hint = {})
{
  return makeError(ErrorCode::InvalidArgument, std::move(message), std::move(hint));
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
 * The properties of the argument `properties` (propertiesFromJson). `origin` is rejected
 * if `allowOrigin` is false (point entities take `position`).
 */
Result<PropertyList, ToolError> propertiesArgument(
  const Args& args, const bool allowOrigin)
{
  const auto properties = args.getOptional<Json>("properties");
  return properties ? propertiesFromJson(*properties, allowOrigin) : PropertyList{};
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
 * that are still missing are set afterwards. Keys without a value are removed last, so
 * that they also remove the defaults set here or when the entity was created.
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
      if (!value)
      {
        continue;
      }
      validateProperty(context, definition, key, *value, {id});
      if (!mdl::setEntityProperty(map, key, *value))
      {
        return context.operationFailed("Property '" + key + "' could not be set.");
      }
    }
    if (applyDefaults)
    {
      mdl::setDefaultEntityProperties(map, mdl::SetDefaultPropertyMode::SetMissing);
    }
    for (const auto& [key, value] : properties)
    {
      if (!value && entityNode.entity().hasProperty(key))
      {
        if (!mdl::removeEntityProperty(map, key))
        {
          return context.operationFailed("Property '" + key + "' could not be removed.");
        }
      }
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
 * the bounds center at the center and the inset corners, against brushes and patches
 * that are not triggers and not in a layer omitted from export (hidden ones count).
 */
std::optional<RayHit> findFloor(mdl::Map& map, const vm::bbox3d& bounds)
{
  const auto accept = [&](const mdl::Node& node) {
    // hidden brushes are compiled, so they are floors too
    if (isPointEntity(node) || inOmittedLayer(node))
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
    properties.emplace_back(
      mdl::EntityPropertyKeys::Angle, formatPropertyNumber(Json(*angle)));
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
    // Keys given as null are removed after creation, so they do not affect the model
    auto presentProperties = std::vector<std::pair<std::string, std::string>>{};
    for (const auto& [key, value] : properties)
    {
      if (value)
      {
        presentProperties.emplace_back(key, *value);
      }
    }
    auto bounds = dropToFloorBounds(
      definition,
      classname,
      presentProperties,
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
    const auto z = cleanDropHeight(floor->point.z() - localBounds.min.z());
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
              "The brush entity could not be created.",
              "details.editorMessages holds the editor's reason; check that the brushes "
              "are editable (object_get).");
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
              "The brushes could not be moved.",
              "details.editorMessages holds the editor's reason; check that the brushes "
              "and the entity are editable (object_get).");
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

// entities_create

/** At most this many object ids are named in an aggregated warning. */
constexpr auto MaxIdsPerWarning = size_t(10);

/** An item of entities_create after validation. */
struct EntityItem
{
  std::string classname;
  const mdl::EntityDefinition* definition = nullptr;
  vm::vec3d origin;
  PropertyList properties;
  std::optional<std::string> ref;
  bool dropped = false;
};

/** A link of entities_create: `from` gets `key` = the `targetKey` value of `to`. */
struct EntityLink
{
  size_t from;
  std::string key;
  std::string targetKey;
  /** An item index or an existing entity. */
  std::variant<size_t, mdl::EntityNodeBase*> to;
};

/** The values of the given key of all entities of the map. */
std::set<std::string> propertyValuesInMap(mdl::Map& map, const std::string& key)
{
  auto result = std::set<std::string>{};
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    if (const auto* entityNode = dynamic_cast<const mdl::EntityNodeBase*>(&node))
    {
      if (const auto* value = entityNode->entity().property(key))
      {
        result.insert(*value);
      }
    }
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());
  return result;
}

/** Adds up problems of many objects to one warning per code and subject. */
class AggregatedWarnings
{
private:
  struct Entry
  {
    std::string code;
    std::string message;
    std::vector<std::string> ids;
  };
  std::vector<Entry> m_entries;

public:
  void add(std::string code, std::string message, std::string id)
  {
    auto it = std::ranges::find_if(m_entries, [&](const auto& entry) {
      return entry.code == code && entry.message == message;
    });
    if (it == m_entries.end())
    {
      m_entries.push_back(Entry{std::move(code), std::move(message), {}});
      it = std::prev(m_entries.end());
    }
    it->ids.push_back(std::move(id));
  }

  void report(CallContext& context)
  {
    for (auto& entry : m_entries)
    {
      const auto count = entry.ids.size();
      if (count > MaxIdsPerWarning)
      {
        entry.ids.resize(MaxIdsPerWarning);
      }
      context.warn(
        std::move(entry.code),
        count == 1 ? std::move(entry.message)
                   : fmt::format(
                       "{} ({} entities{})",
                       entry.message,
                       count,
                       count > MaxIdsPerWarning ? ", the first 10 listed" : ""),
        std::move(entry.ids));
    }
  }
};

ToolResult entitiesCreate(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& grid = map.grid();
  const auto items = args.get<Json>("items");
  const auto snapToGrid = args.get<bool>("snapToGrid");
  const auto applyDefaults = args.get<bool>("applyDefaults");
  const auto dropToFloorDefault = args.get<bool>("dropToFloor");

  auto errors = ItemErrors{};
  auto entityItems = std::vector<EntityItem>{};
  auto links = std::vector<EntityLink>{};
  auto refs = std::map<std::string, size_t>{};
  auto models = EntityModelLoader{map};

  // everything is validated before anything is created
  for (size_t i = 0; i < items.size(); ++i)
  {
    const auto item = Args{items[i]};
    auto entityItem = EntityItem{};

    auto classname = classnameArgument(item);
    if (classname.is_error())
    {
      errors.add(i, errorOf(classname));
      entityItems.push_back(std::move(entityItem));
      continue;
    }
    entityItem.classname = std::move(classname).value();
    entityItem.definition = findEntityDefinition(map, entityItem.classname);
    if (
      entityItem.definition
      && mdl::getType(*entityItem.definition) != mdl::EntityDefinitionType::Point)
    {
      errors.add(
        i,
        "'" + entityItem.classname
          + "' is a brush entity class; create it with brushes_create (entity) or "
            "entity_create_brush.");
    }

    auto properties = propertiesArgument(item, false);
    if (properties.is_error())
    {
      errors.add(i, errorOf(properties));
    }
    else
    {
      entityItem.properties = std::move(properties).value();
    }
    if (const auto angle = item.getOptional<double>("angle"))
    {
      if (std::ranges::any_of(entityItem.properties, [](const auto& property) {
            return property.first == mdl::EntityPropertyKeys::Angle;
          }))
      {
        errors.add(i, "The angle is given twice, as 'angle' and in 'properties'.");
      }
      entityItem.properties.emplace_back(
        mdl::EntityPropertyKeys::Angle, formatPropertyNumber(Json(*angle)));
    }

    const auto dropToFloor = item.getOr<bool>("dropToFloor", dropToFloorDefault);
    auto position = item.get<vm::vec3d>("position");
    if (snapToGrid)
    {
      position[0] = grid.snap(position.x());
      position[1] = grid.snap(position.y());
      if (!dropToFloor)
      {
        position[2] = grid.snap(position.z());
      }
    }
    if (!map.worldBounds().contains(position))
    {
      errors.add(
        i,
        makeError(
          ErrorCode::OutOfWorldBounds,
          "The position " + toJson(position).dump() + " lies outside the world bounds.",
          "Move it inside the world bounds " + toJson(map.worldBounds()).dump() + "."));
    }
    else if (dropToFloor)
    {
      auto presentProperties = std::vector<std::pair<std::string, std::string>>{};
      for (const auto& [key, value] : entityItem.properties)
      {
        if (value)
        {
          presentProperties.emplace_back(key, *value);
        }
      }
      auto bounds = dropToFloorBounds(
        entityItem.definition,
        entityItem.classname,
        presentProperties,
        applyDefaults,
        "auto",
        models);
      if (bounds.is_error())
      {
        errors.add(i, errorOf(bounds));
      }
      else if (
        const auto floor = findFloor(map, bounds.value().bounds.translate(position)))
      {
        position[2] = cleanDropHeight(floor->point.z() - bounds.value().bounds.min.z());
        entityItem.dropped = true;
      }
      else
      {
        errors.add(
          i,
          "There is no floor below " + toJson(position).dump()
            + "; move the position above a floor or omit dropToFloor.");
      }
    }
    entityItem.origin = position;

    if (const auto ref = item.getOptional<std::string>("ref"))
    {
      if (!refs.emplace(*ref, i).second)
      {
        errors.add(i, "The ref '" + *ref + "' is used by another item, too.");
      }
      entityItem.ref = *ref;
    }
    entityItems.push_back(std::move(entityItem));
  }

  // links name refs of any item or existing entities
  for (size_t i = 0; i < items.size(); ++i)
  {
    const auto linksJson = items[i].value("links", Json::array());
    for (const auto& linkJson : linksJson)
    {
      const auto to = linkJson.value("to", std::string{});
      auto link = EntityLink{
        i,
        linkJson.value("key", std::string{"target"}),
        linkJson.value("targetKey", std::string{"targetname"}),
        size_t(0)};
      if (
        link.key.empty() || containsQuote(link.key) || link.targetKey.empty()
        || containsQuote(link.targetKey))
      {
        errors.add(i, "The link keys must be non-empty and must not contain quotes.");
        continue;
      }
      if (const auto it = refs.find(to); it != refs.end())
      {
        link.to = it->second;
      }
      else if (const auto ref = parseObjectRef(to);
               ref && ref->kind == ObjectKind::Entity)
      {
        auto node = ids.resolve(*ref);
        auto* entityNode =
          node.is_success() ? dynamic_cast<mdl::EntityNodeBase*>(node.value()) : nullptr;
        if (!entityNode)
        {
          errors.add(i, "The link target " + to + " does not exist.");
          continue;
        }
        if (!entityNode->entity().property(link.targetKey))
        {
          errors.add(
            i,
            "The link target " + to + " has no '" + link.targetKey
              + "'; set one with entity_properties_set first.");
          continue;
        }
        link.to = entityNode;
      }
      else
      {
        errors.add(
          i,
          "The link target '" + to
            + "' is neither the ref of an item nor the id of an existing entity.");
        continue;
      }
      links.push_back(std::move(link));
    }
  }

  if (!errors.empty())
  {
    return errors.error(
      items.size(),
      "Fix the listed items (details.errors) and call entities_create again with all "
      "items.");
  }

  // build the entities
  const auto setDefaults = map.worldNode().entityPropertyConfig().setDefaultProperties;
  auto entities = std::vector<mdl::Entity>{};
  entities.reserve(entityItems.size());
  for (const auto& entityItem : entityItems)
  {
    auto entity =
      mdl::Entity{{{mdl::EntityPropertyKeys::Classname, entityItem.classname}}};
    if (entityItem.definition && setDefaults)
    {
      mdl::setDefaultProperties(
        *entityItem.definition, entity, mdl::SetDefaultPropertyMode::SetAll);
    }
    for (const auto& [key, value] : entityItem.properties)
    {
      if (value)
      {
        entity.addOrUpdateProperty(key, *value);
      }
    }
    if (entityItem.definition && applyDefaults)
    {
      mdl::setDefaultProperties(
        *entityItem.definition, entity, mdl::SetDefaultPropertyMode::SetMissing);
    }
    for (const auto& [key, value] : entityItem.properties)
    {
      if (!value)
      {
        entity.removeProperty(key);
      }
    }
    entity.setOrigin(entityItem.origin);
    entities.push_back(std::move(entity));
  }

  // link values: the target's value, or a new unique name
  auto generatedNames = size_t(0);
  auto usedNames = std::map<std::string, std::set<std::string>>{};
  for (const auto& link : links)
  {
    auto value = std::string{};
    if (const auto* targetIndex = std::get_if<size_t>(&link.to))
    {
      auto& target = entities[*targetIndex];
      if (const auto* existing = target.property(link.targetKey))
      {
        value = *existing;
      }
      else
      {
        auto [it, inserted] = usedNames.try_emplace(link.targetKey);
        if (inserted)
        {
          it->second = propertyValuesInMap(map, link.targetKey);
          for (const auto& entity : entities)
          {
            if (const auto* name = entity.property(link.targetKey))
            {
              it->second.insert(*name);
            }
          }
        }
        const auto& base = entityItems[*targetIndex].ref
                             ? *entityItems[*targetIndex].ref
                             : entityItems[*targetIndex].classname;
        value = base;
        for (size_t n = 1; it->second.contains(value) || containsQuote(value); ++n)
        {
          value = fmt::format("{}_{}", base, n);
        }
        it->second.insert(value);
        target.addOrUpdateProperty(link.targetKey, value);
        ++generatedNames;
      }
    }
    else
    {
      value = *std::get<mdl::EntityNodeBase*>(link.to)->entity().property(link.targetKey);
    }
    entities[link.from].addOrUpdateProperty(link.key, value);
  }

  // add them in one step
  auto* parent = &mdl::parentForNodes(map);
  if (const auto layerId = args.getOptional<std::string>("layer"))
  {
    auto layer = ids.resolve(*layerId);
    if (layer.is_error())
    {
      return errorOf(layer);
    }
    parent = layer.value();
  }
  auto nodes = std::vector<mdl::Node*>{};
  nodes.reserve(entities.size());
  for (auto& entity : entities)
  {
    nodes.push_back(new mdl::EntityNode{std::move(entity)});
  }
  auto* group = static_cast<mdl::GroupNode*>(nullptr);
  auto topLevel = nodes;
  if (const auto groupName = args.getOptional<std::string>("group"))
  {
    group = new mdl::GroupNode{mdl::Group{*groupName}};
    group->addChildren(nodes);
    topLevel = {group};
  }

  mdl::deselectAll(map);
  if (mdl::addNodes(map, {{parent, topLevel}}).empty())
  {
    return context.operationFailed("The entities could not be added to the map.");
  }
  auto selectable = std::vector<mdl::Node*>{};
  std::ranges::copy_if(topLevel, std::back_inserter(selectable), [&](const auto* node) {
    return map.editorContext().selectable(*node);
  });
  mdl::selectNodes(map, selectable);

  // problems as aggregated warnings
  auto warnings = AggregatedWarnings{};
  auto unknownClasses = std::map<std::string, std::vector<std::string>>{};
  auto outside = std::vector<std::string>{};
  auto overlapping = std::vector<std::string>{};
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    const auto& entityNode = static_cast<const mdl::EntityNode&>(*nodes[i]);
    const auto id = ids.format(entityNode);
    const auto& entityItem = entityItems[i];
    if (!entityItem.definition)
    {
      unknownClasses[entityItem.classname].push_back(id);
    }
    for (const auto& [key, value] : entityItem.properties)
    {
      if (value)
      {
        if (auto problem = checkPropertyValue(entityItem.definition, key, *value))
        {
          warnings.add(std::move(problem->code), std::move(problem->message), id);
        }
      }
    }
    const auto bounds = entityNode.logicalBounds();
    if (!map.worldBounds().contains(bounds))
    {
      outside.push_back(id);
    }
    if (!overlappingBrushes(context, bounds).empty())
    {
      overlapping.push_back(id);
    }
  }
  for (auto& [classname, classIds] : unknownClasses)
  {
    if (classIds.size() > MaxIdsPerWarning)
    {
      classIds.resize(MaxIdsPerWarning);
    }
    warnUnknownClassname(context, classname, std::move(classIds));
  }
  warnings.report(context);
  const auto warnIds =
    [&](const char* code, std::vector<std::string> warnedIds, auto message) {
      if (!warnedIds.empty())
      {
        const auto count = warnedIds.size();
        warnedIds.resize(std::min(count, MaxIdsPerWarning));
        context.warn(
          code, fmt::format(fmt::runtime(message), count), std::move(warnedIds));
      }
    };
  warnIds(
    "ENTITY_OVERLAPS_BRUSHES",
    std::move(overlapping),
    "{} entities intersect brushes; move them into open space or use dropToFloor "
    "(entity_placement_check lists the brushes).");
  warnIds(
    "OUTSIDE_WORLD_BOUNDS",
    std::move(outside),
    "{} entities lie (partly) outside the world bounds.");

  auto refIds = Json::object();
  for (const auto& [ref, index] : refs)
  {
    refIds[ref] = ids.format(*nodes[index]);
  }
  return Json{
    {"ids", formatIds(nodes, ids)},
    {"count", nodes.size()},
    {"refs", std::move(refIds)},
    {"links", links.size()},
    {"generatedNames", generatedNames},
    {"dropped",
     std::ranges::count_if(entityItems, [](const auto& item) { return item.dropped; })},
    {"group", group ? Json(ids.format(*group)) : Json(nullptr)},
    {"layer", layerIdOf(*nodes.front(), ids)},
  };
}

Field propertiesField()
{
  return field("properties", object({}).allowAdditionalProperties())
    .describe(
      "Properties to set, key -> value. Values may be strings, numbers, booleans (1 / "
      "0), arrays of numbers (written space separated, e.g. [255, 128, 0] -> \"255 "
      "128 0\") or null, which removes the key, including a default value the entity "
      "got when it was created (like entity_properties_set). Values are checked against "
      "the entity definition; problems are warnings (UNKNOWN_PROPERTY, INVALID_CHOICE, "
      "...), never errors");
}

} // namespace

void registerEntityCreateTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"entity_create_point"}
      .title("Create Point Entity")
      .description(
        "Creates a point entity (monster, light, item, player start, ...) with its "
        "origin at 'position' (map units) in the open group or the current layer, and "
        "selects it (one undo step). The position is snapped to the grid (snapToGrid). "
        "With dropToFloor, the entity is lowered (or raised) so that the bottom of its "
        "bounds (dropUsing) rests on the highest floor below it (brushes, hidden ones "
        "too, and "
        "patches, triggers ignored; rays start at the height of the bounds center); the "
        "dropped z is kept integral (rounded when within 0.01 of an integer, otherwise "
        "rounded up, so the bounds float less than 1 unit above the floor); no floor "
        "fails with INVALID_ARGUMENT. Properties are checked against the entity "
        "definition (warnings only). Games that set default properties on creation "
        "(e.g. Half-Life) add all defaults of the definition, like the editor; a null "
        "value removes one. A brush entity class fails (use entity_create_brush); an "
        "unknown class is created anyway (UNKNOWN_CLASSNAME warning). Placement "
        "warnings: ENTITY_OVERLAPS_BRUSHES, OUTSIDE_WORLD_BOUNDS and, for loadable "
        "models, the findings of entity_placement_check (MODEL_BELOW_FLOOR, "
        "MODEL_FLOATING, MODEL_PENETRATES_BRUSHES, MODEL_NO_FLOOR). Returns the entity "
        "id, origin, bounds, floor, onFloor (a floor within 1 unit) and all properties. "
        "Example: {\"classname\": \"monster_ogre\", \"position\": [256, 128, 64], "
        "\"angle\": 90, \"dropToFloor\": true, \"properties\": {\"spawnflags\": 256}}")
      .input(object({
        field("classname", string().nonEmpty())
          .required()
          .describe("Entity class, e.g. 'info_player_start' (see entity_classes_list)"),
        field("position", vec3())
          .required()
          .describe("Origin [x, y, z] of the entity in map units"),
        propertiesField(),
        field("angle", angle())
          .describe("Yaw in degrees (0 = east / +x, 90 = north / +y); sets 'angle'"),
        field("dropToFloor", boolean().defaultsTo(false))
          .describe("Move the entity down (or up) until it stands on the floor below the "
                    "position"),
        field("dropUsing", enumOf({"auto", "model", "definition"}).defaultsTo("auto"))
          .describe(
            "Bounds that dropToFloor rests on the floor: 'model' (the model bounds of "
            "the animation the properties select, e.g. a sitting pose), 'definition' "
            "(the class size) or 'auto' (the model if it can be loaded)"),
        field("applyDefaults", boolean().defaultsTo(false))
          .describe("Also set the definition's default values of all properties not "
                    "given"),
        field("snapToGrid", boolean().defaultsTo(true))
          .describe("Snap the position to the grid (x and y; z too unless dropToFloor)"),
      }))
      .output(object({
        field("entity", objectId()).required().describe("Id of the new entity"),
        field("classname", string()).required(),
        field("origin", vec3()).required().describe("Origin after snapping and dropping"),
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
        "func_detail, ...), like the editor's Create Entity menu, and selects them (one "
        "undo step). "
        "Brushes that belong to another brush entity are moved out of it; entities that "
        "become empty are removed (removedEntities). If all brushes belong to one "
        "entity, its properties are kept. A point entity class fails (use "
        "entity_create_point); an unknown class is created anyway with an "
        "UNKNOWN_CLASSNAME warning. Properties are validated against the entity "
        "definition (warnings only). Games that set default properties on creation (e.g. "
        "Half-Life) add all defaults of the definition, like the editor, including empty "
        "ones such as a func_breakable's gibmodel; a null value removes a key. Example: "
        "{\"classname\": \"func_door\", \"ids\": [\"brush:1042\"], \"properties\": "
        "{\"angle\": -1, \"speed\": 200}}")
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
        field("brushes", array(objectId()))
          .required()
          .describe("Brushes and patches of the new entity"),
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
        "Moves brushes (and patches) into an existing brush entity (one undo step), or "
        "with entity "
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

  registry.add(
    ToolDef{"entities_create"}
      .title("Create Entities")
      .description(
        "Creates many point entities in one call and one undo step, e.g. the lights, "
        "monsters and items of a generated level. Each item: classname, position (map "
        "units), optional angle, properties (as in entity_create_point), dropToFloor "
        "(default: the call's dropToFloor), ref (a name for links) and links: [{to: a "
        "ref of another item or an existing entity id, key (default target), targetKey "
        "(default targetname)}] sets key to the target's targetKey value, generating a "
        "unique name (the ref, else the classname, with _n) if the target has none. All "
        "items are validated first; if any is invalid nothing is created and "
        "INVALID_ARGUMENT lists every problem with its item index in details.errors. "
        "Games that set default properties on creation get them like the editor. The "
        "entities go into 'layer' (default: the open group or current layer), into a "
        "new group with 'group', and are selected. Result ids[i] is the entity of "
        "items[i]; lists are cut to the detail level. Property problems, unknown classes "
        "and overlaps are aggregated into one warning per code. Use checks: \"defer\" "
        "while the surrounding brushes are not yet built. Example: {\"items\": "
        "[{\"classname\": \"light\", \"position\": [0, 0, 128], \"properties\": "
        "{\"light\": 300}, \"ref\": \"lamp\"}, {\"classname\": \"func_button_target\", "
        "\"position\": [64, 0, 32], \"links\": [{\"to\": \"lamp\"}]}, {\"classname\": "
        "\"monster_army\", \"position\": [256, 0, 64], \"angle\": 180, \"dropToFloor\": "
        "true}]}")
      .input(object({
        field(
          "items",
          array(object({
                  field("classname", string().nonEmpty())
                    .required()
                    .describe("Entity class, e.g. 'light'"),
                  field("position", vec3()).required().describe("Origin in map units"),
                  field("angle", angle()).describe("Yaw in degrees; sets 'angle'"),
                  propertiesField(),
                  field("dropToFloor", boolean())
                    .describe("Stand the entity on the floor below the position"),
                  field("ref", string().nonEmpty())
                    .describe("A name that links of other items refer to"),
                  field(
                    "links",
                    array(object({
                      field("to", string().nonEmpty())
                        .required()
                        .describe("The ref of an item or an existing entity id"),
                      field("key", string().nonEmpty())
                        .describe("Property of this entity to set. Default: target"),
                      field("targetKey", string().nonEmpty())
                        .describe("Property of the target whose value is used. Default: "
                                  "targetname"),
                    })))
                    .describe("Properties that point at other entities"),
                }))
            .nonEmpty()
            .maxSize(5000))
          .required()
          .describe("The entities to create"),
        field("dropToFloor", boolean().defaultsTo(false))
          .describe("Default of the items' dropToFloor"),
        field("snapToGrid", boolean().defaultsTo(true))
          .describe("Snap positions to the grid (x and y; z too unless dropped)"),
        field("applyDefaults", boolean().defaultsTo(false))
          .describe("Also set the definitions' default values of all properties not "
                    "given"),
        field("layer", objectId({ObjectKind::Layer}))
          .describe("Layer of the new entities. Default: the open group or current "
                    "layer"),
        field("group", string().nonEmpty())
          .describe("Put the new entities into a new group with this name"),
      }))
      .output(object({
        field("ids", array(objectId()))
          .required()
          .describe("The new entities; ids[i] belongs to items[i]"),
        field("count", integer()).required(),
        field("refs", object({}).allowAdditionalProperties())
          .required()
          .describe("Entity id per ref"),
        field("links", integer()).required().describe("Number of links set"),
        field("generatedNames", integer())
          .required()
          .describe("Number of target names generated for links"),
        field("dropped", integer())
          .required()
          .describe("Number of entities dropped to the floor"),
        field("group", any()).required().describe("Id of the new group, or null"),
        field("layer", any()).required().describe("Layer of the new entities"),
      }))
      .mutation(Mutation::Map)
      .handler(entitiesCreate));
}

} // namespace tb::mcp
