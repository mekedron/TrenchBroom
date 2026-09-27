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


#include "ui/McpViewHost.h"

#include <QApplication>
#include <QList>
#include <QWidget>

#include "base/PreferenceManager.h"
#include "gl/Camera.h"
#include "gl/PerspectiveCamera.h"
#include "prefs/Preferences.h"
#include "ui/MapViewBase.h"
#include "ui/MapWindow.h"
#include "ui/McpSnapshotRenderer.h"
#include "ui/SwitchableMapViewContainer.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace tb::ui
{
namespace
{

constexpr auto MinZoom = 0.02;
constexpr auto MaxZoom = 100.0;

std::vector<MapViewBase*> mapViews(MapWindow& mapWindow)
{
  const auto result = mapWindow.mapView().findChildren<MapViewBase*>();
  return {result.begin(), result.end()};
}

MapViewBase* findMapView(MapWindow& mapWindow, const std::string& viewId)
{
  for (auto* mapView : mapViews(mapWindow))
  {
    if (mapViewId(*mapView) == viewId)
    {
      return mapView;
    }
  }
  return nullptr;
}

/** Whether the view is shown in the window's layout (also if the window is not shown). */
bool isShown(const MapViewBase& mapView, const MapWindow& mapWindow)
{
  return mapView.isVisibleTo(&mapWindow);
}

bool isOnePane()
{
  return pref(Preferences::MapViewLayout) == 0;
}

/**
 * If the view shares a pane with other views (a CyclingMapView shows one of them at a
 * time), cycles the pane until it shows the view.
 */
void cycleIntoView(MapViewBase& mapView)
{
  auto* pane = mapView.parentWidget();
  if (!pane)
  {
    return;
  }
  const auto siblings = pane->findChildren<MapViewBase*>(Qt::FindDirectChildrenOnly);
  for (qsizetype i = 0; i < siblings.size() && !mapView.isVisibleTo(pane); ++i)
  {
    const auto it = std::ranges::find_if(
      siblings, [&](const auto* sibling) { return sibling->isVisibleTo(pane); });
    if (it == siblings.end())
    {
      return;
    }
    (*it)->cycleMapView();
  }
}

} // namespace

McpViewHost::McpViewHost(FindMapWindow findMapWindow)
  : m_findMapWindow{std::move(findMapWindow)}
{
}

McpViewHost::~McpViewHost() = default;

MapWindow* McpViewHost::mapWindow(const MapDocument& document) const
{
  return m_findMapWindow ? m_findMapWindow(document) : nullptr;
}

std::vector<mcp::UserView> McpViewHost::views(MapDocument& document)
{
  auto* window = mapWindow(document);
  if (!window)
  {
    return {};
  }

  auto result = std::vector<mcp::UserView>{};
  for (auto* mapView : mapViews(*window))
  {
    auto id = mapViewId(*mapView);
    if (std::ranges::none_of(result, [&](const auto& view) { return view.id == id; }))
    {
      const auto ratio = mapView->devicePixelRatioF();
      result.push_back(mcp::UserView{
        .id = std::move(id),
        .visible = isShown(*mapView, *window),
        .width = size_t(std::round(mapView->width() * ratio)),
        .height = size_t(std::round(mapView->height() * ratio)),
        .camera = toAgentCamera(mapView->camera()),
      });
    }
  }

  std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
    const auto order = [](const auto& id) {
      return id == "3d" ? 0 : id == "xy" ? 1 : id == "xz" ? 2 : 3;
    };
    return order(lhs.id) < order(rhs.id);
  });
  return result;
}

