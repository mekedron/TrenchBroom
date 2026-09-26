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

#include "mcp/Json.h"
#include "mcp/ObjectIds.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp::schema
{

enum class Type
{
  Any,
  Boolean,
  Integer,
  Number,
  String,
  Array,
  Object,
  OneOf,
};

struct SchemaError
{
  /** A JSON pointer-like path, e.g. "/faces/2/material". Empty for the root. */
  std::string path;
  std::string message;
};

class Field;

/**
 * A schema node. It serves both as the JSON Schema that `tools/list` publishes
 * (`toJsonSchema`) and as the validator and decoder of the arguments (`validate`), so
 * there is a single source of truth.
 *
 * Schemas are values; the modifiers return modified copies:
 *
 *   object({
 *     field("min", vec3()).required().describe("Minimum corner"),
 *     field("limit", integer().min(1).max(1000).defaultValue(100)),
 *   })
 */
class Schema
{
public:
  using Check = std::function<std::optional<std::string>(const Json&)>;

  Type type = Type::Any;
  std::string description;
  std::optional<Json> defaultValue;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::optional<size_t> minItems;
  std::optional<size_t> maxItems;
  std::optional<size_t> minLength;
  std::optional<std::string> pattern;
  std::vector<std::string> enumValues;
  std::shared_ptr<const Schema> items;
  std::vector<Field> fields;
  std::vector<Schema> alternatives;
  bool additionalProperties = false;
  /** Published as the JSON Schema "format" (informational), e.g. "object-id". */
  std::string format;
  /** Additional semantic checks. Returns an error message on failure. */
  std::vector<Check> checks;

  Schema describe(std::string description) const;
  Schema defaultsTo(Json value) const;
  Schema min(double minimum) const;
  Schema max(double maximum) const;
  Schema minSize(size_t minItems) const;
  Schema maxSize(size_t maxItems) const;
  Schema nonEmpty() const;
  Schema matching(std::string pattern) const;
  Schema withFormat(std::string format) const;
  Schema withCheck(Check check) const;
  Schema allowAdditionalProperties() const;

  /** Returns the JSON Schema representation. */
  Json toJsonSchema() const;

  /**
   * Validates the given value. On success, returns the value with defaults filled in
   * and integral numbers normalized; otherwise, returns the errors.
   */
  std::optional<Json> validate(
    const Json& value,
    std::vector<SchemaError>& errors,
    const std::string& path = {}) const;

  /** Returns the field with the given name, if this is an object schema. */
  const Field* findField(std::string_view name) const;
};

class Field
{
public:
  std::string name;
  Schema schema;
  bool isRequired = false;

  Field(std::string name, Schema schema);

  Field required() const;
  Field describe(std::string description) const;
  Field defaultsTo(Json value) const;
};

// Primitives
Schema any();
Schema boolean();
Schema integer();
Schema number();
Schema string();
Schema enumOf(std::vector<std::string> values);
Schema array(Schema items);
Schema object(std::vector<Field> fields);
Schema oneOf(std::vector<Schema> alternatives);

Field field(std::string name, Schema schema);

// Domain types
/** `[x, y, z]` in map units. */
Schema vec3();
/** `[x, y]`. */
Schema vec2();
/** `{"min": [x, y, z], "max": [x, y, z]}` with min <= max. */
Schema box();
/** An angle in degrees. */
Schema angle();

/** An object id such as `brush:1042`. With no kinds given, any kind is accepted. */
Schema objectId(std::vector<ObjectKind> kinds = {});
/** A document handle such as `doc:1`. */
Schema documentId();

/** Formats the given errors as one line, e.g. "/min: expected an array". */
std::string formatErrors(const std::vector<SchemaError>& errors);

} // namespace tb::mcp::schema
