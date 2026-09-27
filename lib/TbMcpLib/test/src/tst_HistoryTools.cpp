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
#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestFactory.h"
#include "mdl/TransactionScope.h"
#include "ui/MapDocument.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

mdl::BrushNode* addBrush(mdl::Map& map)
{
  auto* brushNode = mdl::createBrushNode(map);
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
  return brushNode;
}

} // namespace

TEST_CASE("HistoryTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("history_get")
  {
    addBrush(map);
    map.startTransaction("AI: Create Box Brush", mdl::TransactionScope::Oneshot);
    addBrush(map);
    map.commitTransaction();
    addBrush(map);
    map.undoCommand();

    const auto result = fixture.call("history_get");
    CHECK(result["undoCount"] == 2);
    CHECK(result["redoCount"] == 1);
    CHECK(result["undo"][0] == Json{{"name", "AI: Create Box Brush"}, {"agent", true}});
    CHECK(result["undo"][1]["agent"] == false);
    CHECK(result["redo"].size() == 1);
    CHECK(result["transaction"].is_null());

    CHECK(fixture.call("history_get", Json{{"limit", 1}})["undo"].size() == 1);
  }

  SECTION("undo")
  {
    auto* brushNode = addBrush(map);
    const auto brushId = fixture.id(*brushNode);
    addBrush(map);

    SECTION("undoes steps and reports the changes")
    {
      const auto result = fixture.call("undo", Json{{"count", 2}});
      CHECK(result["result"]["undone"].size() == 2);
      CHECK(result["undoStep"].is_null());
      CHECK(result["changes"]["removed"].size() == 2);
      CHECK(!map.canUndoCommand());
      CHECK(fixture.node(brushId) == nullptr);
    }

    SECTION("dry run")
    {
      const auto result = fixture.call("undo", Json{{"dryRun", true}});
      CHECK(result["result"]["wouldUndo"].size() == 1);
      CHECK(map.commandProcessor().undoCommandNames().size() == 2);
    }

    SECTION("fails if there are too few steps")
    {
      CHECK(
        fixture.callExpectingError("undo", Json{{"count", 3}}).code
        == ErrorCode::InvalidArgument);
      CHECK(map.commandProcessor().undoCommandNames().size() == 2);
    }

    SECTION("fails if there is nothing to undo")
    {
      map.undoCommand();
      map.undoCommand();
      CHECK(fixture.callExpectingError("undo").code == ErrorCode::OperationFailed);
    }

    SECTION("fails while an agent transaction is open")
    {
      fixture.call("transaction_begin", Json{{"name", "x"}});
      CHECK(fixture.callExpectingError("undo").code == ErrorCode::TransactionActive);
    }

    SECTION("fails for invalid counts")
    {
      CHECK(
        fixture.callExpectingError("undo", Json{{"count", 0}}).code
        == ErrorCode::InvalidArgument);
    }
  }

  SECTION("redo")
  {
    auto* brushNode = addBrush(map);
    const auto brushId = fixture.id(*brushNode);

    CHECK(fixture.callExpectingError("redo").code == ErrorCode::OperationFailed);

    fixture.call("undo");
    CHECK(fixture.node(brushId) == nullptr);

    const auto dryRun = fixture.call("redo", Json{{"dryRun", true}});
    CHECK(dryRun["result"]["wouldRedo"].size() == 1);
    CHECK(map.canRedoCommand());

    const auto result = fixture.call("redo");
    CHECK(result["result"]["redone"].size() == 1);
    CHECK(result["changes"]["created"] == Json::array({brushId}));
    CHECK(fixture.node(brushId) == brushNode);
  }

  SECTION("transaction_begin")
  {
    const auto result = fixture.call("transaction_begin", Json{{"name", "Build"}});
    CHECK(result["result"]["transaction"] == "Build");
    CHECK(map.commandProcessor().transactionDepth() == 1);
    CHECK(
      fixture.server().activity().openTransactions == std::vector<std::string>{"Build"});

    CHECK(
      fixture.callExpectingError("transaction_begin", Json{{"name", "Again"}}).code
      == ErrorCode::TransactionActive);
    CHECK(
      fixture.callExpectingError("transaction_begin", Json{{"name", ""}}).code
      == ErrorCode::InvalidArgument);

    SECTION("dry run")
    {
      fixture.call("transaction_rollback");
      const auto dryRun =
        fixture.call("transaction_begin", Json{{"name", "Build"}, {"dryRun", true}});
      CHECK(dryRun["result"]["wouldBegin"] == "Build");
      CHECK(map.commandProcessor().transactionDepth() == 0);
    }
  }

  SECTION("transaction_commit")
  {
    CHECK(
      fixture.callExpectingError("transaction_commit").code == ErrorCode::NoTransaction);

    fixture.call("transaction_begin", Json{{"name", "Build"}});

    SECTION("an empty transaction creates no undo step")
    {
      const auto result = fixture.call("transaction_commit");
      CHECK(result["result"]["committed"] == "Build");
      CHECK(result["result"]["empty"] == true);
      CHECK(result["undoStep"].is_null());
      CHECK(map.commandProcessor().transactionDepth() == 0);
      CHECK(!map.canUndoCommand());
    }

    SECTION("human edits become part of the transaction")
    {
      addBrush(map);
      addBrush(map);
      const auto result = fixture.call("transaction_commit");
      CHECK(result["undoStep"] == "AI: Build");
      CHECK(
        map.commandProcessor().undoCommandNames()
        == std::vector<std::string>{"AI: Build"});
      CHECK(fixture.server().activity().openTransactions.empty());
    }

    SECTION("calls report no undo step and the commit reports their net changes")
    {
      auto* existing = addBrush(map);
      const auto existingId = fixture.id(*existing);

      const auto kept =
        fixture.call("brush_create_box", Json{{"min", {0, 0, 0}}, {"max", {16, 16, 16}}});
      CHECK(kept["undoStep"].is_null());
      CHECK(kept["changes"]["created"].size() == 1);
      const auto keptId = kept["result"]["brush"].get<std::string>();

      const auto discarded = fixture.call(
        "brush_create_box", Json{{"min", {32, 0, 0}}, {"max", {48, 16, 16}}});
      const auto discardedId = discarded["result"]["brush"].get<std::string>();

      const auto moved = fixture.call(
        "objects_move", Json{{"ids", {keptId, existingId}}, {"vector", {0, 0, 16}}});
      CHECK(moved["undoStep"].is_null());
      fixture.call("objects_delete", Json{{"ids", {discardedId}}});

      // a dry run inside the transaction is not part of it
      fixture.call("objects_delete", Json{{"ids", {existingId}}, {"dryRun", true}});

      const auto result = fixture.call("transaction_commit");
      CHECK(result["undoStep"] == "AI: Build");
      CHECK(result["changes"]["created"] == Json::array({keptId}));
      // the layer is modified because objects were added to it
      CHECK(result["changes"]["modified"] == Json::array({"layer:default", existingId}));
      CHECK(result["changes"]["removed"].empty());
      CHECK(
        map.commandProcessor().undoCommandNames()
        == std::vector<std::string>{"AI: Build"});

      // the next transaction starts with no changes
      fixture.call("transaction_begin", Json{{"name", "Next"}});
      const auto next = fixture.call("transaction_commit");
      CHECK(next["changes"]["created"].empty());
      CHECK(next["changes"]["modified"].empty());
    }

    SECTION("dry run")
    {
      CHECK(
        fixture.call(
          "transaction_commit", Json{{"dryRun", true}})["result"]["wouldCommit"]
        == "Build");
      CHECK(map.commandProcessor().transactionDepth() == 1);
    }

    SECTION("only the owning session can commit")
    {
      const auto other = fixture.openSession("other");
      CHECK(
        fixture.callExpectingErrorAs(other, "transaction_commit", Json::object()).code
        == ErrorCode::TransactionActive);
    }
  }

  SECTION("transaction_rollback")
  {
    CHECK(
      fixture.callExpectingError("transaction_rollback").code
      == ErrorCode::NoTransaction);

    auto* existing = addBrush(map);
    fixture.call("undo");
    REQUIRE(map.canRedoCommand());

    fixture.call("transaction_begin", Json{{"name", "Build"}});
    auto* brushNode = addBrush(map);
    const auto brushId = fixture.id(*brushNode);

    SECTION("dry run")
    {
      CHECK(
        fixture.call(
          "transaction_rollback", Json{{"dryRun", true}})["result"]["wouldRollBack"]
        == "Build");
      CHECK(map.commandProcessor().transactionDepth() == 1);
    }

    SECTION("discards the changes and keeps the redo history")
    {
      const auto result = fixture.call("transaction_rollback");
      CHECK(result["result"]["rolledBack"] == "Build");
      CHECK(result["changes"]["removed"] == Json::array({brushId}));
      CHECK(map.commandProcessor().transactionDepth() == 0);
      CHECK(!map.canUndoCommand());
      CHECK(map.canRedoCommand());

      fixture.call("redo");
      CHECK(fixture.node(fixture.id(*existing)) == existing);
    }
  }
}

} // namespace tb::mcp
