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
}

namespace tb::mcp
{
class ToolRegistry;

/** Registers entity_classes_list, entity_class_describe and entity_model_info. */
void registerEntityClassTools(ToolRegistry& registry);

/**
 * The content of the trenchbroom://documents/{doc}/entity-definitions resource: the
 * definition file in use (`spec`, `definitionFile`), `count`, and `classes` sorted by
 * name, each `{name, type, group, description (first line), size (point classes),
 * propertyKeys, spawnflags (flag names, if the class has spawnflags)}`.
 */
Json entityDefinitionsResource(const mdl::Map& map);

} // namespace tb::mcp
