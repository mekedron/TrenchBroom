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

#include "ui/McpServerController.h"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QtGlobal>

#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "mcp/CallLog.h"
#include "mcp/Json.h"
#include "mcp/McpServer.h"
#include "mcp/RegisterAll.h"
#include "mcp/StreamableHttp.h"
#include "mdl/EnvironmentConfig.h"
#include "ui/AppController.h"
#include "ui/GetVersion.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpConsoleHook.h"
#include "ui/McpPreferencePane.h"
#include "ui/McpPreferences.h"
#include "ui/McpStatusIndicator.h"
#include "ui/McpTcpTransport.h"
#include "ui/McpUiIntegration.h"
#include "ui/PreferenceDialog.h"
#include "ui/QtMcpHost.h"
#include "ui/QtScheduler.h"

#include <fmt/format.h>

#include <chrono>
#include <fstream>
#include <system_error>

namespace tb::ui
{
namespace
{

const auto McpPreferencePrefix = std::filesystem::path{"MCP"};

bool isMcpPreference(const std::filesystem::path& path)
{
  return !path.empty() && *path.begin() == McpPreferencePrefix;
}

std::optional<mcp::Json> readJsonFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path};
  if (!stream)
  {
    return std::nullopt;
  }
  const auto text =
    std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  return mcp::parseJson(text);
}

QString clientCountText(const size_t clientCount)
{
  return clientCount == 1 ? QObject::tr("1 client")
                          : QObject::tr("%1 clients").arg(clientCount);
}

} // namespace

std::string formatCallLogEntryForConsole(const mcp::CallLogEntry& entry)
{
  const auto durationMs = static_cast<long long>(entry.durationMs + 0.5);
  const auto dryRun = entry.dryRun ? " [dry run]" : "";
  if (entry.ok)
  {
    return fmt::format(
      "[AI] {} ok {} ms (+{} created, ~{} modified, -{} removed){}",
      entry.tool,
      durationMs,
      entry.created,
      entry.modified,
      entry.removed,
      dryRun);
  }
  return fmt::format(
    "[AI] {} failed: {} ({} ms){}", entry.tool, entry.errorCode, durationMs, dryRun);
}

QString formatMcpActivity(const mcp::ServerActivity& activity, const size_t clientCount)
{
  using State = mcp::ServerActivity::State;

  if (activity.state == State::Idle && !activity.openTransactions.empty())
  {
    return QObject::tr("AI transaction open: %1")
      .arg(QString::fromStdString(activity.openTransactions.front()));
  }

  const auto clients = clientCountText(clientCount);
  switch (activity.state)
  {
  case State::Running:
    return QObject::tr("AI: %1 · %2")
      .arg(clients)
      .arg(QString::fromStdString(activity.toolTitle));
  case State::WaitingForUser:
    return QObject::tr("AI: %1 · waiting for you").arg(clients);
  case State::Idle:
    break;
  }
  return QObject::tr("AI: %1 · idle").arg(clients);
}

McpServerController::McpServerController(AppController& appController, QObject* parent)
  : QObject{parent}
  , m_appController{appController}
  , m_consoleHook{std::make_unique<McpConsoleHook>()}
{
  auto& prefs = PreferenceManager::instance();
  m_notifierConnection += prefs.preferenceDidChangeNotifier.connect(
    this, &McpServerController::preferenceDidChange);

  connect(
    QCoreApplication::instance(),
    &QCoreApplication::aboutToQuit,
    this,
    &McpServerController::stop);

  // Add the status indicator to map windows and the preference pane to preference
  // dialogs when they are shown, and the status indicator to the open map windows now
  qApp->installEventFilter(this);
  for (auto* mapWindow : m_appController.mapWindowManager().mapWindows())
  {
    addWidget(addMcpStatusIndicator(*mapWindow, *this));
  }

  m_forceEnabled = QCoreApplication::arguments().contains("--mcp-server");
  update();
}

McpServerController::~McpServerController()
{
  stop();

  // The added widgets refer to this controller
  for (auto& widget : m_addedWidgets)
  {
    delete widget.data();
  }
}

void McpServerController::setForceEnabled(const bool forceEnabled)
{
  if (forceEnabled != m_forceEnabled)
  {
    m_forceEnabled = forceEnabled;
    update();
  }
}

bool McpServerController::forceEnabled() const
{
  return m_forceEnabled;
}

bool McpServerController::enabled() const
{
  return m_forceEnabled || pref(McpPreferences::McpServerEnabled);
}

bool McpServerController::listening() const
{
  return m_server != nullptr;
}

std::optional<int> McpServerController::port() const
{
  return m_port;
}

size_t McpServerController::clientCount() const
{
  return m_server ? m_server->sessionCount() : 0;
}

