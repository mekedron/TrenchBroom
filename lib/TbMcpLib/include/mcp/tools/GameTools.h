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

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
struct GameInfo;
class Map;
} // namespace tb::mdl

namespace tb::mcp
{
class ToolRegistry;

/**
 * Registers game_list, game_info, game_set_path, mods_get, mods_set,
 * entity_definitions_get, entity_definitions_set, entity_definitions_reload,
 * materials_collections_get, materials_collections_set, materials_reload,
 * soft_bounds_get and soft_bounds_set.
 */
void registerGameTools(ToolRegistry& registry);

/** The URI of the game configuration resource, e.g. trenchbroom://games/Quake%202/config
 */
std::string gameConfigUri(const std::string& gameName);

/**
 * The description of a game configuration (game_info and the
 * trenchbroom://games/{game}/config resource).
 */
Json gameConfigJson(const mdl::GameInfo& gameInfo);

/** `{"default": "id1", "enabled": [...]}` */
Json modsJson(const mdl::Map& map);

/** The entity definition file in use and the number of definitions. */
Json entityDefinitionsJson(const mdl::Map& map);

/** The material setup: WAD list or enabled folder collections, and loaded counts. */
Json materialsJson(const mdl::Map& map);

/** A relative entry of the map's WAD list. */
struct RelativeWadPath
{
  std::string path;
  /** The file the editor loads, if it was found. */
  std::optional<std::filesystem::path> absolutePath;
};

/**
 * The relative entries of the map's WAD list (WAD games only). Compile tools such as
 * hlcsg open them relative to their working directory and usually fail.
 */
std::vector<RelativeWadPath> relativeWadPaths(const mdl::Map& map);

/** The soft map bounds in effect. */
Json softBoundsJson(const mdl::Map& map);

} // namespace tb::mcp
