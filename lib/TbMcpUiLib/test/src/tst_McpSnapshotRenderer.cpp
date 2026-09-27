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

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QStandardPaths>
#include <QSurfaceFormat>

#include "McpUiTestUtils.h"
#include "gl/GlManager.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "gl/ResourceManager.h"
#include "mcp/CameraProjection.h"
#include "mcp/Image.h"
#include "mcp/Json.h"
#include "mcp/McpToolFixture.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"
#include "ui/AppControllerFixture.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/McpSnapshotRenderer.h"
#include "ui/QtMcpHost.h"

#include "kd/result.h"
#include "kd/task_manager.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ranges>

#include <catch2/catch_test_macros.hpp>

namespace tb::ui
{
namespace
{

const auto WorldBounds = vm::bbox3d{8192.0};

/** Whether this process can create an OpenGL 2.1 context, e.g. not under offscreen. */
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

gl::GlManager createGlManager()
{
  return gl::GlManager{[](const std::filesystem::path& path) {
    return std::filesystem::path{MCP_TEST_RESOURCE_DIR} / path;
  }};
}

/** Collects the nodes of the map except the world and layers, skipping triggers. */
void collectNodes(mdl::Node& node, bool withTriggers, std::vector<mdl::Node*>& result)
{
  if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
      entityNode && !withTriggers
      && entityNode->entity().classname().starts_with("trigger"))
  {
    return;
  }

  if (
    !dynamic_cast<const mdl::WorldNode*>(&node)
    && !dynamic_cast<const mdl::LayerNode*>(&node))
  {
    result.push_back(&node);
  }

  for (auto* child : node.children())
  {
    collectNodes(*child, withTriggers, result);
  }
}

std::vector<mdl::Node*> sceneNodes(MapDocument& document, const bool withTriggers)
{
  auto result = std::vector<mdl::Node*>{};
  collectNodes(document.map().worldNode(), withTriggers, result);
  return result;
}

mdl::Node* findEntity(MapDocument& document, const std::string& classname)
{
  const auto nodes = sceneNodes(document, true);
  const auto it = std::ranges::find_if(nodes, [&](const auto* node) {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node);
    return entityNode && entityNode->entity().classname() == classname;
  });
  return it != nodes.end() ? *it : nullptr;
}

/** The number of pixels that differ from the given color. */
size_t countPixelsDifferentFrom(const mcp::RgbaImage& image, const RgbB& color)
{
  auto count = size_t{0};
  for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
  {
    if (
      image.pixels[i] != color.get<ColorChannel::r>()
      || image.pixels[i + 1] != color.get<ColorChannel::g>()
      || image.pixels[i + 2] != color.get<ColorChannel::b>())
    {
      ++count;
    }
  }
  return count;
}

/** The number of pixels that differ between the given images of the same size. */
size_t countDifferentPixels(const mcp::RgbaImage& lhs, const mcp::RgbaImage& rhs)
{
  REQUIRE(lhs.width == rhs.width);
  REQUIRE(lhs.height == rhs.height);

  auto count = size_t{0};
  for (size_t i = 0; i + 3 < lhs.pixels.size(); i += 4)
  {
    if (!std::equal(
          lhs.pixels.begin() + std::ptrdiff_t(i),
          lhs.pixels.begin() + std::ptrdiff_t(i + 3),
          rhs.pixels.begin() + std::ptrdiff_t(i)))
    {
      ++count;
    }
  }
  return count;
}

/** Saves the image as PNG if MCP_SNAPSHOT_TEST_DIR is set, to inspect it manually. */
void saveForInspection(const mcp::RgbaImage& image, const std::string& name)
{
  if (const auto* dir = std::getenv("MCP_SNAPSHOT_TEST_DIR"))
  {
    if (const auto png = mcp::encodePng(image))
    {
      auto stream = std::ofstream{
        std::filesystem::path{dir} / (name + ".png"), std::ios::binary | std::ios::out};
      stream << *png;
    }
  }
}

/** Looks from the first room of two_rooms.map through the corridor with the trigger. */
mcp::AgentCamera corridorCamera()
{
  return mcp::AgentCamera{
    .projection = mcp::CameraProjection::Perspective,
    .position = vm::vec3d{300, 256, 64},
    .direction = vm::vec3d{1, 0, 0},
    .up = vm::vec3d{0, 0, 1},
    .fov = 90.0,
  };
}

const auto Black = RgbB{0, 0, 0};

} // namespace

