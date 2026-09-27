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

#include "ui/McpSnapshotRenderer.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>

#include "base/Logger.h"
#include "base/Macros.h"
#include "base/PreferenceManager.h"
#include "gl/FontManager.h"
#include "gl/GlManager.h"
#include "gl/GlUtils.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "gl/ResourceManager.h"
#include "gl/VboManager.h"
#include "mcp/CameraProjection.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityLinkManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/PointTrace.h"
#include "prefs/Preferences.h"
#include "render/BrushRenderer.h"
#include "render/GridRenderer.h"
#include "render/LinkRenderer.h"
#include "render/ObjectRenderer.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"
#include "render/RenderService.h"
#include "ui/GlQt.h"
#include "ui/MapDocument.h"
#include "ui/MapView2D.h"
#include "ui/MapView3D.h"
#include "ui/MapWindow.h"
#include "ui/SwitchableMapViewContainer.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/task_manager.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace tb::ui
{
namespace
{

/** The largest image side that is rendered even if the driver allows more. */
constexpr auto MaxImageSize = int(mcp::MaxCameraImageSize);

/** The number of samples per pixel; Qt uses none if multisampling is not supported. */
constexpr auto Samples = 4;

/** Restores the current OpenGL context (or none) when destroyed. */
class ScopedCurrentContext
{
private:
  QOpenGLContext* m_previousContext;
  QSurface* m_previousSurface;

public:
  ScopedCurrentContext()
    : m_previousContext{QOpenGLContext::currentContext()}
    , m_previousSurface{m_previousContext ? m_previousContext->surface() : nullptr}
  {
  }

  ~ScopedCurrentContext()
  {
    if (m_previousContext && m_previousSurface)
    {
      m_previousContext->makeCurrent(m_previousSurface);
    }
    else if (auto* currentContext = QOpenGLContext::currentContext())
    {
      currentContext->doneCurrent();
    }
  }

  deleteCopyAndMove(ScopedCurrentContext);
};

/**
 * Draws all faces of the brushes it is given for which the face filter returns true, and
 * all edges of brushes that have a drawn face. The brushes' hidden, locked and selected
 * states are ignored.
 */
class SceneBrushFilter : public render::BrushRenderer::Filter
{
private:
  std::function<bool(const mdl::BrushNode&, const mdl::BrushFace&)> m_faceFilter;

public:
  explicit SceneBrushFilter(
    std::function<bool(const mdl::BrushNode&, const mdl::BrushFace&)> faceFilter)
    : m_faceFilter{std::move(faceFilter)}
  {
  }

  RenderSettings markFaces(const mdl::BrushNode& brushNode) const override
  {
    // Marking is transient: every brush renderer marks the faces of a brush right before
    // it reads the marks, so this does not affect the editor's renderers
    auto anyFaceMarked = false;
    for (const auto& face : brushNode.brush().faces())
    {
      const auto marked = !m_faceFilter || m_faceFilter(brushNode, face);
      face.setMarked(marked);
      anyFaceMarked |= marked;
    }

    if (!anyFaceMarked)
    {
      return renderNothing();
    }
    return {FaceRenderPolicy::RenderMarked, EdgeRenderPolicy::RenderAll};
  }
};

/** Draws the links between the entities of a snapshot, see EntityLinkRenderer. */
class SceneEntityLinkRenderer : public render::LinkRenderer
{
private:
  const mdl::EntityLinkManager& m_entityLinkManager;
  std::vector<const mdl::EntityNode*> m_entityNodes;
  std::unordered_set<const mdl::EntityNodeBase*> m_entityNodeSet;

public:
  SceneEntityLinkRenderer(
    const mdl::EntityLinkManager& entityLinkManager,
    std::vector<const mdl::EntityNode*> entityNodes)
    : m_entityLinkManager{entityLinkManager}
    , m_entityNodes{std::move(entityNodes)}
    , m_entityNodeSet{m_entityNodes.begin(), m_entityNodes.end()}
  {
  }

private:
  std::vector<LineVertex> getLinks() override
  {
    const auto color = RgbaF{0.5f, 1.0f, 0.5f, 1.0f}.toVec();

    auto links = std::vector<LineVertex>{};
    for (const auto* sourceNode : m_entityNodes)
    {
      for (const auto& [propertyKey, linkEnds] :
           m_entityLinkManager.linksFrom(*sourceNode))
      {
        for (const auto& linkEnd : linkEnds)
        {
          if (m_entityNodeSet.contains(linkEnd.node))
          {
            links.emplace_back(vm::vec3f{sourceNode->linkSourceAnchor()}, color);
            links.emplace_back(vm::vec3f{linkEnd.node->linkTargetAnchor()}, color);
          }
        }
      }
    }
    return links;
  }
};

/** Sets up the GL state for the map like MapRenderer does. */
class SetupMapGl : public render::Renderable
{
public:
  void render(render::RenderContext& renderContext) override
  {
    auto& gl = renderContext.gl();
    gl.frontFace(GL_CW);
    gl.enable(GL_CULL_FACE);
    gl.enable(GL_DEPTH_TEST);
    gl.depthFunc(GL_LEQUAL);
    gl::glResetEdgeOffset(gl);
  }
};

void setupDefaultRenderer(render::ObjectRenderer& renderer)
{
  // like MapRenderer::setupDefaultRenderer
  renderer.setEntityOverlayTextColor(pref(Preferences::InfoOverlayTextColor));
  renderer.setGroupOverlayTextColor(pref(Preferences::GroupInfoOverlayTextColor));
  renderer.setOverlayBackgroundColor(pref(Preferences::InfoOverlayBackgroundColor));
  renderer.setTint(false);
  renderer.setTransparencyAlpha(pref(Preferences::TransparentFaceAlpha));
  renderer.setGroupBoundsColor(pref(Preferences::DefaultGroupColor));
  renderer.setEntityBoundsColor(pref(Preferences::UndefinedEntityColor));
  renderer.setBrushFaceColor(pref(Preferences::FaceColor));
  renderer.setBrushEdgeColor(pref(Preferences::EdgeColor));
}

void setupHighlightRenderer(render::ObjectRenderer& renderer, const Color& color)
{
  // like MapRenderer::setupSelectionRenderer, with the highlight color
  renderer.setEntityOverlayTextColor(pref(Preferences::SelectedInfoOverlayTextColor));
  renderer.setGroupOverlayTextColor(pref(Preferences::SelectedInfoOverlayTextColor));
  renderer.setOverlayBackgroundColor(
    pref(Preferences::SelectedInfoOverlayBackgroundColor));
  renderer.setShowBrushEdges(true);
  renderer.setShowOccludedObjects(true);
  renderer.setOccludedEdgeColor(
    RgbaF{color.to<RgbF>(), pref(Preferences::OccludedSelectedEdgeAlpha)});
  renderer.setTint(true);
  renderer.setTintColor(color);
  renderer.setOverrideGroupColors(true);
  renderer.setGroupBoundsColor(color);
  renderer.setOverrideEntityBoundsColor(true);
  renderer.setEntityBoundsColor(color);
  renderer.setBrushFaceColor(pref(Preferences::FaceColor));
  renderer.setBrushEdgeColor(color);
}

/**
 * The renderers of one snapshot. They draw the scene's nodes regardless of the editor's
 * hidden state, and never as selected or locked.
 */
struct SceneRenderers
{
  NullLogger logger;
  mdl::EditorContext editorContext;
  render::ObjectRenderer defaultRenderer;
  render::ObjectRenderer highlightRenderer;
  std::unique_ptr<SceneEntityLinkRenderer> entityLinkRenderer;

  SceneRenderers(mdl::Map& map, const mcp::SnapshotRequest& request)
    : defaultRenderer{logger, map.entityModelManager(), editorContext, SceneBrushFilter{request.scene.faceFilter}}
    , highlightRenderer{logger, map.entityModelManager(), editorContext, SceneBrushFilter{request.scene.faceFilter}}
  {
    // The MCP core decided which nodes are visible
    editorContext.setIgnoreHiddenState(true);

    setupDefaultRenderer(defaultRenderer);
    setupHighlightRenderer(highlightRenderer, request.scene.highlightColor);

    const auto highlighted = std::unordered_set<const mdl::Node*>{
      request.scene.highlighted.begin(), request.scene.highlighted.end()};

    auto entityNodes = std::vector<const mdl::EntityNode*>{};
    for (auto* node : request.scene.nodes)
    {
      if (highlighted.contains(node))
      {
        highlightRenderer.addNode(*node);
      }
      else
      {
        defaultRenderer.addNode(*node);
      }

      if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node))
      {
        entityNodes.push_back(entityNode);
      }
    }

    if (request.options.entityLinks)
    {
      entityLinkRenderer = std::make_unique<SceneEntityLinkRenderer>(
        map.entityLinkManager(), std::move(entityNodes));
    }
  }
};