const QString& McpServerController::statusText() const
{
  return m_statusText;
}

QString McpServerController::activityText() const
{
  return m_server ? formatMcpActivity(m_server->activity(), m_server->sessionCount())
                  : QString{};
}

mcp::McpServer* McpServerController::server()
{
  return m_server.get();
}

QtMcpHost* McpServerController::host()
{
  return m_host.get();
}

McpConsoleHook& McpServerController::consoleHook()
{
  return *m_consoleHook;
}

std::filesystem::path McpServerController::discoveryFilePath() const
{
  return m_appController.environmentConfig().userDataFolderPath / "mcp-server.json";
}

std::filesystem::path McpServerController::logDirectoryPath() const
{
  return m_appController.environmentConfig().userDataFolderPath / "mcp-logs";
}

std::optional<std::filesystem::path> McpServerController::logFilePath() const
{
  return m_fileSink ? std::optional{m_fileSink->currentPath()} : std::nullopt;
}

void McpServerController::stopAgents()
{
  if (m_server)
  {
    m_server->stopAgents();
    m_transport->closeAllConnections();
  }
}

bool McpServerController::eventFilter(QObject* watched, QEvent* event)
{
  if (event->type() == QEvent::Show)
  {
    if (auto* mapWindow = qobject_cast<MapWindow*>(watched))
    {
      addWidget(addMcpStatusIndicator(*mapWindow, *this));
    }
    else if (auto* preferenceDialog = qobject_cast<PreferenceDialog*>(watched))
    {
      addWidget(addMcpPreferencePane(*preferenceDialog, *this));
    }
  }
  return QObject::eventFilter(watched, event);
}

void McpServerController::addWidget(QWidget* widget)
{
  if (widget && !m_addedWidgets.contains(widget))
  {
    m_addedWidgets.removeIf([](const auto& addedWidget) { return addedWidget.isNull(); });
    m_addedWidgets.append(widget);
  }
}

void McpServerController::preferenceDidChange(const std::filesystem::path& path)
{
  if (isMcpPreference(path))
  {
    update();
  }
}

void McpServerController::update()
{
  if (!enabled())
  {
    stop();
    setStatusText({});
    return;
  }

  const auto settings = Settings{
    .bindAddress = pref(McpPreferences::McpServerBindAddress),
    .port = pref(McpPreferences::McpServerPort),
    .accessToken = pref(McpPreferences::McpServerAccessToken),
  };

  if (m_server && m_settings == settings)
  {
    applyOptions();
    updateLogToFile();
    return;
  }

  stop();
  start(settings);
}

void McpServerController::start(const Settings& settings)
{
  const auto config = mcp::StreamableHttpServer::Config{
    .bindAddress = settings.bindAddress,
    .accessToken = settings.accessToken,
  };

  if (const auto error = mcp::StreamableHttpServer::validateConfig(config))
  {
    logError(fmt::format("MCP server not started: {}", *error));
    setStatusText(tr("MCP: %1").arg(QString::fromStdString(*error)));
    return;
  }

  m_scheduler = std::make_unique<QtScheduler>();
  m_host = std::make_unique<QtMcpHost>(m_appController);
  m_host->setConsoleHook(m_consoleHook.get());
  m_server = std::make_unique<mcp::McpServer>(
    *m_host,
    *m_scheduler,
    mcp::ServerInfo{.version = getBuildVersion().toStdString()},
    mcp::ServerOptions{});
  mcp::registerAll(*m_server);
  applyOptions();

  m_serverNotifierConnection += m_server->sessionsDidChangeNotifier.connect([this]() {
    emit clientsChanged(int(clientCount()));
    emit activityChanged(activityText());
  });
  m_serverNotifierConnection += m_server->activityDidChangeNotifier.connect(
    [this](const auto&) { emit activityChanged(activityText()); });
  m_server->callLog().addSink([this](const mcp::CallLogEntry& entry) {
    logInfo(formatCallLogEntryForConsole(entry));
  });

  m_transport = std::make_unique<McpTcpTransport>(*m_server);
  if (!m_transport->listen(
        QString::fromStdString(settings.bindAddress),
        quint16(settings.port),
        QString::fromStdString(settings.accessToken)))
  {
    const auto errorString = m_transport->errorString();
    m_transport.reset();
    m_serverNotifierConnection.disconnect();
    m_server.reset();
    m_host.reset();
    m_scheduler.reset();

    if (errorString.contains("in use", Qt::CaseInsensitive))
    {
      logError(fmt::format("MCP server: port {} in use", settings.port));
      setStatusText(tr("MCP: port %1 in use").arg(settings.port));
    }
    else
    {
      logError(fmt::format("MCP server not started: {}", errorString.toStdString()));
      setStatusText(tr("MCP: %1").arg(errorString));
    }
    return;
  }

  m_settings = settings;
  m_port = int(m_transport->serverPort());

  writeDiscoveryFile();
  updateLogToFile();

  logInfo(fmt::format("MCP server listening on {}:{}", settings.bindAddress, *m_port));
  setStatusText({});
  emit clientsChanged(0);
  emit activityChanged(activityText());
}

