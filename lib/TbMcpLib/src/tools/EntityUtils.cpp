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

#include "EntityUtils.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ObjectIds.h"
#include "mcp/Targets.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/Map.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <sstream>

namespace tb::mcp
{
namespace
{

std::string lowercase(std::string_view str)
{
  auto result = std::string{str};
  std::ranges::transform(result, result.begin(), [](const unsigned char c) {
    return c == ' ' ? '_' : char(std::tolower(c));
  });
  return result;
}

std::string_view trim(std::string_view str)
{
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front())))
  {
    str.remove_prefix(1);
  }
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))
  {
    str.remove_suffix(1);
  }
  return str;
}

/** Parses the whole string as an integer (no trailing characters). */
std::optional<long long> parseInteger(std::string_view str)
{
  str = trim(str);
  if (!str.empty() && str.front() == '+')
  {
    str.remove_prefix(1);
  }
  auto value = 0LL;
  const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc{} && ptr == str.data() + str.size() && !str.empty()
           ? std::optional{value}
           : std::nullopt;
}

/** Parses the whole string as a number (no trailing characters). */
std::optional<double> parseNumber(std::string_view str)
{
  str = trim(str);
  if (!str.empty() && str.front() == '+')
  {
    str.remove_prefix(1);
  }
  auto value = 0.0;
  const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc{} && ptr == str.data() + str.size() && !str.empty()
             && std::isfinite(value)
           ? std::optional{value}
           : std::nullopt;
}

/** Splits at whitespace and parses every component as a number. */
std::optional<std::vector<double>> parseNumbers(const std::string& str)
{
  auto result = std::vector<double>{};
  auto stream = std::istringstream{str};
  auto token = std::string{};
  while (stream >> token)
  {
    const auto number = parseNumber(token);
    if (!number)
    {
      return std::nullopt;
    }
    result.push_back(*number);
  }
  return result;
}

bool isWellKnownKey(const std::string& key)
{
  using namespace mdl::EntityPropertyKeys;
  static const auto keys = std::vector<std::string>{
    Classname,
    Origin,
    Angle,
    Angles,
    Mangle,
    Spawnflags,
    Target,
    Targetname,
    Killtarget,
    "model",
  };
  return key.starts_with("_") || std::ranges::find(keys, key) != keys.end()
         || mdl::isNumberedProperty(Target, key)
         || mdl::isNumberedProperty(Killtarget, key);
}

std::string describeValue(const std::string& key, const std::string& value)
{
  return "'" + key + "' = '" + value + "'";
}

std::optional<PropertyProblem> invalidValue(
  const std::string& key, const std::string& value, const std::string& expected)
{
  return PropertyProblem{
    "INVALID_PROPERTY_VALUE",
    describeValue(key, value) + " is not " + expected + ".",
  };
}

std::optional<PropertyProblem> checkColor(
  const std::string& key,
  const std::string& value,
  const std::optional<std::string>& range)
{
  const auto numbers = parseNumbers(value);
  if (!numbers || numbers->size() < 3)
  {
    return invalidValue(key, value, "a color (three numbers such as '1 0.5 0')");
  }

  const auto rgb = std::vector<double>{numbers->begin(), numbers->begin() + 3};
  if (range == "float" && std::ranges::any_of(rgb, [](const auto c) {
        return c < 0.0 || c > 1.0;
      }))
  {
    return invalidValue(key, value, "a float color (components between 0 and 1)");
  }
  if (range == "byte" && std::ranges::any_of(rgb, [](const auto c) {
        return c < 0.0 || c > 255.0 || c != std::floor(c);
      }))
  {
    return invalidValue(key, value, "a byte color (integer components 0 to 255)");
  }
  return std::nullopt;
}

} // namespace

const mdl::EntityDefinition* findEntityDefinition(
  const mdl::Map& map, const std::string_view classname)
{
  return map.entityDefinitionManager().definition(classname);
}

