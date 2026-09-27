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

#include "TestEnvironment.h"
#include "el/Expression.h"
#include "el/ParseExpression.h"
#include "el/Value.h"
#include "el/VariableStore.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ObjectIds.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/ModelDefinition.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/bbox_io.h"
#include "vm/vec_io.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

using Properties = std::vector<std::pair<std::string, std::string>>;

mdl::ModelDefinition makeModelDefinition(const std::string& expression)
{
  return mdl::ModelDefinition{
    el::parseExpression(el::ParseMode::Strict, expression).value()};
}

el::VariableTable variables(std::map<std::string, std::string> values)
{
  auto table = std::map<std::string, el::Value>{};
  for (auto& [key, value] : values)
  {
    table.emplace(key, el::Value{value});
  }
  return el::VariableTable{std::move(table)};
}

/** A Quake document with the model fixture definitions (test/mcp/models.fgd). */
ui::MapDocument& createDocument(McpToolFixture& fixture)
{
  auto& document =
    fixture.create({.mapFormat = mdl::MapFormat::Valve, .gameInfo = mdl::QuakeGameInfo});
  fixture.call(
    "entity_definitions_set",
    {{"type", "external"},
     {"path", (getFixtureRoot() / "test" / "mcp" / "models.fgd").string()}});
  return document;
}

mdl::EntityNode* addPointEntity(mdl::Map& map, Properties properties)
{
  auto entityProperties = std::vector<mdl::EntityProperty>{};
  for (auto& [key, value] : properties)
  {
    entityProperties.emplace_back(std::move(key), std::move(value));
  }
  auto* entityNode = new mdl::EntityNode{mdl::Entity{std::move(entityProperties)}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  return entityNode;
}

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return fixture.call("brush_create_box", {{"min", min}, {"max", max}})["result"]["brush"]
    .get<std::string>();
}

std::vector<std::string> codes(const PlacementCheck& check)
{
  auto result = std::vector<std::string>{};
  for (const auto& finding : check.findings)
  {
    result.push_back(finding.code);
  }
  return result;
}

const vm::bbox3d StandBounds = {{-16, -16, -24}, {16, 16, 40}};
const vm::bbox3d SitBounds = {{-16, -16, -59}, {16, 16, 8}};

} // namespace

