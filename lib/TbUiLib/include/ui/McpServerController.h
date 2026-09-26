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
#include <QString>

#include "base/NotifierConnection.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace tb
{
namespace mcp
{
class JsonlFileSink;
class McpServer;
struct CallLogEntry;
struct ServerActivity;
} // namespace mcp

namespace ui
{
class AppController;
class McpTcpTransport;
class QtMcpHost;
class QtScheduler;

/**
 * Formats a call log entry for the editor console, e.g.
 * `[AI] brush_create_box ok 3 ms (+1 created, ~0 modified, -0 removed)`.
 */
std::string formatCallLogEntryForConsole(const mcp::CallLogEntry& entry);

/**
 * Formats the server activity for the status bar, e.g. `AI: 2 clients · idle`.
 */
QString formatMcpActivity(const mcp::ServerActivity& activity, size_t clientCount);

/**
 * Owns the MCP server and its transport. Starts and stops the server according to the
 * preferences (or the `--mcp-server` command line option), writes the discovery file
 * while the server listens, and exposes the server state to the UI.
 */
class McpServerController : public QObject
{
  Q_OBJECT
private:
  struct Settings
  {
    std::string bindAddress;
    int port = 0;
    std::string accessToken;

    bool operator==(const Settings&) const = default;
  };

  AppController& m_appController;
  bool m_forceEnabled = false;

  std::unique_ptr<QtScheduler> m_scheduler;
  std::unique_ptr<QtMcpHost> m_host;
  std::unique_ptr<mcp::McpServer> m_server;
  std::unique_ptr<McpTcpTransport> m_transport;

  std::optional<Settings> m_settings;
  std::optional<int> m_port;
  bool m_ownsDiscoveryFile = false;

  std::unique_ptr<mcp::JsonlFileSink> m_fileSink;
  std::optional<size_t> m_fileSinkId;

  QString m_statusText;

  NotifierConnection m_notifierConnection;
  NotifierConnection m_serverNotifierConnection;

public:
  explicit McpServerController(AppController& appController, QObject* parent = nullptr);
  ~McpServerController() override;

  /**
   * Enables the server for this process regardless of the preference (`--mcp-server`).
   */
  void setForceEnabled(bool forceEnabled);
  bool forceEnabled() const;

  /** Whether the server should run, i.e. it is enabled in the preferences or forced. */
  bool enabled() const;

  /** Whether the server is listening for connections. */
  bool listening() const;

  /** The port the server listens on, if it is listening. */
  std::optional<int> port() const;

  /** The number of connected MCP sessions. */
  size_t clientCount() const;

  /**
   * A short description of the server state for the status bar, e.g.
   * "MCP: port 47100 in use". Empty while the server listens normally or is disabled.
   */
  const QString& statusText() const;

  /** The current activity for the status bar, e.g. "AI: 1 client · idle". */
  QString activityText() const;

  /** The server, if it is running. */
  mcp::McpServer* server();

  /** The host, if the server is running. */
  QtMcpHost* host();

  /** The path of the discovery file that is written while the server listens. */
  std::filesystem::path discoveryFilePath() const;

  /** The directory that contains the JSONL call log files. */
  std::filesystem::path logDirectoryPath() const;

  /** The path of the current JSONL call log file, if logging to a file. */
  std::optional<std::filesystem::path> logFilePath() const;

  /**
   * Stops all agents: drops queued calls, rolls back agent transactions, closes all
   * sessions and connections. The server keeps listening.
   */
  void stopAgents();

signals:
  void clientsChanged(int clientCount);
  void activityChanged(const QString& activityText);
  void statusChanged();

private:
  void preferenceDidChange(const std::filesystem::path& path);

  /** Starts, restarts or stops the server to match the preferences. */
  void update();
  void start(const Settings& settings);
  void stop();

  void applyOptions();
  void updateLogToFile();

  void writeDiscoveryFile();
  void removeDiscoveryFile();

  void setStatusText(QString statusText);
  void logInfo(const std::string& message);
  void logError(const std::string& message);
};

} // namespace ui
} // namespace tb
