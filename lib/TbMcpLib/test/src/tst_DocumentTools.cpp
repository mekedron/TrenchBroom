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
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mdl/BrushNode.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Layers.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const auto CubeMapPath = std::filesystem::path{"test/mdl/Game/Quake/id1/cube.map"};

std::filesystem::path cubeMapPath()
{
  return getFixtureRoot() / CubeMapPath;
}

void addBrush(ui::MapDocument& document)
{
  auto& map = document.map();
  auto* brushNode = mdl::createBrushNode(map);
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
}

size_t brushCount(ui::MapDocument& document)
{
  return document.map().worldNode().defaultLayer()->children().size();
}

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

} // namespace

TEST_CASE("DocumentTools")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};

  SECTION("document_new")
  {
    SECTION("creates a document with the given game and format")
    {
      const auto result =
        fixture.call("document_new", Json{{"game", "Quake"}, {"format", "Standard"}});
      const auto& document = resultOf(result)["document"];
      CHECK(result["undoStep"].is_null());
      CHECK(document["game"] == "Quake");
      CHECK(document["format"] == "Standard");
      CHECK(document["path"].is_null());
      CHECK(document["gamePathValid"] == true);
      CHECK(document["mods"]["default"] == "id1");
      CHECK(document["entityDefinitions"]["type"] == "builtin");
      CHECK(document["materials"]["mode"] == "wad");
      CHECK(resultOf(result)["initialMap"].is_null());
      CHECK(resultOf(result)["replaced"].is_null());

      REQUIRE(fixture.host().documentList.size() == 1);
      CHECK(Json(fixture.host().documentList[0].id) == document["id"]);
      CHECK(fixture.call("document_list")["activeDocument"] == document["id"]);
    }

    SECTION("defaults to the game's first format and matches names case-insensitively")
    {
      const auto result = fixture.call("document_new", Json{{"game", "quake 2"}});
      CHECK(resultOf(result)["document"]["game"] == "Quake 2");
      CHECK(resultOf(result)["document"]["format"] == "Quake2");
    }

    SECTION("invalid arguments")
    {
      CHECK(
        fixture.callExpectingError("document_new", Json{{"game", "Doom"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "document_new", Json{{"game", "Quake"}, {"format", "Quake3"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("document_new", Json::object()).code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("dry run")
    {
      const auto result =
        fixture.call("document_new", Json{{"game", "Quake"}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(resultOf(result)["wouldDo"] == "create a new Quake map in Valve format");
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("single window mode replaces the document")
    {
      fixture.host().singleWindow = true;
      auto& document = fixture.create();
      const auto documentId = fixture.documentId(document);
      addBrush(document);
      REQUIRE(document.map().modified());

      const auto error =
        fixture.callExpectingError("document_new", Json{{"game", "Quake"}});
      CHECK(error.code == ErrorCode::UnsavedChanges);

      const auto result = fixture.call(
        "document_new", Json{{"game", "Quake"}, {"unsavedChanges", "discard"}});
      CHECK(resultOf(result)["replaced"] == documentId);
      CHECK(resultOf(result)["document"]["id"] == documentId);
      CHECK(resultOf(result)["document"]["game"] == "Quake");
      CHECK(fixture.host().documentList.size() == 1);
      CHECK(!document.map().modified());
    }
  }

  SECTION("document_open")
  {
    SECTION("detects game and format from the file header")
    {
      const auto path = cubeMapPath();
      const auto result = fixture.call("document_open", Json{{"path", path.string()}});
      const auto& opened = resultOf(result);
      CHECK(opened["alreadyOpen"] == false);
      CHECK(opened["document"]["path"] == path.string());
      CHECK(opened["document"]["game"] == "Quake");
      CHECK(opened["document"]["format"] == "Standard");
      CHECK(opened["detection"]["gameSource"] == "fileHeader");
      CHECK(opened["detection"]["formatSource"] == "fileHeader");
      CHECK(opened["loadMessages"].is_array());
      CHECK(fixture.host().recentDocumentList == std::vector{path});
      CHECK(fixture.call("document_list")["activeDocument"] == opened["document"]["id"]);

      SECTION("opening the same file again returns the open document")
      {
        const auto again = fixture.call("document_open", Json{{"path", path.string()}});
        CHECK(resultOf(again)["alreadyOpen"] == true);
        CHECK(resultOf(again)["document"]["id"] == opened["document"]["id"]);
        CHECK(fixture.host().documentList.size() == 1);
      }
    }

    SECTION("uses the given game for files without header")
    {
      env.createFile("noheader.map", "{\n\"classname\" \"worldspawn\"\n}\n");
      const auto path = env.dir() / "noheader.map";

      const auto error =
        fixture.callExpectingError("document_open", Json{{"path", path.string()}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.hint.find("Quake") != std::string::npos);

      const auto result =
        fixture.call("document_open", Json{{"path", path.string()}, {"game", "Quake"}});
      CHECK(resultOf(result)["detection"]["gameSource"] == "argument");
      CHECK(resultOf(result)["detection"]["formatSource"] == "detected");
      CHECK(resultOf(result)["document"]["game"] == "Quake");
    }

    SECTION("invalid arguments")
    {
      CHECK(
        fixture.callExpectingError("document_open", Json{{"path", "maps/a.map"}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "document_open", Json{{"path", (env.dir() / "missing.map").string()}})
          .code
        == ErrorCode::IoError);
      CHECK(
        fixture
          .callExpectingError(
            "document_open", Json{{"path", cubeMapPath().string()}, {"format", "Quake2"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "document_open", Json{{"path", cubeMapPath().string()}, {"dryRun", true}});
      CHECK(resultOf(result)["wouldDo"].get<std::string>().starts_with("open "));
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("reports progress")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          1000,
          "tools/call",
          Json{
            {"name", "document_open"},
            {"arguments", Json{{"path", cubeMapPath().string()}}},
            {"_meta", Json{{"progressToken", "open-1"}}},
          }));

      // the map is loaded in a later step
      CHECK(!stream->response.has_value());
      CHECK(fixture.host().documentList.empty());

      fixture.scheduler().runPending();
      REQUIRE(stream->response.has_value());
      CHECK((*stream->response)["result"]["isError"] == false);
      CHECK(fixture.host().documentList.size() == 1);

      REQUIRE(stream->notifications.size() == 3);
      for (const auto& notification : stream->notifications)
      {
        CHECK(notification["method"] == "notifications/progress");
        CHECK(notification["params"]["progressToken"] == "open-1");
      }
      CHECK(stream->notifications.back()["params"]["progress"] == 3);
    }

    SECTION("can be cancelled before the map is loaded")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          1001,
          "tools/call",
          Json{
            {"name", "document_open"},
            {"arguments", Json{{"path", cubeMapPath().string()}}},
          }));
      REQUIRE(!stream->response.has_value());

      fixture.post(
        fixture.sessionId(),
        jsonrpc::makeNotification("notifications/cancelled", Json{{"requestId", 1001}}));
      fixture.scheduler().runPending();

      REQUIRE(stream->response.has_value());
      const auto& result = (*stream->response)["result"];
      CHECK(result["isError"] == true);
      CHECK(result["structuredContent"]["error"]["code"] == "CANCELLED");
      CHECK(fixture.host().documentList.empty());

      // the queue continues
      CHECK(fixture.call("document_list")["documents"].empty());
      fixture.call("document_new", Json{{"game", "Quake"}});
    }
  }

  SECTION("document_save and document_save_as")
  {
    auto& document = fixture.create();
    addBrush(document);

    CHECK(fixture.callExpectingError("document_save").code == ErrorCode::InvalidArgument);

    const auto path = env.dir() / "saved.map";

    SECTION("dry run")
    {
      const auto result =
        fixture.call("document_save_as", Json{{"path", path.string()}, {"dryRun", true}});
      CHECK(resultOf(result)["path"] == path.string());
      CHECK(!env.fileExists("saved.map"));
      CHECK(document.map().modified());
    }

    SECTION("save as, then save")
    {
      const auto result = fixture.call("document_save_as", Json{{"path", path.string()}});
      CHECK(resultOf(result)["path"] == path.string());
      CHECK(resultOf(result)["previousPath"].is_null());
      CHECK(resultOf(result)["overwritten"] == false);
      CHECK(env.fileExists("saved.map"));
      CHECK(!document.map().modified());
      CHECK(document.map().path() == path);

      addBrush(document);
      const auto saved = fixture.call("document_save");
      CHECK(resultOf(saved)["path"] == path.string());
      CHECK(!document.map().modified());

      // saving over its own file needs no overwrite flag
      fixture.call("document_save_as", Json{{"path", path.string()}});
    }

    SECTION("overwrite")
    {
      env.createFile("existing.map", "old");
      const auto existing = env.dir() / "existing.map";

      const auto error =
        fixture.callExpectingError("document_save_as", Json{{"path", existing.string()}});
      CHECK(error.code == ErrorCode::FileExists);
      CHECK(env.loadFile("existing.map") == "old");

      const auto result = fixture.call(
        "document_save_as", Json{{"path", existing.string()}, {"overwrite", true}});
      CHECK(resultOf(result)["overwritten"] == true);
      CHECK(env.loadFile("existing.map") != "old");
    }

    SECTION("invalid paths")
    {
      CHECK(
        fixture.callExpectingError("document_save_as", Json{{"path", "relative.map"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "document_save_as", Json{{"path", (env.dir() / "no/such/a.map").string()}})
          .code
        == ErrorCode::IoError);
      CHECK(
        fixture.callExpectingError("document_save_as", Json{{"path", env.dir().string()}})
          .code
        == ErrorCode::InvalidArgument);
    }

    SECTION("warns about unusual extensions")
    {
      const auto result = fixture.call(
        "document_save_as", Json{{"path", (env.dir() / "saved.txt").string()}});
      REQUIRE(result["warnings"].size() == 1);
      CHECK(result["warnings"][0]["code"] == "UNUSUAL_EXTENSION");
    }
  }

  SECTION("document_close")
  {
    auto& document = fixture.create();
    const auto documentId = fixture.documentId(document);

    SECTION("unmodified documents close without further arguments")
    {
      const auto result = fixture.call("document_close");
      CHECK(resultOf(result)["closed"] == documentId);
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("unsaved changes require a decision")
    {
      addBrush(document);

      CHECK(
        fixture.callExpectingError("document_close").code == ErrorCode::UnsavedChanges);
      // never saved, so it cannot be saved first
      CHECK(
        fixture.callExpectingError("document_close", Json{{"unsavedChanges", "save"}})
          .code
        == ErrorCode::UnsavedChanges);

      const auto dryRun = fixture.call(
        "document_close", Json{{"unsavedChanges", "discard"}, {"dryRun", true}});
      CHECK(resultOf(dryRun)["wouldDo"].get<std::string>().starts_with("close "));
      CHECK(fixture.host().documentList.size() == 1);

      const auto result =
        fixture.call("document_close", Json{{"unsavedChanges", "discard"}});
      CHECK(resultOf(result)["discardedChanges"] == true);
      CHECK(resultOf(result)["saved"] == false);
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("saves first if asked to")
    {
      const auto path = env.dir() / "close.map";
      fixture.call("document_save_as", Json{{"path", path.string()}});
      addBrush(document);

      const auto result =
        fixture.call("document_close", Json{{"unsavedChanges", "save"}});
      CHECK(resultOf(result)["saved"] == true);
      CHECK(!document.map().modified());
      CHECK(fixture.host().documentList.empty());
    }

    SECTION("refuses while a compilation is running")
    {
      fixture.host().compileRunning = true;
      CHECK(
        fixture.callExpectingError("document_close").code == ErrorCode::OperationFailed);
      CHECK(fixture.host().documentList.size() == 1);
    }
  }

  SECTION("document_revert")
  {
    auto& document = fixture.create();

    CHECK(
      fixture.callExpectingError("document_revert").code == ErrorCode::InvalidArgument);

    const auto path = env.dir() / "revert.map";
    fixture.call("document_save_as", Json{{"path", path.string()}});
    const auto brushesBefore = brushCount(document);
    addBrush(document);

    CHECK(
      fixture.callExpectingError("document_revert").code == ErrorCode::UnsavedChanges);
    CHECK(brushCount(document) == brushesBefore + 1);

    const auto dryRun = fixture.call(
      "document_revert", Json{{"unsavedChanges", "discard"}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["idsInvalidated"] == true);
    CHECK(brushCount(document) == brushesBefore + 1);

    const auto result =
      fixture.call("document_revert", Json{{"unsavedChanges", "discard"}});
    CHECK(resultOf(result)["idsInvalidated"] == true);
    CHECK(resultOf(result)["document"]["modified"] == false);
    CHECK(brushCount(document) == brushesBefore);
    CHECK(!document.map().modified());
  }

  SECTION("document_recent")
  {
    fixture.host().recentDocumentList = {cubeMapPath(), env.dir() / "gone.map"};

    const auto result = fixture.call("document_recent");
    REQUIRE(result["items"].size() == 2);
    CHECK(result["items"][0]["path"] == cubeMapPath().string());
    CHECK(result["items"][0]["exists"] == true);
    CHECK(result["items"][0]["openAs"].is_null());
    CHECK(result["items"][1]["exists"] == false);
  }

  SECTION("map_files_list")
  {
    env.createFile("b.map", "");
    env.createFile("A.MAP", "");
    env.createFile("notes.txt", "");
    env.createDirectory("sub");
    env.createFile("sub/c.map", "");

    const auto folder = env.dir().string();
    const auto names = [](const Json& page) {
      auto result = std::vector<std::string>{};
      for (const auto& item : page["items"])
      {
        result.push_back(item["name"].get<std::string>());
      }
      return result;
    };

    CHECK(
      names(fixture.call("map_files_list", Json{{"folder", folder}}))
      == std::vector<std::string>{"A.MAP", "b.map"});
    CHECK(
      names(fixture.call("map_files_list", Json{{"folder", folder}, {"recursive", true}}))
      == std::vector<std::string>{"A.MAP", "b.map", "c.map"});
    CHECK(
      names(
        fixture.call("map_files_list", Json{{"folder", folder}, {"pattern", "*.txt"}}))
      == std::vector<std::string>{"notes.txt"});

    const auto page =
      fixture.call("map_files_list", Json{{"folder", folder}, {"limit", 1}});
    CHECK(page["total"] == 2);
    CHECK(page["items"].size() == 1);
    CHECK(page["nextCursor"].is_string());
    CHECK(page["items"][0]["size"] == 0);
    CHECK(page["items"][0]["modified"].is_string());

    CHECK(
      fixture
        .callExpectingError(
          "map_files_list", Json{{"folder", (env.dir() / "b.map").string()}})
        .code
      == ErrorCode::IoError);
    CHECK(
      fixture.callExpectingError("map_files_list", Json{{"folder", "maps"}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("document_export_map")
  {
    auto& document = fixture.create();
    auto& map = document.map();
    const auto path = env.dir() / "doc.map";
    fixture.call("document_save_as", Json{{"path", path.string()}});

    auto* layerNode = new mdl::LayerNode{mdl::Layer{"Notes"}};
    mdl::addNodes(map, {{&map.worldNode(), {layerNode}}});
    mdl::setOmitLayerFromExport(map, layerNode, true);

    const auto exportPath = env.dir() / "export.map";
    const auto result = fixture.call(
      "document_export_map",
      Json{{"path", exportPath.string()}, {"stripEditorProperties", true}});
    CHECK(resultOf(result)["path"] == exportPath.string());
    CHECK(resultOf(result)["strippedEditorProperties"] == true);
    REQUIRE(resultOf(result)["omittedLayers"].size() == 1);
    CHECK(resultOf(result)["omittedLayers"][0]["name"] == "Notes");
    CHECK(env.fileExists("export.map"));
    CHECK(env.loadFile("export.map").find("Notes") == std::string::npos);
    CHECK(map.path() == path);

    CHECK(
      fixture
        .callExpectingError("document_export_map", Json{{"path", exportPath.string()}})
        .code
      == ErrorCode::FileExists);
    fixture.call(
      "document_export_map", Json{{"path", exportPath.string()}, {"overwrite", true}});
    CHECK(
      fixture
        .callExpectingError(
          "document_export_map", Json{{"path", path.string()}, {"overwrite", true}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("document_export_obj")
  {
    auto& document = fixture.create();
    addBrush(document);

    const auto path = env.dir() / "doc.obj";
    const auto dryRun = fixture.call(
      "document_export_obj", Json{{"path", path.string()}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["mtlPath"] == (env.dir() / "doc.mtl").string());
    CHECK(!env.fileExists("doc.obj"));

    const auto result = fixture.call(
      "document_export_obj",
      Json{{"path", path.string()}, {"materialPaths", "relativeToExportPath"}});
    CHECK(resultOf(result)["objPath"] == path.string());
    CHECK(env.fileExists("doc.obj"));
    CHECK(env.fileExists("doc.mtl"));

    CHECK(
      fixture.callExpectingError("document_export_obj", Json{{"path", path.string()}})
        .code
      == ErrorCode::FileExists);
  }

  SECTION("create, save, close, reopen, switch mods and WADs")
  {
    fixture.call("document_new", Json{{"game", "Quake"}});
    const auto path = env.dir() / "level.map";
    fixture.call("document_save_as", Json{{"path", path.string()}});
    fixture.call("document_close");
    CHECK(fixture.host().documentList.empty());

    const auto opened = fixture.call("document_open", Json{{"path", path.string()}});
    CHECK(resultOf(opened)["document"]["game"] == "Quake");
    CHECK(resultOf(opened)["document"]["format"] == "Valve");
    CHECK(resultOf(opened)["detection"]["gameSource"] == "fileHeader");

    fixture.call("mods_set", Json{{"mods", Json{"quoth"}}});
    const auto wad = getFixtureRoot() / "test" / "mcp" / "wads" / "cr8_a_excerpt.wad";
    const auto wads =
      fixture.call("materials_collections_set", Json{{"wads", Json{wad.string()}}});
    CHECK(resultOf(wads)["materialCount"].get<size_t>() > 0);

    fixture.call("document_save");
    fixture.call("document_close");
    const auto reopened = fixture.call("document_open", Json{{"path", path.string()}});
    CHECK(resultOf(reopened)["document"]["mods"]["enabled"] == Json{"quoth"});
    CHECK(resultOf(reopened)["document"]["materials"]["wads"] == Json{wad.string()});
  }

  SECTION("autosave_list")
  {
    auto& document = fixture.create();
    static_cast<void>(document);

    const auto unsaved = fixture.call("autosave_list");
    CHECK(unsaved["items"].empty());
    CHECK(unsaved["note"].is_string());

    fixture.call("document_save_as", Json{{"path", (env.dir() / "level.map").string()}});
    env.createDirectory("autosave");
    env.createFile("autosave/level.1.map", "");
    env.createFile("autosave/level.2.map", "");
    env.createFile("autosave/other.1.map", "");

    const auto result = fixture.call("autosave_list");
    REQUIRE(result["items"].size() == 2);
    CHECK(result["items"][0]["name"] == "level.2.map");
    CHECK(result["items"][0]["number"] == 2);
    CHECK(result["items"][1]["name"] == "level.1.map");
    CHECK(result["folder"] == (env.dir() / "autosave").string());
  }
}

} // namespace tb::mcp
