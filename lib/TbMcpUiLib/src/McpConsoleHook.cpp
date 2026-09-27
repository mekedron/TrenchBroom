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


#include "ui/McpConsoleHook.h"

#include <QMetaObject>
#include <QThread>

#include "base/NotifierConnection.h"
#include "mdl/Map.h"
#include "ui/Console.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace tb::ui
{

/**
 * Forwards Console::messageLoggedNotifier to the existing hooks. It connects once and
 * stays connected, so that hooks can be created and destroyed while other threads log:
 * the notifier itself is not thread-safe, but Console serializes its calls.
 */
struct McpConsoleHookRegistry
{
  std::mutex mutex;
  std::vector<McpConsoleHook*> hooks;
  NotifierConnection connection;

  McpConsoleHookRegistry()
  {
    connection = Console::messageLoggedNotifier.connect(
      [this](Console& console, const LogLevel level, const std::string_view text) {
        const auto lock = std::lock_guard{mutex};
        for (auto* hook : hooks)
        {
          hook->messageLogged(console, level, text);
        }
      });
  }

  static McpConsoleHookRegistry& instance()
  {
    static auto registry = McpConsoleHookRegistry{};
    return registry;
  }
};

McpConsoleHook::McpConsoleHook(const size_t capacity, QObject* parent)
  : QObject{parent}
  , m_buffer{capacity}
{
  auto& registry = McpConsoleHookRegistry::instance();
  const auto lock = std::lock_guard{registry.mutex};
  registry.hooks.push_back(this);
}

McpConsoleHook::~McpConsoleHook()
{
  auto& registry = McpConsoleHookRegistry::instance();
  const auto lock = std::lock_guard{registry.mutex};
  std::erase(registry.hooks, this);
}

mcp::ConsoleBuffer& McpConsoleHook::buffer()
{
  return m_buffer;
}

void McpConsoleHook::clearConsoleViews()
{
  m_consoles.removeIf([](const auto& console) { return console.isNull(); });
  for (auto& console : m_consoles)
  {
    console->clear();
  }
}

void McpConsoleHook::messageLogged(
  Console& console, const LogLevel level, const std::string_view text)
{
  if (QThread::currentThread() == thread())
  {
    addMessage(&console, level, text);
  }
  else
  {
    // the buffer is not thread-safe; the message is dropped if the hook is destroyed
    // before it is delivered
    QMetaObject::invokeMethod(
      this,
      [this, console = QPointer<Console>{&console}, level, text = std::string{text}]() {
        addMessage(console, level, text);
      },
      Qt::QueuedConnection);
  }
}

void McpConsoleHook::addMessage(
  const QPointer<Console>& console, const LogLevel level, const std::string_view text)
{
  const MapDocument* document = nullptr;
  auto documentName = std::string{};
  if (console)
  {
    if (!m_consoles.contains(console))
    {
      m_consoles.removeIf([](const auto& known) { return known.isNull(); });
      m_consoles.append(console);
    }

    // the console of a map window belongs to the window's document
    if (const auto* mapWindow = qobject_cast<const MapWindow*>(console->window()))
    {
      document = &mapWindow->document();
      documentName = document->map().path().filename().string();
    }
  }

  m_buffer.add(level, text, document, std::move(documentName));
}

} // namespace tb::ui
