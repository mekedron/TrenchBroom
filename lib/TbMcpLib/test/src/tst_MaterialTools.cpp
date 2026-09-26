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
#include "base/PreferenceManager.h"
#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "gl/Texture.h"
#include "mcp/McpToolFixture.h"
#include "mcp/Pagination.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Selection.h"
#include "mdl/NodeQueries.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include "kd/invoke.h"

#include "vm/vec.h"

#include <future>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

bool hasWarning(const Json& result, const std::string& code)
{
  return result.contains("warnings")
         && std::ranges::any_of(result["warnings"], [&](const auto& warning) {
              return warning["code"] == code;
            });
}

std::string materialsWad()
{
  return (getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad").string();
}

ui::MapDocument& newDocument(
  McpToolFixture& fixture, const std::string& game, const std::string& format = {})
{
  auto args = Json{{"game", game}};
  if (!format.empty())
  {
    args["format"] = format;
  }
  const auto result = fixture.call("document_new", args);
  const auto id = resultOf(result)["document"]["id"].get<std::string>();
  for (const auto& info : fixture.host().documentList)
  {
    if (info.id == id)
    {
      return *info.document;
    }
  }
  FAIL("document not found");
  return *fixture.host().documentList.front().document;
}

/** Loads and uploads all pending resources, like the editor does after loading. */
void processResources(mdl::Map& map)
{
  auto gl = gl::TestGl{};
  gl::processResourcesSync(
    map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
}

/** A Quake document with the materials of fixture/mcp/wads/materials.wad loaded. */
ui::MapDocument& quakeDocument(McpToolFixture& fixture, const std::string& format = {})
{
  auto& document = newDocument(fixture, "Quake", format);
  fixture.call("materials_collections_set", Json{{"wads", Json{materialsWad()}}});
  processResources(document.map());
  REQUIRE(document.map().materialManager().materials().size() == 6);
  return document;
}

mdl::BrushNode* createBox(
  McpToolFixture& fixture,
  mdl::Map& map,
  const Json& min,
  const Json& max,
  const std::string& material)
{
  const auto result = fixture.call(
    "brush_create_box", Json{{"min", min}, {"max", max}, {"material", material}});
  auto* brushNode = dynamic_cast<mdl::BrushNode*>(
    fixture.node(resultOf(result)["brush"].get<std::string>()));
  REQUIRE(brushNode);
  mdl::deselectAll(map);
  return brushNode;
}

std::vector<std::string> materialsOf(const mdl::BrushNode& brushNode)
{
  auto result = std::vector<std::string>{};
  for (const auto& face : brushNode.brush().faces())
  {
    result.push_back(face.materialName());
  }
  return result;
}

std::vector<std::string> repeat(const std::string& str, const size_t count)
{
  return std::vector<std::string>(count, str);
}

/** Gives all faces of the brush a distinctive alignment. */
void setAlignment(mdl::Map& map, mdl::BrushNode& brushNode)
{
  mdl::deselectAll(map);
  mdl::selectNodes(map, {&brushNode});
  REQUIRE(mdl::setBrushFaceAttributes(
    map,
    {
      .xOffset = mdl::SetValue{5.0f},
      .yOffset = mdl::SetValue{-7.0f},
      .rotation = mdl::SetValue{30.0f},
      .xScale = mdl::SetValue{0.5f},
      .yScale = mdl::SetValue{2.0f},
    }));
  mdl::deselectAll(map);
}

using Alignment = std::tuple<mdl::UvAttributes, vm::vec3d, vm::vec3d>;

std::vector<Alignment> alignmentOf(const mdl::BrushNode& brushNode)
{
  auto result = std::vector<Alignment>{};
  for (const auto& face : brushNode.brush().faces())
  {
    result.emplace_back(face.uvAttributes(), face.uAxis(), face.vAxis());
  }
  return result;
}

std::vector<std::string> sorted(const Json& array)
{
  auto result = array.get<std::vector<std::string>>();
  std::ranges::sort(result);
  return result;
}

std::vector<std::string> itemNames(const Json& items)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items)
  {
    result.push_back(item["name"].get<std::string>());
  }
  return result;
}