TEST_CASE("makeGlCamera")
{
  SECTION("Perspective")
  {
    const auto agentCamera = mcp::AgentCamera{
      .projection = mcp::CameraProjection::Perspective,
      .position = vm::vec3d{1, 2, 3},
      .direction = vm::vec3d{0, 2, 0},
      .up = vm::vec3d{0, 0, 1},
      .fov = 75.0,
      .nearPlane = 2.0,
      .farPlane = 4096.0,
    };

    const auto camera = mcp::makeGlCamera(agentCamera, 640, 480) | kdl::value();
    const auto* perspectiveCamera =
      dynamic_cast<const gl::PerspectiveCamera*>(camera.get());
    REQUIRE(perspectiveCamera != nullptr);
    CHECK(perspectiveCamera->fov() == 75.0f);
    CHECK(perspectiveCamera->zoomedFov() == vm::approx{75.0f});
    CHECK(camera->viewport() == gl::Camera::Viewport{0, 0, 640, 480});
    CHECK(camera->position() == vm::vec3f{1, 2, 3});
    CHECK(camera->direction() == vm::vec3f{0, 1, 0});
    CHECK(camera->up() == vm::vec3f{0, 0, 1});
    CHECK(camera->nearPlane() == 2.0f);
    CHECK(camera->farPlane() == 4096.0f);
  }

  SECTION("Orthographic")
  {
    const auto agentCamera = mcp::AgentCamera{
      .projection = mcp::CameraProjection::Orthographic,
      .position = vm::vec3d{0, 0, 1000},
      .direction = vm::vec3d{0, 0, -1},
      .up = vm::vec3d{0, 1, 0},
      .zoom = 0.5,
      .nearPlane = 1.0,
      .farPlane = 2048.0,
    };

    const auto camera = mcp::makeGlCamera(agentCamera, 800, 600) | kdl::value();
    const auto* orthographicCamera =
      dynamic_cast<const gl::OrthographicCamera*>(camera.get());
    REQUIRE(orthographicCamera != nullptr);
    CHECK(camera->zoom() == 0.5f);
    CHECK(camera->viewport() == gl::Camera::Viewport{0, 0, 800, 600});
    // at 0.5 pixels per unit, the image shows 1600x1200 units
    CHECK(orthographicCamera->zoomedViewport().width == 1600);
    CHECK(orthographicCamera->zoomedViewport().height == 1200);
    CHECK(camera->direction() == vm::vec3f{0, 0, -1});
    CHECK(camera->up() == vm::vec3f{0, 1, 0});
  }

  SECTION("Invalid cameras and sizes")
  {
    const auto valid = mcp::AgentCamera{};
    CHECK(mcp::makeGlCamera(valid, 64, 64).is_success());

    CHECK(mcp::makeGlCamera(valid, 0, 64).is_error());
    CHECK(mcp::makeGlCamera(valid, 64, 0).is_error());
    CHECK(mcp::makeGlCamera(valid, 100000, 64).is_error());

    auto camera = valid;
    camera.nearPlane = 0.0;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera = valid;
    camera.farPlane = camera.nearPlane;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera = valid;
    camera.direction = vm::vec3d{0, 0, 0};
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera = valid;
    camera.up = camera.direction;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera = valid;
    camera.fov = 0.0;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera = valid;
    camera.fov = 170.0;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera = valid;
    camera.projection = mcp::CameraProjection::Orthographic;
    camera.zoom = 0.0;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());

    camera.zoom = 1000.0;
    CHECK(mcp::makeGlCamera(camera, 64, 64).is_error());
  }
}

