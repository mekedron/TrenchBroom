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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{

struct SseEvent
{
  /** The event type; `message` if the event had no `event` field. */
  std::string event = "message";
  /** The data fields joined with LF. */
  std::string data = {};
  /** The last event ID seen on the stream so far, if any. */
  std::optional<std::string> id = std::nullopt;
  /** The value of a `retry` field in this event, if any. */
  std::optional<int> retry = std::nullopt;

  friend bool operator==(const SseEvent&, const SseEvent&) = default;
};

/**
 * An incremental parser for a Server-Sent Events stream (the body of a
 * `text/event-stream` response, after any transfer encoding was removed), following the
 * WHATWG HTML event stream interpretation rules.
 *
 * Lines may end with CRLF, LF or CR. Comments (lines that start with a colon) and unknown
 * fields are ignored. Events without any data field are not dispatched.
 */
class SseParser
{
private:
  std::string m_line;
  bool m_skipLineFeed = false;
  bool m_atStart = true;

  std::string m_eventType;
  std::string m_data;
  bool m_hasData = false;
  std::optional<std::string> m_lastEventId;
  std::optional<int> m_retry;

  std::vector<SseEvent> m_events;

public:
  /**
   * Consumes the given bytes and returns the events that were completed by them.
   */
  std::vector<SseEvent> feed(std::string_view bytes);

private:
  void processLine(std::string_view line);
  void dispatch();
};

} // namespace tb::mcp
