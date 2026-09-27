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

#include "mcp/Host.h"

#include <functional>
#include <memory>

namespace tb
{
namespace gl
{
class PerspectiveCamera;
}

namespace ui
{
class MapDocument;

/** Returns a new camera with the same projection, viewport, position and orientation. */
std::unique_ptr<gl::PerspectiveCamera> copyPerspectiveCamera(
  const gl::PerspectiveCamera& camera);

/**
 * Runs compilation profiles for the MCP server with the editor's compilation runner
 * (CompilationRun), like the compilation dialog does.
 *
 * Each job collects the runner's output in a hidden text widget and keeps a copy of the
 * camera that export tasks use to place an entity at the camera position. A job is
 * cancelled when its document is reloaded, because the runner refers to the document's
 * map.
 */
class McpCompileHost : public mcp::CompileHost
{
public:
  /**
   * Returns the camera of the given document's 3D view, or nullptr to use a default
   * camera.
   */
  using CameraProvider =
    std::function<std::unique_ptr<gl::PerspectiveCamera>(const MapDocument&)>;

private:
  CameraProvider m_cameraProvider;

public:
  explicit McpCompileHost(CameraProvider cameraProvider = {});
  ~McpCompileHost() override;

  Result<std::unique_ptr<mcp::CompileJob>> startCompile(
    MapDocument& document,
    const mdl::CompilationProfile& profile,
    bool test,
    mcp::CompileJobCallbacks callbacks) override;
};

} // namespace ui
} // namespace tb
