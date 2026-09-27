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

#pragma once

#include "base/Result.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mdl/PropertyDefinition.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::mdl
{
class EntityNodeBase;
class Map;
class Node;
struct EntityDefinition;
} // namespace tb::mdl

namespace tb::mcp
{
class Args;
class CallContext;
class IdRegistry;

// Shared helpers of the entity tools (epic E5): definition lookup, property type
// descriptions, value validation (spec X14) and entity targeting. Keep this small; it
// must not grow into a second tool file.

/** The definition of the given classname in the document's entity definitions. */
const mdl::EntityDefinition* findEntityDefinition(
  const mdl::Map& map, std::string_view classname);

/** "point" or "brush". */
std::string entityDefinitionTypeName(const mdl::EntityDefinition& definition);

/**
 * The type of a property as published to agents: "string", "boolean", "integer",
 * "float", "choice", "flags", "origin", "color", "link_source" (the property names other
 * entities, e.g. target), "link_target" (the entity's own name, e.g. targetname),
 * "input", "output" or "unknown".
 */
std::string propertyTypeName(const mdl::PropertyValueType& valueType);

/**
 * The color range of a color property: "float" (components 0..1), "byte" (0..255) or
 * "any"; nullopt for other types.
 */
std::optional<std::string> colorRangeName(const mdl::PropertyValueType& valueType);

/**
 * Describes a property definition: `{"key", "type", "description"}` plus, where
 * applicable, `"longDescription"`, `"default"`, `"readOnly"`, `"choices": [{"value",
 * "description"}]`, `"flags": [{"bit", "value", "name", "description", "default"}]`,
 * `"colorRange"` and `"linkRole": "source" | "target"`.
 */
Json propertyDefinitionJson(const mdl::PropertyDefinition& definition);

/** The definition of the given property key, also for numbered keys ("target2"). */
const mdl::PropertyDefinition* findPropertyDefinition(
  const mdl::EntityDefinition* definition, const std::string& key);

/**
 * Finds a flag of a flags property by name: the flag's short description (the name the
 * definition file gives it, case-insensitive; spaces and underscores are
 * interchangeable), or its value ("256") or bit number ("bit8").
 */
const mdl::PropertyValueTypes::Flag* findFlag(
  const mdl::PropertyValueTypes::Flags& flags, std::string_view name);

/** The names of all flags of a flags property, for error messages. */
std::vector<std::string> flagNames(const mdl::PropertyValueTypes::Flags& flags);

/** A problem with a property value (X14). */
struct PropertyProblem
{
  /**
   * UNKNOWN_PROPERTY, INVALID_PROPERTY_VALUE, INVALID_CHOICE, UNKNOWN_FLAGS or
   * READ_ONLY_PROPERTY.
   */
  std::string code;
  std::string message;
};

/**
 * Checks a property value against the entity definition. Keys that are not defined are
 * reported as UNKNOWN_PROPERTY, except well-known keys (classname, origin, angle(s),
 * mangle, spawnflags, target, targetname, killtarget, model, TrenchBroom's _tb_* keys)
 * and compiler keys starting with an underscore. Returns nothing without a definition.
 */
std::optional<PropertyProblem> checkPropertyValue(
  const mdl::EntityDefinition* definition,
  const std::string& key,
  const std::string& value);

/**
 * Warns with UNKNOWN_CLASSNAME if the classname has no definition in the document's
 * entity definitions. Does nothing if the document has no definitions at all.
 */
void warnUnknownClassname(
  CallContext& context, std::string_view classname, std::vector<std::string> objectIds);

/**
 * Validates a property that is about to be set on entities with the given definition
 * (spec X14) and adds a warning for each problem. Setting "classname" checks the new
 * classname instead. Never fails.
 */
void validateProperty(
  CallContext& context,
  const mdl::EntityDefinition* definition,
  const std::string& key,
  const std::string& value,
  std::vector<std::string> objectIds);

/**
 * Resolves the entities a tool acts on: the ids in the argument `key` (entities, `world`
 * for worldspawn, brushes and patches for the entity that contains them) or, without ids,
 * the entities of the current selection (the entities of selected brushes, worldspawn for
 * world brushes). Fails with NO_SELECTION if nothing is selected, and with
 * WRONG_OBJECT_KIND / OBJECT_NOT_EDITABLE like resolveTargets. The result contains no
 * duplicates.
 */
Result<std::vector<mdl::EntityNodeBase*>, ToolError> resolveEntities(
  mdl::Map& map, const IdRegistry& ids, const Args& args, std::string_view key = "ids");

/**
 * Runs the function so that `map.selection().allEntities()` yields the given entities,
 * which is what the Map_* entity functions act on: selects the point entities and the
 * brushes of brush entities, or selects nothing for worldspawn. Worldspawn is handled in
 * a separate run because the editor ignores it when other entities are selected. The
 * selection is restored afterwards. Stops at the first error.
 */
ToolResult withEntities(
  CallContext& context,
  const std::vector<mdl::EntityNodeBase*>& entities,
  const std::function<ToolResult()>& function);

/** Key-value pairs to set; a missing value removes the key. */
using PropertyList = std::vector<std::pair<std::string, std::optional<std::string>>>;

/** Formats a number as a property value: integers without decimals. */
std::string formatPropertyNumber(const Json& value);

/**
 * The property value given as JSON: strings as they are, numbers formatted, booleans as
 * "1" / "0" and arrays of numbers as space separated numbers ("255 128 0"); nullopt for
 * other values.
 */
std::optional<std::string> propertyValueFromJson(const Json& value);

/** Whether the string contains a double quote, which map files cannot store. */
bool containsQuote(std::string_view str);

/**
 * The properties of a JSON object as key-value strings (propertyValueFromJson); null
 * values (no value in the result) remove the key, like in entity_properties_set. Fails
 * with INVALID_ARGUMENT for invalid keys or values, `classname` (it has its own argument)
 * and, unless `allowOrigin`, `origin` (point entities take `position`).
 */
Result<PropertyList, ToolError> propertiesFromJson(
  const Json& properties, bool allowOrigin);

} // namespace tb::mcp
