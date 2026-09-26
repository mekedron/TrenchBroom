/*
 Copyright (C) 2010 Kristian Duske

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

#include "base/Result.h"

#include "vm/bbox.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb
{
namespace gl
{
class ResourceManager;
}

namespace mdl
{
enum class MapFormat;

struct EnvironmentConfig;
struct GameInfo;
} // namespace mdl

namespace ui
{
class AppController;
class MapDocument;
class MapWindow;

class MapWindowManager : public QObject
{
  Q_OBJECT
private:
  AppController& m_appController;
  bool m_singleMapWindow;
  std::vector<MapWindow*> m_mapWindows;

public:
  MapWindowManager(AppController& appController, bool singleMapWindow);
  ~MapWindowManager() override;

  Result<void> createDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const vm::bbox3d& worldBounds);

  Result<void> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const vm::bbox3d& worldBounds,
    std::filesystem::path path);

  std::vector<MapWindow*> mapWindows() const;
  MapWindow* topMapWindow() const;
  bool allMapWindowsClosed() const;

  /**
   * Registers the given window as the top window. The window must have been created with
   * this manager's app controller. The manager does not show the window.
   */
  void addMapWindow(MapWindow* mapWindow);

signals:
  /**
   * Emitted when the given window is about to close, before its document is destroyed.
   */
  void mapWindowWillClose(MapWindow* mapWindow);

  /**
   * Emitted after a window was opened or closed, after a window's document was replaced,
   * or when the window order changed because another window received the focus.
   */
  void mapWindowsDidChange();

private:
  void onFocusChange(QWidget* old, QWidget* now);

  bool shouldCreateWindowForDocument() const;
  MapWindow* createMapWindow(std::unique_ptr<MapDocument> document);
  void removeMapWindow(MapWindow* mapWindow);

  friend class MapWindow;
};

} // namespace ui
} // namespace tb
