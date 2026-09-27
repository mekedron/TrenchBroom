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

#include "base/PreferenceManager.h"
#include "fs/TestEnvironment.h"
#include "mcp/FakeHost.h"
#include "mcp/FakeViewHost.h"
#include "mcp/McpToolFixture.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Selection.h"
#include "mdl/PointTrace.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include "vm/vec.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

/** Restores the preferences a test changes when it goes out of scope. */
class PreferenceRestorer
{
private:
  std::vector<std::function<void()>> m_restore;

public:
  template <typename T>
  void keep(Preference<T>& preference)
  {
    m_restore.push_back(
      [&preference, value = pref(preference)]() { setPref(preference, value); });
  }

  ~PreferenceRestorer()
  {
    for (const auto& restore : m_restore)
    {
      restore();
    }
  }
};

ui::MapDocument& newDocument(McpToolFixture& fixture, const std::string& game)
{
  const auto result = fixture.call("document_new", Json{{"game", game}});
  const auto id = result["result"]["document"]["id"].get<std::string>();
  for (const auto& info : fixture.host().documentList)
  {
    if (info.id == id)
    {
      return *info.document;
    }
  }
  FAIL("document not found");
  return *fixture.host().documentList.front().document;
}

vm::vec3d vec(const Json& json)
{
  return vm::vec3d{json[0].get<double>(), json[1].get<double>(), json[2].get<double>()};
}

bool near(const vm::vec3d& lhs, const vm::vec3d& rhs)
{
  return vm::length(lhs - rhs) < 1e-4;
}

bool hasWarning(const Json& result, const std::string& code)
{
  return std::ranges::any_of(result.value("warnings", Json::array()), [&](const auto& w) {
    return w["code"] == code;
  });
}

bool tagVisible(const Json& options, const std::string& name)
{
  for (const auto& tag : options["tags"])
  {
    if (tag["name"] == name)
    {
      return tag["visible"].get<bool>();
    }
  }
  FAIL("no tag " << name);
  return false;
}

} // namespace

