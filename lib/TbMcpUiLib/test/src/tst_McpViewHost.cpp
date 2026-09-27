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

#include "McpUiTestUtils.h"
#include "base/PreferenceManager.h"
#include "mcp/AgentCamera.h"
#include "mcp/Snapshot.h"
#include "prefs/Preferences.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/McpViewHost.h"
#include "ui/QtMcpHost.h"

#include "vm/vec.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const mcp::UserView& findView(
  const std::vector<mcp::UserView>& views, const std::string& id)
{
  const auto it =
    std::ranges::find_if(views, [&](const auto& view) { return view.id == id; });
  REQUIRE(it != views.end());
  return *it;
}

std::vector<std::string> visibleViews(mcp::ViewHost& viewHost, MapDocument& document)
{
  auto result = std::vector<std::string>{};
  for (const auto& view : viewHost.views(document))
  {
    if (view.visible)
    {
      result.push_back(view.id);
    }
  }
  return result;
}

bool near(const vm::vec3d& lhs, const vm::vec3d& rhs)
{
  return vm::length(lhs - rhs) < 1e-3;
}

} // namespace

// The window is not shown because the offscreen platform has no OpenGL, so the views are
// not rendered and never get the keyboard focus; `visible` means shown in the layout.
TEST_CASE("McpViewHost")
{
  QStandardPaths::setTestModeEnabled(true);

  const auto layout = pref(Preferences::MapViewLayout);
  const auto link = pref(Preferences::Link2DCameras);
  setPref(Preferences::MapViewLayout, 3);
  setPref(Preferences::Link2DCameras, true);

  {
    auto appControllerFixture = AppControllerFixture{};
    auto& appController = appControllerFixture.appController();
    auto& mapWindow = createMapWindow(appController);
    auto& document = mapWindow.document();
    auto host = QtMcpHost{appController};
    auto* viewHost = host.viewHost();
    REQUIRE(viewHost != nullptr);

    SECTION("views")
    {
      const auto views = viewHost->views(document);
      REQUIRE(views.size() == 4);
      CHECK(views[0].id == "3d");
      CHECK(views[1].id == "xy");
      CHECK(views[2].id == "xz");
      CHECK(views[3].id == "yz");
      CHECK(views[0].camera.projection == mcp::CameraProjection::Perspective);
      CHECK(views[1].camera.projection == mcp::CameraProjection::Orthographic);
      CHECK(mcp::orthoViewOf(views[2].camera) == mcp::OrthoView::Front);
      CHECK(
        visibleViews(*viewHost, document)
        == std::vector<std::string>{"3d", "xy", "xz", "yz"});

      const auto viewLayout = viewHost->layout(document);
      REQUIRE(viewLayout.is_success());
      CHECK(viewLayout.value().maximizedView == std::nullopt);
      CHECK(viewLayout.value().currentView.has_value());
    }

    SECTION("setCamera of the 3D view")
    {
      auto camera = mcp::lookAtCamera({0, -512, 128}, {0, 0, 0});
      camera.fov = findView(viewHost->views(document), "3d").camera.fov;
      REQUIRE(viewHost->setCamera(document, "3d", camera).is_success());

      const auto& result = findView(viewHost->views(document), "3d").camera;
      CHECK(near(result.position, camera.position));
      CHECK(near(result.direction, camera.direction));
      CHECK(near(result.up, camera.up));
      CHECK(result.fov == Catch::Approx(camera.fov));

      camera.fov = 60.0;
      REQUIRE(viewHost->setCamera(document, "3d", camera).is_success());
      CHECK(
        findView(viewHost->views(document), "3d").camera.fov
        == Catch::Approx(60.0).margin(1e-3));
    }

    SECTION("setCamera of a 2D view moves the linked views")
    {
      auto camera = findView(viewHost->views(document), "xy").camera;
      const auto oldZ = camera.position.z();
      camera.position = {256, 128, 9999};
      camera.zoom = 2.0;
      REQUIRE(viewHost->setCamera(document, "xy", camera).is_success());

      const auto views = viewHost->views(document);
      const auto& xy = findView(views, "xy").camera;
      CHECK(xy.position.x() == Catch::Approx(256));
      CHECK(xy.position.y() == Catch::Approx(128));
      CHECK(xy.position.z() == Catch::Approx(oldZ));
      CHECK(xy.zoom == Catch::Approx(2.0));
      CHECK(findView(views, "xz").camera.zoom == Catch::Approx(2.0));
      CHECK(findView(views, "xz").camera.position.x() == Catch::Approx(256));
      CHECK(findView(views, "yz").camera.position.y() == Catch::Approx(128));
    }

    SECTION("setCamera errors")
    {
      auto camera = findView(viewHost->views(document), "xy").camera;
      camera.zoom = 1000.0;
      CHECK(viewHost->setCamera(document, "xy", camera).is_error());
      CHECK(viewHost->setCamera(document, "zz", camera).is_error());
      CHECK(viewHost->setCamera(document, "3d", camera).is_error());
      CHECK(
        viewHost->setCamera(document, "xy", mcp::perspectiveCamera({0, 0, 0}, {1, 0, 0}))
          .is_error());
    }

    SECTION("setMaximizedView in the four-pane layout")
    {
      REQUIRE(viewHost->setMaximizedView(document, "xz").is_success());
      CHECK(mapWindow.currentViewMaximized());
      CHECK(viewHost->layout(document).value().maximizedView == "xz");
      CHECK(visibleViews(*viewHost, document) == std::vector<std::string>{"xz"});

      // maximizing another view restores the views first
      REQUIRE(viewHost->setMaximizedView(document, "3d").is_success());
      CHECK(viewHost->layout(document).value().maximizedView == "3d");
      CHECK(visibleViews(*viewHost, document) == std::vector<std::string>{"3d"});

      REQUIRE(viewHost->setMaximizedView(document, "3d").is_success());
      CHECK(viewHost->layout(document).value().maximizedView == "3d");

      REQUIRE(viewHost->setMaximizedView(document, std::nullopt).is_success());
      CHECK(!mapWindow.currentViewMaximized());
      CHECK(viewHost->layout(document).value().maximizedView == std::nullopt);
      CHECK(visibleViews(*viewHost, document).size() == 4);

      CHECK(viewHost->setMaximizedView(document, "zz").is_error());
    }

    SECTION("setMaximizedView cycles the 2D pane of the two-pane layout")
    {
      setPref(Preferences::MapViewLayout, 1);
      CHECK(visibleViews(*viewHost, document) == std::vector<std::string>{"3d", "xy"});

      REQUIRE(viewHost->setMaximizedView(document, "yz").is_success());
      CHECK(viewHost->layout(document).value().maximizedView == "yz");
      CHECK(visibleViews(*viewHost, document) == std::vector<std::string>{"yz"});

      REQUIRE(viewHost->setMaximizedView(document, std::nullopt).is_success());
      CHECK(visibleViews(*viewHost, document) == std::vector<std::string>{"3d", "yz"});
    }

    SECTION("switching the layout recreates the views")
    {
      REQUIRE(viewHost->setMaximizedView(document, "xy").is_success());
      setPref(Preferences::MapViewLayout, 2);
      CHECK(viewHost->views(document).size() == 4);
      CHECK(viewHost->layout(document).value().maximizedView == std::nullopt);
      CHECK(visibleViews(*viewHost, document).size() == 3);

      setPref(Preferences::MapViewLayout, 0);
      CHECK(visibleViews(*viewHost, document) == std::vector<std::string>{"3d"});
      CHECK(viewHost->setMaximizedView(document, "3d").is_error());
      CHECK(viewHost->setMaximizedView(document, std::nullopt).is_success());
    }

    SECTION("no window")
    {
      auto noWindow = McpViewHost{[](const MapDocument&) { return nullptr; }};
      CHECK(noWindow.views(document).empty());
      CHECK(noWindow.layout(document).is_error());
      CHECK(noWindow.setCamera(document, "3d", mcp::AgentCamera{}).is_error());
      CHECK(noWindow.setMaximizedView(document, "3d").is_error());
    }

    closeAllMapWindows(appController);
    processDeferredDeletes();
  }

  setPref(Preferences::MapViewLayout, layout);
  setPref(Preferences::Link2DCameras, link);
  QStandardPaths::setTestModeEnabled(false);
}

} // namespace tb::ui