std::string entityDefinitionTypeName(const mdl::EntityDefinition& definition)
{
  return mdl::getType(definition) == mdl::EntityDefinitionType::Point ? "point" : "brush";
}

std::string propertyTypeName(const mdl::PropertyValueType& valueType)
{
  namespace T = mdl::PropertyValueTypes;
  return std::visit(
    kdl::overload(
      [](const T::LinkTarget&) { return "link_target"; },
      [](const T::LinkSource&) { return "link_source"; },
      [](const T::String&) { return "string"; },
      [](const T::Boolean&) { return "boolean"; },
      [](const T::Integer&) { return "integer"; },
      [](const T::Float&) { return "float"; },
      [](const T::Choice&) { return "choice"; },
      [](const T::Flags&) { return "flags"; },
      [](const T::Origin&) { return "origin"; },
      [](const T::Input&) { return "input"; },
      [](const T::Output&) { return "output"; },
      [](const T::Color<RgbF>&) { return "color"; },
      [](const T::Color<RgbB>&) { return "color"; },
      [](const T::Color<Rgb>&) { return "color"; },
      [](const T::Unknown&) { return "unknown"; }),
    valueType);
}

std::optional<std::string> colorRangeName(const mdl::PropertyValueType& valueType)
{
  namespace T = mdl::PropertyValueTypes;
  if (std::holds_alternative<T::Color<RgbF>>(valueType))
  {
    return "float";
  }
  if (std::holds_alternative<T::Color<RgbB>>(valueType))
  {
    return "byte";
  }
  if (std::holds_alternative<T::Color<Rgb>>(valueType))
  {
    return "any";
  }
  return std::nullopt;
}

Json propertyDefinitionJson(const mdl::PropertyDefinition& definition)
{
  namespace T = mdl::PropertyValueTypes;

  auto result = Json{
    {"key", definition.key},
    {"type", propertyTypeName(definition.valueType)},
    {"description", definition.shortDescription},
  };
  if (!definition.longDescription.empty())
  {
    result["longDescription"] = definition.longDescription;
  }
  if (const auto* floatType = std::get_if<T::Float>(&definition.valueType);
      floatType && floatType->defaultValue)
  {
    // the shortest representation, e.g. "0.5" instead of "0.500000"
    result["default"] = fmt::format("{}", *floatType->defaultValue);
  }
  else if (const auto defaultValue = mdl::PropertyDefinition::defaultValue(definition))
  {
    result["default"] = *defaultValue;
  }
  if (definition.readOnly)
  {
    result["readOnly"] = true;
  }
  if (const auto* choice = std::get_if<T::Choice>(&definition.valueType))
  {
    auto choices = Json::array();
    for (const auto& option : choice->options)
    {
      choices.push_back(
        Json{{"value", option.value}, {"description", option.description}});
    }
    result["choices"] = std::move(choices);
  }
  if (const auto* flags = std::get_if<T::Flags>(&definition.valueType))
  {
    auto flagsJson = Json::array();
    for (const auto& flag : flags->flags)
    {
      auto bit = 0;
      while (bit < 31 && (1 << bit) != flag.value)
      {
        ++bit;
      }
      auto flagJson = Json{
        {"bit", bit},
        {"value", flag.value},
        {"name", flag.shortDescription},
      };
      if (!flag.longDescription.empty())
      {
        flagJson["description"] = flag.longDescription;
      }
      flagJson["default"] = flags->isDefault(flag.value);
      flagsJson.push_back(std::move(flagJson));
    }
    result["flags"] = std::move(flagsJson);
  }
  if (const auto range = colorRangeName(definition.valueType))
  {
    result["colorRange"] = *range;
  }
  if (std::holds_alternative<T::LinkSource>(definition.valueType))
  {
    result["linkRole"] = "source";
  }
  else if (std::holds_alternative<T::LinkTarget>(definition.valueType))
  {
    result["linkRole"] = "target";
  }
  return result;
}

