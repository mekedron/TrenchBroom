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

#include "vm/bbox.h"
#include "vm/vec.h"

#include <optional>

namespace tb::mcp
{

/** Converts a vector to `[x, y, z]`, rounding each component for output. */
Json toJson(const vm::vec3d& v);
Json toJson(const vm::vec2d& v);

/** Converts a box to `{"min": [...], "max": [...]}`. */
Json toJson(const vm::bbox3d& box);

std::optional<vm::vec3d> vec3FromJson(const Json& value);
std::optional<vm::vec2d> vec2FromJson(const Json& value);
std::optional<vm::bbox3d> boxFromJson(const Json& value);

} // namespace tb::mcp