Result<void> McpViewHost::setCamera(
  MapDocument& document, const std::string& viewId, const mcp::AgentCamera& camera)
{
  auto* window = mapWindow(document);
  if (!window)
  {
    return Error{"The document has no window"};
  }
  auto* mapView = findMapView(*window, viewId);
  if (!mapView)
  {
    return Error{fmt::format("The document's window has no view '{}'", viewId)};
  }

  auto& glCamera = mapView->camera();
  if (auto* perspectiveCamera = dynamic_cast<gl::PerspectiveCamera*>(&glCamera))
  {
    if (camera.projection != mcp::CameraProjection::Perspective)
    {
      return Error{fmt::format("The view '{}' is a 3D view", viewId)};
    }
    if (std::abs(camera.fov - double(perspectiveCamera->zoomedFov())) > 1e-3)
    {
      if (camera.fov < 1.0 || camera.fov > 150.0)
      {
        return Error{fmt::format("Invalid field of view {}", camera.fov)};
      }
      perspectiveCamera->setZoom(1.0f);
      perspectiveCamera->setFov(float(camera.fov));
    }
    glCamera.moveTo(vm::vec3f{camera.position});
    glCamera.setDirection(vm::vec3f{camera.direction}, vm::vec3f{camera.up});
    return kdl::void_success;
  }

  if (camera.projection != mcp::CameraProjection::Orthographic)
  {
    return Error{fmt::format("The view '{}' is a 2D view", viewId)};
  }
  if (camera.zoom < MinZoom || camera.zoom > MaxZoom)
  {
    return Error{fmt::format(
      "Invalid zoom {}; the 2D views zoom from {} to {}", camera.zoom, MinZoom, MaxZoom)};
  }

  // Like MapView2D::animateCamera: the position along the view axis is kept
  const auto position = vm::vec3f{camera.position};
  const auto& axis = glCamera.direction();
  glCamera.setZoom(float(camera.zoom));
  glCamera.moveTo(
    position - vm::dot(position, axis) * axis
    + vm::dot(glCamera.position(), axis) * axis);
  return kdl::void_success;
}

Result<mcp::ViewLayout> McpViewHost::layout(MapDocument& document)
{
  auto* window = mapWindow(document);
  if (!window)
  {
    return Error{"The document has no window"};
  }

  auto result = mcp::ViewLayout{};
  if (auto* current = window->currentMapViewBase())
  {
    result.currentView = mapViewId(*current);
  }
  if (!isOnePane() && window->currentViewMaximized())
  {
    const auto views = mapViews(*window);
    const auto shown = std::ranges::count_if(
      views, [&](const auto* mapView) { return isShown(*mapView, *window); });
    const auto it = std::ranges::find_if(
      views, [&](const auto* mapView) { return isShown(*mapView, *window); });
    if (shown == 1 && it != views.end())
    {
      result.maximizedView = mapViewId(**it);
    }
  }
  return result;
}

Result<void> McpViewHost::setMaximizedView(
  MapDocument& document, const std::optional<std::string>& viewId)
{
  auto* window = mapWindow(document);
  if (!window)
  {
    return Error{"The document has no window"};
  }

  if (!viewId)
  {
    if (window->currentViewMaximized())
    {
      window->toggleMaximizeCurrentView();
    }
    return kdl::void_success;
  }

  if (isOnePane())
  {
    return Error{"The one-pane layout cannot maximize a view"};
  }
  auto* target = findMapView(*window, *viewId);
  if (!target)
  {
    return Error{fmt::format("The document's window has no view '{}'", *viewId)};
  }

  if (window->currentViewMaximized())
  {
    if (layout(document).value().maximizedView == viewId)
    {
      return kdl::void_success;
    }
    window->toggleMaximizeCurrentView();
  }

  cycleIntoView(*target);

  // MultiPaneMapView maximizes the current view, which MapViewActivationTracker sets
  // when a view gets the focus; the focus only arrives while the window is active
  for (auto* mapView : mapViews(*window))
  {
    mapView->setIsCurrent(mapView == target);
  }
  target->setFocus();
  window->toggleMaximizeCurrentView();

  if (!window->currentViewMaximized())
  {
    return Error{fmt::format("Failed to maximize the view '{}'", *viewId)};
  }
  return kdl::void_success;
}

void McpViewHost::prepareForLayoutChange()
{
  for (auto* widget = QApplication::focusWidget(); widget;
       widget = widget->parentWidget())
  {
    if (qobject_cast<MapViewBase*>(widget))
    {
      QApplication::focusWidget()->clearFocus();
      return;
    }
  }
}

} // namespace tb::ui
