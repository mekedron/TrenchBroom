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

#include "base/Logger.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/CallRunner.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ObjectIds.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/NodeContents.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
using namespace schema;
using namespace std::chrono_literals;

namespace
{

ToolResult addEntity(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto* entityNode = new mdl::EntityNode{mdl::Entity{{{"classname", "info_null"}}}};
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {entityNode}}});

  if (args.get<bool>("fail"))
  {
    return makeError(ErrorCode::InvalidGeometry, "Failing on purpose.");
  }
  if (args.get<bool>("throw"))
  {
    throw std::runtime_error{"boom"};
  }
  if (args.get<bool>("logError"))
  {
    context.document().logger().error() << "Something went wrong";
    return context.operationFailed("Could not do it.");
  }
  return Json{{"entity", context.ids().format(*entityNode)}};
}

ToolResult setWorldMessage(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto entity = map.worldNode().entity();
  entity.addOrUpdateProperty("message", args.get<std::string>("message"));
  mdl::updateNodeContents(
    map, "Set Message", {{&map.worldNode(), mdl::NodeContents{std::move(entity)}}});
  return Json::object();
}

/** Runs two deferred steps; fails or throws in the second step if asked to. */
void asyncSteps(CallContext& context, const Args& args, ToolCompletion completion)
{
  const auto throwInStep = args.get<bool>("throw");
  context.progress(0, 2, "first step");
  context.defer([&context, completion, throwInStep]() {
    if (context.cancelled())
    {
      completion(makeError(ErrorCode::Cancelled, "cancelled"));
      return;
    }
    context.progress(1, 2, "second step");
    context.defer([&context, completion, throwInStep]() {
      if (throwInStep)
      {
        throw std::runtime_error{"boom"};
      }
      context.warn("TEST_WARNING", "async warning");
      completion(Json{
        {"steps", 2},
        {"document",
         context.hasDocument() ? Json(context.documentInfo().id) : Json(nullptr)}});
    });
  });
}

/** A read-only asynchronous call: waits 100 ms, then completes in a second step. */
void asyncRead(CallContext& context, const Args&, ToolCompletion completion)
{
  context.defer(
    [&context, completion]() {
      if (context.cancelled())
      {
        completion(makeError(ErrorCode::Cancelled, "cancelled"));
        return;
      }
      context.defer([&context, completion]() {
        context.addText("label");
        completion(Json{
          {"steps", 2},
          {"document",
           context.hasDocument() ? Json(context.documentInfo().id) : Json(nullptr)}});
      });
    },
    100ms);
}

void registerTestTools(McpServer& server)
{
  server.tools().add(ToolDef{"test_async_read"}
                       .title("Async Read")
                       .mutation(Mutation::None)
                       .documentUse(DocumentUse::Optional)
                       .asyncHandler(asyncRead));

  server.tools().add(ToolDef{"test_set_message"}
                       .title("Set Message")
                       .input(object({field("message", string()).required()}))
                       .mutation(Mutation::Map)
                       .handler(setWorldMessage));

  server.tools().add(ToolDef{"test_async"}
                       .title("Async")
                       .input(object({field("throw", boolean().defaultsTo(false))}))
                       .mutation(Mutation::External)
                       .documentUse(DocumentUse::Optional)
                       .asyncHandler(asyncSteps));

  server.tools().add(ToolDef{"test_add_entity"}
                       .title("Add Test Entity")
                       .input(object({
                         field("fail", boolean().defaultsTo(false)),
                         field("throw", boolean().defaultsTo(false)),
                         field("logError", boolean().defaultsTo(false)),
                       }))
                       .mutation(Mutation::Map)
                       .handler(addEntity));

  server.tools().add(
    ToolDef{"test_noop"}
      .title("Do Nothing")
      .mutation(Mutation::Map)
      .handler([](CallContext&, const Args&) -> ToolResult { return Json::object(); }));

  server.tools().add(ToolDef{"test_external"}
                       .title("External")
                       .mutation(Mutation::External)
                       .documentUse(DocumentUse::Optional)
                       .handler([](CallContext& context, const Args&) -> ToolResult {
                         return Json{{"wouldDo", context.dryRun()}};
                       }));

  server.tools().add(ToolDef{"test_image"}
                       .title("Image")
                       .documentUse(DocumentUse::None)
                       .handler([](CallContext& context, const Args&) -> ToolResult {
                         context.addImage("abc", "image/png");
                         return Json{{"width", 1}};
                       }));

  server.tools().add(
    ToolDef{"test_read"}
      .title("Read")
      .documentUse(DocumentUse::Optional)
      .handler([](CallContext& context, const Args&) -> ToolResult {
        context.warn("TEST_WARNING", "just so you know");
        return Json{
          {"document",
           context.hasDocument() ? Json(context.documentInfo().id) : Json(nullptr)}};
      }));
}

