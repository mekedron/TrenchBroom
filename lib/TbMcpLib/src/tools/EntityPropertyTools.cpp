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

#include "mcp/tools/EntityPropertyTools.h"

#include "EntityUtils.h"
#include "base/Color.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityLinkManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Node.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

namespace T = mdl::PropertyValueTypes;

const auto EntityTargetKinds = std::vector<ObjectKind>{
  ObjectKind::World,
  ObjectKind::Entity,
  ObjectKind::Brush,
  ObjectKind::Patch,
};

schema::Field entityIdsField()
{
  return idsField(
    EntityTargetKinds,
    "Entity ids, 'world' for worldspawn, or brushes/patches (meaning their entity). "
    "Default: the entities of the current selection");
}

/**
 * Collects warnings so that identical warnings for several entities are reported once,
 * with all their entity ids.
 */
class WarningCollector
{
private:
  std::vector<Warning> m_warnings;

public:
  void add(std::string code, std::string message, const std::string& objectId)
  {
    const auto it = std::ranges::find_if(m_warnings, [&](const auto& warning) {
      return warning.code == code && warning.message == message;
    });
    if (it == m_warnings.end())
    {
      m_warnings.push_back(Warning{std::move(code), std::move(message), {objectId}});
    }
    else if (std::ranges::find(it->objectIds, objectId) == it->objectIds.end())
    {
      it->objectIds.push_back(objectId);
    }
  }

  void addProblem(const std::optional<PropertyProblem>& problem, const std::string& id)
  {
    if (problem)
    {
      add(problem->code, problem->message, id);
    }
  }

  void emit(CallContext& context)
  {
    for (auto& warning : m_warnings)
    {
      context.warn(
        std::move(warning.code),
        std::move(warning.message),
        std::move(warning.objectIds));
    }
    m_warnings.clear();
  }
};

std::vector<std::string> formatIds(
  CallContext& context, const std::vector<mdl::EntityNodeBase*>& entities)
{
  auto result = std::vector<std::string>{};
  for (const auto* entityNode : entities)
  {
    result.push_back(context.ids().format(*entityNode));
  }
  return result;
}

std::string joined(const std::vector<std::string>& strings)
{
  auto result = std::string{};
  for (const auto& str : strings)
  {
    result += (result.empty() ? "" : ", ") + str;
  }
  return result;
}

bool containsQuote(const std::string& str)
{
  return str.find('"') != std::string::npos;
}

std::optional<ToolError> checkKey(const std::string& key)
{
  if (key.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Property keys must not be empty.",
      "Pass a key such as 'targetname'.");
  }
  if (containsQuote(key))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Property key '" + key + "' contains a double quote, which map files cannot store.",
      "Remove the double quotes from the key.");
  }
  return std::nullopt;
}

/** Runs a Map_* entity function on the given entities; fails if it returns false. */
template <typename F>
ToolResult applyToEntities(
  CallContext& context,
  const std::vector<mdl::EntityNodeBase*>& entities,
  const std::string& failure,
  F function)
{
  if (entities.empty())
  {
    return Json::object();
  }
  return withEntities(context, entities, [&]() -> ToolResult {
    if (!function())
    {
      return context.operationFailed(failure);
    }
    return Json::object();
  });
}

/** Sets a property on the given entities. */
ToolResult setProperty(
  CallContext& context,
  const std::vector<mdl::EntityNodeBase*>& entities,
  const std::string& key,
  const std::string& value)
{
  return applyToEntities(
    context, entities, "Could not set property '" + key + "'.", [&]() {
      return mdl::setEntityProperty(context.map(), key, value);
    });
}

/** The entities that have the given property. */
std::vector<mdl::EntityNodeBase*> entitiesWithProperty(
  const std::vector<mdl::EntityNodeBase*>& entities, const std::string& key)
{
  auto result = std::vector<mdl::EntityNodeBase*>{};
  std::ranges::copy_if(entities, std::back_inserter(result), [&](const auto* entityNode) {
    return entityNode->entity().hasProperty(key);
  });
  return result;
}

// entity_properties_set

std::string formatNumber(const Json& number)
{
  if (number.is_number_integer())
  {
    return number.dump();
  }
  const auto value = number.get<double>();
  if (value == std::floor(value) && std::abs(value) < 1e15)
  {
    return std::to_string(static_cast<long long>(value));
  }
  return number.dump();
}

/** The value to store for a JSON value, or nullopt for null (remove the key). */
Result<std::optional<std::string>, ToolError> propertyValue(
  const std::string& key, const Json& value)
{
  const auto invalid = [&]() {
    return makeError(
      ErrorCode::InvalidArgument,
      "The value of '" + key + "' must be a string, number, boolean, array of numbers "
        + "or null.",
      "Pass e.g. \"300\", 300, true, [255, 128, 0] or null (removes the key).");
  };

  if (value.is_null())
  {
    return std::optional<std::string>{};
  }
  if (value.is_string())
  {
    auto str = value.get<std::string>();
    if (containsQuote(str))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The value of '" + key
          + "' contains a double quote, which map files cannot store.",
        "Remove the double quotes from the value.");
    }
    return std::optional{std::move(str)};
  }
  if (value.is_boolean())
  {
    return std::optional<std::string>{value.get<bool>() ? "1" : "0"};
  }
  if (value.is_number())
  {
    return std::optional{formatNumber(value)};
  }
  if (value.is_array())
  {
    auto result = std::string{};
    for (const auto& component : value)
    {
      if (!component.is_number())
      {
        return invalid();
      }
      result += (result.empty() ? "" : " ") + formatNumber(component);
    }
    return std::optional{std::move(result)};
  }
  return invalid();
}

