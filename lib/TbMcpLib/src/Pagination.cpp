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

#include "mcp/Pagination.h"

#include "mcp/Args.h"

#include <algorithm>
#include <array>

namespace tb::mcp
{
namespace
{

constexpr auto Base64Chars =
  std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

Json selectPath(const Json& value, const std::string_view path)
{
  const auto dot = path.find('.');
  const auto head = path.substr(0, dot);
  const auto tail =
    dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);

  if (value.is_array())
  {
    auto result = Json::array();
    for (const auto& element : value)
    {
      result.push_back(selectPath(element, path));
    }
    return result;
  }

  const auto* member = findMember(value, head);
  if (!member)
  {
    return Json(nullptr);
  }
  if (tail.empty())
  {
    return Json{{std::string{head}, *member}};
  }
  auto nested = selectPath(*member, tail);
  return Json{{std::string{head}, std::move(nested)}};
}

bool hasPath(const Json& value, const std::string_view path)
{
  if (value.is_array())
  {
    return std::ranges::any_of(
      value, [&](const auto& element) { return hasPath(element, path); });
  }

  const auto dot = path.find('.');
  const auto* member = findMember(value, path.substr(0, dot));
  return member
         && (dot == std::string_view::npos || hasPath(*member, path.substr(dot + 1)));
}

void merge(Json& target, const Json& source)
{
  if (source.is_null())
  {
    return;
  }
  if (target.is_null())
  {
    target = source;
    return;
  }
  if (target.is_object() && source.is_object())
  {
    for (const auto& [key, value] : source.items())
    {
      if (target.contains(key))
      {
        merge(target[key], value);
      }
      else
      {
        target[key] = value;
      }
    }
    return;
  }
  if (target.is_array() && source.is_array() && target.size() == source.size())
  {
    for (size_t i = 0; i < target.size(); ++i)
    {
      merge(target[i], source[i]);
    }
  }
}

} // namespace

std::vector<schema::Field> paginationFields()
{
  using namespace schema;
  return {
    field("cursor", string()).describe("Opaque cursor from a previous page's nextCursor"),
    field("limit", integer().min(1).max(1000).defaultsTo(100))
      .describe("Maximum number of items to return"),
    field("fields", array(string()))
      .describe(
        "Only return these keys of each item; dotted paths select nested keys, e.g. "
        "'faces.material'"),
    field("detail", enumOf({"summary", "full"}).defaultsTo("summary"))
      .describe("'summary' returns a compact shape, 'full' returns everything"),
  };
}

std::string encodeCursor(const size_t offset, const size_t modificationCount)
{
  return base64Encode(dumpJson(Json{{"o", offset}, {"m", modificationCount}}));
}

std::optional<DecodedCursor> decodeCursor(const std::string_view cursor)
{
  const auto decoded = base64Decode(cursor);
  if (!decoded)
  {
    return std::nullopt;
  }
  const auto value = parseJson(*decoded);
  if (!value)
  {
    return std::nullopt;
  }
  const auto* offset = findMember(*value, "o");
  const auto* modificationCount = findMember(*value, "m");
  if (
    !offset || !modificationCount || !offset->is_number_unsigned()
    || !modificationCount->is_number_unsigned())
  {
    return std::nullopt;
  }
  return DecodedCursor{offset->get<size_t>(), modificationCount->get<size_t>()};
}

Result<PageRequest, ToolError> pageRequest(
  const Args& args, const size_t modificationCount)
{
  auto request = PageRequest{};
  request.limit = size_t(args.getOr<int64_t>("limit", 100));
  request.fields = args.getOr<std::vector<std::string>>("fields", {});
  request.detail = args.getOr<std::string>("detail", "summary") == "full"
                     ? Detail::Full
                     : Detail::Summary;

  if (const auto cursor = args.getOptional<std::string>("cursor"))
  {
    const auto decoded = decodeCursor(*cursor);
    if (!decoded)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The cursor is invalid.",
        "Pass the nextCursor value of the previous page unchanged, or omit the cursor to "
        "start from the beginning.");
    }
    request.offset = decoded->offset;
    request.stale = decoded->modificationCount != modificationCount;
  }

  return request;
}

Json makePage(
  const std::vector<Json>& items,
  const PageRequest& request,
  const size_t modificationCount)
{
  auto page = Json::array();
  for (size_t i = request.offset; i < items.size() && page.size() < request.limit; ++i)
  {
    page.push_back(selectFields(items[i], request.fields));
  }

  const auto end = request.offset + page.size();
  auto result = Json{
    {"items", std::move(page)},
    {"total", items.size()},
    {"nextCursor",
     end < items.size() ? Json(encodeCursor(end, modificationCount)) : Json(nullptr)},
  };
  if (request.stale)
  {
    result["stale"] = true;
  }
  return result;
}

Json selectFields(const Json& item, const std::vector<std::string>& fields)
{
  if (fields.empty() || !item.is_object())
  {
    return item;
  }

  auto result = Json::object();
  for (const auto& path : fields)
  {
    merge(result, selectPath(item, path));
  }
  return result;
}

std::vector<std::string> unknownFields(
  const std::vector<Json>& items, const std::vector<std::string>& fields)
{
  auto result = std::vector<std::string>{};
  for (const auto& path : fields)
  {
    if (std::ranges::none_of(
          items, [&](const auto& item) { return hasPath(item, path); }))
    {
      result.push_back(path);
    }
  }
  return result;
}

std::string base64Encode(const std::string_view data)
{
  auto result = std::string{};
  auto i = size_t{0};
  while (i + 2 < data.size())
  {
    const auto n = (uint32_t(uint8_t(data[i])) << 16)
                   | (uint32_t(uint8_t(data[i + 1])) << 8)
                   | uint32_t(uint8_t(data[i + 2]));
    result += Base64Chars[(n >> 18) & 63];
    result += Base64Chars[(n >> 12) & 63];
    result += Base64Chars[(n >> 6) & 63];
    result += Base64Chars[n & 63];
    i += 3;
  }
  if (i + 1 == data.size())
  {
    const auto n = uint32_t(uint8_t(data[i])) << 16;
    result += Base64Chars[(n >> 18) & 63];
    result += Base64Chars[(n >> 12) & 63];
    result += "==";
  }
  else if (i + 2 == data.size())
  {
    const auto n =
      (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8);
    result += Base64Chars[(n >> 18) & 63];
    result += Base64Chars[(n >> 12) & 63];
    result += Base64Chars[(n >> 6) & 63];
    result += '=';
  }
  return result;
}

std::optional<std::string> base64Decode(const std::string_view data)
{
  if (data.size() % 4 != 0)
  {
    return std::nullopt;
  }

  auto result = std::string{};
  for (size_t i = 0; i < data.size(); i += 4)
  {
    auto n = uint32_t{0};
    auto padding = 0;
    for (size_t j = 0; j < 4; ++j)
    {
      const auto c = data[i + j];
      auto v = uint32_t{0};
      if (c == '=')
      {
        if (i + 4 != data.size() || j < 2)
        {
          return std::nullopt;
        }
        ++padding;
      }
      else
      {
        if (padding > 0)
        {
          return std::nullopt;
        }
        const auto pos = Base64Chars.find(c);
        if (pos == std::string_view::npos)
        {
          return std::nullopt;
        }
        v = uint32_t(pos);
      }
      n = (n << 6) | v;
    }
    result += char((n >> 16) & 0xff);
    if (padding < 2)
    {
      result += char((n >> 8) & 0xff);
    }
    if (padding < 1)
    {
      result += char(n & 0xff);
    }
  }
  return result;
}

} // namespace tb::mcp