void setupRenderContext(
  render::RenderContext& renderContext, const mcp::SnapshotOptions& options)
{
  const auto perspective = renderContext.camera().perspectiveProjection();

  renderContext.setFilterMode(
    pref(Preferences::TextureMinFilter), pref(Preferences::TextureMagFilter));
  renderContext.setShowMaterials(options.faceMode == mcp::FaceMode::Textured);
  renderContext.setShowFaces(options.faceMode != mcp::FaceMode::Wireframe);
  renderContext.setShowEdges(
    options.edges || options.faceMode == mcp::FaceMode::Wireframe);
  renderContext.setShadeFaces(options.shading);
  renderContext.setShowPointEntities(true);
  renderContext.setShowPointEntityModels(options.entityModels);
  renderContext.setShowEntityClassnames(options.classnames);
  renderContext.setShowGroupBounds(options.bounds);
  renderContext.setShowBrushEntityBounds(options.bounds);
  renderContext.setShowPointEntityBounds(options.bounds);
  renderContext.setShowFog(options.fog && perspective);
  renderContext.setShowGrid(options.grid);
  renderContext.setGridSize(options.gridSize);
  renderContext.setDpiScale(1.0f);
  renderContext.setSoftMapBounds(vm::bbox3f{});
  renderContext.setHideSelectionGuide();
}