ToolResult entityPropertiesSet(CallContext& context, const Args& args)
{
  const auto properties = args.get<Json>("properties");
  if (properties.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "No properties were given.",
      "Pass e.g. {\"properties\": {\"targetname\": \"door1\"}}.");
  }

  // parse and check all values before changing anything
  auto toSet = std::vector<std::pair<std::string, std::string>>{};
  auto toRemove = std::vector<std::string>{};
  for (const auto& [key, value] : properties.items())
  {
    if (auto error = checkKey(key))
    {
      return *error;
    }
    auto parsed = propertyValue(key, value);
    if (parsed.is_error())
    {
      return errorOf(parsed);
    }
    if (auto str = std::move(parsed).value())
    {
      toSet.emplace_back(key, std::move(*str));
    }
    else if (key == mdl::EntityPropertyKeys::Classname)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The classname cannot be removed.",
        "Set a different classname instead, or delete the entity.");
    }
    else
    {
      toRemove.push_back(key);
    }
  }

  auto resolved = resolveEntities(context.map(), context.ids(), args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  const auto entities = resolved.value();
  const auto ids = formatIds(context, entities);

  // validate against the definition the entities will have after the call
  const auto newClassname = std::ranges::find_if(toSet, [](const auto& entry) {
    return entry.first == mdl::EntityPropertyKeys::Classname;
  });
  auto warnings = WarningCollector{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto* definition = newClassname != toSet.end()
                               ? findEntityDefinition(context.map(), newClassname->second)
                               : entities[i]->entity().definition();
    for (const auto& [key, value] : toSet)
    {
      if (key != mdl::EntityPropertyKeys::Classname)
      {
        warnings.addProblem(checkPropertyValue(definition, key, value), ids[i]);
      }
    }
  }
  if (newClassname != toSet.end())
  {
    warnUnknownClassname(context, newClassname->second, ids);
  }
  warnings.emit(context);

  auto set = Json::object();
  for (const auto& [key, value] : toSet)
  {
    if (auto result = setProperty(context, entities, key, value); result.is_error())
    {
      return result;
    }
    set[key] = value;
  }

  for (const auto& key : toRemove)
  {
    const auto withKey = entitiesWithProperty(entities, key);
    if (withKey.empty())
    {
      context.warn(
        "PROPERTY_NOT_PRESENT",
        "None of the entities has the property '" + key + "'.",
        ids);
      continue;
    }
    if (auto result = applyToEntities(
          context,
          withKey,
          "Could not remove property '" + key + "'.",
          [&]() { return mdl::removeEntityProperty(context.map(), key); });
        result.is_error())
    {
      return result;
    }
  }

  return Json{
    {"entities", ids},
    {"set", std::move(set)},
    {"removed", toRemove},
  };
}

// entity_property_remove

ToolResult entityPropertyRemove(CallContext& context, const Args& args)
{
  const auto keys = args.get<std::vector<std::string>>("keys");
  for (const auto& key : keys)
  {
    if (key == mdl::EntityPropertyKeys::Classname)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The classname cannot be removed.",
        "Set a different classname with entity_properties_set, or delete the entity.");
    }
    if (auto error = checkKey(key))
    {
      return *error;
    }
  }

  auto resolved = resolveEntities(context.map(), context.ids(), args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  const auto entities = resolved.value();
  const auto ids = formatIds(context, entities);

  auto removed = std::vector<std::string>{};
  for (const auto& key : keys)
  {
    const auto withKey = entitiesWithProperty(entities, key);
    if (withKey.empty())
    {
      context.warn(
        "PROPERTY_NOT_PRESENT",
        "None of the entities has the property '" + key + "'.",
        ids);
      continue;
    }
    if (auto result = applyToEntities(
          context,
          withKey,
          "Could not remove property '" + key + "'.",
          [&]() { return mdl::removeEntityProperty(context.map(), key); });
        result.is_error())
    {
      return result;
    }
    if (std::ranges::find(removed, key) == removed.end())
    {
      removed.push_back(key);
    }
  }

  return Json{
    {"entities", ids},
    {"removed", removed},
  };
}

// entity_property_rename

