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

#include <QList>
#include <QObject>
#include <QPointer>

#include "base/Logger.h"
#include "mcp/ConsoleBuffer.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace tb::ui
{
class Console;

/**
 * Collects the messages that the editor's consoles log into an mcp::ConsoleBuffer, with
 * their level and the document of the console's map window. Created by
 * McpServerController when the editor starts, so the buffer holds the messages from
 * editor start, whether the MCP server runs or not; QtMcpHost exposes it to the server.
 *
 * The hook observes Console::messageLoggedNotifier. Messages logged on the main thread
 * are added to the buffer immediately, so a tool call sees the messages it caused before
 * it returns. Messages logged on other threads are added on the main thread later (the
 * buffer is not thread-safe). Messages that observers of the buffer log are not
 * collected (see Console::messageLoggedNotifier).
 */
class McpConsoleHook : public QObject
{
  Q_OBJECT
private:
  mcp::ConsoleBuffer m_buffer;
  /** The consoles that logged messages, to clear their views. */
  QList<QPointer<Console>> m_consoles;

public:
  explicit McpConsoleHook(
    size_t capacity = mcp::ConsoleBuffer::DefaultCapacity, QObject* parent = nullptr);
  ~McpConsoleHook() override;

  mcp::ConsoleBuffer& buffer();

  /**
   * Clears the text views of all consoles that logged messages, like their context
   * menu's Clear action. Does not clear the buffer.
   */
  void clearConsoleViews();

private:
  friend struct McpConsoleHookRegistry;

  /** Called for every message a console logs, on the logging thread. */
  void messageLogged(Console& console, LogLevel level, std::string_view text);
  void addMessage(
    const QPointer<Console>& console, LogLevel level, std::string_view text);
};

} // namespace tb::ui
