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
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mcp/tools/SelectionTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

std::vector<mdl::Node*> allNodes(mdl::Node& root)
{
  auto result = std::vector<mdl::Node*>{&root};
  for (auto* child : root.children())
  {
    const auto childNodes = allNodes(*child);
    result.insert(result.end(), childNodes.begin(), childNodes.end());
  }
  return result;
}

mdl::BrushNode* brushWithBounds(mdl::Map& map, const vm::bbox3d& bounds)
{
  for (auto* node : allNodes(map.worldNode()))
  {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node);
        brushNode && brushNode->logicalBounds() == bounds)
    {
      return brushNode;
    }
  }
  return nullptr;
}

mdl::EntityNode* entityWithClassname(mdl::Map& map, const std::string& classname)
{
  for (auto* node : allNodes(map.worldNode()))
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
        entityNode && entityNode->entity().classname() == classname)
    {
      return entityNode;
    }
  }
  return nullptr;
}

template <typename T>
T* nodeWithName(mdl::Map& map, const std::string& name)
{
  for (auto* node : allNodes(map.worldNode()))
  {
    if (auto* typedNode = dynamic_cast<T*>(node); typedNode && typedNode->name() == name)
    {
      return typedNode;
    }
  }
  return nullptr;
}

size_t topFaceIndex(const mdl::BrushNode& brushNode)
{
  const auto& faces = brushNode.brush().faces();
  for (size_t i = 0; i < faces.size(); ++i)
  {
    if (faces[i].normal() == vm::vec3d{0, 0, 1})
    {
      return i;
    }
  }
  return faces.size();
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

std::vector<std::string> selectedIds(McpToolFixture& fixture, mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto* node : map.selection().nodes)
  {
    result.push_back(fixture.id(*node));
  }
  return result;
}

} // namespace