ToolResult entityPropertyRename(CallContext& context, const Args& args)
{
  const auto from = args.get<std::string>("from");
  const auto to = args.get<std::string>("to");
  if (from == to)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'from' and 'to' are the same key.",
      "Pass a different new key.");
  }
  if (
    from == mdl::EntityPropertyKeys::Classname
    || to == mdl::EntityPropertyKeys::Classname)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The classname cannot be renamed, and no property can be renamed to classname.",
      "Use entity_properties_set to change the classname.");
  }
  for (const auto& key : {from, to})
  {
    if (auto error = checkKey(key))
    {
      return *error;
    }
  }

  auto resolved = resolveEntities(context.map(), context.ids(), args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  const auto entities = resolved.value();
  const auto ids = formatIds(context, entities);

  auto missing = std::vector<std::string>{};
  auto overwritten = std::vector<std::string>{};
  auto renamed = std::vector<mdl::EntityNodeBase*>{};
  auto warnings = WarningCollector{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto& entity = entities[i]->entity();
    const auto* value = entity.property(from);
    if (!value)
    {
      missing.push_back(ids[i]);
      continue;
    }
    if (entity.hasProperty(to))
    {
      overwritten.push_back(ids[i]);
    }
    renamed.push_back(entities[i]);
    warnings.addProblem(checkPropertyValue(entity.definition(), to, *value), ids[i]);
  }

  if (!missing.empty())
  {
    context.warn(
      "PROPERTY_NOT_PRESENT",
      "Property '" + from + "' is not set on " + std::to_string(missing.size())
        + " of the entities; they were left unchanged.",
      missing);
  }
  if (!overwritten.empty())
  {
    context.warn(
      "PROPERTY_OVERWRITTEN",
      "Property '" + to + "' already existed and was replaced by the value of '" + from
        + "'.",
      overwritten);
  }
  warnings.emit(context);

  if (auto result = applyToEntities(
        context,
        renamed,
        "Could not rename property '" + from + "'.",
        [&]() { return mdl::renameEntityProperty(context.map(), from, to); });
      result.is_error())
  {
    return result;
  }

  return Json{
    {"entities", ids},
    {"from", from},
    {"to", to},
    {"renamed", formatIds(context, renamed)},
  };
}

// entity_spawnflags_set

std::string normalizeFlagName(std::string_view name)
{
  auto result = std::string{};
  for (const auto c : name)
  {
    if (!std::isspace(static_cast<unsigned char>(c)))
    {
      result.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return result;
}

/** Parses "bit8" or "256" (a single bit). */
std::optional<int> parseFlagBit(std::string_view name)
{
  auto str = normalizeFlagName(name);
  auto isBit = false;
  if (str.starts_with("bit"))
  {
    str = str.substr(3);
    isBit = true;
  }
  auto value = 0LL;
  const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  if (ec != std::errc{} || ptr != str.data() + str.size() || str.empty())
  {
    return std::nullopt;
  }
  if (isBit)
  {
    return value >= 0 && value < 31 ? std::optional{1 << value} : std::nullopt;
  }
  return value > 0 && value < (1LL << 31) && (value & (value - 1)) == 0
           ? std::optional{int(value)}
           : std::nullopt;
}

const T::Flags* flagsDefinition(
  const mdl::EntityDefinition* definition, const std::string& key)
{
  const auto* propertyDefinition = findPropertyDefinition(definition, key);
  return propertyDefinition ? std::get_if<T::Flags>(&propertyDefinition->valueType)
                            : nullptr;
}

/** The names of the flags set in the value, "bitN" for bits without a definition. */
std::vector<std::string> setFlagNames(const T::Flags* flags, const int value)
{
  auto result = std::vector<std::string>{};
  for (auto bit = 0; bit < 31; ++bit)
  {
    const auto flagValue = 1 << bit;
    if ((value & flagValue) == 0)
    {
      continue;
    }
    const auto* flag = flags ? flags->flag(flagValue) : nullptr;
    result.push_back(
      flag && !flag->shortDescription.empty() ? flag->shortDescription
                                              : "bit" + std::to_string(bit));
  }
  return result;
}

int intValue(const std::string* value)
{
  auto result = 0;
  if (value)
  {
    std::from_chars(value->data(), value->data() + value->size(), result);
  }
  return result;
}

ToolResult entitySpawnflagsSet(CallContext& context, const Args& args)
{
  const auto set = args.getOr<std::vector<std::string>>("set", {});
  const auto clear = args.getOr<std::vector<std::string>>("clear", {});
  const auto key = args.getOr<std::string>("key", mdl::EntityPropertyKeys::Spawnflags);
  if (set.empty() && clear.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Neither 'set' nor 'clear' names a flag.",
      "Pass e.g. {\"set\": [\"Not on Easy\"]}; entity_class_describe lists the flags.");
  }
  if (auto error = checkKey(key))
  {
    return *error;
  }

  auto resolved = resolveEntities(context.map(), context.ids(), args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  const auto entities = resolved.value();
  const auto ids = formatIds(context, entities);

  // resolve all flag names first so that the call is atomic
  struct UnknownFlags
  {
    std::vector<std::string> names;
    std::vector<std::string> ids;
    std::vector<std::string> known;
  };
  auto unknown = std::map<std::string, UnknownFlags>{};
  auto newValues = std::vector<int>{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto& entity = entities[i]->entity();
    const auto* flags = flagsDefinition(entity.definition(), key);

    auto setMask = 0;
    auto clearMask = 0;
    auto unknownNames = std::vector<std::string>{};
    const auto resolveNames = [&](const auto& names, int& mask) {
      for (const auto& name : names)
      {
        if (flags)
        {
          if (const auto* flag = findFlag(*flags, name))
          {
            mask |= flag->value;
            continue;
          }
        }
        else if (const auto value = parseFlagBit(name))
        {
          mask |= *value;
          continue;
        }
        unknownNames.push_back(name);
      }
    };
    resolveNames(set, setMask);
    resolveNames(clear, clearMask);

    if (!unknownNames.empty())
    {
      auto& entry = unknown[entity.classname()];
      for (const auto& name : unknownNames)
      {
        if (std::ranges::find(entry.names, name) == entry.names.end())
        {
          entry.names.push_back(name);
        }
      }
      entry.ids.push_back(ids[i]);
      entry.known = flags ? flagNames(*flags) : std::vector<std::string>{};
      continue;
    }
    if ((setMask & clearMask) != 0)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The same flag is both set and cleared for " + ids[i] + ".",
        "Name each flag in either 'set' or 'clear'.",
        {ids[i]});
    }

    newValues.push_back((intValue(entity.property(key)) | setMask) & ~clearMask);
  }

  if (!unknown.empty())
  {
    auto message = std::string{};
    auto objectIds = std::vector<std::string>{};
    for (const auto& [classname, entry] : unknown)
    {
      message += (message.empty() ? "" : " ") + std::string{"Unknown flag(s) "}
                 + joined(entry.names) + " for " + classname + " ("
                 + joined(entry.ids) + "); "
                 + (entry.known.empty()
                      ? "it defines no flags for '" + key
                          + "', so only bit names ('bit8') or values ('256') work."
                      : "its flags are: " + joined(entry.known) + ".");
      objectIds.insert(objectIds.end(), entry.ids.begin(), entry.ids.end());
    }
    return makeError(
      ErrorCode::InvalidArgument,
      message,
      "Use the flag names from entity_class_describe, or 'bitN' / the flag value.",
      objectIds);
  }

  // group the entities by their new value to set it with as few commands as possible
  auto byValue = std::map<int, std::vector<mdl::EntityNodeBase*>>{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto* oldValue = entities[i]->entity().property(key);
    if (!oldValue || intValue(oldValue) != newValues[i])
    {
      byValue[newValues[i]].push_back(entities[i]);
    }
  }
  for (const auto& [value, entitiesWithValue] : byValue)
  {
    if (auto result = setProperty(context, entitiesWithValue, key, std::to_string(value));
        result.is_error())
    {
      return result;
    }
  }

  auto result = Json::array();
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto& entity = entities[i]->entity();
    const auto* flags = flagsDefinition(entity.definition(), key);
    result.push_back(Json{
      {"id", ids[i]},
      {"classname", entity.classname()},
      {"spawnflags", newValues[i]},
      {"flags", setFlagNames(flags, newValues[i])},
    });
  }
  return Json{{"entities", std::move(result)}};
}

