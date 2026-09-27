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

#include "mcp/tools/EntityClassTools.h"

#include "EntityUtils.h"
#include "ToolUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/GameTools.h"
#include "mdl/DecalDefinition.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/EntityPropertiesVariableStore.h"
#include "mdl/EntityRotation.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/ModelDefinition.h"
#include "mdl/ModelSpecification.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"
#include "kd/string_compare_natural.h"

#include "vm/mat.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr auto MaxSummaryDescriptionLength = size_t(120);
constexpr auto MaxModelInfoIds = size_t(50);

// helpers

/** The first non-empty line of a description, truncated to the given length. */
std::string firstLine(const std::string& description, const size_t maxLength)
{
  auto begin = description.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
  {
    return {};
  }
  auto end = description.find_first_of("\r\n", begin);
  auto line =
    description.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
  {
    line.pop_back();
  }
  if (line.size() > maxLength)
  {
    line = line.substr(0, maxLength - 3) + "...";
  }
  return line;
}

double roundTo(const double value, const double precision)
{
  return std::round(value / precision) * precision;
}

/** `[r, g, b]` with float components 0..1. */
Json colorJson(const Color& color)
{
  const auto rgb = color.to<RgbF>().toVec();
  return Json::array(
    {roundTo(rgb[0], 0.001), roundTo(rgb[1], 0.001), roundTo(rgb[2], 0.001)});
}

/** `{"path", "skin", "frame"}` */
Json modelSpecificationJson(const mdl::ModelSpecification& spec)
{
  return Json{
    {"path", spec.path.string()},
    {"skin", spec.skinIndex},
    {"frame", spec.frameIndex},
  };
}

/** The model a point class shows by default, or null if it has none. */
Json defaultModelJson(const mdl::PointEntityDefinition& pointDefinition)
{
  const auto spec = pointDefinition.modelDefinition.defaultModelSpecification();
  return spec.is_success() && !spec.value().path.empty()
           ? modelSpecificationJson(spec.value())
           : Json(nullptr);
}

/** The decal a point class shows by default, or null if it has none. */
Json defaultDecalJson(const mdl::PointEntityDefinition& pointDefinition)
{
  const auto spec = pointDefinition.decalDefinition.defaultDecalSpecification();
  return spec.is_success() && !spec.value().materialName.empty()
           ? Json{{"material", spec.value().materialName}}
           : Json(nullptr);
}

std::vector<std::string> propertyKeys(const mdl::EntityDefinition& definition)
{
  auto result = std::vector<std::string>{};
  for (const auto& propertyDefinition : definition.propertyDefinitions)
  {
    result.push_back(propertyDefinition.key);
  }
  return result;
}

const mdl::PropertyValueTypes::Flags* spawnflagsOf(
  const mdl::EntityDefinition& definition)
{
  const auto* propertyDefinition =
    mdl::getPropertyDefinition(definition, mdl::EntityPropertyKeys::Spawnflags);
  return propertyDefinition
           ? std::get_if<mdl::PropertyValueTypes::Flags>(&propertyDefinition->valueType)
           : nullptr;
}

/** The flags of the spawnflags property as described by propertyDefinitionJson. */
Json spawnflagsJson(const mdl::EntityDefinition& definition)
{
  const auto* propertyDefinition =
    mdl::getPropertyDefinition(definition, mdl::EntityPropertyKeys::Spawnflags);
  if (!propertyDefinition || !spawnflagsOf(definition))
  {
    return Json::array();
  }
  return propertyDefinitionJson(*propertyDefinition)["flags"];
}

std::string rotationTypeName(const mdl::EntityRotationType type)
{
  switch (type)
  {
  case mdl::EntityRotationType::None:
    return "none";
  case mdl::EntityRotationType::Angle:
    return "angle";
  case mdl::EntityRotationType::AngleUpDown:
    return "angle_up_down";
  case mdl::EntityRotationType::Euler:
    return "euler";
  case mdl::EntityRotationType::Euler_PositivePitchDown:
    return "euler_positive_pitch_down";
  case mdl::EntityRotationType::Mangle:
    return "mangle";
  }
  return "none";
}

/**
 * How the editor orients an entity: the property it reads and writes when the entity is
 * rotated, and its kind. Null if the entity cannot be oriented.
 */
