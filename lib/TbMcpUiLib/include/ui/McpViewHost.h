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
class MapWindow;

/**
 * Implements mcp::ViewHost over the views of the document's map window (found with the
 * given function): the MapViewBase widgets of its SwitchableMapViewContainer, their
 * gl::Camera and MapWindow's maximize toggle.
 */
class McpViewHost : public mcp::ViewHost
{
public:
  using FindMapWindow = std::function<MapWindow*(const MapDocument&)>;

private:
  FindMapWindow m_findMapWindow;

public:
  explicit McpViewHost(FindMapWindow findMapWindow);
  ~McpViewHost() override;

  std::vector<mcp::UserView> views(MapDocument& document) override;
  Result<void> setCamera(
    MapDocument& document,
    const std::string& viewId,
    const mcp::AgentCamera& camera) override;
  Result<mcp::ViewLayout> layout(MapDocument& document) override;
  Result<void> setMaximizedView(
    MapDocument& document, const std::optional<std::string>& viewId) override;
  /** Clears the focus if a map view (or a widget in one) has it. */
  void prepareForLayoutChange() override;

private:
  MapWindow* mapWindow(const MapDocument& document) const;
};

} // namespace tb::ui