void McpServerController::stop()
{
  if (!m_server)
  {
    return;
  }

  m_server->stopAgents();
  m_transport->closeAllConnections();
  m_transport->close();

  if (m_fileSinkId)
  {
    m_server->callLog().removeSink(*m_fileSinkId);
    m_fileSinkId = std::nullopt;
  }
  m_fileSink.reset();

  // The transport refers to the server, and the server refers to the host and the
  // scheduler
  m_transport.reset();
  m_serverNotifierConnection.disconnect();
  m_server.reset();
  m_host.reset();
  m_scheduler.reset();

  removeDiscoveryFile();

  m_settings = std::nullopt;
  m_port = std::nullopt;

  emit clientsChanged(0);
  emit activityChanged(QString{});
  emit statusChanged();
}

void McpServerController::applyOptions()
{
  if (m_server)
  {
    auto options = m_server->options();
    options.busyWaitTimeout =
      std::chrono::milliseconds{std::max(0, pref(McpPreferences::McpBusyWaitTimeoutMs))};
    m_server->setOptions(options);
  }
}

void McpServerController::updateLogToFile()
{
  if (!m_server)
  {
    return;
  }

  if (pref(McpPreferences::McpLogToFile))
  {
    if (!m_fileSink)
    {
      const auto directory = logDirectoryPath();
      auto ec = std::error_code{};
      std::filesystem::create_directories(directory, ec);
      if (ec)
      {
        logError(fmt::format(
          "Could not create MCP log directory {}: {}", directory.string(), ec.message()));
        return;
      }

      const auto fileName = fmt::format(
        "{}-{}.jsonl",
        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss").toStdString(),
        QCoreApplication::applicationPid());
      m_fileSink = std::make_unique<mcp::JsonlFileSink>(directory / fileName);
      m_fileSinkId = m_server->callLog().addSink(
        [sink = m_fileSink.get()](const auto& entry) { sink->write(entry); });
    }
  }
  else if (m_fileSink)
  {
    m_server->callLog().removeSink(*m_fileSinkId);
    m_fileSinkId = std::nullopt;
    m_fileSink.reset();
  }
}

void McpServerController::writeDiscoveryFile()
{
  const auto path = discoveryFilePath();
  auto ec = std::error_code{};
  std::filesystem::create_directories(path.parent_path(), ec);

  const auto json = mcp::Json{
    {"port", *m_port},
    {"bind", m_settings->bindAddress},
    {"pid", QCoreApplication::applicationPid()},
    {"version", getBuildVersion().toStdString()},
  };

  // Write to a temporary file first so that readers never see a partial file
  auto tempPath = path;
  tempPath += ".tmp";
  {
    auto stream = std::ofstream{tempPath, std::ios::out | std::ios::trunc};
    stream << mcp::dumpJson(json);
    if (!stream)
    {
      logError(fmt::format("Could not write MCP discovery file {}", path.string()));
      return;
    }
  }

  std::filesystem::rename(tempPath, path, ec);
  if (ec)
  {
    std::filesystem::remove(tempPath, ec);
    logError(fmt::format("Could not write MCP discovery file {}", path.string()));
    return;
  }

  m_ownsDiscoveryFile = true;
}

void McpServerController::removeDiscoveryFile()
{
  if (!m_ownsDiscoveryFile)
  {
    return;
  }
  m_ownsDiscoveryFile = false;

  // Only remove the file if another instance hasn't replaced it in the meantime
  const auto path = discoveryFilePath();
  if (const auto json = readJsonFile(path))
  {
    const auto* pid = mcp::findMember(*json, "pid");
    if (pid && *pid != mcp::Json(QCoreApplication::applicationPid()))
    {
      return;
    }
  }

  auto ec = std::error_code{};
  std::filesystem::remove(path, ec);
}

void McpServerController::setStatusText(QString statusText)
{
  if (statusText != m_statusText)
  {
    m_statusText = std::move(statusText);
    emit statusChanged();
  }
}

void McpServerController::logInfo(const std::string& message)
{
  if (auto* mapWindow = m_appController.mapWindowManager().topMapWindow())
  {
    mapWindow->logger().info() << message;
  }
}

void McpServerController::logError(const std::string& message)
{
  if (auto* mapWindow = m_appController.mapWindowManager().topMapWindow())
  {
    mapWindow->logger().error() << message;
  }
  else
  {
    qWarning("%s", message.c_str());
  }
}

} // namespace tb::ui