// entity_defaults_apply

ToolResult entityDefaultsApply(CallContext& context, const Args& args)
{
  const auto modeName = args.get<std::string>("mode");
  const auto mode = modeName == "existing" ? mdl::SetDefaultPropertyMode::SetExisting
                    : modeName == "all"    ? mdl::SetDefaultPropertyMode::SetAll
                                           : mdl::SetDefaultPropertyMode::SetMissing;

  auto resolved = resolveEntities(context.map(), context.ids(), args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  const auto entities = resolved.value();
  const auto ids = formatIds(context, entities);

  auto withDefinition = std::vector<mdl::EntityNodeBase*>{};
  auto before = std::vector<std::vector<mdl::EntityProperty>>{};
  auto warnings = WarningCollector{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto& entity = entities[i]->entity();
    before.push_back(entity.properties());
    if (entity.definition())
    {
      withDefinition.push_back(entities[i]);
    }
    else
    {
      warnings.add(
        "NO_DEFINITION",
        "Class '" + entity.classname()
          + "' has no entity definition, so it has no default values.",
        ids[i]);
    }
  }
  warnings.emit(context);

  if (!withDefinition.empty())
  {
    auto result = withEntities(context, withDefinition, [&]() -> ToolResult {
      mdl::setDefaultEntityProperties(context.map(), mode);
      return Json::object();
    });
    if (result.is_error())
    {
      return result;
    }
  }

  auto result = Json::array();
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto& entity = entities[i]->entity();
    auto set = Json::object();
    for (const auto& property : entity.properties())
    {
      const auto it = std::ranges::find_if(before[i], [&](const auto& oldProperty) {
        return oldProperty.key() == property.key();
      });
      if (it == before[i].end() || it->value() != property.value())
      {
        set[property.key()] = property.value();
      }
    }
    result.push_back(Json{
      {"id", ids[i]},
      {"classname", entity.classname()},
      {"set", std::move(set)},
    });
  }
  return Json{{"entities", std::move(result)}};
}

// entity_links_get

void collectEntities(mdl::Node& node, std::vector<mdl::EntityNodeBase*>& result)
{
  if (auto* entityNode = dynamic_cast<mdl::EntityNodeBase*>(&node))
  {
    result.push_back(entityNode);
  }
  if (!dynamic_cast<mdl::EntityNode*>(&node))
  {
    for (auto* child : node.children())
    {
      collectEntities(*child, result);
    }
  }
}

std::vector<mdl::EntityNodeBase*> allEntities(mdl::Map& map)
{
  auto result = std::vector<mdl::EntityNodeBase*>{};
  collectEntities(map.worldNode(), result);
  return result;
}

std::vector<std::string> propertyKeys(
  const std::vector<const mdl::PropertyDefinition*>& definitions)
{
  auto result = std::vector<std::string>{};
  for (const auto* definition : definitions)
  {
    result.push_back(definition->key);
  }
  return result;
}

std::vector<std::string> linkSourceKeys(const mdl::EntityNodeBase& entityNode)
{
  return propertyKeys(
    mdl::getLinkSourcePropertyDefinitions(entityNode.entity().definition()));
}

