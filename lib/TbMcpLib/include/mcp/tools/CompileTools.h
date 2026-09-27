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
class ResourceRegistry;
class ToolRegistry;

/**
 * Registers compile_tools_get, compile_tools_set, compile_presets_list,
 * compile_profiles_list, compile_profile_save, compile_profile_delete, compile_run,
 * compile_status, compile_cancel, pointfile_load, pointfile_unload, portalfile_load and
 * portalfile_unload.
 */
void registerCompileTools(ToolRegistry& registry);

/** Registers the compile log resource trenchbroom://compile/{run}/log. */
void registerCompileResources(ResourceRegistry& registry);

} // namespace tb::mcp