Json rotationInfoJson(const mdl::EntityRotationInfo& info)
{
  if (info.type == mdl::EntityRotationType::None || info.propertyKey.empty())
  {
    return nullptr;
  }
  auto result = Json{
    {"key", info.propertyKey},
    {"type", rotationTypeName(info.type)},
  };
  if (info.usage == mdl::EntityRotationUsage::BlockRotation)
  {
    // the definition's bounds are not centered on the origin
    result["blocked"] = true;
  }
  return result;
}

/**
 * The orientation hint of an entity class, as the editor would determine it for a new
 * entity of that class without properties.
 */
Json classRotationJson(const mdl::EntityDefinition& definition)
{
  auto entity = mdl::Entity{{{mdl::EntityPropertyKeys::Classname, definition.name}}};
  entity.setPointEntity(mdl::getType(definition) == mdl::EntityDefinitionType::Point);
  entity.setDefinition(&definition);
  return rotationInfoJson(mdl::entityRotationInfo(entity));
}

/** A short description of a class, as used by the list tool and the resource. */
Json classSummary(const mdl::EntityDefinition& definition)
{
  return Json{
    {"name", definition.name},
    {"type", entityDefinitionTypeName(definition)},
    {"group", std::string{mdl::getGroupName(definition)}},
    {"description", firstLine(definition.description, MaxSummaryDescriptionLength)},
  };
}

std::vector<const mdl::EntityDefinition*> sortedDefinitions(const mdl::Map& map)
{
  auto result = std::vector<const mdl::EntityDefinition*>{};
  for (const auto& definition : map.entityDefinitionManager().definitions())
  {
    result.push_back(&definition);
  }
  std::ranges::sort(result, [](const auto* lhs, const auto* rhs) {
    return kdl::ci::string_less_natural{}(lhs->name, rhs->name);
  });
  return result;
}

void warnNoDefinitions(CallContext& context)
{
  context.warn(
    "NO_ENTITY_DEFINITIONS",
    "The document has no entity definitions loaded. Check the definition file with "
    "entity_definitions_get and choose one with entity_definitions_set.");
}

// entity_classes_list

struct ClassFilter
{
  std::optional<std::string> type;
  std::optional<std::string> prefix;
  std::optional<std::string> group;
  std::optional<std::string> search;
  std::optional<bool> used;
};

bool matches(const mdl::EntityDefinition& definition, const ClassFilter& filter)
{
  if (filter.type && entityDefinitionTypeName(definition) != *filter.type)
  {
    return false;
  }
  if (filter.prefix)
  {
    const auto isGlob = filter.prefix->find_first_of("*?") != std::string::npos;
    if (
      isGlob ? !kdl::ci::str_matches_glob(definition.name, *filter.prefix)
             : !kdl::ci::str_is_prefix(definition.name, *filter.prefix))
    {
      return false;
    }
  }
  if (
    filter.group && !kdl::ci::str_is_equal(mdl::getGroupName(definition), *filter.group))
  {
    return false;
  }
  if (
    filter.search && !kdl::ci::str_contains(definition.name, *filter.search)
    && !kdl::ci::str_contains(definition.description, *filter.search))
  {
    return false;
  }
  if (filter.used && (definition.usageCount() > 0) != *filter.used)
  {
    return false;
  }
  return true;
}

struct GroupCounts
{
  size_t point = 0;
  size_t brush = 0;
  size_t used = 0;
};

