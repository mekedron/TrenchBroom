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
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <filesystem>
#include <memory>
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

ui::MapDocument& documentById(McpToolFixture& fixture, const std::string& id)
{
  const auto& documents = fixture.host().documentList;
  const auto it =
    std::ranges::find_if(documents, [&](const auto& info) { return info.id == id; });
  REQUIRE(it != documents.end());
  return *it->document;
}

size_t objectCount(ui::MapDocument& document)
{
  return document.map().worldNode().defaultLayer()->childCount();
}

Json createBoxArgs(const double z = 0)
{
  return Json{{"min", {0, 0, z}}, {"max", {64, 64, z + 16}}};
}

bool hasWarning(const Json& structured, const std::string& code)
{
  const auto warnings = structured.value("warnings", Json::array());
  return std::ranges::any_of(
    warnings, [&](const auto& warning) { return warning["code"] == code; });
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

Json readStatus(
  McpToolFixture& fixture, const std::string& sessionId, const int requestId)
{
  const auto stream = fixture.post(
    sessionId,
    jsonrpc::makeRequest(
      requestId, "resources/read", Json{{"uri", "trenchbroom://editor/status"}}));
  REQUIRE(stream->response.has_value());
  const auto& contents = (*stream->response)["result"]["contents"];
  REQUIRE(contents.size() == 1);
  auto status = parseJson(contents[0]["text"].get<std::string>());
  REQUIRE(status.has_value());
  return *status;
}

} // namespace