void clearBackground(gl::Gl& gl, const Color& color)
{
  const auto backgroundColor = color.to<RgbaF>();
  gl.clearColor(
    backgroundColor.get<ColorChannel::r>(),
    backgroundColor.get<ColorChannel::g>(),
    backgroundColor.get<ColorChannel::b>(),
    1.0f);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void setupGl(gl::Gl& gl, const size_t width, const size_t height)
{
  // like MapViewBase::setupGL
  gl.viewport(0, 0, int(width), int(height));
  gl.enable(GL_MULTISAMPLE);
  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl.shadeModel(GL_SMOOTH);
}

/** Makes the image opaque; translucent draws leave alpha values below 1. */
void makeOpaque(gl::Gl& gl)
{
  gl.pushAttrib(GL_COLOR_BUFFER_BIT);
  gl.colorMask(false, false, false, true);
  gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl.clear(GL_COLOR_BUFFER_BIT);
  gl.popAttrib();
}

void renderOverlays(
  MapDocument& document,
  const mcp::SnapshotRequest& request,
  render::RenderContext& renderContext,
  render::RenderBatch& renderBatch)
{
  const auto& options = request.options;
  auto renderService = render::RenderService{renderContext, renderBatch};

  if (options.axes)
  {
    // like MapViewBase::renderCoordinateSystem
    renderService.renderCoordinateSystem(vm::bbox3f{document.map().worldBounds()});
  }

  if (options.leakPath)
  {
    // like MapViewBase::renderPointFile
    if (const auto* pointTrace = document.pointTrace())
    {
      renderService.setForegroundColor(pref(Preferences::PointFileColor));
      renderService.renderLineStrip(pointTrace->points());
    }
  }

  if (!request.scene.markers.empty())
  {
    renderService.setShowOccludedObjects();
    renderService.setBackgroundColor(pref(Preferences::InfoOverlayBackgroundColor));
    for (const auto& marker : request.scene.markers)
    {
      const auto position = vm::vec3f{marker.position};
      renderService.setForegroundColor(marker.color);
      renderService.renderHandle(position);
      if (!marker.label.empty())
      {
        renderService.setForegroundColor(pref(Preferences::InfoOverlayTextColor));
        renderService.renderString(marker.label, position);
      }
    }
  }
}

void renderScene(
  gl::Gl& gl,
  gl::GlManager& glManager,
  MapDocument& document,
  const mcp::SnapshotRequest& request,
  const gl::Camera& camera)
{
  auto& map = document.map();
  const auto& options = request.options;

  const auto renderMode = camera.perspectiveProjection() ? render::RenderMode::Render3D
                                                         : render::RenderMode::Render2D;
  auto renderContext = render::RenderContext{
    gl, renderMode, camera, glManager.fontManager(), glManager.shaderManager()};
  setupRenderContext(renderContext, options);

  clearBackground(gl, options.background.value_or(pref(Preferences::BackgroundColor)));
  setupGl(gl, request.width, request.height);

  auto renderers = SceneRenderers{map, request};
  renderers.defaultRenderer.setShowOverlays(renderContext.render3D());

  {
    auto renderBatch = render::RenderBatch{glManager.vboManager()};

    if (
      const auto* orthographicCamera =
        dynamic_cast<const gl::OrthographicCamera*>(&camera))
    {
      // like MapView2D::renderGrid
      renderBatch.addOneShot(
        new render::GridRenderer{*orthographicCamera, map.worldBounds()});
    }

    // like MapRenderer::render, without decals, group links and the locked renderer
    renderBatch.addOneShot(new SetupMapGl{});
    if (renderers.entityLinkRenderer)
    {
      renderers.entityLinkRenderer->render(renderContext, renderBatch);
    }
    renderers.defaultRenderer.renderOpaque(renderContext, renderBatch);
    renderers.highlightRenderer.renderOpaque(renderContext, renderBatch);
    renderers.defaultRenderer.renderTransparent(renderContext, renderBatch);
    renderers.highlightRenderer.renderTransparent(renderContext, renderBatch);

    renderOverlays(document, request, renderContext, renderBatch);

    renderBatch.render(renderContext);
  }

  makeOpaque(gl);
}

mcp::RgbaImage toRgbaImage(const QImage& image)
{
  const auto rgbaImage = image.convertToFormat(QImage::Format_RGBA8888);
  const auto width = size_t(rgbaImage.width());
  const auto height = size_t(rgbaImage.height());

  auto result = mcp::RgbaImage{width, height, {}};
  result.pixels.resize(width * height * 4);
  for (size_t y = 0; y < height; ++y)
  {
    const auto* line = rgbaImage.constScanLine(int(y));
    std::copy_n(line, width * 4, result.pixels.begin() + std::ptrdiff_t(y * width * 4));
  }
  return result;
}

QImage toQImage(const mcp::RgbaImage& image)
{
  return QImage{
    image.pixels.data(),
    int(image.width),
    int(image.height),
    int(image.width * 4),
    QImage::Format_RGBA8888}
    .copy();
}

/** "3d" for the 3D view, "xy", "xz" or "yz" for a 2D view by its viewing direction. */
std::string viewId(MapViewBase& mapView)
{
  if (qobject_cast<MapView3D*>(&mapView))
  {
    return "3d";
  }

  switch (vm::find_abs_max_component(mapView.camera().direction()))
  {
  case vm::axis::x:
    return "yz";
  case vm::axis::y:
    return "xz";
  default:
    return "xy";
  }
}

/** The editor's map views of the given window, visible ones first. */
std::vector<MapViewBase*> mapViews(MapWindow& mapWindow)
{
  auto result = mapWindow.mapView().findChildren<MapViewBase*>();
  std::ranges::stable_partition(
    result, [](const auto* mapView) { return mapView->isVisible(); });
  return {result.begin(), result.end()};
}

Result<void> checkImageSize(const size_t width, const size_t height, gl::Gl& gl)
{
  if (width == 0 || height == 0)
  {
    return Error{fmt::format("Invalid image size {}x{}", width, height)};
  }

  auto maxRenderbufferSize = GLint{0};
  auto maxTextureSize = GLint{0};
  GLint maxViewportDims[2] = {0, 0};
  gl.getIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbufferSize);
  gl.getIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  gl.getIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);

  const auto maxWidth = size_t(
    std::min({MaxImageSize, maxRenderbufferSize, maxTextureSize, maxViewportDims[0]}));
  const auto maxHeight = size_t(
    std::min({MaxImageSize, maxRenderbufferSize, maxTextureSize, maxViewportDims[1]}));
  if (width > maxWidth || height > maxHeight)
  {
    return Error{fmt::format(
      "Image size {}x{} exceeds the maximum framebuffer size {}x{}",
      width,
      height,
      maxWidth,
      maxHeight)};
  }

  return kdl::void_success;
}

