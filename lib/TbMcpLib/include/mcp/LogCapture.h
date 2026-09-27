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
#include "mcp/Json.h"

#include <string>
#include <string_view>
#include <vector>

namespace tb::ui
{
class MapDocument;
}

namespace tb::mcp
{

/** A message that the editor logged. */
struct LogMessage
{
  LogLevel level = LogLevel::Info;
  std::string text;

  bool operator==(const LogMessage&) const = default;
};

/** Returns "debug", "info", "warning" or "error". */
std::string_view toString(LogLevel level);

/** Returns `{"level": "warning", "message": "..."}`. */
Json toJson(const LogMessage& message);

/**
 * Forwards everything to a target logger (if any) and records warnings and errors, so
 * that failures of Map_* functions and load problems can be explained to the agent.
 */
class CapturingLogger : public Logger
{
private:
  Logger* m_target;
  std::vector<LogMessage> m_messages;
  std::vector<std::string> m_texts;

public:
  explicit CapturingLogger(Logger* target);

  /** The recorded warnings and errors. */
  const std::vector<LogMessage>& messages() const;

  /** The texts of the recorded warnings and errors. */
  const std::vector<std::string>& texts() const;

  void clear();

private:
  void doLog(LogLevel level, std::string_view message) override;
};

/**
 * Records the warnings and errors that a document logs while this object exists. The
 * capture becomes the document's target logger and forwards all messages to the given
 * target (e.g. the console, may be null). The destructor makes the given target the
 * document's target logger again.
 */
class ScopedLogCapture
{
private:
  ui::MapDocument& m_document;
  Logger* m_target;
  CapturingLogger m_logger;

public:
  ScopedLogCapture(ui::MapDocument& document, Logger* target);
  ~ScopedLogCapture();

  ScopedLogCapture(const ScopedLogCapture&) = delete;
  ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

  const std::vector<LogMessage>& messages() const;
  const std::vector<std::string>& texts() const;
};

/**
 * Returns the warnings and errors that a document without a target logger has cached,
 * e.g. while it was loaded. The messages stay cached, so the console shows them once the
 * document gets its window.
 *
 * Precondition: the document has no target logger.
 */
std::vector<LogMessage> collectCachedMessages(ui::MapDocument& document);

} // namespace tb::mcp
