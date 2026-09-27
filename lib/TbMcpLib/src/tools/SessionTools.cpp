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

#include "mcp/tools/SessionTools.h"

#include "base/PreferenceManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/CallLog.h"
#include "mcp/ChangeCollector.h"
#include "mcp/Pagination.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>

namespace tb::mcp
{
namespace
{
using namespace schema;

Schema documentSummarySchema()
{
  return object({
    field("id", string()).required().describe("Document handle, e.g. 'doc:1'"),
    field("title", string()).describe("Window title"),
    field("path", any()).describe("Absolute file path, or null if never saved"),
    field("game", string()).describe("Game name, e.g. 'Quake'"),
    field("format", string()).describe("Map format, e.g. 'Valve'"),
    field("modified", boolean()).describe("Whether there are unsaved changes"),
    field("focused", boolean()).describe("Whether its window is the focused window"),
    field("background", boolean())
      .describe(
        "Whether it is a background document without an editor window (document_new / "
        "document_open with window: false); document_show gives it one"),
    field("active", boolean())
      .describe("Whether it is this session's active document, which calls without "
                "'document' act on"),
    field("activeIn", array(string()))
      .describe("The clients (sessions) that have it as their active document"),
  });
}

Field activeDocumentField()
{
  return field("activeDocument", any())
    .describe(
      "Handle of this session's active document (set by document_new, document_open, "
      "document_activate), or null");
}

Field activeDocumentClosedField()
{
  return field("activeDocumentClosed", any())
    .describe(
      "Handle of this session's active document if it was closed, else null; calls "
      "without 'document' then fail with ACTIVE_DOCUMENT_CLOSED until document_activate");
}

Field targetDocumentField()
{
  return field("targetDocument", any())
    .describe(
      "Handle of the document that calls without 'document' act on now: the active "
      "document, or the focused one if this session has none; null if there is none");
}

/** activeDocument, activeDocumentClosed and targetDocument of the given session. */
Json activeDocumentJson(ServerState& server, const Session& session)
{
  const auto target = server.targetDocument(session);
  const auto closed = target.source == DocumentTarget::Source::ActiveClosed;
  return Json{
    {"activeDocument",
     target.source == DocumentTarget::Source::Active ? Json(target.document->id)
                                                     : Json(nullptr)},
    {"activeDocumentClosed", closed ? Json(*session.activeDocumentId) : Json(nullptr)},
    {"targetDocument", target.document ? Json(target.document->id) : Json(nullptr)},
  };
}

ToolResult editorStatusTool(CallContext& context, const Args&)
{
  return editorStatus(context.server(), context.session());
}

ToolResult documentListTool(CallContext& context, const Args&)
{
  auto documents = Json::array();
  for (const auto& document : context.host().documents())
  {
    documents.push_back(documentSummary(context.server(), document, context.session()));
  }

  auto result = Json{{"documents", std::move(documents)}};
  result.update(activeDocumentJson(context.server(), context.session()));
  return result;
}

ToolResult documentActivateTool(CallContext& context, const Args& args)
{
  const auto documentId = args.get<std::string>("document");
  const auto document = context.server().findDocument(documentId);
  if (!document)
  {
    return makeError(
      ErrorCode::DocumentNotFound,
      "Document " + documentId + " is not open.",
      "Use document_list to see the open documents.");
  }

  context.server().setActiveDocument(context.session(), documentId);
  return Json{
    {"activeDocument", documentId},
    {"document", documentSummary(context.server(), *document, context.session())},
  };
}

ToolResult sessionLogTool(CallContext& context, const Args& args)
{
  const auto& log = context.server().callLog;
  const auto lastSeq =
    log.entries().empty() ? size_t{0} : size_t(log.entries().back().seq);

  auto request = pageRequest(args, lastSeq);
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto allSessions = args.get<std::string>("scope") == "all";
  const auto toolFilter = args.getOptional<std::string>("tool");
  const auto errorsOnly = args.get<bool>("errorsOnly");

  auto items = std::vector<Json>{};
  for (auto it = log.entries().rbegin(); it != log.entries().rend(); ++it)
  {
    const auto& entry = *it;
    if (!allSessions && entry.sessionId != context.session().id)
    {
      continue;
    }
    if (toolFilter && entry.tool != *toolFilter)
    {
      continue;
    }
    if (errorsOnly && entry.ok)
    {
      continue;
    }

    auto item = toJson(entry);
    if (request.value().detail == Detail::Summary)
    {
      item.erase("arguments");
    }
    items.push_back(std::move(item));
  }

  return makePage(items, request.value(), lastSeq);
}

} // namespace

Json documentSummary(
  ServerState& server, const DocumentInfo& document, const Session& session)
{
  const auto& map = document.document->map();
  return Json{
    {"id", document.id},
    {"title", document.windowTitle},
    {"path", map.persistent() ? Json(map.path().string()) : Json(nullptr)},
    {"game", map.gameInfo().gameConfig.name},
    {"format", mdl::formatName(map.worldNode().mapFormat())},
    {"modified", map.modified()},
    {"focused", document.focused},
    {"background", document.background},
    {"active", session.activeDocumentId == document.id},
    {"activeIn", server.sessionsWithActiveDocument(document.id)},
  };
}

Json editorStatus(ServerState& server, const Session& session)
{
  auto documents = Json::array();
  for (const auto& document : server.host.documents())
  {
    documents.push_back(documentSummary(server, document, session));
  }

  const auto active = server.targetDocument(session).document;
  auto status = Json{
    {"version", server.host.applicationVersion()},
    {"protocolVersion", session.protocolVersion},
    {"sessions", server.sessions.size()},
    {"documents", std::move(documents)},
  };
  status.update(activeDocumentJson(server, session));
  status.update(Json{
    {"locks",
     Json{
       {"alignmentLock", pref(Preferences::AlignmentLock)},
       {"uvLock", pref(Preferences::UvLock)},
     }},
  });

  if (active)
  {
    auto& document = *active->document;
    auto& map = document.map();
    auto& state = server.documentState(document);
    const auto toolName = server.host.currentToolName(document);

    status["tool"] = toolName ? Json(*toolName) : Json(nullptr);
    status["grid"] = Json{
      {"size", map.grid().actualSize()},
      {"visible", map.grid().visible()},
      {"snap", map.grid().snap()},
    };
    status["compileRunning"] = server.isCompileRunning(document);
    status["transaction"] =
      state.transaction
        ? Json{{"name", state.transaction->name}, {"owner", state.transaction->clientName}}
        : Json(nullptr);
    status["selection"] = selectionSummary(map, state.ids, 20);
  }
  else
  {
    status["tool"] = nullptr;
    status["grid"] = nullptr;
    status["compileRunning"] = false;
    status["transaction"] = nullptr;
    status["selection"] = nullptr;
  }

  const auto& activity = server.activity;
  status["agentActivity"] = Json{
    {"state",
     activity.state == ServerActivity::State::Idle      ? "idle"
     : activity.state == ServerActivity::State::Running ? "running"
                                                        : "waitingForUser"},
    {"tool", activity.toolTitle},
  };
  return status;
}

namespace
{

ToolResult resultListGetTool(CallContext& context, const Args& args)
{
  const auto listsId = args.get<std::string>("listsId");
  const auto path = args.get<std::string>("path");
  const auto* kept = context.session().findKeptLists(listsId);
  if (!kept)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("No kept lists with id {}.", listsId),
      fmt::format(
        "The lists of the last {} calls with cut lists are kept per session; repeat the "
        "call with detail: \"full\" or use a newer listsId.",
        Session::MaxKeptLists));
  }
  const auto it = kept->lists.find(path);
  if (it == kept->lists.end())
  {
    auto paths = std::vector<std::string>{};
    for (const auto& [listPath, items] : kept->lists)
    {
      paths.push_back(listPath);
    }
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("{} has no list {}.", listsId, path),
      fmt::format("Its lists are: {}.", fmt::join(paths, ", ")));
  }

  auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }
  const auto items = std::vector<Json>(it->second.begin(), it->second.end());
  auto page = makePage(items, request.value(), 0);
  page["listsId"] = listsId;
  page["path"] = path;
  page["tool"] = kept->tool;
  return page;
}

} // namespace

void registerSessionTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"editor_status"}
      .title("Editor Status")
      .description(
        "Returns the editor state (read-only): version, open documents and which "
        "sessions work on them, this session's active document (the one calls without "
        "a 'document' argument act on; each session has its own), and for the target "
        "document the current tool, grid, a selection summary, whether a compile is "
        "running and the open agent transaction; also alignment/UV locks and what the "
        "agents are doing. Call it first to orient yourself; map_summary describes the "
        "map's content. Example: {}")
      .input(object({}))
      .output(object({
        field("version", string()).required(),
        field("protocolVersion", string()).describe("Negotiated MCP protocol revision"),
        field("sessions", integer()).describe("Number of connected MCP clients"),
        field("documents", array(documentSummarySchema())).required(),
        activeDocumentField(),
        activeDocumentClosedField(),
        targetDocumentField(),
        field("tool", any())
          .describe("Name of the active editor tool of the target document or null"),
        field("grid", any()).describe("{size (map units), visible, snap} or null"),
        field(
          "locks",
          object({
            field("alignmentLock", boolean())
              .describe("Materials stay aligned when objects move (locks_set)"),
            field("uvLock", boolean())
              .describe("UVs stay fixed on faces when vertices move (locks_set)"),
          })),
        field("compileRunning", boolean())
          .describe("Whether a compile of the target document is running"),
        field("transaction", any())
          .describe("{name, owner} of the open agent transaction, or null"),
        field("selection", any())
          .describe("Selection summary of the target document (up to 20 items)"),
        field("agentActivity", any())
          .describe("{state: 'idle', 'running' or 'waitingForUser', tool}"),
      }))
      .mutation(Mutation::None)
      .idempotent()
      .handler(editorStatusTool));

  registry.add(
    ToolDef{"document_list"}
      .title("List Documents")
      .description(
        "Lists the open documents (one per editor window), read-only: handle ('doc:1'), "
        "path, game, format, modified flag, focused window, whether it is this "
        "session's active document and which sessions (clients) have it active. Several "
        "agents can work in the editor at once, each on its own active document; use "
        "document_activate to change yours. Example: {}")
      .input(object({}))
      .output(object({
        field("documents", array(documentSummarySchema())).required(),
        activeDocumentField(),
        activeDocumentClosedField(),
        targetDocumentField(),
      }))
      .mutation(Mutation::None)
      .idempotent()
      .handler(documentListTool));

  registry.add(
    ToolDef{"document_activate"}
      .title("Activate Document")
      .description(
        "Chooses this session's active document: the one its calls act on when they get "
        "no 'document' argument. Each session has its own; document_new and "
        "document_open set it, too. A session without one acts on the focused window "
        "(and adopts it, with a DOCUMENT_FROM_FOCUS warning); if the active document is "
        "closed, calls fail with ACTIVE_DOCUMENT_CLOSED until this is called. It does "
        "not change the focused window, the map or other sessions. Handles come from "
        "document_list. Example: {\"document\": \"doc:2\"}")
      .input(object({
        field("document", documentId()).required().describe("Handle from document_list"),
      }))
      .output(object({
        field("activeDocument", string()).required().describe("The new active document"),
        field("document", documentSummarySchema()).required(),
      }))
      .mutation(Mutation::None)
      .idempotent()
      .handler(documentActivateTool));

  registry.add(
    ToolDef{"session_log"}
      .title("Session Log")
      .description(
        "Returns the agent calls performed so far (read-only), newest first: tool, "
        "time, duration, success or error code, undo step and change counts; detail "
        "'full' adds the arguments. Filter by client (scope), tool name or failures. "
        "Example: {\"scope\": \"all\", \"errorsOnly\": true, \"limit\": 20}")
      .input(object({
        field("scope", enumOf({"session", "all"}).defaultsTo("session"))
          .describe("'session': calls of this client; 'all': calls of all clients"),
        field("tool", string()).describe("Only calls of this tool, e.g. 'objects_move'"),
        field("errorsOnly", boolean().defaultsTo(false)).describe("Only failed calls"),
      }))
      .output(object({
        field("items", array(any())).required().describe("Log entries, newest first"),
        field("total", integer()).required().describe("Number of matching entries"),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
      }))
      .mutation(Mutation::None)
      .paginated()
      .handler(sessionLogTool));

  registry.add(
    ToolDef{"result_list_get"}
      .title("Get Result List")
      .description(
        "Pages through a full id list that a modifying call cut to its detail level "
        "(read-only): the response's truncatedLists names the listsId and the paths "
        "(changes.created, changes.modified, changes.removed, issuesIntroduced, "
        "result.<field>). The lists of the last 10 such calls of this session are "
        "kept. Example: {\"listsId\": \"lists:3\", \"path\": \"changes.created\", "
        "\"limit\": 500}")
      .input(object({
        field("listsId", string()).required().describe("truncatedLists.listsId"),
        field("path", string())
          .required()
          .describe("A path from truncatedLists.lists, e.g. 'result.ids'"),
      }))
      .output(object({
        field("items", array(any())).required().describe("The list's items"),
        field("total", integer()).required().describe("Number of items in the list"),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
        field("listsId", string()),
        field("path", string()),
        field("tool", string()).describe("The tool of the call"),
      }))
      .mutation(Mutation::None)
      .paginated()
      .idempotent()
      .handler(resultListGetTool));
}

} // namespace tb::mcp
