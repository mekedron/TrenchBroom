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

#include "base/Color.h"
#include "base/Result.h"
#include "mcp/AgentCamera.h"
#include "mcp/Image.h"

#include "vm/vec.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tb
{
namespace mdl
{
class BrushFace;
class BrushNode;
class Node;
} // namespace mdl

namespace ui
{
class MapDocument;
}
} // namespace tb

namespace tb::mcp
{

enum class FaceMode
{
  /** Faces with their materials. */
  Textured,
  /** Faces in a flat color. */
  Flat,
  /** No faces, only edges. */
  Wireframe,
};

/**
 * How a snapshot is drawn. Which objects are drawn is decided by SnapshotScene; these
 * options never affect the editor's own views or preferences.
 */
struct SnapshotOptions
{
  FaceMode faceMode = FaceMode::Textured;
  /** Shade faces by their orientation, like the editor's "Shade faces" option. */
  bool shading = true;
  /** Fog that fades distant geometry (perspective cameras only). */
  bool fog = false;
  /** Draw brush edges. Wireframe mode always draws edges. */
  bool edges = true;
  /** Draw the models of point entities; entities without a model are drawn as boxes. */
  bool entityModels = true;
  /** Draw the bounding boxes of point entities, brush entities and groups. */
  bool bounds = false;
  /** Draw the classnames of entities. */
  bool classnames = false;
  /** Draw lines between linked entities (target / targetname and similar). */
  bool entityLinks = false;
  /** Draw the leak path of the document's loaded point file, if any. */
  bool leakPath = false;
  /** Draw the grid: grid lines on faces in 3D, the background grid in 2D. */
  bool grid = false;
  double gridSize = 16.0;
  /** Draw the coordinate axes through the origin. */
  bool axes = false;
  /** The background color; nullopt uses the editor's background color preference. */
  std::optional<Color> background;

  bool operator==(const SnapshotOptions&) const = default;
};

/** A marker drawn on top of the scene, e.g. an entity in a plan view. */
struct SnapshotMarker
{
  vm::vec3d position;
  Color color;
  /** Drawn next to the marker; may be empty. */
  std::string label;
};

/**
 * The objects that a snapshot draws. The MCP core resolves the agent's visibility
 * options into this list, so the renderer does not need the editor's visibility state.
 *
 * The node pointers are only valid during SnapshotRenderer::render.
 */
struct SnapshotScene
{
  /**
   * The objects to draw: brushes, patches, point entities, brush entities (for their
   * bounds and classnames; their brushes are listed separately) and groups (for their
   * bounds). They are drawn even if they are hidden or locked in the editor, and they
   * are never drawn as selected.
   */
  std::vector<mdl::Node*> nodes;
  /**
   * Decides whether a face of a listed brush is drawn, e.g. to hide faces whose smart
   * tag (clip, skip, hint, ...) is hidden. Null draws all faces.
   */
  std::function<bool(const mdl::BrushNode&, const mdl::BrushFace&)> faceFilter;
  /** Objects drawn tinted in highlightColor. Each of them is also listed in `nodes`. */
  std::vector<mdl::Node*> highlighted;
  Color highlightColor = RgbaF{1.0f, 0.5f, 0.0f, 1.0f};
  std::vector<SnapshotMarker> markers;
};

/** One image to render. */
struct SnapshotRequest
{
  AgentCamera camera;
  /** The image size in pixels. */
  size_t width = 1024;
  size_t height = 768;
  SnapshotOptions options;
  SnapshotScene scene;
};

/** One of the editor views of a document's window, see SnapshotRenderer::userViews. */
struct UserView
{
  /** "3d", "xy", "xz" or "yz". */
  std::string id;
  /** Whether the view is currently shown in the window's layout. */
  bool visible = false;
  /** The view's size in pixels. */
  size_t width = 0;
  size_t height = 0;
  /** The view's current camera (a copy; changing it changes nothing in the editor). */
  AgentCamera camera;
};

/**
 * Renders images of a document offscreen, independent of the editor's map windows and
 * without changing anything the user sees: no camera, filter, visibility, selection or
 * preference of the editor is changed. Implemented by the editor
 * (ui::McpSnapshotRenderer); tests use FakeSnapshotRenderer.
 *
 * All functions are called on the main thread.
 */
class SnapshotRenderer
{
public:
  virtual ~SnapshotRenderer();

  /**
   * Whether materials or entity models of the document are still being loaded or
   * uploaded, so that an image rendered now would miss some of them. The default
   * implementation returns false.
   */
  virtual bool resourcesPending(ui::MapDocument& document);

  /**
   * Renders the given scene into an image of the requested size. Fails if no OpenGL
   * context is available.
   */
  virtual Result<RgbaImage> render(
    ui::MapDocument& document, const SnapshotRequest& request) = 0;

  /**
   * The editor views of the document's map window (3D and 2D views), or an empty list if
   * the document has no window.
   */
  virtual std::vector<UserView> userViews(ui::MapDocument& document) = 0;

  /**
   * Captures the image that the given editor view currently shows, without changing the
   * view. Fails if the view does not exist or is not visible.
   */
  virtual Result<RgbaImage> captureUserView(
    ui::MapDocument& document, const std::string& viewId) = 0;

  /**
   * Encodes the image as JPEG with the given quality (1-100), or returns nullopt if the
   * renderer cannot encode JPEG. The default implementation returns nullopt.
   */
  virtual std::optional<std::string> encodeJpeg(const RgbaImage& image, int quality);
};

} // namespace tb::mcp