TEST_CASE("toAgentCamera")
{
  SECTION("Perspective")
  {
    auto camera = gl::PerspectiveCamera{
      60.0f,
      1.0f,
      8192.0f,
      gl::Camera::Viewport{0, 0, 100, 100},
      vm::vec3f{10, 20, 30},
      vm::vec3f{1, 0, 0},
      vm::vec3f{0, 0, 1}};

    CHECK(
      toAgentCamera(camera)
      == mcp::AgentCamera{
        .projection = mcp::CameraProjection::Perspective,
        .position = vm::vec3d{10, 20, 30},
        .direction = vm::vec3d{1, 0, 0},
        .up = vm::vec3d{0, 0, 1},
        .fov = 60.0,
        .zoom = 1.0,
        .nearPlane = 1.0,
        .farPlane = 8192.0,
      });

    // the effective field of view of a zoomed camera
    camera.setZoom(2.0f);
    CHECK(toAgentCamera(camera).fov == vm::approx{double(camera.zoomedFov())});
    CHECK(toAgentCamera(camera).fov != vm::approx{60.0});
  }

  SECTION("Orthographic")
  {
    auto camera = gl::OrthographicCamera{
      1.0f,
      4096.0f,
      gl::Camera::Viewport{0, 0, 100, 100},
      vm::vec3f{0, 0, 512},
      vm::vec3f{0, 0, -1},
      vm::vec3f{0, 1, 0}};
    camera.setZoom(4.0f);

    const auto agentCamera = toAgentCamera(camera);
    CHECK(agentCamera.projection == mcp::CameraProjection::Orthographic);
    CHECK(agentCamera.position == vm::vec3d{0, 0, 512});
    CHECK(agentCamera.direction == vm::vec3d{0, 0, -1});
    CHECK(agentCamera.up == vm::vec3d{0, 1, 0});
    CHECK(agentCamera.zoom == 4.0);
    CHECK(agentCamera.farPlane == 4096.0);
  }

  SECTION("Round trip")
  {
    const auto agentCamera = mcp::AgentCamera{
      .projection = mcp::CameraProjection::Orthographic,
      .position = vm::vec3d{0, -512, 0},
      .direction = vm::vec3d{0, 1, 0},
      .up = vm::vec3d{0, 0, 1},
      .zoom = 2.0,
      .nearPlane = 1.0,
      .farPlane = 1024.0,
    };
    CHECK(
      toAgentCamera(*(mcp::makeGlCamera(agentCamera, 32, 32) | kdl::value()))
      == agentCamera);
  }
}

TEST_CASE("McpSnapshotRenderer")
{
  auto glManager = createGlManager();

  SECTION("encodeJpeg")
  {
    auto renderer = McpSnapshotRenderer{glManager};

    auto image = mcp::RgbaImage{4, 2, std::vector<unsigned char>(4 * 2 * 4, 255)};
    const auto jpeg = renderer.encodeJpeg(image, 80);
    REQUIRE(jpeg.has_value());

    const auto decoded = QImage::fromData(
      reinterpret_cast<const uchar*>(jpeg->data()), int(jpeg->size()), "JPG");
    CHECK(decoded.width() == 4);
    CHECK(decoded.height() == 2);

    CHECK(renderer.encodeJpeg(mcp::RgbaImage{}, 80) == std::nullopt);
  }

  SECTION("User views without a window")
  {
    auto taskManager = createTestTaskManager();
    auto document = MapDocument::createDocument(
                      mdl::EnvironmentConfig{},
                      mdl::QuakeGameInfo,
                      mdl::MapFormat::Standard,
                      WorldBounds,
                      *taskManager,
                      glManager.resourceManager())
                    | kdl::value();

    auto renderer = McpSnapshotRenderer{glManager};
    CHECK(renderer.userViews(*document).empty());
    CHECK(renderer.captureUserView(*document, "3d").is_error());
  }
}

TEST_CASE("McpSnapshotRenderer user views")
{
  QStandardPaths::setTestModeEnabled(true);

  auto appControllerFixture = AppControllerFixture{};
  auto& appController = appControllerFixture.appController();

  auto& mapWindow = createMapWindow(appController);
  auto host = QtMcpHost{appController};
  auto* renderer = host.snapshotRenderer();
  REQUIRE(renderer != nullptr);
  CHECK(host.snapshotRenderer() == renderer);

  auto& document = mapWindow.document();

  // The window is not shown because the offscreen platform has no OpenGL
  const auto views = renderer->userViews(document);
  REQUIRE(!views.empty());
  CHECK(views.front().id == "3d");
  CHECK(views.front().camera.projection == mcp::CameraProjection::Perspective);
  for (const auto& view : views)
  {
    CHECK(!view.visible);
    CHECK((view.id == "3d" || view.id == "xy" || view.id == "xz" || view.id == "yz"));
    if (view.id != "3d")
    {
      CHECK(view.camera.projection == mcp::CameraProjection::Orthographic);
    }
  }

  const auto error = renderer->captureUserView(document, "3d");
  REQUIRE(error.is_error());
  CHECK(renderer->captureUserView(document, "zz").is_error());

  closeAllMapWindows(appController);
  processDeferredDeletes();
  QStandardPaths::setTestModeEnabled(false);
}

