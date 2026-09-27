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
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ServerState.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

std::filesystem::path cubeMapPath()
{
  return getFixtureRoot() / "test/mdl/Game/Quake/id1/cube.map";
}

const Json& resultOf(const Json& structured)
{
  return structured["result"];
}

const DocumentInfo& infoById(McpToolFixture& fixture, const std::string& id)
{
  const auto& documents = fixture.host().documentList;
  const auto it =
    std::ranges::find_if(documents, [&](const auto& info) { return info.id == id; });
  REQUIRE(it != documents.end());
  return *it;
}

const Json* findSummary(const Json& documentList, const std::string& id)
{
  for (const auto& summary : documentList["documents"])
  {
    if (summary["id"] == id)
    {
      return &summary;
    }
  }
  return nullptr;
}

size_t objectCount(ui::MapDocument& document)
{
  return document.map().worldNode().defaultLayer()->childCount();
}

Json createBoxArgs()
{
  return Json{{"min", {0, 0, 0}}, {"max", {64, 64, 16}}};
}

/** Creates a background Quake document with document_new and returns its handle. */
std::string newBackgroundDocument(McpToolFixture& fixture)
{
  const auto result =
    fixture.call("document_new", Json{{"game", "Quake"}, {"window", false}});
  return resultOf(result)["document"]["id"].get<std::string>();
}

} // namespace

