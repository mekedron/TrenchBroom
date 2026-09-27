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
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <filesystem>
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

mdl::BrushNode* addBox(
  mdl::Map& map, const vm::bbox3d& bounds, const std::string& material = "stone")
{
  auto brush = mdl::BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()}
                 .createCuboid(bounds, material)
                 .value();
  auto* brushNode = new mdl::BrushNode{std::move(brush)};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  mdl::deselectAll(map);
  return brushNode;
}

mdl::EntityNode* addPlayerStart(mdl::Map& map)
{
  auto* entityNode = new mdl::EntityNode{mdl::Entity{{
    {"classname", "info_player_start"},
    {"origin", "256 0 32"},
  }}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});
  mdl::deselectAll(map);
  return entityNode;
}

size_t countNodes(const mdl::Node& node, const auto& predicate)
{
  auto count = size_t(predicate(node) ? 1 : 0);
  for (const auto* child : node.children())
  {
    count += countNodes(*child, predicate);
  }
  return count;
}

size_t brushCount(const mdl::Map& map)
{
  return countNodes(map.worldNode(), [](const auto& node) {
    return dynamic_cast<const mdl::BrushNode*>(&node) != nullptr;
  });
}

size_t entityCount(const mdl::Map& map)
{
  return countNodes(map.worldNode(), [](const auto& node) {
    return dynamic_cast<const mdl::EntityNode*>(&node) != nullptr;
  });
}

size_t groupCount(const mdl::Map& map)
{
  return countNodes(map.worldNode(), [](const auto& node) {
    return dynamic_cast<const mdl::GroupNode*>(&node) != nullptr;
  });
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(
    result["warnings"], [&](const auto& warning) { return warning["code"] == code; });
}

Json box(const vm::vec3d& min, const vm::vec3d& max)
{
  return Json{{"min", {min.x(), min.y(), min.z()}}, {"max", {max.x(), max.y(), max.z()}}};
}

std::filesystem::path fixtureMap(const std::string& name)
{
  return getFixtureRoot() / "test" / "mcp" / "maps" / name;
}

std::string roomsMap()
{
  return fixtureMap("rooms.map").string();
}

mdl::Map& newDocument(McpToolFixture& fixture, const Json& args)
{
  fixture.call("document_new", args);
  auto& map = fixture.host().documentList.back().document->map();

  // remove the default brush of a new map
  mdl::selectAllNodes(map);
  mdl::removeSelectedNodes(map);
  return map;
}

void loadMaterials(McpToolFixture& fixture, mdl::Map& map)
{
  // the WAD has wall_old_a/b/c, wall_new_a/b and floor_tile
  const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad";
  fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

std::vector<std::string> names(const Json& items, const std::string& key = "name")
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item[key].get<std::string>());
  }
  return result;
}

} // namespace

