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

#include "ToolUtils.h"

#include "base/PreferenceManager.h"
#include "mcp/Args.h"
#include "mcp/Host.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"

#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>

namespace tb::mcp
{

std::string isoTime(const std::chrono::system_clock::time_point time)
{
  return fmt::format(
    "{:%Y-%m-%dT%H:%M:%S}Z", fmt::gmtime(std::chrono::system_clock::to_time_t(time)));
}

std::string isoTime(const std::filesystem::file_time_type time)
{
  // std::chrono::clock_cast is not available with every standard library; converting via
  // the current time of both clocks is portable and exact enough for file times
  const auto systemTime =
    std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      time - std::filesystem::file_time_type::clock::now()
      + std::chrono::system_clock::now());
  return isoTime(systemTime);
}

const mdl::GameInfo* findGame(McpHost& host, const std::string_view name)
{
  const auto& gameInfos = host.gameManager().gameInfos();
  const auto it = std::ranges::find_if(gameInfos, [&](const auto& gameInfo) {
    return kdl::ci::str_is_equal(gameInfo.gameConfig.name, name);
  });
  return it != gameInfos.end() ? &*it : nullptr;
}

std::vector<std::string> gameNames(McpHost& host)
{
  auto result = std::vector<std::string>{};
  for (const auto& gameInfo : host.gameManager().gameInfos())
  {
    result.push_back(gameInfo.gameConfig.name);
  }
  return result;
}

ToolError unknownGameError(McpHost& host, const std::string_view name)
{
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("Unknown game '{}'.", name),
    fmt::format(
      "Use one of: {}. game_list shows the games and whether they are set up.",
      kdl::str_join(gameNames(host), ", ")));
}

void ItemErrors::add(const size_t index, std::string message)
{
  m_errors.push_back(Json{{"index", index}, {"message", std::move(message)}});
}

void ItemErrors::add(const size_t index, const ToolError& error)
{
  auto item = Json{{"index", index}, {"message", error.message}};
  if (error.code != ErrorCode::InvalidArgument)
  {
    item["code"] = std::string{toString(error.code)};
  }
  if (!error.hint.empty())
  {
    item["hint"] = error.hint;
  }
  m_errors.push_back(std::move(item));
}

bool ItemErrors::empty() const
{
  return m_errors.empty();
}

ToolError ItemErrors::error(const size_t itemCount, std::string hint) const
{
  constexpr auto MaxInMessage = size_t(3);

  auto indices = std::vector<size_t>{};
  auto parts = std::vector<std::string>{};
  for (const auto& item : m_errors)
  {
    const auto index = item["index"].get<size_t>();
    if (std::ranges::find(indices, index) == indices.end())
    {
      indices.push_back(index);
    }
    if (parts.size() < MaxInMessage)
    {
      parts.push_back(
        fmt::format("items[{}]: {}", index, item["message"].get<std::string>()));
    }
  }
  auto message = fmt::format(
    "{} of {} items are invalid, nothing was created. {}{}",
    indices.size(),
    itemCount,
    kdl::str_join(parts, " "),
    m_errors.size() > MaxInMessage
      ? fmt::format(
          " ({} more problems in details.errors)", m_errors.size() - MaxInMessage)
      : std::string{});
  auto error = makeError(ErrorCode::InvalidArgument, std::move(message), std::move(hint));
  error.details["errors"] = m_errors;
  return error;
}

std::optional<ToolError> backgroundDocumentError(
  const DocumentInfo& document, const std::string_view what)
{
  if (!document.background)
  {
    return std::nullopt;
  }
  return makeError(
    ErrorCode::DocumentInBackground,
    fmt::format(
      "{} need an editor window, and {} is a background document without one.",
      what,
      document.id),
    fmt::format(
      "Look at it with agent cameras (view_snapshot), or give it a window with "
      "document_show {{\"document\": \"{}\"}}.",
      document.id));
}

std::filesystem::path gamePath(const mdl::GameInfo& gameInfo)
{
  return pref(gameInfo.gamePathPreference);
}

bool isGamePathValid(const mdl::GameInfo& gameInfo)
{
  const auto path = gamePath(gameInfo);
  auto ec = std::error_code{};
  return !path.empty() && std::filesystem::is_directory(path, ec) && !ec;
}

Result<std::filesystem::path, ToolError> absolutePathArgument(
  const Args& args, const std::string_view key)
{
  const auto str = args.get<std::string>(key);
  const auto path = std::filesystem::path{str};
  if (str.empty() || !path.is_absolute())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("'{}' must be an absolute path, got '{}'.", key, str),
      "Pass the full path, e.g. /home/user/maps/start.map or C:\\\\maps\\\\start.map.");
  }
  return path.lexically_normal();
}

Json toJson(const std::vector<LogMessage>& messages)
{
  auto result = Json::array();
  for (const auto& message : messages)
  {
    result.push_back(toJson(message));
  }
  return result;
}

std::string percentEncode(const std::string_view str)
{
  auto result = std::string{};
  for (const auto c : str)
  {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~')
    {
      result.push_back(c);
    }
    else
    {
      result += fmt::format("%{:02X}", u);
    }
  }
  return result;
}

std::optional<std::string> percentDecode(const std::string_view str)
{
  auto result = std::string{};
  for (size_t i = 0; i < str.size(); ++i)
  {
    if (str[i] != '%')
    {
      result.push_back(str[i]);
      continue;
    }
    if (
      i + 2 >= str.size() || !std::isxdigit(static_cast<unsigned char>(str[i + 1]))
      || !std::isxdigit(static_cast<unsigned char>(str[i + 2])))
    {
      return std::nullopt;
    }
    result.push_back(char(std::stoi(std::string{str.substr(i + 1, 2)}, nullptr, 16)));
    i += 2;
  }
  return result;
}

bool pathExists(const std::filesystem::path& path)
{
  auto ec = std::error_code{};
  return std::filesystem::exists(path, ec) && !ec;
}

} // namespace tb::mcp
