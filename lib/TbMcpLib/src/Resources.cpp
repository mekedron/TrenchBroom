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

#include "mcp/Resources.h"

#include "mcp/McpServer.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/ServerState.h"
#include "mcp/tools/DocumentTools.h"
#include "mcp/tools/GameTools.h"
#include "mcp/tools/SceneTools.h"
#include "mcp/tools/SelectionTools.h"
#include "mcp/tools/SessionTools.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "tools/ToolUtils.h"
#include "ui/MapDocument.h"

#include <algorithm>

namespace tb::mcp
{
namespace
{

constexpr auto AgentGuide = R"(# TrenchBroom MCP server — agent guide

Conventions
- All coordinates and lengths are in map units; Z is up. Angles are in degrees.
- Object ids look like 'brush:1042', 'entity:7', 'group:3', 'layer:5', 'layer:default',
  'world'. Faces are 'brush:1042/face:3'. Ids stay valid while the object exists,
  including across undo and redo. They do not survive reloading a document.
- Tools that act on objects take 'ids'; without ids they act on the current selection.

Changing the map
- Every modifying call is one undo step named 'AI: <tool title>' and is atomic: if it
  fails, the map is unchanged.
- Every modifying call accepts 'dryRun': it reports the changes and introduced issues
  without changing anything.
- Group several calls into one undo step with transaction_begin / transaction_commit,
  or discard them with transaction_rollback.
- If the user is dragging or has a dialog open, modifying calls wait until they finish.

Understanding the map
- Start with map_summary, then narrow down with objects_find (filters such as
  classname, material, layer, region) and object_get for details. map_tree shows the
  hierarchy, map_plan_view a top-down text plan of a region.
- Spatial questions: ray_pick ({"from": "entity:12"} finds what is below an entity),
  objects_at_point, and space_check before placing entities or rooms.
- Selection tools (selection_set, select_by, ...) change the editor's selection; each
  call is an undo step, like selecting in the editor.
- Subscribe to trenchbroom://documents/{doc}/summary and .../selection to learn about
  changes the user makes; updates are coalesced.

Results
- Modifying calls return 'changes' (created / modified / removed ids), 'selection',
  'issuesIntroduced', 'warnings' and the grid size in effect.
- Errors carry a code, a message, the involved object ids and a hint for the next step.
- List results are paginated: pass 'cursor' from 'nextCursor' to get the next page.

Resumability
- Server-sent event streams cannot be resumed with Last-Event-ID. After reconnecting,
  read the resources again.
)";

/** Lists one entry per open document for a document resource template. */
ResourceLister documentLister(
  const DocumentAspect aspect, std::string name, std::string title)
{
  return [=](ServerState& state, Session&) {
    auto entries = std::vector<Json>{};
    for (const auto& document : state.host.documents())
    {
      entries.push_back(Json{
        {"uri", ServerState::documentResourceUri(document.id, aspect)},
        {"name", name + "-" + document.id},
        {"title", title + ": " + document.windowTitle},
        {"mimeType", "application/json"},
      });
    }
    return entries;
  };
}

/** Reads a document resource: finds the document named by {doc} and describes it. */
ResourceReader documentReader(
  std::function<Json(ServerState&, const DocumentInfo&, Session&)> describe)
{
  return [describe = std::move(describe)](
           ServerState& state,
           Session& session,
           const std::string& uri,
           const ResourceVariables& vars) -> Result<Json, ToolError> {
    const auto documentId = vars.at("doc");
    if (const auto document = state.findDocument(documentId))
    {
      return jsonResourceContents(uri, describe(state, *document, session));
    }
    return makeError(
      ErrorCode::DocumentNotFound,
      "Document " + documentId + " is not open.",
      "Use document_list to see the open documents.");
  };
}

} // namespace

