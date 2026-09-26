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

#include <QApplication>
#include <QDialog>
#include <QStandardPaths>

#include "gl/GlManager.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "ui/AppControllerFixture.h"
#include "ui/CatchConfig.h"
#include "ui/GestureTracker.h"
#include "ui/InputState.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/QtMcpHost.h"
#include "ui/RecentDocuments.h"
#include "ui/Tool.h"
#include "ui/ToolChain.h"
#include "ui/ToolController.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

class FakeTool : public Tool
{
public:
  FakeTool()
    : Tool{true}
  {
  }
};

class FakeGestureTracker : public GestureTracker
{
public:
  bool update(const InputState&) override { return true; }
  void end(const InputState&) override {}
  void cancel() override {}
};

class DraggingToolController : public ToolController
{
private:
  FakeTool m_tool;

public:
  Tool& tool() override { return m_tool; }
  const Tool& tool() const override { return m_tool; }

  std::unique_ptr<GestureTracker> acceptMouseDrag(const InputState&) override
  {
    return std::make_unique<FakeGestureTracker>();
  }
};

/**
 * Redirects QSettings to a test location while map windows save their state on close.
 */
struct TestModeStandardPaths
{
  TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(true); }
  ~TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(false); }
};

/**
 * Creates a map window and registers it with the map window manager. The window is not
 * shown because the offscreen platform used in CI doesn't support OpenGL.
 */
MapWindow& createMapWindow(AppController& appController)
{
  auto document = MapDocument::createDocument(
                    appController.environmentConfig(),
                    mdl::QuakeGameInfo,
                    mdl::MapFormat::Valve,
                    vm::bbox3d{8192.0},
                    appController.taskManager(),
                    appController.glManager().resourceManager())
                  | kdl::value();

  // Deleted on close because MapWindow sets Qt::WA_DeleteOnClose
  auto* mapWindow = new MapWindow{appController, std::move(document)};
  appController.mapWindowManager().addMapWindow(mapWindow);
  return *mapWindow;
}

