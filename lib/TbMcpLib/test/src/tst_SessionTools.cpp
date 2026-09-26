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

#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "ui/MapDocument.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

TEST_CASE("SessionTools")
{
  auto fixture = McpToolFixture{};

  SECTION("editor_status")
  {
    SECTION("without documents")
    {
      const auto status = fixture.call("editor_status");
      CHECK(status["version"] == "test-version");
      CHECK(status["protocolVersion"] == "2025-11-25");
      CHECK(status["sessions"] == 1);
      CHECK(status["documents"] == Json::array());
      CHECK(status["activeDocument"].is_null());
      CHECK(status["tool"].is_null());
      CHECK(status["grid"].is_null());
      CHECK(status["locks"].contains("alignmentLock"));
      CHECK(status["locks"].contains("uvLock"));
      CHECK(status["agentActivity"]["state"] == "running");
    }

    SECTION("with documents")
    {
      auto& document = fixture.create();
      auto& map = document.map();
      auto* brushNode = mdl::createBrushNode(map);
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
      mdl::selectNodes(map, {brushNode});

      fixture.host().toolName = "Vertex Tool";
      fixture.host().compileRunning = true;

      const auto status = fixture.call("editor_status");
      const auto documentId = fixture.documentId(document);
      CHECK(status["activeDocument"] == documentId);
      REQUIRE(status["documents"].size() == 1);
      CHECK(status["documents"][0]["id"] == documentId);
      CHECK(status["documents"][0]["active"] == true);
      CHECK(status["tool"] == "Vertex Tool");
      CHECK(status["compileRunning"] == true);
      CHECK(status["grid"]["size"] == 16.0);
      CHECK(status["transaction"].is_null());
      CHECK(status["selection"]["ids"] == Json::array({fixture.id(*brushNode)}));

      fixture.call("transaction_begin", Json{{"name", "Work"}});
      CHECK(
        fixture.call("editor_status")["transaction"]
        == Json{{"name", "Work"}, {"owner", "test-client 1.0"}});
    }
  }

  SECTION("document_list")
  {
    CHECK(
      fixture.call("document_list")
      == Json{{"documents", Json::array()}, {"activeDocument", nullptr}});

    auto& first = fixture.create();
    auto& second = fixture.create();

    const auto result = fixture.call("document_list");
    REQUIRE(result["documents"].size() == 2);
    const auto& summary = result["documents"][0];
    CHECK(summary["id"] == fixture.documentId(first));
    CHECK(summary["title"] == "unnamed1");
    CHECK(summary["path"].is_null());
    CHECK(summary["game"].is_string());
    CHECK(summary["format"] == "Standard");
    CHECK(summary["modified"] == false);
    CHECK(summary["focused"] == false);
    CHECK(summary["active"] == false);
    CHECK(result["documents"][1]["focused"] == true);
    CHECK(result["activeDocument"] == fixture.documentId(second));
  }

  SECTION("document_activate")
  {
    auto& first = fixture.create();
    fixture.create();
    const auto firstId = fixture.documentId(first);

    const auto result = fixture.call("document_activate", Json{{"document", firstId}});
    CHECK(result["activeDocument"] == firstId);
    CHECK(result["document"]["active"] == true);
    CHECK(fixture.call("document_list")["activeDocument"] == firstId);

    CHECK(
      fixture.callExpectingError("document_activate", Json{{"document", "doc:99"}}).code
      == ErrorCode::DocumentNotFound);
    CHECK(
      fixture.callExpectingError("document_activate", Json{{"document", "x"}}).code
      == ErrorCode::InvalidArgument);
    CHECK(
      fixture.callExpectingError("document_activate", Json::object()).code
      == ErrorCode::InvalidArgument);

    SECTION("falls back to the focused document when the activated one is closed")
    {
      fixture.host().removeDocument(first);
      CHECK(fixture.call("document_list")["activeDocument"] != firstId);
    }
  }

  SECTION("session_log")
  {
    fixture.create();
    fixture.call("editor_status");
    fixture.callExpectingError("undo");

    const auto other = fixture.openSession("other");
    fixture.callAs(other, "document_list", Json::object());

    SECTION("returns this session's calls, newest first")
    {
      const auto result = fixture.call("session_log");
      REQUIRE(result["items"].size() == 2);
      CHECK(result["total"] == 2);
      CHECK(result["items"][0]["tool"] == "undo");
      CHECK(result["items"][0]["ok"] == false);
      CHECK(result["items"][0]["errorCode"] == "OPERATION_FAILED");
      CHECK(!result["items"][0].contains("arguments"));
      CHECK(result["items"][1]["tool"] == "editor_status");
    }

    SECTION("filters")
    {
      CHECK(fixture.call("session_log", Json{{"scope", "all"}})["total"] == 3);
      CHECK(fixture.call("session_log", Json{{"errorsOnly", true}})["total"] == 1);
      CHECK(fixture.call("session_log", Json{{"tool", "editor_status"}})["total"] == 1);
    }

    SECTION("paginates")
    {
      const auto page1 =
        fixture.call("session_log", Json{{"limit", 1}, {"detail", "full"}});
      CHECK(page1["items"].size() == 1);
      CHECK(page1["items"][0].contains("arguments"));
      REQUIRE(page1["nextCursor"].is_string());

      const auto page2 =
        fixture.call("session_log", Json{{"limit", 1}, {"cursor", page1["nextCursor"]}});
      CHECK(page2["items"].size() == 1);
      // the first session_log call was logged meanwhile
      CHECK(page2["stale"] == true);
    }
  }
}

} // namespace tb::mcp
