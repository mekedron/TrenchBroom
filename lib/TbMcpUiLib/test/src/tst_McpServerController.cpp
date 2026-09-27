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

#include <QCoreApplication>
#include <QHostAddress>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>

#include "McpUiTestUtils.h"
#include "base/PreferenceManager.h"
#include "mcp/CallLog.h"
#include "mcp/Json.h"
#include "mcp/McpServer.h"
#include "mdl/EnvironmentConfig.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapWindow.h"
#include "ui/McpPreferencePane.h"
#include "ui/McpPreferences.h"
#include "ui/McpServerController.h"
#include "ui/McpStatusIndicator.h"
#include "ui/PreferenceDialog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/**
 * Resets the MCP preferences to their defaults when it goes out of scope, because the
 * preference store is shared by all tests in the process.
 */
struct McpPreferenceGuard
{
  McpPreferenceGuard() { reset(); }
  ~McpPreferenceGuard() { reset(); }

  static void reset()
  {
    auto& prefs = PreferenceManager::instance();
    prefs.resetToDefault(McpPreferences::McpServerEnabled);
    prefs.resetToDefault(McpPreferences::McpServerPort);
    prefs.resetToDefault(McpPreferences::McpServerBindAddress);
    prefs.resetToDefault(McpPreferences::McpServerAccessToken);
    prefs.resetToDefault(McpPreferences::McpLogToFile);
    prefs.resetToDefault(McpPreferences::McpBusyWaitTimeoutMs);
    prefs.saveChanges();
  }
};

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

/**
 * Redirects QSettings to a test location while map windows save their state on close.
 */
struct TestModeStandardPaths
{
  TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(true); }
  ~TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(false); }
};

bool canConnect(const int port)
{
  auto socket = QTcpSocket{};
  socket.connectToHost(QHostAddress{QHostAddress::LocalHost}, quint16(port));
  return socket.waitForConnected(5000);
}

} // namespace