TEST_CASE("ViewTools")
{
  auto fixture = McpToolFixture{};
  auto& document = fixture.create();
  auto& grid = document.map().grid();
  REQUIRE(grid.size() == 4);

  SECTION("grid_get")
  {
    const auto result = fixture.call("grid_get");
    CHECK(result["size"] == 16);
    CHECK(result["exponent"] == 4);
    CHECK(result["effectiveSize"] == 16);
    CHECK(result["visible"] == grid.visible());
    CHECK(result["snap"] == grid.snap());
    CHECK(result["angle"] == 15);

    grid.toggleSnap();
    const auto noSnap = fixture.call("grid_get");
    CHECK(noSnap["snap"] == false);
    CHECK(noSnap["size"] == 16);
    CHECK(noSnap["effectiveSize"] == 1);
  }

  SECTION("grid_set")
  {
    SECTION("size")
    {
      const auto result = fixture.call("grid_set", Json{{"size", 32}});
      CHECK(result["undoStep"].is_null());
      CHECK(result["result"]["size"] == 32);
      CHECK(result["result"]["exponent"] == 5);
      CHECK(result["result"]["previous"]["size"] == 16);
      CHECK(result["grid"] == 32);
      CHECK(grid.size() == 5);

      fixture.call("grid_set", Json{{"size", 0.125}});
      CHECK(grid.size() == -3);
    }

    SECTION("exponent, visible and snap")
    {
      const auto visible = grid.visible();
      const auto result = fixture.call(
        "grid_set", Json{{"exponent", 3}, {"visible", !visible}, {"snap", false}});
      CHECK(result["result"]["size"] == 8);
      CHECK(result["result"]["effectiveSize"] == 1);
      CHECK(grid.size() == 3);
      CHECK(grid.visible() == !visible);
      CHECK_FALSE(grid.snap());

      fixture.call("grid_set", Json{{"snap", true}});
      CHECK(grid.snap());
      CHECK(grid.size() == 3);
    }

    SECTION("dry run")
    {
      const auto result =
        fixture.call("grid_set", Json{{"size", 64}, {"snap", false}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(result["result"]["size"] == 64);
      CHECK(grid.size() == 4);
      CHECK(grid.snap());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 24}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 512}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 0}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"exponent", 9}}).code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("grid_set", Json{{"size", 16}, {"exponent", 4}}).code
        == ErrorCode::InvalidArgument);
      CHECK(fixture.callExpectingError("grid_set").code == ErrorCode::InvalidArgument);

      const auto error = fixture.callExpectingError("grid_set", Json{{"size", 24}});
      CHECK(error.hint.find("0.125") != std::string::npos);
      CHECK(grid.size() == 4);
    }
  }

  auto& views = fixture.host().view;
  auto prefs = PreferenceRestorer{};
  prefs.keep(Preferences::Link2DCameras);
  prefs.keep(Preferences::MapViewLayout);

  SECTION("unsupported in host")
  {
    fixture.host().supportsViews = false;
    for (const auto& [tool, args] : std::vector<std::pair<std::string, Json>>{
           {"camera_get", Json::object()},
           {"camera_set", Json{{"position", {0, 0, 0}}}},
           {"camera_focus", Json{{"point", {0, 0, 0}}}},
           {"camera_step_pointfile", Json::object()},
           {"view_layout_set", Json{{"panes", 2}}},
         })
    {
      CHECK(fixture.callExpectingError(tool, args).code == ErrorCode::UnsupportedInHost);
    }
    // view options are preferences and document state, which need no views
    CHECK(fixture.call("view_options_get").contains("faceMode"));
  }

  SECTION("no window")
  {
    views.hasWindow = false;
    const auto error = fixture.callExpectingError("camera_get");
    CHECK(error.code == ErrorCode::OperationFailed);
    CHECK(
      fixture.callExpectingError("camera_set", Json{{"position", {0, 0, 0}}}).code
      == ErrorCode::OperationFailed);
  }

  SECTION("camera_get")
  {
    const auto result = fixture.call("camera_get");
    REQUIRE(result["views"].size() == 4);
    CHECK(result["views"][0]["id"] == "3d");
    CHECK(result["views"][0]["camera"]["projection"] == "perspective");
    CHECK(result["views"][1]["id"] == "xy");
    CHECK(result["views"][1]["camera"]["view"] == "top");
    CHECK(result["views"][1]["camera"]["zoom"] == 1);
    CHECK(result["views"][0]["width"] == 800);
    CHECK(result["layout"]["panes"] == pref(Preferences::MapViewLayout) + 1);
    CHECK(result["layout"]["maximizedView"].is_null());
    CHECK(result["layout"]["currentView"] == "3d");
    CHECK(result["link2dCameras"] == pref(Preferences::Link2DCameras));
    CHECK(!result.contains("pointFile"));
  }

  SECTION("camera_set")
  {
    const auto before = views.view("3d").camera;

    SECTION("3D position and lookAt")
    {
      const auto result = fixture.call(
        "camera_set", Json{{"position", {0, -512, 0}}, {"lookAt", {0, 0, 0}}});
      CHECK(result["undoStep"].is_null());
      const auto& camera = result["result"]["camera"];
      CHECK(vec(camera["position"]) == vm::vec3d{0, -512, 0});
      CHECK(vec(camera["direction"]) == vm::vec3d{0, 1, 0});
      CHECK(vec(camera["up"]) == vm::vec3d{0, 0, 1});
      CHECK(camera["yaw"] == 90);
      CHECK(camera["pitch"] == 0);
      CHECK(vec(result["result"]["previous"]["position"]) == before.position);
      CHECK(result["result"]["linkedViews"].empty());
      REQUIRE(views.setCameraCalls.size() == 1);
      CHECK(views.setCameraCalls[0].first == "3d");
      CHECK(views.view("3d").camera.position == vm::vec3d{0, -512, 0});
    }

    SECTION("yaw keeps pitch and position")
    {
      const auto previous = fixture.call("camera_get")["views"][0]["camera"];
      const auto result = fixture.call("camera_set", Json{{"yaw", 90}});
      const auto& camera = result["result"]["camera"];
      CHECK(camera["yaw"] == 90);
      CHECK(
        camera["pitch"].get<double>()
        == Catch::Approx(previous["pitch"].get<double>()).margin(1e-4));
      CHECK(camera["position"] == previous["position"]);
      CHECK(camera["up"][2].get<double>() > 0.0);
    }

    SECTION("direction, up and fov")
    {
      const auto result = fixture.call(
        "camera_set", Json{{"direction", {0, 0, -2}}, {"up", {0, 1, 0}}, {"fov", 60}});
      const auto& camera = result["result"]["camera"];
      CHECK(vec(camera["direction"]) == vm::vec3d{0, 0, -1});
      CHECK(vec(camera["up"]) == vm::vec3d{0, 1, 0});
      CHECK(camera["fov"] == 60);
    }

    SECTION("2D view with linked cameras")
    {
      setPref(Preferences::Link2DCameras, true);
      const auto result = fixture.call(
        "camera_set", Json{{"view", "xy"}, {"position", {128, 64, 999}}, {"zoom", 2}});
      const auto& camera = result["result"]["camera"];
      CHECK(camera["position"][0] == 128);
      CHECK(camera["position"][1] == 64);
      CHECK(camera["position"][2] == result["result"]["previous"]["position"][2]);
      CHECK(camera["zoom"] == 2);
      const auto& linked = result["result"]["linkedViews"];
      REQUIRE(linked.size() == 2);
      CHECK(linked[0]["id"] == "xz");
      CHECK(linked[0]["camera"]["zoom"] == 2);
      CHECK(linked[0]["camera"]["position"][0] == 128);
      CHECK(views.view("3d").camera == before);
    }

    SECTION("2D view without linked cameras")
    {
      setPref(Preferences::Link2DCameras, false);
      const auto result = fixture.call("camera_set", Json{{"view", "yz"}, {"zoom", 0.5}});
      CHECK(result["result"]["camera"]["zoom"] == 0.5);
      CHECK(result["result"]["linkedViews"].empty());
      CHECK(views.view("xy").camera.zoom == 1.0);
    }

    SECTION("dry run")
    {
      const auto result =
        fixture.call("camera_set", Json{{"position", {1, 2, 3}}, {"dryRun", true}});
      CHECK(result["dryRun"] == true);
      CHECK(vec(result["result"]["camera"]["position"]) == vm::vec3d{1, 2, 3});
      CHECK(views.setCameraCalls.empty());
      CHECK(views.view("3d").camera == before);
    }

    SECTION("invalid input")
    {
      const auto invalid = [&](const Json& args) {
        return fixture.callExpectingError("camera_set", args).code
               == ErrorCode::InvalidArgument;
      };
      CHECK(invalid(Json::object()));
      CHECK(invalid(Json{{"view", "zz"}, {"zoom", 1}}));
      CHECK(invalid(Json{{"direction", {1, 0, 0}}, {"lookAt", {0, 0, 0}}}));
      CHECK(invalid(Json{{"yaw", 10}, {"direction", {1, 0, 0}}}));
      CHECK(invalid(Json{{"direction", {0, 0, 0}}}));
      CHECK(invalid(Json{{"direction", {1, 0, 0}}, {"up", {2, 0, 0}}}));
      CHECK(invalid(Json{{"lookAt", {-256, -256, 256}}}));
      CHECK(invalid(Json{{"zoom", 2}}));
      CHECK(invalid(Json{{"view", "xy"}, {"fov", 60}}));
      CHECK(invalid(Json{{"view", "xz"}, {"yaw", 60}}));
      CHECK(invalid(Json{{"view", "xy"}, {"zoom", 1000}}));
      CHECK(views.setCameraCalls.empty());

      views.viewList.pop_back();
      const auto error =
        fixture.callExpectingError("camera_set", Json{{"view", "yz"}, {"zoom", 2}});
      CHECK(error.code == ErrorCode::InvalidArgument);
      CHECK(error.hint.find("xz") != std::string::npos);
    }
  }

  SECTION("camera_focus")
  {
    setPref(Preferences::Link2DCameras, false);
    const auto brushId =
      fixture
        .call(
          "brush_create_box",
          Json{{"min", {512, 0, 0}}, {"max", {576, 64, 64}}})["result"]["brush"]
        .get<std::string>();
    auto& map = document.map();
    mdl::deselectAll(map);
    const auto before = views.view("3d").camera;
    const auto center = vm::vec3d{544, 32, 32};

    const auto checkFramed = [&](const Json& camera) {
      CHECK(near(vec(camera["direction"]), before.direction));
      const auto toCenter = vm::normalize(center - vec(camera["position"]));
      CHECK(vm::dot(toCenter, before.direction) == Catch::Approx(1.0).margin(1e-3));
    };

    SECTION("explicit ids")
    {
      const auto result = fixture.call("camera_focus", Json{{"ids", {brushId}}});
      CHECK(vec(result["result"]["box"]["min"]) == vm::vec3d{512, 0, 0});
      const auto& cameras = result["result"]["views"];
      REQUIRE(cameras.size() == 4);
      CHECK(cameras[0]["id"] == "3d");
      checkFramed(cameras[0]["camera"]);
      CHECK(vec(cameras[0]["previous"]["position"]) == before.position);
      // the 2D views center on the box and keep their zoom
      CHECK(cameras[1]["camera"]["position"][0] == 544);
      CHECK(cameras[1]["camera"]["position"][1] == 32);
      CHECK(cameras[1]["camera"]["zoom"] == 1);
      CHECK(map.selection().nodes.empty());
      CHECK(views.setCameraCalls.size() == 4);
    }

    SECTION("selection")
    {
      mdl::selectNodes(map, {fixture.node(brushId)});
      const auto result =
        fixture.call("camera_focus", Json{{"views", {"3d"}}, {"margin", 1.5}});
      REQUIRE(result["result"]["views"].size() == 1);
      checkFramed(result["result"]["views"][0]["camera"]);
      CHECK(views.setCameraCalls.size() == 1);
      CHECK(map.selection().nodes.size() == 1);
    }

    SECTION("a box that does not fit zooms the 2D views out")
    {
      const auto result = fixture.call(
        "camera_focus",
        Json{{"box", {{"min", {-2048, -2048, -2048}}, {"max", {2048, 2048, 2048}}}}});
      const auto& cameras = result["result"]["views"];
      CHECK(cameras[1]["camera"]["zoom"].get<double>() < 0.1);
      CHECK(cameras[1]["camera"]["position"][0] == 0);
    }

    SECTION("point")
    {
      const auto result = fixture.call(
        "camera_focus", Json{{"point", {100, 100, 100}}, {"views", {"xy", "xz"}}});
      CHECK(vec(result["result"]["box"]["min"]) == vm::vec3d{36, 36, 36});
      CHECK(result["result"]["views"].size() == 2);
    }

    SECTION("dry run")
    {
      const auto result =
        fixture.call("camera_focus", Json{{"ids", {brushId}}, {"dryRun", true}});
      checkFramed(result["result"]["views"][0]["camera"]);
      CHECK(views.setCameraCalls.empty());
      CHECK(views.view("3d").camera == before);
    }

    SECTION("invalid input")
    {
      CHECK(fixture.callExpectingError("camera_focus").code == ErrorCode::NoSelection);
      CHECK(
        fixture
          .callExpectingError(
            "camera_focus", Json{{"ids", {brushId}}, {"point", {0, 0, 0}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("camera_focus", Json{{"ids", {"brush:999999"}}}).code
        == ErrorCode::ObjectNotFound);
      CHECK(
        fixture.callExpectingError("camera_focus", Json{{"views", {"zz"}}}).code
        == ErrorCode::InvalidArgument);
      CHECK(views.setCameraCalls.empty());
    }
  }

  SECTION("camera_step_pointfile")
  {
    const auto noPointFile = fixture.callExpectingError("camera_step_pointfile");
    CHECK(noPointFile.code == ErrorCode::OperationFailed);
    CHECK(noPointFile.hint.find("pointfile_load") != std::string::npos);

    auto env = fs::TestEnvironment{};
    env.createFile("leak.pts", "0 0 0\n256 0 0\n256 256 0\n256 256 256\n");
    document.loadPointFile(env.dir() / "leak.pts");
    REQUIRE(document.pointTrace());

    // the editor subdivides the path
    const auto& points = document.pointTrace()->points();
    const auto count = points.size();
    REQUIRE(count >= 4);
    const auto second = vm::vec3d{points[1]};
    const auto along = vm::normalize(vm::vec3d{points[2]} - second);

    const auto next = fixture.call("camera_step_pointfile")["result"];
    CHECK(next["index"] == 1);
    CHECK(next["count"] == count);
    CHECK(next["moved"] == true);
    CHECK(near(vec(next["point"]), second));
    CHECK(near(vec(next["direction"]), along));
    CHECK(near(vec(next["camera"]["position"]), second + vm::vec3d{0, 0, 16}));
    CHECK(near(vec(next["camera"]["direction"]), along));
    CHECK(views.view("xy").camera.position.x() == Catch::Approx(second.x()));
    CHECK(fixture.call("camera_get")["pointFile"]["index"] == 1);

    const auto dryRun = fixture.call(
      "camera_step_pointfile", Json{{"direction", "last"}, {"dryRun", true}});
    CHECK(dryRun["result"]["index"] == count - 1);
    CHECK(vm::vec3d{document.pointTrace()->currentPoint()} == second);

    const auto last = fixture.call("camera_step_pointfile", Json{{"direction", "last"}});
    CHECK(last["result"]["index"] == count - 1);
    CHECK(near(vec(last["result"]["point"]), vm::vec3d{256, 256, 256}));
    CHECK(last["result"]["hasNext"] == false);

    const auto end = fixture.call("camera_step_pointfile");
    CHECK(end["result"]["moved"] == false);
    CHECK(hasWarning(end, "END_OF_TRACE"));

    fixture.call("camera_step_pointfile", Json{{"direction", "first"}});
    const auto previous =
      fixture.call("camera_step_pointfile", Json{{"direction", "previous"}});
    CHECK(previous["result"]["index"] == 0);
    CHECK(hasWarning(previous, "END_OF_TRACE"));

    CHECK(
      fixture.callExpectingError("camera_step_pointfile", Json{{"direction", "up"}}).code
      == ErrorCode::InvalidArgument);
  }

  SECTION("view_layout_set")
  {
    setPref(Preferences::MapViewLayout, 0);

    SECTION("panes and maximized view")
    {
      const auto result = fixture.call("view_layout_set", Json{{"panes", 4}});
      CHECK(result["result"]["panes"] == 4);
      CHECK(result["result"]["viewsRecreated"] == true);
      CHECK(result["result"]["previous"]["panes"] == 1);
      CHECK(pref(Preferences::MapViewLayout) == 3);
      CHECK(views.setMaximizedViewCalls.empty());
      // the focus leaves the views before the editor recreates them
      CHECK(views.prepareForLayoutChangeCount == 1);

      const auto maximized = fixture.call("view_layout_set", Json{{"maximized", "xz"}});
      CHECK(maximized["result"]["maximizedView"] == "xz");
      CHECK(maximized["result"]["currentView"] == "xz");
      CHECK(maximized["result"]["viewsRecreated"] == false);
      CHECK(views.setMaximizedViewCalls == std::vector<std::optional<std::string>>{"xz"});
      CHECK(views.prepareForLayoutChangeCount == 1);
      CHECK(fixture.call("camera_get")["layout"]["maximizedView"] == "xz");

      const auto restored = fixture.call("view_layout_set", Json{{"maximized", "none"}});
      CHECK(restored["result"]["maximizedView"].is_null());
      CHECK(restored["result"]["previous"]["maximizedView"] == "xz");
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "view_layout_set", Json{{"panes", 2}, {"maximized", "3d"}, {"dryRun", true}});
      CHECK(result["result"]["panes"] == 2);
      CHECK(result["result"]["maximizedView"] == "3d");
      CHECK(pref(Preferences::MapViewLayout) == 0);
      CHECK(views.setMaximizedViewCalls.empty());
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("view_layout_set").code == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_layout_set", Json{{"panes", 5}}).code
        == ErrorCode::InvalidArgument);
      const auto onePane =
        fixture.callExpectingError("view_layout_set", Json{{"maximized", "3d"}});
      CHECK(onePane.code == ErrorCode::InvalidArgument);
      CHECK(onePane.hint.find("panes") != std::string::npos);
      CHECK(pref(Preferences::MapViewLayout) == 0);
      CHECK(views.setMaximizedViewCalls.empty());
    }
  }
}

