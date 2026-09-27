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
#include <QStatusBar>

#include "McpUiTestUtils.h"
#include "fs/TestEnvironment.h"
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

#include "vm/bbox.h"

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
    auto closedDocumentHadMap = false;

    auto connection =
      host.documentsDidChangeNotifier.connect([&]() { ++didChangeCount; });
    connection += host.documentWillCloseNotifier.connect([&](auto& document) {
      closedDocuments.push_back(&document);

      // The document is still alive while the notifier runs
      closedDocumentHadMap = document.map().filename() == "unnamed.map";
    });

    // Showing a window opens its document
    auto& window1 = createMapWindow(appController);
    CHECK(didChangeCount == 1);

    // Showing it again changes nothing
    sendShowEvent(window1);
    CHECK(didChangeCount == 1);

    auto& window2 = createMapWindow(appController);
    CHECK(didChangeCount == 2);
    REQUIRE(documentIds(host) == std::vector<std::string>{"doc:1", "doc:2"});

    didChangeCount = 0;
    auto* document1 = &window1.document();
    REQUIRE(window1.close());

    // The closed window's document isn't listed anymore, but the host only reports it
    // as closed when the window is deleted
    CHECK(documentIds(host) == std::vector<std::string>{"doc:2"});
    CHECK(closedDocuments.empty());

    processDeferredDeletes();
    CHECK(closedDocuments == std::vector<const MapDocument*>{document1});
    CHECK(closedDocumentHadMap);
    CHECK(didChangeCount == 1);
    CHECK(documentIds(host) == std::vector<std::string>{"doc:2"});

    // Ids are never reused
    createMapWindow(appController);
    CHECK(documentIds(host) == std::vector<std::string>{"doc:2", "doc:3"});

    static_cast<void>(window2);
    closeAllMapWindows(appController);
  }

  SECTION("documentsDidChangeNotifier on focus change")
  {
    auto& window1 = createMapWindow(appController);
    auto& window2 = createMapWindow(appController);
    REQUIRE(host.documents()[1].focused);

    auto didChangeCount = 0;
    auto connection =
      host.documentsDidChangeNotifier.connect([&]() { ++didChangeCount; });

    // The map window manager moves the window that receives the focus to the top
    emit qApp->focusChanged(window2.statusBar(), window1.statusBar());
    CHECK(didChangeCount == 1);
    CHECK(host.documents()[0].focused);

    // A focus change within the top window doesn't change the order
    emit qApp->focusChanged(window1.statusBar(), &window1);
    CHECK(didChangeCount == 1);

    closeAllMapWindows(appController);
  }

  SECTION("documentsDidChangeNotifier when a document is created in place")
  {
    auto& window = createMapWindow(appController);
    auto& document = window.document();

    auto didChangeCount = 0;
    auto connection =
      host.documentsDidChangeNotifier.connect([&]() { ++didChangeCount; });

    // Single window mode creates and loads documents in the top window
    REQUIRE(document.create(
      appController.environmentConfig(),
      mdl::QuakeGameInfo,
      mdl::MapFormat::Valve,
      vm::bbox3d{8192.0}));
    CHECK(didChangeCount == 1);
    CHECK(documentIds(host) == std::vector<std::string>{"doc:1"});

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
      CHECK(host.documents().empty());
      CHECK(appController.mapWindowManager().allMapWindowsClosed());

      // the document is still alive until the event loop deletes the window
      CHECK(closedDocuments.empty());
      processDeferredDeletes();
      CHECK(closedDocuments == std::vector<const MapDocument*>{&document});
    }

    SECTION("closeDocument keeps unsaved changes if the window refuses to close")
    {
      auto& window = createMapWindow(appController);
      auto& document = window.document();
      auto& map = document.map();
      mdl::addNodes(map, {{&mdl::parentForNodes(map), {mdl::createBrushNode(map)}}});
      REQUIRE(map.modified());
      const auto modificationCount = map.modificationCount();

      auto refuseClose = RefuseClose{};
      window.installEventFilter(&refuseClose);

      documentHost.closeDocument(document);
      processDeferredDeletes();

      CHECK(documentIds(host) == std::vector<std::string>{"doc:1"});
      CHECK(map.modified());
      CHECK(map.modificationCount() == modificationCount);

      window.removeEventFilter(&refuseClose);
      closeAllMapWindows(appController);
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
    auto env = fs::TestEnvironment{};
    // The map refers to its game info
    const auto gameInfo = withCompilationProfile(mdl::QuakeGameInfo, env.dir());
    auto& window = createMapWindow(appController, gameInfo);
    auto& document = window.document();
    CHECK(!host.isCompileRunning(document));

    const auto& dialog = startCompilation(window);
    CHECK(host.isCompileRunning(document));

    waitForCompilation(dialog);
    CHECK(!host.isCompileRunning(document));

    closeAllMapWindows(appController);
  }

  SECTION("logTarget")
  {
    auto& window = createMapWindow(appController);
    CHECK(host.logTarget(window.document()) == &window.logger());

    closeAllMapWindows(appController);
  }
}

} // namespace tb::ui
