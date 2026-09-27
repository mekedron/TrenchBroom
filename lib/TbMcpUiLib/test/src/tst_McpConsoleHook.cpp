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


#include <QStandardPaths>
#include <QTest>
#include <QTextEdit>

#include "McpUiTestUtils.h"
#include "base/PreferenceManager.h"
#include "mcp/CallLog.h"
#include "mcp/ConsoleBuffer.h"
#include "mcp/McpServer.h"
#include "mdl/Map.h"
#include "ui/AppControllerFixture.h"
#include "ui/Console.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/McpConsoleHook.h"
#include "ui/McpPreferences.h"
#include "ui/McpServerController.h"
#include "ui/QtMcpHost.h"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/**
 * Redirects QSettings to a test location while map windows save their state on close.
 */
struct TestModeStandardPaths
{
  TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(true); }
  ~TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(false); }
};

/** Resets the MCP preferences, which are shared by all tests in the process. */
struct McpPreferenceGuard
{
  McpPreferenceGuard() { reset(); }
  ~McpPreferenceGuard() { reset(); }

  static void reset()
  {
    auto& prefs = PreferenceManager::instance();
    prefs.resetToDefault(McpPreferences::McpServerEnabled);
    prefs.resetToDefault(McpPreferences::McpServerPort);
    prefs.resetToDefault(McpPreferences::McpLogToFile);
    prefs.saveChanges();
  }
};

const mcp::ConsoleMessage* findMessage(
  const mcp::ConsoleBuffer& buffer, const std::string& text)
{
  const auto& messages = buffer.messages();
  const auto it = std::ranges::find_if(
    messages, [&](const auto& message) { return message.text == text; });
  return it != messages.end() ? &*it : nullptr;
}

QTextEdit& textView(Console& console)
{
  auto* result = console.findChild<QTextEdit*>();
  REQUIRE(result);
  return *result;
}

} // namespace