/** Reads the size from the IHDR chunk of a PNG file. */
std::tuple<size_t, size_t> pngSize(const std::string& png)
{
  REQUIRE(png.size() > 24);
  REQUIRE(png.substr(1, 3) == "PNG");
  const auto readInt = [&](const size_t offset) {
    auto value = size_t{0};
    for (size_t i = 0; i < 4; ++i)
    {
      value = (value << 8) | static_cast<unsigned char>(png[offset + i]);
    }
    return value;
  };
  return {readInt(16), readInt(20)};
}

} // namespace


TEST_CASE("MaterialTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  const auto& editorContext = document.map().editorContext();

  const auto alignmentLock = pref(Preferences::AlignmentLock);
  const auto uvLock = pref(Preferences::UvLock);
  const auto restorePreferences = kdl::invoke_later{[&]() {
    setPref(Preferences::AlignmentLock, alignmentLock);
    setPref(Preferences::UvLock, uvLock);
  }};

  SECTION("locks_get")
  {
    CHECK(
      fixture.call("locks_get")
      == Json{{"alignmentLock", alignmentLock}, {"uvLock", uvLock}});

    setPref(Preferences::UvLock, !uvLock);
    CHECK(fixture.call("locks_get")["uvLock"] == !uvLock);
  }

  SECTION("locks_set")
  {
    SECTION("alignment lock")
    {
      const auto result =
        fixture.call("locks_set", Json{{"alignmentLock", !alignmentLock}});
      CHECK(result["undoStep"].is_null());
      CHECK(result["result"]["alignmentLock"] == !alignmentLock);
      CHECK(result["result"]["uvLock"] == uvLock);
      CHECK(result["result"]["previous"]["alignmentLock"] == alignmentLock);
      CHECK(pref(Preferences::AlignmentLock) == !alignmentLock);
      CHECK(pref(Preferences::UvLock) == uvLock);
      // the document applies the preference to its editor context
      CHECK(editorContext.alignmentLock() == !alignmentLock);
    }

    SECTION("both")
    {
      fixture.call("locks_set", Json{{"alignmentLock", false}, {"uvLock", true}});
      CHECK_FALSE(pref(Preferences::AlignmentLock));
      CHECK(pref(Preferences::UvLock));
      CHECK_FALSE(editorContext.alignmentLock());
      CHECK(editorContext.uvLock());
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "locks_set", Json{{"alignmentLock", !alignmentLock}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["alignmentLock"] == !alignmentLock);
      CHECK(pref(Preferences::AlignmentLock) == alignmentLock);
      CHECK(editorContext.alignmentLock() == alignmentLock);
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("locks_set").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("locks_set", Json{{"uvLock", "yes"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("materials_list")
  {
    auto& quake = quakeDocument(fixture);
    auto& map = quake.map();
    createBox(fixture, map, {0, 0, 0}, {64, 64, 64}, "wall_old_a");

    SECTION("lists the loaded materials sorted by name")
    {
      const auto result = fixture.call("materials_list");
      CHECK(result["total"] == 6);
      CHECK(
        itemNames(result["items"])
        == std::vector<std::string>{
          "floor_tile",
          "wall_new_a",
          "wall_new_b",
          "wall_old_a",
          "wall_old_b",
          "wall_old_c"});
      CHECK(
        result["items"][0]
        == Json{
          {"name", "floor_tile"},
          {"collection", "materials.wad"},
          {"width", 32},
          {"height", 16},
          {"usage", 0},
        });
      CHECK(result["items"][3]["usage"] == 6);
      CHECK(result["missingCount"] == 0);
      CHECK(result["nextCursor"].is_null());
    }

    SECTION("filters")
    {
      CHECK(
        itemNames(fixture.call("materials_list", Json{{"search", "OLD"}})["items"])
        == std::vector<std::string>{"wall_old_a", "wall_old_b", "wall_old_c"});
      CHECK(
        itemNames(fixture.call("materials_list", Json{{"search", "wall_new_?"}})["items"])
        == std::vector<std::string>{"wall_new_a", "wall_new_b"});
      CHECK(fixture.call("materials_list", Json{{"search", "*tile"}})["total"] == 1);
      CHECK(
        fixture.call("materials_list", Json{{"collection", "MATERIALS*"}})["total"] == 6);
      CHECK(fixture.call("materials_list", Json{{"collection", "other"}})["total"] == 0);
      CHECK(
        itemNames(fixture.call("materials_list", Json{{"usedOnly", true}})["items"])
        == std::vector<std::string>{"wall_old_a"});
    }

    SECTION("materials the map uses that are not loaded")
    {
      createBox(fixture, map, {128, 0, 0}, {192, 64, 64}, "no_such_material");
      auto result = fixture.call("materials_list");
      CHECK(result["total"] == 6);
      CHECK(result["missingCount"] == 1);

      result = fixture.call("materials_list", Json{{"includeMissing", true}});
      CHECK(result["total"] == 7);
      CHECK(
        result["items"][6]
        == Json{
          {"name", "no_such_material"},
          {"collection", nullptr},
          {"width", nullptr},
          {"height", nullptr},
          {"usage", 6},
          {"missing", true},
        });
    }

    SECTION("pagination and detail")
    {
      const auto page = fixture.call("materials_list", Json{{"limit", 4}});
      CHECK(page["items"].size() == 4);
      REQUIRE(page["nextCursor"].is_string());
      const auto next = fixture.call(
        "materials_list", Json{{"limit", 4}, {"cursor", page["nextCursor"]}});
      CHECK(
        itemNames(next["items"]) == std::vector<std::string>{"wall_old_b", "wall_old_c"});

      const auto full = fixture.call(
        "materials_list", Json{{"search", "floor_tile"}, {"detail", "full"}});
      CHECK(full["items"][0]["path"] == "textures/floor_tile.D");
      CHECK(full["items"][0]["loaded"] == true);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("materials_list", Json{{"usedOnly", "yes"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("materials_list", Json{{"cursor", "garbage"}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("material_apply")
  {
    auto& quake = quakeDocument(fixture);
    auto& map = quake.map();
    auto* brush1 = createBox(fixture, map, {0, 0, 0}, {64, 64, 64}, "wall_old_a");
    auto* brush2 = createBox(fixture, map, {128, 0, 0}, {192, 64, 64}, "wall_old_a");

    SECTION("explicit ids: brushes and faces")
    {
      const auto faceId = fixture.id(*brush2) + "/face:2";
      const auto result = fixture.call(
        "material_apply",
        Json{{"material", "WALL_NEW_A"}, {"ids", {fixture.id(*brush1), faceId}}});
      CHECK(result["undoStep"] == "AI: Apply Material");
      CHECK(resultOf(result) == Json{{"material", "wall_new_a"}, {"faces", 7}});
      CHECK(materialsOf(*brush1) == repeat("wall_new_a", 6));
      CHECK(brush2->brush().face(2).materialName() == "wall_new_a");
      CHECK(brush2->brush().face(1).materialName() == "wall_old_a");
      CHECK(
        sorted(result["changes"]["modified"])
        == sorted(Json::array({fixture.id(*brush1), fixture.id(*brush2)})));
      CHECK(!hasWarning(result, "UNKNOWN_MATERIAL"));
      // the human's (empty) selection is restored
      CHECK(!map.selection().hasAny());
    }

    SECTION("the selection")
    {
      mdl::selectNodes(map, {brush2});
      const auto result =
        fixture.call("material_apply", Json{{"material", "floor_tile"}});
      CHECK(resultOf(result)["faces"] == 6);
      CHECK(materialsOf(*brush2) == repeat("floor_tile", 6));
      CHECK(materialsOf(*brush1) == repeat("wall_old_a", 6));
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brush2});
    }

    SECTION("keeps the alignment")
    {
      setAlignment(map, *brush1);
      const auto before = alignmentOf(*brush1);
      fixture.call(
        "material_apply",
        Json{{"material", "floor_tile"}, {"ids", {fixture.id(*brush1)}}});
      CHECK(alignmentOf(*brush1) == before);
    }

    SECTION("unknown materials are applied with a warning")
    {
      const auto result = fixture.call(
        "material_apply",
        Json{{"material", "no_such_material"}, {"ids", {fixture.id(*brush1)}}});
      CHECK(hasWarning(result, "UNKNOWN_MATERIAL"));
      CHECK(materialsOf(*brush1) == repeat("no_such_material", 6));
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "material_apply",
        Json{
          {"material", "floor_tile"}, {"ids", {fixture.id(*brush1)}}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(resultOf(result)["faces"] == 6);
      CHECK(materialsOf(*brush1) == repeat("wall_old_a", 6));
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("material_apply", Json{{"ids", {fixture.id(*brush1)}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("material_apply", Json{{"material", "floor_tile"}})
          .code
        == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "material_apply", Json{{"material", "floor_tile"}, {"ids", {"brush:999999"}}})
          .code
        == ErrorCode::ObjectNotFound);
    }
  }

  SECTION("material_set_current")
  {
    auto& quake = quakeDocument(fixture);
    auto& map = quake.map();
    const auto initial = map.currentMaterialName();

    SECTION("sets the current material")
    {
      const auto result =
        fixture.call("material_set_current", Json{{"material", "Floor_Tile"}});
      CHECK(result["undoStep"].is_null());
      CHECK(
        resultOf(result)
        == Json{{"material", "floor_tile"}, {"loaded", true}, {"previous", initial}});
      CHECK(map.currentMaterialName() == "floor_tile");

      // new brushes get it
      const auto box =
        fixture.call("brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {64, 64, 64}}});
      auto* brushNode = dynamic_cast<mdl::BrushNode*>(
        fixture.node(resultOf(box)["brush"].get<std::string>()));
      REQUIRE(brushNode);
      CHECK(materialsOf(*brushNode) == repeat("floor_tile", 6));
    }

    SECTION("unknown material")
    {
      const auto result =
        fixture.call("material_set_current", Json{{"material", "nope"}});
      CHECK(hasWarning(result, "UNKNOWN_MATERIAL"));
      CHECK(resultOf(result)["loaded"] == false);
      CHECK(map.currentMaterialName() == "nope");
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "material_set_current", Json{{"material", "floor_tile"}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(resultOf(result)["material"] == "floor_tile");
      CHECK(map.currentMaterialName() == initial);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("material_set_current").code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("material_set_current", Json{{"material", ""}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("material_replace")
  {
    const auto format = GENERATE(std::string{"Standard"}, std::string{"Valve"});
    CAPTURE(format);

    auto& quake = quakeDocument(fixture, format);
    auto& map = quake.map();
    auto* brushA = createBox(fixture, map, {0, 0, 0}, {64, 64, 64}, "wall_old_a");
    auto* brushB = createBox(fixture, map, {128, 0, 0}, {192, 64, 64}, "WALL_OLD_B");
    auto* brushC = createBox(fixture, map, {256, 0, 0}, {320, 64, 64}, "wall_old_c");
    auto* floor = createBox(fixture, map, {0, 128, 0}, {64, 192, 16}, "floor_tile");
    for (auto* brushNode : {brushA, brushB, brushC})
    {
      setAlignment(map, *brushNode);
    }
    // the new map may contain a brush without a material
    const auto mapFaces = mdl::collectBrushFaces({&map.worldNode()}).size();
    REQUIRE(mapFaces >= 24);
    const auto alignmentA = alignmentOf(*brushA);
    const auto alignmentB = alignmentOf(*brushB);

    SECTION("patterns fill the wildcards of the target")
    {
      const auto result = fixture.call(
        "material_replace", Json{{"from", "wall_old*"}, {"to", "wall_new*"}});
      CHECK(result["undoStep"] == "AI: Replace Materials");
      CHECK(
        resultOf(result)
        == Json{
          {"scope", {{"kind", "map"}, {"faces", mapFaces}}},
          {"replaced",
           {
             {{"from", "WALL_OLD_B"}, {"to", "wall_new_b"}, {"faces", 6}},
             {{"from", "wall_old_a"}, {"to", "wall_new_a"}, {"faces", 6}},
           }},
          {"totalFaces", 12},
          {"unmatched", {{{"from", "wall_old_c"}, {"to", "wall_new_c"}, {"faces", 6}}}},
          {"noMatch", Json::array()},
          {"skippedFaces", 0},
        });
      CHECK(materialsOf(*brushA) == repeat("wall_new_a", 6));
      CHECK(materialsOf(*brushB) == repeat("wall_new_b", 6));
      CHECK(materialsOf(*brushC) == repeat("wall_old_c", 6));
      CHECK(materialsOf(*floor) == repeat("floor_tile", 6));
      CHECK(alignmentOf(*brushA) == alignmentA);
      CHECK(alignmentOf(*brushB) == alignmentB);
      CHECK(
        sorted(result["changes"]["modified"])
        == sorted(Json::array({fixture.id(*brushA), fixture.id(*brushB)})));

      // one undo step reverts everything
      fixture.call("undo");
      CHECK(materialsOf(*brushA) == repeat("wall_old_a", 6));
      CHECK(materialsOf(*brushB) == repeat("WALL_OLD_B", 6));
    }

    SECTION("several rules and exact names")
    {
      const auto result = fixture.call(
        "material_replace",
        Json{
          {"rules",
           {
             {{"from", "wall_old_a"}, {"to", "floor_tile"}},
             {{"from", "wall_old_?"}, {"to", "wall_new_a"}},
             {{"from", "brick*"}, {"to", "wall_new_b"}},
           }},
          {"scope", "map"},
        });
      CHECK(resultOf(result)["totalFaces"] == 18);
      CHECK(resultOf(result)["noMatch"] == Json{"brick*"});
      CHECK(materialsOf(*brushA) == repeat("floor_tile", 6));
      CHECK(materialsOf(*brushB) == repeat("wall_new_a", 6));
      CHECK(materialsOf(*brushC) == repeat("wall_new_a", 6));
    }

    SECTION("missing targets can be applied explicitly")
    {
      const auto result = fixture.call(
        "material_replace",
        Json{
          {"from", "wall_old_c"}, {"to", "wall_new_c"}, {"allowMissingTargets", true}});
      CHECK(hasWarning(result, "UNKNOWN_MATERIAL"));
      CHECK(resultOf(result)["unmatched"] == Json::array());
      CHECK(resultOf(result)["totalFaces"] == 6);
      CHECK(materialsOf(*brushC) == repeat("wall_new_c", 6));
    }

    SECTION("defaults to the selection")
    {
      mdl::selectNodes(map, {brushA});
      const auto result = fixture.call(
        "material_replace", Json{{"from", "wall_old*"}, {"to", "wall_new*"}});
      CHECK(resultOf(result)["scope"] == Json{{"kind", "selection"}, {"faces", 6}});
      CHECK(resultOf(result)["totalFaces"] == 6);
      CHECK(materialsOf(*brushA) == repeat("wall_new_a", 6));
      CHECK(materialsOf(*brushB) == repeat("WALL_OLD_B", 6));
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{brushA});

      // scope "map" ignores the selection
      fixture.call(
        "material_replace",
        Json{{"from", "wall_old*"}, {"to", "wall_new*"}, {"scope", "map"}});
      CHECK(materialsOf(*brushB) == repeat("wall_new_b", 6));
    }

    SECTION("explicit ids")
    {
      const auto result = fixture.call(
        "material_replace",
        Json{
          {"from", "wall_old*"},
          {"to", "wall_new*"},
          {"ids", {fixture.id(*brushB) + "/face:0", fixture.id(*floor)}},
        });
      CHECK(resultOf(result)["scope"] == Json{{"kind", "ids"}, {"faces", 7}});
      CHECK(resultOf(result)["totalFaces"] == 1);
      CHECK(brushB->brush().face(0).materialName() == "wall_new_b");
      CHECK(brushB->brush().face(1).materialName() == "WALL_OLD_B");
    }

    SECTION("hidden faces are skipped")
    {
      mdl::hideNodes(map, {brushB});
      const auto result = fixture.call(
        "material_replace", Json{{"from", "wall_old*"}, {"to", "wall_new*"}});
      CHECK(resultOf(result)["skippedFaces"] == 6);
      CHECK(resultOf(result)["totalFaces"] == 6);
      CHECK(materialsOf(*brushB) == repeat("WALL_OLD_B", 6));
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "material_replace",
        Json{{"from", "wall_old*"}, {"to", "wall_new*"}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(resultOf(result)["totalFaces"] == 12);
      CHECK(materialsOf(*brushA) == repeat("wall_old_a", 6));
    }

    SECTION("invalid input")
    {
      const auto errorCode = [&](const Json& args) {
        return fixture.callExpectingError("material_replace", args).code;
      };
      CHECK(errorCode(Json::object()) == ErrorCode::InvalidArgument);
      CHECK(errorCode(Json{{"from", "a"}}) == ErrorCode::InvalidArgument);
      CHECK(
        errorCode(
          Json{{"from", "a"}, {"to", "b"}, {"rules", {{{"from", "c"}, {"to", "d"}}}}})
        == ErrorCode::InvalidArgument);
      CHECK(
        errorCode(Json{{"from", "wall_*"}, {"to", "*_*"}}) == ErrorCode::InvalidArgument);
      CHECK(
        errorCode(Json{{"from", "a"}, {"to", "b"}, {"scope", "map"}, {"layer", "x"}})
        == ErrorCode::InvalidArgument);
      CHECK(
        errorCode(Json{{"from", "a"}, {"to", "b"}, {"layer", "x"}})
        == ErrorCode::ObjectNotFound);
      CHECK(
        errorCode(Json{{"from", "a"}, {"to", "b"}, {"scope", "selection"}})
        == ErrorCode::NoSelection);
      CHECK(
        errorCode(Json{{"from", "a"}, {"to", "b"}, {"scope", "everything"}})
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("material_preview")
  {
    SECTION("reloads the image when the editor dropped it")
    {
      auto& quake = quakeDocument(fixture);
      const auto* material = quake.map().materialManager().material("floor_tile");
      REQUIRE(material->texture());
      REQUIRE(material->texture()->buffersIfLoaded().empty());

      const auto raw = fixture.callRaw(
        "material_preview", Json{{"material", "FLOOR_TILE"}, {"maxSize", 8}});
      CHECK(raw["isError"] == false);
      const auto& result = raw["structuredContent"];
      CHECK(result["name"] == "floor_tile");
      CHECK(result["collection"] == "materials.wad");
      CHECK(result["width"] == 32);
      CHECK(result["height"] == 16);
      CHECK(result["previewWidth"] == 8);
      CHECK(result["previewHeight"] == 4);
      CHECK(result["source"] == "file");
      CHECK(result["averageColor"].get<std::string>().size() == 7);

      REQUIRE(raw["content"].size() == 2);
      CHECK(raw["content"][1]["type"] == "image");
      CHECK(raw["content"][1]["mimeType"] == "image/png");
      const auto png = base64Decode(raw["content"][1]["data"].get<std::string>());
      REQUIRE(png);
      CHECK(pngSize(*png) == std::tuple<size_t, size_t>{8, 4});
    }

    SECTION("uses the image in memory")
    {
      auto& quake = newDocument(fixture, "Quake");
      fixture.call("materials_collections_set", Json{{"wads", Json{materialsWad()}}});

      // load the textures without uploading them
      auto& resourceManager = quake.map().resourceManager();
      auto gl = gl::TestGl{};
      const auto processContext = gl::ProcessContext{gl, [](auto, auto) {}};
      const auto runTask = [](auto task) {
        auto promise = std::promise<std::unique_ptr<gl::TaskResult>>{};
        promise.set_value(task());
        return promise.get_future();
      };
      resourceManager.process(runTask, processContext);
      resourceManager.process(runTask, processContext);
      const auto* material = quake.map().materialManager().material("floor_tile");
      REQUIRE(material->texture());
      REQUIRE(!material->texture()->buffersIfLoaded().empty());

      const auto inMemory =
        fixture.call("material_preview", Json{{"material", "floor_tile"}});
      CHECK(inMemory["source"] == "memory");
      CHECK(inMemory["previewWidth"] == 32);
      CHECK(inMemory["previewHeight"] == 16);

      processResources(quake.map());
      const auto fromFile =
        fixture.call("material_preview", Json{{"material", "floor_tile"}});
      CHECK(fromFile["source"] == "file");
      CHECK(fromFile["averageColor"] == inMemory["averageColor"]);
    }

    SECTION("Quake 2 textures")
    {
      newDocument(fixture, "Quake 2");
      const auto result = fixture.call(
        "material_preview", Json{{"material", "e1m1/b_pv_v1a2"}, {"maxSize", 1000}});
      CHECK(result["source"] == "file");
      CHECK(result["previewWidth"] == result["width"]);
      CHECK(result["previewHeight"] == result["height"]);
    }

    SECTION("invalid input")
    {
      quakeDocument(fixture);
      const auto error =
        fixture.callExpectingError("material_preview", Json{{"material", "wall_old"}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.hint.find("wall_old_a") != std::string::npos);
      CHECK(error.hint.find("materials_list") != std::string::npos);
      CHECK(
        fixture
          .callExpectingError(
            "material_preview", Json{{"material", "floor_tile"}, {"maxSize", 0}})
          .code
        == ErrorCode::InvalidArgument);
    }
  }
}

} // namespace tb::mcp
