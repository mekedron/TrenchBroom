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

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QtTest/QTest>

#include "McpUiTestUtils.h"
#include "base/Logger.h"
#include "fs/TestEnvironment.h"
#include "gl/GlManager.h"
#include "mcp/ConsoleBuffer.h"
#include "mcp/Snapshot.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"
#include "ui/AppController.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpConsoleHook.h"
#include "ui/McpSnapshotRenderer.h"
#include "ui/QtMcpHost.h"
#include "ui/RecentDocuments.h"

#include "kd/result.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

/** Redirects QSettings to a test location while map windows save their state. */
struct TestModeStandardPaths
{
  TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(true); }
  ~TestModeStandardPaths() { QStandardPaths::setTestModeEnabled(false); }
};

/** Registers an unshown window like createMapWindow in McpUiTestUtils. */
QtMcpHost::CreateMapWindow createUnshownMapWindow(AppController& appController)
{
  return [&](std::unique_ptr<MapDocument> document) {
    auto* mapWindow = new MapWindow{appController, std::move(document)};
    appController.mapWindowManager().addMapWindow(mapWindow);
    sendShowEvent(*mapWindow);
    return mapWindow;
  };
}

bool isRecent(QtMcpHost& host, const std::filesystem::path& path)
{
  const auto recent = host.recentDocuments();
  return std::ranges::find(recent, path) != recent.end();
}

void addBrush(MapDocument& document)
{
  auto& map = document.map();
  mdl::addNodes(map, {{&mdl::parentForNodes(map), {mdl::createBrushNode(map)}}});
}

bool glAvailable()
{
  auto format = QSurfaceFormat{};
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(2, 1);
  format.setProfile(QSurfaceFormat::CompatibilityProfile);

  auto surface = QOffscreenSurface{};
  surface.setFormat(format);
  surface.create();

  auto context = QOpenGLContext{};
  context.setFormat(format);
  return surface.isValid() && context.create() && !context.isOpenGLES()
         && context.makeCurrent(&surface) && (context.doneCurrent(), true);
}

void collectNodes(mdl::Node& node, std::vector<mdl::Node*>& result)
{
  if (
    !dynamic_cast<const mdl::WorldNode*>(&node)
    && !dynamic_cast<const mdl::LayerNode*>(&node))
  {
    result.push_back(&node);
  }
  for (auto* child : node.children())
  {
    collectNodes(*child, result);
  }
}

} // namespace

