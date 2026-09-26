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
#include "fs/TestEnvironment.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/Map_World.h"
#include "ui/MapDocument.h"

#include "kd/invoke.h"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

std::filesystem::path wadPath()
{
  return getFixtureRoot() / "test" / "mcp" / "wads" / "cr8_a_excerpt.wad";
}

/** Creates a document through document_new and returns it. */
ui::MapDocument& newDocument(McpToolFixture& fixture, const std::string& game)
{
  const auto result = fixture.call("document_new", Json{{"game", game}});
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

std::vector<std::string> strings(const Json& array)
{
  return array.get<std::vector<std::string>>();
}

} // namespace

TEST_CASE("GameTools")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};

  auto& quakeInfo = *fixture.host().gameManager().gameInfo("Quake");
  const auto originalQuakePath = pref(quakeInfo.gamePathPreference);
  const auto restoreQuakePath = kdl::invoke_later{
    [&]() { setPref(quakeInfo.gamePathPreference, originalQuakePath); }};

  SECTION("game_list")
  {
    const auto findGame = [](const Json& list, const std::string& name) {
      for (const auto& item : list["items"])
      {
        if (item["name"] == name)
        {
          return item;
        }
      }
      return Json{};
    };

    const auto result = fixture.call("game_list");
    CHECK(result["total"] == 3);
    REQUIRE(result["items"].size() == 3);

    const auto quake = findGame(result, "Quake");
    REQUIRE(quake.is_object());
    CHECK(strings(quake["formats"]) == std::vector<std::string>{"Valve", "Standard"});
    CHECK(quake["gamePathValid"] == true);
    CHECK(quake["activeDocumentGame"] == false);

    newDocument(fixture, "Quake");
    CHECK(findGame(fixture.call("game_list"), "Quake")["activeDocumentGame"] == true);

    const auto names = fixture.call("game_list", Json{{"fields", Json{"name"}}});
    CHECK(names["items"][0].size() == 1);
    CHECK(names["items"][0].contains("name"));
  }

  SECTION("game_info")
  {
    const auto quake = fixture.call("game_info", Json{{"game", "Quake"}});
    CHECK(quake["name"] == "Quake");
    CHECK(quake["formats"][0]["format"] == "Valve");
    CHECK(quake["fileSystem"]["searchPath"] == "id1");
    CHECK(quake["materials"]["mode"] == "wad");
    CHECK(quake["materials"]["wadProperty"] == "wad");
    CHECK(quake["entityDefinitions"]["builtin"][0] == "Quake.fgd");
    CHECK(!quake["tags"].empty());
    CHECK(quake["tags"][0]["name"].is_string());
    CHECK(quake["softMapBounds"]["min"] == Json{-4096, -4096, -4096});
    CHECK(quake["gamePath"]["valid"] == true);

    const auto quake2 = fixture.call("game_info", Json{{"game", "Quake 2"}});
    CHECK(quake2["materials"]["mode"] == "folders");
    CHECK(!quake2["surfaceFlags"].empty());
    CHECK(quake2["surfaceFlags"][0]["name"].is_string());
    CHECK(!quake2["contentFlags"].empty());

    CHECK(
      fixture.callExpectingError("game_info", Json{{"game", "Doom"}}).code
      == ErrorCode::InvalidArgument);
    CHECK(fixture.callExpectingError("game_info").code == ErrorCode::InvalidArgument);

    newDocument(fixture, "Quake 2");
    CHECK(fixture.call("game_info")["name"] == "Quake 2");
  }

  SECTION("game_set_path")
  {
    auto& document = newDocument(fixture, "Quake");
    const auto documentId = fixture.documentId(document);
    const auto newPath = env.dir();

    const auto dryRun = fixture.call(
      "game_set_path",
      Json{{"game", "Quake"}, {"path", newPath.string()}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["affectedDocuments"] == Json{documentId});
    CHECK(pref(quakeInfo.gamePathPreference) == originalQuakePath);

    const auto result =
      fixture.call("game_set_path", Json{{"game", "quake"}, {"path", newPath.string()}});
    CHECK(resultOf(result)["game"] == "Quake");
    CHECK(resultOf(result)["path"] == newPath.string());
    CHECK(resultOf(result)["previousPath"] == originalQuakePath.string());
    CHECK(resultOf(result)["valid"] == true);
    CHECK(pref(quakeInfo.gamePathPreference) == newPath);
    CHECK(document.map().gamePath() == newPath);

    const auto cleared =
      fixture.call("game_set_path", Json{{"game", "Quake"}, {"path", ""}});
    CHECK(resultOf(cleared)["valid"] == false);

    CHECK(
      fixture
        .callExpectingError(
          "game_set_path",
          Json{{"game", "Quake"}, {"path", (env.dir() / "missing").string()}})
        .code
      == ErrorCode::IoError);
    CHECK(
      fixture.callExpectingError("game_set_path", Json{{"game", "Quake"}, {"path", "q"}})
        .code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("game_set_path", Json{{"game", "Doom"}, {"path", ""}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("mods_get and mods_set")
  {
    env.createDirectory("id1");
    env.createDirectory("hipnotic");
    env.createDirectory("quoth");
    setPref(quakeInfo.gamePathPreference, env.dir());

    auto& document = newDocument(fixture, "Quake");

    const auto mods = fixture.call("mods_get");
    CHECK(mods["default"] == "id1");
    CHECK(mods["enabled"] == Json::array());
    CHECK(strings(mods["available"]) == std::vector<std::string>{"hipnotic", "quoth"});

    const auto dryRun =
      fixture.call("mods_set", Json{{"mods", Json{"quoth"}}, {"dryRun", true}});
    CHECK(resultOf(dryRun)["enabled"] == Json{"quoth"});
    CHECK(mdl::enabledMods(document.map()).empty());

    const auto result = fixture.call("mods_set", Json{{"mods", Json{"quoth", "nope"}}});
    CHECK(result["undoStep"] == "AI: Set Mods");
    CHECK(resultOf(result)["enabled"] == Json{"quoth", "nope"});
    REQUIRE(result["warnings"].size() == 1);
    CHECK(result["warnings"][0]["code"] == "UNKNOWN_MOD");
    CHECK(mdl::enabledMods(document.map()) == std::vector<std::string>{"quoth", "nope"});
    CHECK(result["changes"]["modified"] == Json{"world"});

    CHECK(
      fixture.callExpectingError("mods_set", Json{{"mods", Json{"a", "A"}}}).code
      == ErrorCode::InvalidArgument);

    fixture.call("mods_set", Json{{"mods", Json::array()}});
    CHECK(mdl::enabledMods(document.map()).empty());
  }

  SECTION("entity definitions")
  {
    auto& document = newDocument(fixture, "Quake");
    static_cast<void>(document);

    const auto current = fixture.call("entity_definitions_get");
    CHECK(current["type"] == "builtin");
    CHECK(current["path"] == "Quake.fgd");
    CHECK(current["explicitlySet"] == false);
    CHECK(current["definitionCount"].get<size_t>() > 0);
    REQUIRE(current["builtin"].size() == 4);
    CHECK(current["builtin"][0] == Json{{"path", "Quake.fgd"}, {"current", true}});

    const auto quoth = fixture.call(
      "entity_definitions_set", Json{{"type", "builtin"}, {"path", "Quoth2.fgd"}});
    CHECK(quoth["undoStep"] == "AI: Set Entity Definitions");
    CHECK(resultOf(quoth)["spec"] == "builtin:Quoth2.fgd");
    CHECK(resultOf(quoth)["explicitlySet"] == true);
    CHECK(resultOf(quoth)["definitionCount"].get<size_t>() > 0);

    CHECK(
      fixture
        .callExpectingError(
          "entity_definitions_set", Json{{"type", "builtin"}, {"path", "Doom.fgd"}})
        .code
      == ErrorCode::InvalidArgument);

    const auto fgd = getFixtureRoot() / "games" / "Quake" / "Quake.fgd";
    const auto external = fixture.call(
      "entity_definitions_set", Json{{"type", "external"}, {"path", fgd.string()}});
    CHECK(resultOf(external)["type"] == "external");
    CHECK(resultOf(external)["definitionCount"].get<size_t>() > 0);
    CHECK(external["warnings"].empty());

    const auto missing = fixture.call(
      "entity_definitions_set",
      Json{{"type", "external"}, {"path", (env.dir() / "missing.fgd").string()}});
    CHECK(missing["warnings"][0]["code"] == "FILE_NOT_FOUND");

    const auto reloaded = fixture.call("entity_definitions_reload");
    CHECK(reloaded["undoStep"].is_null());
    CHECK(resultOf(reloaded)["type"] == "external");
    CHECK(resultOf(reloaded)["loadMessages"].is_array());
  }

  SECTION("material collections")
  {
    SECTION("WAD games")
    {
      auto& document = newDocument(fixture, "Quake");
      static_cast<void>(document);

      const auto current = fixture.call("materials_collections_get");
      CHECK(current["mode"] == "wad");
      CHECK(current["wadProperty"] == "wad");
      CHECK(current["wads"] == Json::array());

      const auto result = fixture.call(
        "materials_collections_set", Json{{"wads", Json{wadPath().string()}}});
      CHECK(result["undoStep"] == "AI: Set Material Collections");
      CHECK(resultOf(result)["wads"] == Json{wadPath().string()});
      REQUIRE(resultOf(result)["collections"].size() == 1);
      CHECK(resultOf(result)["collections"][0]["materialCount"].get<size_t>() > 0);
      CHECK(resultOf(result)["materialCount"].get<size_t>() > 0);

      const auto missing =
        fixture.call("materials_collections_set", Json{{"wads", Json{"missing.wad"}}});
      CHECK(missing["warnings"][0]["code"] == "FILE_NOT_FOUND");

      fixture.call("undo");
      CHECK(
        fixture.call("materials_collections_get")["wads"] == Json{wadPath().string()});

      const auto reloaded = fixture.call("materials_reload");
      CHECK(resultOf(reloaded)["collections"].size() == 1);
    }

    SECTION("folder games")
    {
      auto& document = newDocument(fixture, "Quake 2");
      static_cast<void>(document);

      const auto current = fixture.call("materials_collections_get");
      CHECK(current["mode"] == "folders");
      CHECK(!current.contains("wads"));
      REQUIRE(current["collections"].size() == 3);
      CHECK(current["enabled"].size() == 3);

      CHECK(
        fixture
          .callExpectingError("materials_collections_set", Json{{"wads", Json::array()}})
          .code
        == ErrorCode::Unsupported);
      CHECK(
        fixture.callExpectingError("materials_collections_set").code
        == ErrorCode::InvalidArgument);

      const auto enabled = current["collections"][0]["path"].get<std::string>();
      const auto result = fixture.call(
        "materials_collections_set", Json{{"enabled", Json{enabled, "textures/nope"}}});
      CHECK(resultOf(result)["enabled"] == Json{enabled, "textures/nope"});
      CHECK(resultOf(result)["collections"][0]["enabled"] == true);
      CHECK(resultOf(result)["collections"][1]["enabled"] == false);
      CHECK(result["warnings"][0]["code"] == "UNKNOWN_COLLECTION");
    }
  }

  SECTION("soft bounds")
  {
    auto& document = newDocument(fixture, "Quake");

    const auto current = fixture.call("soft_bounds_get");
    CHECK(current["mode"] == "game");
    CHECK(current["bounds"]["max"] == Json{4096, 4096, 4096});
    CHECK(current["gameDefault"] == current["bounds"]);

    const auto box =
      Json{{"min", Json{-1024, -1024, -512}}, {"max", Json{1024, 1024, 512}}};
    const auto custom =
      fixture.call("soft_bounds_set", Json{{"mode", "custom"}, {"bounds", box}});
    CHECK(custom["undoStep"] == "AI: Set Soft Map Bounds");
    CHECK(resultOf(custom)["mode"] == "custom");
    CHECK(resultOf(custom)["bounds"] == box);

    const auto unlimited = fixture.call("soft_bounds_set", Json{{"mode", "unlimited"}});
    CHECK(resultOf(unlimited)["mode"] == "unlimited");
    CHECK(resultOf(unlimited)["bounds"].is_null());

    fixture.call("soft_bounds_set", Json{{"mode", "game"}});
    CHECK(mdl::softMapBounds(document.map()).source == mdl::SoftMapBoundsType::Game);

    CHECK(
      fixture.callExpectingError("soft_bounds_set", Json{{"mode", "custom"}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture
        .callExpectingError("soft_bounds_set", Json{{"mode", "game"}, {"bounds", box}})
        .code
      == ErrorCode::InvalidArgument);
  }

  SECTION("resources")
  {
    auto& document = newDocument(fixture, "Quake");
    const auto documentId = fixture.documentId(document);
    const auto infoUri = "trenchbroom://documents/" + documentId + "/info";

    const auto list = fixture.rpc("resources/list");
    auto uris = std::vector<std::string>{};
    for (const auto& resource : list["result"]["resources"])
    {
      uris.push_back(resource["uri"].get<std::string>());
    }
    CHECK(std::ranges::find(uris, infoUri) != uris.end());
    CHECK(std::ranges::find(uris, "trenchbroom://games/Quake/config") != uris.end());

    const auto info = fixture.rpc("resources/read", Json{{"uri", infoUri}});
    const auto infoJson =
      parseJson(info["result"]["contents"][0]["text"].get<std::string>());
    REQUIRE(infoJson);
    CHECK((*infoJson)["id"] == documentId);
    CHECK((*infoJson)["mods"]["default"] == "id1");

    const auto config = fixture.rpc(
      "resources/read", Json{{"uri", "trenchbroom://games/Quake%202/config"}});
    const auto configJson =
      parseJson(config["result"]["contents"][0]["text"].get<std::string>());
    REQUIRE(configJson);
    CHECK((*configJson)["name"] == "Quake 2");

    CHECK(
      fixture.rpc("resources/read", Json{{"uri", "trenchbroom://documents/doc:99/info"}})
        .contains("error"));
    CHECK(fixture.rpc("resources/read", Json{{"uri", "trenchbroom://games/Doom/config"}})
            .contains("error"));

    SECTION("subscribers are notified when the document info changes")
    {
      auto stream = std::make_shared<CapturingNotificationStream>();
      fixture.server().openNotificationStream(fixture.sessionId(), stream);
      fixture.rpc("resources/subscribe", Json{{"uri", infoUri}});

      fixture.call("mods_set", Json{{"mods", Json{"quoth"}}});

      const auto updated =
        std::ranges::count_if(stream->notifications, [&](const auto& n) {
          return n["method"] == "notifications/resources/updated"
                 && n["params"]["uri"] == infoUri;
        });
      CHECK(updated > 0);
    }
  }
}

} // namespace tb::mcp
