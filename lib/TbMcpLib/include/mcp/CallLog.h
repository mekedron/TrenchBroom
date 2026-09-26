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

#include "mcp/Json.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{

struct CallLogEntry
{
  uint64_t seq = 0;
  /** Wall clock time in ISO 8601 (UTC). */
  std::string time;
  std::string sessionId;
  std::string clientName;
  std::string tool;
  /** The arguments, truncated to 4 KB when serialized. */
  Json arguments;
  double durationMs = 0.0;
  bool ok = true;
  std::string errorCode;
  std::optional<std::string> undoStep;
  bool dryRun = false;
  size_t created = 0;
  size_t modified = 0;
  size_t removed = 0;
};

Json toJson(const CallLogEntry& entry);

/**
 * A sink that receives every call log entry, e.g. the JSONL file writer or the editor
 * console.
 */
using CallLogSink = std::function<void(const CallLogEntry&)>;

/**
 * Writes call log entries as JSON lines to a file, rotating to a new file (suffix .1,
 * .2, ...) when the current one exceeds the maximum size.
 */
class JsonlFileSink
{
private:
  std::filesystem::path m_path;
  std::filesystem::path m_currentPath;
  size_t m_maxSize;
  size_t m_rotation = 0;
  std::ofstream m_stream;
  size_t m_size = 0;

public:
  explicit JsonlFileSink(
    std::filesystem::path path, size_t maxSize = size_t(10) * 1024 * 1024);

  void write(const CallLogEntry& entry);

  const std::filesystem::path& currentPath() const;

private:
  void open(std::filesystem::path path);
};

/**
 * Records every tool call in a ring buffer (used by `session_log`) and forwards the
 * entries to the registered sinks.
 */
class CallLog
{
private:
  size_t m_capacity;
  uint64_t m_nextSeq = 1;
  std::deque<CallLogEntry> m_entries;
  std::vector<std::pair<size_t, CallLogSink>> m_sinks;
  size_t m_nextSinkId = 1;

public:
  static constexpr size_t MaxArgumentSize = 4096;

  explicit CallLog(size_t capacity = 5000);

  /** Assigns the sequence number and time, stores the entry and forwards it to sinks. */
  const CallLogEntry& add(CallLogEntry entry);

  const std::deque<CallLogEntry>& entries() const;

  /** Returns an id that can be passed to removeSink. */
  size_t addSink(CallLogSink sink);
  void removeSink(size_t id);

  /** Returns the given arguments, or a truncated string if they exceed 4 KB. */
  static Json truncateArguments(const Json& arguments);
};

} // namespace tb::mcp
