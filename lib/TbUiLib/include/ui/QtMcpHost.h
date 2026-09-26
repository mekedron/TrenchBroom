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

#include <QObject>

#include "mcp/Host.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::ui
{
class AppController;
class MapDocument;
class MapViewToolBox;
class MapWindow;

/**
 * Returns the name of the active modal tool of the given tool box, e.g. "Vertex Tool", or
 * nullopt if no modal tool is active.
 */
std::optional<std::string> activeModalToolName(const MapViewToolBox& toolBox);

/**
 * Implements the MCP server's view of the editor using the application controller and its
 * map windows.
 */
class QtMcpHost : public QObject, public mcp::McpHost
{
  Q_OBJECT
private:
  AppController& m_appController;
  std::unordered_map<const MapDocument*, size_t> m_documentIds;
  size_t m_nextDocumentId = 1;

public:
  explicit QtMcpHost(AppController& appController, QObject* parent = nullptr);
  ~QtMcpHost() override;

  /** Returns the map window that shows the given document, or nullptr. */
  MapWindow* findMapWindow(const MapDocument& document) const;

public: // mcp::McpHost
  std::string applicationVersion() const override;
  std::vector<mcp::DocumentInfo> documents() override;
  mcp::BusyState busyState(MapDocument& document) override;
  std::vector<std::string> prepareForAgentEdit(MapDocument& document) override;
  std::optional<std::string> currentToolName(MapDocument& document) override;
  bool isCompileRunning(MapDocument& document) override;

private:
  void assignDocumentIds();
  size_t documentId(const MapDocument& document);

  void mapWindowWillClose(MapWindow* mapWindow);
  void mapWindowsDidChange();
};

} // namespace tb::ui