TEST_CASE("BackgroundDocuments")
{
  auto fixture = McpToolFixture{};
  auto env = fs::TestEnvironment{};

  SECTION("document_new with window: false")
  {
    SECTION("creates a background document that becomes the active document")
    {
      auto& shown = fixture.create();
      const auto shownId = fixture.documentId(shown);

      const auto result =
        fixture.call("document_new", Json{{"game", "Quake"}, {"window", false}});
      const auto& document = resultOf(result)["document"];
      CHECK(document["background"] == true);
      CHECK(document["focused"] == false);
      CHECK(document["active"] == true);

      const auto id = document["id"].get<std::string>();
      CHECK(infoById(fixture, id).background);
      // the shown document keeps the focus
      CHECK(infoById(fixture, shownId).focused);

      const auto list = fixture.call("document_list");
      CHECK(list["activeDocument"] == id);
      REQUIRE(findSummary(list, id));
      CHECK((*findSummary(list, id))["background"] == true);
      CHECK((*findSummary(list, shownId))["background"] == false);
    }

    SECTION("never replaces a document in single-window mode")
    {
      auto& shown = fixture.create();
      const auto shownId = fixture.documentId(shown);
      fixture.host().singleWindow = true;
      const auto shownObjects = objectCount(shown);

      const auto result =
        fixture.call("document_new", Json{{"game", "Quake"}, {"window", false}});
      CHECK(resultOf(result)["replaced"].is_null());
      CHECK(fixture.host().documentList.size() == 2);
      CHECK(infoById(fixture, shownId).document == &shown);
      CHECK(objectCount(shown) == shownObjects);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "document_new", Json{{"game", "Quake"}, {"window", false}, {"dryRun", true}});
      CHECK(
        resultOf(result)["wouldDo"].get<std::string>().find("in the background")
        != std::string::npos);
      CHECK(fixture.host().documentList.empty());
    }
  }

  SECTION("document_open with window: false")
  {
    const auto result =
      fixture.call("document_open", Json{{"path", cubeMapPath()}, {"window", false}});
    const auto& document = resultOf(result)["document"];
    CHECK(document["background"] == true);
    CHECK(document["active"] == true);
    // a background document is not a recent document until it is shown
    CHECK(fixture.host().recentDocumentList.empty());

    // opening it again returns the same document
    const auto again = fixture.call("document_open", Json{{"path", cubeMapPath()}});
    CHECK(resultOf(again)["alreadyOpen"] == true);
    CHECK(resultOf(again)["document"]["id"] == document["id"]);
  }

  SECTION("background documents work with the map, snapshot and save tools")
  {
    const auto id = newBackgroundDocument(fixture);
    auto& document = *infoById(fixture, id).document;
    const auto count = objectCount(document);

    fixture.call("brush_create_box", createBoxArgs());
    CHECK(objectCount(document) == count + 1);

    fixture.call("view_snapshot");
    CHECK(!fixture.host().snapshot.requests.empty());

    const auto path = env.dir() / "prefab.map";
    const auto saved = fixture.call("document_save_as", Json{{"path", path}});
    CHECK(resultOf(saved)["path"] == path.string());
    CHECK(std::filesystem::exists(path));
    CHECK(!document.map().modified());
    CHECK(infoById(fixture, id).background);
  }

  SECTION("tools that need a window fail with DOCUMENT_IN_BACKGROUND")
  {
    newBackgroundDocument(fixture);

    const auto cameraError = fixture.callExpectingError("camera_get");
    CHECK(cameraError.code == ErrorCode::DocumentInBackground);
    CHECK(cameraError.hint.find("document_show") != std::string::npos);

    CHECK(
      fixture
        .callExpectingError(
          "camera_set", Json{{"position", {0, -512, 256}}, {"lookAt", {0, 0, 64}}})
        .code
      == ErrorCode::DocumentInBackground);
    CHECK(
      fixture.callExpectingError("view_layout_set", Json{{"maximized", "3d"}}).code
      == ErrorCode::DocumentInBackground);
    CHECK(
      fixture.callExpectingError("actions_list").code == ErrorCode::DocumentInBackground);
    CHECK(
      fixture.callExpectingError("action_invoke", Json{{"path", "Menu/Edit/Undo"}}).code
      == ErrorCode::DocumentInBackground);
    CHECK(
      fixture.callExpectingError("view_snapshot_user").code
      == ErrorCode::DocumentInBackground);
  }

  SECTION("the focus fallback never chooses a background document")
  {
    newBackgroundDocument(fixture);
    const auto session = fixture.openSession("other");
    CHECK(
      fixture.callExpectingErrorAs(session, "brush_create_box", createBoxArgs()).code
      == ErrorCode::NoDocument);

    auto& shown = fixture.create();
    const auto result = fixture.callAs(session, "brush_create_box", createBoxArgs());
    CHECK(objectCount(shown) > 0);
    CHECK(
      fixture.callAs(session, "document_list", Json::object())["activeDocument"]
      == fixture.documentId(shown));
    CHECK(result.contains("warnings"));
  }

  SECTION("document_show")
  {
    SECTION("gives the document a window and keeps its ids and history")
    {
      const auto id = newBackgroundDocument(fixture);
      auto& document = *infoById(fixture, id).document;

      const auto created = fixture.call("brush_create_box", createBoxArgs());
      const auto brushId = resultOf(created)["brush"].get<std::string>();
      const auto undoSteps = document.map().commandProcessor().undoCommandNames();

      const auto path = env.dir() / "shown.map";
      fixture.call("document_save_as", Json{{"path", path}});

      const auto result = fixture.call("document_show");
      CHECK(resultOf(result)["alreadyShown"] == false);
      CHECK(resultOf(result)["document"]["id"] == id);
      CHECK(resultOf(result)["document"]["background"] == false);
      CHECK(resultOf(result)["document"]["focused"] == true);

      const auto& info = infoById(fixture, id);
      CHECK(!info.background);
      CHECK(info.document == &document);
      CHECK(fixture.node(brushId, &document) != nullptr);
      CHECK(document.map().commandProcessor().undoCommandNames() == undoSteps);
      CHECK(fixture.host().recentDocumentList == std::vector{path});
      CHECK(fixture.call("document_list")["activeDocument"] == id);

      const auto again = fixture.call("document_show", Json{{"document", id}});
      CHECK(resultOf(again)["alreadyShown"] == true);
    }

    SECTION("dry run")
    {
      const auto id = newBackgroundDocument(fixture);
      const auto result = fixture.call("document_show", Json{{"dryRun", true}});
      CHECK(resultOf(result).contains("wouldDo"));
      CHECK(infoById(fixture, id).background);
    }

    SECTION("refuses while an agent transaction is open")
    {
      const auto id = newBackgroundDocument(fixture);
      fixture.call("transaction_begin", Json{{"name", "Work"}});
      CHECK(
        fixture.callExpectingError("document_show").code == ErrorCode::TransactionActive);
      CHECK(infoById(fixture, id).background);
    }

    SECTION("refuses in single-window mode while another document is shown")
    {
      auto& shown = fixture.create();
      fixture.host().singleWindow = true;
      const auto id = newBackgroundDocument(fixture);

      const auto error = fixture.callExpectingError("document_show");
      CHECK(error.code == ErrorCode::OperationFailed);
      CHECK(infoById(fixture, id).background);

      fixture.call(
        "document_close",
        Json{{"document", fixture.documentId(shown)}, {"unsavedChanges", "discard"}});
      fixture.call("document_show", Json{{"document", id}});
      CHECK(!infoById(fixture, id).background);
    }

    SECTION("without an active document")
    {
      newBackgroundDocument(fixture);
      const auto session = fixture.openSession("other");
      CHECK(
        fixture.callExpectingErrorAs(session, "document_show", Json::object()).code
        == ErrorCode::NoDocument);
      CHECK(
        fixture.callExpectingError("document_show", Json{{"document", "doc:99"}}).code
        == ErrorCode::DocumentNotFound);
    }
  }

  SECTION("document_close")
  {
    const auto id = newBackgroundDocument(fixture);
    fixture.call("brush_create_box", createBoxArgs());

    CHECK(
      fixture.callExpectingError("document_close", Json{{"document", id}}).code
      == ErrorCode::UnsavedChanges);

    const auto result = fixture.call(
      "document_close", Json{{"document", id}, {"unsavedChanges", "discard"}});
    CHECK(resultOf(result)["closed"] == id);
    CHECK(resultOf(result)["discardedChanges"] == true);
    CHECK(fixture.host().documentList.empty());
  }
}

} // namespace tb::mcp
