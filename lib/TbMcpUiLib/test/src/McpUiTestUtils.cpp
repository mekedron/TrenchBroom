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

#include "McpUiTestUtils.h"

#include <QCoreApplication>
#include <QEvent>
#include <QShowEvent>
#include <QtTest/QTest>

#include "CmdTool.h"
#include "gl/GlManager.h"
#include "mdl/CompilationProfile.h"
#include "mdl/CompilationTask.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/MapFormat.h"
#include "ui/AppController.h"
#include "ui/CompilationDialog.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"

#include "kd/result.h"

#include "vm/bbox.h"

#include <memory>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{

MapWindow& createMapWindow(AppController& appController)
{
  return createMapWindow(appController, mdl::QuakeGameInfo);
}

MapWindow& createMapWindow(AppController& appController, const mdl::GameInfo& gameInfo)
{
  auto document = MapDocument::createDocument(
                    appController.environmentConfig(),
                    gameInfo,
                    mdl::MapFormat::Valve,
                    vm::bbox3d{8192.0},
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();

  // Deleted on close because MapWindow sets Qt::WA_DeleteOnClose
  auto* mapWindow = new MapWindow{appController, std::move(document)};
  appController.mapWindowManager().addMapWindow(mapWindow);

  sendShowEvent(*mapWindow);
  return *mapWindow;
}

mdl::GameInfo withCompilationProfile(
  mdl::GameInfo gameInfo, const std::filesystem::path& workDir)
{
  gameInfo.compilationConfig.profiles = {mdl::CompilationProfile{
    .name = "Test",
    .workDirSpec = workDir.string(),
    .tasks = {mdl::CompilationRunTool{true, CMD_TOOL_PATH, "--exit 0", false}},
  }};
  return gameInfo;
}

const CompilationDialog& startCompilation(MapWindow& mapWindow)
{
  mapWindow.showCompileDialog();
  auto* dialog = mapWindow.compilationDialog();
  REQUIRE(dialog != nullptr);

  dialog->runSelectedProfile();
  return *dialog;
}

void waitForCompilation(const CompilationDialog& dialog)
{
  CHECK(QTest::qWaitFor([&]() { return !dialog.running(); }, 10000));
}

bool RefuseClose::eventFilter(QObject* watched, QEvent* event)
{
  if (event->type() == QEvent::Close)
  {
    event->ignore();
    return true;
  }
  return QObject::eventFilter(watched, event);
}

void sendShowEvent(MapWindow& mapWindow)
{
  auto event = QShowEvent{};
  QCoreApplication::sendEvent(&mapWindow, &event);
}

void closeAllMapWindows(AppController& appController)
{
  for (auto* mapWindow : appController.mapWindowManager().mapWindows())
  {
    REQUIRE(mapWindow->closeDiscardingChanges());
  }
  processDeferredDeletes();
}

void processDeferredDeletes()
{
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace tb::ui
