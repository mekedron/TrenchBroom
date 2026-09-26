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

#include <cstddef>

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::mcp
{
class IdRegistry;
class ToolRegistry;

/**
 * Registers selection_get, selection_set, selection_clear, select_all, select_invert,
 * select_by, select_spatial, select_siblings, select_by_line and select_faces_of.
 */
void registerSelectionTools(ToolRegistry& registry);

/**
 * The current selection with a short description of each selected object or face (at
 * most `limit` of them), as returned by selection_get in summary detail and by the
 * trenchbroom://documents/{doc}/selection resource.
 */
Json selectionDetails(const mdl::Map& map, const IdRegistry& ids, size_t limit = 100);

} // namespace tb::mcp
