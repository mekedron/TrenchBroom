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

#include <functional>

namespace tb::ui
{
class AppController;
class MapWindow;

/**
 * Implements mcp::ActionHost over the editor's ActionManager: the main menu, the map view
 * actions, and the tag and entity definition actions, which it creates for the document
 * on every call (like MapDocumentActionCache, but without keeping them, because they
 * refer to the document's tags and entity definitions).
 */
class McpActionHost : public mcp::ActionHost
{
public:
  using FindMapWindow = std::function<MapWindow*(const MapDocument&)>;

private:
  AppController& m_appController;
  FindMapWindow m_findMapWindow;

public:
  McpActionHost(AppController& appController, FindMapWindow findMapWindow);
  ~McpActionHost() override;

  Result<std::vector<mcp::EditorAction>> actions(
    MapDocument& document, const std::optional<std::string>& viewId) override;

  /**
   * Runs the action with an ActionExecutionContext for the window and the view, like the
   * menu (MapWindow) and the view shortcuts (MapViewBase) do. A deferred action runs from
   * a zero timer owned by the window; the action and the view are looked up again then,
   * and nothing happens if the window was closed or the action is gone or disabled.
   */
  Result<mcp::EditorAction> invokeAction(
    MapDocument& document,
    const std::string& path,
    const std::optional<std::string>& viewId,
    bool deferred) override;
};

} // namespace tb::ui
