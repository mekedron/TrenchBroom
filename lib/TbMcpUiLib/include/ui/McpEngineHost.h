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

#include "mcp/Host.h"

#include <filesystem>
#include <optional>

namespace tb::ui
{
class MapDocument;

/**
 * Launches game engines for the MCP server with the editor's launchGameEngineProfile and
 * launch variables (LaunchGameEngineVariables), like the Launch Engine dialog does.
 */
class McpEngineHost : public mcp::EngineHost
{
private:
  std::optional<std::filesystem::path> m_logFilePath;

public:
  /**
   * If a log file is given, the engines' standard output and error are written to it;
   * otherwise they are discarded as in the Launch Engine dialog.
   */
  explicit McpEngineHost(std::optional<std::filesystem::path> logFilePath = std::nullopt);
  ~McpEngineHost() override;

  Result<std::string> engineParameters(
    MapDocument& document, const std::string& parameterSpec) override;
  Result<int64_t> launchEngine(
    MapDocument& document,
    const mdl::GameEngineProfile& profile,
    std::optional<std::string> parameterSpec) override;
};

} // namespace tb::ui
