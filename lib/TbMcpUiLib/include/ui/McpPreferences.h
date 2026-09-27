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

#include "base/Preference.h"

#include <string>

namespace tb::McpPreferences
{

inline auto McpServerEnabled = Preference<bool>{"MCP/Enabled", false};
inline auto McpServerPort = Preference<int>{"MCP/Port", 47100};
inline auto McpServerBindAddress =
  Preference<std::string>{"MCP/Bind address", "127.0.0.1"};
inline auto McpServerAccessToken = Preference<std::string>{"MCP/Access token", ""};
inline auto McpLogToFile = Preference<bool>{"MCP/Log to file", true};
inline auto McpBusyWaitTimeoutMs = Preference<int>{"MCP/Busy wait timeout", 30000};

} // namespace tb::McpPreferences
