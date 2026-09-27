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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

/**
 * How much of the id lists a modifying call returns (the `detail` argument of
 * `Mutation::Map` tools): its change report, selection, introduced issues and the id
 * lists of its result.
 */
enum class ListDetail
{
  /** Counts per kind and at most SummaryListLimit items per list. */
  Summary,
  /** At most IdsListLimit items per list (the default). */
  Ids,
  /** Every item. */
  Full,
};

constexpr auto SummaryListLimit = size_t(5);
constexpr auto IdsListLimit = size_t(50);

std::optional<ListDetail> listDetailFromString(std::string_view name);

/** The most items a list keeps at this detail level (SIZE_MAX for Full). */
size_t listLimit(ListDetail detail);

/** A list that was cut: its path in the response (e.g. "result.ids") and all items. */
struct TruncatedList
{
  std::string path;
  Json items;
};

/** Whether the string is an object id such as "brush:12" or "brush:12/face:3". */
bool isObjectIdString(const Json& value);

/**
 * The kind of an item of a list: the object kind of an object id ("brush", "entity",
 * "group", "patch", "layer", "world"; "face" for a face id), of an object's "id", or an
 * issue's "code"; nullopt for other items.
 */
std::optional<std::string> itemKind(const Json& item);

/** The number of items per kind (itemKind), e.g. {"brush": 1490, "entity": 10}. */
Json countsByKind(const Json& items);

/**
 * Cuts the id lists in `json` to `limit` items: arrays of object ids and arrays of
 * objects whose "id" is an object id (object summaries). Recurses into objects and into
 * arrays of arrays or objects (e.g. the instances of objects_array). Each cut list is
 * appended to `truncated` with its full items, its path prefixed with `path`.
 */
void truncateIdLists(
  Json& json,
  const std::string& path,
  size_t limit,
  std::vector<TruncatedList>& truncated);

/**
 * Describes the lists of a call that were cut, for the `truncatedLists` field of the
 * result: `{listsId, lists: [{path, total, shown, byKind}], hint}`. The full lists can be
 * paged with `result_list_get` under `listsId`.
 */
Json truncatedListsJson(
  const std::vector<TruncatedList>& truncated, const std::string& listsId, size_t shown);

} // namespace tb::mcp
