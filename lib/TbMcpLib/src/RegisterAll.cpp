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

#include "mcp/RegisterAll.h"

#include "mcp/McpServer.h"
#include "mcp/Resources.h"
#include "mcp/tools/DocumentTools.h"
#include "mcp/tools/GameTools.h"
#include "mcp/tools/HistoryTools.h"
#include "mcp/tools/SessionTools.h"

namespace tb::mcp
{

void registerAll(McpServer& server)
{
  registerSessionTools(server.tools());
  registerHistoryTools(server.tools());
  registerDocumentTools(server.tools());
  registerGameTools(server.tools());
  registerResources(server);
}

} // namespace tb::mcp