TEST_CASE("SelectionTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  auto& map = document.map();

  auto* floorA = brushWithBounds(map, {{-16, -16, -16}, {528, 528, 0}});
  auto* westWall = brushWithBounds(map, {{-16, -16, 0}, {0, 528, 256}});
  auto* corridorFloor = brushWithBounds(map, {{528, 208, -16}, {768, 304, 0}});
  auto* doorBrush = brushWithBounds(map, {{768, 224, 0}, {784, 288, 128}});
  auto* pillar = brushWithBounds(map, {{192, 192, 0}, {224, 224, 256}});
  auto* ogre = entityWithClassname(map, "monster_ogre");
  auto* playerStart = entityWithClassname(map, "info_player_start");
  auto* pillars = nodeWithName<mdl::GroupNode>(map, "Pillars");
  auto* arena = nodeWithName<mdl::LayerNode>(map, "Arena");
  REQUIRE(floorA);
  REQUIRE(westWall);
  REQUIRE(corridorFloor);
  REQUIRE(doorBrush);
  REQUIRE(pillar);
  REQUIRE(ogre);
  REQUIRE(playerStart);
  REQUIRE(pillars);
  REQUIRE(arena);

  const auto floorId = fixture.id(*floorA);
  const auto westId = fixture.id(*westWall);
  const auto ogreId = fixture.id(*ogre);
  const auto playerStartId = fixture.id(*playerStart);
  const auto pillarsId = fixture.id(*pillars);
  const auto arenaId = fixture.id(*arena);
  const auto floorTopId = floorId + "/face:" + std::to_string(topFaceIndex(*floorA));

  const auto& selection = map.selection();

  SECTION("selection_get")
  {
    const auto empty = fixture.call("selection_get");
    CHECK(empty["mode"] == "none");
    CHECK(empty["count"] == 0);
    CHECK(empty["bounds"].is_null());
    CHECK(empty["items"].empty());
    CHECK(empty["nextCursor"].is_null());

    mdl::selectNodes(map, {floorA, westWall, ogre});

    const auto first = fixture.call("selection_get", Json{{"limit", 2}});
    CHECK(first["mode"] == "objects");
    CHECK(first["count"] == 3);
    CHECK(first["total"] == 3);
    CHECK(first["countsByKind"] == Json{{"entity", 1}, {"brush", 2}});
    CHECK(first["bounds"].is_object());
    REQUIRE(first["items"].size() == 2);
    CHECK(first["items"][0]["id"] == floorId);
    CHECK(first["items"][0]["kind"] == "brush");
    CHECK(first["items"][0].contains("bounds"));
    CHECK_FALSE(first["items"][0].contains("state"));
    REQUIRE(first["nextCursor"].is_string());

    const auto second =
      fixture.call("selection_get", Json{{"limit", 2}, {"cursor", first["nextCursor"]}});
    REQUIRE(second["items"].size() == 1);
    CHECK(second["items"][0]["id"] == ogreId);
    CHECK(second["items"][0]["classname"] == "monster_ogre");
    CHECK(second["nextCursor"].is_null());

    const auto fields =
      fixture.call("selection_get", Json{{"fields", {"id"}}, {"detail", "full"}});
    CHECK(fields["items"][0] == Json{{"id", floorId}});

    const auto full = fixture.call("selection_get", Json{{"detail", "full"}});
    CHECK(full["items"][0]["state"]["selected"] == true);

    const auto badCursor =
      fixture.callExpectingError("selection_get", Json{{"cursor", "not a cursor"}});
    CHECK(badCursor.code == ErrorCode::InvalidArgument);

    SECTION("faces")
    {
      mdl::deselectAll(map);
      mdl::selectBrushFaces(map, {{floorA, topFaceIndex(*floorA)}});
      const auto faces = fixture.call("selection_get", Json{{"detail", "full"}});
      CHECK(faces["mode"] == "faces");
      CHECK(faces["count"] == 1);
      CHECK(faces["countsByKind"] == Json{{"face", 1}, {"brush", 1}});
      CHECK(faces["bounds"]["min"] == Json{-16, -16, 0});
      CHECK(faces["bounds"]["max"] == Json{528, 528, 0});
      REQUIRE(faces["items"].size() == 1);
      CHECK(faces["items"][0]["id"] == floorTopId);
      CHECK(faces["items"][0]["material"] == "floor_stone");
      CHECK(faces["items"][0].contains("vertices"));
    }

    SECTION("selectionDetails")
    {
      const auto& ids = fixture.server().state().documentState(document).ids;
      const auto details = selectionDetails(map, ids, 1);
      CHECK(details["count"] == 3);
      CHECK(details["items"].size() == 1);
      CHECK(details["truncated"] == true);
      CHECK(selectionDetails(map, ids)["truncated"] == false);
    }
  }

  SECTION("selection_set")
  {
    const auto replaced = fixture.call("selection_set", Json{{"ids", {floorId, westId}}});
    CHECK(resultOf(replaced) == Json{{"mode", "objects"}, {"count", 2}});
    CHECK(replaced["selection"]["count"] == 2);
    CHECK(replaced["undoStep"] == "AI: Set Selection");
    CHECK(selectedIds(fixture, map) == std::vector<std::string>{floorId, westId});

    const auto added =
      fixture.call("selection_set", Json{{"ids", {ogreId}}, {"mode", "add"}});
    CHECK(resultOf(added)["count"] == 3);

    const auto removed =
      fixture.call("selection_set", Json{{"ids", {floorId}}, {"mode", "remove"}});
    CHECK(resultOf(removed)["count"] == 2);
    CHECK(selectedIds(fixture, map) == std::vector<std::string>{westId, ogreId});

    SECTION("undo restores the previous selection in one step")
    {
      fixture.call("undo");
      CHECK(selection.nodes.size() == 3);
      fixture.call("undo");
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{floorId, westId});
    }

    SECTION("dry run")
    {
      const auto dryRun =
        fixture.call("selection_set", Json{{"ids", {playerStartId}}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(dryRun["selection"]["ids"] == Json{playerStartId});
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{westId, ogreId});
    }

    SECTION("faces")
    {
      const auto faces = fixture.call("selection_set", Json{{"ids", {floorTopId}}});
      CHECK(resultOf(faces) == Json{{"mode", "faces"}, {"count", 1}});
      CHECK(selection.nodes.empty());
      CHECK(selection.brushFaces.size() == 1);

      const auto addObject = fixture.callExpectingError(
        "selection_set", Json{{"ids", {floorId}}, {"mode", "add"}});
      CHECK(addObject.code == ErrorCode::InvalidArgument);
      CHECK(addObject.hint.find("replace") != std::string::npos);

      const auto removedFace =
        fixture.call("selection_set", Json{{"ids", {floorTopId}}, {"mode", "remove"}});
      CHECK(resultOf(removedFace)["mode"] == "none");
    }

    SECTION("adding faces to objects fails")
    {
      const auto error = fixture.callExpectingError(
        "selection_set", Json{{"ids", {floorTopId}}, {"mode", "add"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("selection_set", Json{{"ids", {floorId, floorTopId}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("selection_set", Json{{"ids", Json::array()}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("selection_set", Json{{"ids", {"brush:999999"}}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("selection_set", Json{{"ids", {floorId + "/face:99"}}})
          .code
        == ErrorCode::ObjectNotFound);

      const auto layer =
        fixture.callExpectingError("selection_set", Json{{"ids", {arenaId}}});
      CHECK(layer.code == ErrorCode::WrongObjectKind);
      CHECK(layer.hint.find("select_by") != std::string::npos);

      const auto closedGroup =
        fixture.callExpectingError("selection_set", Json{{"ids", {fixture.id(*pillar)}}});
      CHECK(closedGroup.code == ErrorCode::ObjectNotEditable);

      // the selection is unchanged
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{westId, ogreId});
    }
  }

  SECTION("selection_clear")
  {
    mdl::selectNodes(map, {floorA, westWall});

    const auto dryRun = fixture.call("selection_clear", Json{{"dryRun", true}});
    CHECK(resultOf(dryRun)["cleared"] == 2);
    CHECK(dryRun["selection"]["mode"] == "none");
    CHECK(selection.nodes.size() == 2);

    const auto cleared = fixture.call("selection_clear");
    CHECK(resultOf(cleared)["cleared"] == 2);
    CHECK_FALSE(selection.hasAny());

    CHECK(resultOf(fixture.call("selection_clear"))["cleared"] == 0);
  }

  SECTION("select_all and select_invert")
  {
    const auto all = fixture.call("select_all");
    CHECK(resultOf(all)["count"] == 29);
    CHECK(std::ranges::find(selection.nodes, pillars) != selection.nodes.end());
    CHECK(std::ranges::find(selection.nodes, pillar) == selection.nodes.end());

    mdl::deselectAll(map);
    mdl::selectNodes(map, {floorA});
    const auto inverted = fixture.call("select_invert");
    CHECK(resultOf(inverted)["count"] == 28);
    CHECK_FALSE(floorA->selected());

    const auto dryRun = fixture.call("select_invert", Json{{"dryRun", true}});
    CHECK(dryRun["selection"]["ids"] == Json{floorId});
    CHECK(selection.nodes.size() == 28);
  }

  SECTION("select_by")
  {
    SECTION("classname")
    {
      const auto result = fixture.call("select_by", Json{{"classname", "MONSTER_OGRE"}});
      CHECK(resultOf(result)["count"] == 1);
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{ogreId});

      const auto none =
        fixture.call("select_by", Json{{"classname", "monster_shambler"}});
      CHECK(resultOf(none)["count"] == 0);
      CHECK(hasWarning(none, "NO_MATCH"));
    }

    SECTION("material")
    {
      const auto brushes = fixture.call("select_by", Json{{"material", "wall_metal"}});
      CHECK(resultOf(brushes) == Json{{"mode", "objects"}, {"count", 5}});

      const auto faces =
        fixture.call("select_by", Json{{"material", "wall_metal"}, {"target", "faces"}});
      CHECK(resultOf(faces) == Json{{"mode", "faces"}, {"count", 30}});

      const auto none = fixture.call("select_by", Json{{"material", "no_such_material"}});
      CHECK(resultOf(none)["count"] == 0);
      CHECK(hasWarning(none, "NO_MATCH"));
    }

    SECTION("layers")
    {
      const auto result = fixture.call("select_by", Json{{"layers", {arenaId}}});
      CHECK(resultOf(result)["count"] == 13);
      CHECK(ogre->selected());

      CHECK(
        fixture.callExpectingError("select_by", Json{{"layers", {floorId}}}).code
        == ErrorCode::InvalidArgument);
    }

    SECTION("linked groups")
    {
      const auto unlinked =
        fixture.callExpectingError("select_by", Json{{"linkedGroup", pillarsId}});
      CHECK(unlinked.code == ErrorCode::InvalidArgument);

      mdl::selectNodes(map, {pillars});
      auto* duplicate = mdl::createLinkedDuplicate(map);
      REQUIRE(duplicate);
      mdl::deselectAll(map);

      const auto result = fixture.call("select_by", Json{{"linkedGroup", pillarsId}});
      CHECK(resultOf(result)["count"] == 2);
      CHECK(pillars->selected());
      CHECK(duplicate->selected());

      const auto list = fixture.call("select_by", Json{{"linkedGroup", {pillarsId}}});
      CHECK(resultOf(list)["count"] == 2);
    }

    SECTION("dry run")
    {
      mdl::selectNodes(map, {floorA});
      const auto dryRun =
        fixture.call("select_by", Json{{"classname", "monster_ogre"}, {"dryRun", true}});
      CHECK(dryRun["selection"]["ids"] == Json{ogreId});
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{floorId});
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("select_by").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "select_by", Json{{"classname", "light"}, {"material", "wall_metal"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "select_by", Json{{"classname", "light"}, {"target", "faces"}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("select_spatial")
  {
    const auto addSelector = [&](const vm::bbox3d& bounds) {
      auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                     .createCuboid(bounds, "selector")
                     .value();
      auto* brushNode = new mdl::BrushNode{std::move(brush)};
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      mdl::deselectAll(map);
      return brushNode;
    };

    SECTION("inside")
    {
      auto* selector = addSelector({{-32, -32, -32}, {544, 544, 288}});
      const auto selectorId = fixture.id(*selector);

      const auto dryRun = fixture.call(
        "select_spatial",
        Json{{"mode", "inside"}, {"ids", {selectorId}}, {"dryRun", true}});
      CHECK(resultOf(dryRun)["count"] == 12);
      CHECK_FALSE(selection.hasAny());

      const auto result =
        fixture.call("select_spatial", Json{{"mode", "inside"}, {"ids", {selectorId}}});
      CHECK(resultOf(result)["count"] == 12);
      CHECK(resultOf(result)["selectors"] == Json{selectorId});
      CHECK(floorA->selected());
      CHECK(pillars->selected());
      CHECK(playerStart->selected());
      CHECK_FALSE(selector->selected());
      CHECK_FALSE(corridorFloor->selected());
    }

    SECTION("touching with the selected brushes")
    {
      auto* selector = addSelector({{100, 100, -8}, {110, 110, -4}});
      mdl::selectNodes(map, {selector});

      const auto result = fixture.call("select_spatial", Json{{"mode", "touching"}});
      CHECK(resultOf(result)["count"] == 1);
      CHECK(floorA->selected());
    }

    SECTION("tall with deleted selectors")
    {
      auto* selector = addSelector({{240, 240, 500}, {272, 272, 510}});
      const auto selectorId = fixture.id(*selector);

      const auto dryRun = fixture.call(
        "select_spatial",
        Json{
          {"mode", "tall"},
          {"ids", {selectorId}},
          {"deleteSelectors", true},
          {"dryRun", true}});
      CHECK(dryRun["changes"]["removed"] == Json{selectorId});
      CHECK(fixture.node(selectorId) == selector);

      const auto result = fixture.call(
        "select_spatial",
        Json{{"mode", "tall"}, {"ids", {selectorId}}, {"deleteSelectors", true}});
      CHECK(resultOf(result)["count"] == 1);
      CHECK(resultOf(result)["selectorsDeleted"] == true);
      CHECK(entityWithClassname(map, "light")->selected());
      CHECK(
        fixture.callExpectingError("selection_set", Json{{"ids", {selectorId}}}).code
        == ErrorCode::ObjectNotFound);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("select_spatial", Json{{"mode", "inside"}}).code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "select_spatial", Json{{"mode", "inside"}, {"ids", {ogreId}}})
          .code
        == ErrorCode::WrongObjectKind);

      mdl::selectNodes(map, {ogre});
      CHECK(
        fixture.callExpectingError("select_spatial", Json{{"mode", "touching"}}).code
        == ErrorCode::WrongObjectKind);
      CHECK(
        fixture
          .callExpectingError("select_spatial", Json{{"mode", "tall"}, {"axis", "w"}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("select_siblings")
  {
    const auto door =
      fixture.call("select_siblings", Json{{"ids", {fixture.id(*doorBrush)}}});
    CHECK(resultOf(door)["count"] == 1);
    CHECK(doorBrush->selected());

    mdl::deselectAll(map);
    mdl::selectNodes(map, {ogre});
    const auto arenaSiblings = fixture.call("select_siblings");
    CHECK(resultOf(arenaSiblings)["count"] == 13);

    const auto dryRun =
      fixture.call("select_siblings", Json{{"ids", {westId}}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["count"] == 16);
    CHECK(selection.nodes.size() == 13);

    mdl::deselectAll(map);
    CHECK(fixture.callExpectingError("select_siblings").code == ErrorCode::NoSelection);
  }

  SECTION("select_by_line")
  {
    // monster_ogre is defined at lines 230-235 of the fixture
    const auto ogreLine = fixture.call("select_by_line", Json{{"lines", {232}}});
    CHECK(resultOf(ogreLine)["count"] == 1);
    CHECK(selectedIds(fixture, map) == std::vector<std::string>{ogreId});

    // the first brush of worldspawn (the floor of room A) is at lines 6-13
    const auto floorLine = fixture.call("select_by_line", Json{{"lines", {7, 232}}});
    CHECK(resultOf(floorLine)["count"] == 2);
    CHECK(floorA->selected());

    const auto none = fixture.call("select_by_line", Json{{"lines", {1}}});
    CHECK(resultOf(none)["count"] == 0);
    CHECK(hasWarning(none, "NO_MATCH"));

    CHECK(
      fixture.callExpectingError("select_by_line", Json{{"lines", Json::array()}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("select_by_line", Json{{"lines", {0}}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("select_faces_of")
  {
    const auto brushFaces = fixture.call("select_faces_of", Json{{"ids", {floorId}}});
    CHECK(resultOf(brushFaces) == Json{{"mode", "faces"}, {"count", 6}});

    // the floors of room A, the corridor and room B are coplanar and share edges
    const auto coplanar = fixture.call("select_faces_of", Json{{"face", floorTopId}});
    CHECK(resultOf(coplanar)["count"] == 3);
    CHECK(
      std::ranges::find(
        selection.brushFaces,
        mdl::BrushFaceHandle{corridorFloor, topFaceIndex(*corridorFloor)})
      != selection.brushFaces.end());

    const auto single =
      fixture.call("select_faces_of", Json{{"face", floorTopId}, {"coplanar", false}});
    CHECK(resultOf(single)["count"] == 1);

    mdl::deselectAll(map);
    mdl::selectNodes(map, {floorA, westWall});
    const auto fromSelection = fixture.call("select_faces_of", Json{{"dryRun", true}});
    CHECK(resultOf(fromSelection)["count"] == 12);
    CHECK(selection.nodes.size() == 2);

    CHECK(
      fixture
        .callExpectingError(
          "select_faces_of", Json{{"ids", {floorId}}, {"face", floorTopId}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("select_faces_of", Json{{"face", floorId}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("select_faces_of", Json{{"ids", {ogreId}}}).code
      == ErrorCode::WrongObjectKind);
    CHECK(
      fixture.callExpectingError("select_faces_of", Json{{"coplanar", true}}).code
      == ErrorCode::InvalidArgument);
  }
}

} // namespace tb::mcp
