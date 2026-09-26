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

#include "mcp/Schema.h"

#include <cmath>
#include <regex>
#include <sstream>

namespace tb::mcp::schema
{
namespace
{

std::string typeName(const Json& value)
{
  if (value.is_null())
  {
    return "null";
  }
  if (value.is_boolean())
  {
    return "a boolean";
  }
  if (value.is_number())
  {
    return "a number";
  }
  if (value.is_string())
  {
    return "a string";
  }
  if (value.is_array())
  {
    return "an array";
  }
  return "an object";
}

std::string expectedName(const Type type)
{
  switch (type)
  {
  case Type::Any:
    return "any value";
  case Type::Boolean:
    return "a boolean";
  case Type::Integer:
    return "an integer";
  case Type::Number:
    return "a number";
  case Type::String:
    return "a string";
  case Type::Array:
    return "an array";
  case Type::Object:
    return "an object";
  case Type::OneOf:
    return "one of the alternatives";
  }
  return "";
}

std::string formatNumber(const double value)
{
  auto str = std::ostringstream{};
  str << value;
  return str.str();
}

void addError(
  std::vector<SchemaError>& errors, const std::string& path, std::string message)
{
  errors.push_back(SchemaError{path, std::move(message)});
}

bool checkRange(
  const Schema& schema,
  const double value,
  std::vector<SchemaError>& errors,
  const std::string& path)
{
  if (schema.minimum && value < *schema.minimum)
  {
    addError(errors, path, "must be >= " + formatNumber(*schema.minimum));
    return false;
  }
  if (schema.maximum && value > *schema.maximum)
  {
    addError(errors, path, "must be <= " + formatNumber(*schema.maximum));
    return false;
  }
  return true;
}

} // namespace

Schema Schema::describe(std::string description_) const
{
  auto result = *this;
  result.description = std::move(description_);
  return result;
}

Schema Schema::defaultsTo(Json value) const
{
  auto result = *this;
  result.defaultValue = std::move(value);
  return result;
}

Schema Schema::min(const double minimum_) const
{
  auto result = *this;
  result.minimum = minimum_;
  return result;
}

Schema Schema::max(const double maximum_) const
{
  auto result = *this;
  result.maximum = maximum_;
  return result;
}

Schema Schema::minSize(const size_t minItems_) const
{
  auto result = *this;
  if (type == Type::String)
  {
    result.minLength = minItems_;
  }
  else
  {
    result.minItems = minItems_;
  }
  return result;
}

Schema Schema::maxSize(const size_t maxItems_) const
{
  auto result = *this;
  result.maxItems = maxItems_;
  return result;
}

Schema Schema::nonEmpty() const
{
  return minSize(1);
}

Schema Schema::matching(std::string pattern_) const
{
  auto result = *this;
  result.pattern = std::move(pattern_);
  return result;
}

Schema Schema::withFormat(std::string format_) const
{
  auto result = *this;
  result.format = std::move(format_);
  return result;
}

Schema Schema::withCheck(Check check) const
{
  auto result = *this;
  result.checks.push_back(std::move(check));
  return result;
}

Schema Schema::allowAdditionalProperties() const
{
  auto result = *this;
  result.additionalProperties = true;
  return result;
}

Json Schema::toJsonSchema() const
{
  auto result = Json::object();
  switch (type)
  {
  case Type::Any:
    break;
  case Type::Boolean:
    result["type"] = "boolean";
    break;
  case Type::Integer:
    result["type"] = "integer";
    break;
  case Type::Number:
    result["type"] = "number";
    break;
  case Type::String:
    result["type"] = "string";
    break;
  case Type::Array:
    result["type"] = "array";
    break;
  case Type::Object:
    result["type"] = "object";
    break;
  case Type::OneOf: {
    auto alternativesJson = Json::array();
    for (const auto& alternative : alternatives)
    {
      alternativesJson.push_back(alternative.toJsonSchema());
    }
    result["oneOf"] = std::move(alternativesJson);
    break;
  }
  }

  if (!description.empty())
  {
    result["description"] = description;
  }
  if (!format.empty())
  {
    result["format"] = format;
  }
  if (!enumValues.empty())
  {
    result["enum"] = enumValues;
  }
  if (pattern)
  {
    result["pattern"] = *pattern;
  }
  if (minLength)
  {
    result["minLength"] = *minLength;
  }
  if (minimum)
  {
    result["minimum"] = type == Type::Integer ? Json(int64_t(*minimum)) : Json(*minimum);
  }
  if (maximum)
  {
    result["maximum"] = type == Type::Integer ? Json(int64_t(*maximum)) : Json(*maximum);
  }
  if (items)
  {
    result["items"] = items->toJsonSchema();
  }
  if (minItems)
  {
    result["minItems"] = *minItems;
  }
  if (maxItems)
  {
    result["maxItems"] = *maxItems;
  }
  if (type == Type::Object)
  {
    auto properties = Json::object();
    auto required = Json::array();
    for (const auto& f : fields)
    {
      properties[f.name] = f.schema.toJsonSchema();
      if (f.isRequired)
      {
        required.push_back(f.name);
      }
    }
    result["properties"] = std::move(properties);
    if (!required.empty())
    {
      result["required"] = std::move(required);
    }
    if (!additionalProperties)
    {
      result["additionalProperties"] = false;
    }
  }
  if (defaultValue)
  {
    result["default"] = *defaultValue;
  }
  return result;
}

std::optional<Json> Schema::validate(
  const Json& value, std::vector<SchemaError>& errors, const std::string& path) const
{
  const auto errorCount = errors.size();
  auto result = std::optional<Json>{};

  switch (type)
  {
  case Type::Any:
    result = value;
    break;
  case Type::Boolean:
    if (!value.is_boolean())
    {
      addError(errors, path, "expected a boolean, got " + typeName(value));
      return std::nullopt;
    }
    result = value;
    break;
  case Type::Integer: {
    if (!value.is_number())
    {
      addError(errors, path, "expected an integer, got " + typeName(value));
      return std::nullopt;
    }
    const auto d = value.get<double>();
    if (!value.is_number_integer() && !value.is_number_unsigned() && std::floor(d) != d)
    {
      addError(errors, path, "expected an integer, got " + formatNumber(d));
      return std::nullopt;
    }
    if (!checkRange(*this, d, errors, path))
    {
      return std::nullopt;
    }
    result = value.is_number_float() ? Json(int64_t(d)) : value;
    break;
  }
  case Type::Number: {
    if (!value.is_number())
    {
      addError(errors, path, "expected a number, got " + typeName(value));
      return std::nullopt;
    }
    if (!checkRange(*this, value.get<double>(), errors, path))
    {
      return std::nullopt;
    }
    result = value;
    break;
  }
  case Type::String: {
    if (!value.is_string())
    {
      addError(errors, path, "expected a string, got " + typeName(value));
      return std::nullopt;
    }
    const auto& str = value.get_ref<const std::string&>();
    if (!enumValues.empty() && std::ranges::find(enumValues, str) == enumValues.end())
    {
      auto allowed = std::string{};
      for (const auto& enumValue : enumValues)
      {
        allowed += (allowed.empty() ? "" : ", ") + ("'" + enumValue + "'");
      }
      addError(errors, path, "must be one of " + allowed + ", got '" + str + "'");
      return std::nullopt;
    }
    if (minLength && str.size() < *minLength)
    {
      addError(
        errors,
        path,
        *minLength == 1
          ? "must not be empty"
          : "must have at least " + std::to_string(*minLength) + " characters");
      return std::nullopt;
    }
    if (pattern && !std::regex_match(str, std::regex{*pattern}))
    {
      addError(errors, path, "'" + str + "' does not match the pattern " + *pattern);
      return std::nullopt;
    }
    result = value;
    break;
  }
  case Type::Array: {
    if (!value.is_array())
    {
      addError(errors, path, "expected an array, got " + typeName(value));
      return std::nullopt;
    }
    if (minItems && value.size() < *minItems)
    {
      addError(
        errors, path, "must have at least " + std::to_string(*minItems) + " elements");
      return std::nullopt;
    }
    if (maxItems && value.size() > *maxItems)
    {
      addError(
        errors, path, "must have at most " + std::to_string(*maxItems) + " elements");
      return std::nullopt;
    }
    auto array = Json::array();
    for (size_t i = 0; i < value.size(); ++i)
    {
      auto element = items
                       ? items->validate(value[i], errors, path + "/" + std::to_string(i))
                       : std::optional{value[i]};
      if (element)
      {
        array.push_back(std::move(*element));
      }
    }
    result = std::move(array);
    break;
  }
  case Type::Object: {
    if (!value.is_object())
    {
      addError(errors, path, "expected an object, got " + typeName(value));
      return std::nullopt;
    }
    auto object = Json::object();
    for (const auto& f : fields)
    {
      const auto* member = findMember(value, f.name);
      if (!member || member->is_null())
      {
        if (f.isRequired)
        {
          addError(errors, path + "/" + f.name, "is required");
        }
        else if (f.schema.defaultValue)
        {
          object[f.name] = *f.schema.defaultValue;
        }
        continue;
      }
      if (auto validated = f.schema.validate(*member, errors, path + "/" + f.name))
      {
        object[f.name] = std::move(*validated);
      }
    }
    for (const auto& [key, member] : value.items())
    {
      if (!findField(key))
      {
        if (additionalProperties)
        {
          object[key] = member;
        }
        else
        {
          auto allowed = std::string{};
          for (const auto& f : fields)
          {
            allowed += (allowed.empty() ? "" : ", ") + f.name;
          }
          addError(
            errors,
            path + "/" + key,
            "unknown property"
              + (allowed.empty() ? std::string{} : "; allowed properties: " + allowed));
        }
      }
    }
    result = std::move(object);
    break;
  }
  case Type::OneOf: {
    for (const auto& alternative : alternatives)
    {
      auto alternativeErrors = std::vector<SchemaError>{};
      if (auto validated = alternative.validate(value, alternativeErrors, path);
          validated && alternativeErrors.empty())
      {
        result = std::move(validated);
        break;
      }
    }
    if (!result)
    {
      auto expected = std::string{};
      for (const auto& alternative : alternatives)
      {
        const auto name = !alternative.description.empty()
                            ? alternative.description
                            : expectedName(alternative.type);
        expected += (expected.empty() ? "" : " or ") + name;
      }
      addError(errors, path, "expected " + expected + ", got " + typeName(value));
      return std::nullopt;
    }
    break;
  }
  }

  if (errors.size() > errorCount)
  {
    return std::nullopt;
  }

  for (const auto& check : checks)
  {
    if (const auto error = check(*result))
    {
      addError(errors, path, *error);
      return std::nullopt;
    }
  }

  return result;
}

const Field* Schema::findField(const std::string_view name) const
{
  const auto it =
    std::ranges::find_if(fields, [&](const auto& f) { return f.name == name; });
  return it != fields.end() ? &*it : nullptr;
}

Field::Field(std::string name_, Schema schema_)
  : name{std::move(name_)}
  , schema{std::move(schema_)}
{
}

Field Field::required() const
{
  auto result = *this;
  result.isRequired = true;
  return result;
}

Field Field::describe(std::string description) const
{
  auto result = *this;
  result.schema.description = std::move(description);
  return result;
}

Field Field::defaultsTo(Json value) const
{
  auto result = *this;
  result.schema.defaultValue = std::move(value);
  return result;
}

Schema any()
{
  return Schema{};
}

Schema boolean()
{
  auto result = Schema{};
  result.type = Type::Boolean;
  return result;
}

Schema integer()
{
  auto result = Schema{};
  result.type = Type::Integer;
  return result;
}

Schema number()
{
  auto result = Schema{};
  result.type = Type::Number;
  return result;
}

Schema string()
{
  auto result = Schema{};
  result.type = Type::String;
  return result;
}

Schema enumOf(std::vector<std::string> values)
{
  auto result = string();
  result.enumValues = std::move(values);
  return result;
}

Schema array(Schema items)
{
  auto result = Schema{};
  result.type = Type::Array;
  result.items = std::make_shared<const Schema>(std::move(items));
  return result;
}

Schema object(std::vector<Field> fields)
{
  auto result = Schema{};
  result.type = Type::Object;
  result.fields = std::move(fields);
  return result;
}

Schema oneOf(std::vector<Schema> alternatives)
{
  auto result = Schema{};
  result.type = Type::OneOf;
  result.alternatives = std::move(alternatives);
  return result;
}

Field field(std::string name, Schema schema)
{
  return Field{std::move(name), std::move(schema)};
}

Schema vec3()
{
  return array(number()).minSize(3).maxSize(3).describe("[x, y, z]");
}

Schema vec2()
{
  return array(number()).minSize(2).maxSize(2).describe("[x, y]");
}

Schema box()
{
  return object({
                  field("min", vec3()).required(),
                  field("max", vec3()).required(),
                })
    .withCheck([](const Json& value) -> std::optional<std::string> {
      for (size_t i = 0; i < 3; ++i)
      {
        if (value["min"][i].get<double>() > value["max"][i].get<double>())
        {
          return "min must not be greater than max in any component";
        }
      }
      return std::nullopt;
    });
}

Schema angle()
{
  return number().describe("Angle in degrees");
}

Schema objectId(std::vector<ObjectKind> kinds)
{
  auto description = std::string{"Object id"};
  if (!kinds.empty())
  {
    description += " (";
    for (size_t i = 0; i < kinds.size(); ++i)
    {
      description += (i > 0 ? ", " : "") + std::string{toString(kinds[i])};
    }
    description += ")";
  }
  description += ", e.g. 'brush:1042'";

  return string()
    .describe(std::move(description))
    .withFormat("object-id")
    .withCheck(
      [kinds = std::move(kinds)](const Json& value) -> std::optional<std::string> {
        const auto& str = value.get_ref<const std::string&>();
        const auto ref = parseObjectRef(str);
        if (!ref)
        {
          return "'" + str
               + "' is not a valid object id; ids look like 'brush:1042', "
                 "'layer:default' or 'world'";
        }
        if (!kinds.empty() && std::ranges::find(kinds, ref->kind) == kinds.end())
        {
          auto expected = std::string{};
          for (const auto kind : kinds)
          {
            expected += (expected.empty() ? "" : " or ") + std::string{toString(kind)};
          }
          return "'" + str + "' is a " + std::string{toString(ref->kind)}
                 + " id, expected " + expected;
        }
        return std::nullopt;
      });
}

Schema documentId()
{
  return string()
    .matching("^doc:[1-9][0-9]*$")
    .describe("Document handle from document_list, e.g. 'doc:1'");
}

std::string formatErrors(const std::vector<SchemaError>& errors)
{
  auto result = std::string{};
  for (const auto& error : errors)
  {
    if (!result.empty())
    {
      result += "; ";
    }
    result +=
      (error.path.empty() ? std::string{"arguments"} : error.path) + ": " + error.message;
  }
  return result;
}

} // namespace tb::mcp::schema
