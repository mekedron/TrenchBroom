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

#include "base/Logger.h"
#include "base/Notifier.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace tb::ui
{
class MapDocument;
}

namespace tb::mcp
{

/** A message that the editor logged to a console. */
struct ConsoleMessage
{
  /** Increases by one for every message; never reused, also not after clear(). */
  uint64_t seq = 0;
  std::chrono::system_clock::time_point time;
  LogLevel level = LogLevel::Info;
  std::string text;
  /**
   * The document whose window logged the message, or nullptr if unknown. Only used to
   * compare with open documents; it may dangle after the document was closed.
   */
  const ui::MapDocument* document = nullptr;
  /** The document's file name when the message was logged, or empty. */
  std::string documentName;
};

/**
 * A bounded in-memory buffer of the messages the editor logs to its consoles. The editor
 * (ui::McpConsoleHook) fills it from the start of the application, independently of
 * whether the MCP server runs; tests fill it through FakeHost. Used by console_read, the
 * console resource and the per-call console report.
 *
 * Not thread-safe: all functions are called on the main thread.
 */
class ConsoleBuffer
{
public:
  static constexpr size_t DefaultCapacity = 10000;

  /** Fired after messages were added. */
  Notifier<> messagesAddedNotifier;
  /** Fired after the buffer was cleared. */
  Notifier<> clearedNotifier;

private:
  size_t m_capacity;
  std::deque<ConsoleMessage> m_messages;
  uint64_t m_lastSeq = 0;
  /** The number of messages dropped because the buffer was full (not by clear()). */
  size_t m_droppedCount = 0;

public:
  explicit ConsoleBuffer(size_t capacity = DefaultCapacity);

  /** Appends a message. Empty messages are ignored. Drops the oldest message if full. */
  void add(
    LogLevel level,
    std::string_view text,
    const ui::MapDocument* document = nullptr,
    std::string documentName = {});

  /** The buffered messages, oldest first. */
  const std::deque<ConsoleMessage>& messages() const;

  /** The messages with a sequence number greater than the given one, oldest first. */
  std::vector<ConsoleMessage> messagesAfter(uint64_t seq) const;

  /** The sequence number of the last message ever added, or 0. */
  uint64_t lastSeq() const;

  /** The number of messages dropped because the buffer was full. */
  size_t droppedCount() const;

  size_t capacity() const;

  /** Removes all messages. Sequence numbers continue. */
  void clear();
};

} // namespace tb::mcp
