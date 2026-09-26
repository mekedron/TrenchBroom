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

#pragma once

#include "mcp/Json.h"

namespace tb::mcp
{
struct DocumentInfo;
class ServerState;
class Session;
class ToolRegistry;

/**
 * Registers document_new, document_open, document_save, document_save_as,
 * document_close, document_revert, document_recent, map_files_list,
 * document_export_map, document_export_obj and autosave_list.
 */
void registerDocumentTools(ToolRegistry& registry);

/**
 * The detailed description of an open document: the summary of document_list plus game
 * path, world bounds, mods, entity definitions, material collections and soft bounds.
 * Used by the document tools and the trenchbroom://documents/{doc}/info resource.
 */
Json documentInfo(
  ServerState& server, const DocumentInfo& document, const Session& session);

} // namespace tb::mcp
