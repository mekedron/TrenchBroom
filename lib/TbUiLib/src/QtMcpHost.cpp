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

#include "ui/QtMcpHost.h"

#include <QApplication>

#include "mdl/Map.h"
#include "ui/AppController.h"
#include "ui/GetVersion.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"

#include "kd/ranges/to.h"

#include <fmt/format.h>

#include <algorithm>
#include <ranges>

namespace tb::ui
{
namespace
{

/**
 * Whether the active modal tool keeps state that depends on the current selection and
 * would become stale if the agent changed the map, e.g. vertex handles or clip points.
 */
bool modalToolHoldsSelectionState(const MapViewToolBox& toolBox)
{
  return toolBox.selectionOwnedByTool() || toolBox.anyNodeHandleToolActive()
         || toolBox.clipToolActive();
}

std::string windowTitle(const MapDocument& document)
{
  const auto& map = document.map();
  return fmt::format("{}{} - TrenchBroom", map.filename(), map.modified() ? "*" : "");
}

} // namespace

std::optional<std::string> activeModalToolName(const MapViewToolBox& toolBox)
{
  if (toolBox.clipToolActive())
  {
    return "Clip Tool";
  }
  if (toolBox.assembleBrushToolActive())
  {
    return "Assemble Brush Tool";
  }
  if (toolBox.rotateToolActive())
  {
    return "Rotate Tool";
  }
  if (toolBox.sweepToolActive())
  {
    return "Sweep Tool";
  }
  if (toolBox.scaleToolActive())
  {
    return "Scale Tool";
  }
  if (toolBox.shearToolActive())
  {
    return "Shear Tool";
  }
  if (toolBox.vertexToolActive())
  {
    return "Vertex Tool";
  }
  if (toolBox.edgeToolActive())
  {
    return "Edge Tool";
  }
  if (toolBox.faceToolActive())
  {
    return "Face Tool";
  }
  if (toolBox.controlPointToolActive())
  {
    return "Control Point Tool";
  }
  return std::nullopt;
}

QtMcpHost::QtMcpHost(AppController& appController, QObject* parent)
  : QObject{parent}
  , m_appController{appController}
{
  auto& mapWindowManager = m_appController.mapWindowManager();
  connect(
    &mapWindowManager,
    &MapWindowManager::mapWindowWillClose,
    this,
    &QtMcpHost::mapWindowWillClose);
  connect(
    &mapWindowManager,
    &MapWindowManager::mapWindowsDidChange,
    this,
    &QtMcpHost::mapWindowsDidChange);

  assignDocumentIds();
}

QtMcpHost::~QtMcpHost() = default;

MapWindow* QtMcpHost::findMapWindow(const MapDocument& document) const
{
  const auto mapWindows = m_appController.mapWindowManager().mapWindows();
  const auto it = std::ranges::find_if(mapWindows, [&](const auto* mapWindow) {
    return &mapWindow->document() == &document;
  });
  return it != mapWindows.end() ? *it : nullptr;
}

std::string QtMcpHost::applicationVersion() const
{
  return getBuildVersion().toStdString();
}

std::vector<mcp::DocumentInfo> QtMcpHost::documents()
{
  assignDocumentIds();

  const auto mapWindows = m_appController.mapWindowManager().mapWindows();
  auto* focusedWindow = mapWindows.empty() ? nullptr : mapWindows.front();

  auto result = std::vector<std::pair<size_t, mcp::DocumentInfo>>{};
  for (auto* mapWindow : mapWindows)
  {
    auto& document = mapWindow->document();
    const auto id = documentId(document);
    result.emplace_back(
      id,
      mcp::DocumentInfo{
        .id = fmt::format("doc:{}", id),
        .document = &document,
        .windowTitle = windowTitle(document),
        .focused = mapWindow == focusedWindow,
      });
  }

  // List the documents in the order in which they were opened
  std::ranges::sort(result, {}, [](const auto& pair) { return pair.first; });

  return result | std::views::values | kdl::ranges::to<std::vector>();
}

mcp::BusyState QtMcpHost::busyState(MapDocument& document)
{
  if (QApplication::activeModalWidget() != nullptr)
  {
    return mcp::BusyState::Busy;
  }

  if (const auto* mapWindow = findMapWindow(document);
      mapWindow && mapWindow->toolBox().dragging())
  {
    return mcp::BusyState::Busy;
  }

  return mcp::BusyState::Idle;
}

std::vector<std::string> QtMcpHost::prepareForAgentEdit(MapDocument& document)
{
  auto notes = std::vector<std::string>{};

  if (auto* mapWindow = findMapWindow(document))
  {
    auto& toolBox = mapWindow->toolBox();
    while (modalToolHoldsSelectionState(toolBox))
    {
      const auto toolName = activeModalToolName(toolBox).value_or("modal tool");
      if (!toolBox.deactivateCurrentTool())
      {
        break;
      }
      notes.push_back(fmt::format("deactivated tool: {}", toolName));
    }
  }

  return notes;
}

std::optional<std::string> QtMcpHost::currentToolName(MapDocument& document)
{
  if (const auto* mapWindow = findMapWindow(document))
  {
    return activeModalToolName(mapWindow->toolBox());
  }
  return std::nullopt;
}

bool QtMcpHost::isCompileRunning(MapDocument& document)
{
  const auto* mapWindow = findMapWindow(document);
  return mapWindow && mapWindow->compilationRunning();
}

void QtMcpHost::assignDocumentIds()
{
  const auto mapWindows = m_appController.mapWindowManager().mapWindows();

  // Forget documents that are no longer open. Their ids are never reused.
  std::erase_if(m_documentIds, [&](const auto& entry) {
    return std::ranges::none_of(mapWindows, [&](const auto* mapWindow) {
      return &mapWindow->document() == entry.first;
    });
  });

  // The most recently focused window comes first, so older windows receive their ids
  // first. New windows are always focused when they open.
  for (const auto* mapWindow : mapWindows | std::views::reverse)
  {
    documentId(mapWindow->document());
  }
}

size_t QtMcpHost::documentId(const MapDocument& document)
{
  if (const auto it = m_documentIds.find(&document); it != m_documentIds.end())
  {
    return it->second;
  }
  return m_documentIds.emplace(&document, m_nextDocumentId++).first->second;
}

void QtMcpHost::mapWindowWillClose(MapWindow* mapWindow)
{
  auto& document = mapWindow->document();
  documentWillCloseNotifier(document);
  m_documentIds.erase(&document);
}

void QtMcpHost::mapWindowsDidChange()
{
  assignDocumentIds();
  documentsDidChangeNotifier();
}

} // namespace tb::ui