TEST_CASE("EntityModelUtils")
{
  SECTION("findFrameProperty")
  {
    const auto noVariables = variables({});

    // the frame is taken from a property, as in the Half-Life FGD
    CHECK(
      findFrameProperty(
        makeModelDefinition(
          R"({ "path": "models/player.mdl", "frame": sequence, "skin": skin })"),
        noVariables)
      == "sequence");
    CHECK(
      findFrameProperty(
        makeModelDefinition(R"({ "path": "models/player.mdl", "frame": sequence })"),
        variables({{"sequence", "13"}}))
      == "sequence");
    CHECK(
      findFrameProperty(
        makeModelDefinition(R"({ "path": "progs/a.mdl", "frame": frame })"), noVariables)
      == "frame");

    // fixed frames
    CHECK(
      findFrameProperty(makeModelDefinition(R"("progs/ogre.mdl")"), noVariables)
      == std::nullopt);
    CHECK(
      findFrameProperty(
        makeModelDefinition(R"({ "path": "progs/flame2.mdl", "frame": 1 })"), noVariables)
      == std::nullopt);

    // a spawnflag that picks another frame is not a frame property (Quake's zombie)
    CHECK(
      findFrameProperty(
        makeModelDefinition(R"({{
          spawnflags & 1 -> { "path": "progs/zombie.mdl", "frame": 192 },
                            "progs/zombie.mdl"
        }})"),
        noVariables)
      == std::nullopt);

    // a variable that also changes the model is not a frame property
    CHECK(
      findFrameProperty(
        makeModelDefinition(R"({{
          body == 1 -> { "path": "progs/b.mdl", "frame": body },
                       { "path": "progs/a.mdl", "frame": body }
        }})"),
        noVariables)
      == std::nullopt);

    // no model at all
    CHECK(findFrameProperty(mdl::ModelDefinition{}, noVariables) == std::nullopt);
  }

  SECTION("framePropertyValue")
  {
    const auto noVariables = variables({});
    CHECK(
      framePropertyValue(
        makeModelDefinition(R"({ "path": "progs/a.mdl", "frame": sequence })"),
        noVariables,
        "sequence",
        7)
      == "7");

    // an offset is found by searching the values
    CHECK(
      framePropertyValue(
        makeModelDefinition(R"({ "path": "progs/a.mdl", "frame": sequence + 2 })"),
        noVariables,
        "sequence",
        5)
      == "3");

    CHECK(
      framePropertyValue(
        makeModelDefinition(R"({ "path": "progs/a.mdl", "frame": 1 })"),
        noVariables,
        "sequence",
        5)
      == std::nullopt);
  }

  auto fixture = McpToolFixture{};
  auto& document = createDocument(fixture);
  auto& map = document.map();
  auto ids = IdRegistry{document};

  SECTION("EntityModelLoader")
  {
    auto loader = EntityModelLoader{map};

    const auto person = loader.load("progs/person.mdl");
    REQUIRE(person.is_success());
    const auto* data = person.value()->data();
    REQUIRE(data);
    REQUIRE(data->frameCount() == 2);
    CHECK(data->frames()[0].name() == "stand");
    CHECK(vm::bbox3d(data->frames()[0].bounds()) == StandBounds);
    CHECK(data->frames()[1].name() == "sit");
    CHECK(vm::bbox3d(data->frames()[1].bounds()) == SitBounds);

    // loaded once
    CHECK(loader.load("progs/person.mdl").value() == person.value());

    // a Half-Life studio model: one frame per sequence
    const auto cube = loader.load("models/cube.mdl");
    REQUIRE(cube.is_success());
    CHECK(
      animationNames(*cube.value()->data(), 10)
      == std::vector<std::string>{"idle", "slide", "spin"});

    CHECK(loader.load("progs/missing.mdl").is_error());
    CHECK(loader.load("progs/missing.mdl").is_error());
  }

  SECTION("resolveEntityModel")
  {
    auto loader = EntityModelLoader{map};

    SECTION("origin and animation")
    {
      const auto* entityNode = addPointEntity(
        map,
        {{"classname", "monster_person"}, {"origin", "64 32 16"}, {"sequence", "1"}});
      const auto state = resolveEntityModel(entityNode->entity(), loader);
      REQUIRE(state.is_success());
      CHECK(state.value().specification.path == "progs/person.mdl");
      CHECK(state.value().specification.frameIndex == 1);
      REQUIRE(state.value().frame());
      CHECK(state.value().frame()->name() == "sit");
      CHECK(state.value().worldBounds() == SitBounds.translate({64, 32, 16}));
    }

    SECTION("rotation")
    {
      // the model's x axis points to +y after a yaw of 90 degrees
      const auto* entityNode = addPointEntity(
        map, {{"classname", "monster_person"}, {"angle", "90"}, {"sequence", "0"}});
      const auto state = resolveEntityModel(entityNode->entity(), loader);
      REQUIRE(state.is_success());
      const auto bounds = *state.value().worldBounds();
      CHECK(vm::is_equal(bounds.min, vm::vec3d{-16, -16, -24}, 0.001));
      CHECK(vm::is_equal(bounds.max, vm::vec3d{16, 16, 40}, 0.001));
    }

    SECTION("scale")
    {
      const auto* entityNode = addPointEntity(map, {{"classname", "monster_giant"}});
      const auto state = resolveEntityModel(entityNode->entity(), loader);
      REQUIRE(state.is_success());
      CHECK(state.value().worldBounds() == vm::bbox3d{{-32, -32, -48}, {32, 32, 80}});

      const auto animations = modelAnimations(state.value());
      REQUIRE(animations.size() == 2);
      CHECK(animations[1].name == "sit");
      CHECK(animations[1].bounds == SitBounds);
      CHECK(animations[1].worldBounds == vm::bbox3d{{-32, -32, -118}, {32, 32, 16}});
      CHECK(modelAnimations(state.value(), 1).size() == 1);
    }

    SECTION("frame out of range")
    {
      const auto* entityNode =
        addPointEntity(map, {{"classname", "monster_person"}, {"sequence", "5"}});
      const auto state = resolveEntityModel(entityNode->entity(), loader);
      REQUIRE(state.is_success());
      CHECK(state.value().frame() == nullptr);
      CHECK(state.value().worldBounds() == std::nullopt);
    }

    SECTION("errors")
    {
      CHECK(resolveEntityModel(
              addPointEntity(map, {{"classname", "info_marker"}})->entity(), loader)
              .is_error());
      CHECK(resolveEntityModel(
              addPointEntity(map, {{"classname", "no_such_class"}})->entity(), loader)
              .is_error());
      CHECK(resolveEntityModel(
              addPointEntity(map, {{"classname", "monster_missing"}})->entity(), loader)
              .is_error());
    }
  }

  SECTION("findFrameProperty of entities")
  {
    CHECK(
      findFrameProperty(addPointEntity(map, {{"classname", "monster_person"}})->entity())
      == "sequence");
    CHECK(
      findFrameProperty(
        addPointEntity(map, {{"classname", "monster_person_seated"}})->entity())
      == std::nullopt);
    CHECK(
      findFrameProperty(addPointEntity(map, {{"classname", "info_marker"}})->entity())
      == std::nullopt);
  }

  SECTION("findAnimation and animationNames")
  {
    auto loader = EntityModelLoader{map};
    const auto& data = *loader.load("progs/person.mdl").value()->data();

    CHECK(findAnimation(data, Json("sit")) == 1);
    CHECK(findAnimation(data, Json("SIT")) == 1);
    CHECK(findAnimation(data, Json("stand")) == 0);
    CHECK(findAnimation(data, Json(1)) == 1);
    CHECK(findAnimation(data, Json("1")) == 1);
    CHECK(findAnimation(data, Json("lie")) == std::nullopt);
    CHECK(findAnimation(data, Json(2)) == std::nullopt);
    CHECK(findAnimation(data, Json(-1)) == std::nullopt);
    CHECK(findAnimation(data, Json("7")) == std::nullopt);

    CHECK(animationNames(data, 10) == std::vector<std::string>{"stand", "sit"});
    CHECK(animationNames(data, 1) == std::vector<std::string>{"stand"});
  }

  SECTION("checkModelPlacement")
  {
    // a floor whose surface is at z = 0 and a chair seat at z = 16
    const auto floorId = createBox(fixture, {-256, -256, -16}, {256, 256, 0});
    const auto chairId = createBox(fixture, {64, -16, 0}, {96, 16, 16});
    const auto entityId = std::string{"entity:1"};

    const auto check = [&](const vm::bbox3d& bounds) {
      return checkModelPlacement(bounds, map, ids, entityId, "The model");
    };

    SECTION("standing on the floor")
    {
      const auto result = check({{-16, -16, 0}, {16, 16, 64}});
      CHECK(result.findings.empty());
      REQUIRE(result.surface);
      CHECK(result.surface->z == 0);
      CHECK(ids.format(*result.surface->node) == floorId);
      CHECK(placementSurfaceJson(result.surface, ids)["object"] == floorId);

      // within the tolerance
      CHECK(check({{-16, -16, -0.5}, {16, 16, 64}}).findings.empty());
      CHECK(check({{-16, -16, 0.5}, {16, 16, 64}}).findings.empty());
    }

    SECTION("below the floor")
    {
      const auto result = check({{-16, -16, -35}, {16, 16, 29}});
      REQUIRE(codes(result) == std::vector<std::string>{"MODEL_BELOW_FLOOR"});
      const auto& finding = result.findings.front();
      CHECK(finding.distance == 35);
      CHECK(finding.suggestedMove == vm::vec3d{0, 0, 35});
      CHECK(finding.objectIds == std::vector<std::string>{entityId, floorId});
      CHECK(finding.message.starts_with(
        "The model reaches 35 units below the floor at z=0 (" + floorId + ")."));

      const auto json = placementFindingJson(finding);
      CHECK(json["code"] == "MODEL_BELOW_FLOOR");
      CHECK(json["distance"] == 35);
      CHECK(json["suggestedMove"] == Json{0, 0, 35});
    }

    SECTION("floating")
    {
      const auto result = check({{-16, -16, 10}, {16, 16, 74}});
      REQUIRE(codes(result) == std::vector<std::string>{"MODEL_FLOATING"});
      CHECK(result.findings.front().distance == 10);
      CHECK(result.findings.front().suggestedMove == vm::vec3d{0, 0, -10});
    }

    SECTION("no floor")
    {
      const auto result = check({{1000, 1000, 0}, {1032, 1032, 64}});
      CHECK(codes(result) == std::vector<std::string>{"MODEL_NO_FLOOR"});
      CHECK(result.surface == std::nullopt);
    }

    SECTION("penetrating a chair")
    {
      // standing on the floor, partly in the chair
      const auto result = check({{48, -16, 0}, {80, 16, 64}});
      REQUIRE(codes(result) == std::vector<std::string>{"MODEL_PENETRATES_BRUSHES"});
      CHECK(
        result.findings.front().objectIds == std::vector<std::string>{entityId, chairId});
      CHECK(ids.format(*result.surface->node) == floorId);
      CHECK(check({{32, -40, 0}, {72, -8, 64}}).findings.size() == 1);

      // touching the chair or overlapping it by up to the tolerance is fine
      CHECK(check({{32, -16, 0}, {64, 16, 64}}).findings.empty());
      CHECK(check({{33, -16, 0}, {65, 16, 64}}).findings.empty());
    }

    SECTION("sitting on the chair")
    {
      // the feet rest on the floor below the seat
      const auto result = check({{64, -16, -1}, {96, 16, 60}});
      REQUIRE(codes(result) == std::vector<std::string>{"MODEL_PENETRATES_BRUSHES"});
      CHECK(
        result.findings.front().objectIds == std::vector<std::string>{entityId, chairId});
      CHECK(ids.format(*result.surface->node) == floorId);

      // feet that reach into the floor: nothing supports the model, and the seat is the
      // highest surface below it
      const auto deeper = check({{64, -16, -5}, {96, 16, 60}});
      CHECK(
        codes(deeper)
        == std::vector<std::string>{"MODEL_BELOW_FLOOR", "MODEL_PENETRATES_BRUSHES"});
      CHECK(deeper.findings.front().distance == 21);
      CHECK(
        deeper.findings.front().objectIds == std::vector<std::string>{entityId, chairId});
      CHECK(
        deeper.findings.back().objectIds == std::vector<std::string>{entityId, floorId});

      // sitting on the seat
      CHECK(check({{64, -16, 16}, {96, 16, 60}}).findings.empty());
    }

    SECTION("triggers are ignored")
    {
      const auto triggerBrush = createBox(fixture, {-16, -16, 0}, {16, 16, 128});
      fixture.call(
        "entity_create_brush",
        {{"classname", "trigger_multiple"}, {"ids", {triggerBrush}}});
      CHECK(check({{-16, -16, 0}, {16, 16, 64}}).findings.empty());
    }

    SECTION("brush entities count")
    {
      const auto wallBrush = createBox(fixture, {-16, -16, 0}, {16, 16, 128});
      fixture.call(
        "entity_create_brush", {{"classname", "func_wall"}, {"ids", {wallBrush}}});
      const auto result = check({{-16, -16, 0}, {16, 16, 64}});
      // the rays start inside the wall, so they find the floor below it
      CHECK(codes(result) == std::vector<std::string>{"MODEL_PENETRATES_BRUSHES"});
    }
  }

  SECTION("dropToFloorBounds")
  {
    auto loader = EntityModelLoader{map};
    const auto& definitions = map.entityDefinitionManager();
    const auto* person = definitions.definition("monster_person");
    REQUIRE(person);

    const auto sitting = dropToFloorBounds(
      person, "monster_person", {{"sequence", "1"}}, false, "auto", loader);
    REQUIRE(sitting.is_success());
    CHECK(sitting.value().model);
    CHECK(sitting.value().bounds == SitBounds);

    const auto standing =
      dropToFloorBounds(person, "monster_person", {}, true, "model", loader);
    REQUIRE(standing.is_success());
    CHECK(standing.value().model);
    CHECK(standing.value().bounds == StandBounds);

    const auto definitionBounds = dropToFloorBounds(
      person, "monster_person", {{"sequence", "1"}}, false, "definition", loader);
    REQUIRE(definitionBounds.is_success());
    CHECK_FALSE(definitionBounds.value().model);
    CHECK(definitionBounds.value().bounds == StandBounds);

    // without a loadable model
    const auto* marker = definitions.definition("info_marker");
    const auto markerBounds =
      dropToFloorBounds(marker, "info_marker", {}, false, "auto", loader);
    REQUIRE(markerBounds.is_success());
    CHECK_FALSE(markerBounds.value().model);
    CHECK(markerBounds.value().bounds == vm::bbox3d{8.0});
    CHECK(
      errorOf(dropToFloorBounds(marker, "info_marker", {}, false, "model", loader)).code
      == ErrorCode::InvalidArgument);

    const auto* missing = definitions.definition("monster_missing");
    const auto missingBounds =
      dropToFloorBounds(missing, "monster_missing", {}, false, "auto", loader);
    REQUIRE(missingBounds.is_success());
    CHECK_FALSE(missingBounds.value().model);
    CHECK(dropToFloorBounds(missing, "monster_missing", {}, false, "model", loader)
            .is_error());

    const auto unknownBounds =
      dropToFloorBounds(nullptr, "no_such_class", {}, false, "auto", loader);
    REQUIRE(unknownBounds.is_success());
    CHECK(unknownBounds.value().bounds == mdl::EntityNode::DefaultBounds);
  }
}

} // namespace tb::mcp