void closeMapWindow(MapWindow& mapWindow)
{
  REQUIRE(mapWindow.close());
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void closeAllMapWindows(AppController& appController)
{
  for (auto* mapWindow : appController.mapWindowManager().mapWindows())
  {
    // Undo all changes so that the window doesn't ask whether to save them
    auto& map = mapWindow->document().map();
    while (map.modified() && map.canUndoCommand())
    {
      map.undoCommand();
    }
    REQUIRE(!map.modified());
    mapWindow->close();
  }
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

std::vector<std::string> documentIds(QtMcpHost& host)
{
  auto result = std::vector<std::string>{};
  for (const auto& info : host.documents())
  {
    result.push_back(info.id);
  }
  return result;
}

} // namespace

TEST_CASE("QtMcpHost")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  auto host = QtMcpHost{appController};

  SECTION("applicationVersion")
  {
    CHECK_FALSE(host.applicationVersion().empty());
  }

  SECTION("documents")
  {
    CHECK(host.documents().empty());

    auto& window1 = createMapWindow(appController);
    auto& window2 = createMapWindow(appController);

    const auto documents = host.documents();
    REQUIRE(documents.size() == 2);

    // Documents are listed in the order in which they were opened
    CHECK(documents[0].id == "doc:1");
    CHECK(documents[0].document == &window1.document());
    CHECK(documents[0].windowTitle == "unnamed.map - TrenchBroom");
    CHECK(!documents[0].focused);

    CHECK(documents[1].id == "doc:2");
    CHECK(documents[1].document == &window2.document());
    CHECK(documents[1].focused);

    CHECK(host.findMapWindow(window1.document()) == &window1);
    CHECK(host.findMapWindow(window2.document()) == &window2);

    closeAllMapWindows(appController);
  }

  SECTION("documentsDidChangeNotifier and documentWillCloseNotifier")
  {
    auto didChangeCount = 0;
    auto closedDocuments = std::vector<const MapDocument*>{};
    auto closedDocumentWasListed = false;

    auto connection =
      host.documentsDidChangeNotifier.connect([&]() { ++didChangeCount; });
    connection += host.documentWillCloseNotifier.connect([&](auto& document) {
      closedDocuments.push_back(&document);

      // The document is still alive and listed while the notifier runs
      closedDocumentWasListed = std::ranges::any_of(
        host.documents(), [&](const auto& info) { return info.document == &document; });
    });

    auto& window1 = createMapWindow(appController);
    CHECK(didChangeCount > 0);

    auto& window2 = createMapWindow(appController);
    REQUIRE(documentIds(host) == std::vector<std::string>{"doc:1", "doc:2"});

    didChangeCount = 0;
    auto* document1 = &window1.document();
    closeMapWindow(window1);

    CHECK(closedDocuments == std::vector<const MapDocument*>{document1});
    CHECK(closedDocumentWasListed);
    CHECK(didChangeCount > 0);
    CHECK(documentIds(host) == std::vector<std::string>{"doc:2"});

    // Ids are never reused
    createMapWindow(appController);
    CHECK(documentIds(host) == std::vector<std::string>{"doc:2", "doc:3"});

    static_cast<void>(window2);
    closeAllMapWindows(appController);
  }

  SECTION("busyState")
  {
    auto& window = createMapWindow(appController);
    auto& document = window.document();

    CHECK(host.busyState(document) == mcp::BusyState::Idle);

    SECTION("modal dialog")
    {
      auto dialog = QDialog{};
      dialog.setModal(true);
      dialog.show();
      REQUIRE(QApplication::activeModalWidget() == &dialog);

      CHECK(host.busyState(document) == mcp::BusyState::Busy);

      dialog.hide();
      CHECK(host.busyState(document) == mcp::BusyState::Idle);
    }

    SECTION("mouse drag")
    {
      auto chain = ToolChain{};
      chain.append(std::make_unique<DraggingToolController>());

      auto& toolBox = window.toolBox();
      toolBox.startMouseDrag(chain, InputState{0.0f, 0.0f});
      REQUIRE(toolBox.dragging());

      CHECK(host.busyState(document) == mcp::BusyState::Busy);

      toolBox.cancelMouseDrag();
      CHECK(host.busyState(document) == mcp::BusyState::Idle);
    }

    closeAllMapWindows(appController);
  }

  SECTION("currentToolName and prepareForAgentEdit")
  {
    auto& window = createMapWindow(appController);
    auto& document = window.document();
    auto& map = document.map();
    auto& toolBox = window.toolBox();

    auto* brushNode = mdl::createBrushNode(map);
    mdl::addNodes(map, {{&mdl::parentForNodes(map), {brushNode}}});
    mdl::selectNodes(map, {brushNode});

    CHECK(host.currentToolName(document) == std::nullopt);
    CHECK(host.prepareForAgentEdit(document).empty());

    SECTION("tool that holds selection state is deactivated")
    {
      toolBox.toggleVertexTool();
      REQUIRE(toolBox.vertexToolActive());
      CHECK(host.currentToolName(document) == "Vertex Tool");

      CHECK(
        host.prepareForAgentEdit(document)
        == std::vector<std::string>{"deactivated tool: Vertex Tool"});
      CHECK(!toolBox.vertexToolActive());
      CHECK(host.currentToolName(document) == std::nullopt);
    }

    SECTION("clip tool is deactivated")
    {
      toolBox.toggleClipTool();
      REQUIRE(toolBox.clipToolActive());
      CHECK(host.currentToolName(document) == "Clip Tool");

      CHECK(
        host.prepareForAgentEdit(document)
        == std::vector<std::string>{"deactivated tool: Clip Tool"});
      CHECK(!toolBox.clipToolActive());
    }

    SECTION("other modal tools stay active")
    {
      toolBox.toggleRotateTool();
      REQUIRE(toolBox.rotateToolActive());
      CHECK(host.currentToolName(document) == "Rotate Tool");

      CHECK(host.prepareForAgentEdit(document).empty());
      CHECK(toolBox.rotateToolActive());

      toolBox.toggleRotateTool();
    }

    SECTION("currentToolDidChangeNotifier")
    {
      auto changedDocuments = std::vector<MapDocument*>{};
      auto connection =
        host.currentToolDidChangeNotifier.connect([&](MapDocument& changedDocument) {
          changedDocuments.push_back(&changedDocument);
        });

      // switching tools may activate and deactivate several tools; the server
      // coalesces the resulting updates
      const auto onlyThisDocument = [&]() {
        return std::ranges::all_of(
          changedDocuments, [&](const auto* changed) { return changed == &document; });
      };

      toolBox.toggleRotateTool();
      REQUIRE(toolBox.rotateToolActive());
      CHECK(!changedDocuments.empty());
      CHECK(onlyThisDocument());

      changedDocuments.clear();
      toolBox.toggleRotateTool();
      CHECK(!changedDocuments.empty());
      CHECK(onlyThisDocument());
    }

    closeAllMapWindows(appController);
  }

  SECTION("gameManager")
  {
    CHECK(&host.gameManager() == &appController.gameManager());
  }

  SECTION("documentHost")
  {
    auto& documentHost = host.documentHost();

    SECTION("documentToReplace")
    {
      CHECK(!documentHost.documentToReplace());

      auto& window = createMapWindow(appController);
      const auto replaced = documentHost.documentToReplace();
      CHECK(replaced.has_value() == AppController::useSDI);
      if (replaced)
      {
        CHECK(replaced->document == &window.document());
      }

      closeAllMapWindows(appController);
    }

    // createDocument and loadDocument show a window, which needs OpenGL; the offscreen
    // platform used for the tests does not support it. The tool logic is covered by
    // TbMcpLibTest with FakeHost.

    SECTION("closeDocument discards unsaved changes without asking")
    {
      auto closedDocuments = std::vector<const MapDocument*>{};
      auto connection = host.documentWillCloseNotifier.connect(
        [&](auto& document) { closedDocuments.push_back(&document); });

      auto& window = createMapWindow(appController);
      auto& document = window.document();
      auto& map = document.map();
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {mdl::createBrushNode(map)}}});
      REQUIRE(map.modified());

      documentHost.closeDocument(document);
      // the document is still alive until the event loop deletes the window
      CHECK(closedDocuments == std::vector<const MapDocument*>{&document});
      CHECK(host.documents().empty());

      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      CHECK(appController.mapWindowManager().allMapWindowsClosed());
    }

    SECTION("recentDocuments")
    {
      CHECK(
        documentHost.recentDocuments()
        == appController.recentDocuments().recentDocuments());
    }
  }

  SECTION("isCompileRunning")
  {
    auto& window = createMapWindow(appController);
    CHECK(!host.isCompileRunning(window.document()));

    closeAllMapWindows(appController);
  }
}

} // namespace tb::ui
