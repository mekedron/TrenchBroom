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
#include "mcp/JsonVm.h"

#include "kd/contracts.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace tb::mcp
{

/**
 * Typed access to validated tool arguments. The arguments were validated against the
 * tool's input schema and defaults were filled in, so accessing a required argument or
 * an argument with a default value always succeeds.
 */
class Args
{
private:
  Json m_json;

public:
  explicit Args(Json json);

  const Json& json() const;

  /** Whether the argument is present and not null. */
  bool has(std::string_view key) const;

  template <typename T>
  std::optional<T> getOptional(const std::string_view key) const
  {
    const auto* value = findMember(m_json, key);
    if (!value || value->is_null())
    {
      return std::nullopt;
    }

    if constexpr (std::is_same_v<T, vm::vec3d>)
    {
      return vec3FromJson(*value);
    }
    else if constexpr (std::is_same_v<T, vm::vec2d>)
    {
      return vec2FromJson(*value);
    }
    else if constexpr (std::is_same_v<T, vm::bbox3d>)
    {
      return boxFromJson(*value);
    }
    else if constexpr (std::is_same_v<T, Json>)
    {
      return *value;
    }
    else
    {
      return value->get<T>();
    }
  }

  /** Precondition: the argument is present (required or defaulted by the schema). */
  template <typename T>
  T get(const std::string_view key) const
  {
    auto result = getOptional<T>(key);
    contract_pre(result.has_value());
    return std::move(*result);
  }

  template <typename T>
  T getOr(const std::string_view key, T defaultValue) const
  {
    return getOptional<T>(key).value_or(std::move(defaultValue));
  }
};

} // namespace tb::mcp
