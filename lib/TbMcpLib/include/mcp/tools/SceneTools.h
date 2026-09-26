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

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::mcp
{
class IdRegistry;
class ToolRegistry;

/**
 * Registers map_summary, map_tree, object_get, objects_find, map_text_get and map_stats.
 */
void registerSceneTools(ToolRegistry& registry);

/**
 * The high-level overview of a map returned by map_summary and the
 * trenchbroom://documents/{doc}/summary resource.
 */
Json mapSummary(mdl::Map& map, const IdRegistry& ids);

} // namespace tb::mcp