void processResources(gl::Gl& gl, mdl::Map& map)
{
  auto taskRunner = [&](auto task) {
    return map.taskManager().run_task(std::move(task));
  };
  auto errorHandler = [&](const auto&, const auto& error) {
    map.logger().error() << error;
  };
  auto processContext = gl::ProcessContext{gl, errorHandler};
  map.resourceManager().process(taskRunner, processContext);
}

} // namespace

mcp::AgentCamera toAgentCamera(const gl::Camera& camera)
{
  auto result = mcp::AgentCamera{
    .projection = camera.perspectiveProjection() ? mcp::CameraProjection::Perspective
                                                 : mcp::CameraProjection::Orthographic,
    .position = vm::vec3d{camera.position()},
    .direction = vm::vec3d{camera.direction()},
    .up = vm::vec3d{camera.up()},
    .nearPlane = double(camera.nearPlane()),
    .farPlane = double(camera.farPlane()),
  };

  if (const auto* perspectiveCamera = dynamic_cast<const gl::PerspectiveCamera*>(&camera))
  {
    result.fov = double(perspectiveCamera->zoomedFov());
  }
  else
  {
    result.zoom = double(camera.zoom());
  }
  return result;
}

McpSnapshotRenderer::McpSnapshotRenderer(
  gl::GlManager& glManager, FindMapWindow findMapWindow)
  : m_glManager{glManager}
  , m_findMapWindow{std::move(findMapWindow)}
{
}

