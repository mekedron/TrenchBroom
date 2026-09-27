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

namespace tb::ui
{
class MapWindow;
class McpPreferencePane;
class McpServerController;
class McpStatusIndicator;
class PreferenceDialog;

/*
 * Adds the MCP server's widgets to the editor's windows and dialogs.
 */

/**
 * Adds an MCP status indicator to the end of the window's status bar, after the update
 * indicator. Does nothing if the window already has one. Returns the window's indicator.
 */
McpStatusIndicator* addMcpStatusIndicator(
  MapWindow& mapWindow, McpServerController& controller);

/**
 * Adds the "AI Agents" pane and its tool bar button to the given preference dialog. Does
 * nothing if the dialog already has the pane. Returns the dialog's pane.
 */
McpPreferencePane* addMcpPreferencePane(
  PreferenceDialog& dialog, McpServerController& controller);

} // namespace tb::ui