TEST_CASE("McpConsoleHook")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  auto hook = McpConsoleHook{};
  auto& buffer = hook.buffer();

  SECTION("collects the messages of map window consoles with level and document")
  {
    auto& mapWindow = createMapWindow(appController);
    auto& document = mapWindow.document();

    mapWindow.logger().warn() << "window warning";
    document.logger().error() << "document error";
    document.logger().debug() << "document debug";

    const auto* warning = findMessage(buffer, "window warning");
    REQUIRE(warning);
    CHECK(warning->level == LogLevel::Warn);
    CHECK(warning->document == &document);
    CHECK(warning->documentName == document.map().path().filename().string());
    CHECK(!warning->documentName.empty());

    const auto* error = findMessage(buffer, "document error");
    REQUIRE(error);
    CHECK(error->level == LogLevel::Error);
    CHECK(error->document == &document);

    const auto* debug = findMessage(buffer, "document debug");
    REQUIRE(debug);
    CHECK(debug->level == LogLevel::Debug);

    SECTION("messages that a document logged before its window existed")
    {
      // the window flushes the document's cached messages into its console
      CHECK(std::ranges::all_of(buffer.messages(), [&](const auto& message) {
        return message.document == &document;
      }));
    }

    closeAllMapWindows(appController);
  }

  SECTION("messages of a console outside of a map window have no document")
  {
    auto console = Console{};
    console.info() << "no window";

    const auto* message = findMessage(buffer, "no window");
    REQUIRE(message);
    CHECK(message->level == LogLevel::Info);
    CHECK(message->document == nullptr);
    CHECK(message->documentName.empty());
  }

  SECTION("messages logged on other threads are added on the main thread")
  {
    auto& mapWindow = createMapWindow(appController);
    auto& document = mapWindow.document();

    auto thread =
      std::thread{[&]() { document.logger().warn() << "from a worker thread"; }};
    thread.join();

    CHECK(findMessage(buffer, "from a worker thread") == nullptr);
    REQUIRE(QTest::qWaitFor(
      [&]() { return findMessage(buffer, "from a worker thread") != nullptr; }));

    const auto* message = findMessage(buffer, "from a worker thread");
    CHECK(message->level == LogLevel::Warn);
    CHECK(message->document == &document);

    closeAllMapWindows(appController);
  }

  SECTION("a destroyed hook drops the messages that were not delivered yet")
  {
    auto console = Console{};
    auto otherHook = std::make_unique<McpConsoleHook>();
    auto thread = std::thread{[&]() { console.info() << "late"; }};
    thread.join();
    otherHook.reset();

    QTest::qWait(10);
    CHECK(findMessage(buffer, "late") != nullptr);
  }

  SECTION("several hooks see the same messages")
  {
    auto otherHook = McpConsoleHook{2};
    auto console = Console{};
    console.info() << "a";
    console.info() << "b";
    console.info() << "c";

    CHECK(otherHook.buffer().messages().size() == 2);
    CHECK(otherHook.buffer().droppedCount() == 1);
    CHECK(findMessage(buffer, "a") != nullptr);
  }

  SECTION("clearConsoleViews")
  {
    auto& mapWindow = createMapWindow(appController);
    auto* console = dynamic_cast<Console*>(&mapWindow.logger());
    REQUIRE(console);
    auto otherConsole = Console{};

    mapWindow.logger().info() << "in the window";
    otherConsole.info() << "elsewhere";
    REQUIRE(QTest::qWaitFor([&]() {
      return !textView(*console).toPlainText().isEmpty()
             && !textView(otherConsole).toPlainText().isEmpty();
    }));

    const auto count = buffer.messages().size();
    hook.clearConsoleViews();
    CHECK(textView(*console).toPlainText().isEmpty());
    CHECK(textView(otherConsole).toPlainText().isEmpty());
    // the buffer is cleared separately
    CHECK(buffer.messages().size() == count);

    closeAllMapWindows(appController);

    // closed consoles are skipped
    hook.clearConsoleViews();
  }

  SECTION("QtMcpHost")
  {
    auto host = QtMcpHost{appController};
    CHECK(host.consoleBuffer() == nullptr);
    host.clearConsoleViews();

    host.setConsoleHook(&hook);
    CHECK(host.consoleBuffer() == &buffer);

    auto console = Console{};
    console.info() << "shown";
    REQUIRE(
      QTest::qWaitFor([&]() { return !textView(console).toPlainText().isEmpty(); }));
    host.clearConsoleViews();
    CHECK(textView(console).toPlainText().isEmpty());
  }
}

TEST_CASE("McpServerController console hook")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};
  const auto preferenceGuard = McpPreferenceGuard{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto controller = McpServerController{appController};
  setPref(McpPreferences::McpServerPort, 0);
  setPref(McpPreferences::McpLogToFile, false);

  auto& buffer = controller.consoleHook().buffer();
  auto& mapWindow = createMapWindow(appController);

  // collects messages while the server is stopped
  REQUIRE(!controller.listening());
  mapWindow.logger().info() << "before the server started";
  CHECK(findMessage(buffer, "before the server started") != nullptr);

  setPref(McpPreferences::McpServerEnabled, true);
  REQUIRE(controller.listening());
  CHECK(controller.host()->consoleBuffer() == &buffer);
  CHECK(findMessage(buffer, "before the server started") != nullptr);

  // the call log lines reach the buffer
  auto entry = mcp::CallLogEntry{};
  entry.tool = "test_tool";
  entry.ok = true;
  controller.server()->callLog().add(std::move(entry));
  CHECK(std::ranges::any_of(buffer.messages(), [](const auto& message) {
    return message.text.starts_with("[AI] test_tool ok");
  }));

  // the buffer outlives the server
  setPref(McpPreferences::McpServerEnabled, false);
  REQUIRE(!controller.listening());
  mapWindow.logger().info() << "after the server stopped";
  CHECK(findMessage(buffer, "after the server stopped") != nullptr);

  closeAllMapWindows(appController);
}

} // namespace tb::ui
