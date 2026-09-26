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

Results
- Modifying calls return 'changes' (created / modified / removed ids), 'selection',
  'issuesIntroduced', 'warnings' and the grid size in effect.
- Errors carry a code, a message, the involved object ids and a hint for the next step.
- List results are paginated: pass 'cursor' from 'nextCursor' to get the next page.

Resumability
- Server-sent event streams cannot be resumed with Last-Event-ID. After reconnecting,
  read the resources again.
)";

} // namespace

void registerResources(McpServer& server)
{
  auto& resources = server.resources();

  resources.add(ResourceDef{
    "trenchbroom://editor/status",
    "editor-status",
    "Editor Status",
    "Active document, open documents, current tool, grid, locks and selection summary. "
    "Subscribe to get notified when documents are opened or closed.",
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
    [](ServerState& state, Session& session, const std::string& uri, const auto& vars)
      -> Result<Json, ToolError> {
      const auto documentId = vars.at("doc");
      if (const auto document = state.findDocument(documentId))
      {
        return jsonResourceContents(uri, documentInfo(state, *document, session));
      }
      return makeError(
        ErrorCode::DocumentNotFound,
        "Document " + documentId + " is not open.",
        "Use document_list to see the open documents.");
    },
    [](ServerState& state, Session&) {
      auto entries = std::vector<Json>{};
      for (const auto& document : state.host.documents())
      {
        entries.push_back(Json{
          {"uri", "trenchbroom://documents/" + document.id + "/info"},
          {"name", "document-info-" + document.id},
          {"title", "Document Info: " + document.windowTitle},
          {"mimeType", "application/json"},
        });
      }
      return entries;
    },
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
