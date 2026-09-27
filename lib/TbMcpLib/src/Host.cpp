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

#include "mcp/Host.h"

namespace tb::mcp
{

DocumentHost::~DocumentHost() = default;

CompileJob::~CompileJob() = default;

CompileHost::~CompileHost() = default;

EngineHost::~EngineHost() = default;

ViewHost::~ViewHost() = default;

ActionHost::~ActionHost() = default;

PreferenceHost::~PreferenceHost() = default;

McpHost::~McpHost() = default;

CompileHost* McpHost::compileHost()
{
  return nullptr;
}

EngineHost* McpHost::engineHost()
{
  return nullptr;
}

Logger* McpHost::logTarget(ui::MapDocument&)
{
  return nullptr;
}

SnapshotRenderer* McpHost::snapshotRenderer()
{
  return nullptr;
}

ConsoleBuffer* McpHost::consoleBuffer()
{
  return nullptr;
}

void McpHost::clearConsoleViews() {}

std::optional<std::filesystem::path> McpHost::knowledgeDirectory()
{
  return std::nullopt;
}

ViewHost* McpHost::viewHost()
{
  return nullptr;
}

ActionHost* McpHost::actionHost()
{
  return nullptr;
}

PreferenceHost* McpHost::preferenceHost()
{
  return nullptr;
}

std::optional<std::filesystem::path> McpHost::manualPath()
{
  return std::nullopt;
}

} // namespace tb::mcp
