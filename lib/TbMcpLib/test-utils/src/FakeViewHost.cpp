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


#include "mcp/FakeViewHost.h"

#include "base/PreferenceManager.h"
#include "prefs/Preferences.h"

#include "kd/contracts.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>

namespace tb::mcp
{

FakeViewHost::FakeViewHost()
{
  const auto bounds = vm::bbox3d{8192.0};
  auto perspective = perspectiveCamera(vm::vec3d{-256, -256, 256}, vm::vec3d{1, 1, -1});
  perspective.up = upVector(perspective.direction);
  viewList = {
    UserView{"3d", true, 800, 600, perspective},
    UserView{
      "xy", true, 400, 300, orthographicCamera(OrthoView::Top, {0, 0, 0}, 1.0, bounds)},
    UserView{
      "xz", true, 400, 300, orthographicCamera(OrthoView::Front, {0, 0, 0}, 1.0, bounds)},
    UserView{
      "yz", true, 400, 300, orthographicCamera(OrthoView::Side, {0, 0, 0}, 1.0, bounds)},
  };
  currentLayout.currentView = "3d";
}

std::vector<UserView> FakeViewHost::views(ui::MapDocument&)
{
  return hasWindow ? viewList : std::vector<UserView>{};
}

UserView& FakeViewHost::view(const std::string& viewId)
{
  const auto it =
    std::ranges::find_if(viewList, [&](const auto& v) { return v.id == viewId; });
  contract_assert(it != viewList.end());
  return *it;
}

Result<void> FakeViewHost::setCamera(
  ui::MapDocument&, const std::string& viewId, const AgentCamera& camera)
{
  setCameraCalls.emplace_back(viewId, camera);
  if (!hasWindow)
  {
    return Error{"The document has no window"};
  }
  if (std::ranges::none_of(viewList, [&](const auto& v) { return v.id == viewId; }))
  {
    return Error{"no such view: " + viewId};
  }

  auto& target = view(viewId);
  if (target.camera.projection == CameraProjection::Perspective)
  {
    target.camera.position = camera.position;
    target.camera.direction = vm::normalize(camera.direction);
    const auto up =
      camera.up - vm::dot(camera.up, target.camera.direction) * target.camera.direction;
    target.camera.up = vm::normalize(up);
    target.camera.fov = camera.fov;
    return kdl::void_success;
  }

  if (camera.zoom < 0.02 || camera.zoom > 100.0)
  {
    return Error{"invalid zoom"};
  }
  const auto& axis = target.camera.direction;
  target.camera.position = camera.position - vm::dot(camera.position, axis) * axis
                           + vm::dot(target.camera.position, axis) * axis;
  target.camera.zoom = camera.zoom;

  // like CameraLinkHelper::updateCameras
  if (pref(Preferences::Link2DCameras))
  {
    for (auto& other : viewList)
    {
      if (other.id != viewId && other.camera.projection == CameraProjection::Orthographic)
      {
        other.camera.zoom = camera.zoom;
        const auto factors = vm::vec3d{1, 1, 1} - vm::abs(target.camera.direction)
                             - vm::abs(other.camera.direction);
        other.camera.position = (vm::vec3d{1, 1, 1} - factors) * other.camera.position
                                + factors * target.camera.position;
      }
    }
  }
  return kdl::void_success;
}

Result<ViewLayout> FakeViewHost::layout(ui::MapDocument&)
{
  if (!hasWindow)
  {
    return Error{"The document has no window"};
  }
  return currentLayout;
}

Result<void> FakeViewHost::setMaximizedView(
  ui::MapDocument&, const std::optional<std::string>& viewId)
{
  setMaximizedViewCalls.push_back(viewId);
  if (!hasWindow)
  {
    return Error{"The document has no window"};
  }
  if (pref(Preferences::MapViewLayout) == 0)
  {
    return Error{"The one-pane layout cannot maximize a view"};
  }
  if (viewId && std::ranges::none_of(viewList, [&](const auto& v) {
        return v.id == *viewId;
      }))
  {
    return Error{"no such view: " + *viewId};
  }

  currentLayout.maximizedView = viewId;
  if (viewId)
  {
    currentLayout.currentView = viewId;
  }
  for (auto& v : viewList)
  {
    v.visible = !viewId || v.id == *viewId;
  }
  return kdl::void_success;
}

void FakeViewHost::prepareForLayoutChange()
{
  ++prepareForLayoutChangeCount;
}

} // namespace tb::mcp
