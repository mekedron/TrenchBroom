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
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
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

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

std::vector<std::string> layerNames(mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto* layerNode : map.worldNode().allLayersUserSorted())
  {
    result.push_back(layerNode->name());
  }
  return result;
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

TEST_CASE("LayerTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  auto& map = document.map();
  const auto& editorContext = map.editorContext();

  auto* defaultLayer = map.worldNode().defaultLayer();
  auto* arena = nodeWithName<mdl::LayerNode>(map, "Arena");
  auto* pillars = nodeWithName<mdl::GroupNode>(map, "Pillars");
  auto* floorA = brushWithBounds(map, {{-16, -16, -16}, {528, 528, 0}});
  auto* westWall = brushWithBounds(map, {{-16, -16, 0}, {0, 528, 256}});
  auto* doorBrush = brushWithBounds(map, {{768, 224, 0}, {784, 288, 128}});
  auto* ogre = entityWithClassname(map, "monster_ogre");
  auto* door = entityWithClassname(map, "func_door");
  auto* playerStart = entityWithClassname(map, "info_player_start");
  REQUIRE(arena);
  REQUIRE(pillars);
  REQUIRE(floorA);
  REQUIRE(westWall);
  REQUIRE(doorBrush);
  REQUIRE(ogre);
  REQUIRE(door);
  REQUIRE(playerStart);
  REQUIRE(pillars->childCount() > 0);

  const auto arenaId = fixture.id(*arena);
  const auto pillarsId = fixture.id(*pillars);
  const auto floorId = fixture.id(*floorA);
  const auto westId = fixture.id(*westWall);
  const auto doorBrushId = fixture.id(*doorBrush);
  const auto ogreId = fixture.id(*ogre);
  const auto doorId = fixture.id(*door);
  const auto playerStartId = fixture.id(*playerStart);
  const auto pillarId = fixture.id(*pillars->children().front());

  SECTION("layers_list")
  {
    const auto list = fixture.call("layers_list");
    REQUIRE(list["layers"].size() == 2);
    CHECK(list["current"] == "layer:default");

    const auto& first = list["layers"][0];
    CHECK(first["id"] == "layer:default");
    CHECK(first["default"] == true);
    CHECK(first["position"] == 0);
    CHECK(first["current"] == true);
    CHECK(first["hidden"] == false);
    CHECK(first["locked"] == false);
    CHECK(first["omitFromExport"] == false);
    CHECK(first["color"].is_null());
    CHECK(first["counts"]["groups"] == 1);
    CHECK(first["counts"]["entities"] == 3);

    const auto& second = list["layers"][1];
    CHECK(second["id"] == arenaId);
    CHECK(second["name"] == "Arena");
    CHECK(second["default"] == false);
    CHECK(second["position"] == 1);
    CHECK(second["current"] == false);
    CHECK(second["counts"]["entities"] == 5);
    CHECK(second["counts"]["groups"] == 0);
    CHECK(second["counts"]["brushes"] == 10);
    CHECK(second["counts"]["total"] == 15);

    SECTION("reports the state")
    {
      mdl::hideLayers(map, {arena});
      mdl::setCurrentLayer(map, arena);
      mdl::setOmitLayerFromExport(map, arena, true);

      const auto changed = fixture.call("layers_list");
      CHECK(changed["current"] == arenaId);
      CHECK(changed["layers"][1]["hidden"] == true);
      CHECK(changed["layers"][1]["current"] == true);
      CHECK(changed["layers"][1]["omitFromExport"] == true);
      CHECK(changed["layers"][0]["current"] == false);
    }
  }

  SECTION("layer_create")
  {
    SECTION("appends a layer and makes it current")
    {
      const auto created = fixture.call("layer_create", Json{{"name", "Lighting"}});
      CHECK(created["undoStep"] == "AI: Create Layer");
      const auto& layer = resultOf(created)["layer"];
      CHECK(layer["name"] == "Lighting");
      CHECK(layer["position"] == 2);
      CHECK(layer["current"] == true);
      CHECK(layer["counts"]["total"] == 0);
      CHECK(created["changes"]["created"] == Json::array({layer["id"]}));

      auto* layerNode = fixture.node(layer["id"].get<std::string>());
      REQUIRE(layerNode);
      CHECK(editorContext.currentLayer() == layerNode);
      CHECK(
        layerNames(map)
        == std::vector<std::string>{"Default Layer", "Arena", "Lighting"});

      fixture.call("undo");
      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer", "Arena"});
      CHECK(editorContext.currentLayer() == defaultLayer);
    }

    SECTION("at a position or after a layer")
    {
      const auto atTop =
        fixture.call("layer_create", Json{{"name", "Top"}, {"position", 1}});
      CHECK(resultOf(atTop)["layer"]["position"] == 1);
      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer", "Top", "Arena"});

      const auto afterTop = fixture.call(
        "layer_create",
        Json{
          {"name", "Second"},
          {"after", resultOf(atTop)["layer"]["id"]},
          {"makeCurrent", false}});
      CHECK(resultOf(afterTop)["layer"]["position"] == 2);
      CHECK(resultOf(afterTop)["layer"]["current"] == false);
      CHECK(
        layerNames(map)
        == std::vector<std::string>{"Default Layer", "Top", "Second", "Arena"});

      const auto afterDefault =
        fixture.call("layer_create", Json{{"name", "First"}, {"after", "layer:default"}});
      CHECK(resultOf(afterDefault)["layer"]["position"] == 1);
    }

    SECTION("warns about duplicate names")
    {
      const auto created = fixture.call("layer_create", Json{{"name", "Arena"}});
      CHECK(hasWarning(created, "DUPLICATE_LAYER_NAME"));
    }

    SECTION("dry run")
    {
      const auto dryRun =
        fixture.call("layer_create", Json{{"name", "Lighting"}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(resultOf(dryRun)["layer"]["name"] == "Lighting");
      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer", "Arena"});
      CHECK(editorContext.currentLayer() == defaultLayer);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("layer_create", Json{{"name", ""}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("layer_create", Json{{"name", "X"}, {"position", 3}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "layer_create",
            Json{{"name", "X"}, {"position", 1}, {"after", "layer:default"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("layer_create", Json{{"name", "X"}, {"after", floorId}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "layer_create", Json{{"name", "X"}, {"after", "layer:99999"}})
          .code
        == ErrorCode::ObjectNotFound);
      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer", "Arena"});
    }
  }

  SECTION("layer_rename")
  {
    const auto renamed =
      fixture.call("layer_rename", Json{{"layer", arenaId}, {"name", "Battle"}});
    CHECK(renamed["undoStep"] == "AI: Rename Layer");
    CHECK(resultOf(renamed)["layer"]["name"] == "Battle");
    CHECK(arena->name() == "Battle");

    fixture.call("undo");
    CHECK(arena->name() == "Arena");

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "layer_rename", Json{{"layer", arenaId}, {"name", "Battle"}, {"dryRun", true}});
      CHECK(resultOf(dryRun)["layer"]["name"] == "Battle");
      CHECK(arena->name() == "Arena");
    }

    SECTION("invalid input")
    {
      const auto error = fixture.callExpectingError(
        "layer_rename", Json{{"layer", "layer:default"}, {"name", "X"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("default layer") != std::string::npos);
      CHECK(
        fixture.callExpectingError("layer_rename", Json{{"layer", arenaId}, {"name", ""}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("layer_rename", Json{{"layer", floorId}, {"name", "X"}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("layer_remove")
  {
    const auto arenaObjects = arena->childCount();
    const auto defaultObjects = defaultLayer->childCount();

    SECTION("moves the objects to the default layer")
    {
      mdl::setCurrentLayer(map, arena);
      mdl::selectNodes(map, {ogre});

      const auto removed = fixture.call("layer_remove", Json{{"layer", arenaId}});
      CHECK(removed["undoStep"] == "AI: Remove Layer");
      CHECK(resultOf(removed)["removed"] == arenaId);
      CHECK(resultOf(removed)["movedToDefaultLayer"].size() == arenaObjects);
      CHECK(resultOf(removed)["current"] == "layer:default");
      CHECK(removed["changes"]["removed"] == Json::array({arenaId}));

      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer"});
      CHECK(defaultLayer->childCount() == defaultObjects + arenaObjects);
      CHECK(mdl::findContainingLayer(ogre) == defaultLayer);
      CHECK(editorContext.currentLayer() == defaultLayer);
      CHECK(map.selection().nodes.empty());

      fixture.call("undo");
      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer", "Arena"});
      CHECK(mdl::findContainingLayer(ogre) == arena);
      CHECK(fixture.id(*fixture.node(arenaId)) == arenaId);
    }

    SECTION("dry run")
    {
      const auto dryRun =
        fixture.call("layer_remove", Json{{"layer", arenaId}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(layerNames(map) == std::vector<std::string>{"Default Layer", "Arena"});
      CHECK(arena->childCount() == arenaObjects);
    }

    SECTION("the default layer cannot be removed")
    {
      const auto error =
        fixture.callExpectingError("layer_remove", Json{{"layer", "layer:default"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.message.find("default layer") != std::string::npos);
    }

    SECTION("needs another visible, unlocked layer")
    {
      mdl::hideLayers(map, {defaultLayer});
      const auto error =
        fixture.callExpectingError("layer_remove", Json{{"layer", arenaId}});
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(error.hint.find("layer_set_state") != std::string::npos);
    }
  }

  SECTION("layer_reorder")
  {
    fixture.call("layer_create", Json{{"name", "B"}});
    fixture.call("layer_create", Json{{"name", "C"}});
    REQUIRE(
      layerNames(map) == std::vector<std::string>{"Default Layer", "Arena", "B", "C"});

    SECTION("by offset")
    {
      const auto moved =
        fixture.call("layer_reorder", Json{{"layer", arenaId}, {"offset", 2}});
      CHECK(moved["undoStep"] == "AI: Reorder Layer");
      CHECK(resultOf(moved)["layer"]["position"] == 3);
      CHECK(
        layerNames(map) == std::vector<std::string>{"Default Layer", "B", "C", "Arena"});

      fixture.call("undo");
      CHECK(
        layerNames(map) == std::vector<std::string>{"Default Layer", "Arena", "B", "C"});
    }

    SECTION("to a position")
    {
      const auto cId = fixture.id(*nodeWithName<mdl::LayerNode>(map, "C"));
      fixture.call("layer_reorder", Json{{"layer", cId}, {"position", 1}});
      CHECK(
        layerNames(map) == std::vector<std::string>{"Default Layer", "C", "Arena", "B"});
    }

    SECTION("dry run")
    {
      fixture.call(
        "layer_reorder", Json{{"layer", arenaId}, {"offset", 1}, {"dryRun", true}});
      CHECK(
        layerNames(map) == std::vector<std::string>{"Default Layer", "Arena", "B", "C"});
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture
          .callExpectingError(
            "layer_reorder", Json{{"layer", "layer:default"}, {"offset", 1}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("layer_reorder", Json{{"layer", arenaId}, {"offset", -1}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError("layer_reorder", Json{{"layer", arenaId}, {"position", 4}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("layer_reorder", Json{{"layer", arenaId}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "layer_reorder", Json{{"layer", arenaId}, {"offset", 1}, {"position", 2}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("layer_set_state")
  {
    SECTION("hidden")
    {
      mdl::selectNodes(map, {ogre});
      const auto hidden =
        fixture.call("layer_set_state", Json{{"layer", arenaId}, {"hidden", true}});
      CHECK(hidden["undoStep"] == "AI: Set Layer State");
      CHECK(resultOf(hidden)["layer"]["hidden"] == true);
      CHECK(arena->hidden());
      CHECK(!editorContext.visible(*ogre));
      CHECK(map.selection().nodes.empty());

      fixture.call("layer_set_state", Json{{"layer", arenaId}, {"hidden", false}});
      CHECK(!arena->hidden());
      CHECK(editorContext.visible(*ogre));

      fixture.call("undo");
      CHECK(arena->hidden());
    }

    SECTION("locked")
    {
      fixture.call("layer_set_state", Json{{"layer", arenaId}, {"locked", true}});
      CHECK(arena->locked());
      CHECK(!editorContext.selectable(*ogre));

      fixture.call("layer_set_state", Json{{"layer", arenaId}, {"locked", false}});
      CHECK(!arena->locked());
      CHECK(editorContext.selectable(*ogre));
    }

    SECTION("omitFromExport")
    {
      fixture.call("layer_set_state", Json{{"layer", arenaId}, {"omitFromExport", true}});
      CHECK(arena->layer().omitFromExport());
      fixture.call("undo");
      CHECK(!arena->layer().omitFromExport());
    }

    SECTION("current")
    {
      const auto current =
        fixture.call("layer_set_state", Json{{"layer", arenaId}, {"current", true}});
      CHECK(resultOf(current)["layer"]["current"] == true);
      CHECK(editorContext.currentLayer() == arena);

      const auto error = fixture.callExpectingError(
        "layer_set_state", Json{{"layer", arenaId}, {"current", false}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.hint.find("layer:default") != std::string::npos);

      // unsetting a layer that is not current changes nothing
      const auto noChange = fixture.call(
        "layer_set_state", Json{{"layer", "layer:default"}, {"current", false}});
      CHECK(noChange["undoStep"].is_null());

      fixture.call("undo");
      CHECK(editorContext.currentLayer() == defaultLayer);
    }

    SECTION("hiding or locking the current layer warns")
    {
      const auto hidden = fixture.call(
        "layer_set_state", Json{{"layer", "layer:default"}, {"hidden", true}});
      CHECK(hasWarning(hidden, "CURRENT_LAYER_HIDDEN"));
      CHECK(defaultLayer->hidden());

      const auto locked = fixture.call(
        "layer_set_state", Json{{"layer", "layer:default"}, {"locked", true}});
      CHECK(hasWarning(locked, "CURRENT_LAYER_LOCKED"));

      const auto moved =
        fixture.call("layer_set_state", Json{{"layer", arenaId}, {"current", true}});
      CHECK(!hasWarning(moved, "CURRENT_LAYER_HIDDEN"));
    }

    SECTION("isolate")
    {
      const auto isolated =
        fixture.call("layer_set_state", Json{{"layer", arenaId}, {"isolate", true}});
      CHECK(isolated["undoStep"] == "AI: Set Layer State");
      CHECK(defaultLayer->hidden());
      CHECK(editorContext.visible(*arena));
      CHECK(editorContext.visible(*ogre));
      CHECK(!editorContext.visible(*floorA));

      fixture.call("undo");
      CHECK(!defaultLayer->hidden());
      CHECK(editorContext.visible(*floorA));
    }

    SECTION("several states in one step")
    {
      const auto result = fixture.call(
        "layer_set_state",
        Json{
          {"layer", arenaId},
          {"locked", true},
          {"omitFromExport", true},
          {"current", true}});
      CHECK(result["undoStep"] == "AI: Set Layer State");
      CHECK(arena->locked());
      CHECK(arena->layer().omitFromExport());
      CHECK(editorContext.currentLayer() == arena);

      fixture.call("undo");
      CHECK(!arena->locked());
      CHECK(!arena->layer().omitFromExport());
      CHECK(editorContext.currentLayer() == defaultLayer);
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "layer_set_state", Json{{"layer", arenaId}, {"hidden", true}, {"dryRun", true}});
      CHECK(resultOf(dryRun)["layer"]["hidden"] == true);
      CHECK(!arena->hidden());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("layer_set_state", Json{{"layer", arenaId}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "layer_set_state", Json{{"layer", arenaId}, {"isolate", false}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "layer_set_state",
            Json{{"layer", arenaId}, {"isolate", true}, {"hidden", true}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "layer_set_state", Json{{"layer", floorId}, {"hidden", true}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("objects_move_to_layer")
  {
    SECTION("explicit ids")
    {
      mdl::selectNodes(map, {westWall});

      const auto moved = fixture.call(
        "objects_move_to_layer",
        Json{{"ids", {floorId, playerStartId, pillarsId}}, {"layer", arenaId}});
      CHECK(moved["undoStep"] == "AI: Move Objects to Layer");
      CHECK(resultOf(moved)["layer"] == arenaId);
      CHECK(resultOf(moved)["moved"] == Json::array({floorId, playerStartId, pillarsId}));
      CHECK(mdl::findContainingLayer(floorA) == arena);
      CHECK(mdl::findContainingLayer(playerStart) == arena);
      CHECK(mdl::findContainingLayer(pillars) == arena);
      CHECK(mdl::findContainingLayer(westWall) == defaultLayer);

      // like the editor, the moved objects are left selected
      auto selected = selectedIds(fixture, map);
      std::ranges::sort(selected);
      auto expected = std::vector<std::string>{floorId, playerStartId, pillarsId};
      std::ranges::sort(expected);
      CHECK(selected == expected);

      fixture.call("undo");
      CHECK(mdl::findContainingLayer(floorA) == defaultLayer);
      CHECK(mdl::findContainingLayer(pillars) == defaultLayer);
    }

    SECTION("the selection")
    {
      mdl::selectNodes(map, {ogre});
      const auto moved =
        fixture.call("objects_move_to_layer", Json{{"layer", "layer:default"}});
      CHECK(resultOf(moved)["moved"] == Json::array({ogreId}));
      CHECK(mdl::findContainingLayer(ogre) == defaultLayer);
    }

    SECTION("a brush of a brush entity moves the entity")
    {
      const auto moved = fixture.call(
        "objects_move_to_layer",
        Json{{"ids", {doorBrushId}}, {"layer", "layer:default"}});
      CHECK(resultOf(moved)["moved"] == Json::array({doorId}));
      CHECK(mdl::findContainingLayer(door) == defaultLayer);
      CHECK(doorBrush->parent() == door);
    }

    SECTION("into a hidden layer")
    {
      mdl::hideLayers(map, {arena});
      const auto moved = fixture.call(
        "objects_move_to_layer", Json{{"ids", {floorId}}, {"layer", arenaId}});
      CHECK(hasWarning(moved, "LAYER_HIDDEN"));
      CHECK(!editorContext.visible(*floorA));
      CHECK(map.selection().nodes.empty());
    }

    SECTION("reports only objects from other layers")
    {
      const auto moved = fixture.call(
        "objects_move_to_layer", Json{{"ids", {floorId, ogreId}}, {"layer", arenaId}});
      CHECK(resultOf(moved)["moved"] == Json::array({floorId}));
      CHECK(mdl::findContainingLayer(floorA) == arena);
      CHECK(mdl::findContainingLayer(ogre) == arena);
    }

    SECTION("already in the layer")
    {
      const auto moved = fixture.call(
        "objects_move_to_layer", Json{{"ids", {floorId}}, {"layer", "layer:default"}});
      CHECK(hasWarning(moved, "NO_CHANGE"));
      CHECK(moved["undoStep"].is_null());
    }

    SECTION("dry run")
    {
      fixture.call(
        "objects_move_to_layer",
        Json{{"ids", {floorId}}, {"layer", arenaId}, {"dryRun", true}});
      CHECK(mdl::findContainingLayer(floorA) == defaultLayer);
    }

    SECTION("invalid input")
    {
      const auto grouped = fixture.callExpectingError(
        "objects_move_to_layer", Json{{"ids", {pillarId}}, {"layer", arenaId}});
      CHECK(grouped.code == ErrorCode::InvalidArgument);
      CHECK(grouped.hint.find(pillarsId) != std::string::npos);

      CHECK(
        fixture.callExpectingError("objects_move_to_layer", Json{{"layer", arenaId}}).code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "objects_move_to_layer", Json{{"ids", {arenaId}}, {"layer", "layer:default"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "objects_move_to_layer", Json{{"ids", {floorId}}, {"layer", floorId}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("visibility_set")
  {
    SECTION("hide explicit ids")
    {
      mdl::selectNodes(map, {floorA, westWall});
      const auto hidden = fixture.call(
        "visibility_set", Json{{"mode", "hide"}, {"ids", {floorId, ogreId}}});
      CHECK(hidden["undoStep"] == "AI: Set Visibility");
      CHECK(resultOf(hidden)["ids"] == Json::array({floorId, ogreId}));
      CHECK(resultOf(hidden)["hiddenObjects"].get<size_t>() >= 2);
      CHECK(!editorContext.visible(*floorA));
      CHECK(!editorContext.visible(*ogre));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{westId});

      fixture.call("undo");
      CHECK(editorContext.visible(*floorA));
      CHECK(editorContext.visible(*ogre));
    }

    SECTION("hide the selection")
    {
      mdl::selectNodes(map, {floorA});
      fixture.call("visibility_set", Json{{"mode", "hide"}});
      CHECK(!editorContext.visible(*floorA));
      CHECK(map.selection().nodes.empty());
    }

    SECTION("show")
    {
      mdl::hideNodes(map, {floorA});
      const auto shown =
        fixture.call("visibility_set", Json{{"mode", "show"}, {"ids", {floorId}}});
      CHECK(editorContext.visible(*floorA));
      CHECK(resultOf(shown)["hiddenObjects"] == 0);

      // objects in a hidden layer are shown too
      mdl::hideLayers(map, {arena});
      fixture.call("visibility_set", Json{{"mode", "show"}, {"ids", {ogreId}}});
      CHECK(editorContext.visible(*ogre));
      CHECK(arena->hidden());
    }

    SECTION("isolate")
    {
      mdl::selectNodes(map, {floorA});
      const auto isolated = fixture.call("visibility_set", Json{{"mode", "isolate"}});
      CHECK(isolated["undoStep"] == "AI: Set Visibility");
      CHECK(editorContext.visible(*floorA));
      CHECK(!editorContext.visible(*westWall));
      CHECK(!editorContext.visible(*ogre));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{floorId});

      const auto explicitIds = fixture.call("visibility_set", Json{{"mode", "show_all"}});
      CHECK(resultOf(explicitIds)["hiddenObjects"] == 0);

      fixture.call("visibility_set", Json{{"mode", "isolate"}, {"ids", {ogreId}}});
      CHECK(editorContext.visible(*ogre));
      CHECK(!editorContext.visible(*floorA));
      CHECK(selectedIds(fixture, map) == std::vector<std::string>{ogreId});
    }

    SECTION("show_all")
    {
      mdl::hideNodes(map, {floorA, ogre});
      mdl::hideLayers(map, {arena});
      const auto shown = fixture.call("visibility_set", Json{{"mode", "show_all"}});
      CHECK(editorContext.visible(*floorA));
      CHECK(hasWarning(shown, "HIDDEN_LAYERS"));
      CHECK(arena->hidden());
      CHECK(!editorContext.visible(*ogre));

      fixture.call("undo");
      CHECK(!editorContext.visible(*floorA));
    }

    SECTION("dry run")
    {
      fixture.call(
        "visibility_set", Json{{"mode", "hide"}, {"ids", {floorId}}, {"dryRun", true}});
      CHECK(editorContext.visible(*floorA));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("visibility_set", Json{{"mode", "hide"}}).code
        == ErrorCode::NoSelection);
      CHECK(
        fixture.callExpectingError("visibility_set", Json{{"mode", "show"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "visibility_set", Json{{"mode", "show_all"}, {"ids", {floorId}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("visibility_set", Json{{"mode", "blink"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "visibility_set", Json{{"mode", "hide"}, {"ids", {arenaId}}})
          .code
        == ErrorCode::InvalidArgument);

      mdl::hideNodes(map, {floorA});
      CHECK(
        fixture
          .callExpectingError(
            "visibility_set", Json{{"mode", "isolate"}, {"ids", {floorId}}})
          .code
        == ErrorCode::ObjectNotEditable);
    }
  }
}

} // namespace tb::mcp
