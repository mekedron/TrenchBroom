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

#include "mcp/SseParser.h"

#include <algorithm>
#include <utility>

namespace tb::mcp
{

std::vector<SseEvent> SseParser::feed(std::string_view bytes)
{
  if (m_atStart && !bytes.empty())
  {
    // Strip a UTF-8 byte order mark at the start of the stream. The BOM may be split
    // across feeds; we accept that we only detect it if it arrives in one piece.
    if (bytes.starts_with("\xEF\xBB\xBF"))
    {
      bytes.remove_prefix(3);
    }
    m_atStart = false;
  }

  for (const auto c : bytes)
  {
    if (m_skipLineFeed)
    {
      m_skipLineFeed = false;
      if (c == '\n')
      {
        continue;
      }
    }

    if (c == '\r' || c == '\n')
    {
      m_skipLineFeed = c == '\r';
      processLine(m_line);
      m_line.clear();
    }
    else
    {
      m_line += c;
    }
  }

  return std::exchange(m_events, {});
}

void SseParser::processLine(const std::string_view line)
{
  if (line.empty())
  {
    dispatch();
    return;
  }

  if (line.front() == ':')
  {
    // comment
    return;
  }

  const auto colon = line.find(':');
  const auto field = line.substr(0, colon);
  auto value =
    colon != std::string_view::npos ? line.substr(colon + 1) : std::string_view{};
  if (value.starts_with(' '))
  {
    value.remove_prefix(1);
  }

  if (field == "event")
  {
    m_eventType = value;
  }
  else if (field == "data")
  {
    if (m_hasData)
    {
      m_data += '\n';
    }
    m_data += value;
    m_hasData = true;
  }
  else if (field == "id")
  {
    if (value.find('\0') == std::string_view::npos)
    {
      m_lastEventId = std::string{value};
    }
  }
  else if (field == "retry")
  {
    if (
      !value.empty() && value.size() < 10
      && std::ranges::all_of(value, [](const char c) { return c >= '0' && c <= '9'; }))
    {
      m_retry = std::stoi(std::string{value});
    }
  }
}

void SseParser::dispatch()
{
  if (m_hasData)
  {
    m_events.push_back(SseEvent{
      m_eventType.empty() ? "message" : m_eventType,
      std::move(m_data),
      m_lastEventId,
      m_retry,
    });
  }

  m_eventType.clear();
  m_data.clear();
  m_hasData = false;
  m_retry = std::nullopt;
}

} // namespace tb::mcp
