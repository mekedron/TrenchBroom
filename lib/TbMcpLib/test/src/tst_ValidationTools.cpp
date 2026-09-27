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
#include "fs/TestEnvironment.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_World.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/vec.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::string createBox(McpToolFixture& fixture, const Json& min, const Json& max)
{
  return fixture
    .call("brush_create_box", Json{{"min", min}, {"max", max}})["result"]["brush"]
    .get<std::string>();
}

std::vector<Json> itemsWith(const Json& result, const std::string& code)
{
  auto items = std::vector<Json>{};
  for (const auto& item : result["items"])
  {
    if (item["code"] == code)
    {
      items.push_back(item);
    }
  }
  return items;
}

std::vector<Json> introduced(const Json& result, const std::string& code)
{
  auto items = std::vector<Json>{};
  for (const auto& item : result["issuesIntroduced"])
  {
    if (item["code"] == code)
    {
      items.push_back(item);
    }
  }
  return items;
}

bool contains(const Json& list, const std::string& value)
{
  return std::ranges::find(list, Json(value)) != list.end();
}

/**
 * A copy of issues.map with the entity classes of models.fgd. validators_set writes the
 * map manifest next to the map, so the fixture itself is not loaded.
 */
ui::MapDocument& loadIssuesMap(McpToolFixture& fixture)
{
  static auto env = fs::TestEnvironment{};
  const auto path = env.dir() / "issues.map";
  std::filesystem::copy_file(
    getFixtureRoot() / "test" / "mcp" / "maps" / "issues.map",
    path,
    std::filesystem::copy_options::overwrite_existing);
  std::filesystem::remove(env.dir() / "issues.mcp.json");
  auto& document = fixture.load(
    path, {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  fixture.call(
    "entity_definitions_set",
    {{"type", "external"},
     {"path", (getFixtureRoot() / "test" / "mcp" / "models.fgd").string()}});
  return document;
}

size_t countOf(McpToolFixture& fixture, const std::string& code)
{
  const auto result = fixture.call("issues_list", Json{{"codes", {code}}});
  return result["total"].get<size_t>();
}

/** Fixes all issues of the code and checks that they are gone. */
Json fixCode(McpToolFixture& fixture, const std::string& code, Json args = Json::object())
{
  const auto before = countOf(fixture, code);
  REQUIRE(before > 0);
  args["codes"] = Json{code};
  auto result = fixture.call("issue_fix", args);
  CHECK(result["result"]["fixedCount"] == before);
  CHECK(result["result"]["notFixedCount"] == 0);
  CHECK(result["undoStep"] == "AI: Fix Issues");
  CHECK(countOf(fixture, code) == 0);
  return result;
}

void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

std::vector<std::string> updatedUris(const CapturingNotificationStream& stream)
{
  auto result = std::vector<std::string>{};
  for (const auto& notification : stream.notifications)
  {
    if (notification["method"] == "notifications/resources/updated")
    {
      result.push_back(notification["params"]["uri"].get<std::string>());
    }
  }
  return result;
}

} // namespace

TEST_CASE("ValidationTools")
{
  auto fixture = McpToolFixture{};

  SECTION("issues_list")
  {
    auto& document = fixture.create();
    auto& map = document.map();

    // an editor issue: an entity without classname
    auto* entityNode = new mdl::EntityNode{mdl::Entity{}};
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
    const auto entityId = fixture.id(*entityNode);

    // an MCP issue: z-fighting
    const auto slab = createBox(fixture, {0, 0, 0}, {128, 128, 16});
    const auto inset = createBox(fixture, {32, 32, 8}, {96, 96, 16});
    const auto other = createBox(fixture, {512, 0, 0}, {576, 64, 64});

    SECTION("lists editor and MCP issues")
    {
      const auto result = fixture.call("issues_list");

      const auto missing = itemsWith(result, "MISSING_ENTITY_CLASSNAME");
      REQUIRE(missing.size() == 1);
      CHECK(missing[0]["source"] == "editor");
      CHECK(missing[0]["type"] == "Missing entity classname");
      CHECK(missing[0]["objectId"] == entityId);
      CHECK(missing[0]["id"].get<std::string>().starts_with("issue:"));
      CHECK(missing[0]["hidden"] == false);
      CHECK(missing[0]["lineNumber"].is_null());
      CHECK(missing[0]["fixes"] == Json{"Delete Objects"});

      const auto zFighting = itemsWith(result, "Z_FIGHTING");
      REQUIRE(zFighting.size() == 1);
      CHECK(zFighting[0]["source"] == "mcp");
      CHECK(zFighting[0]["type"] == "Z-fighting");
      CHECK(zFighting[0]["id"].get<std::string>().starts_with("mcp:Z_FIGHTING|"));
      CHECK(zFighting[0]["fixes"] == Json::array());
      CHECK(zFighting[0]["details"]["area"] == 4096);
      const auto faces = zFighting[0]["details"]["brushes"];
      CHECK((faces == Json{slab, inset} || faces == Json{inset, slab}));

      CHECK(result["counts"]["Z_FIGHTING"] == 1);
      CHECK(result["counts"]["MISSING_ENTITY_CLASSNAME"] == 1);
      CHECK(result["total"] == result["items"].size());
      CHECK(result.contains("leakCheck"));
      CHECK_FALSE(result.contains("disabledValidators"));
    }

    SECTION("filters by source, code and object")
    {
      auto result = fixture.call("issues_list", Json{{"sources", {"editor"}}});
      CHECK(itemsWith(result, "Z_FIGHTING").empty());
      CHECK(!itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      CHECK_FALSE(result.contains("leakCheck"));

      result = fixture.call("issues_list", Json{{"sources", {"mcp"}}});
      CHECK(itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      CHECK(itemsWith(result, "Z_FIGHTING").size() == 1);

      // codes and editor type names, case-insensitive
      result = fixture.call("issues_list", Json{{"codes", {"z_fighting"}}});
      CHECK(result["total"] == 1);
      CHECK_FALSE(result.contains("leakCheck"));
      result = fixture.call("issues_list", Json{{"codes", {"missing entity classname"}}});
      CHECK(result["total"] == 1);
      CHECK(result["items"][0]["objectId"] == entityId);

      result = fixture.call("issues_list", Json{{"ids", {other}}});
      CHECK(result["total"] == 0);
      result = fixture.call("issues_list", Json{{"ids", {inset}}});
      CHECK(itemsWith(result, "Z_FIGHTING").size() == 1);
      CHECK(itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      result = fixture.call("issues_list", Json{{"ids", {entityId}}});
      CHECK(!itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());
      CHECK(itemsWith(result, "Z_FIGHTING").empty());
    }

    SECTION("hidden issues")
    {
      const auto validators = map.worldNode().registeredValidators();
      for (const auto* issue : entityNode->issues(validators))
      {
        map.setIssueHidden(*issue, true);
      }

      auto result = fixture.call("issues_list", Json{{"sources", {"editor"}}});
      CHECK(itemsWith(result, "MISSING_ENTITY_CLASSNAME").empty());

      result = fixture.call(
        "issues_list", Json{{"sources", {"editor"}}, {"includeHidden", true}});
      const auto hidden = itemsWith(result, "MISSING_ENTITY_CLASSNAME");
      REQUIRE(hidden.size() == 1);
      CHECK(hidden[0]["hidden"] == true);
    }

    SECTION("pagination")
    {
      const auto all = fixture.call("issues_list");
      REQUIRE(all["total"].get<size_t>() >= 2);

      const auto first = fixture.call("issues_list", Json{{"limit", 1}});
      CHECK(first["items"].size() == 1);
      CHECK(first["total"] == all["total"]);
      REQUIRE(first["nextCursor"].is_string());

      const auto second =
        fixture.call("issues_list", Json{{"limit", 1}, {"cursor", first["nextCursor"]}});
      CHECK(second["items"].size() == 1);
      CHECK(second["items"][0] == all["items"][1]);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("issues_list", Json{{"ids", {"brush:999999999"}}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("issues_list", Json{{"sources", {"compiler"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("issues_list", Json{{"cursor", "garbage"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("issue_fix")
  {
    SECTION("the editor's quick fixes on issues.map")
    {
      auto& document = loadIssuesMap(fixture);
      auto& map = document.map();

      SECTION("Delete Objects")
      {
        const auto listed =
          fixture.call("issues_list", Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}});
        REQUIRE(listed["total"] == 1);
        const auto entity = listed["items"][0]["objectId"].get<std::string>();

        const auto result = fixCode(fixture, "MISSING_ENTITY_CLASSNAME");
        CHECK(
          result["result"]["applied"]
          == Json{
            {{"fix", "Delete Objects"},
             {"code", "MISSING_ENTITY_CLASSNAME"},
             {"count", 1}}});
        CHECK(contains(result["changes"]["removed"], entity));

        fixCode(fixture, "EMPTY_BRUSH_ENTITY");
      }

      SECTION("Delete Property, also for several issues of one object")
      {
        const auto listed =
          fixture.call("issues_list", Json{{"codes", {"EMPTY_PROPERTY_VALUE"}}});
        REQUIRE(listed["total"] == 2);
        const auto entity = listed["items"][0]["objectId"].get<std::string>();
        CHECK(listed["items"][1]["objectId"] == entity);

        const auto result = fixCode(fixture, "EMPTY_PROPERTY_VALUE");
        CHECK(contains(result["changes"]["modified"], entity));
        const auto* entityNode = dynamic_cast<mdl::EntityNode*>(fixture.node(entity));
        REQUIRE(entityNode);
        CHECK(entityNode->entity().property("note") == nullptr);
        CHECK(entityNode->entity().property("comment") == nullptr);

        fixCode(fixture, "MISSING_ENTITY_LINK_TARGET");
      }

      SECTION("Replace \\ with /")
      {
        fixCode(fixture, "PATHS_MUST_USE_FORWARD_SLASHES");
        CHECK(*map.worldNode().entity().property("wad") == "gfx/base.wad");
      }

      SECTION("Snap Vertices")
      {
        const auto listed =
          fixture.call("issues_list", Json{{"codes", {"NON_INTEGER_VERTICES"}}});
        REQUIRE(listed["total"] == 1);
        const auto brush = listed["items"][0]["objectId"].get<std::string>();

        fixCode(fixture, "NON_INTEGER_VERTICES");
        const auto* brushNode = dynamic_cast<mdl::BrushNode*>(fixture.node(brush));
        REQUIRE(brushNode);
        CHECK(std::ranges::all_of(brushNode->brush().vertices(), [](const auto* vertex) {
          return vm::is_integral(vertex->position());
        }));
      }

      SECTION("Reset UV Scale")
      {
        const auto listed =
          fixture.call("issues_list", Json{{"codes", {"INVALID_UV_SCALE"}}});
        REQUIRE(listed["total"].get<size_t>() > 0);
        CHECK(listed["items"][0].contains("face"));

        fixCode(fixture, "INVALID_UV_SCALE");
      }

      SECTION("Move Brushes to World")
      {
        const auto listed =
          fixture.call("issues_list", Json{{"codes", {"POINT_ENTITY_WITH_BRUSHES"}}});
        REQUIRE(listed["total"] == 1);
        const auto* entityNode =
          fixture.node(listed["items"][0]["objectId"].get<std::string>());
        REQUIRE(entityNode->childCount() == 1);
        const auto* brushNode = entityNode->children().front();

        fixCode(fixture, "POINT_ENTITY_WITH_BRUSHES");
        CHECK(entityNode->childCount() == 0);
        CHECK(brushNode->parent() == &mdl::parentForNodes(map));
      }

      SECTION("Remove Mod")
      {
        mdl::setEnabledMods(map, {"missing_mod"});
        fixCode(fixture, "MISSING_MOD_DIRECTORY");
        CHECK(mdl::enabledMods(map).empty());
      }

      SECTION("Replace \" with ', Truncate Property Values and several fixes")
      {
        auto* quoted = new mdl::EntityNode{mdl::Entity{{
          {"classname", "info_marker"},
          {"origin", "0 0 64"},
          {"message", "say \"hi\""},
        }}};
        auto* longValue = new mdl::EntityNode{mdl::Entity{{
          {"classname", "info_marker"},
          {"origin", "0 32 64"},
          {"message", std::string(2000, 'x')},
        }}};
        auto* emptyGroup = new mdl::GroupNode{mdl::Group{"empty"}};
        mdl::addNodes(
          map, {{&mdl::parentForNodes(map), {quoted, longValue, emptyGroup}}});

        // two fixes: the call must choose one
        auto result =
          fixture.call("issue_fix", Json{{"codes", {"INVALID_ENTITY_PROPERTY_VALUES"}}});
        CHECK(result["result"]["fixedCount"] == 0);
        REQUIRE(result["result"]["notFixed"].size() == 1);
        CHECK(
          result["result"]["notFixed"][0]["reason"].get<std::string>().find(
            "several fixes")
          != std::string::npos);
        CHECK(result["warnings"][0]["code"] == "NOTHING_FIXED");
        CHECK(result["undoStep"].is_null());

        fixCode(
          fixture, "INVALID_ENTITY_PROPERTY_VALUES", Json{{"fix", "replace \" with '"}});
        CHECK(*quoted->entity().property("message") == "say 'hi'");

        // the editor truncates to the maximum length, which its validator still
        // reports: the issue is listed as not fixed
        result = fixture.call(
          "issue_fix",
          Json{
            {"codes", {"LONG_ENTITY_PROPERTY_VALUE"}},
            {"fix", "Truncate Property Values"}});
        CHECK(result["result"]["fixedCount"] == 0);
        REQUIRE(result["result"]["notFixed"].size() == 1);
        CHECK(
          result["result"]["notFixed"][0]["reason"]
          == "The issue is still present after 'Truncate Property Values'.");
        CHECK(longValue->entity().property("message")->size() == 1023);
        fixCode(fixture, "LONG_ENTITY_PROPERTY_VALUE", Json{{"fix", "delete property"}});
        CHECK(longValue->entity().property("message") == nullptr);

        // a fix of another issue type does not apply
        result = fixture.call(
          "issue_fix", Json{{"codes", {"EMPTY_GROUP"}}, {"fix", "Delete Property"}});
        CHECK(result["result"]["fixedCount"] == 0);
        CHECK(
          result["result"]["notFixed"][0]["reason"].get<std::string>().find(
            "does not apply")
          != std::string::npos);

        fixCode(fixture, "EMPTY_GROUP");
        CHECK(emptyGroup->parent() == nullptr);
      }

      SECTION("by issue id and by object, one undo step")
      {
        const auto before = fixture.call("issues_list");
        const auto empty = itemsWith(before, "EMPTY_PROPERTY_VALUE");
        REQUIRE(empty.size() == 2);

        // one issue by id
        auto result = fixture.call("issue_fix", Json{{"issues", {empty[0]["id"]}}});
        CHECK(result["result"]["fixed"] == Json{empty[0]["id"]});
        CHECK(countOf(fixture, "EMPTY_PROPERTY_VALUE") == 1);

        // all fixable issues of an object and its contents; the others are reported
        const auto brushEntity = itemsWith(before, "POINT_ENTITY_WITH_BRUSHES");
        REQUIRE(brushEntity.size() == 1);
        result = fixture.call("issue_fix", Json{{"ids", {brushEntity[0]["objectId"]}}});
        CHECK(result["result"]["fixedCount"] == 1);

        // all fixes of the call are one undo step
        result = fixture.call(
          "issue_fix",
          Json{
            {"codes",
             {"MISSING_ENTITY_CLASSNAME",
              "EMPTY_BRUSH_ENTITY",
              "NON_INTEGER_VERTICES",
              "EMPTY_PROPERTY_VALUE"}}});
        CHECK(result["result"]["fixedCount"] == 4);
        CHECK(result["undoStep"] == "AI: Fix Issues");
        fixture.call("undo");
        CHECK(countOf(fixture, "NON_INTEGER_VERTICES") == 1);
        CHECK(countOf(fixture, "MISSING_ENTITY_CLASSNAME") == 1);
        CHECK(countOf(fixture, "EMPTY_PROPERTY_VALUE") == 1);
      }

      SECTION("dry run")
      {
        const auto result = fixture.call(
          "issue_fix", Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}, {"dryRun", true}});
        CHECK(result["dryRun"] == true);
        CHECK(result["result"]["fixedCount"] == 1);
        CHECK(result["changes"]["removed"].size() == 1);
        CHECK(result["undoStep"].is_null());
        CHECK(countOf(fixture, "MISSING_ENTITY_CLASSNAME") == 1);
      }

      SECTION("checks without fixes")
      {
        const auto result = fixture.call("issue_fix", Json{{"codes", {"Z_FIGHTING"}}});
        CHECK(result["result"]["fixedCount"] == 0);
        REQUIRE(result["result"]["notFixed"].size() == 1);
        CHECK(result["result"]["notFixed"][0]["code"] == "Z_FIGHTING");
        CHECK(result["warnings"][0]["code"] == "NOTHING_FIXED");

        const auto zFighting =
          fixture.call("issues_list", Json{{"codes", {"Z_FIGHTING"}}})["items"];
        REQUIRE(zFighting.size() == 1);
        const auto byId =
          fixture.call("issue_fix", Json{{"issues", {zFighting[0]["id"]}}});
        CHECK(byId["result"]["notFixed"][0]["id"] == zFighting[0]["id"]);
      }

      SECTION("invalid input")
      {
        CHECK(fixture.callExpectingError("issue_fix").code == ErrorCode::InvalidArgument);
        CHECK(
          fixture.callExpectingError("issue_fix", Json{{"issues", {"issue:1:2:3"}}}).code
          == ErrorCode::ObjectNotFound);
        CHECK(
          fixture.callExpectingError("issue_fix", Json{{"issues", {"mcp:garbage"}}}).code
          == ErrorCode::ObjectNotFound);
        CHECK(
          fixture.callExpectingError("issue_fix", Json{{"ids", {"brush:999999999"}}}).code
          == ErrorCode::ObjectNotFound);
      }
    }

    SECTION("Apply Suggested Move")
    {
      loadIssuesMap(fixture);
      const auto listed =
        fixture.call("issues_list", Json{{"codes", {"MODEL_FLOATING"}}});
      REQUIRE(listed["total"] == 1);
      const auto person = listed["items"][0]["objectId"].get<std::string>();
      CHECK(listed["items"][0]["fixes"] == Json{"Apply Suggested Move"});

      const auto result = fixCode(fixture, "MODEL_FLOATING");
      CHECK(
        result["result"]["applied"][0]
        == Json{
          {"fix", "Apply Suggested Move"}, {"code", "MODEL_FLOATING"}, {"count", 1}});
      const auto* entityNode = dynamic_cast<mdl::EntityNode*>(fixture.node(person));
      REQUIRE(entityNode);
      CHECK(*entityNode->entity().property("origin") == "128 -64 24");
    }

    SECTION("Apply Suggested UV Fix")
    {
      fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Valve"}});
      auto* document = fixture.host().documentList.back().document;
      const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "knowledge.wad";
      fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
      processResources(document->map());

      const auto box = fixture
                         .call(
                           "brush_create_box",
                           Json{
                             {"min", {0, 0, 0}},
                             {"max", {128, 128, 128}},
                             {"material", "k_tile"}})["result"]["brush"]
                         .get<std::string>();
      fixture.call(
        "objects_scale",
        Json{{"ids", {box}}, {"factors", {1, 1, 2}}, {"alignmentLock", true}});

      const auto listed =
        fixture.call("issues_list", Json{{"codes", {"UV_ASPECT_DISTORTION"}}});
      REQUIRE(listed["total"].get<size_t>() > 0);
      CHECK(listed["items"][0]["fixes"] == Json{"Apply Suggested UV Fix"});

      const auto result = fixCode(fixture, "UV_ASPECT_DISTORTION");
      CHECK(result["result"]["applied"][0]["fix"] == "Apply Suggested UV Fix");
      CHECK(contains(result["changes"]["modified"], box));
    }
  }

  SECTION("issue_hide and issue_show")
  {
    loadIssuesMap(fixture);
    const auto listed =
      fixture.call("issues_list", Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}});
    REQUIRE(listed["total"] == 1);
    const auto id = listed["items"][0]["id"];

    SECTION("hide and show")
    {
      auto result = fixture.call("issue_hide", Json{{"issues", {id}}});
      CHECK(result["result"]["hidden"] == Json{id});
      CHECK(result["undoStep"].is_null());
      CHECK(countOf(fixture, "MISSING_ENTITY_CLASSNAME") == 0);

      auto all = fixture.call(
        "issues_list",
        Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}, {"includeHidden", true}});
      REQUIRE(all["total"] == 1);
      CHECK(all["items"][0]["hidden"] == true);

      // hiding again changes nothing
      result = fixture.call("issue_hide", Json{{"issues", {id}}});
      CHECK(result["result"]["hidden"] == Json::array());
      CHECK(result["result"]["unchanged"] == Json{id});

      // hidden issues are not fixed by code unless requested
      result = fixture.call("issue_fix", Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}});
      CHECK(result["result"]["fixedCount"] == 0);

      result = fixture.call("issue_show", Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}});
      CHECK(result["result"]["shown"] == Json{id});
      CHECK(countOf(fixture, "MISSING_ENTITY_CLASSNAME") == 1);
    }

    SECTION("dry run")
    {
      const auto result =
        fixture.call("issue_hide", Json{{"issues", {id}}, {"dryRun", true}});
      CHECK(result["result"]["hidden"] == Json{id});
      CHECK(countOf(fixture, "MISSING_ENTITY_CLASSNAME") == 1);
    }

    SECTION("MCP issues cannot be hidden")
    {
      const auto zFighting =
        fixture.call("issues_list", Json{{"codes", {"Z_FIGHTING"}}})["items"];
      REQUIRE(zFighting.size() == 1);
      const auto result =
        fixture.call("issue_hide", Json{{"issues", {zFighting[0]["id"]}}});
      CHECK(result["result"]["skipped"][0]["id"] == zFighting[0]["id"]);
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("issue_hide").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("issue_show", Json{{"issues", {"issue:x"}}}).code
        == ErrorCode::ObjectNotFound);
    }
  }

  SECTION("validators_list and validators_set")
  {
    auto& document = fixture.create();
    auto& map = document.map();

    SECTION("validators_list")
    {
      const auto result = fixture.call("validators_list");
      const auto& validators = result["validators"];
      const auto find = [&](const std::string& name) {
        return *std::ranges::find_if(
          validators, [&](const auto& validator) { return validator["name"] == name; });
      };
      CHECK(
        find("EMPTY_BRUSH_ENTITY")
        == Json{
          {"name", "EMPTY_BRUSH_ENTITY"},
          {"source", "editor"},
          {"title", "Empty brush entity"},
          {"codes", {"EMPTY_BRUSH_ENTITY"}},
          {"enabled", true},
          {"fixes", {"Delete Objects"}},
        });
      CHECK(find("MODEL_PLACEMENT")["source"] == "mcp");
      CHECK(find("MODEL_PLACEMENT")["codes"].size() == 4);
      CHECK(find("MODEL_PLACEMENT")["fixes"] == Json{"Apply Suggested Move"});
      CHECK(find("Z_FIGHTING")["fixes"] == Json::array());
      CHECK(result["disabled"] == Json::array());
    }

    SECTION("turned-off validators are neither listed nor reported per call")
    {
      auto result = fixture.call(
        "validators_set", Json{{"disable", {"z_fighting", "Empty property value"}}});
      CHECK(result["result"]["disabled"] == Json{"EMPTY_PROPERTY_VALUE", "Z_FIGHTING"});
      CHECK(result["result"]["changed"] == Json{"EMPTY_PROPERTY_VALUE", "Z_FIGHTING"});
      CHECK(fixture.call("validators_list")["disabled"] == result["result"]["disabled"]);

      createBox(fixture, {0, 0, 0}, {128, 128, 16});
      result = fixture.call(
        "brush_create_box", Json{{"min", {32, 32, 8}}, {"max", {96, 96, 16}}});
      CHECK(introduced(result, "Z_FIGHTING").empty());
      result = fixture.call(
        "entity_create_point",
        Json{
          {"classname", "info_null"},
          {"position", {0, 0, 64}},
          {"properties", {{"note", ""}}}});
      CHECK(introduced(result, "EMPTY_PROPERTY_VALUE").empty());

      auto listed = fixture.call("issues_list");
      CHECK(itemsWith(listed, "Z_FIGHTING").empty());
      CHECK(itemsWith(listed, "EMPTY_PROPERTY_VALUE").empty());
      CHECK(listed["disabledValidators"] == Json{"EMPTY_PROPERTY_VALUE", "Z_FIGHTING"});

      // dry run
      result =
        fixture.call("validators_set", Json{{"enableAll", true}, {"dryRun", true}});
      CHECK(result["result"]["disabled"] == Json::array());
      CHECK(fixture.call("validators_list")["disabled"].size() == 2);

      fixture.call("validators_set", Json{{"enable", {"Z_FIGHTING"}}});
      listed = fixture.call("issues_list");
      CHECK(itemsWith(listed, "Z_FIGHTING").size() == 1);
      CHECK(itemsWith(listed, "EMPTY_PROPERTY_VALUE").empty());

      fixture.call("validators_set", Json{{"enableAll", true}});
      CHECK(itemsWith(fixture.call("issues_list"), "EMPTY_PROPERTY_VALUE").size() == 1);
      CHECK(&map == &document.map());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("validators_set", Json{{"disable", {"NO_SUCH_CHECK"}}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("issues resource")
  {
    auto& document = loadIssuesMap(fixture);
    const auto uri =
      "trenchbroom://documents/" + fixture.documentId(document) + "/issues";

    const auto read = [&]() {
      const auto response = fixture.rpc("resources/read", Json{{"uri", uri}});
      REQUIRE(response.contains("result"));
      return *parseJson(response["result"]["contents"][0]["text"].get<std::string>());
    };

    auto content = read();
    const auto listed = fixture.call("issues_list");
    CHECK(content["total"] == listed["total"]);
    CHECK(content["counts"] == listed["counts"]);
    CHECK(content["items"] == listed["items"]);
    CHECK(content["truncated"] == false);
    CHECK(content["document"] == fixture.documentId(document));

    const auto templates = fixture.rpc("resources/templates/list");
    CHECK(std::ranges::any_of(
      templates["result"]["resourceTemplates"], [](const auto& resourceTemplate) {
        return resourceTemplate["uriTemplate"] == "trenchbroom://documents/{doc}/issues";
      }));

    auto notifications = std::make_shared<CapturingNotificationStream>();
    REQUIRE(fixture.server().openNotificationStream(fixture.sessionId(), notifications));
    REQUIRE(fixture.rpc("resources/subscribe", Json{{"uri", uri}})["result"].is_object());

    SECTION("map changes notify once per burst")
    {
      createBox(fixture, {512, 0, 0}, {576, 64, 64});
      createBox(fixture, {640, 0, 0}, {704, 64, 64});
      fixture.scheduler().runPending();
      CHECK(std::ranges::count(updatedUris(*notifications), uri) == 1);
    }

    SECTION("hiding issues and turning off validators notify")
    {
      fixture.call("issue_hide", Json{{"codes", {"MISSING_ENTITY_CLASSNAME"}}});
      fixture.scheduler().runPending();
      CHECK(std::ranges::count(updatedUris(*notifications), uri) == 1);
      CHECK(read()["counts"].contains("MISSING_ENTITY_CLASSNAME") == false);

      notifications->notifications.clear();
      fixture.call("validators_set", Json{{"disable", {"Z_FIGHTING"}}});
      fixture.scheduler().runPending();
      CHECK(std::ranges::count(updatedUris(*notifications), uri) == 1);
      content = read();
      CHECK(content["counts"].contains("Z_FIGHTING") == false);
      CHECK(content["disabledValidators"] == Json{"Z_FIGHTING"});
    }
  }
}

} // namespace tb::mcp
