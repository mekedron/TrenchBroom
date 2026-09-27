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

#pragma once

#include "base/Result.h"
#include "mcp/AgentCamera.h"
#include "mcp/Snapshot.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QOffscreenSurface;
class QOpenGLContext;

namespace tb
{
namespace gl
{
class Camera;
class GlManager;
} // namespace gl

namespace ui
{
class MapDocument;
class MapWindow;

/**
 * Returns an editor camera for the given agent camera that renders an image of the given
 * size: a perspective camera with the agent camera's field of view, or an orthographic
 * camera whose zoom is the agent camera's pixels per map unit. Fails if the camera or the
 * size is invalid.
 */
Result<std::unique_ptr<gl::Camera>> makeGlCamera(
  const mcp::AgentCamera& camera, size_t width, size_t height);

/**
 * Returns an agent camera with the projection, position, orientation, clipping planes
 * and effective field of view (perspective) or zoom (orthographic) of the given editor
 * camera.
 */
mcp::AgentCamera toAgentCamera(const gl::Camera& camera);

/**
 * Renders snapshots of a document into an offscreen framebuffer with its own OpenGL
 * context, which shares the editor's resources (materials, entity models, shaders) when
 * the application has a global share context.
 *
 * Every snapshot uses its own renderers, camera, editor context and render settings
 * built from the request, so it never changes what the editor's views show: the
 * document's map renderer, editor context, selection, cameras, view filters and
 * preferences are left alone. The renderer only depends on the GL manager and the
 * document, so it works without a map window.
 *
 * The editor's views of a document (userViews, captureUserView) are found with the given
 * function; without one, a document has no views.
 */
class McpSnapshotRenderer : public mcp::SnapshotRenderer
{
public:
  using FindMapWindow = std::function<MapWindow*(const MapDocument&)>;

private:
  gl::GlManager& m_glManager;
  FindMapWindow m_findMapWindow;
  std::unique_ptr<QOffscreenSurface> m_surface;
  std::unique_ptr<QOpenGLContext> m_context;

public:
  explicit McpSnapshotRenderer(
    gl::GlManager& glManager, FindMapWindow findMapWindow = {});
  ~McpSnapshotRenderer() override;

  /**
   * Uploads the document's loaded resources and returns whether some are still being
   * loaded or waiting to be uploaded.
   */
  bool resourcesPending(MapDocument& document) override;

  Result<mcp::RgbaImage> render(
    MapDocument& document, const mcp::SnapshotRequest& request) override;

  std::vector<mcp::UserView> userViews(MapDocument& document) override;

  Result<mcp::RgbaImage> captureUserView(
    MapDocument& document, const std::string& viewId) override;

  std::optional<std::string> encodeJpeg(
    const mcp::RgbaImage& image, int quality) override;

private:
  Result<void> createContext();
  Result<mcp::RgbaImage> renderInContext(
    MapDocument& document, const mcp::SnapshotRequest& request, const gl::Camera& camera);
};

} // namespace ui
} // namespace tb