ToolResult entityClassesList(CallContext& context, const Args& args)
{
  const auto& map = context.map();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  if (map.entityDefinitionManager().definitions().empty())
  {
    warnNoDefinitions(context);
  }

  const auto filter = ClassFilter{
    args.getOptional<std::string>("type"),
    args.getOptional<std::string>("prefix"),
    args.getOptional<std::string>("group"),
    args.getOptional<std::string>("search"),
    args.getOptional<bool>("used"),
  };

  const auto detail = request.value().detail;
  auto items = std::vector<Json>{};
  auto groups = std::map<std::string, GroupCounts, kdl::ci::string_less_natural>{};
  for (const auto* definition : sortedDefinitions(map))
  {
    if (!matches(*definition, filter))
    {
      continue;
    }

    auto item = classSummary(*definition);
    item["usageCount"] = definition->usageCount();
    if (detail == Detail::Full)
    {
      item["color"] = colorJson(definition->color);
      if (const auto& pointDefinition = definition->pointEntityDefinition)
      {
        item["size"] = toJson(pointDefinition->bounds);
        const auto model = defaultModelJson(*pointDefinition);
        item["model"] = model.is_null() ? model : model["path"];
      }
      item["propertyKeys"] = propertyKeys(*definition);
    }
    items.push_back(std::move(item));

    auto& counts = groups[std::string{mdl::getGroupName(*definition)}];
    ++(
      mdl::getType(*definition) == mdl::EntityDefinitionType::Point ? counts.point
                                                                    : counts.brush);
    if (definition->usageCount() > 0)
    {
      ++counts.used;
    }
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  auto groupsJson = Json::array();
  for (const auto& [name, counts] : groups)
  {
    groupsJson.push_back(Json{
      {"name", name},
      {"point", counts.point},
      {"brush", counts.brush},
      {"used", counts.used},
    });
  }
  result["groups"] = std::move(groupsJson);
  return result;
}

// entity_class_describe

/** Up to 10 class names that resemble the given name, best matches first. */
std::vector<std::string> similarClassnames(
  const mdl::Map& map, const std::string& classname)
{
  const auto commonPrefixLength = [&](const std::string& name) {
    auto length = size_t(0);
    while (length < name.size() && length < classname.size()
           && std::tolower(static_cast<unsigned char>(name[length]))
                == std::tolower(static_cast<unsigned char>(classname[length])))
    {
      ++length;
    }
    return length;
  };

  // the part after the group name, e.g. "ogre" for "monster_ogre"
  const auto underscore = classname.find('_');
  const auto shortName =
    underscore == std::string::npos ? classname : classname.substr(underscore + 1);

  auto candidates = std::vector<std::pair<size_t, const mdl::EntityDefinition*>>{};
  for (const auto* definition : sortedDefinitions(map))
  {
    const auto& name = definition->name;
    const auto prefix = commonPrefixLength(name);
    auto score = size_t(0);
    if (kdl::ci::str_contains(name, classname) || kdl::ci::str_contains(classname, name))
    {
      score = 1000 + prefix;
    }
    else if (shortName.size() >= 3 && kdl::ci::str_contains(name, shortName))
    {
      score = 500 + prefix;
    }
    else if (prefix >= 3)
    {
      score = prefix;
    }
    if (score > 0)
    {
      candidates.emplace_back(score, definition);
    }
  }

  std::ranges::stable_sort(
    candidates, [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });

  auto result = std::vector<std::string>{};
  for (const auto& [score, definition] : candidates)
  {
    if (result.size() == 10)
    {
      break;
    }
    result.push_back(definition->name);
  }
  return result;
}

/** Finds a definition by exact name, or else by a unique case-insensitive match. */
const mdl::EntityDefinition* findDefinitionIgnoringCase(
  const mdl::Map& map, const std::string& classname)
{
  if (const auto* definition = findEntityDefinition(map, classname))
  {
    return definition;
  }

  const mdl::EntityDefinition* result = nullptr;
  for (const auto& definition : map.entityDefinitionManager().definitions())
  {
    if (kdl::ci::str_is_equal(definition.name, classname))
    {
      if (result)
      {
        return nullptr;
      }
      result = &definition;
    }
  }
  return result;
}

ToolResult entityClassDescribe(CallContext& context, const Args& args)
{
  const auto& map = context.map();
  const auto classname = args.get<std::string>("classname");

  const auto* definition = findDefinitionIgnoringCase(map, classname);
  if (!definition)
  {
    if (map.entityDefinitionManager().definitions().empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Class '" + classname + "' is unknown: the document has no entity definitions.",
        "Check the definition file with entity_definitions_get and choose one with "
        "entity_definitions_set.");
    }

    const auto similar = similarClassnames(map, classname);
    auto message = "Class '" + classname + "' is not defined by the entity definitions.";
    if (!similar.empty())
    {
      message += " Similar classes: ";
      for (size_t i = 0; i < similar.size(); ++i)
      {
        message += (i > 0 ? ", " : "") + similar[i];
      }
      message += ".";
    }
    return makeError(
      ErrorCode::InvalidArgument,
      std::move(message),
      "Use entity_classes_list (e.g. with prefix or search) to find a class.");
  }

  auto properties = Json::array();
  auto defaults = Json::object();
  for (const auto& propertyDefinition : definition->propertyDefinitions)
  {
    properties.push_back(propertyDefinitionJson(propertyDefinition));
    if (
      const auto defaultValue = mdl::PropertyDefinition::defaultValue(propertyDefinition))
    {
      defaults[propertyDefinition.key] = *defaultValue;
    }
  }

  const auto linkKeys = [](const std::vector<const mdl::PropertyDefinition*>& list) {
    auto keys = std::vector<std::string>{};
    for (const auto* propertyDefinition : list)
    {
      keys.push_back(propertyDefinition->key);
    }
    return keys;
  };

  auto result = classSummary(*definition);
  result["description"] = definition->description;
  result["color"] = colorJson(definition->color);
  if (const auto& pointDefinition = definition->pointEntityDefinition)
  {
    result["size"] = toJson(pointDefinition->bounds);
    result["model"] = Json{{"default", defaultModelJson(*pointDefinition)}};
    result["decal"] = defaultDecalJson(*pointDefinition);
  }
  else
  {
    result["size"] = nullptr;
    result["model"] = nullptr;
    result["decal"] = nullptr;
  }
  result["properties"] = std::move(properties);
  result["spawnflags"] = spawnflagsJson(*definition);
  result["linkProperties"] = Json{
    {"sources", linkKeys(mdl::getLinkSourcePropertyDefinitions(definition))},
    {"targets", linkKeys(mdl::getLinkTargetPropertyDefinitions(definition))},
  };
  result["defaults"] = std::move(defaults);
  result["rotation"] = classRotationJson(*definition);
  result["usageCount"] = definition->usageCount();
  return result;
}

