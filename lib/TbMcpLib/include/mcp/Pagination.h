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

#include "base/Result.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/Schema.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
class Args;

enum class Detail
{
  Summary,
  Full,
};

/** A decoded page request (`cursor`, `limit`, `fields`, `detail`). */
struct PageRequest
{
  size_t offset = 0;
  size_t limit = 100;
  /** The document was modified since the cursor was issued. */
  bool stale = false;
  std::vector<std::string> fields;
  Detail detail = Detail::Summary;
};

/** The standard list parameters that `ToolDef::paginated()` adds to a tool's input. */
std::vector<schema::Field> paginationFields();

/** Opaque cursor: base64 of {"o": offset, "m": modificationCount}. */
std::string encodeCursor(size_t offset, size_t modificationCount);

struct DecodedCursor
{
  size_t offset;
  size_t modificationCount;
};

std::optional<DecodedCursor> decodeCursor(std::string_view cursor);

/**
 * Decodes the list parameters of the given arguments. The modification count identifies
 * the state of the listed data; a cursor issued for a different state marks the page as
 * stale.
 */
Result<PageRequest, ToolError> pageRequest(const Args& args, size_t modificationCount);

/**
 * Returns one page of the given items as `{"items": [...], "total": n, "nextCursor":
 * "..."|null}` (plus `"stale": true` if applicable), applying field selection.
 */
Json makePage(
  const std::vector<Json>& items, const PageRequest& request, size_t modificationCount);

/**
 * Selects the given top-level keys and dotted paths (e.g. "faces.material") from an
 * object. Arrays along a path are mapped element-wise. An empty selection returns the
 * item unchanged.
 */
Json selectFields(const Json& item, const std::vector<std::string>& fields);

/**
 * The given fields (keys or dotted paths, as for selectFields) that select nothing from
 * any of the items, e.g. misspelled keys. A path that exists in some items only (e.g.
 * "classname" in a list of entities and brushes) is not unknown. Arrays along a path are
 * searched element-wise.
 */
std::vector<std::string> unknownFields(
  const std::vector<Json>& items, const std::vector<std::string>& fields);

std::string base64Encode(std::string_view data);
std::optional<std::string> base64Decode(std::string_view data);

} // namespace tb::mcp