TEST_CASE("McpSnapshotRenderer renders with the GPU", "[gpu]")
{
  if (!glAvailable())
  {
    SKIP("No OpenGL context available on this platform");
  }

  auto glManager = createGlManager();
  auto taskManager = createTestTaskManager();
  auto renderer = McpSnapshotRenderer{glManager};

  SECTION("Sample map")
  {
    auto document = MapDocument::loadDocument(
                      mdl::EnvironmentConfig{},
                      mdl::QuakeGameInfo,
                      mdl::MapFormat::Unknown,
                      WorldBounds,
                      std::filesystem::path{MCP_TEST_MAPS_DIR} / "two_rooms.map",
                      *taskManager,
                      glManager.resourceManager())
                    | kdl::value();

    auto request = mcp::SnapshotRequest{
      .camera = corridorCamera(),
      .width = 320,
      .height = 240,
      .options = mcp::SnapshotOptions{.background = RgbaF{0.0f, 0.0f, 0.0f, 1.0f}},
      .scene = mcp::SnapshotScene{.nodes = sceneNodes(*document, true)},
    };

    const auto withTriggers = renderer.render(*document, request) | kdl::value();
    saveForInspection(withTriggers, "with_triggers");

    CHECK(withTriggers.width == 320);
    CHECK(withTriggers.height == 240);
    CHECK(withTriggers.pixels.size() == 320 * 240 * 4);
    CHECK(countPixelsDifferentFrom(withTriggers, Black) > 320 * 240 / 2);

    SECTION("Toggling triggers changes the image")
    {
      request.scene.nodes = sceneNodes(*document, false);
      const auto withoutTriggers = renderer.render(*document, request) | kdl::value();
      saveForInspection(withoutTriggers, "without_triggers");

      CHECK(countDifferentPixels(withTriggers, withoutTriggers) > 100);
    }

    SECTION("The editor's hidden state does not matter")
    {
      auto* trigger = findEntity(*document, "trigger_once");
      REQUIRE(trigger != nullptr);
      mdl::hideNodes(document->map(), {trigger});
      REQUIRE(!trigger->visible());

      const auto hiddenTrigger = renderer.render(*document, request) | kdl::value();
      CHECK(countDifferentPixels(withTriggers, hiddenTrigger) == 0);
    }

    SECTION("Rendering is repeatable")
    {
      const auto again = renderer.render(*document, request) | kdl::value();
      CHECK(countDifferentPixels(withTriggers, again) == 0);
    }

    SECTION("Highlighted objects are tinted")
    {
      auto* trigger = findEntity(*document, "trigger_once");
      REQUIRE(trigger != nullptr);
      request.scene.highlighted = {trigger->children().front()};

      const auto highlighted = renderer.render(*document, request) | kdl::value();
      saveForInspection(highlighted, "highlighted");
      CHECK(countDifferentPixels(withTriggers, highlighted) > 100);
    }

    SECTION("Face filter")
    {
      request.scene.faceFilter = [](const auto&, const auto& face) {
        return face.materialName() != "trigger";
      };

      const auto filtered = renderer.render(*document, request) | kdl::value();
      request.scene.nodes = sceneNodes(*document, false);
      request.scene.faceFilter = nullptr;
      const auto withoutTriggers = renderer.render(*document, request) | kdl::value();
      CHECK(countDifferentPixels(filtered, withoutTriggers) == 0);
    }

    SECTION("Options")
    {
      request.options.faceMode = mcp::FaceMode::Wireframe;
      const auto wireframe = renderer.render(*document, request) | kdl::value();
      saveForInspection(wireframe, "wireframe");
      CHECK(countPixelsDifferentFrom(wireframe, Black) > 0);
      CHECK(countPixelsDifferentFrom(wireframe, Black) < 320 * 240 / 2);

      request.options = mcp::SnapshotOptions{
        .faceMode = mcp::FaceMode::Flat,
        .bounds = true,
        .classnames = true,
        .entityLinks = true,
        .grid = true,
        .axes = true,
        .background = RgbaF{0.0f, 0.0f, 0.0f, 1.0f},
      };
      request.scene.markers = {
        mcp::SnapshotMarker{vm::vec3d{600, 256, 64}, RgbaF{1.0f, 0.0f, 0.0f, 1.0f}, "A"}};
      const auto decorated = renderer.render(*document, request) | kdl::value();
      saveForInspection(decorated, "decorated");
      CHECK(countDifferentPixels(withTriggers, decorated) > 100);
    }

    SECTION("Orthographic camera")
    {
      request.camera = mcp::AgentCamera{
        .projection = mcp::CameraProjection::Orthographic,
        .position = vm::vec3d{640, 256, 1024},
        .direction = vm::vec3d{0, 0, -1},
        .up = vm::vec3d{0, 1, 0},
        .zoom = 0.2,
        .nearPlane = 1.0,
        .farPlane = 16384.0,
      };
      request.options.faceMode = mcp::FaceMode::Wireframe;
      request.options.grid = true;

      const auto top = renderer.render(*document, request) | kdl::value();
      saveForInspection(top, "top");
      CHECK(countPixelsDifferentFrom(top, Black) > 0);
    }

    SECTION("Invalid sizes")
    {
      request.width = 0;
      CHECK(renderer.render(*document, request).is_error());

      request.width = 20000;
      CHECK(renderer.render(*document, request).is_error());
    }

    CHECK(!renderer.resourcesPending(*document));
  }

  SECTION("5000 brushes")
  {
    auto document = MapDocument::createDocument(
                      mdl::EnvironmentConfig{},
                      mdl::QuakeGameInfo,
                      mdl::MapFormat::Standard,
                      WorldBounds,
                      *taskManager,
                      glManager.resourceManager())
                    | kdl::value();
    auto& map = document->map();

    const auto builder = mdl::BrushBuilder{mdl::MapFormat::Standard, WorldBounds};
    auto brushes = std::vector<mdl::Node*>{};
    for (int x = 0; x < 100; ++x)
    {
      for (int y = 0; y < 50; ++y)
      {
        const auto min = vm::vec3d{x * 48.0, y * 48.0, 0.0};
        brushes.push_back(new mdl::BrushNode{
          builder.createCuboid(vm::bbox3d{min, min + vm::vec3d{32, 32, 32}}, "mat")
          | kdl::value()});
      }
    }
    mdl::addNodes(map, {{&mdl::parentForNodes(map), brushes}});

    const auto request = mcp::SnapshotRequest{
      .camera =
        mcp::AgentCamera{
          .projection = mcp::CameraProjection::Perspective,
          .position = vm::vec3d{2400, -1600, 2400},
          .direction = vm::normalize(vm::vec3d{0, 2800, -2400}),
          .up = vm::normalize(vm::vec3d{0, 2400, 2800}),
          .fov = 90.0,
        },
      .width = 1024,
      .height = 768,
      .scene = mcp::SnapshotScene{.nodes = brushes},
    };

    using Clock = std::chrono::steady_clock;
    auto timings = std::vector<long>{};
    auto image = mcp::RgbaImage{};
    for (int i = 0; i < 3; ++i)
    {
      const auto start = Clock::now();
      image = renderer.render(*document, request) | kdl::value();
      timings.push_back(
        long(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start)
               .count()));
    }
    saveForInspection(image, "5000_brushes");
    WARN(
      "5000 brushes at 1024x768 on " << gl::GlManager::glInfo().renderer << ": "
                                     << timings[0] << " ms, " << timings[1] << " ms, "
                                     << timings[2] << " ms");
    CHECK(timings[2] < 2000);
  }
}

