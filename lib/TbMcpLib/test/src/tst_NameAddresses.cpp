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

#include "mcp/McpToolFixture.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

std::string createBox(McpToolFixture& fixture, const double x)
{
  return resultOf(fixture.call(
    "brush_create_box", Json{{"min", {x, 0, 0}}, {"max", {x + 64, 64, 64}}}))["brush"]
    .get<std::string>();
}

std::string createGroup(McpToolFixture& fixture, const std::string& name, const double x)
{
  const auto brush = createBox(fixture, x);
  return resultOf(
           fixture.call("group_create", Json{{"ids", {brush}}, {"name", name}}))["group"]
    .get<std::string>();
}

} // namespace

TEST_CASE("NameAddresses")
{
  auto fixture = McpToolFixture{};
  fixture.create();

  SECTION("layers, groups and entities are addressed by name wherever ids are")
  {
    const auto group = createGroup(fixture, "Bar", 0);
    const auto layer =
      resultOf(fixture.call("layer_create", Json{{"name", "Details"}}))["layer"]["id"]
        .get<std::string>();
    const auto entity = resultOf(fixture.call(
      "entity_create_point",
      Json{
        {"classname", "info_null"},
        {"position", {0, 0, 128}},
        {"properties", {{"targetname", "door1"}}}}))["entity"]
                          .get<std::string>();

    // object lists report the address next to the id
    const auto objects = fixture.call(
      "object_get", Json{{"ids", {"group:@Bar", "entity:@door1", "layer:@Details"}}});
    CHECK(objects["objects"][0]["id"] == group);
    CHECK(objects["objects"][0]["address"] == "group:@Bar");
    CHECK(objects["objects"][1]["id"] == entity);
    CHECK(objects["objects"][1]["address"] == "entity:@door1");
    CHECK(objects["objects"][2]["id"] == layer);

    const auto layers = fixture.call("layers_list");
    CHECK(layers["layers"][1]["address"] == "layer:@Details");

    // modifying tools; the change report uses ids
    const auto moved =
      fixture.call("objects_move", Json{{"ids", {"group:@Bar"}}, {"vector", {0, 0, 16}}});
    CHECK(moved["changes"]["modified"].dump().find(group) != std::string::npos);
    fixture.call("layer_set_state", Json{{"layer", "layer:@Details"}, {"locked", true}});
    CHECK(fixture.call("layers_list")["layers"][1]["locked"] == true);

    // names resolve when the call runs
    fixture.call("group_rename", Json{{"ids", {group}}, {"name", "Counter"}});
    CHECK(
      fixture.callExpectingError("object_get", Json{{"ids", {"group:@Bar"}}}).code
      == ErrorCode::ObjectNotFound);
    CHECK(
      fixture.call("object_get", Json{{"ids", {"group:@Counter"}}})["objects"][0]["id"]
      == group);
  }

  SECTION("an ambiguous name fails with the candidates")
  {
    const auto first = createGroup(fixture, "Stool", 0);
    const auto second = createGroup(fixture, "Stool", 128);

    const auto error = fixture.callExpectingError(
      "objects_move", Json{{"ids", {"group:@Stool"}}, {"vector", {0, 0, 16}}});
    CHECK(error.code == ErrorCode::AmbiguousName);
    CHECK(error.objectIds == std::vector<std::string>{first, second});
    REQUIRE(error.details["candidates"].size() == 2);
    CHECK(error.details["candidates"][0]["id"] == first);
    CHECK(error.details["candidates"][0]["name"] == "Stool");
    CHECK(error.details["candidates"][0].contains("bounds"));
    CHECK(fixture.call("history_get")["undoCount"] == 4);
  }

  SECTION("invalid name addresses")
  {
    createGroup(fixture, "Bar", 0);

    // the kind is checked by the schema
    CHECK(
      fixture
        .callExpectingError(
          "layer_set_state", Json{{"layer", "group:@Bar"}, {"hidden", true}})
        .code
      == ErrorCode::InvalidArgument);
    // brushes have no names
    CHECK(
      fixture.callExpectingError("object_get", Json{{"ids", {"brush:@x"}}}).code
      == ErrorCode::InvalidArgument);

    const auto missing =
      fixture.callExpectingError("object_get", Json{{"ids", {"group:@Table"}}});
    CHECK(missing.code == ErrorCode::ObjectNotFound);
    CHECK(missing.hint.find("'Bar'") != std::string::npos);
  }
}

} // namespace tb::mcp
