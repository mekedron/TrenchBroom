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

#include <QAction>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>

#include "McpUiTestUtils.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapWindow.h"
#include "ui/McpPreferencePane.h"
#include "ui/McpServerController.h"
#include "ui/McpStatusIndicator.h"
#include "ui/McpUiIntegration.h"
#include "ui/PreferenceDialog.h"
#include "ui/PreferencePane.h"

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

QAction* findAction(QToolBar& toolBar, const QString& text)
{
  for (auto* action : toolBar.actions())
  {
    if (action->text() == text)
    {
      return action;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("McpUiIntegration")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  SECTION("addMcpStatusIndicator")
  {
    auto controller = McpServerController{appController};
    auto& mapWindow = createMapWindow(appController);

    // The controller already added the indicator when the window was shown
    auto* indicator = mapWindow.statusBar()->findChild<McpStatusIndicator*>();
    REQUIRE(indicator != nullptr);

    CHECK(addMcpStatusIndicator(mapWindow, controller) == indicator);
    CHECK(mapWindow.statusBar()->findChildren<McpStatusIndicator*>().size() == 1);

    delete indicator;
    indicator = addMcpStatusIndicator(mapWindow, controller);
    REQUIRE(indicator != nullptr);
    CHECK(indicator->parentWidget() == mapWindow.statusBar());

    closeAllMapWindows(appController);
  }

  SECTION("addMcpPreferencePane")
  {
    auto controller = McpServerController{appController};
    auto dialog = PreferenceDialog{appController, nullptr};

    auto* toolBar = dialog.findChild<QToolBar*>();
    auto* stackedWidget = dialog.findChild<QStackedWidget*>();
    auto* buttonBox = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(toolBar != nullptr);
    REQUIRE(stackedWidget != nullptr);
    REQUIRE(buttonBox != nullptr);

    auto* firstPane = stackedWidget->currentWidget();
    const auto paneCount = stackedWidget->count();
    const auto actionCount = toolBar->actions().size();

    auto* pane = addMcpPreferencePane(dialog, controller);
    REQUIRE(pane != nullptr);
    CHECK(stackedWidget->count() == paneCount + 1);
    CHECK(stackedWidget->indexOf(pane) == paneCount);
    CHECK(stackedWidget->currentWidget() == firstPane);
    CHECK(dialog.height() >= pane->contentSizeHint().height());

    auto* action = findAction(*toolBar, "AI Agents");
    REQUIRE(action != nullptr);
    CHECK(toolBar->actions().size() == actionCount + 1);
    CHECK(toolBar->actions().back() == action);
    CHECK(!action->icon().isNull());

    // Only once
    CHECK(addMcpPreferencePane(dialog, controller) == pane);
    CHECK(stackedWidget->count() == paneCount + 1);
    CHECK(toolBar->actions().size() == actionCount + 1);

    SECTION("the tool bar button switches to the pane")
    {
      auto* resetButton = buttonBox->button(QDialogButtonBox::RestoreDefaults);
      REQUIRE(resetButton != nullptr);
      resetButton->setEnabled(false);

      action->trigger();
      CHECK(stackedWidget->currentWidget() == pane);
      CHECK(resetButton->isEnabled());
    }

    SECTION("removing the pane removes its button")
    {
      delete pane;
      CHECK(stackedWidget->count() == paneCount);
      CHECK(findAction(*toolBar, "AI Agents") == nullptr);
    }
  }
}

} // namespace tb::ui