size_t entityCount(ui::MapDocument& document)
{
  return document.map().worldNode().defaultLayer()->childCount();
}

Json callRequest(const int id, const std::string& tool, Json arguments = Json::object())
{
  return jsonrpc::makeRequest(
    id, "tools/call", Json{{"name", tool}, {"arguments", std::move(arguments)}});
}

} // namespace

TEST_CASE("CallRunner")
{
  auto fixture = McpToolFixture{};
  registerTestTools(fixture.server());
  auto& document = fixture.create();
  auto& map = document.map();

  SECTION("a modifying call is one undo step named after the tool")
  {
    const auto result = fixture.call("test_add_entity");
    CHECK(result["ok"] == true);
    CHECK(result["dryRun"] == false);
    CHECK(result["undoStep"] == "AI: Add Test Entity");
    CHECK(result["changes"]["created"] == Json::array({result["result"]["entity"]}));
    CHECK(result["changes"]["modified"] == Json::array({"layer:default"}));
    CHECK(result["selection"]["mode"] == "none");
    CHECK(result["grid"] == 16.0);
    CHECK(result["warnings"] == Json::array());

    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "AI: Add Test Entity");
    CHECK(map.commandProcessor().undoCommandNames().size() == 1);
    CHECK(entityCount(document) == 1);
  }

  SECTION("a call that changes nothing creates no undo step")
  {
    const auto result = fixture.call("test_noop");
    CHECK(result["undoStep"].is_null());
    CHECK(!map.canUndoCommand());
  }

  SECTION("a failed call leaves no trace")
  {
    const auto modificationCount = map.modificationCount();

    const auto error =
      fixture.callExpectingError("test_add_entity", Json{{"fail", true}});
    CHECK(error.code == ErrorCode::InvalidGeometry);
    CHECK(entityCount(document) == 0);
    CHECK(map.modificationCount() == modificationCount);
    CHECK(!map.canUndoCommand());
    CHECK(map.commandProcessor().transactionDepth() == 0);
  }

  SECTION("an exception becomes INTERNAL_ERROR and leaves no trace")
  {
    const auto error =
      fixture.callExpectingError("test_add_entity", Json{{"throw", true}});
    CHECK(error.code == ErrorCode::InternalError);
    CHECK(error.message.find("boom") != std::string::npos);
    CHECK(entityCount(document) == 0);
    CHECK(map.commandProcessor().transactionDepth() == 0);
  }

  SECTION("operationFailed includes the messages the editor logged")
  {
    const auto error =
      fixture.callExpectingError("test_add_entity", Json{{"logError", true}});
    CHECK(error.code == ErrorCode::OperationFailed);
    CHECK(error.message.find("Something went wrong") != std::string::npos);
    CHECK(error.details["editorMessages"] == Json::array({"Something went wrong"}));
  }

  SECTION("dry run")
  {
    fixture.call("test_add_entity");
    map.undoCommand();
    REQUIRE(map.canRedoCommand());
    const auto modificationCount = map.modificationCount();

    const auto result = fixture.call("test_add_entity", Json{{"dryRun", true}});
    CHECK(result["dryRun"] == true);
    CHECK(result["undoStep"].is_null());
    CHECK(result["changes"]["created"].size() == 1);
    CHECK(result["changes"]["ephemeral"] == true);

    CHECK(entityCount(document) == 0);
    CHECK(map.modificationCount() == modificationCount);
    CHECK(!map.canUndoCommand());
    CHECK(map.canRedoCommand());
    CHECK(*map.redoCommandName() == "AI: Add Test Entity");
  }

  SECTION("external tools honor dry run and get no transaction")
  {
    const auto result = fixture.call("test_external", Json{{"dryRun", true}});
    CHECK(result["dryRun"] == true);
    CHECK(result["result"]["wouldDo"] == true);
    CHECK(!result.contains("changes"));
  }

  SECTION("read-only tools return their result directly, with warnings")
  {
    const auto result = fixture.call("test_read");
    CHECK(result["document"] == fixture.documentId(document));
    CHECK(result["warnings"][0]["code"] == "TEST_WARNING");
  }

  SECTION("images follow the text content")
  {
    const auto result = fixture.callRaw("test_image");
    CHECK(result["isError"] == false);
    REQUIRE(result["content"].size() == 2);
    CHECK(result["content"][0]["type"] == "text");
    CHECK(
      result["content"][1]
      == Json{{"type", "image"}, {"data", "YWJj"}, {"mimeType", "image/png"}});
    CHECK(result["structuredContent"] == Json{{"width", 1}});
  }

  SECTION("notes from prepareForAgentEdit become warnings")
  {
    fixture.host().prepareNotes = {"deactivated tool: Vertex Tool"};
    const auto result = fixture.call("test_add_entity");
    CHECK(fixture.host().prepareCount == 1);
    CHECK(result["warnings"][0]["message"] == "deactivated tool: Vertex Tool");
  }

  SECTION("calls within an agent transaction")
  {
    fixture.call("transaction_begin", Json{{"name", "Build"}});
    fixture.call("test_add_entity");
    fixture.call("test_add_entity");

    SECTION("a failed call only rolls back itself")
    {
      fixture.callExpectingError("test_add_entity", Json{{"fail", true}});
      CHECK(entityCount(document) == 2);
      CHECK(map.commandProcessor().transactionDepth() == 1);
    }

    SECTION("commit creates one undo step")
    {
      const auto result = fixture.call("transaction_commit");
      CHECK(result["undoStep"] == "AI: Build");
      CHECK(
        map.commandProcessor().undoCommandNames()
        == std::vector<std::string>{"AI: Build"});
      CHECK(entityCount(document) == 2);
    }

    SECTION("rollback leaves no trace")
    {
      const auto result = fixture.call("transaction_rollback");
      CHECK(result["changes"]["removed"].size() == 2);
      CHECK(!map.canUndoCommand());
      CHECK(entityCount(document) == 0);
    }

    SECTION("other sessions cannot modify the document")
    {
      const auto other = fixture.openSession("other");
      const auto error =
        fixture.callExpectingErrorAs(other, "test_add_entity", Json::object());
      CHECK(error.code == ErrorCode::TransactionActive);
      CHECK(error.message.find("test-client") != std::string::npos);

      // but they can read
      fixture.callAs(other, "test_read", Json::object());
    }

    SECTION("closing the session rolls the transaction back")
    {
      fixture.server().deleteSession(fixture.sessionId());
      CHECK(map.commandProcessor().transactionDepth() == 0);
      CHECK(entityCount(document) == 0);
    }

    SECTION("closing the document drops the transaction")
    {
      fixture.host().removeDocument(document);
      CHECK(map.commandProcessor().transactionDepth() == 0);
      CHECK(fixture.server().activity().openTransactions.empty());
    }
  }

  SECTION("busy gate")
  {
    auto& server = fixture.server();

    SECTION("modifying calls wait while the human is busy")
    {
      fixture.host().busy = BusyState::Busy;
      auto stream = fixture.post(fixture.sessionId(), callRequest(1, "test_add_entity"));
      CHECK(!stream->response);
      CHECK(server.activity().state == ServerActivity::State::WaitingForUser);
      CHECK(server.activity().toolTitle == "Add Test Entity");

      fixture.scheduler().advance(100ms);
      CHECK(!stream->response);

      // read-only calls are not blocked
      fixture.call("test_read");
      CHECK(!stream->response);

      fixture.host().busy = BusyState::Idle;
      fixture.scheduler().advance(50ms);
      REQUIRE(stream->response);
      CHECK((*stream->response)["result"]["isError"] == false);
      CHECK(entityCount(document) == 1);
      CHECK(server.activity().state == ServerActivity::State::Idle);
    }

    SECTION("calls run in order")
    {
      fixture.host().busy = BusyState::Busy;
      auto first = fixture.post(fixture.sessionId(), callRequest(1, "test_add_entity"));
      auto second = fixture.post(fixture.sessionId(), callRequest(2, "undo"));
      fixture.host().busy = BusyState::Idle;
      fixture.scheduler().advance(50ms);

      REQUIRE(first->response);
      REQUIRE(second->response);
      CHECK(entityCount(document) == 0);
      CHECK(map.canRedoCommand());
    }

    SECTION("a transaction the human opened makes the document busy")
    {
      map.startTransaction("drag", mdl::TransactionScope::LongRunning);
      auto stream = fixture.post(fixture.sessionId(), callRequest(1, "test_add_entity"));
      CHECK(!stream->response);

      map.commitTransaction();
      fixture.scheduler().advance(50ms);
      CHECK(stream->response);
    }

    SECTION("calls time out")
    {
      fixture.host().busy = BusyState::Busy;
      auto stream = fixture.post(fixture.sessionId(), callRequest(1, "test_add_entity"));
      fixture.scheduler().advance(30s);
      REQUIRE(stream->response);
      CHECK(
        (*stream->response)["result"]["structuredContent"]["error"]["code"]
        == "BUSY_TIMEOUT");
      CHECK(entityCount(document) == 0);
    }

    SECTION("invalid arguments fail immediately")
    {
      fixture.host().busy = BusyState::Busy;
      auto stream = fixture.post(
        fixture.sessionId(), callRequest(1, "test_add_entity", Json{{"fail", "yes"}}));
      REQUIRE(stream->response);
      CHECK(
        (*stream->response)["result"]["structuredContent"]["error"]["code"]
        == "INVALID_ARGUMENT");
    }
  }

  SECTION("document targeting")
  {
    auto& other = fixture.create();
    const auto firstId = fixture.documentId(document);
    const auto otherId = fixture.documentId(other);

    SECTION("defaults to the session's active document, not the focused one")
    {
      CHECK(fixture.call("test_read")["document"] == otherId);
      fixture.host().setFocused(firstId);
      CHECK(fixture.call("test_read")["document"] == otherId);
    }

    SECTION("uses the explicit document argument")
    {
      fixture.call("test_add_entity", Json{{"document", firstId}});
      CHECK(entityCount(document) == 1);
      CHECK(entityCount(other) == 0);
    }

    SECTION("uses the activated document")
    {
      fixture.call("document_activate", Json{{"document", firstId}});
      CHECK(fixture.call("test_read")["document"] == firstId);

      // other sessions are not affected
      const auto session = fixture.openSession();
      CHECK(fixture.callAs(session, "test_read", Json::object())["document"] == otherId);
    }

    SECTION("fails for unknown documents")
    {
      CHECK(
        fixture.callExpectingError("test_add_entity", Json{{"document", "doc:99"}}).code
        == ErrorCode::DocumentNotFound);
    }

    SECTION("fails without documents")
    {
      fixture.host().removeDocument(document);
      fixture.host().removeDocument(other);
      CHECK(
        fixture.callExpectingError("test_add_entity").code
        == ErrorCode::ActiveDocumentClosed);

      // a tool that does not need a document runs without one and is warned
      const auto read = fixture.call("test_read");
      CHECK(read["document"].is_null());
      CHECK(std::ranges::any_of(read["warnings"], [](const auto& warning) {
        return warning["code"] == "ACTIVE_DOCUMENT_CLOSED";
      }));

      const auto session = fixture.openSession();
      CHECK(
        fixture.callExpectingErrorAs(session, "test_add_entity", Json::object()).code
        == ErrorCode::NoDocument);
      CHECK(fixture.callAs(session, "test_read", Json::object())["document"].is_null());
    }
  }

  SECTION("consecutive calls changing the same object are separate undo steps")
  {
    map.setIsCommandCollationEnabled(true);

    fixture.call("test_set_message", Json{{"message", "a"}});
    fixture.call("test_set_message", Json{{"message", "b"}});
    CHECK(map.commandProcessor().undoCommandNames().size() == 2);
    CHECK(map.isCommandCollationEnabled());
  }

  SECTION("asynchronous calls")
  {
    SECTION("complete in steps and report progress")
    {
      auto stream = fixture.post(
        fixture.sessionId(),
        jsonrpc::makeRequest(
          500,
          "tools/call",
          Json{
            {"name", "test_async"},
            {"arguments", Json::object()},
            {"_meta", Json{{"progressToken", 7}}},
          }));
      CHECK(!stream->response.has_value());
      CHECK(stream->notifications.size() == 1);

      // a modifying call waits in the queue until the asynchronous call is done
      auto queued =
        fixture.post(fixture.sessionId(), callRequest(501, "test_add_entity"));
      CHECK(!queued->response.has_value());

      fixture.scheduler().runPending();
      REQUIRE(stream->response.has_value());
      const auto& result = (*stream->response)["result"]["structuredContent"];
      CHECK(result["ok"] == true);
      CHECK(result["undoStep"].is_null());
      CHECK(result["result"]["steps"] == 2);
      CHECK(result["result"]["document"] == fixture.documentId(document));
      CHECK(result["warnings"][0]["code"] == "TEST_WARNING");
      CHECK(result["grid"] == 16.0);
      CHECK(stream->notifications.size() == 2);

      REQUIRE(queued->response.has_value());
      CHECK(entityCount(document) == 1);
      CHECK(fixture.server().callLog().entries().front().tool == "test_async");
    }

    SECTION("can be cancelled between steps")
    {
      auto stream = fixture.post(fixture.sessionId(), callRequest(510, "test_async"));
      fixture.post(
        fixture.sessionId(),
        jsonrpc::makeNotification("notifications/cancelled", Json{{"requestId", 510}}));
      fixture.scheduler().runPending();

      REQUIRE(stream->response.has_value());
      CHECK(
        (*stream->response)["result"]["structuredContent"]["error"]["code"]
        == "CANCELLED");
    }

    SECTION("are abandoned when the session closes")
    {
      auto stream = fixture.post(fixture.sessionId(), callRequest(520, "test_async"));
      fixture.server().deleteSession(fixture.sessionId());

      REQUIRE(stream->response.has_value());
      CHECK(
        (*stream->response)["result"]["structuredContent"]["error"]["code"]
        == "CANCELLED");
      // the remaining steps are dropped
      fixture.scheduler().runPending();
    }

    SECTION("fail when the document is closed meanwhile")
    {
      auto stream = fixture.post(fixture.sessionId(), callRequest(530, "test_async"));
      fixture.host().removeDocument(document);
      fixture.scheduler().runPending();

      REQUIRE(stream->response.has_value());
      CHECK(
        (*stream->response)["result"]["structuredContent"]["error"]["code"]
        == "DOCUMENT_NOT_FOUND");
    }

    SECTION("exceptions in a step become INTERNAL_ERROR")
    {
      const auto error = fixture.callExpectingError("test_async", Json{{"throw", true}});
      CHECK(error.code == ErrorCode::InternalError);
      // the queue continues
      fixture.call("test_add_entity");
    }
  }

  SECTION("asynchronous read-only calls")
  {
    const auto structured = [](const auto& stream) {
      return (*stream->response)["result"]["structuredContent"];
    };

    SECTION("run immediately, even while the human is busy, and return their result")
    {
      fixture.host().busy = BusyState::Busy;
      auto queued =
        fixture.post(fixture.sessionId(), callRequest(600, "test_add_entity"));
      auto stream =
        fixture.post(fixture.sessionId(), callRequest(601, "test_async_read"));
      CHECK(!stream->response.has_value());

      // the first step waits for its delay
      fixture.scheduler().runPending();
      CHECK(!stream->response.has_value());

      fixture.scheduler().advance(100ms);
      REQUIRE(stream->response.has_value());
      CHECK(structured(stream)["steps"] == 2);
      CHECK(structured(stream)["document"] == fixture.documentId(document));
      CHECK(!structured(stream).contains("ok"));
      const auto& content = (*stream->response)["result"]["content"];
      REQUIRE(content.size() == 2);
      CHECK(content[1] == Json{{"type", "text"}, {"text", "label"}});

      // the modifying call still waits for the human
      CHECK(!queued->response.has_value());
      CHECK(fixture.server().activity().state == ServerActivity::State::WaitingForUser);
      fixture.host().busy = BusyState::Idle;
      fixture.scheduler().advance(50ms);
      REQUIRE(queued->response.has_value());
    }

    SECTION("run while an asynchronous modifying call runs")
    {
      auto modifying = fixture.post(fixture.sessionId(), callRequest(610, "test_async"));
      auto reading =
        fixture.post(fixture.sessionId(), callRequest(611, "test_async_read"));
      fixture.scheduler().advance(100ms);

      REQUIRE(modifying->response.has_value());
      REQUIRE(reading->response.has_value());
      CHECK(structured(modifying)["ok"] == true);
      CHECK(structured(reading)["steps"] == 2);
    }

    SECTION("several run at the same time")
    {
      auto first = fixture.post(fixture.sessionId(), callRequest(620, "test_async_read"));
      auto second =
        fixture.post(fixture.sessionId(), callRequest(621, "test_async_read"));
      fixture.scheduler().advance(100ms);
      REQUIRE(first->response.has_value());
      REQUIRE(second->response.has_value());
      CHECK(fixture.server().activity().state == ServerActivity::State::Idle);
    }

    SECTION("can be cancelled between steps")
    {
      auto stream =
        fixture.post(fixture.sessionId(), callRequest(630, "test_async_read"));
      fixture.post(
        fixture.sessionId(),
        jsonrpc::makeNotification("notifications/cancelled", Json{{"requestId", 630}}));
      fixture.scheduler().advance(100ms);

      REQUIRE(stream->response.has_value());
      CHECK(structured(stream)["error"]["code"] == "CANCELLED");
    }

    SECTION("are abandoned when the session closes")
    {
      auto stream =
        fixture.post(fixture.sessionId(), callRequest(640, "test_async_read"));
      fixture.server().deleteSession(fixture.sessionId());

      REQUIRE(stream->response.has_value());
      CHECK(structured(stream)["error"]["code"] == "CANCELLED");
      fixture.scheduler().advance(100ms);
    }

    SECTION("fail when the document is closed meanwhile")
    {
      auto stream =
        fixture.post(fixture.sessionId(), callRequest(650, "test_async_read"));
      fixture.host().removeDocument(document);
      fixture.scheduler().advance(100ms);

      REQUIRE(stream->response.has_value());
      CHECK(structured(stream)["error"]["code"] == "DOCUMENT_NOT_FOUND");
    }

    SECTION("create no undo step")
    {
      fixture.call("test_async_read");
      CHECK(!map.canUndoCommand());
    }
  }

  SECTION("calls are logged")
  {
    fixture.call("test_add_entity");
    fixture.callExpectingError("test_add_entity", Json{{"fail", true}});

    const auto& entries = fixture.server().callLog().entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].tool == "test_add_entity");
    CHECK(entries[0].ok);
    CHECK(entries[0].undoStep == "AI: Add Test Entity");
    CHECK(entries[0].created == 1);
    CHECK(entries[0].clientName == "test-client 1.0");
    CHECK(!entries[1].ok);
    CHECK(entries[1].errorCode == "INVALID_GEOMETRY");
  }
}

} // namespace tb::mcp