McpSnapshotRenderer::~McpSnapshotRenderer()
{
  // Destroy the context while no other context is current, and restore the current one
  const auto currentContext = ScopedCurrentContext{};
  m_context.reset();
  m_surface.reset();
}

Result<void> McpSnapshotRenderer::createContext()
{
  if (m_context)
  {
    return kdl::void_success;
  }

  if (!qGuiApp)
  {
    return Error{"OpenGL is not available without a GUI application"};
  }

  auto* shareContext = QOpenGLContext::globalShareContext();
  auto format = QSurfaceFormat::defaultFormat();
  if (shareContext)
  {
    format = shareContext->format();
  }
  else
  {
    // Like the editor's default format, see Main.cpp
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
  }

  auto surface = std::make_unique<QOffscreenSurface>();
  surface->setFormat(format);
  surface->create();
  if (!surface->isValid())
  {
    return Error{"Failed to create an offscreen surface for OpenGL rendering"};
  }

  auto context = std::make_unique<QOpenGLContext>();
  context->setFormat(format);
  context->setShareContext(shareContext);
  if (!context->create())
  {
    return Error{"Failed to create an OpenGL context"};
  }
  if (context->isOpenGLES())
  {
    return Error{"OpenGL ES is not supported"};
  }

  m_surface = std::move(surface);
  m_context = std::move(context);
  return kdl::void_success;
}

bool McpSnapshotRenderer::resourcesPending(MapDocument& document)
{
  auto& map = document.map();
  if (createContext().is_success())
  {
    const auto currentContext = ScopedCurrentContext{};
    if (m_context->makeCurrent(m_surface.get()))
    {
      if (
        auto* functions =
          QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_2_1>(m_context.get()))
      {
        auto gl = GlQt{*functions};
        processResources(gl, map);
      }
    }
  }

  return map.resourceManager().needsProcessing();
}

Result<mcp::RgbaImage> McpSnapshotRenderer::render(
  MapDocument& document, const mcp::SnapshotRequest& request)
{
  return mcp::makeGlCamera(request.camera, request.width, request.height)
         | kdl::and_then([&](const auto& camera) {
             return createContext() | kdl::and_then([&]() {
                      return renderInContext(document, request, *camera);
                    });
           });
}