TEST_CASE("ClipboardTools clipboard_copy")
{
  auto fixture = McpToolFixture{};
  auto& map = fixture.create().map();

  auto* brushA = addBox(map, {{0, 0, 0}, {64, 64, 64}});
  auto* brushB = addBox(map, {{128, 0, 0}, {192, 64, 64}}, "brick");
  auto* playerStart = addPlayerStart(map);
  const auto idA = fixture.id(*brushA);
  const auto idB = fixture.id(*brushB);
  const auto idStart = fixture.id(*playerStart);

  SECTION("explicit ids")
  {
    const auto result = fixture.call("clipboard_copy", Json{{"ids", {idA, idStart}}});
    CHECK(result["mode"] == "objects");
    CHECK(result["ids"] == Json{idA, idStart});
    CHECK(result["count"] == 2);
    const auto text = result["text"].get<std::string>();
    CHECK(text.find("stone") != std::string::npos);
    CHECK(text.find("info_player_start") != std::string::npos);
    CHECK(text.find("brick") == std::string::npos);
    CHECK(result["bytes"] == text.size());
    CHECK(result["lineCount"].get<size_t>() > 10);

    // the map and the selection are unchanged
    CHECK(brushCount(map) == 2);
    CHECK(map.selection().nodes.empty());
  }

  SECTION("the selection")
  {
    mdl::selectNodes(map, {brushB});
    const auto result = fixture.call("clipboard_copy");
    CHECK(result["mode"] == "objects");
    CHECK(result["ids"] == Json{idB});
    CHECK(result["text"].get<std::string>().find("brick") != std::string::npos);
    CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushB});
  }

  SECTION("faces")
  {
    const auto result =
      fixture.call("clipboard_copy", Json{{"faces", {idB + "/face:0"}}});
    CHECK(result["mode"] == "faces");
    CHECK(result["ids"] == Json{idB + "/face:0"});
    const auto text = result["text"].get<std::string>();
    CHECK(text.starts_with("("));
    CHECK(text.find("brick") != std::string::npos);
    CHECK(result["lineCount"] == 1);
  }

  SECTION("selected faces")
  {
    mdl::selectBrushFaces(map, {{brushA, 1}, {brushA, 2}});
    const auto result = fixture.call("clipboard_copy");
    CHECK(result["mode"] == "faces");
    CHECK(result["ids"] == Json{idA + "/face:1", idA + "/face:2"});
    CHECK(result["lineCount"] == 2);
  }

  SECTION("without the text")
  {
    const auto result =
      fixture.call("clipboard_copy", Json{{"ids", {idA}}, {"includeText", false}});
    CHECK(!result.contains("text"));
    CHECK(result["bytes"].get<size_t>() > 0);
  }

  SECTION("fills the server's clipboard")
  {
    fixture.call("clipboard_copy", Json{{"ids", {idB}}});
    const auto pasted = fixture.call("clipboard_paste");
    CHECK(resultOf(pasted)["bounds"] == box({128, 0, 0}, {192, 64, 64}));
    CHECK(brushCount(map) == 3);
  }

  SECTION("invalid input")
  {
    CHECK(fixture.callExpectingError("clipboard_copy").code == ErrorCode::NoSelection);
    CHECK(
      fixture.callExpectingError("clipboard_copy", Json{{"ids", {"brush:999999"}}}).code
      == ErrorCode::ObjectNotFound);
    CHECK(
      fixture
        .callExpectingError(
          "clipboard_copy", Json{{"ids", {idA}}, {"faces", {idB + "/face:0"}}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("clipboard_copy", Json{{"ids", {"world"}}}).code
      == ErrorCode::InvalidArgument);
  }
}

TEST_CASE("ClipboardTools clipboard_cut")
{
  auto fixture = McpToolFixture{};
  auto& map = fixture.create().map();

  auto* brushA = addBox(map, {{0, 0, 0}, {64, 64, 64}});
  auto* brushB = addBox(map, {{128, 0, 0}, {192, 64, 64}}, "brick");
  const auto idA = fixture.id(*brushA);
  const auto idB = fixture.id(*brushB);

  SECTION("explicit ids, then paste and undo")
  {
    const auto cut = fixture.call("clipboard_cut", Json{{"ids", {idA}}});
    CHECK(cut["undoStep"] == "AI: Cut");
    CHECK(resultOf(cut)["mode"] == "objects");
    CHECK(resultOf(cut)["ids"] == Json{idA});
    CHECK(resultOf(cut)["text"].get<std::string>().find("stone") != std::string::npos);
    CHECK(cut["changes"]["removed"] == Json{idA});
    CHECK(brushCount(map) == 1);

    // the cut objects come back at their position
    const auto pasted = fixture.call("clipboard_paste");
    CHECK(resultOf(pasted)["bounds"] == box({0, 0, 0}, {64, 64, 64}));
    CHECK(brushCount(map) == 2);

    // one undo step each
    fixture.call("undo");
    fixture.call("undo");
    CHECK(brushCount(map) == 2);
    CHECK(fixture.node(idA) == brushA);
    CHECK(
      fixture.call("history_get", Json{{"limit", 2}})["redo"][0]["name"] == "AI: Cut");
  }

  SECTION("the selection")
  {
    mdl::selectNodes(map, {brushB});
    const auto cut = fixture.call("clipboard_cut");
    CHECK(resultOf(cut)["ids"] == Json{idB});
    CHECK(brushCount(map) == 1);
    CHECK(map.selection().nodes.empty());
  }

  SECTION("dry run leaves the map and the clipboard unchanged")
  {
    fixture.call("clipboard_copy", Json{{"ids", {idB}}});
    const auto dryRun =
      fixture.call("clipboard_cut", Json{{"ids", {idA}}, {"dryRun", true}});
    CHECK(dryRun["dryRun"] == true);
    CHECK(dryRun["changes"]["removed"] == Json{idA});
    CHECK(brushCount(map) == 2);

    const auto pasted = fixture.call("clipboard_paste");
    CHECK(resultOf(pasted)["bounds"] == box({128, 0, 0}, {192, 64, 64}));
  }

  SECTION("invalid input")
  {
    CHECK(fixture.callExpectingError("clipboard_cut").code == ErrorCode::NoSelection);
    CHECK(
      fixture.callExpectingError("clipboard_cut", Json{{"ids", {"brush:999999"}}}).code
      == ErrorCode::ObjectNotFound);
    CHECK(brushCount(map) == 2);
  }
}

TEST_CASE("ClipboardTools clipboard_paste")
{
  auto fixture = McpToolFixture{};
  auto& map = fixture.create().map();

  auto* brushA = addBox(map, {{0, 0, 0}, {64, 64, 64}});
  auto* brushB = addBox(map, {{128, 0, 0}, {192, 64, 32}}, "brick");
  const auto idA = fixture.id(*brushA);
  const auto idB = fixture.id(*brushB);

  SECTION("empty clipboard")
  {
    CHECK(
      fixture.callExpectingError("clipboard_paste").code == ErrorCode::InvalidArgument);
  }

  SECTION("objects")
  {
    fixture.call("clipboard_copy", Json{{"ids", {idA}}});

    SECTION("at the original position")
    {
      const auto pasted = fixture.call("clipboard_paste");
      CHECK(pasted["undoStep"] == "AI: Paste");
      const auto& result = resultOf(pasted);
      CHECK(result["pasteType"] == "objects");
      CHECK(result["count"] == 1);
      CHECK(result["bounds"] == box({0, 0, 0}, {64, 64, 64}));
      CHECK(result["offset"] == Json{0, 0, 0});
      CHECK(result["placement"]["mode"] == "original");
      CHECK(result["layer"] == "layer:default");
      CHECK(pasted["changes"]["created"] == result["ids"]);
      CHECK(brushCount(map) == 3);

      // the new objects are selected
      CHECK(pasted["selection"]["ids"] == result["ids"]);

      // no material collection has "stone"
      CHECK(result["missingMaterials"] == Json{"stone"});
      CHECK(hasWarning(pasted, "MISSING_MATERIALS"));

      // one undo step
      fixture.call("undo");
      CHECK(brushCount(map) == 2);
    }

    SECTION("at a point")
    {
      const auto atMin =
        fixture.call("clipboard_paste", Json{{"position", {512, 256, 0}}});
      CHECK(resultOf(atMin)["bounds"] == box({512, 256, 0}, {576, 320, 64}));
      CHECK(resultOf(atMin)["offset"] == Json{512, 256, 0});

      const auto atCenter = fixture.call(
        "clipboard_paste", Json{{"position", {0, 512, 0}}, {"anchor", "center"}});
      CHECK(resultOf(atCenter)["bounds"] == box({-32, 480, -32}, {32, 544, 32}));

      const auto atBottom = fixture.call(
        "clipboard_paste", Json{{"position", {0, 1024, 16}}, {"anchor", "bottomCenter"}});
      CHECK(resultOf(atBottom)["bounds"] == box({-32, 992, 16}, {32, 1056, 80}));

      const auto atMax =
        fixture.call("clipboard_paste", Json{{"position", {0, 0, 0}}, {"anchor", "max"}});
      CHECK(resultOf(atMax)["bounds"] == box({-64, -64, -64}, {0, 0, 0}));
    }

    SECTION("at a point snapped to the grid")
    {
      const auto pasted = fixture.call(
        "clipboard_paste", Json{{"position", {3, 250, 0}}, {"snapToGrid", true}});
      CHECK(resultOf(pasted)["bounds"] == box({0, 256, 0}, {64, 320, 64}));
    }

    SECTION("by an offset")
    {
      const auto pasted = fixture.call("clipboard_paste", Json{{"offset", {0, 0, 64}}});
      CHECK(resultOf(pasted)["bounds"] == box({0, 0, 64}, {64, 64, 128}));
      CHECK(resultOf(pasted)["placement"]["mode"] == "offset");
    }

    SECTION("into a layer")
    {
      auto* layer = new mdl::LayerNode{mdl::Layer{"Props"}};
      mdl::addNodes(map, {{&map.worldNode(), {layer}}});
      const auto layerId = fixture.id(*layer);

      const auto pasted = fixture.call("clipboard_paste", Json{{"targetLayer", layerId}});
      CHECK(resultOf(pasted)["layer"] == layerId);
      REQUIRE(layer->children().size() == 1);
      CHECK(resultOf(pasted)["ids"] == Json{fixture.id(*layer->children().front())});
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call("clipboard_paste", Json{{"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(dryRun["changes"]["created"].size() == 1);
      CHECK(brushCount(map) == 2);
    }

    SECTION("invalid placement")
    {
      CHECK(
        fixture
          .callExpectingError(
            "clipboard_paste", Json{{"position", {0, 0, 0}}, {"offset", {0, 0, 64}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("clipboard_paste", Json{{"faces", {idB + "/face:0"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("clipboard_paste", Json{{"position", {9000, 0, 0}}})
          .code
        == ErrorCode::OutOfWorldBounds);
      CHECK(brushCount(map) == 2);
    }
  }

  SECTION("text")
  {
    const auto text =
      "// entity 0\n{\n\"classname\" \"light\"\n\"origin\" \"32 32 32\"\n}\n";
    const auto pasted = fixture.call("clipboard_paste", Json{{"text", text}});
    CHECK(resultOf(pasted)["pasteType"] == "objects");
    CHECK(resultOf(pasted)["objects"][0]["classname"] == "light");
    CHECK(entityCount(map) == 1);
  }

  SECTION("invalid text")
  {
    const auto error =
      fixture.callExpectingError("clipboard_paste", Json{{"text", "not a map {"}});
    CHECK(error.code == ErrorCode::OperationFailed);
    CHECK(brushCount(map) == 2);
  }

  SECTION("faces")
  {
    fixture.call("clipboard_copy", Json{{"faces", {idB + "/face:0"}}});

    SECTION("explicit faces")
    {
      const auto pasted = fixture.call(
        "clipboard_paste", Json{{"faces", {idA + "/face:1", idA + "/face:2"}}});
      const auto& result = resultOf(pasted);
      CHECK(result["pasteType"] == "faces");
      CHECK(result["faces"] == Json{idA + "/face:1", idA + "/face:2"});
      CHECK(result["material"] == "brick");
      CHECK(result["missingMaterials"] == Json{"brick"});
      CHECK(brushA->brush().face(0).materialName() == "stone");
      CHECK(brushA->brush().face(1).materialName() == "brick");
      CHECK(brushA->brush().face(2).materialName() == "brick");
      CHECK(pasted["changes"]["modified"] == Json{idA});
      CHECK(brushCount(map) == 2);

      // the selection is restored
      CHECK(map.selection().nodes.empty());
      CHECK(!map.selection().hasBrushFaces());
    }

    SECTION("selected faces")
    {
      mdl::selectBrushFaces(map, {{brushA, 3}});
      const auto pasted = fixture.call("clipboard_paste");
      CHECK(resultOf(pasted)["faces"] == Json{idA + "/face:3"});
      CHECK(brushA->brush().face(3).materialName() == "brick");
    }

    SECTION("a placement does not apply")
    {
      CHECK(
        fixture
          .callExpectingError(
            "clipboard_paste",
            Json{{"faces", {idA + "/face:1"}}, {"position", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("no target faces")
    {
      CHECK(fixture.callExpectingError("clipboard_paste").code == ErrorCode::NoSelection);
    }
  }
}

TEST_CASE("ClipboardTools map_file_inspect")
{
  auto fixture = McpToolFixture{};
  auto& map = newDocument(fixture, Json{{"game", "Quake"}, {"format", "Standard"}});

  SECTION("layers, groups and classnames")
  {
    loadMaterials(fixture, map);
    const auto result = fixture.call("map_file_inspect", Json{{"path", roomsMap()}});
    CHECK(result["format"] == "Valve");
    CHECK(result["formatSource"] == "header");
    CHECK(result["game"] == "Quake");
    CHECK(result["documentFormat"] == "Standard");
    CHECK(result["converted"] == true);
    CHECK(
      names(result["layers"])
      == std::vector<std::string>{"Default Layer", "Rooms", "Props"});
    CHECK(result["layers"][1]["index"] == 1);
    CHECK(result["layers"][1]["contents"]["groups"] == 2);
    CHECK(names(result["groups"]) == std::vector<std::string>{"Armory", "Storage"});

    const auto& armory = result["groups"][0];
    CHECK(armory["layer"] == "Rooms");
    CHECK(armory["parent"].is_null());
    CHECK(armory["bounds"] == box({0, 512, -16}, {192, 704, 128}));
    CHECK(armory["contents"]["brushes"] == 3);
    CHECK(armory["contents"]["entities"] == 1);
    CHECK(armory["materials"] == Json{"armory_metal", "wall_new_a"});

    CHECK(
      names(result["classnames"], "classname")
      == std::vector<std::string>{
        "func_detail", "info_player_start", "item_armor1", "light"});
    CHECK(result["contents"]["brushes"] == 6);
    CHECK(result["missingMaterials"] == Json{"armory_metal", "crate_wood"});

    // nothing changed
    CHECK(!result.contains("undoStep"));
    CHECK(brushCount(map) == 0);
  }

  SECTION("format detection")
  {
    const auto result = fixture.call(
      "map_file_inspect", Json{{"path", fixtureMap("no_header.map").string()}});
    CHECK(result["format"] == "Valve");
    CHECK(result["formatSource"] == "detected");
    CHECK(result["game"].is_null());
  }

  SECTION("invalid input")
  {
    CHECK(
      fixture.callExpectingError("map_file_inspect", Json{{"path", "maps/rooms.map"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "map_file_inspect", Json{{"path", fixtureMap("missing.map").string()}})
        .code
      == ErrorCode::IoError);

    // not a map file
    const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad";
    CHECK(
      fixture.callExpectingError("map_file_inspect", Json{{"path", wad.string()}}).code
      == ErrorCode::InvalidArgument);
  }
}

TEST_CASE("ClipboardTools map_import")
{
  auto fixture = McpToolFixture{};

  SECTION("into a Standard map")
  {
    auto& map = newDocument(fixture, Json{{"game", "Quake"}, {"format", "Standard"}});
    loadMaterials(fixture, map);

    SECTION("a group, converted from Valve")
    {
      const auto imported =
        fixture.call("map_import", Json{{"path", roomsMap()}, {"group", "Armory"}});
      CHECK(imported["undoStep"] == "AI: Import Map");
      const auto& result = resultOf(imported);
      CHECK(result["sourceFormat"] == "Valve");
      CHECK(result["documentFormat"] == "Standard");
      CHECK(result["converted"] == true);
      CHECK(result["sourceObjects"] == 1);
      REQUIRE(result["ids"].size() == 1);
      CHECK(result["objects"][0]["kind"] == "group");
      CHECK(result["objects"][0]["name"] == "Armory");
      CHECK(result["bounds"] == box({0, 512, -16}, {192, 704, 128}));
      CHECK(result["layer"] == "layer:default");
      CHECK(brushCount(map) == 3);
      CHECK(entityCount(map) == 1); // item_armor1

      // imported objects are selected
      CHECK(imported["selection"]["ids"] == result["ids"]);

      // missing materials are reported
      CHECK(result["missingMaterials"] == Json{"armory_metal"});
      CHECK(hasWarning(imported, "MISSING_MATERIALS"));

      // the brushes are in the Standard format: no UV axes
      const auto text = fixture.call("map_text_get", Json{{"ids", result["ids"]}});
      CHECK(text["text"].get<std::string>().find('[') == std::string::npos);

      // one undo step
      fixture.call("undo");
      CHECK(brushCount(map) == 0);
      CHECK(
        fixture.call("history_get", Json{{"limit", 1}})["redo"][0]["name"]
        == "AI: Import Map");
    }

    SECTION("placement")
    {
      const auto atPoint = fixture.call(
        "map_import",
        Json{{"path", roomsMap()}, {"group", "Armory"}, {"position", {1024, 0, 0}}});
      CHECK(resultOf(atPoint)["bounds"] == box({1024, 0, 0}, {1216, 192, 144}));

      const auto byOffset = fixture.call(
        "map_import",
        Json{{"path", roomsMap()}, {"group", "storage"}, {"offset", {0, 0, 64}}});
      CHECK(resultOf(byOffset)["bounds"] == box({512, 512, 64}, {576, 576, 128}));
    }

    SECTION("filters")
    {
      SECTION("a layer by name or index")
      {
        const auto byName =
          fixture.call("map_import", Json{{"path", roomsMap()}, {"layer", "Props"}});
        CHECK(resultOf(byName)["sourceObjects"] == 2);
        CHECK(brushCount(map) == 1);
        fixture.call("undo");

        const auto byIndex =
          fixture.call("map_import", Json{{"path", roomsMap()}, {"layer", 2}});
        CHECK(resultOf(byIndex)["sourceObjects"] == 2);
      }

      SECTION("a classname, also inside groups")
      {
        const auto result =
          fixture.call("map_import", Json{{"path", roomsMap()}, {"classname", "item_*"}});
        CHECK(resultOf(result)["sourceObjects"] == 1);
        CHECK(resultOf(result)["objects"][0]["classname"] == "item_armor1");
        CHECK(groupCount(map) == 0);
      }

      SECTION("world brushes of a layer")
      {
        const auto result = fixture.call(
          "map_import",
          Json{{"path", roomsMap()}, {"layer", 0}, {"classname", "worldspawn"}});
        CHECK(resultOf(result)["sourceObjects"] == 1);
        CHECK(resultOf(result)["bounds"] == box({0, 0, -16}, {256, 256, 0}));
      }

      SECTION("a region")
      {
        const auto intersecting = fixture.call(
          "map_import",
          Json{{"path", roomsMap()}, {"region", box({-8, -8, -8}, {300, 300, 100})}});
        // hall floor, player start, light, func_detail
        CHECK(resultOf(intersecting)["sourceObjects"] == 4);
        fixture.call("undo");

        const auto inside = fixture.call(
          "map_import",
          Json{
            {"path", roomsMap()},
            {"region", box({400, 400, -100}, {800, 800, 100})},
            {"regionMode", "inside"}});
        CHECK(resultOf(inside)["sourceObjects"] == 1);
        CHECK(resultOf(inside)["objects"][0]["name"] == "Storage");
      }
    }

    SECTION("into a target layer")
    {
      auto* layer = new mdl::LayerNode{mdl::Layer{"Imports"}};
      mdl::addNodes(map, {{&map.worldNode(), {layer}}});
      const auto layerId = fixture.id(*layer);

      const auto result = fixture.call(
        "map_import",
        Json{{"path", roomsMap()}, {"group", "Armory"}, {"targetLayer", layerId}});
      CHECK(resultOf(result)["layer"] == layerId);
      CHECK(layer->children().size() == 1);
    }

    SECTION("dry run")
    {
      const auto dryRun = fixture.call(
        "map_import", Json{{"path", roomsMap()}, {"group", "Armory"}, {"dryRun", true}});
      CHECK(dryRun["dryRun"] == true);
      CHECK(dryRun["changes"]["ephemeral"] == true);
      CHECK(resultOf(dryRun)["missingMaterials"] == Json{"armory_metal"});
      CHECK(brushCount(map) == 0);
    }

    SECTION("from Quake 2")
    {
      const auto imported = fixture.call(
        "map_import", Json{{"path", fixtureMap("crate_quake2.map").string()}});
      CHECK(resultOf(imported)["sourceFormat"] == "Quake2");
      CHECK(resultOf(imported)["converted"] == true);
      CHECK(resultOf(imported)["sourceObjects"] == 2);
      CHECK(hasWarning(imported, "GAME_MISMATCH"));
      CHECK(brushCount(map) == 1);
    }

    SECTION("a file without a format comment")
    {
      const auto imported =
        fixture.call("map_import", Json{{"path", fixtureMap("no_header.map").string()}});
      CHECK(resultOf(imported)["sourceFormat"] == "Valve");
      CHECK(resultOf(imported)["formatSource"] == "detected");
      CHECK(brushCount(map) == 1);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("map_import", Json{{"path", "rooms.map"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "map_import", Json{{"path", fixtureMap("missing.map").string()}})
          .code
        == ErrorCode::IoError);
      CHECK(
        fixture
          .callExpectingError(
            "map_import", Json{{"path", roomsMap()}, {"group", "Vault"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "map_import", Json{{"path", roomsMap()}, {"layer", "Attic"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "map_import",
            Json{
              {"path", roomsMap()}, {"region", box({5000, 5000, 0}, {5100, 5100, 10})}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(brushCount(map) == 0);
    }
  }

  SECTION("into a Valve map, converted from Standard")
  {
    auto& map = newDocument(fixture, Json{{"game", "Quake"}, {"format", "Valve"}});
    const auto imported =
      fixture.call("map_import", Json{{"path", fixtureMap("two_rooms.map").string()}});
    CHECK(resultOf(imported)["sourceFormat"] == "Standard");
    CHECK(resultOf(imported)["documentFormat"] == "Valve");
    CHECK(brushCount(map) > 0);

    const auto text =
      fixture.call("map_text_get", Json{{"ids", resultOf(imported)["ids"]}});
    CHECK(text["text"].get<std::string>().find('[') != std::string::npos);
  }

  SECTION("into a Quake 2 map, converted from Valve")
  {
    auto& map = newDocument(fixture, Json{{"game", "Quake 2"}});
    const auto imported =
      fixture.call("map_import", Json{{"path", roomsMap()}, {"group", "Storage"}});
    CHECK(resultOf(imported)["sourceFormat"] == "Valve");
    CHECK(resultOf(imported)["documentFormat"] == "Quake2");
    CHECK(brushCount(map) == 1);
  }
}

TEST_CASE("ClipboardTools replace")
{
  auto fixture = McpToolFixture{};
  auto& map = newDocument(fixture, Json{{"game", "Quake"}, {"format", "Standard"}});
  loadMaterials(fixture, map);

  const auto layerCount = [&]() { return map.worldNode().allLayers().size(); };

  SECTION("layer_replace")
  {
    // the first run creates the layer
    auto result = fixture.call(
      "layer_replace",
      Json{{"layer", "Generated"}, {"path", roomsMap()}, {"sourceLayer", "Props"}});
    CHECK(result["undoStep"] == "AI: Replace Layer");
    auto replaced = resultOf(result)["replaced"];
    CHECK(replaced["kind"] == "layer");
    CHECK(replaced["created"] == true);
    CHECK(replaced["removedObjects"] == 0);
    CHECK(replaced["importedObjects"] == 2);
    CHECK(replaced["address"] == "layer:@Generated");
    const auto layer = replaced["layer"].get<std::string>();
    CHECK(resultOf(result)["layer"] == layer);
    CHECK(layerCount() == 2);
    CHECK(brushCount(map) == 1);
    CHECK(entityCount(map) == 2); // light, func_detail

    // repeating it replaces the objects instead of duplicating them
    result = fixture.call(
      "layer_replace",
      Json{{"layer", "Generated"}, {"path", roomsMap()}, {"sourceLayer", "Props"}});
    replaced = resultOf(result)["replaced"];
    CHECK(replaced["created"] == false);
    CHECK(replaced["layer"] == layer);
    CHECK(replaced["removedObjects"] == 3); // light, func_detail and its brush
    CHECK(replaced["importedObjects"] == 2);
    CHECK(result["changes"]["removed"].size() == 3);
    CHECK(layerCount() == 2);
    CHECK(brushCount(map) == 1);
    CHECK(entityCount(map) == 2);

    // one undo step
    fixture.call("undo");
    CHECK(brushCount(map) == 1);
    CHECK(entityCount(map) == 2);
    CHECK(
      fixture.call("history_get", Json{{"limit", 1}})["redo"][0]["name"]
      == "AI: Replace Layer");
    fixture.call("redo");

    // by name address, another part of the file; a dry run changes nothing
    const auto dryRun = fixture.call(
      "layer_replace",
      Json{
        {"layer", "layer:@Generated"},
        {"path", roomsMap()},
        {"group", "Armory"},
        {"dryRun", true}});
    CHECK(resultOf(dryRun)["replaced"]["removedObjects"] == 3);
    CHECK(resultOf(dryRun)["replaced"]["importedObjects"] == 1);
    CHECK(brushCount(map) == 1);

    fixture.call(
      "layer_replace",
      Json{{"layer", "layer:@Generated"}, {"path", roomsMap()}, {"group", "Armory"}});
    CHECK(brushCount(map) == 3);
    CHECK(groupCount(map) == 1);

    // a failed import changes nothing
    CHECK(
      fixture
        .callExpectingError(
          "layer_replace",
          Json{{"layer", "Generated"}, {"path", roomsMap()}, {"group", "Nothing"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(brushCount(map) == 3);

    // a locked layer is refused
    fixture.call("layer_set_state", Json{{"layer", layer}, {"locked", true}});
    CHECK(
      fixture
        .callExpectingError(
          "layer_replace", Json{{"layer", "Generated"}, {"path", roomsMap()}})
        .code
      == ErrorCode::ObjectNotEditable);

    // name addresses must exist
    CHECK(
      fixture
        .callExpectingError(
          "layer_replace", Json{{"layer", "layer:@Missing"}, {"path", roomsMap()}})
        .code
      == ErrorCode::ObjectNotFound);
  }

  SECTION("map_import replaceGroup")
  {
    // the first run creates the group; an imported single group gets the name
    auto result = fixture.call(
      "map_import",
      Json{{"path", roomsMap()}, {"group", "Armory"}, {"replaceGroup", "Bar"}});
    auto replaced = resultOf(result)["replaced"];
    CHECK(replaced["kind"] == "group");
    CHECK(replaced["created"] == true);
    CHECK(replaced["address"] == "group:@Bar");
    CHECK(resultOf(result)["ids"] == Json{replaced["group"]});
    CHECK(groupCount(map) == 1);
    CHECK(brushCount(map) == 3);

    // repeating it replaces the group
    result = fixture.call(
      "map_import",
      Json{{"path", roomsMap()}, {"group", "Armory"}, {"replaceGroup", "Bar"}});
    replaced = resultOf(result)["replaced"];
    CHECK(replaced["created"] == false);
    CHECK(replaced["removedObjects"] == 5); // group, 3 brushes, item_armor1
    CHECK(groupCount(map) == 1);
    CHECK(brushCount(map) == 3);

    // several objects are grouped; the group stays inside its parent group
    const auto outer = resultOf(fixture.call(
      "group_create", Json{{"ids", {replaced["group"]}}, {"name", "Outer"}}))["group"];
    result = fixture.call(
      "map_import",
      Json{{"path", roomsMap()}, {"layer", "Props"}, {"replaceGroup", "group:@Bar"}});
    replaced = resultOf(result)["replaced"];
    CHECK(replaced["importedObjects"] == 2);
    CHECK(groupCount(map) == 2);
    CHECK(brushCount(map) == 1);
    auto* groupNode = fixture.node(replaced["group"].get<std::string>());
    REQUIRE(groupNode);
    CHECK(groupNode->parent() == fixture.node(outer.get<std::string>()));
    CHECK(groupNode->childCount() == 2);

    // invalid combinations and ambiguous names
    CHECK(
      fixture
        .callExpectingError(
          "map_import",
          Json{{"path", roomsMap()}, {"replaceGroup", "Bar"}, {"replaceLayer", "X"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError(
          "map_import",
          Json{
            {"path", roomsMap()},
            {"replaceGroup", "Bar"},
            {"targetLayer", "layer:default"}})
        .code
      == ErrorCode::InvalidArgument);
    fixture.call(
      "map_import",
      Json{{"path", roomsMap()}, {"group", "Storage"}, {"replaceGroup", "Outer2"}});
    fixture.call("group_rename", Json{{"ids", {"group:@Outer2"}}, {"name", "Bar"}});
    const auto ambiguous = fixture.callExpectingError(
      "map_import", Json{{"path", roomsMap()}, {"replaceGroup", "Bar"}});
    CHECK(ambiguous.code == ErrorCode::AmbiguousName);
    CHECK(ambiguous.details["candidates"].size() == 2);
  }
}

} // namespace tb::mcp
