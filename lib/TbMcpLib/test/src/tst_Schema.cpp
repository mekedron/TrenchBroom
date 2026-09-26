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

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp::schema
{
namespace
{

std::optional<Json> validate(const Schema& schema, const Json& value)
{
  auto errors = std::vector<SchemaError>{};
  return schema.validate(value, errors);
}

std::vector<SchemaError> errorsOf(const Schema& schema, const Json& value)
{
  auto errors = std::vector<SchemaError>{};
  schema.validate(value, errors);
  return errors;
}

} // namespace

TEST_CASE("Schema")
{
  SECTION("toJsonSchema")
  {
    const auto schema = object({
      field("min", vec3()).required().describe("Minimum corner"),
      field("limit", integer().min(1).max(10).defaultsTo(5)),
      field("mode", enumOf({"a", "b"})),
      field("id", objectId({ObjectKind::Brush})),
    });

    CHECK(schema.toJsonSchema() == Json::parse(R"({
        "type": "object",
        "properties": {
          "min": {"type": "array", "description": "Minimum corner",
                  "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
          "limit": {"type": "integer", "minimum": 1, "maximum": 10, "default": 5},
          "mode": {"type": "string", "enum": ["a", "b"]},
          "id": {"type": "string", "description": "Object id (brush), e.g. 'brush:1042'",
                 "format": "object-id"}
        },
        "required": ["min"],
        "additionalProperties": false
      })"));

    CHECK(
      oneOf({string(), integer()}).toJsonSchema()
      == Json::parse(R"({"oneOf": [{"type": "string"}, {"type": "integer"}]})"));
  }

  SECTION("validate")
  {
    SECTION("fills in defaults")
    {
      const auto schema = object({field("limit", integer().defaultsTo(5))});
      CHECK(validate(schema, Json::object()) == Json{{"limit", 5}});
      CHECK(validate(schema, Json{{"limit", 3}}) == Json{{"limit", 3}});
    }

    SECTION("reports missing required fields with paths")
    {
      const auto schema = object({field("a", object({field("b", string()).required()}))});
      const auto errors = errorsOf(schema, Json{{"a", Json::object()}});
      REQUIRE(errors.size() == 1);
      CHECK(errors[0].path == "/a/b");
      CHECK(errors[0].message == "is required");
    }

    SECTION("rejects unknown properties")
    {
      const auto errors = errorsOf(object({field("a", string())}), Json{{"b", 1}});
      REQUIRE(errors.size() == 1);
      CHECK(errors[0].path == "/b");
      CHECK(errors[0].message == "unknown property; allowed properties: a");
    }

    SECTION("checks types")
    {
      CHECK(validate(boolean(), true));
      CHECK(!validate(boolean(), 1));
      CHECK(!validate(string(), 1));
      CHECK(!validate(number(), "1"));
      CHECK(!validate(array(number()), Json::object()));
      CHECK(!validate(object({}), Json::array()));
    }

    SECTION("integers")
    {
      CHECK(validate(integer(), 3.0) == Json(3));
      CHECK(!validate(integer(), 3.5));
      CHECK(!validate(integer().min(1), 0));
      CHECK(!validate(integer().max(1), 2));
    }

    SECTION("strings")
    {
      CHECK(!validate(enumOf({"a"}), "b"));
      CHECK(!validate(string().nonEmpty(), ""));
      CHECK(validate(documentId(), "doc:1"));
      CHECK(!validate(documentId(), "doc:x"));
    }

    SECTION("arrays")
    {
      CHECK(validate(vec3(), Json::array({1, 2, 3})));
      CHECK(!validate(vec3(), Json::array({1, 2})));
      CHECK(!validate(vec3(), Json::array({1, 2, "x"})));

      const auto errors = errorsOf(array(integer()), Json::array({1, "x"}));
      REQUIRE(errors.size() == 1);
      CHECK(errors[0].path == "/1");
    }

    SECTION("box")
    {
      CHECK(validate(box(), Json::parse(R"({"min":[0,0,0],"max":[1,1,1]})")));
      CHECK(!validate(box(), Json::parse(R"({"min":[2,0,0],"max":[1,1,1]})")));
    }

    SECTION("object ids")
    {
      CHECK(validate(objectId(), "brush:12"));
      CHECK(validate(objectId(), "world"));
      CHECK(!validate(objectId(), "brush"));
      CHECK(!validate(objectId({ObjectKind::Entity}), "brush:12"));
    }

    SECTION("oneOf")
    {
      const auto schema = oneOf({integer(), string()});
      CHECK(validate(schema, 1));
      CHECK(validate(schema, "a"));
      CHECK(!validate(schema, true));
    }
  }

  SECTION("formatErrors")
  {
    CHECK(
      formatErrors({{"/a", "is required"}, {"", "expected an object, got null"}})
      == "/a: is required; arguments: expected an object, got null");
  }
}

} // namespace tb::mcp::schema