Result<mcp::RgbaImage> McpSnapshotRenderer::renderInContext(
  MapDocument& document, const mcp::SnapshotRequest& request, const gl::Camera& camera)
{
  const auto currentContext = ScopedCurrentContext{};
  if (!m_context->makeCurrent(m_surface.get()))
  {
    return Error{"Failed to make the OpenGL context current"};
  }

  auto* functions =
    QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_2_1>(m_context.get());
  if (!functions)
  {
    return Error{"OpenGL 2.1 is not available"};
  }

  auto gl = GlQt{*functions};
  if (!m_glManager.initialized())
  {
    try
    {
      m_glManager.initialize(gl);
    }
    catch (const std::exception& e)
    {
      return Error{fmt::format("Failed to initialize OpenGL: {}", e.what())};
    }
  }

  return checkImageSize(request.width, request.height, gl)
         | kdl::and_then([&]() -> Result<mcp::RgbaImage> {
             processResources(gl, document.map());

             auto format = QOpenGLFramebufferObjectFormat{};
             format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
             format.setSamples(Samples);

             auto framebuffer = QOpenGLFramebufferObject{
               QSize{int(request.width), int(request.height)}, format};
             if (!framebuffer.isValid() || !framebuffer.bind())
             {
               return Error{"Failed to create an OpenGL framebuffer"};
             }

             renderScene(gl, m_glManager, document, request, camera);
             auto image = toRgbaImage(framebuffer.toImage());
             framebuffer.release();

             m_glManager.vboManager().destroyPendingVbos(gl);
             m_glManager.fontManager().destroyPendingFonts(gl);
             return image;
           });
}

std::vector<mcp::UserView> McpSnapshotRenderer::userViews(MapDocument& document)
{
  auto* mapWindow = m_findMapWindow ? m_findMapWindow(document) : nullptr;
  if (!mapWindow)
  {
    return {};
  }

  auto result = std::vector<mcp::UserView>{};
  for (auto* mapView : mapViews(*mapWindow))
  {
    auto id = viewId(*mapView);
    if (std::ranges::none_of(result, [&](const auto& view) { return view.id == id; }))
    {
      const auto ratio = mapView->devicePixelRatioF();
      result.push_back(mcp::UserView{
        .id = std::move(id),
        .visible = mapView->isVisible(),
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

Result<mcp::RgbaImage> McpSnapshotRenderer::captureUserView(
  MapDocument& document, const std::string& id)
{
  auto* mapWindow = m_findMapWindow ? m_findMapWindow(document) : nullptr;
  if (!mapWindow)
  {
    return Error{"The document has no window"};
  }

  const auto views = mapViews(*mapWindow);
  const auto it =
    std::ranges::find_if(views, [&](auto* mapView) { return viewId(*mapView) == id; });
  if (it == views.end())
  {
    return Error{fmt::format("The document's window has no view '{}'", id)};
  }

  auto* mapView = *it;
  if (!mapView->isVisible() || mapView->width() <= 0 || mapView->height() <= 0)
  {
    return Error{fmt::format("The view '{}' is not visible", id)};
  }

  const auto currentContext = ScopedCurrentContext{};
  const auto image = mapView->grabFramebuffer();
  if (image.isNull())
  {
    return Error{fmt::format("Failed to capture the view '{}'", id)};
  }
  return toRgbaImage(image);
}

std::optional<std::string> McpSnapshotRenderer::encodeJpeg(
  const mcp::RgbaImage& image, const int quality)
{
  if (
    image.width == 0 || image.height == 0
    || image.pixels.size() != image.width * image.height * 4)
  {
    return std::nullopt;
  }

  auto bytes = QByteArray{};
  auto buffer = QBuffer{&bytes};
  buffer.open(QIODevice::WriteOnly);

  const auto qImage = toQImage(image).convertToFormat(QImage::Format_RGB888);
  if (!qImage.save(&buffer, "JPG", std::clamp(quality, 1, 100)))
  {
    return std::nullopt;
  }
  return bytes.toStdString();
}

} // namespace tb::ui
