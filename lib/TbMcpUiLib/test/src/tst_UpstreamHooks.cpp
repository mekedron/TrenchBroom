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
#include "fs/TestEnvironment.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/VisibilityState.h"
#include "ui/AppControllerFixture.h"
#include "ui/CompilationDialog.h"
#include "ui/Console.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/SwitchableMapViewContainer.h"

#include <string>
#include <thread>
#include <vector>

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

} // namespace

/*
 * Tests of the hooks that the fork adds to TrenchBroom's classes for the MCP server.
 * PreferenceDialog::addPane is tested with addMcpPreferencePane in tst_McpUiIntegration.
 */

TEST_CASE("MapWindowManager hooks")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto& mapWindowManager = appController.mapWindowManager();

  SECTION("addMapWindow")
  {
    // createMapWindow registers the windows with addMapWindow
    auto& first = createMapWindow(appController);
    auto& second = createMapWindow(appController);

    CHECK(mapWindowManager.mapWindows() == std::vector<MapWindow*>{&second, &first});
    CHECK(mapWindowManager.topMapWindow() == &second);

    closeAllMapWindows(appController);
    CHECK(mapWindowManager.allMapWindowsClosed());
  }
}

TEST_CASE("MapWindow hooks")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  SECTION("mapView")
  {
    auto& mapWindow = createMapWindow(appController);
    CHECK(&mapWindow.mapView() == mapWindow.findChild<SwitchableMapViewContainer*>());

    closeAllMapWindows(appController);
  }

  SECTION("closeDiscardingChanges")
  {
    auto& mapWindow = createMapWindow(appController);
    auto& map = mapWindow.document().map();
    map.incModificationCount(2);
    REQUIRE(map.modified());

    SECTION("closes the window without asking")
    {
      CHECK(mapWindow.closeDiscardingChanges());
      CHECK(appController.mapWindowManager().allMapWindowsClosed());
      processDeferredDeletes();
    }

    SECTION("keeps the unsaved changes if the window refuses to close")
    {
      auto refuseClose = RefuseClose{};
      mapWindow.installEventFilter(&refuseClose);

      CHECK(!mapWindow.closeDiscardingChanges());
      CHECK(map.modified());
      CHECK(map.modificationCount() == 2);

      mapWindow.removeEventFilter(&refuseClose);
      closeAllMapWindows(appController);
    }
  }

  SECTION("compilationDialog")
  {
    auto& mapWindow = createMapWindow(appController);
    CHECK(mapWindow.compilationDialog() == nullptr);

    mapWindow.showCompileDialog();
    auto* dialog = mapWindow.compilationDialog();
    CHECK(dialog != nullptr);
    CHECK(dialog->parent() == &mapWindow);

    // The window keeps the dialog when it is closed
    dialog->close();
    CHECK(mapWindow.compilationDialog() == dialog);

    closeAllMapWindows(appController);
  }
}

TEST_CASE("CompilationDialog hooks")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto env = fs::TestEnvironment{};
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  SECTION("running")
  {
    // The map refers to its game info
    const auto gameInfo = withCompilationProfile(mdl::QuakeGameInfo, env.dir());
    auto& mapWindow = createMapWindow(appController, gameInfo);
    mapWindow.showCompileDialog();
    CHECK(!mapWindow.compilationDialog()->running());

    const auto& dialog = startCompilation(mapWindow);
    CHECK(dialog.running());

    waitForCompilation(dialog);
    CHECK(!dialog.running());

    closeAllMapWindows(appController);
  }
}

TEST_CASE("Console hooks")
{
  auto console = Console{};

  SECTION("messageLoggedNotifier")
  {
    struct Message
    {
      Console* console;
      LogLevel level;
      std::string text;
      std::thread::id thread;
    };
    auto messages = std::vector<Message>{};
    auto connection = Console::messageLoggedNotifier.connect(
      [&](Console& source, const LogLevel level, const std::string_view text) {
        messages.push_back(
          {&source, level, std::string{text}, std::this_thread::get_id()});
        // messages logged by an observer are not reported (and do not deadlock)
        source.debug() << "logged by the observer";
      });

    SECTION("reports every message synchronously with its level")
    {
      console.warn() << "a warning";
      console.info() << "some info";
      console.info() << "";

      REQUIRE(messages.size() == 2);
      CHECK(messages[0].console == &console);
      CHECK(messages[0].level == LogLevel::Warn);
      CHECK(messages[0].text == "a warning");
      CHECK(messages[1].level == LogLevel::Info);
      CHECK(messages[1].text == "some info");
    }

    SECTION("reports messages on the logging thread")
    {
      auto thread = std::thread{[&]() { console.error() << "from a worker"; }};
      const auto workerId = thread.get_id();
      thread.join();

      REQUIRE(messages.size() == 1);
      CHECK(messages[0].text == "from a worker");
      CHECK(messages[0].level == LogLevel::Error);
      CHECK(messages[0].thread == workerId);
    }
  }

  SECTION("clear")
  {
    auto* textView = console.findChild<QTextEdit*>();
    REQUIRE(textView);

    console.info() << "first";
    REQUIRE(QTest::qWaitFor([&]() { return !textView->toPlainText().isEmpty(); }));

    // also drops the messages that were not shown yet
    console.info() << "second";
    console.clear();
    CHECK(textView->toPlainText().isEmpty());

    QTest::qWait(100);
    CHECK(textView->toPlainText().isEmpty());
  }
}

TEST_CASE("EditorContext hooks")
{
  SECTION("setIgnoreHiddenState")
  {
    // The snapshot renderer draws the objects that the MCP core chose, even if they are
    // hidden in the editor
    const auto worldBounds = vm::bbox3d{8192.0};
    const auto builder = mdl::BrushBuilder{mdl::MapFormat::Standard, worldBounds};

    auto groupNode = mdl::GroupNode{mdl::Group{"group"}};
    auto* brushNode =
      new mdl::BrushNode{builder.createCube(32.0, "material") | kdl::value()};
    groupNode.addChild(brushNode);

    auto entityNode = mdl::EntityNode{mdl::Entity{{{"classname", "info_null"}}}};

    groupNode.setVisibilityState(mdl::VisibilityState::Hidden);
    entityNode.setVisibilityState(mdl::VisibilityState::Hidden);

    auto editorContext = mdl::EditorContext{};
    CHECK(!editorContext.ignoreHiddenState());
    CHECK(!editorContext.visible(groupNode));
    CHECK(!editorContext.visible(*brushNode));
    CHECK(!editorContext.visible(entityNode));

    editorContext.setIgnoreHiddenState(true);
    CHECK(editorContext.ignoreHiddenState());
    CHECK(editorContext.visible(groupNode));
    CHECK(editorContext.visible(*brushNode));
    CHECK(editorContext.visible(entityNode));

    // Other filters still apply
    editorContext.setShowBrushes(false);
    CHECK(!editorContext.visible(*brushNode));
  }
}

} // namespace tb::ui