// entity_model_info

Result<std::vector<mdl::EntityNodeBase*>, ToolError> modelInfoTargets(
  CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  auto result = std::vector<mdl::EntityNodeBase*>{};
  const auto add = [&](mdl::EntityNodeBase* entityNode) {
    if (
      entityNode && entityNode != &map.worldNode()
      && std::ranges::find(result, entityNode) == result.end())
    {
      result.push_back(entityNode);
    }
  };

  if (const auto explicitIds = args.getOptional<std::vector<std::string>>("ids"))
  {
    for (const auto& id : *explicitIds)
    {
      auto resolved = ids.resolve(id);
      if (resolved.is_error())
      {
        return errorOf(resolved);
      }
      auto* entityNode = dynamic_cast<mdl::EntityNodeBase*>(resolved.value());
      if (!entityNode || entityNode == &map.worldNode())
      {
        return makeError(
          ErrorCode::WrongObjectKind,
          "Object " + id + " is not an entity.",
          "Pass entity ids.",
          {id});
      }
      add(entityNode);
    }
    return result;
  }

  for (auto* entityNode : map.selection().allEntities())
  {
    add(entityNode);
  }
  if (result.empty())
  {
    return makeError(
      ErrorCode::NoSelection,
      "No ids were given and no entities are selected.",
      "Pass entity ids, e.g. from objects_find {\"kinds\":[\"entity\"]}.");
  }
  if (result.size() > MaxModelInfoIds)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      std::to_string(result.size()) + " entities are selected; at most "
        + std::to_string(MaxModelInfoIds) + " can be described at once.",
      "Pass up to 50 entity ids.");
  }
  return result;
}

