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

#include "mcp/LogCapture.h"

#include "ui/MapDocument.h"

#include "kd/contracts.h"

namespace tb::mcp
{
namespace
{

class RecordingLogger : public Logger
{
public:
  std::vector<LogMessage> messages;

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    messages.push_back(LogMessage{level, std::string{message}});
  }
};

bool isProblem(const LogLevel level)
{
  return level == LogLevel::Warn || level == LogLevel::Error;
}

} // namespace

std::string_view toString(const LogLevel level)
{
  switch (level)
  {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warning";
  case LogLevel::Error:
    return "error";
  }
  return "info";
}

Json toJson(const LogMessage& message)
{
  return Json{{"level", toString(message.level)}, {"message", message.text}};
}

CapturingLogger::CapturingLogger(Logger* target)
  : m_target{target}
{
}

const std::vector<LogMessage>& CapturingLogger::messages() const
{
  return m_messages;
}

const std::vector<std::string>& CapturingLogger::texts() const
{
  return m_texts;
}

void CapturingLogger::clear()
{
  m_messages.clear();
  m_texts.clear();
}

void CapturingLogger::doLog(const LogLevel level, const std::string_view message)
{
  if (isProblem(level))
  {
    m_messages.push_back(LogMessage{level, std::string{message}});
    m_texts.emplace_back(message);
  }
  if (m_target)
  {
    m_target->log(level, message);
  }
}

ScopedLogCapture::ScopedLogCapture(ui::MapDocument& document)
  : m_document{document}
  , m_previousTarget{document.targetLogger()}
  , m_logger{m_previousTarget}
{
  m_document.setTargetLogger(&m_logger);
  // setting a target logger flushes cached messages that predate the capture
  m_logger.clear();
}

ScopedLogCapture::~ScopedLogCapture()
{
  m_document.setTargetLogger(m_previousTarget);
}

const std::vector<LogMessage>& ScopedLogCapture::messages() const
{
  return m_logger.messages();
}

const std::vector<std::string>& ScopedLogCapture::texts() const
{
  return m_logger.texts();
}

std::vector<LogMessage> collectCachedMessages(ui::MapDocument& document)
{
  contract_pre(document.targetLogger() == nullptr);

  // setting a target logger flushes the cache into it
  auto recorder = RecordingLogger{};
  document.setTargetLogger(&recorder);
  document.setTargetLogger(nullptr);

  // cache the messages again for the console
  auto result = std::vector<LogMessage>{};
  for (const auto& message : recorder.messages)
  {
    document.logger().log(message.level, message.text);
    if (isProblem(message.level))
    {
      result.push_back(message);
    }
  }
  return result;
}

} // namespace tb::mcp
