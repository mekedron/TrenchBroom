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

#include <vector>

namespace tb::ui
{
class Action;
class AppController;

/**
 * Implements mcp::PreferenceHost: the MCP server's preferences (category "mcp") and the
 * keyboard shortcuts of the editor's actions (category "keyboard"): the main menu, the
 * map view actions and, for a document, its tag and entity definition actions. The
 * shortcuts of the menu and map view actions are the preferences of the action manager's
 * actions; the tag and entity definition actions are created for the document and kept
 * until the next call, so that their preferences stay valid.
 */
class McpPreferenceHost : public mcp::PreferenceHost
{
private:
  AppController& m_appController;
  std::vector<Action> m_documentActions;

public:
  explicit McpPreferenceHost(AppController& appController);
  ~McpPreferenceHost() override;

  std::vector<mcp::HostPreference> preferences(MapDocument* document) override;
};

} // namespace tb::ui
