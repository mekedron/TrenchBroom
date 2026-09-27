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

#include <QCoreApplication>
#include <QStandardPaths>
#include <QtTest/QTest>

#include "McpUiTestUtils.h"
#include "TestEnvironment.h"
#include "base/PreferenceManager.h"
#include "fs/DiskIO.h"
#include "gl/GlManager.h"
#include "mcp/McpServer.h"
#include "mcp/McpToolFixture.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/ActionCatalog.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "prefs/Preferences.h"
#include "ui/Action.h"
#include "ui/ActionManager.h"
#include "ui/AppController.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/McpActionHost.h"
#include "ui/QtMcpHost.h"

#include "kd/result.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

struct TestModeStandardPaths
{
  TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(true); }
  ~TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(false); }
};

/** The real Quake configuration, with its smart tags and builtin entity definitions. */
mdl::GameInfo quakeGameInfo()
{
  const auto configPath = getFixtureRoot() / "games" / "Quake" / "GameConfig.cfg";
  return fs::Disk::withInputStream(
           configPath,
           [](auto& stream) {
             return std::string{std::istreambuf_iterator<char>{stream}, {}};
           })
         | kdl::and_then(
           [&](const auto& str) { return mdl::parseGameConfig(str, configPath); })
         | kdl::transform([](auto gameConfig) {
             return mdl::detail::makeGameInfoFixture(
               std::move(gameConfig),
               getFixtureRoot() / "test" / "mdl" / "Game" / "Quake");
           })
         | kdl::value();
}

/** Unregisters the document from the fake host before the host is destroyed. */
struct RegisteredDocument
{
  mcp::FakeHost& host;
  MapDocument& document;

  ~RegisteredDocument() { host.removeDocument(document); }
};

/**
 * Every action in the registry for the given document: the action manager's actions and
 * the document's tag and entity definition actions, as the map views create them.
 */
std::vector<std::string> registryPaths(
  const ActionManager& actionManager, const MapDocument& document)
{
  auto result = std::vector<std::string>{};
  for (const auto& [path, action] : actionManager.actionsMap())
  {
    result.push_back(path.generic_string());
  }

  const auto& map = document.map();
  for (const auto& action : actionManager.createTagActions(map.tagManager().smartTags()))
  {
    result.push_back(action.preference().path.generic_string());
  }
  for (const auto& action : actionManager.createEntityDefinitionActions(
         map.entityDefinitionManager().definitions()))
  {
    result.push_back(action.preference().path.generic_string());
  }
  return result;
}

const mcp::EditorAction* findAction(
  const std::vector<mcp::EditorAction>& actions, const std::string& path)
{
  const auto it = std::ranges::find(actions, path, &mcp::EditorAction::path);
  return it != actions.end() ? &*it : nullptr;
}

} // namespace

