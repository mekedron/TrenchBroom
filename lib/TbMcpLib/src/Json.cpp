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

#include "mcp/Json.h"

#include <cmath>

namespace tb::mcp
{

double roundForOutput(const double value)
{
  if (!std::isfinite(value))
  {
    return value;
  }
  const auto rounded = std::round(value * 1e6) / 1e6;
  // avoid emitting -0
  return rounded == 0.0 ? 0.0 : rounded;
}

std::optional<Json> parseJson(const std::string_view text)
{
  auto result = Json::parse(text, nullptr, false);
  if (result.is_discarded())
  {
    return std::nullopt;
  }
  return result;
}

std::string dumpJson(const Json& value)
{
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

const Json* findMember(const Json& value, const std::string_view key)
{
  if (!value.is_object())
  {
    return nullptr;
  }
  const auto it = value.find(key);
  return it != value.end() ? &*it : nullptr;
}

} // namespace tb::mcp