const mdl::PropertyDefinition* findPropertyDefinition(
  const mdl::EntityDefinition* definition, const std::string& key)
{
  if (!definition)
  {
    return nullptr;
  }
  if (const auto* propertyDefinition = mdl::getPropertyDefinition(*definition, key))
  {
    return propertyDefinition;
  }

  // numbered properties such as target2 share the definition of target
  auto prefix = key;
  while (!prefix.empty() && std::isdigit(static_cast<unsigned char>(prefix.back())))
  {
    prefix.pop_back();
  }
  return prefix != key && !prefix.empty()
           ? mdl::getPropertyDefinition(*definition, prefix)
           : nullptr;
}

const mdl::PropertyValueTypes::Flag* findFlag(
  const mdl::PropertyValueTypes::Flags& flags, const std::string_view name)
{
  const auto wanted = lowercase(trim(name));
  for (const auto& flag : flags.flags)
  {
    if (!flag.shortDescription.empty() && lowercase(flag.shortDescription) == wanted)
    {
      return &flag;
    }
  }

  if (wanted.starts_with("bit"))
  {
    if (const auto bit = parseInteger(std::string_view{wanted}.substr(3));
        bit && *bit >= 0 && *bit < 31)
    {
      return flags.flag(1 << *bit);
    }
  }
  if (const auto value = parseInteger(wanted);
      value && *value > 0 && *value < (1LL << 31))
  {
    return flags.flag(int(*value));
  }
  return nullptr;
}

std::vector<std::string> flagNames(const mdl::PropertyValueTypes::Flags& flags)
{
  auto result = std::vector<std::string>{};
  for (const auto& flag : flags.flags)
  {
    result.push_back(
      flag.shortDescription.empty() ? std::to_string(flag.value) : flag.shortDescription);
  }
  return result;
}

std::optional<PropertyProblem> checkPropertyValue(
  const mdl::EntityDefinition* definition,
  const std::string& key,
  const std::string& value)
{
  namespace T = mdl::PropertyValueTypes;

  if (!definition)
  {
    return std::nullopt;
  }

  const auto* propertyDefinition = findPropertyDefinition(definition, key);
  if (!propertyDefinition)
  {
    if (isWellKnownKey(key))
    {
      return std::nullopt;
    }
    return PropertyProblem{
      "UNKNOWN_PROPERTY",
      "Property '" + key + "' is not defined for " + definition->name + ".",
    };
  }

  if (propertyDefinition->readOnly)
  {
    return PropertyProblem{
      "READ_ONLY_PROPERTY",
      "Property '" + key + "' of " + definition->name + " is marked read-only.",
    };
  }

  // an empty value is how the editor clears a value; do not complain about it
  if (value.empty())
  {
    return std::nullopt;
  }

  return std::visit(
    kdl::overload(
      [&](const T::Integer&) -> std::optional<PropertyProblem> {
        return parseInteger(value) ? std::nullopt
                                   : invalidValue(key, value, "an integer");
      },
      [&](const T::Float&) -> std::optional<PropertyProblem> {
        return parseNumber(value) ? std::nullopt : invalidValue(key, value, "a number");
      },
      [&](const T::Boolean&) -> std::optional<PropertyProblem> {
        const auto lower = lowercase(trim(value));
        return lower == "0" || lower == "1" || lower == "true" || lower == "false"
                 ? std::nullopt
                 : invalidValue(key, value, "a boolean (0, 1, true or false)");
      },
      [&](const T::Choice& choice) -> std::optional<PropertyProblem> {
        if (std::ranges::any_of(
              choice.options, [&](const auto& option) { return option.value == value; }))
        {
          return std::nullopt;
        }
        auto options = std::string{};
        for (const auto& option : choice.options)
        {
          options += (options.empty() ? "" : ", ") + option.value;
          if (!option.description.empty())
          {
            options += " (" + option.description + ")";
          }
        }
        return PropertyProblem{
          "INVALID_CHOICE",
          describeValue(key, value) + " is not one of the choices of " + definition->name
            + ": " + options + ".",
        };
      },
      [&](const T::Flags& flags) -> std::optional<PropertyProblem> {
        const auto number = parseInteger(value);
        if (!number || *number < 0)
        {
          return invalidValue(key, value, "a non-negative integer bit mask");
        }
        auto known = 0LL;
        for (const auto& flag : flags.flags)
        {
          known |= flag.value;
        }
        if (const auto unknown = *number & ~known; unknown != 0)
        {
          return PropertyProblem{
            "UNKNOWN_FLAGS",
            describeValue(key, value) + " sets bits that " + definition->name
              + " does not define (" + std::to_string(unknown) + ").",
          };
        }
        return std::nullopt;
      },
      [&](const T::Origin&) -> std::optional<PropertyProblem> {
        const auto numbers = parseNumbers(value);
        return numbers && numbers->size() == 3
                 ? std::nullopt
                 : invalidValue(
                     key, value, "a position (three numbers such as '0 0 24')");
      },
      [&](const T::Color<RgbF>&) { return checkColor(key, value, "float"); },
      [&](const T::Color<RgbB>&) { return checkColor(key, value, "byte"); },
      [&](const T::Color<Rgb>&) { return checkColor(key, value, "any"); },
      [](const auto&) -> std::optional<PropertyProblem> { return std::nullopt; }),
    propertyDefinition->valueType);
}