TEST_CASE("McpServerController")
{
  const auto preferenceGuard = McpPreferenceGuard{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto controller = McpServerController{appController};

  // Let the operating system choose a free port
  setPref(McpPreferences::McpServerPort, 0);

  const auto userDataDir = appController.environmentConfig().userDataFolderPath;
  REQUIRE(controller.discoveryFilePath() == userDataDir / "mcp-server.json");
  REQUIRE(controller.logDirectoryPath() == userDataDir / "mcp-logs");

  SECTION("server is disabled by default")
  {
    CHECK(!controller.enabled());
    CHECK(!controller.listening());
    CHECK(controller.port() == std::nullopt);
    CHECK(controller.server() == nullptr);
    CHECK(controller.statusText().isEmpty());
    CHECK(controller.activityText().isEmpty());
    CHECK(!std::filesystem::exists(controller.discoveryFilePath()));
  }

  SECTION("preference starts and stops the server")
  {
    setPref(McpPreferences::McpServerEnabled, true);

    CHECK(controller.enabled());
    REQUIRE(controller.listening());
    REQUIRE(controller.port().has_value());
    CHECK(*controller.port() > 0);
    CHECK(controller.server() != nullptr);
    CHECK(controller.host() != nullptr);
    CHECK(controller.clientCount() == 0);
    CHECK(controller.activityText() == "AI: 0 clients · idle");
    CHECK(canConnect(*controller.port()));

    const auto json = readJsonFile(controller.discoveryFilePath());
    REQUIRE(json.has_value());
    CHECK((*json)["port"] == *controller.port());
    CHECK((*json)["bind"] == "127.0.0.1");
    CHECK((*json)["pid"] == QCoreApplication::applicationPid());
    CHECK((*json)["version"].is_string());

    const auto port = *controller.port();
    setPref(McpPreferences::McpServerEnabled, false);

    CHECK(!controller.enabled());
    CHECK(!controller.listening());
    CHECK(controller.port() == std::nullopt);
    CHECK(controller.server() == nullptr);
    CHECK(!std::filesystem::exists(controller.discoveryFilePath()));
    CHECK(!canConnect(port));
  }

  SECTION("forced start ignores the preference")
  {
    controller.setForceEnabled(true);

    CHECK(controller.forceEnabled());
    CHECK(controller.enabled());
    CHECK(controller.listening());
    CHECK(std::filesystem::exists(controller.discoveryFilePath()));

    controller.setForceEnabled(false);

    CHECK(!controller.listening());
    CHECK(!std::filesystem::exists(controller.discoveryFilePath()));
  }

  SECTION("changing the port restarts the server")
  {
    setPref(McpPreferences::McpServerEnabled, true);
    REQUIRE(controller.listening());

    auto freePortServer = QTcpServer{};
    REQUIRE(freePortServer.listen(QHostAddress{QHostAddress::LocalHost}, 0));
    const auto freePort = int(freePortServer.serverPort());
    freePortServer.close();

    setPref(McpPreferences::McpServerPort, freePort);

    REQUIRE(controller.listening());
    CHECK(controller.port() == freePort);

    const auto json = readJsonFile(controller.discoveryFilePath());
    REQUIRE(json.has_value());
    CHECK((*json)["port"] == freePort);
  }

  SECTION("port in use")
  {
    auto blockingServer = QTcpServer{};
    REQUIRE(blockingServer.listen(QHostAddress{QHostAddress::LocalHost}, 0));
    const auto port = int(blockingServer.serverPort());

    setPref(McpPreferences::McpServerPort, port);
    setPref(McpPreferences::McpServerEnabled, true);

    CHECK(controller.enabled());
    CHECK(!controller.listening());
    CHECK(controller.statusText() == QString{"MCP: port %1 in use"}.arg(port));
    CHECK(!std::filesystem::exists(controller.discoveryFilePath()));

    // The server starts once the port becomes available
    blockingServer.close();
    setPref(McpPreferences::McpServerAccessToken, "retry");

    CHECK(controller.listening());
    CHECK(controller.statusText().isEmpty());
  }

  SECTION("non-loopback bind address requires an access token")
  {
    setPref(McpPreferences::McpServerBindAddress, "0.0.0.0");
    setPref(McpPreferences::McpServerEnabled, true);

    CHECK(!controller.listening());
    CHECK(!controller.statusText().isEmpty());
  }

  SECTION("busy wait timeout")
  {
    setPref(McpPreferences::McpBusyWaitTimeoutMs, 5000);
    setPref(McpPreferences::McpServerEnabled, true);
    REQUIRE(controller.server() != nullptr);

    CHECK(
      controller.server()->options().busyWaitTimeout == std::chrono::milliseconds{5000});

    setPref(McpPreferences::McpBusyWaitTimeoutMs, 7000);
    CHECK(
      controller.server()->options().busyWaitTimeout == std::chrono::milliseconds{7000});
  }

  SECTION("log to file")
  {
    setPref(McpPreferences::McpLogToFile, true);
    setPref(McpPreferences::McpServerEnabled, true);
    REQUIRE(controller.listening());

    const auto logFilePath = controller.logFilePath();
    REQUIRE(logFilePath.has_value());
    CHECK(logFilePath->parent_path() == controller.logDirectoryPath());
    CHECK(logFilePath->extension() == ".jsonl");

    auto entry = mcp::CallLogEntry{};
    entry.tool = "test_tool";
    controller.server()->callLog().add(std::move(entry));

    const auto json = readJsonFile(*logFilePath);
    REQUIRE(json.has_value());
    CHECK((*json)["tool"] == "test_tool");

    setPref(McpPreferences::McpLogToFile, false);
    CHECK(controller.logFilePath() == std::nullopt);
  }

  SECTION("stopAgents keeps the server listening")
  {
    setPref(McpPreferences::McpServerEnabled, true);
    REQUIRE(controller.listening());

    controller.stopAgents();

    CHECK(controller.listening());
    CHECK(controller.clientCount() == 0);
  }

  SECTION("status indicator")
  {
    auto indicator = McpStatusIndicator{controller};
    CHECK(indicator.isHidden());

    setPref(McpPreferences::McpServerEnabled, true);
    CHECK(!indicator.isHidden());
    CHECK(indicator.text() == "AI: 0 clients · idle");

    setPref(McpPreferences::McpServerEnabled, false);
    CHECK(indicator.isHidden());
  }
}

TEST_CASE("McpServerController editor integration")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};
  const auto preferenceGuard = McpPreferenceGuard{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  const auto statusIndicatorCount = [](MapWindow& mapWindow) {
    return mapWindow.statusBar()->findChildren<McpStatusIndicator*>().size();
  };

  SECTION("adds the status indicator to the open map windows")
  {
    auto& mapWindow = createMapWindow(appController);
    REQUIRE(statusIndicatorCount(mapWindow) == 0);

    {
      auto controller = McpServerController{appController};
      CHECK(statusIndicatorCount(mapWindow) == 1);
    }

    // The controller removes the indicators when it is destroyed
    CHECK(statusIndicatorCount(mapWindow) == 0);

    closeAllMapWindows(appController);
  }

  SECTION("adds the status indicator to map windows when they are shown")
  {
    auto controller = McpServerController{appController};

    auto& mapWindow = createMapWindow(appController);
    CHECK(statusIndicatorCount(mapWindow) == 1);

    // Only once
    sendShowEvent(mapWindow);
    CHECK(statusIndicatorCount(mapWindow) == 1);

    closeAllMapWindows(appController);
  }

  SECTION("adds the preference pane to preference dialogs when they are shown")
  {
    auto controller = McpServerController{appController};

    auto dialog = PreferenceDialog{appController, nullptr};
    CHECK(dialog.findChild<McpPreferencePane*>() == nullptr);

    dialog.show();
    CHECK(dialog.findChildren<McpPreferencePane*>().size() == 1);

    dialog.hide();
    dialog.show();
    CHECK(dialog.findChildren<McpPreferencePane*>().size() == 1);

    dialog.hide();
  }
}