std::vector<std::string> linkTargetKeys(const mdl::EntityNodeBase& entityNode)
{
  return propertyKeys(
    mdl::getLinkTargetPropertyDefinitions(entityNode.entity().definition()));
}

ToolResult entityLinksGet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto entities = allEntities(map);
  auto order = std::unordered_map<const mdl::EntityNodeBase*, size_t>{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    order[entities[i]] = i;
  }

  // without ids, the whole map is listed (not the selection)
  auto scope = std::unordered_set<const mdl::EntityNodeBase*>{};
  if (args.has("ids"))
  {
    auto resolved = resolveEntities(context.map(), context.ids(), args);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    scope.insert(resolved.value().begin(), resolved.value().end());
  }
  else
  {
    scope.insert(entities.begin(), entities.end());
  }

  const auto brokenOnly = args.getOr<bool>("brokenOnly", false);
  const auto includeUnreferenced = args.getOr<bool>("includeUnreferenced", false);
  const auto& linkManager = map.entityLinkManager();

  auto items = std::vector<Json>{};
  auto links = size_t(0);
  auto missingTarget = size_t(0);
  auto missingSource = size_t(0);

  for (auto* source : entities)
  {
    const auto sourceInScope = scope.contains(source);
    for (const auto& key : linkSourceKeys(*source))
    {
      const auto& linkEnds = linkManager.linksFrom(*source, key);
      for (const auto& property : source->entity().numberedProperties(key))
      {
        const auto& name = property.value();
        if (name.empty())
        {
          continue;
        }

        auto targets = std::vector<const mdl::LinkEnd*>{};
        for (const auto& linkEnd : linkEnds)
        {
          if (linkEnd.node->entity().hasNumberedProperty(linkEnd.propertyKey, name))
          {
            targets.push_back(&linkEnd);
          }
        }
        std::ranges::sort(targets, [&](const auto* lhs, const auto* rhs) {
          return std::tuple{order[lhs->node], lhs->propertyKey}
                 < std::tuple{order[rhs->node], rhs->propertyKey};
        });

        if (targets.empty())
        {
          if (sourceInScope)
          {
            ++missingTarget;
            items.push_back(Json{
              {"source", ids.format(*source)},
              {"sourceKey", property.key()},
              {"target", nullptr},
              {"name", name},
              {"broken", "missing_target"},
            });
          }
          continue;
        }

        for (const auto* target : targets)
        {
          if (!sourceInScope && !scope.contains(target->node))
          {
            continue;
          }
          ++links;
          if (!brokenOnly)
          {
            items.push_back(Json{
              {"source", ids.format(*source)},
              {"sourceKey", property.key()},
              {"target", ids.format(*target->node)},
              {"targetKey", target->propertyKey},
              {"name", name},
            });
          }
        }
      }
    }
  }

  for (auto* target : entities)
  {
    if (!scope.contains(target))
    {
      continue;
    }
    for (const auto& key : linkTargetKeys(*target))
    {
      const auto& linkEnds = linkManager.linksTo(*target, key);
      for (const auto& property : target->entity().numberedProperties(key))
      {
        const auto& name = property.value();
        if (name.empty() || std::ranges::any_of(linkEnds, [&](const auto& linkEnd) {
              return linkEnd.node->entity().hasNumberedProperty(
                linkEnd.propertyKey, name);
            }))
        {
          continue;
        }
        ++missingSource;
        if (includeUnreferenced)
        {
          items.push_back(Json{
            {"target", ids.format(*target)},
            {"targetKey", property.key()},
            {"name", name},
            {"broken", "missing_source"},
          });
        }
      }
    }
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  result["counts"] = Json{
    {"links", links},
    {"missingTarget", missingTarget},
    {"missingSource", missingSource},
  };
  return result;
}

// entity_link

Result<mdl::EntityNodeBase*, ToolError> resolveEntity(
  CallContext& context, const std::string& id)
{
  auto resolved = resolveEntities(
    context.map(), context.ids(), Args{Json{{"ids", Json::array({id})}}}, "ids");
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  return resolved.value().front();
}

bool isLinkProperty(const mdl::EntityDefinition* definition, const std::string& key)
{
  using namespace mdl::EntityPropertyKeys;
  if (
    key == Targetname || mdl::isNumberedProperty(Target, key)
    || mdl::isNumberedProperty(Killtarget, key))
  {
    return true;
  }
  const auto* propertyDefinition = findPropertyDefinition(definition, key);
  return propertyDefinition
         && (std::holds_alternative<T::LinkSource>(propertyDefinition->valueType)
             || std::holds_alternative<T::LinkTarget>(propertyDefinition->valueType));
}

/** `<classname>_<n>`, unique among all link names in the map. */
std::string generateLinkName(mdl::Map& map, const std::string& classname)
{
  auto used = std::unordered_set<std::string>{};
  for (const auto* entityNode : allEntities(map))
  {
    const auto& entity = entityNode->entity();
    for (const auto& property : entity.properties())
    {
      if (isLinkProperty(entity.definition(), property.key()))
      {
        used.insert(property.value());
      }
    }
  }

  const auto prefix = classname.empty() ? std::string{"entity"} : classname;
  for (auto n = size_t(1);; ++n)
  {
    auto name = prefix + "_" + std::to_string(n);
    if (!used.contains(name))
    {
      return name;
    }
  }
}