TEST_CASE("QtMcpHost background documents")
{
  const auto testModeStandardPaths = TestModeStandardPaths{};
  auto env = fs::TestEnvironment{};

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto& mapWindowManager = appController.mapWindowManager();

  auto consoleHook = McpConsoleHook{};
  auto host = std::make_unique<QtMcpHost>(appController);
  host->setConsoleHook(&consoleHook);
  host->setCreateMapWindow(createUnshownMapWindow(appController));
  auto& documentHost = host->documentHost();

  auto didChangeCount = 0;
  auto closedDocuments = std::vector<const MapDocument*>{};
  auto connection = host->documentsDidChangeNotifier.connect([&]() { ++didChangeCount; });
  connection += host->documentWillCloseNotifier.connect(
    [&](auto& document) { closedDocuments.push_back(&document); });

  SECTION("createDocument in the background")
  {
    auto& window = createMapWindow(appController);
    didChangeCount = 0;

    const auto opened =
      documentHost.createDocument(mdl::QuakeGameInfo, mdl::MapFormat::Valve, true)
      | kdl::value();
    auto& document = *opened.document.document;
    CHECK(opened.document.id == "doc:2");
    CHECK(opened.document.background);
    CHECK(!opened.document.focused);
    CHECK(didChangeCount == 1);

    // not a map window; the window keeps the focus
    CHECK(mapWindowManager.mapWindows() == std::vector<MapWindow*>{&window});
    CHECK(host->findMapWindow(document) == nullptr);

    const auto documents = host->documents();
    REQUIRE(documents.size() == 2);
    CHECK(documents[0].document == &window.document());
    CHECK(documents[0].focused);
    CHECK(!documents[0].background);
    CHECK(documents[1].document == &document);
    CHECK(documents[1].background);

    // background documents never replace the window's document
    if (const auto replaced = documentHost.documentToReplace())
    {
      CHECK(replaced->document == &window.document());
    }

    CHECK(host->busyState(document) == mcp::BusyState::Idle);
    CHECK(!host->currentToolName(document));
    CHECK(!host->isCompileRunning(document));

    SECTION("messages go to the console buffer")
    {
      REQUIRE(host->logTarget(document) != nullptr);
      document.logger().warn() << "background warning";

      const auto& messages = consoleHook.buffer().messages();
      const auto it = std::ranges::find_if(messages, [](const auto& message) {
        return message.text == "background warning";
      });
      REQUIRE(it != messages.end());
      CHECK(it->level == LogLevel::Warn);
      CHECK(it->document == &document);
    }

    SECTION("save as does not add a recent document until the document is shown")
    {
      const auto path = env.dir() / "background.map";
      REQUIRE(document.map().saveAs(path).is_success());
      CHECK(!isRecent(*host, path));
      CHECK(host->documents()[1].background);
    }

    SECTION("closeDocument")
    {
      addBrush(document);
      REQUIRE(document.map().modified());
      didChangeCount = 0;

      documentHost.closeDocument(document);
      CHECK(host->documents().size() == 1);

      // destroyed when control returns to the event loop
      CHECK(closedDocuments.empty());
      REQUIRE(QTest::qWaitFor([&]() { return !closedDocuments.empty(); }, 1000));
      CHECK(closedDocuments == std::vector<const MapDocument*>{&document});
      CHECK(didChangeCount == 1);

      // ids are never reused
      const auto next =
        documentHost.createDocument(mdl::QuakeGameInfo, mdl::MapFormat::Valve, true)
        | kdl::value();
      CHECK(next.document.id == "doc:3");
    }

    closeAllMapWindows(appController);
  }

  SECTION("loadDocument in the background")
  {
    const auto path = env.dir() / "load.map";
    {
      auto created =
        documentHost.createDocument(mdl::QuakeGameInfo, mdl::MapFormat::Valve, true)
        | kdl::value();
      REQUIRE(created.document.document->map().saveAs(path).is_success());
      documentHost.closeDocument(*created.document.document);
      REQUIRE(QTest::qWaitFor([&]() { return !closedDocuments.empty(); }, 1000));
    }

    const auto opened =
      documentHost.loadDocument(mdl::QuakeGameInfo, mdl::MapFormat::Unknown, path, true)
      | kdl::value();
    CHECK(opened.document.background);
    CHECK(opened.document.document->map().path() == path);
    CHECK(mapWindowManager.mapWindows().empty());
    CHECK(!isRecent(*host, path));
  }

  SECTION("showDocument")
  {
    const auto opened =
      documentHost.createDocument(mdl::QuakeGameInfo, mdl::MapFormat::Valve, true)
      | kdl::value();
    auto& document = *opened.document.document;
    addBrush(document);
    const auto path = env.dir() / "shown.map";
    REQUIRE(document.map().saveAs(path).is_success());
    addBrush(document);
    REQUIRE(document.map().modified());
    const auto undoName = document.map().undoCommandName();
    didChangeCount = 0;

    REQUIRE(documentHost.showDocument(document).is_success());

    auto* mapWindow = host->findMapWindow(document);
    REQUIRE(mapWindow != nullptr);
    CHECK(mapWindowManager.topMapWindow() == mapWindow);
    CHECK(closedDocuments.empty());
    CHECK(didChangeCount >= 1);

    const auto documents = host->documents();
    REQUIRE(documents.size() == 1);
    CHECK(documents[0].id == opened.document.id);
    CHECK(!documents[0].background);
    CHECK(documents[0].focused);

    // the document keeps its changes and history, and the window's console its messages
    CHECK(document.map().modified());
    CHECK(document.map().undoCommandName() == undoName);
    CHECK(host->logTarget(document) == &mapWindow->logger());
    CHECK(isRecent(*host, path));

    // it is no longer a background document
    CHECK(documentHost.showDocument(document).is_error());

    closeAllMapWindows(appController);
    CHECK(closedDocuments == std::vector<const MapDocument*>{&document});
  }

  // Single-window mode is a platform setting (AppController::useSDI)
  if (AppController::useSDI)
  {
    SECTION("showDocument in single-window mode while a window is open")
    {
      auto& window = createMapWindow(appController);
      const auto opened =
        documentHost.createDocument(mdl::QuakeGameInfo, mdl::MapFormat::Valve, true)
        | kdl::value();
      CHECK(documentHost.showDocument(*opened.document.document).is_error());
      CHECK(host->documents()[1].background);
      CHECK(host->findMapWindow(window.document()) == &window);

      closeAllMapWindows(appController);
    }
  }

  SECTION("destroying the host discards unsaved background documents")
  {
    auto opened =
      documentHost.createDocument(mdl::QuakeGameInfo, mdl::MapFormat::Valve, true)
      | kdl::value();
    addBrush(*opened.document.document);
    REQUIRE(opened.document.document->map().modified());

    connection.disconnect();
    host.reset();
    CHECK(mapWindowManager.mapWindows().empty());
  }
}