TEST_CASE("DocumentTargeting")
{
  auto fixture = McpToolFixture{};

  // session A opens map 1, session B (e.g. a subagent) creates map 2 afterwards; map 2's
  // window takes the focus
  const auto sessionA = fixture.sessionId();
  const auto opened =
    fixture.call("document_open", Json{{"path", cubeMapPath().string()}});
  const auto map1 = opened["result"]["document"]["id"].get<std::string>();

  const auto sessionB = fixture.openSession("subagent");
  const auto created = fixture.callAs(sessionB, "document_new", Json{{"game", "Quake"}});
  const auto map2 = created["result"]["document"]["id"].get<std::string>();
  REQUIRE(map1 != map2);

  auto& document1 = documentById(fixture, map1);
  auto& document2 = documentById(fixture, map2);
  const auto count1 = objectCount(document1);
  const auto count2 = objectCount(document2);

  SECTION("each session acts on its own active document")
  {
    const auto resultA = fixture.callAs(sessionA, "brush_create_box", createBoxArgs());
    CHECK(!hasWarning(resultA, "DOCUMENT_FROM_FOCUS"));
    CHECK(objectCount(document1) == count1 + 1);
    CHECK(objectCount(document2) == count2);

    fixture.callAs(sessionB, "brush_create_box", createBoxArgs(64));
    CHECK(objectCount(document1) == count1 + 1);
    CHECK(objectCount(document2) == count2 + 1);

    // the focus does not matter either
    fixture.host().setFocused(map1);
    fixture.callAs(sessionB, "brush_create_box", createBoxArgs(128));
    CHECK(objectCount(document1) == count1 + 1);
    CHECK(objectCount(document2) == count2 + 2);
  }

  SECTION("document_list and editor_status show the sessions' active documents")
  {
    const auto list = fixture.callAs(sessionA, "document_list", Json::object());
    CHECK(list["activeDocument"] == map1);
    CHECK(list["targetDocument"] == map1);
    CHECK(list["activeDocumentClosed"].is_null());

    const auto* summary1 = findSummary(list, map1);
    const auto* summary2 = findSummary(list, map2);
    REQUIRE(summary1);
    REQUIRE(summary2);
    CHECK((*summary1)["active"] == true);
    CHECK((*summary1)["activeIn"] == Json::array({"test-client 1.0"}));
    CHECK((*summary1)["focused"] == false);
    CHECK((*summary2)["active"] == false);
    CHECK((*summary2)["activeIn"] == Json::array({"subagent 1.0"}));
    CHECK((*summary2)["focused"] == true);

    CHECK(
      fixture.callAs(sessionA, "editor_status", Json::object())["activeDocument"]
      == map1);
    CHECK(
      fixture.callAs(sessionB, "editor_status", Json::object())["activeDocument"]
      == map2);

    // the editor status resource follows the reading session's active document
    CHECK(readStatus(fixture, sessionA, 9001)["activeDocument"] == map1);
    CHECK(readStatus(fixture, sessionB, 9002)["activeDocument"] == map2);
  }

  SECTION("an explicit document argument does not change the active document")
  {
    fixture.callAs(
      sessionA,
      "brush_create_box",
      Json{{"min", {0, 0, 0}}, {"max", {8, 8, 8}}, {"document", map2}});
    CHECK(objectCount(document2) == count2 + 1);
    CHECK(
      fixture.callAs(sessionA, "document_list", Json::object())["activeDocument"]
      == map1);
  }

  SECTION("closing the active document of a session")
  {
    const auto checkClosedError = [&]() {
      const auto error =
        fixture.callExpectingErrorAs(sessionA, "brush_create_box", createBoxArgs());
      CHECK(error.code == ErrorCode::ActiveDocumentClosed);
      CHECK(error.message.find(map1) != std::string::npos);
      CHECK(error.message.find(map2) != std::string::npos);
      CHECK(error.hint.find("document_activate") != std::string::npos);
      CHECK(error.details["closedDocument"] == map1);
      REQUIRE(error.details["openDocuments"].size() == 1);
      CHECK(error.details["openDocuments"][0]["id"] == map2);

      // map 2 is not touched
      CHECK(objectCount(document2) == count2);

      // read-only tools that need a document fail, too
      CHECK(
        fixture.callExpectingErrorAs(sessionA, "map_summary", Json::object()).code
        == ErrorCode::ActiveDocumentClosed);

      const auto list = fixture.callAs(sessionA, "document_list", Json::object());
      CHECK(list["activeDocument"].is_null());
      CHECK(list["activeDocumentClosed"] == map1);
      CHECK(list["targetDocument"].is_null());

      // session B keeps working on map 2
      fixture.callAs(sessionB, "brush_create_box", createBoxArgs());
      CHECK(objectCount(document2) == count2 + 1);

      // document_activate resolves it
      fixture.callAs(sessionA, "document_activate", Json{{"document", map2}});
      fixture.callAs(sessionA, "brush_create_box", createBoxArgs(64));
      CHECK(objectCount(document2) == count2 + 2);
    };

    SECTION("by another session")
    {
      fixture.callAs(
        sessionB,
        "document_close",
        Json{{"document", map1}, {"unsavedChanges", "discard"}});
      checkClosedError();
    }

    SECTION("by the user")
    {
      fixture.host().removeDocument(document1);
      checkClosedError();
    }

    SECTION("by the session itself")
    {
      fixture.callAs(sessionA, "document_close", Json{{"unsavedChanges", "discard"}});
      CHECK(fixture.host().documentList.size() == 1);

      // the session has no active document now: it falls back to the focused window
      const auto result = fixture.callAs(sessionA, "map_summary", Json::object());
      CHECK(hasWarning(result, "DOCUMENT_FROM_FOCUS"));
    }
  }

  SECTION("a session without an active document falls back to the focused window")
  {
    const auto sessionC = fixture.openSession("fresh");
    CHECK(
      fixture.callAs(sessionC, "document_list", Json::object())["targetDocument"]
      == map2);

    const auto first = fixture.callAs(sessionC, "brush_create_box", createBoxArgs());
    CHECK(objectCount(document2) == count2 + 1);
    REQUIRE(hasWarning(first, "DOCUMENT_FROM_FOCUS"));
    const auto warnings = first["warnings"];
    const auto it = std::ranges::find_if(warnings, [](const auto& warning) {
      return warning["code"] == "DOCUMENT_FROM_FOCUS";
    });
    CHECK((*it)["message"].template get<std::string>().find(map2) != std::string::npos);

    // the focused document became the session's active document
    CHECK(
      fixture.callAs(sessionC, "document_list", Json::object())["activeDocument"]
      == map2);

    fixture.host().setFocused(map1);
    const auto second = fixture.callAs(sessionC, "brush_create_box", createBoxArgs(64));
    CHECK(!hasWarning(second, "DOCUMENT_FROM_FOCUS"));
    CHECK(objectCount(document1) == count1);
    CHECK(objectCount(document2) == count2 + 2);
  }

  SECTION("closing and reverting never fall back to the focused window")
  {
    const auto sessionC = fixture.openSession("fresh");
    for (const auto* tool : {"document_close", "document_revert"})
    {
      const auto error =
        fixture.callExpectingErrorAs(sessionC, tool, Json{{"unsavedChanges", "discard"}});
      CHECK(error.code == ErrorCode::NoDocument);
      CHECK(error.hint.find(map2) != std::string::npos);
    }
    CHECK(fixture.host().documentList.size() == 2);
    CHECK(fixture.callAs(sessionC, "document_list", Json::object())["activeDocument"]
            .is_null());
  }

  SECTION("activating another document notifies the session's status subscription")
  {
    auto notifications = std::make_shared<CapturingNotificationStream>();
    REQUIRE(fixture.server().openNotificationStream(sessionB, notifications));
    const auto subscribed = fixture.post(
      sessionB,
      jsonrpc::makeRequest(
        9003, "resources/subscribe", Json{{"uri", "trenchbroom://editor/status"}}));
    REQUIRE(subscribed->response.has_value());

    // session A's choice does not concern session B
    fixture.callAs(sessionA, "document_activate", Json{{"document", map2}});
    CHECK(notifications->notifications.empty());

    fixture.callAs(sessionB, "document_activate", Json{{"document", map1}});
    REQUIRE(notifications->notifications.size() == 1);
    CHECK(
      notifications->notifications[0]["params"]["uri"] == "trenchbroom://editor/status");
  }
}

TEST_CASE("DocumentTargeting in single window mode")
{
  auto fixture = McpToolFixture{};
  fixture.host().singleWindow = true;

  const auto sessionA = fixture.sessionId();
  const auto created = fixture.call("document_new", Json{{"game", "Quake"}});
  const auto map1 = created["result"]["document"]["id"].get<std::string>();

  // another session must not replace session A's document
  const auto sessionB = fixture.openSession("subagent");
  const auto error =
    fixture.callExpectingErrorAs(sessionB, "document_new", Json{{"game", "Quake"}});
  CHECK(error.code == ErrorCode::DocumentInUse);
  CHECK(error.message.find(map1) != std::string::npos);
  CHECK(
    fixture
      .callExpectingErrorAs(
        sessionB, "document_open", Json{{"path", cubeMapPath().string()}})
      .code
    == ErrorCode::DocumentInUse);
  REQUIRE(fixture.host().documentList.size() == 1);
  CHECK(fixture.host().documentList[0].id == map1);

  // session A may replace its own document
  const auto replaced = fixture.callAs(
    sessionA, "document_new", Json{{"game", "Quake"}, {"unsavedChanges", "discard"}});
  CHECK(replaced["result"]["replaced"] == map1);
}

} // namespace tb::mcp
