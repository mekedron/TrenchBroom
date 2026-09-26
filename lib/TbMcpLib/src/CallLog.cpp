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

#include "mcp/CallLog.h"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace tb::mcp
{
namespace
{

std::string currentTime()
{
  const auto now = std::chrono::system_clock::now();
  const auto millis =
    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
    % 1000;
  return fmt::format(
    "{:%Y-%m-%dT%H:%M:%S}.{:03d}Z",
    fmt::gmtime(std::chrono::system_clock::to_time_t(now)),
    int(millis));
}

} // namespace

Json toJson(const CallLogEntry& entry)
{
  auto result = Json{
    {"seq", entry.seq},
    {"time", entry.time},
    {"session", entry.sessionId},
    {"client", entry.clientName},
    {"tool", entry.tool},
    {"arguments", entry.arguments},
    {"durationMs", roundForOutput(entry.durationMs)},
    {"ok", entry.ok},
  };
  if (!entry.ok)
  {
    result["errorCode"] = entry.errorCode;
  }
  result["undoStep"] = entry.undoStep ? Json(*entry.undoStep) : Json(nullptr);
  result["dryRun"] = entry.dryRun;
  result["changes"] = Json{
    {"created", entry.created},
    {"modified", entry.modified},
    {"removed", entry.removed},
  };
  return result;
}

JsonlFileSink::JsonlFileSink(std::filesystem::path path, const size_t maxSize)
  : m_path{std::move(path)}
  , m_maxSize{maxSize}
{
  open(m_path);
}

void JsonlFileSink::write(const CallLogEntry& entry)
{
  if (m_size >= m_maxSize)
  {
    ++m_rotation;
    auto rotatedPath = m_path;
    rotatedPath.replace_extension(
      "." + std::to_string(m_rotation) + m_path.extension().string());
    open(rotatedPath);
  }

  if (m_stream)
  {
    const auto line = dumpJson(toJson(entry)) + "\n";
    m_stream << line;
    m_stream.flush();
    m_size += line.size();
  }
}

const std::filesystem::path& JsonlFileSink::currentPath() const
{
  return m_currentPath;
}

void JsonlFileSink::open(std::filesystem::path path)
{
  m_stream.close();
  auto error = std::error_code{};
  std::filesystem::create_directories(path.parent_path(), error);
  m_stream.open(path, std::ios::out | std::ios::app | std::ios::binary);
  m_size =
    std::filesystem::exists(path, error) ? std::filesystem::file_size(path, error) : 0;
  m_currentPath = std::move(path);
}

CallLog::CallLog(const size_t capacity)
  : m_capacity{capacity}
{
}

const CallLogEntry& CallLog::add(CallLogEntry entry)
{
  entry.seq = m_nextSeq++;
  entry.time = currentTime();
  entry.arguments = truncateArguments(entry.arguments);

  m_entries.push_back(std::move(entry));
  while (m_entries.size() > m_capacity)
  {
    m_entries.pop_front();
  }

  const auto& added = m_entries.back();
  for (const auto& [id, sink] : m_sinks)
  {
    sink(added);
  }
  return added;
}

const std::deque<CallLogEntry>& CallLog::entries() const
{
  return m_entries;
}

size_t CallLog::addSink(CallLogSink sink)
{
  const auto id = m_nextSinkId++;
  m_sinks.emplace_back(id, std::move(sink));
  return id;
}

void CallLog::removeSink(const size_t id)
{
  std::erase_if(m_sinks, [&](const auto& entry) { return entry.first == id; });
}

Json CallLog::truncateArguments(const Json& arguments)
{
  auto serialized = dumpJson(arguments);
  if (serialized.size() <= MaxArgumentSize)
  {
    return arguments;
  }
  serialized.resize(MaxArgumentSize);
  return Json(serialized + "... (truncated)");
}

} // namespace tb::mcp
