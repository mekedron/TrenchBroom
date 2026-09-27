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

#include "base/NotifierConnection.h"
#include "mcp/Host.h"
#include "ui/McpCompileHost.h"

#include <filesystem>
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
 *
 * The host observes the map windows from the outside: it watches the application's events
 * for map windows being shown (opened) and deleted (closed), and the application's focus
 * changes, which change the window order.
 */
class QtMcpHost : public QObject, public mcp::McpHost, public mcp::DocumentHost
{
  Q_OBJECT
private:
  AppController& m_appController;
  std::unordered_map<const MapDocument*, size_t> m_documentIds;
  size_t m_nextDocumentId = 1;
  /**
   * Observes the tool box of each known map window to report tool changes, and its
   * document to report documents that were created or loaded in place.
   */
  std::unordered_map<const MapWindow*, NotifierConnection> m_mapWindowConnections;
  /** The top map window when the documents last changed, to detect order changes. */
  const MapWindow* m_topMapWindow = nullptr;
  /** Runs compilations with the camera of the document's 3D view. */
  McpCompileHost m_compileHost;

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
  mcp::DocumentHost& documentHost() override;
  mdl::GameManager& gameManager() override;
  mcp::CompileHost* compileHost() override;
  Logger* logTarget(MapDocument& document) override;

public: // mcp::DocumentHost
  std::optional<mcp::DocumentInfo> documentToReplace() override;
  Result<mcp::OpenedDocument> createDocument(
    const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat) override;
  Result<mcp::OpenedDocument> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const std::filesystem::path& path) override;
  void closeDocument(MapDocument& document) override;
  std::vector<std::filesystem::path> recentDocuments() override;

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  mcp::DocumentInfo documentInfo(const MapDocument& document);
  void assignDocumentIds();
  size_t documentId(const MapDocument& document);
  void connectMapWindows();

  void mapWindowWillBeDeleted(MapWindow& mapWindow);
  void focusDidChange();
  void mapWindowsDidChange();
};

} // namespace tb::ui