TEST_CASE("ViewTools view options")
{
  auto fixture = McpToolFixture{};
  auto& document = newDocument(fixture, "Quake");
  auto& map = document.map();
  auto& editorContext = map.editorContext();
  REQUIRE(!map.tagManager().smartTags().empty());
  const auto* light = map.entityDefinitionManager().definition("light");
  const auto* army = map.entityDefinitionManager().definition("monster_army");
  REQUIRE(light);
  REQUIRE(army);

  auto prefs = PreferenceRestorer{};
  prefs.keep(Preferences::FaceRenderMode);
  prefs.keep(Preferences::ShowFog);
  prefs.keep(Preferences::ShowEdges);
  prefs.keep(Preferences::EntityLinkMode);
  prefs.keep(Preferences::ShowPointEntityModels);
  setPref(Preferences::FaceRenderMode, Preferences::FaceRenderModeTextured);
  setPref(Preferences::ShowFog, false);
  setPref(Preferences::EntityLinkMode, Preferences::EntityLinkModeDirect);

  SECTION("view_options_get")
  {
    const auto result = fixture.call("view_options_get");
    CHECK(result["faceMode"] == "textured");
    CHECK(result["fog"] == false);
    CHECK(result["edges"] == pref(Preferences::ShowEdges));
    CHECK(result["entityLinkMode"] == "direct");
    CHECK(tagVisible(result, "Trigger"));
    CHECK(result["hiddenClassnames"].empty());
    CHECK(!result["classGroups"].empty());
  }

  SECTION("view_options_set")
  {
    const auto triggerType = [&]() {
      for (const auto& tag : map.tagManager().smartTags())
      {
        if (tag.name() == "Trigger")
        {
          return tag.type();
        }
      }
      FAIL("no trigger tag");
      return mdl::TagType::Type{0};
    }();

    SECTION("preferences, tags and classes")
    {
      const auto result = fixture.call(
        "view_options_set",
        Json{
          {"faceMode", "flat"},
          {"fog", true},
          {"entityLinkMode", "all"},
          {"hideTags", {"trigger"}},
          {"hideClassnames", {"LIGHT"}},
        })["result"];
      CHECK(result["faceMode"] == "flat");
      CHECK(result["fog"] == true);
      CHECK(!tagVisible(result, "Trigger"));
      CHECK(result["hiddenClassnames"] == Json::array({"light"}));
      CHECK(result["previous"]["faceMode"] == "textured");
      CHECK(tagVisible(result["previous"], "Trigger"));
      CHECK(
        result["changed"]
        == Json::array(
          {"faceMode", "entityLinkMode", "fog", "tags", "hiddenClassnames"}));

      CHECK(pref(Preferences::FaceRenderMode) == "flat");
      CHECK(pref(Preferences::ShowFog));
      CHECK(pref(Preferences::EntityLinkMode) == "all");
      CHECK((editorContext.hiddenTags() & triggerType) != 0);
      CHECK(editorContext.entityDefinitionHidden(*light));
      CHECK(!editorContext.entityDefinitionHidden(*army));

      const auto shown = fixture.call(
        "view_options_set", Json{{"showTags", {"TRIGGER"}}, {"showClassnames", {"*"}}});
      CHECK(tagVisible(shown["result"], "Trigger"));
      CHECK(shown["result"]["hiddenClassnames"].empty());
      CHECK(editorContext.hiddenTags() == 0);
      CHECK(!editorContext.entityDefinitionHidden(*light));
    }

    SECTION("globs and groups")
    {
      const auto result = fixture.call(
        "view_options_set", Json{{"hideClassnames", {"monster", "light_*"}}});
      CHECK(editorContext.entityDefinitionHidden(*army));
      CHECK(!editorContext.entityDefinitionHidden(*light));
      const auto& hidden = result["result"]["hiddenClassnames"];
      CHECK(std::ranges::find(hidden, Json("monster_army")) != hidden.end());
      CHECK(std::ranges::find(hidden, Json("light_flame_large_yellow")) != hidden.end());
      CHECK(std::ranges::find(hidden, Json("light")) == hidden.end());
    }

    SECTION("restore defaults")
    {
      setPref(Preferences::ShowEdges, false);
      const auto result =
        fixture.call("view_options_set", Json{{"restoreDefaults", true}, {"fog", true}});
      CHECK(result["result"]["edges"] == true);
      CHECK(result["result"]["fog"] == true);
      CHECK(pref(Preferences::ShowEdges));
    }

    SECTION("dry run")
    {
      const auto result = fixture.call(
        "view_options_set",
        Json{{"faceMode", "skip"}, {"hideTags", {"trigger"}}, {"dryRun", true}});
      CHECK(result["result"]["faceMode"] == "skip");
      CHECK(pref(Preferences::FaceRenderMode) == "textured");
      CHECK(editorContext.hiddenTags() == 0);
    }

    SECTION("invalid input")
    {
      CHECK(
        fixture.callExpectingError("view_options_set").code
        == ErrorCode::InvalidArgument);

      const auto unknownTag =
        fixture.callExpectingError("view_options_set", Json{{"hideTags", {"nonsense"}}});
      CHECK(unknownTag.code == ErrorCode::InvalidArgument);
      CHECK(unknownTag.hint.find("Trigger") != std::string::npos);

      const auto unknownClass = fixture.callExpectingError(
        "view_options_set", Json{{"hideClassnames", {"nonsense_*"}}});
      CHECK(unknownClass.code == ErrorCode::InvalidArgument);
      CHECK(unknownClass.hint.find("Monster") != std::string::npos);

      CHECK(
        fixture
          .callExpectingError(
            "view_options_set",
            Json{{"showTags", {"trigger"}}, {"hideTags", {"Trigger"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture
          .callExpectingError(
            "view_options_set",
            Json{{"showClassnames", {"light"}}, {"hideClassnames", {"light*"}}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(
        fixture.callExpectingError("view_options_set", Json{{"faceMode", "wireframe"}})
          .code
        == ErrorCode::InvalidArgument);
      CHECK(editorContext.hiddenTags() == 0);
      CHECK(pref(Preferences::FaceRenderMode) == "textured");
    }
  }
}

} // namespace tb::mcp
