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

namespace tb::mcp
{
class ToolRegistry;

/**
 * Registers the layer and visibility tools: layers_list, layer_create, layer_rename,
 * layer_remove, layer_reorder, layer_set_state, objects_move_to_layer and visibility_set.
 */
void registerLayerTools(ToolRegistry& registry);

} // namespace tb::mcp
