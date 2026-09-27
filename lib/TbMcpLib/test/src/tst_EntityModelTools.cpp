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
#include "mcp/McpToolFixture.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mcp
{
namespace
{

using Properties = std::vector<std::pair<std::string, std::string>>;

const auto StandBounds = Json{{"min", {-16, -16, -24}}, {"max", {16, 16, 40}}};
const auto SitBounds = Json{{"min", {-16, -16, -59}}, {"max", {16, 16, 8}}};

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

/** The warnings of the given code. */
std::vector<Json> warningsOf(const Json& result, const std::string& code)
{
  auto warnings = std::vector<Json>{};
  for (const auto& warning : result["warnings"])
  {
    if (warning["code"] == code)
    {
      warnings.push_back(warning);
    }
  }
  return warnings;
}

/** The codes of the model placement warnings. */
std::vector<std::string> placementCodes(const Json& result)
{
  auto codes = std::vector<std::string>{};
  for (const auto& warning : result["warnings"])
  {
    const auto code = warning["code"].get<std::string>();
    if (code.starts_with("MODEL_") || code == "MORE_PLACEMENT_FINDINGS")
    {
      codes.push_back(code);
    }
  }
  return codes;
}

std::vector<std::string> names(const Json& animations)
{
  auto result = std::vector<std::string>{};
  for (const auto& animation : animations)
  {
    result.push_back(animation["name"].get<std::string>());
  }
  return result;
}

} // namespace

TEST_CASE("EntityModelTools")
{
  auto fixture = McpToolFixture{};
  auto& document = createDocument(fixture);
  auto& map = document.map();

  // a floor whose surface is at z = 0 and a chair whose seat is at z = 16
  const auto floorId = createBox(fixture, {-512, -512, -16}, {512, 512, 0});
  const auto chairId = createBox(fixture, {192, -16, 0}, {224, 16, 16});
  mdl::deselectAll(map);

  SECTION("entity_model_info")
  {
    auto* sitting = addPointEntity(
      map, {{"classname", "monster_person"}, {"origin", "0 0 24"}, {"sequence", "1"}});

    SECTION("animations with bounds")
    {
      const auto result =
        fixture.call("entity_model_info", {{"ids", {fixture.id(*sitting)}}});
      REQUIRE(result["entities"].size() == 1);
      const auto& info = result["entities"][0];
      CHECK(
        info["model"] == Json{{"path", "progs/person.mdl"}, {"skin", 0}, {"frame", 1}});
      CHECK(info["modelLoaded"] == true);
      CHECK(info["modelBounds"] == Json{{"min", {-16, -16, -35}}, {"max", {16, 16, 32}}});
      CHECK(info["frameProperty"] == "sequence");
      CHECK(info["currentAnimation"]["index"] == 1);
      CHECK(info["currentAnimation"]["name"] == "sit");
      CHECK(info["currentAnimation"]["bounds"] == SitBounds);
      CHECK(info["currentAnimation"]["worldBounds"] == info["modelBounds"]);
      CHECK(info["animationCount"] == 2);
      CHECK(info["animationsTruncated"] == false);
      REQUIRE(info["animations"].size() == 2);
      CHECK(names(info["animations"]) == std::vector<std::string>{"stand", "sit"});
      CHECK(info["animations"][0]["index"] == 0);
      CHECK(info["animations"][0]["bounds"] == StandBounds);
      CHECK(
        info["animations"][0]["worldBounds"]
        == Json{{"min", {-16, -16, 0}}, {"max", {16, 16, 64}}});
    }

    SECTION("maxAnimations")
    {
      const auto one = fixture.call(
        "entity_model_info", {{"ids", {fixture.id(*sitting)}}, {"maxAnimations", 1}});
      CHECK(one["entities"][0]["animations"].size() == 1);
      CHECK(one["entities"][0]["animationsTruncated"] == true);
      CHECK(one["entities"][0]["animationCount"] == 2);

      const auto none = fixture.call(
        "entity_model_info", {{"ids", {fixture.id(*sitting)}}, {"maxAnimations", 0}});
      CHECK(none["entities"][0]["animations"].empty());
      CHECK(none["entities"][0]["currentAnimation"]["name"] == "sit");

      CHECK(
        fixture
          .callExpectingError(
            "entity_model_info", {{"ids", {fixture.id(*sitting)}}, {"maxAnimations", -1}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("other models")
    {
      const auto* cube = addPointEntity(map, {{"classname", "studio_cube"}});
      const auto* seated = addPointEntity(map, {{"classname", "monster_person_seated"}});
      const auto* missing = addPointEntity(map, {{"classname", "monster_missing"}});
      const auto* marker = addPointEntity(map, {{"classname", "info_marker"}});

      const auto result = fixture.call(
        "entity_model_info",
        {{"ids",
          {fixture.id(*cube),
           fixture.id(*seated),
           fixture.id(*missing),
           fixture.id(*marker)}}});
      const auto& entities = result["entities"];
      REQUIRE(entities.size() == 4);

      // a Half-Life studio model: its sequences are the animations
      CHECK(entities[0]["modelLoaded"] == true);
      CHECK(
        names(entities[0]["animations"])
        == std::vector<std::string>{"idle", "slide", "spin"});
      CHECK(entities[0]["currentAnimation"]["name"] == "idle");

      // a fixed frame
      CHECK(entities[1]["frameProperty"].is_null());
      CHECK(entities[1]["currentAnimation"]["name"] == "sit");

      CHECK(entities[2]["modelLoaded"] == false);
      CHECK(entities[2]["modelBounds"].is_null());
      CHECK(entities[2]["frameProperty"] == "sequence");
      CHECK(entities[2].contains("modelLoadError"));
      CHECK(entities[2]["animations"].empty());

      CHECK(entities[3]["model"].is_null());
      CHECK(entities[3]["frameProperty"].is_null());
      CHECK(entities[3]["animationCount"].is_null());
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {sitting});
      const auto result = fixture.call("entity_model_info");
      CHECK(result["entities"][0]["currentAnimation"]["name"] == "sit");
    }
  }

  SECTION("entity_animation_set")
  {
    // standing on the floor
    auto* person = addPointEntity(
      map, {{"classname", "monster_person"}, {"origin", "0 0 24"}, {"sequence", "0"}});
    const auto personId = fixture.id(*person);

    SECTION("by name")
    {
      const auto result =
        fixture.call("entity_animation_set", {{"ids", {personId}}, {"animation", "SIT"}});
      CHECK(result["undoStep"] == "AI: Set Entity Animation");
      const auto& items = result["result"]["entities"];
      REQUIRE(items.size() == 1);
      CHECK(items[0]["id"] == personId);
      CHECK(items[0]["classname"] == "monster_person");
      CHECK(items[0]["property"] == "sequence");
      CHECK(items[0]["value"] == "1");
      CHECK(items[0]["previousValue"] == "0");
      CHECK(items[0]["animation"] == Json{{"index", 1}, {"name", "sit"}});
      CHECK(
        items[0]["modelBounds"] == Json{{"min", {-16, -16, -35}}, {"max", {16, 16, 32}}});
      CHECK(*person->entity().property("sequence") == "1");

      // the sitting model reaches below the floor
      const auto below = warningsOf(result, "MODEL_BELOW_FLOOR");
      REQUIRE(below.size() == 1);
      CHECK_THAT(
        below[0]["message"].get<std::string>(),
        Catch::Matchers::ContainsSubstring(
          "reaches 35 units below the floor at z=0 (" + floorId + ")"));
      CHECK(below[0]["objectIds"] == Json{personId, floorId});

      // one undo step
      fixture.call("undo");
      CHECK(*person->entity().property("sequence") == "0");
    }

    SECTION("by index")
    {
      fixture.call("entity_animation_set", {{"ids", {personId}}, {"animation", 1}});
      const auto result =
        fixture.call("entity_animation_set", {{"ids", {personId}}, {"animation", 0}});
      CHECK(result["result"]["entities"][0]["animation"]["name"] == "stand");
      CHECK(result["result"]["entities"][0]["previousValue"] == "1");
      CHECK(placementCodes(result).empty());
    }

    SECTION("selection and several entities")
    {
      auto* other =
        addPointEntity(map, {{"classname", "monster_person"}, {"origin", "64 0 59"}});
      mdl::selectNodes(map, {person, other});

      const auto result = fixture.call("entity_animation_set", {{"animation", "sit"}});
      const auto& items = result["result"]["entities"];
      REQUIRE(items.size() == 2);
      const auto otherItem = std::ranges::find_if(
        items, [&](const auto& item) { return item["id"] == fixture.id(*other); });
      REQUIRE(otherItem != items.end());
      CHECK((*otherItem)["previousValue"].is_null());
      CHECK(*person->entity().property("sequence") == "1");
      CHECK(*other->entity().property("sequence") == "1");

      // the second one sits on the floor
      CHECK(placementCodes(result) == std::vector<std::string>{"MODEL_BELOW_FLOOR"});

      // the selection is kept
      CHECK(map.selection().nodes.size() == 2);

      // one undo step for both
      fixture.call("undo");
      CHECK(*person->entity().property("sequence") == "0");
      CHECK(other->entity().property("sequence") == nullptr);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "entity_animation_set",
        {{"ids", {personId}}, {"animation", "sit"}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["entities"][0]["value"] == "1");
      CHECK(*person->entity().property("sequence") == "0");
    }

    SECTION("errors")
    {
      const auto unknown = fixture.callExpectingError(
        "entity_animation_set", {{"ids", {personId}}, {"animation", "lie"}});
      CHECK(unknown.code == ErrorCode::InvalidArgument);
      CHECK_THAT(unknown.hint, Catch::Matchers::ContainsSubstring("stand, sit"));

      CHECK(
        fixture
          .callExpectingError(
            "entity_animation_set", {{"ids", {personId}}, {"animation", 2}})
          .code
        == ErrorCode::InvalidArgument);

      const auto* seated = addPointEntity(map, {{"classname", "monster_person_seated"}});
      const auto fixed = fixture.callExpectingError(
        "entity_animation_set", {{"ids", {fixture.id(*seated)}}, {"animation", "stand"}});
      CHECK(fixed.code == ErrorCode::InvalidArgument);
      CHECK_THAT(fixed.message, Catch::Matchers::ContainsSubstring("same frame"));

      const auto* marker = addPointEntity(map, {{"classname", "info_marker"}});
      CHECK(
        fixture
          .callExpectingError(
            "entity_animation_set", {{"ids", {fixture.id(*marker)}}, {"animation", 0}})
          .code
        == ErrorCode::InvalidArgument);

      const auto* missing = addPointEntity(map, {{"classname", "monster_missing"}});
      CHECK(
        fixture
          .callExpectingError(
            "entity_animation_set", {{"ids", {fixture.id(*missing)}}, {"animation", 0}})
          .code
        == ErrorCode::OperationFailed);

      // one invalid entity changes nothing
      CHECK(
        fixture
          .callExpectingError(
            "entity_animation_set",
            {{"ids", {personId, fixture.id(*marker)}}, {"animation", "sit"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(*person->entity().property("sequence") == "0");

      CHECK(
        fixture.callExpectingError("entity_animation_set", {{"animation", "sit"}}).code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "entity_animation_set", {{"ids", {floorId}}, {"animation", "sit"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("entity_animation_set", {{"ids", {personId}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "entity_animation_set", {{"ids", {personId}}, {"animation", ""}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("entity_create_point")
  {
    SECTION("dropToFloor with the model of the current animation")
    {
      const auto result = fixture.call(
        "entity_create_point",
        {{"classname", "monster_person"},
         {"position", {0, 0, 100}},
         {"properties", {{"sequence", 1}}},
         {"dropToFloor", true}});
      const auto& created = result["result"];
      CHECK(created["origin"] == Json{0, 0, 59});
      CHECK(created["floor"]["bounds"] == "model");
      CHECK(created["floor"]["object"] == floorId);
      CHECK(placementCodes(result).empty());
    }

    SECTION("dropToFloor with the definition bounds")
    {
      const auto result = fixture.call(
        "entity_create_point",
        {{"classname", "monster_person"},
         {"position", {0, 0, 100}},
         {"properties", {{"sequence", 1}}},
         {"dropToFloor", true},
         {"dropUsing", "definition"}});
      const auto& created = result["result"];
      CHECK(created["origin"] == Json{0, 0, 24});
      CHECK(created["floor"]["bounds"] == "definition");

      // the sitting model reaches below the floor
      CHECK(placementCodes(result) == std::vector<std::string>{"MODEL_BELOW_FLOOR"});
      const auto below = warningsOf(result, "MODEL_BELOW_FLOOR");
      CHECK_THAT(
        below[0]["message"].get<std::string>(),
        Catch::Matchers::ContainsSubstring("reaches 35 units below the floor at z=0"));
    }

    SECTION("dropToFloor using the model fails without model")
    {
      CHECK(
        fixture
          .callExpectingError(
            "entity_create_point",
            {{"classname", "info_marker"},
             {"position", {0, 0, 100}},
             {"dropToFloor", true},
             {"dropUsing", "model"}})
          .code
        == ErrorCode::InvalidArgument);

      // auto falls back to the definition bounds
      const auto result = fixture.call(
        "entity_create_point",
        {{"classname", "monster_missing"},
         {"position", {0, 0, 100}},
         {"dropToFloor", true}});
      CHECK(result["result"]["origin"] == Json{0, 0, 24});
      CHECK(result["result"]["floor"]["bounds"] == "definition");
    }

    SECTION("placement warnings")
    {
      // standing on the floor: no warnings
      CHECK(placementCodes(fixture.call(
                             "entity_create_point",
                             {{"classname", "monster_person"},
                              {"position", {0, 0, 24}},
                              {"snapToGrid", false}}))
              .empty());

      // floating
      const auto floating = fixture.call(
        "entity_create_point",
        {{"classname", "monster_person"}, {"position", {64, 0, 64}}});
      CHECK(placementCodes(floating) == std::vector<std::string>{"MODEL_FLOATING"});
      CHECK(
        warningsOf(floating, "MODEL_FLOATING")[0]["message"].get<std::string>().find(
          "floats 40 units above the floor")
        != std::string::npos);

      // standing partly in the chair
      const auto inChair = fixture.call(
        "entity_create_point",
        {{"classname", "monster_person"},
         {"position", {192, 0, 24}},
         {"snapToGrid", false}});
      CHECK(
        placementCodes(inChair) == std::vector<std::string>{"MODEL_PENETRATES_BRUSHES"});
      CHECK(
        warningsOf(inChair, "MODEL_PENETRATES_BRUSHES")[0]["objectIds"][1] == chairId);

      // entities without model are not checked
      CHECK(placementCodes(fixture.call(
                             "entity_create_point",
                             {{"classname", "info_marker"}, {"position", {-64, 0, 64}}}))
              .empty());
    }

    SECTION("invalid dropUsing")
    {
      CHECK(
        fixture
          .callExpectingError(
            "entity_create_point",
            {{"classname", "monster_person"},
             {"position", {0, 0, 24}},
             {"dropToFloor", true},
             {"dropUsing", "bounds"}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_move")
  {
    auto* person = addPointEntity(
      map, {{"classname", "monster_person"}, {"origin", "0 0 24"}, {"sequence", "1"}});
    const auto personId = fixture.id(*person);

    // moving the sitting model up so that its feet are on the floor
    const auto up =
      fixture.call("objects_move", {{"ids", {personId}}, {"vector", {0, 0, 35}}});
    CHECK(placementCodes(up).empty());

    // too far
    const auto floating =
      fixture.call("objects_move", {{"ids", {personId}}, {"vector", {0, 0, 16}}});
    CHECK(placementCodes(floating) == std::vector<std::string>{"MODEL_FLOATING"});

    // moving it by the selection, back into the floor
    mdl::selectNodes(map, {person});
    const auto down = fixture.call("objects_move", {{"vector", {0, 0, -51}}});
    CHECK(placementCodes(down) == std::vector<std::string>{"MODEL_BELOW_FLOOR"});

    // a dry run warns as well
    const auto dryRun = fixture.call(
      "objects_move", {{"ids", {personId}}, {"vector", {0, 0, 35}}, {"dryRun", true}});
    CHECK(placementCodes(dryRun).empty());
    CHECK(person->entity().origin() == vm::vec3d{0, 0, 24});

    // brushes are not checked
    CHECK(placementCodes(
            fixture.call("objects_move", {{"ids", {chairId}}, {"vector", {0, 0, 64}}}))
            .empty());
  }

  SECTION("entity_placement_check")
  {
    auto* sitting = addPointEntity(
      map, {{"classname", "monster_person"}, {"origin", "0 0 24"}, {"sequence", "1"}});
    auto* standing =
      addPointEntity(map, {{"classname", "monster_person"}, {"origin", "64 0 24"}});
    auto* missing = addPointEntity(map, {{"classname", "monster_missing"}});
    auto* marker = addPointEntity(map, {{"classname", "info_marker"}});
    const auto sittingId = fixture.id(*sitting);
    const auto standingId = fixture.id(*standing);

    SECTION("ids")
    {
      const auto result = fixture.call(
        "entity_placement_check",
        {{"ids", {sittingId, standingId, fixture.id(*missing), fixture.id(*marker)}}});
      REQUIRE(result["items"].size() == 4);
      CHECK(result["total"] == 4);
      CHECK(
        result["summary"]
        == Json{{"entities", 4}, {"checked", 2}, {"notChecked", 2}, {"withFindings", 1}});

      const auto& sittingItem = result["items"][0];
      CHECK(sittingItem["id"] == sittingId);
      CHECK(sittingItem["model"] == "progs/person.mdl");
      CHECK(sittingItem["checked"] == true);
      CHECK(sittingItem["animation"] == Json{{"index", 1}, {"name", "sit"}});
      CHECK(
        sittingItem["modelBounds"]
        == Json{{"min", {-16, -16, -35}}, {"max", {16, 16, 32}}});
      CHECK(sittingItem["surface"]["z"] == 0);
      CHECK(sittingItem["surface"]["object"] == floorId);
      REQUIRE(sittingItem["findings"].size() == 1);
      const auto& finding = sittingItem["findings"][0];
      CHECK(finding["code"] == "MODEL_BELOW_FLOOR");
      CHECK(finding["distance"] == 35);
      CHECK(finding["suggestedMove"] == Json{0, 0, 35});
      CHECK(finding["objectIds"] == Json{sittingId, floorId});

      CHECK(result["items"][1]["findings"].empty());
      CHECK(result["items"][1]["checked"] == true);

      CHECK(result["items"][2]["checked"] == false);
      CHECK(result["items"][2].contains("reason"));
      CHECK(result["items"][3]["checked"] == false);
      CHECK(result["items"][3]["model"].is_null());
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {standing});
      const auto result = fixture.call("entity_placement_check");
      REQUIRE(result["items"].size() == 1);
      CHECK(result["items"][0]["id"] == standingId);
    }

    SECTION("map")
    {
      const auto all = fixture.call("entity_placement_check", {{"scope", "map"}});
      // entities with models, including the one whose model is missing
      CHECK(all["total"] == 3);
      CHECK(all["summary"]["withFindings"] == 1);

      const auto problems = fixture.call(
        "entity_placement_check", {{"scope", "map"}, {"onlyProblems", true}});
      REQUIRE(problems["items"].size() == 1);
      CHECK(problems["items"][0]["id"] == sittingId);
      CHECK(problems["summary"]["entities"] == 3);

      const auto page =
        fixture.call("entity_placement_check", {{"scope", "map"}, {"limit", 1}});
      CHECK(page["items"].size() == 1);
      CHECK(page["nextCursor"].is_string());
    }

    SECTION("errors")
    {
      CHECK(
        fixture.callExpectingError("entity_placement_check").code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "entity_placement_check", {{"ids", {sittingId}}, {"scope", "map"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("entity_placement_check", {{"scope", "world"}}).code
        == ErrorCode::InvalidArgument);
    }
  }
}

} // namespace tb::mcp