TEST_CASE("view_snapshot with McpSnapshotRenderer", "[gpu]")
{
  if (!glAvailable())
  {
    SKIP("No OpenGL context available on this platform");
  }

  // The snapshot tool of the MCP server with the real renderer
  auto glManager = createGlManager();
  auto renderer = McpSnapshotRenderer{glManager};

  auto fixture = mcp::McpToolFixture{};
  fixture.host().snapshotRendererOverride = &renderer;
  fixture.call(
    "document_open",
    mcp::Json{
      {"path", (std::filesystem::path{MCP_TEST_MAPS_DIR} / "two_rooms.map").string()},
      {"game", "Quake"}});

  const auto snapshot = [&](mcp::Json options) {
    const auto result = fixture.callRaw(
      "view_snapshot",
      mcp::Json{
        {"camera",
         mcp::Json{
           {"position", mcp::Json::array({300, 256, 64})},
           {"direction", mcp::Json::array({1, 0, 0})}}},
        {"width", 320},
        {"height", 240},
        {"options", std::move(options)},
      });
    REQUIRE(result.value("isError", false) == false);

    const auto& content = result["content"];
    const auto image = std::ranges::find_if(
      content, [](const auto& block) { return block["type"] == "image"; });
    REQUIRE(image != content.end());
    CHECK((*image)["mimeType"] == "image/png");
    return (*image)["data"].template get<std::string>();
  };

  const auto withTriggers = snapshot(mcp::Json::object());
  const auto withoutTriggers =
    snapshot(mcp::Json{{"hideTags", mcp::Json::array({"trigger"})}});
  CHECK(withTriggers != withoutTriggers);
  CHECK(snapshot(mcp::Json::object()) == withTriggers);
}

} // namespace tb::ui