ToolResult entityLink(CallContext& context, const Args& args)
{
  const auto name = args.getOptional<std::string>("name");
  if (
    name
    && (name->empty() || containsQuote(*name) || std::ranges::any_of(*name, [](const auto c) {
          return std::isspace(static_cast<unsigned char>(c));
        })))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The link name '" + *name + "' must not be empty or contain whitespace or quotes.",
      "Pass a name such as 'door_1', or omit it to generate one.");
  }

  auto sourceResult = resolveEntity(context, args.get<std::string>("source"));
  if (sourceResult.is_error())
  {
    return errorOf(sourceResult);
  }
  auto targetResult = resolveEntity(context, args.get<std::string>("target"));
  if (targetResult.is_error())
  {
    return errorOf(targetResult);
  }
  auto* source = sourceResult.value();
  auto* target = targetResult.value();
  const auto sourceId = context.ids().format(*source);
  const auto targetId = context.ids().format(*target);
  if (source == target)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "An entity cannot be linked to itself.",
      "Pass two different entities.",
      {sourceId});
  }

  const auto sourceKeys = linkSourceKeys(*source);
  const auto targetKeys = linkTargetKeys(*target);
  const auto sourceKey =
    args.getOptional<std::string>("sourceKey")
      .value_or(
        sourceKeys.empty() ? mdl::EntityPropertyKeys::Target : sourceKeys.front());
  const auto targetKey =
    args.getOptional<std::string>("targetKey")
      .value_or(
        targetKeys.empty() ? mdl::EntityPropertyKeys::Targetname : targetKeys.front());
  for (const auto& key : {sourceKey, targetKey})
  {
    if (auto error = checkKey(key))
    {
      return *error;
    }
    if (key == mdl::EntityPropertyKeys::Classname)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The classname cannot be used as a link key.",
        "Omit sourceKey / targetKey to use the class's link properties.");
    }
  }

  // reuse the target's name if it has one
  auto linkName = std::string{};
  auto generated = false;
  const auto* existingName = target->entity().property(targetKey);
  if (existingName && !existingName->empty())
  {
    linkName = *existingName;
    if (name && *name != linkName)
    {
      context.warn(
        "TARGET_ALREADY_NAMED",
        "The target is already named '" + linkName + "' ('" + targetKey
          + "'); that name was used instead of '" + *name + "'.",
        {targetId});
    }
  }
  else if (name)
  {
    linkName = *name;
  }
  else
  {
    linkName = generateLinkName(context.map(), target->entity().classname());
    generated = true;
  }

  const auto* oldSourceValue = source->entity().property(sourceKey);
  if (oldSourceValue && !oldSourceValue->empty() && *oldSourceValue != linkName)
  {
    context.warn(
      "LINK_REPLACED",
      "'" + sourceKey + "' of the source was '" + *oldSourceValue
        + "'; that link was replaced.",
      {sourceId});
  }

  auto warnings = WarningCollector{};
  warnings.addProblem(
    checkPropertyValue(source->entity().definition(), sourceKey, linkName), sourceId);
  warnings.addProblem(
    checkPropertyValue(target->entity().definition(), targetKey, linkName), targetId);
  warnings.emit(context);

  if (!existingName || *existingName != linkName)
  {
    if (auto result = setProperty(context, {target}, targetKey, linkName);
        result.is_error())
    {
      return result;
    }
  }
  if (!oldSourceValue || *oldSourceValue != linkName)
  {
    if (auto result = setProperty(context, {source}, sourceKey, linkName);
        result.is_error())
    {
      return result;
    }
  }

  return Json{
    {"source", sourceId},
    {"target", targetId},
    {"sourceKey", sourceKey},
    {"targetKey", targetKey},
    {"name", linkName},
    {"generated", generated},
  };
}

// entity_color_set

/** The class's first color property, else "_color" (the compilers' light color key). */
std::string defaultColorKey(const mdl::EntityDefinition* definition)
{
  if (definition)
  {
    for (const auto& propertyDefinition : definition->propertyDefinitions)
    {
      if (colorRangeName(propertyDefinition.valueType))
      {
        return propertyDefinition.key;
      }
    }
  }
  return "_color";
}

bool acceptsColor(const mdl::PropertyValueType& valueType)
{
  return colorRangeName(valueType) || std::holds_alternative<T::String>(valueType)
         || std::holds_alternative<T::Unknown>(valueType);
}