void warnUnknownClassname(
  CallContext& context,
  const std::string_view classname,
  std::vector<std::string> objectIds)
{
  const auto& map = context.map();
  if (
    !map.entityDefinitionManager().definitions().empty()
    && !findEntityDefinition(map, classname))
  {
    context.warn(
      "UNKNOWN_CLASSNAME",
      "Class '" + std::string{classname}
        + "' is not defined by the current entity definitions; use "
          "entity_classes_list to see the available classes.",
      std::move(objectIds));
  }
}

void validateProperty(
  CallContext& context,
  const mdl::EntityDefinition* definition,
  const std::string& key,
  const std::string& value,
  std::vector<std::string> objectIds)
{
  if (key == mdl::EntityPropertyKeys::Classname)
  {
    warnUnknownClassname(context, value, std::move(objectIds));
    return;
  }

  if (auto problem = checkPropertyValue(definition, key, value))
  {
    context.warn(
      std::move(problem->code), std::move(problem->message), std::move(objectIds));
  }
}

Result<std::vector<mdl::EntityNodeBase*>, ToolError> resolveEntities(
  mdl::Map& map, const IdRegistry& ids, const Args& args, const std::string_view key)
{
  auto result = std::vector<mdl::EntityNodeBase*>{};
  const auto add = [&](mdl::EntityNodeBase* entityNode) {
    if (entityNode && std::ranges::find(result, entityNode) == result.end())
    {
      result.push_back(entityNode);
    }
  };

  if (const auto explicitIds = args.getOptional<std::vector<std::string>>(key))
  {
    for (const auto& id : *explicitIds)
    {
      auto resolved = ids.resolve(id);
      if (resolved.is_error())
      {
        return errorOf(resolved);
      }
      auto* node = resolved.value();

      const auto kind = objectKindOf(*node);
      if (kind == ObjectKind::Layer || kind == ObjectKind::Group)
      {
        return makeError(
          ErrorCode::WrongObjectKind,
          "Object " + ids.format(*node) + " is a " + std::string{toString(kind)}
            + "; this tool expects entities, 'world', or brushes of entities.",
          "Pass entity ids, or 'world' for worldspawn.",
          {ids.format(*node)});
      }

      if (kind != ObjectKind::World)
      {
        // a brush entity is editable if one of its brushes is
        const auto editable =
          map.editorContext().selectable(*node)
          || std::ranges::any_of(node->children(), [&](const auto* child) {
               return map.editorContext().selectable(*child);
             });
        if (!editable)
        {
          return makeError(
            ErrorCode::ObjectNotEditable,
            "Object " + ids.format(*node)
              + " cannot be edited: it is hidden, locked, or inside a closed group.",
            "Show or unlock its layer (layer_set_state), or open its group "
            "(group_open).",
            {ids.format(*node)});
        }
      }

      if (auto* entityNode = dynamic_cast<mdl::EntityNodeBase*>(node))
      {
        add(entityNode);
      }
      else
      {
        add(mdl::findContainingEntity(node));
      }
    }
    return result;
  }

  if (!map.selection().hasNodes())
  {
    return makeError(
      ErrorCode::NoSelection,
      "No " + std::string{key} + " were given and nothing is selected.",
      "Pass '" + std::string{key} + "' explicitly (use 'world' for worldspawn), or "
        "select entities first.");
  }
  for (auto* entityNode : map.selection().allEntities())
  {
    add(entityNode);
  }
  return result;
}