TEST_CASE("formatCallLogEntryForConsole")
{
  auto entry = mcp::CallLogEntry{};
  entry.tool = "brush_create_box";
  entry.durationMs = 3.2;
  entry.created = 1;
  entry.modified = 2;

  CHECK(
    formatCallLogEntryForConsole(entry)
    == "[AI] brush_create_box ok 3 ms (+1 created, ~2 modified, -0 removed)");

  entry.dryRun = true;
  CHECK(
    formatCallLogEntryForConsole(entry)
    == "[AI] brush_create_box ok 3 ms (+1 created, ~2 modified, -0 removed) [dry run]");

  entry.dryRun = false;
  entry.ok = false;
  entry.errorCode = "NOT_FOUND";
  CHECK(
    formatCallLogEntryForConsole(entry)
    == "[AI] brush_create_box failed: NOT_FOUND (3 ms)");
}

TEST_CASE("formatMcpActivity")
{
  using State = mcp::ServerActivity::State;

  auto activity = mcp::ServerActivity{};
  CHECK(formatMcpActivity(activity, 0) == "AI: 0 clients · idle");
  CHECK(formatMcpActivity(activity, 1) == "AI: 1 client · idle");

  activity.state = State::Running;
  activity.toolTitle = "Create Box";
  CHECK(formatMcpActivity(activity, 2) == "AI: 2 clients · Create Box");

  activity.state = State::WaitingForUser;
  CHECK(formatMcpActivity(activity, 1) == "AI: 1 client · waiting for you");

  activity.state = State::Idle;
  activity.openTransactions = {"Build room"};
  CHECK(formatMcpActivity(activity, 1) == "AI transaction open: Build room");
}

} // namespace tb::ui