void registerResources(McpServer& server)
{
  auto& resources = server.resources();

  resources.add(ResourceDef{
    "trenchbroom://editor/status",
    "editor-status",
    "Editor Status",
    "Active document, open documents, current tool, grid, locks and selection summary. "
    "Subscribe to get notified when documents are opened, closed or saved, or the tool, "
    "locks, open transaction or selection change.",
    "application/json",
    [](ServerState& state, Session& session, const std::string& uri, const auto&)
      -> Result<Json, ToolError> {
      return jsonResourceContents(uri, editorStatus(state, session));
    },
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/info",
    "document-info",
    "Document Info",
    "Path, game, format, modified flag, game folder, mods, entity definitions, material "
    "collections and soft bounds of an open document ({doc} is a handle such as "
    "doc:1). Subscribe to get notified when it is saved, reloaded, modified or its "
    "settings change.",
    "application/json",
    documentReader(
      [](ServerState& state, const DocumentInfo& document, Session& session) {
        return documentInfo(state, document, session);
      }),
    documentLister(DocumentAspect::Info, "document-info", "Document Info"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/summary",
    "map-summary",
    "Map Summary",
    "Overview of an open document's map, as returned by map_summary: object counts, "
    "entities by class, layers, materials, bounds and issue count ({doc} is a handle "
    "such as doc:1). Subscribe to get notified when objects are added, removed or "
    "changed.",
    "application/json",
    documentReader([](ServerState& state, const DocumentInfo& document, Session&) {
      auto& map = document.document->map();
      return mapSummary(map, state.documentState(*document.document).ids);
    }),
    documentLister(DocumentAspect::Summary, "map-summary", "Map Summary"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://documents/{doc}/selection",
    "selection",
    "Selection",
    "The current selection of an open document (objects or faces) with a short "
    "description of each selected item, at most 100 ({doc} is a handle such as doc:1). "
    "Subscribe to get notified when the selection or the selected objects change.",
    "application/json",
    documentReader([](ServerState& state, const DocumentInfo& document, Session&) {
      const auto& map = document.document->map();
      return selectionDetails(map, state.documentState(*document.document).ids, 100);
    }),
    documentLister(DocumentAspect::Selection, "selection", "Selection"),
  });

  resources.addTemplate(ResourceTemplateDef{
    "trenchbroom://games/{game}/config",
    "game-config",
    "Game Configuration",
    "A game's configuration: map formats, file system, material setup, entity "
    "definition files, smart tags, surface and content flags, soft bounds and compile "
    "tools ({game} is the percent-encoded game name, e.g. Quake%202). Updated when the "
    "game folder changes.",
    "application/json",
    [](ServerState& state, Session&, const std::string& uri, const auto& vars)
      -> Result<Json, ToolError> {
      const auto gameName = percentDecode(vars.at("game"));
      const auto* gameInfo = gameName ? findGame(state.host, *gameName) : nullptr;
      if (!gameInfo)
      {
        return unknownGameError(state.host, gameName.value_or(vars.at("game")));
      }
      return jsonResourceContents(uri, gameConfigJson(*gameInfo));
    },
    [](ServerState& state, Session&) {
      // only the games of open documents, to keep the list short
      auto names = std::vector<std::string>{};
      for (const auto& document : state.host.documents())
      {
        const auto& name = document.document->map().gameInfo().gameConfig.name;
        if (std::ranges::find(names, name) == names.end())
        {
          names.push_back(name);
        }
      }

      auto entries = std::vector<Json>{};
      for (const auto& name : names)
      {
        entries.push_back(Json{
          {"uri", gameConfigUri(name)},
          {"name", "game-config-" + percentEncode(name)},
          {"title", "Game Configuration: " + name},
          {"mimeType", "application/json"},
        });
      }
      return entries;
    },
  });

  resources.add(ResourceDef{
    "trenchbroom://guide",
    "agent-guide",
    "Agent Guide",
    "How to use this server well: conventions, ids, transactions, dry runs.",
    "text/markdown",
    [](ServerState&, Session&, const std::string& uri, const auto&)
      -> Result<Json, ToolError> {
      return Json::array({Json{
        {"uri", uri},
        {"mimeType", "text/markdown"},
        {"text", AgentGuide},
      }});
    },
  });
}

} // namespace tb::mcp
