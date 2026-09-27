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

#include "mcp/ConsoleBuffer.h"

#include <algorithm>

namespace tb::mcp
{

ConsoleBuffer::ConsoleBuffer(const size_t capacity)
  : m_capacity{std::max(capacity, size_t{1})}
{
}

void ConsoleBuffer::add(
  const LogLevel level,
  const std::string_view text,
  const ui::MapDocument* document,
  std::string documentName)
{
  if (text.empty())
  {
    return;
  }

  if (m_messages.size() == m_capacity)
  {
    m_messages.pop_front();
    ++m_droppedCount;
  }

  m_messages.push_back(ConsoleMessage{
    ++m_lastSeq,
    std::chrono::system_clock::now(),
    level,
    std::string{text},
    document,
    std::move(documentName),
  });
  messagesAddedNotifier();
}

const std::deque<ConsoleMessage>& ConsoleBuffer::messages() const
{
  return m_messages;
}

std::vector<ConsoleMessage> ConsoleBuffer::messagesAfter(const uint64_t seq) const
{
  const auto first = std::ranges::upper_bound(
    m_messages, seq, std::less<>{}, [](const auto& message) { return message.seq; });
  return std::vector<ConsoleMessage>{first, m_messages.end()};
}

uint64_t ConsoleBuffer::lastSeq() const
{
  return m_lastSeq;
}

size_t ConsoleBuffer::droppedCount() const
{
  return m_droppedCount;
}

size_t ConsoleBuffer::capacity() const
{
  return m_capacity;
}

void ConsoleBuffer::clear()
{
  m_messages.clear();
  clearedNotifier();
}

} // namespace tb::mcp