ToolResult entityColorSet(CallContext& context, const Args& args)
{
  const auto components = args.get<std::vector<double>>("color");
  const auto rangeName = args.get<std::string>("range");
  const auto isFloat =
    rangeName == "float"
    || (rangeName == "auto" && std::ranges::all_of(components, [](const auto c) {
          return c <= 1.0;
        }));

  for (const auto component : components)
  {
    if (
      component < 0.0 || (isFloat && component > 1.0)
      || (!isFloat && (component > 255.0 || component != std::floor(component))))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        std::string{"The color components must be "}
          + (isFloat ? "between 0 and 1 (float range)." : "integers between 0 and 255 (byte range)."),
        "Pass e.g. [1, 0.5, 0] (float) or [255, 128, 0] (byte).");
    }
  }

  const auto color =
    isFloat
      ? Rgb{RgbF{float(components[0]), float(components[1]), float(components[2])}}
      : Rgb{RgbB{uint8_t(components[0]), uint8_t(components[1]), uint8_t(components[2])}};

  const auto explicitKey = args.getOptional<std::string>("key");
  if (explicitKey)
  {
    if (auto error = checkKey(*explicitKey))
    {
      return *error;
    }
  }

  auto resolved = resolveEntities(context.map(), context.ids(), args);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  const auto entities = resolved.value();
  const auto ids = formatIds(context, entities);

  auto keys = std::vector<std::string>{};
  auto byKey = std::map<std::string, std::vector<mdl::EntityNodeBase*>>{};
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto* definition = entities[i]->entity().definition();
    auto key = explicitKey.value_or(defaultColorKey(definition));
    if (const auto* propertyDefinition = findPropertyDefinition(definition, key);
        propertyDefinition && !acceptsColor(propertyDefinition->valueType))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Property '" + key + "' of " + definition->name + " is not a color (type "
          + propertyTypeName(propertyDefinition->valueType) + ").",
        "Pass the key of a color property; entity_class_describe lists the "
        "properties.",
        {ids[i]});
    }
    byKey[key].push_back(entities[i]);
    keys.push_back(std::move(key));
  }

  for (const auto& [key, entitiesWithKey] : byKey)
  {
    if (auto result = applyToEntities(
          context,
          entitiesWithKey,
          "Could not set color property '" + key + "'.",
          [&]() { return mdl::setEntityColorProperty(context.map(), key, color); });
        result.is_error())
    {
      return result;
    }
  }

  auto result = Json::array();
  for (size_t i = 0; i < entities.size(); ++i)
  {
    const auto* value = entities[i]->entity().property(keys[i]);
    result.push_back(Json{
      {"id", ids[i]},
      {"key", keys[i]},
      {"value", value ? Json(*value) : Json(nullptr)},
    });
  }
  return Json{{"entities", std::move(result)}};
}

} // namespace

void registerEntityPropertyTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"entity_properties_set"}
      .title("Set Entity Properties")
      .description(
        "Sets properties on entities (worldspawn included: 'world'). Values may be "
        "strings, numbers, booleans (1/0), arrays of numbers (joined with spaces, e.g. "
        "colors and angles) or null (removes the key). Values are checked against the "
        "entity definitions and problems are warnings (UNKNOWN_PROPERTY, "
        "INVALID_PROPERTY_VALUE, INVALID_CHOICE, UNKNOWN_FLAGS, UNKNOWN_CLASSNAME); "
        "setting classname changes the class. Example: {\"ids\": [\"entity:40\"], "
        "\"properties\": {\"targetname\": \"door1\", \"speed\": 200, \"wait\": null}}")
      .input(object({
        entityIdsField(),
        field("properties", object({}).allowAdditionalProperties())
          .required()
          .describe("Key -> value (string, number, boolean, array of numbers, or null)"),
      }))
      .output(object({
        field("entities", array(string())).required().describe("The changed entities"),
        field("set", object({}).allowAdditionalProperties())
          .required()
          .describe("The stored values by key"),
        field("removed", array(string())).required().describe("The removed keys"),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityPropertiesSet));

  registry.add(
    ToolDef{"entity_property_remove"}
      .title("Remove Entity Properties")
      .description(
        "Removes properties from entities. Keys that none of the entities has are "
        "reported as PROPERTY_NOT_PRESENT warnings; classname cannot be removed. "
        "Example: {\"ids\": [\"entity:40\"], \"keys\": [\"wait\", \"lip\"]}")
      .input(object({
        entityIdsField(),
        field("keys", array(string()).nonEmpty()).required().describe("Keys to remove"),
      }))
      .output(object({
        field("entities", array(string())).required(),
        field("removed", array(string()))
          .required()
          .describe("The keys that were removed from at least one entity"),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .destructive()
      .handler(entityPropertyRemove));

  registry.add(
    ToolDef{"entity_property_rename"}
      .title("Rename Entity Property")
      .description(
        "Renames a property key on entities, keeping its value. Entities without the "
        "key are left unchanged (PROPERTY_NOT_PRESENT); an existing property with the "
        "new key is replaced (PROPERTY_OVERWRITTEN). The new key is checked against the "
        "entity definitions. Example: {\"ids\": [\"entity:40\"], \"from\": "
        "\"target2\", \"to\": \"killtarget\"}")
      .input(object({
        entityIdsField(),
        field("from", string().nonEmpty()).required().describe("The current key"),
        field("to", string().nonEmpty()).required().describe("The new key"),
      }))
      .output(object({
        field("entities", array(string())).required(),
        field("from", string()).required(),
        field("to", string()).required(),
        field("renamed", array(string()))
          .required()
          .describe("The entities that had the key and were changed"),
      }))
      .mutation(Mutation::Map)
      .handler(entityPropertyRename));

  registry.add(
    ToolDef{"entity_spawnflags_set"}
      .title("Set Spawnflags")
      .description(
        "Sets and clears spawnflags by name, as the entity definition names them "
        "(case-insensitive, spaces and underscores interchangeable), or by bit "
        "('bit8') or value ('256'). Names are resolved for each entity's class; an "
        "unknown name fails the whole call and lists the class's flags. Entities "
        "without a flag definition accept only bits and values. Example: {\"ids\": "
        "[\"entity:40\"], \"set\": [\"Not on Easy\", \"Not on Normal\"], \"clear\": "
        "[\"Ambush\"]}")
      .input(object({
        entityIdsField(),
        field("set", array(string())).describe("Flags to set"),
        field("clear", array(string())).describe("Flags to clear"),
        field("key", string().nonEmpty().defaultsTo("spawnflags"))
          .describe("The flags property"),
      }))
      .output(object({
        field(
          "entities",
          array(object({
            field("id", string()).required(),
            field("classname", string()).required(),
            field("spawnflags", integer()).required().describe("The new value"),
            field("flags", array(string()))
              .required()
              .describe("Names of the flags that are set ('bitN' if unnamed)"),
          })))
          .required(),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entitySpawnflagsSet));

  registry.add(
    ToolDef{"entity_defaults_apply"}
      .title("Apply Entity Defaults")
      .description(
        "Applies the default values of the entity definitions: 'missing' adds "
        "properties that are not set, 'existing' resets properties that are set, 'all' "
        "does both. Entities without a definition are skipped (NO_DEFINITION). Returns "
        "what changed per entity. Example: {\"ids\": [\"entity:40\"], \"mode\": "
        "\"missing\"}")
      .input(object({
        entityIdsField(),
        field("mode", enumOf({"missing", "existing", "all"}).defaultsTo("missing"))
          .describe("Which properties to set to their defaults"),
      }))
      .output(object({
        field(
          "entities",
          array(object({
            field("id", string()).required(),
            field("classname", string()).required(),
            field("set", object({}).allowAdditionalProperties())
              .required()
              .describe("The properties that were added or changed"),
          })))
          .required(),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityDefaultsApply));

  registry.add(
    ToolDef{"entity_links_get"}
      .title("Get Entity Links")
      .description(
        "Lists the link relations between entities (e.g. target -> targetname, as "
        "defined by the entity definitions). Without ids, the whole map is listed (not "
        "the selection); with ids, links from or to those entities. Items are links "
        "{source, sourceKey, target, targetKey, name}, broken links {source, sourceKey, "
        "name, target: null, broken: \"missing_target\"} and, with includeUnreferenced, "
        "names nobody refers to {target, targetKey, name, broken: \"missing_source\"}. "
        "counts cover the whole scope regardless of the filters. Example: "
        "{\"brokenOnly\": true}")
      .input(object({
        field("ids", array(objectId(EntityTargetKinds)).nonEmpty())
          .describe(
            "Entities (or 'world', brushes of entities) whose links to list. Default: "
            "the whole map"),
        field("brokenOnly", boolean().defaultsTo(false))
          .describe("List broken links only"),
        field("includeUnreferenced", boolean().defaultsTo(false))
          .describe("Also list target names that no entity refers to"),
      }))
      .output(object({
        field("items", array(any())).required(),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field(
          "counts",
          object({
            field("links", integer()).required(),
            field("missingTarget", integer()).required(),
            field("missingSource", integer()).required(),
          }))
          .required(),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(entityLinksGet));

  registry.add(
    ToolDef{"entity_link"}
      .title("Link Entities")
      .description(
        "Links a source entity to a target (e.g. a trigger to a door): sets the "
        "target's name property (targetKey, default: its class's first link target "
        "property, else 'targetname') and the source's link property (sourceKey, "
        "default: its class's first link source property, else 'target') to the same "
        "name. An existing target name is reused; otherwise name is used or a unique "
        "one is generated ('<classname>_<n>'). Replacing a different source value "
        "warns LINK_REPLACED. Example: {\"source\": \"entity:41\", \"target\": "
        "\"entity:40\"}")
      .input(object({
        field("source", objectId({ObjectKind::Entity}))
          .required()
          .describe("The entity that triggers (e.g. trigger_once)"),
        field("target", objectId({ObjectKind::Entity}))
          .required()
          .describe("The entity that is triggered (e.g. func_door)"),
        field("sourceKey", string().nonEmpty()).describe("Default: e.g. 'target'"),
        field("targetKey", string().nonEmpty()).describe("Default: e.g. 'targetname'"),
        field("name", string().nonEmpty())
          .describe("Name to use if the target has none. Default: generated"),
      }))
      .output(object({
        field("source", string()).required(),
        field("target", string()).required(),
        field("sourceKey", string()).required(),
        field("targetKey", string()).required(),
        field("name", string()).required(),
        field("generated", boolean()).required().describe("Whether the name is new"),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityLink));

  registry.add(
    ToolDef{"entity_color_set"}
      .title("Set Entity Color")
      .description(
        "Sets a color property. The color is given as 3 numbers in float (0..1) or "
        "byte (0..255) range ('auto': float if all components are at most 1) and "
        "stored in the range the entity definition declares for the property; extra "
        "components such as a brightness are kept. key defaults to the class's first "
        "color property, else '_color' (the light color key of the Quake, Quake 2 and "
        "Quake 3 compilers). Example: {\"ids\": [\"entity:40\"], \"color\": [255, 128, "
        "0]}")
      .input(object({
        entityIdsField(),
        field("key", string().nonEmpty())
          .describe("Default: the class's first color property, else '_color'"),
        field("color", array(number()).minSize(3).maxSize(3))
          .required()
          .describe("[r, g, b]"),
        field("range", enumOf({"auto", "float", "byte"}).defaultsTo("auto"))
          .describe("The range of the given components"),
      }))
      .output(object({
        field(
          "entities",
          array(object({
            field("id", string()).required(),
            field("key", string()).required(),
            field("value", any()).required().describe("The stored value"),
          })))
          .required(),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityColorSet));
}

} // namespace tb::mcp
