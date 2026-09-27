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
#include "ui/McpActionHost.h"
#include "ui/McpCompileHost.h"
#include "ui/McpEngineHost.h"
#include "ui/McpPreferenceHost.h"
#include "ui/McpSnapshotRenderer.h"
#include "ui/McpViewHost.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QTimer;

namespace tb::ui
{
class AppController;
class MapDocument;
class MapViewToolBox;
class McpConsoleHook;
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
 *
 * The host also owns the background documents, which agents create or load without a
 * window (createDocument / loadDocument with background = true). They are not map
 * windows, so the map window manager, the recent documents, the welcome window and
 * quitting the application do not know about them. Their messages go to the console
 * buffer, and the host triggers their autosaves like a map window does. showDocument
 * moves a background document into a new map window. Destroying the host (quitting the
 * editor or stopping the MCP server) destroys the background documents and discards
 * their unsaved changes.
 */
class QtMcpHost : public QObject, public mcp::McpHost, public mcp::DocumentHost
{
  Q_OBJECT
public:
  /**
   * Creates, registers and shows a map window for the given document, like
   * MapWindowManager::createMapWindow (the default).
   */
  using CreateMapWindow = std::function<MapWindow*(std::unique_ptr<MapDocument>)>;

private:
  struct BackgroundDocument;

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
  /** Launches game engines. */
  McpEngineHost m_engineHost;
  /** Drives the user's views. */
  McpViewHost m_viewHost;
  /** Lists and runs the editor's actions. */
  McpActionHost m_actionHost;
  /** The action shortcuts and the MCP preferences. */
  McpPreferenceHost m_preferenceHost;
  /** Renders snapshots offscreen; created when first requested. */
  std::unique_ptr<McpSnapshotRenderer> m_snapshotRenderer;
  /** Collects the console messages; owned by McpServerController. */
  McpConsoleHook* m_consoleHook = nullptr;
  /** The documents without a window, in the order in which they were opened. */
  std::vector<std::unique_ptr<BackgroundDocument>> m_backgroundDocuments;
  /** Closed background documents, destroyed when control returns to the event loop. */
  std::vector<std::unique_ptr<BackgroundDocument>> m_closedBackgroundDocuments;
  /** The background document that showDocument is moving into a new window. */
  MapDocument* m_documentBeingShown = nullptr;
  /** Triggers the autosaves of the background documents. */
  QTimer* m_autosaveTimer = nullptr;
  CreateMapWindow m_createMapWindow;

public:
  explicit QtMcpHost(AppController& appController, QObject* parent = nullptr);
  ~QtMcpHost() override;

  /** Returns the map window that shows the given document, or nullptr. */
  MapWindow* findMapWindow(const MapDocument& document) const;

  /**
   * Sets the hook whose buffer consoleBuffer() returns and whose consoles
   * clearConsoleViews() clears. The hook must outlive the host.
   */
  void setConsoleHook(McpConsoleHook* consoleHook);

  /**
   * Replaces the function that showDocument uses to create the new map window. The tests
   * register an unshown window instead, because showing a window needs OpenGL.
   */
  void setCreateMapWindow(CreateMapWindow createMapWindow);

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
  mcp::EngineHost* engineHost() override;
  Logger* logTarget(MapDocument& document) override;
  mcp::SnapshotRenderer* snapshotRenderer() override;
  mcp::ConsoleBuffer* consoleBuffer() override;
  void clearConsoleViews() override;
  /** "mcp-knowledge" in the user data folder. */
  std::optional<std::filesystem::path> knowledgeDirectory() override;
  mcp::ViewHost* viewHost() override;
  mcp::ActionHost* actionHost() override;
  mcp::PreferenceHost* preferenceHost() override;
  /** manual/index.html in the editor's resources, if it exists. */
  std::optional<std::filesystem::path> manualPath() override;

public: // mcp::DocumentHost
  std::optional<mcp::DocumentInfo> documentToReplace() override;
  Result<mcp::OpenedDocument> createDocument(
    const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat, bool background) override;
  Result<mcp::OpenedDocument> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const std::filesystem::path& path,
    bool background) override;
  /**
   * Moves the background document into a new map window, which adds its file to the
   * recent documents like any new window.
   */
  Result<void> showDocument(MapDocument& document) override;
  /**
   * Closes the document's window, or removes a background document and destroys it when
   * control returns to the event loop.
   */
  void closeDocument(MapDocument& document) override;
  std::vector<std::filesystem::path> recentDocuments() override;

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  mcp::DocumentInfo documentInfo(const MapDocument& document);
  BackgroundDocument* findBackgroundDocument(const MapDocument& document) const;
  mcp::OpenedDocument addBackgroundDocument(
    std::unique_ptr<MapDocument> document, std::vector<mcp::LogMessage> messages);
  void destroyClosedBackgroundDocuments();
  void logBackgroundMessage(
    const MapDocument* document, LogLevel level, std::string_view message);
  void autosaveBackgroundDocuments();
  void assignDocumentIds();
  size_t documentId(const MapDocument& document);
  void connectMapWindows();

  void mapWindowWillBeDeleted(MapWindow& mapWindow);
  void focusDidChange();
  void mapWindowsDidChange();
};

} // namespace tb::ui