TEST_CASE("QtMcpHost background documents render snapshots", "[gpu]")
{
  if (!glAvailable())
  {
    SKIP("No OpenGL context available on this platform");
  }

  const auto testModeStandardPaths = TestModeStandardPaths{};
  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();
  auto host = QtMcpHost{appController};

  const auto opened = host.documentHost().loadDocument(
                        mdl::QuakeGameInfo,
                        mdl::MapFormat::Unknown,
                        std::filesystem::path{MCP_TEST_MAPS_DIR} / "two_rooms.map",
                        true)
                      | kdl::value();
  auto& document = *opened.document.document;
  REQUIRE(appController.mapWindowManager().mapWindows().empty());

  // The application controller of the tests finds no shaders, so render with a GL
  // manager that loads them from the editor's resources, as the snapshot renderer tests
  // do
  auto glManager = gl::GlManager{[](const std::filesystem::path& path) {
    return std::filesystem::path{MCP_TEST_RESOURCE_DIR} / path;
  }};
  auto snapshotRenderer = McpSnapshotRenderer{
    glManager,
    [&](const MapDocument& document_) { return host.findMapWindow(document_); }};
  auto* renderer = &snapshotRenderer;
  REQUIRE(QTest::qWaitFor([&]() { return !renderer->resourcesPending(document); }, 5000));

  auto nodes = std::vector<mdl::Node*>{};
  collectNodes(document.map().worldNode(), nodes);

  const auto request = mcp::SnapshotRequest{
    .camera =
      mcp::AgentCamera{
        .projection = mcp::CameraProjection::Perspective,
        .position = vm::vec3d{300, 256, 64},
        .direction = vm::vec3d{1, 0, 0},
        .up = vm::vec3d{0, 0, 1},
        .fov = 90.0,
      },
    .width = 160,
    .height = 120,
    .options = mcp::SnapshotOptions{.background = RgbaF{0.0f, 0.0f, 0.0f, 1.0f}},
    .scene = mcp::SnapshotScene{.nodes = nodes},
  };

  const auto image = renderer->render(document, request);
  REQUIRE(image.is_success());
  const auto& pixels = image.value().pixels;
  auto drawn = size_t{0};
  for (size_t i = 0; i + 3 < pixels.size(); i += 4)
  {
    drawn += (pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0) ? 1 : 0;
  }
  CHECK(drawn > 160 * 120 / 2);
}

} // namespace tb::ui
