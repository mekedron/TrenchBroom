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

#include "mcp/ListDetail.h"

#include "mcp/ObjectIds.h"

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <map>

namespace tb::mcp
{
namespace
{

constexpr auto MaxDepth = 4;

bool isIdList(const Json& array)
{
  return !array.empty() && std::ranges::all_of(array, [](const auto& item) {
    if (item.is_string())
    {
      return isObjectIdString(item);
    }
    if (item.is_object())
    {
      const auto it = item.find("id");
      return it != item.end() && isObjectIdString(*it);
    }
    return false;
  });
}

void truncate(
  Json& json,
  const std::string& path,
  const size_t limit,
  std::vector<TruncatedList>& truncated,
  const int depth)
{
  if (depth > MaxDepth)
  {
    return;
  }
  if (json.is_object())
  {
    for (auto it = json.begin(); it != json.end(); ++it)
    {
      const auto& key = it.key();
      truncate(
        it.value(), path.empty() ? key : path + "." + key, limit, truncated, depth + 1);
    }
  }
  else if (json.is_array())
  {
    if (isIdList(json))
    {
      if (json.size() > limit)
      {
        auto items = std::move(json);
        json = Json::array();
        for (size_t i = 0; i < limit; ++i)
        {
          json.push_back(items[i]);
        }
        truncated.push_back(TruncatedList{path, std::move(items)});
      }
      return;
    }
    for (size_t i = 0; i < json.size(); ++i)
    {
      if (json[i].is_array() || json[i].is_object())
      {
        truncate(json[i], fmt::format("{}[{}]", path, i), limit, truncated, depth + 1);
      }
    }
  }
}

} // namespace

std::optional<ListDetail> listDetailFromString(const std::string_view name)
{
  if (name == "summary")
  {
    return ListDetail::Summary;
  }
  if (name == "ids")
  {
    return ListDetail::Ids;
  }
  if (name == "full")
  {
    return ListDetail::Full;
  }
  return std::nullopt;
}

size_t listLimit(const ListDetail detail)
{
  switch (detail)
  {
  case ListDetail::Summary:
    return SummaryListLimit;
  case ListDetail::Ids:
    return IdsListLimit;
  case ListDetail::Full:
    break;
  }
  return std::numeric_limits<size_t>::max();
}

bool isObjectIdString(const Json& value)
{
  return value.is_string() && parseObjectRef(value.get<std::string>()).has_value();
}

std::optional<std::string> itemKind(const Json& item)
{
  const auto* id = &item;
  if (item.is_object())
  {
    if (const auto it = item.find("id"); it != item.end() && isObjectIdString(*it))
    {
      id = &*it;
    }
    else if (const auto code = item.find("code"); code != item.end() && code->is_string())
    {
      return code->get<std::string>();
    }
  }
  if (!id->is_string())
  {
    return std::nullopt;
  }
  const auto ref = parseObjectRef(id->get<std::string>());
  if (!ref)
  {
    return std::nullopt;
  }
  return ref->faceIndex ? std::string{"face"} : std::string{toString(ref->kind)};
}

Json countsByKind(const Json& items)
{
  auto counts = std::map<std::string, size_t>{};
  for (const auto& item : items)
  {
    ++counts[itemKind(item).value_or("other")];
  }
  auto result = Json::object();
  for (const auto& [kind, count] : counts)
  {
    result[kind] = count;
  }
  return result;
}

void truncateIdLists(
  Json& json,
  const std::string& path,
  const size_t limit,
  std::vector<TruncatedList>& truncated)
{
  truncate(json, path, limit, truncated, 0);
}

Json truncatedListsJson(
  const std::vector<TruncatedList>& truncated,
  const std::string& listsId,
  const size_t shown)
{
  auto lists = Json::array();
  for (const auto& list : truncated)
  {
    lists.push_back(Json{
      {"path", list.path},
      {"total", list.items.size()},
      {"shown", std::min(shown, list.items.size())},
      {"byKind", countsByKind(list.items)},
    });
  }
  return Json{
    {"listsId", listsId},
    {"lists", std::move(lists)},
    {"hint",
     fmt::format(
       "Long lists were cut. Page through a full list with result_list_get "
       "{{\"listsId\": \"{}\", \"path\": \"{}\"}}, or pass detail: \"full\" (detail: "
       "\"summary\" returns counts and a few ids).",
       listsId,
       truncated.empty() ? std::string{} : truncated.front().path)},
  };
}

} // namespace tb::mcp