TEST_CASE("McpActionHost")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  const auto gameInfo = quakeGameInfo();
  auto& mapWindow = createMapWindow(appController, gameInfo);
  auto& document = mapWindow.document();
  auto& map = document.map();

  auto qtHost = QtMcpHost{appController};
  auto& actionHost = *qtHost.actionHost();

  REQUIRE_FALSE(map.tagManager().smartTags().empty());
  REQUIRE_FALSE(map.entityDefinitionManager().definitions().empty());

  SECTION("coverage of the action registry (E14.8)")
  {
    auto fixture = mcp::McpToolFixture{};
    const auto& tools = fixture.server().tools();

    const auto paths = registryPaths(appController.actionManager(), document);
    const auto actions = actionHost.actions(document, std::nullopt);
    REQUIRE(actions.is_success());

    // every action of the registry is listed once
    CHECK(actions.value().size() == paths.size());
    for (const auto& path : paths)
    {
      CAPTURE(path);
      CHECK(findAction(actions.value(), path) != nullptr);
    }

    auto unclassified = std::vector<std::string>{};
    auto unreachable = std::vector<std::string>{};
    auto viaTool = size_t(0);
    auto viaInvoke = size_t(0);
    for (const auto& path : paths)
    {
      const auto* actionClass = mcp::findActionClass(path);
      if (!actionClass)
      {
        unclassified.push_back(path);
        continue;
      }

      const auto hasTool = std::ranges::any_of(
        actionClass->tools, [&](const auto name) { return tools.find(name) != nullptr; });
      if (hasTool)
      {
        ++viaTool;
      }
      else if (actionClass->handling == mcp::ActionHandling::Invoke)
      {
        ++viaInvoke;
      }
      else
      {
        unreachable.push_back(path);
      }
    }

    const auto total = paths.size();
    const auto coverage = 100.0 * double(total - unreachable.size()) / double(total);
    std::cout << "Action coverage: " << total << " actions, " << viaTool
              << " through a semantic tool, " << viaInvoke
              << " only through action_invoke, " << unreachable.size() << " unreachable; "
              << coverage << "% reachable\n";
    for (const auto& path : unreachable)
    {
      std::cout << "  unreachable: " << path << "\n";
    }

    CHECK(unclassified.empty());
    for (const auto& path : unclassified)
    {
      FAIL_CHECK("Unclassified action (add it to ActionCatalog.cpp): " << path);
    }
    CHECK(coverage >= 95.0);

    // every catalog entry still exists in the registry
    for (const auto& entry : mcp::actionCatalog())
    {
#ifdef NDEBUG
      if (entry.path.starts_with("Menu/Debug/"))
      {
        continue;
      }
#endif
      CAPTURE(entry.path);
      CHECK(std::ranges::find(paths, entry.path) != paths.end());
    }

    // every tool named by the catalog exists
    for (const auto& name : mcp::actionCatalogToolNames())
    {
      CAPTURE(name);
      CHECK(tools.find(name) != nullptr);
    }
  }

  SECTION("actions")
  {
    const auto actions = actionHost.actions(document, std::nullopt);
    REQUIRE(actions.is_success());

    const auto* merge = findAction(actions.value(), "Menu/Edit/CSG/Convex Merge");
    REQUIRE(merge != nullptr);
    CHECK(merge->label == "Convex Merge");
    CHECK(merge->kind == "menu");
    CHECK(merge->menu == std::vector<std::string>{"Edit", "CSG"});
    CHECK(merge->shortcuts == std::vector<std::string>{"Ctrl+J"});
    CHECK(merge->context == "any");
    CHECK_FALSE(merge->enabled);
    CHECK_FALSE(merge->checkable);

    const auto* flip =
      findAction(actions.value(), "Controls/Map view/Flip objects horizontally");
    REQUIRE(flip != nullptr);
    CHECK(flip->kind == "menu");
    CHECK(flip->menu == std::vector<std::string>{"Edit", "Transform"});

    const auto* grid = findAction(actions.value(), "Menu/View/Grid/Show Grid");
    REQUIRE(grid != nullptr);
    CHECK(grid->checkable);
    CHECK(grid->checked == map.grid().visible());

    const auto* cancel = findAction(actions.value(), "Controls/Map view/Cancel");
    REQUIRE(cancel != nullptr);
    CHECK(cancel->kind == "view");
    CHECK(cancel->menu.empty());
    CHECK(cancel->enabled);

    const auto& tag = map.tagManager().smartTags().front();
    const auto* toggleTag =
      findAction(actions.value(), "Filters/Tags/" + tag.name() + "/Toggle Visible");
    REQUIRE(toggleTag != nullptr);
    CHECK(toggleTag->kind == "tag");

    const auto* createLight = findAction(actions.value(), "Entities/light/Create");
    REQUIRE(createLight != nullptr);
    CHECK(createLight->kind == "entity");
    CHECK(createLight->enabled);

    // brush entities need selected brushes
    const auto* createDoor = findAction(actions.value(), "Entities/func_door/Create");
    REQUIRE(createDoor != nullptr);
    CHECK_FALSE(createDoor->enabled);
  }

  SECTION("views")
  {
    const auto path = std::string{"Controls/Map view/Reset camera zoom"};
    const auto in3d = actionHost.actions(document, "3d");
    const auto inXy = actionHost.actions(document, "xy");
    REQUIRE(in3d.is_success());
    REQUIRE(inXy.is_success());
    CHECK(findAction(in3d.value(), path)->enabled);
    CHECK_FALSE(findAction(inXy.value(), path)->enabled);
    CHECK(
      findAction(inXy.value(), path)->context
      == "3D view, any or no selection, any or no tool");

    CHECK(actionHost.actions(document, "front").is_error());
    CHECK(actionHost.invokeAction(document, path, "xy", false).is_error());
  }

  SECTION("errors")
  {
    CHECK(
      actionHost.invokeAction(document, "Menu/Nothing", std::nullopt, false).is_error());

    auto other = MapDocument::createDocument(
                   appController.environmentConfig(),
                   gameInfo,
                   mdl::MapFormat::Valve,
                   vm::bbox3d{8192.0},
                   appController.taskManager(),
                   appController.glManager().resourceManager())
                 | kdl::value();
    CHECK(actionHost.actions(*other, std::nullopt).is_error());
    CHECK(actionHost
            .invokeAction(*other, "Menu/View/Grid/Set Grid Size 8", std::nullopt, false)
            .is_error());
  }

  SECTION("deferred actions run from the event loop")
  {
    const auto gridSize = map.grid().size();
    const auto path = std::string{"Menu/View/Grid/Set Grid Size 8"};
    REQUIRE(gridSize != 3);

    const auto result = actionHost.invokeAction(document, path, std::nullopt, true);
    REQUIRE(result.is_success());
    CHECK(map.grid().size() == gridSize);

    CHECK(QTest::qWaitFor([&]() { return map.grid().size() == 3; }, 1000));
  }

  SECTION("action tools end to end")
  {
    auto fixture = mcp::McpToolFixture{};
    fixture.host().actionHostOverride = &actionHost;
    fixture.host().addDocument(document);
    const auto registered = RegisteredDocument{fixture.host(), document};

    SECTION("grid size")
    {
      const auto result = fixture.call(
        "action_invoke", mcp::Json{{"path", "Menu/View/Grid/Set Grid Size 32"}});
      CHECK(result["result"]["executed"] == true);
      CHECK(result["result"]["mapChanged"] == false);
      CHECK(result["undoStep"].is_null());
      CHECK(map.grid().actualSize() == 32.0);
    }

    SECTION("select all and deselect all")
    {
      fixture.call(
        "brush_create_box", mcp::Json{{"min", {0, 0, 0}}, {"max", {64, 64, 64}}});
      fixture.call("selection_clear");
      REQUIRE_FALSE(map.selection().hasNodes());

      fixture.call("action_invoke", mcp::Json{{"path", "Menu/Edit/Select All"}});
      // the new map has a brush already
      CHECK(map.selection().nodes.size() == 2);

      fixture.call("action_invoke", mcp::Json{{"path", "Deselect All"}});
      CHECK_FALSE(map.selection().hasNodes());
    }

    SECTION("a view filter")
    {
      const auto path = "Controls/Map view/View Filter > Toggle show brushes";
      const auto showBrushes = pref(Preferences::ShowBrushes);

      fixture.call("action_invoke", mcp::Json{{"path", path}});
      CHECK(pref(Preferences::ShowBrushes) == !showBrushes);

      fixture.call("action_invoke", mcp::Json{{"path", path}});
      CHECK(pref(Preferences::ShowBrushes) == showBrushes);
    }

    SECTION("a tag visibility action")
    {
      const auto& tag = map.tagManager().smartTags().front();
      const auto path = "Filters/Tags/" + tag.name() + "/Toggle Visible";
      const auto isHidden = [&]() {
        return (map.editorContext().hiddenTags() & tag.type()) != 0;
      };
      REQUIRE_FALSE(isHidden());

      fixture.call("action_invoke", mcp::Json{{"path", path}});
      CHECK(isHidden());

      fixture.call("action_invoke", mcp::Json{{"path", path}});
      CHECK_FALSE(isHidden());
    }

    SECTION("an entity definition action changes the map in one undo step")
    {
      const auto result =
        fixture.call("action_invoke", mcp::Json{{"path", "Entities/light/Create"}});
      CHECK(result["result"]["mapChanged"] == true);
      CHECK(result["undoStep"] == "AI: Invoke Action");
      CHECK(result["changes"]["created"].size() == 1);

      const auto door = fixture.callExpectingError(
        "action_invoke", mcp::Json{{"path", "Entities/func_door/Create"}});
      CHECK(door.code == mcp::ErrorCode::OperationFailed);
      CHECK(door.message.find("disabled") != std::string::npos);
    }

    SECTION("a tool toggle and Deactivate Current Tool")
    {
      fixture.call(
        "brush_create_box", mcp::Json{{"min", {0, 0, 0}}, {"max", {64, 64, 64}}});
      REQUIRE(map.selection().hasOnlyBrushes());

      const auto activated =
        fixture.call("action_invoke", mcp::Json{{"path", "Menu/Edit/Tools/Clip Tool"}});
      CHECK(activated["result"]["checked"] == true);
      CHECK(mapWindow.toolBox().clipToolActive());

      // the clip tool's own actions are enabled now
      const auto list = fixture.call(
        "actions_list", mcp::Json{{"query", "Perform clip"}, {"kind", "view"}});
      REQUIRE(list["items"].size() == 1);
      CHECK(list["items"][0]["enabled"] == true);

      // a Map call through action_invoke keeps the tool active
      fixture.call(
        "action_invoke", mcp::Json{{"path", "Menu/View/Grid/Set Grid Size 16"}});
      CHECK(mapWindow.toolBox().clipToolActive());

      const auto deactivated = fixture.call(
        "action_invoke",
        mcp::Json{{"path", "Controls/Map view/Deactivate current tool"}});
      CHECK(deactivated["result"]["checked"] == true);
      CHECK_FALSE(mapWindow.toolBox().clipToolActive());
    }

    SECTION("refused and dialog actions are never run")
    {
      CHECK(
        fixture.callExpectingError("action_invoke", mcp::Json{{"path", "Menu/Edit/Undo"}})
          .code
        == mcp::ErrorCode::ActionRefused);
      CHECK(
        fixture
          .callExpectingError(
            "action_invoke", mcp::Json{{"path", "Menu/File/Save as..."}})
          .code
        == mcp::ErrorCode::DialogRequired);
      CHECK(
        fixture
          .callExpectingError(
            "action_invoke", mcp::Json{{"path", "Menu/Edit/Replace Material..."}})
          .code
        == mcp::ErrorCode::DialogRequired);
    }

    SECTION("actions_list")
    {
      const auto result = fixture.call(
        "actions_list", mcp::Json{{"menu", "View/Grid"}, {"detail", "full"}});
      CHECK(result["total"] == 16);
      CHECK(result["items"][0]["path"] == "Menu/View/Grid/Show Grid");
      CHECK(result["items"][0]["checkable"] == true);
      CHECK(result["items"][0]["tools"] == mcp::Json{"grid_set"});
    }
  }

  closeAllMapWindows(appController);
}

} // namespace tb::ui
