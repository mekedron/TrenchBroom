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

#include "mcp/tools/MaterialKnowledge.h"

namespace tb::mcp
{
class CallContext;
class ToolRegistry;

/**
 * The material knowledge for the call's document: the host's knowledge directory (if it
 * has one), the document's game and mod. Create it once per call and ask it for as many
 * profiles as needed. Precondition: context.hasDocument()
 */
MaterialKnowledge materialKnowledge(CallContext& context);

/**
 * Warns with KNOWLEDGE_FILE_INVALID for every knowledge file that could not be read or
 * parsed (MaterialKnowledge::problems); such files are ignored.
 */
void warnKnowledgeProblems(CallContext& context, const MaterialKnowledge& knowledge);

/**
 * Registers the tools material_corpus_scan, material_notes_get, material_notes_set and
 * material_usage.
 */
void registerMaterialKnowledgeTools(ToolRegistry& registry);

} // namespace tb::mcp
