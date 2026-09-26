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

#include "mcp/Args.h"

namespace tb::mcp
{

Args::Args(Json json)
  : m_json(std::move(json))
{
}

const Json& Args::json() const
{
  return m_json;
}

bool Args::has(const std::string_view key) const
{
  const auto* value = findMember(m_json, key);
  return value && !value->is_null();
}

} // namespace tb::mcp