ToolResult withEntities(
  CallContext& context,
  const std::vector<mdl::EntityNodeBase*>& entities,
  const std::function<ToolResult()>& function)
{
  auto& world = context.map().worldNode();

  auto targets = std::vector<mdl::Node*>{};
  auto includesWorld = false;
  for (auto* entityNode : entities)
  {
    if (entityNode == &world)
    {
      includesWorld = true;
    }
    else if (entityNode->hasChildren())
    {
      for (auto* child : entityNode->children())
      {
        targets.push_back(child);
      }
    }
    else
    {
      targets.push_back(entityNode);
    }
  }

  auto result = ToolResult{Json::object()};
  if (includesWorld)
  {
    // with nothing selected, the editor acts on worldspawn
    result = withTargets(context, {}, function);
    if (result.is_error())
    {
      return result;
    }
  }
  if (!targets.empty())
  {
    result = withTargets(context, targets, function);
  }
  return result;
}

std::string formatPropertyNumber(const Json& value)
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

std::optional<std::string> propertyValueFromJson(const Json& value)
{
  if (value.is_string())
  {
    return value.get<std::string>();
  }
  if (value.is_number())
  {
    return formatPropertyNumber(value);
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
      result += (result.empty() ? "" : " ") + formatPropertyNumber(element);
    }
    return result;
  }
  return std::nullopt;
}

bool containsQuote(const std::string_view str)
{
  return str.find('"') != std::string::npos;
}

Result<PropertyList, ToolError> propertiesFromJson(
  const Json& properties, const bool allowOrigin)
{
  auto result = PropertyList{};
  if (!properties.is_object())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The properties must be an object of key-value pairs.",
      "Example: {\"light\": 300, \"_color\": [255, 128, 0]}");
  }

  for (const auto& item : properties.items())
  {
    const auto& key = item.key();
    if (key.empty() || containsQuote(key))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Property key '" + key + "' is invalid: keys must be non-empty and must not "
        "contain quotes, which map files cannot store.");
    }
    if (key == mdl::EntityPropertyKeys::Classname)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Set the classname with the argument 'classname', not in 'properties'.",
        "Remove 'classname' from 'properties'.");
    }
    if (!allowOrigin && key == mdl::EntityPropertyKeys::Origin)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Set the origin of a point entity with the argument 'position', not in "
        "'properties'.",
        "Remove 'origin' from 'properties' and pass 'position': [x, y, z].");
    }

    if (item.value().is_null())
    {
      result.emplace_back(key, std::nullopt);
      continue;
    }

    auto value = propertyValueFromJson(item.value());
    if (!value)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The value of property '" + key + "' must be a string, a number, a boolean, "
        "an array of numbers or null.",
        "Example: {\"light\": 300, \"_color\": [255, 128, 0], \"message\": \"Hello\", "
        "\"gibmodel\": null}");
    }
    if (containsQuote(*value))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The value of property '" + key + "' contains a quote, which map files cannot "
        "store.",
        "Use single quotes instead.");
    }
    result.emplace_back(key, std::move(*value));
  }
  return result;
}


} // namespace tb::mcp
