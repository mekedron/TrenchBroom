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

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace tb::mcp
{
class NotificationStream;

/**
 * The state of one connected client. Each `initialize` creates a session.
 */
class Session
{
public:
  std::string id;
  std::string protocolVersion;
  std::string clientName;
  std::string clientVersion;
  Json clientCapabilities = Json::object();

  /** Whether the client sent `notifications/initialized`. */
  bool initialized = false;

  /** The document chosen with `document_activate`, if any. */
  std::optional<std::string> activeDocumentId;

  /** Subscribed resource URIs. */
  std::set<std::string> subscriptions;

  /** The standalone notification stream (HTTP GET), if open. */
  std::weak_ptr<NotificationStream> notificationStream;

  std::string logLevel = "info";

  /**
   * Sends a notification over the standalone stream if the session is initialized and
   * the stream is open. Returns whether it was sent.
   */
  bool send(const Json& notification) const;

  /** A display name for the client, e.g. "claude-code 1.0". */
  std::string clientDisplayName() const;
};

} // namespace tb::mcp
