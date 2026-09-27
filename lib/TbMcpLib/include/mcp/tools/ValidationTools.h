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
class ServerState;
class ToolRegistry;
struct DocumentInfo;

/**
 * Registers issues_list, issue_fix, issue_hide, issue_show, validators_list and
 * validators_set.
 */
void registerValidationTools(ToolRegistry& registry);

/**
 * The content of trenchbroom://documents/{doc}/issues: the issues issues_list returns
 * without filters (hidden issues and turned-off validators excluded), at most 200 items.
 */
Json issuesResource(ServerState& state, const DocumentInfo& document);

} // namespace tb::mcp
