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
#include "mcp/tools/SessionTools.h"

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