Json modelInfoJson(
  const mdl::Map& map,
  const IdRegistry& ids,
  const mdl::EntityNodeBase& entityNode,
  EntityModelLoader& loader,
  const size_t maxAnimations)
{
  const auto& entity = entityNode.entity();
  auto result = Json{
    {"id", ids.format(entityNode)},
    {"classname", entity.classname()},
    {"pointEntity", entity.pointEntity()},
    {"model", nullptr},
    {"modelLoaded", false},
    {"modelBounds", nullptr},
    {"frameProperty", nullptr},
    {"currentAnimation", nullptr},
    {"animationCount", nullptr},
    {"animations", Json::array()},
    {"animationsTruncated", false},
    {"bounds", toJson(entityNode.logicalBounds())},
    {"definitionBounds", nullptr},
    {"scale", nullptr},
    {"rotation", nullptr},
  };

  const auto* pointNode = dynamic_cast<const mdl::EntityNode*>(&entityNode);
  if (!pointNode || !entity.pointEntity())
  {
    return result;
  }

  const auto* pointDefinition = mdl::getPointEntityDefinition(entity.definition());
  if (pointDefinition)
  {
    result["definitionBounds"] = toJson(pointDefinition->bounds);

    if (const auto spec = entity.modelSpecification(); spec.is_error())
    {
      result["modelError"] = errorMessage(spec);
    }
    else if (!spec.value().path.empty())
    {
      result["model"] = modelSpecificationJson(spec.value());
    }

    const auto variableStore = mdl::EntityPropertiesVariableStore{entity};
    result["scale"] = toJson(mdl::safeGetModelScale(
      pointDefinition->modelDefinition,
      variableStore,
      map.gameInfo().gameConfig.entityConfig.scaleExpression));
  }

  // the model data is loaded from the game files if the editor has not loaded it yet
  result.update(entityModelAnimationsJson(entity, loader, maxAnimations));

  if (auto rotation = rotationInfoJson(mdl::entityRotationInfo(entity));
      !rotation.is_null())
  {
    rotation["yawPitchRoll"] =
      toJson(mdl::entityYawPitchRoll(vm::mat4x4d::identity(), entity.rotation()));
    result["rotation"] = std::move(rotation);
  }
  return result;
}

ToolResult entityModelInfo(CallContext& context, const Args& args)
{
  const auto targets = modelInfoTargets(context, args);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  const auto& map = context.map();
  const auto maxAnimations = size_t(args.get<int>("maxAnimations"));
  auto loader = EntityModelLoader{map};
  auto entities = Json::array();
  for (const auto* entityNode : targets.value())
  {
    entities.push_back(
      modelInfoJson(map, context.ids(), *entityNode, loader, maxAnimations));
  }
  return Json{{"entities", std::move(entities)}};
}

} // namespace

Json entityDefinitionsResource(const mdl::Map& map)
{
  const auto definitionFile = entityDefinitionsJson(map);

  auto classes = Json::array();
  for (const auto* definition : sortedDefinitions(map))
  {
    auto item = classSummary(*definition);
    if (const auto& pointDefinition = definition->pointEntityDefinition)
    {
      item["size"] = toJson(pointDefinition->bounds);
    }
    item["propertyKeys"] = propertyKeys(*definition);
    if (const auto* flags = spawnflagsOf(*definition))
    {
      item["spawnflags"] = flagNames(*flags);
    }
    classes.push_back(std::move(item));
  }

  return Json{
    {"spec", definitionFile["spec"]},
    {"definitionFile", definitionFile},
    {"count", classes.size()},
    {"classes", std::move(classes)},
  };
}

void registerEntityClassTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"entity_classes_list"}
      .title("List Entity Classes")
      .description(
        "Lists the entity classes of the document's entity definitions (FGD, DEF or "
        "ENT), sorted by name; read-only. Filters are combined: type, prefix (classname "
        "prefix or a glob with * and ?; ignores case), group (the part before the first "
        "underscore, e.g. \"monster\"), search and used. Items: {name, type, group, "
        "description (first line), usageCount}; detail \"full\" adds color [r,g,b] "
        "(0..1), size {min,max} (point classes), model (default model path or null) and "
        "propertyKeys. groups counts the point, brush and used classes of all matches by "
        "group. Without definitions the list is empty and a NO_ENTITY_DEFINITIONS "
        "warning is returned. Use entity_class_describe for all details of a class. "
        "Examples: {\"prefix\": \"monster_\", \"limit\": 5}; {\"type\": \"brush\", "
        "\"search\": \"door\"}")
      .input(object({
        field("type", enumOf({"point", "brush"})).describe("Only classes of this type"),
        field("prefix", string().nonEmpty())
          .describe("Classname prefix, e.g. \"monster_\", or a glob such as \"*_door\""),
        field("group", string().nonEmpty())
          .describe("Definition group, the classname part before the first '_'"),
        field("search", string().nonEmpty())
          .describe("Case-insensitive substring of the name or the description"),
        field("used", boolean())
          .describe("true: only classes used in the map, false: only unused ones"),
      }))
      .output(object({
        field("items", array(any()))
          .required()
          .describe("[{name, type, group, description, usageCount}]"),
        field("total", integer()).required().describe("Number of matching classes"),
        field("nextCursor", any())
          .required()
          .describe("Cursor of the next page, or null"),
        field("groups", array(any()))
          .required()
          .describe("[{name, point, brush, used}] over all matches"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(entityClassesList));

  registry.add(
    ToolDef{"entity_class_describe"}
      .title("Describe Entity Class")
      .description(
        "Returns everything needed to place and configure an entity class; read-only: "
        "name, type (point or brush), group, full description, color [r,g,b] (0..1), "
        "size {min,max} (point classes, map units relative to the origin), model "
        "{default: {path, skin, frame} | null} and decal (point classes, else null), "
        "properties in definition order ({key, type, description, default, choices, "
        "flags, colorRange, linkRole, readOnly}), spawnflags ({bit, value, name, "
        "description, default}; set them by name with entity_spawnflags_set), "
        "linkProperties {sources (keys naming other entities, e.g. target), targets "
        "(the entity's own name, e.g. targetname)}, defaults {key: value}, rotation "
        "{key, type} (the property that orients the entity, or null) and usageCount. "
        "The classname is matched exactly, or ignoring case. An unknown class fails with "
        "INVALID_ARGUMENT listing similar names; find classes with entity_classes_list. "
        "Example: {\"classname\": \"monster_ogre\"}")
      .input(object({
        field("classname", string().nonEmpty())
          .required()
          .describe("The entity class, e.g. \"monster_ogre\""),
      }))
      .output(object({
        field("name", string()).required(),
        field("type", enumOf({"point", "brush"})).required(),
        field("group", string())
          .required()
          .describe("Classname part before the first '_'"),
        field("description", string()).required(),
        field("color", array(number())).required().describe("[r, g, b], 0..1"),
        field("size", any()).required().describe("{min, max} or null (brush classes)"),
        field("model", any())
          .required()
          .describe("{default: {path, skin, frame} | null} or null (brush classes)"),
        field("decal", any()).required().describe("{material} or null"),
        field("properties", array(any()))
          .required()
          .describe("[{key, type, description, default, choices, flags, colorRange, "
                    "linkRole, readOnly}] in definition order"),
        field("spawnflags", array(any()))
          .required()
          .describe("[{bit, value, name, description, default}]"),
        field(
          "linkProperties",
          object({
            field("sources", array(string()))
              .required()
              .describe("Keys that name other entities, e.g. target"),
            field("targets", array(string()))
              .required()
              .describe("Keys that hold the entity's own name, e.g. targetname"),
          }))
          .required(),
        field("defaults", any()).required().describe("{key: default value}"),
        field("rotation", any()).required().describe("{key, type, blocked?} or null"),
        field("usageCount", integer())
          .required()
          .describe("Entities of this class in the map"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(entityClassDescribe));

  registry.add(
    ToolDef{"entity_model_info"}
      .title("Get Entity Model Info")
      .description(
        "Returns the model of up to 50 entities (default: the selected entities) as the "
        "editor evaluates it with the entity's properties (e.g. spawnflags that pick "
        "another frame); read-only. Per entity: model {path, skin, frame} or null (brush "
        "entities, classes without model); modelLoaded (false if the game data is "
        "missing, reason in modelLoadError); modelBounds (world bounds of the current "
        "frame, else null); frameProperty (the property that selects the animation, "
        "e.g. \"sequence\"; null if the frame is fixed; set it with "
        "entity_animation_set); currentAnimation and animations ({index, name (null if "
        "the format has no names), bounds (model space), worldBounds (with the entity's "
        "origin, rotation and scale)}; at most maxAnimations, animationCount counts all, "
        "animationsTruncated); bounds (the entity's bounds); definitionBounds (the class "
        "size); scale [x,y,z]; rotation {key, type, yawPitchRoll} (degrees, or null if "
        "the entity cannot be oriented). modelError reports a model expression that "
        "failed to evaluate. Use entity_placement_check to test how the model stands. "
        "Example: {\"ids\": [\"entity:40\"], \"maxAnimations\": 2}")
      .input(object({
        field("ids", array(objectId({ObjectKind::Entity})).minSize(1).maxSize(50))
          .describe("Entity ids. Default: the selected entities"),
        field("maxAnimations", integer().min(0).max(1000).defaultsTo(100))
          .describe("List at most this many animations per entity; 0 lists none"),
      }))
      .output(object({
        field("entities", array(any())).required().describe("In the order of ids"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(entityModelInfo));
}

} // namespace tb::mcp
