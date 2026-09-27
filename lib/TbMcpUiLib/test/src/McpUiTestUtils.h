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

#include "mdl/GameInfo.h"

#include <filesystem>

namespace tb::ui
{
class AppController;
class CompilationDialog;
class MapWindow;

/**
 * Creates a map window for a new Quake document (or the given game) and registers it with
 * the map window manager as the top window, like MapWindowManager::createMapWindow. The
 * window is not shown because the offscreen platform used for the tests doesn't support
 * OpenGL; instead, the application's event filters receive a show event for it.
 *
 * The window is deleted when it is closed and control returns to the event loop. The
 * window's map refers to the given game info, which must outlive the window.
 */
MapWindow& createMapWindow(AppController& appController);
MapWindow& createMapWindow(AppController& appController, const mdl::GameInfo& gameInfo);

/**
 * Returns the given game info with a single compilation profile, which runs the CmdTool
 * stub in the given working directory.
 */
mdl::GameInfo withCompilationProfile(
  mdl::GameInfo gameInfo, const std::filesystem::path& workDir);

/**
 * Shows the compilation dialog of the given window and runs its first profile. The run
 * does not end before control returns to the event loop.
 */
const CompilationDialog& startCompilation(MapWindow& mapWindow);

/** Waits until the run of the given compilation dialog has ended. */
void waitForCompilation(const CompilationDialog& dialog);

/** An event filter that makes the watched windows refuse to close. */
class RefuseClose : public QObject
{
  Q_OBJECT
protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
};

/** Sends a show event to the application's event filters and the given window. */
void sendShowEvent(MapWindow& mapWindow);

/** Closes all map windows, discarding their unsaved changes. */
void closeAllMapWindows(AppController& appController);

/** Processes the pending deferred deletes, e.g. of closed windows. */
void processDeferredDeletes();

} // namespace tb::ui
