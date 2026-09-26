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
#include "gl/MaterialManager.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mcp/tools/EntityClassTools.h"
#include "mcp/tools/MaterialTools.h"
#include "mcp/tools/SceneTools.h"
#include "mcp/tools/SelectionTools.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include "kd/invoke.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

Json readResource(McpToolFixture& fixture, const std::string& uri)
{
  const auto response = fixture.rpc("resources/read", Json{{"uri", uri}});
  REQUIRE(response.contains("result"));
  const auto& contents = response["result"]["contents"];
  REQUIRE(contents.size() == 1);
  CHECK(contents[0]["uri"] == uri);
  CHECK(contents[0]["mimeType"] == "application/json");
  return *parseJson(contents[0]["text"].get<std::string>());
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

size_t countOf(const std::vector<std::string>& uris, const std::string& uri)
{
  return size_t(std::ranges::count(uris, uri));
}

} // namespace

TEST_CASE("Resources")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.load(
    getFixtureRoot() / "test" / "mcp" / "maps" / "two_rooms.map",
    {.mapFormat = mdl::MapFormat::Standard, .gameInfo = mdl::QuakeGameInfo});
  auto& map = document.map();
  auto& state = fixture.server().state();
  const auto documentId = fixture.documentId(document);

  const auto statusUri = std::string{"trenchbroom://editor/status"};
  const auto summaryUri = "trenchbroom://documents/" + documentId + "/summary";
  const auto selectionUri = "trenchbroom://documents/" + documentId + "/selection";
  const auto entityDefinitionsUri =
    "trenchbroom://documents/" + documentId + "/entity-definitions";
  const auto materialsUri = "trenchbroom://documents/" + documentId + "/materials";
  const auto materialsWad =
    (getFixtureRoot() / "test" / "mcp" / "wads" / "materials.wad").string();

  SECTION("resources/list lists the document resources")
  {
    const auto response = fixture.rpc("resources/list");
    auto uris = std::vector<std::string>{};
    for (const auto& resource : response["result"]["resources"])
    {
      uris.push_back(resource["uri"].get<std::string>());
    }
    CHECK(countOf(uris, summaryUri) == 1);
    CHECK(countOf(uris, selectionUri) == 1);
    CHECK(countOf(uris, "trenchbroom://documents/" + documentId + "/info") == 1);
    CHECK(countOf(uris, entityDefinitionsUri) == 1);
    CHECK(countOf(uris, materialsUri) == 1);

    const auto templates = fixture.rpc("resources/templates/list");
    auto uriTemplates = std::vector<std::string>{};
    for (const auto& resourceTemplate : templates["result"]["resourceTemplates"])
    {
      uriTemplates.push_back(resourceTemplate["uriTemplate"].get<std::string>());
    }
    CHECK(countOf(uriTemplates, "trenchbroom://documents/{doc}/summary") == 1);
    CHECK(countOf(uriTemplates, "trenchbroom://documents/{doc}/selection") == 1);
    CHECK(countOf(uriTemplates, "trenchbroom://documents/{doc}/entity-definitions") == 1);
    CHECK(countOf(uriTemplates, "trenchbroom://documents/{doc}/materials") == 1);
  }

  SECTION("entity definitions")
  {
    const auto fgd = getFixtureRoot() / "games" / "Quake" / "Quake.fgd";
    fixture.call(
      "entity_definitions_set", Json{{"type", "external"}, {"path", fgd.string()}});
    REQUIRE(!map.entityDefinitionManager().definitions().empty());

    auto expected = entityDefinitionsResource(map);
    expected["document"] = documentId;
    const auto resource = readResource(fixture, entityDefinitionsUri);
    CHECK(resource == expected);
    CHECK(resource["spec"] == "external:" + fgd.string());
    CHECK(resource["count"] == map.entityDefinitionManager().definitions().size());
    CHECK(resource["classes"].size() == resource["count"]);
  }

  SECTION("materials")
  {
    fixture.call("materials_collections_set", Json{{"wads", Json{materialsWad}}});
    REQUIRE(map.materialManager().materials().size() == 6);

    auto expected = materialsResource(map);
    expected["document"] = documentId;
    const auto resource = readResource(fixture, materialsUri);
    CHECK(resource == expected);
    CHECK(
      resource["collections"]
      == Json::array({Json{{"path", "materials.wad"}, {"materialCount", 6}}}));
    CHECK(resource["count"] == 6);
    CHECK(resource["materials"][0]["name"] == "floor_tile");
    CHECK(resource["materials"][0]["collection"] == "materials.wad");
    CHECK(resource["materials"][0].contains("width"));
    CHECK(!resource["materials"][0].contains("usage"));
  }

  SECTION("map summary")
  {
    const auto& ids = state.documentState(document).ids;
    CHECK(readResource(fixture, summaryUri) == mapSummary(map, ids));
    CHECK(
      fixture.rpc(
        "resources/read",
        Json{{"uri", "trenchbroom://documents/doc:99/summary"}})["error"]["code"]
      == jsonrpc::ErrorCode::ResourceNotFound);
  }

  SECTION("selection")
  {
    const auto& ids = state.documentState(document).ids;
    CHECK(readResource(fixture, selectionUri)["count"] == 0);

    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
    mdl::selectNodes(map, {brushNode});

    const auto selection = readResource(fixture, selectionUri);
    CHECK(selection == selectionDetails(map, ids, 100));
    CHECK(selection["count"] == 1);
  }

  SECTION("subscriptions")
  {
    auto notifications = std::make_shared<CapturingNotificationStream>();
    REQUIRE(fixture.server().openNotificationStream(fixture.sessionId(), notifications));
    for (const auto& uri :
         {statusUri, summaryUri, selectionUri, entityDefinitionsUri, materialsUri})
    {
      REQUIRE(
        fixture.rpc("resources/subscribe", Json{{"uri", uri}})["result"].is_object());
    }

    SECTION("map changes update the summary once per burst")
    {
      auto* brushNode1 = mdl::createBrushNode(map);
      auto* brushNode2 = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode1}}});
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode2}}});

      // coalesced: nothing is sent until pending events were processed
      CHECK(updatedUris(*notifications).empty());
      fixture.scheduler().runPending();

      const auto uris = updatedUris(*notifications);
      CHECK(countOf(uris, summaryUri) == 1);
      CHECK(countOf(uris, selectionUri) == 0);
    }

    SECTION("selection changes update the selection and the editor status")
    {
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      fixture.scheduler().runPending();
      notifications->notifications.clear();

      mdl::selectNodes(map, {brushNode});
      fixture.scheduler().runPending();

      auto uris = updatedUris(*notifications);
      CHECK(countOf(uris, selectionUri) == 1);
      CHECK(countOf(uris, statusUri) == 1);

      // moving a selected object changes the bounds in the selection summary
      notifications->notifications.clear();
      REQUIRE(mdl::translateSelection(map, {16, 0, 0}));
      fixture.scheduler().runPending();

      uris = updatedUris(*notifications);
      CHECK(countOf(uris, selectionUri) == 1);
      CHECK(countOf(uris, summaryUri) == 1);
    }

    SECTION("tool, grid and lock changes update the editor status")
    {
      fixture.host().currentToolDidChangeNotifier(document);
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), statusUri) == 1);

      notifications->notifications.clear();
      map.grid().incSize();
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), statusUri) == 1);
      CHECK(countOf(updatedUris(*notifications), summaryUri) == 1);

      notifications->notifications.clear();
      const auto uvLock = pref(Preferences::UvLock);
      const auto restore =
        kdl::invoke_later{[&]() { setPref(Preferences::UvLock, uvLock); }};
      setPref(Preferences::UvLock, !uvLock);
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), statusUri) == 1);
    }

    SECTION("transactions update the editor status")
    {
      fixture.call("transaction_begin", Json{{"name", "Test"}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), statusUri) == 1);
      fixture.call("transaction_rollback");
    }

    SECTION("entity definition changes update the entity definitions")
    {
      fixture.call(
        "entity_definitions_set", Json{{"type", "builtin"}, {"path", "Quoth2.fgd"}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), entityDefinitionsUri) == 1);

      // map changes do not change the definitions
      notifications->notifications.clear();
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), entityDefinitionsUri) == 0);
    }

    SECTION("material collection changes update the materials")
    {
      fixture.call("materials_collections_set", Json{{"wads", Json{materialsWad}}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), materialsUri) == 1);

      // the image sizes are known once the images are loaded
      notifications->notifications.clear();
      auto gl = gl::TestGl{};
      gl::processResourcesSync(
        map.resourceManager(), gl::ProcessContext{gl, [](auto, auto) {}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), materialsUri) == 1);
      CHECK(readResource(fixture, materialsUri)["materials"][0]["width"] == 32);

      // map changes do not change the materials
      notifications->notifications.clear();
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), materialsUri) == 0);
    }

    SECTION("unsubscribed resources are not notified")
    {
      fixture.rpc("resources/unsubscribe", Json{{"uri", summaryUri}});

      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      fixture.scheduler().runPending();
      CHECK(countOf(updatedUris(*notifications), summaryUri) == 0);
    }
  }
}

} // namespace tb::mcp
