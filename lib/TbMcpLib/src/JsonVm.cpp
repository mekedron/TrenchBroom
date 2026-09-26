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

#include "mcp/JsonVm.h"

namespace tb::mcp
{
namespace
{

template <size_t S>
std::optional<vm::vec<double, S>> vecFromJson(const Json& value)
{
  if (!value.is_array() || value.size() != S)
  {
    return std::nullopt;
  }

  auto result = vm::vec<double, S>{};
  for (size_t i = 0; i < S; ++i)
  {
    if (!value[i].is_number())
    {
      return std::nullopt;
    }
    result[i] = value[i].get<double>();
  }
  return result;
}

} // namespace

Json toJson(const vm::vec3d& v)
{
  return Json::array(
    {roundForOutput(v.x()), roundForOutput(v.y()), roundForOutput(v.z())});
}

Json toJson(const vm::vec2d& v)
{
  return Json::array({roundForOutput(v.x()), roundForOutput(v.y())});
}

Json toJson(const vm::bbox3d& box)
{
  return Json{{"min", toJson(box.min)}, {"max", toJson(box.max)}};
}

std::optional<vm::vec3d> vec3FromJson(const Json& value)
{
  return vecFromJson<3>(value);
}

std::optional<vm::vec2d> vec2FromJson(const Json& value)
{
  return vecFromJson<2>(value);
}

std::optional<vm::bbox3d> boxFromJson(const Json& value)
{
  const auto* min = findMember(value, "min");
  const auto* max = findMember(value, "max");
  if (!min || !max)
  {
    return std::nullopt;
  }
  const auto minVec = vec3FromJson(*min);
  const auto maxVec = vec3FromJson(*max);
  if (!minVec || !maxVec)
  {
    return std::nullopt;
  }
  return vm::bbox3d{*minVec, *maxVec};
}

} // namespace tb::mcp
