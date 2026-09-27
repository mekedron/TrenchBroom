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

#include "mcp/tools/ViewTools.h"

#include "ToolUtils.h"
#include "base/PreferenceManager.h"
#include "mcp/AgentCamera.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Host.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Snapshot.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/EditorContext.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionGroup.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/PointTrace.h"
#include "mdl/Selection.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include "kd/string_compare.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

struct GridState
{
  int exponent;
  bool visible;
  bool snap;
};

GridState gridState(const mdl::Grid& grid)
{
  return {grid.size(), grid.visible(), grid.snap()};
}

Json toJson(const GridState& state)
{
  const auto size = mdl::Grid::actualSize(state.exponent);
  return Json{
    {"size", size},
    {"exponent", state.exponent},
    {"effectiveSize", state.snap ? size : 1.0},
    {"visible", state.visible},
    {"snap", state.snap},
    {"angle", 15.0},
  };
}

std::string validSizes()
{
  auto result = std::string{};
  for (auto exponent = mdl::Grid::MinSize; exponent <= mdl::Grid::MaxSize; ++exponent)
  {
    const auto size = mdl::Grid::actualSize(exponent);
    auto text = std::to_string(size);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.')
    {
      text.pop_back();
    }
    result += (result.empty() ? "" : ", ") + text;
  }
  return result;
}

Schema gridSchema()
{
  return object({
    field("size", number()).required().describe("Grid size in map units"),
    field("exponent", integer()).required().describe("size = 2^exponent"),
    field("effectiveSize", number())
      .required()
      .describe("The size tools snap to: size, or 1 if snapping is off"),
    field("visible", boolean()).required(),
    field("snap", boolean()).required(),
    field("angle", number()).required().describe("Rotation snap angle in degrees"),
  });
}

ToolResult gridGet(CallContext& context, const Args&)
{
  return toJson(gridState(context.map().grid()));
}

ToolResult gridSet(CallContext& context, const Args& args)
{
  auto& grid = context.map().grid();
  const auto size = args.getOptional<double>("size");
  const auto exponent = args.getOptional<int>("exponent");
  const auto visible = args.getOptional<bool>("visible");
  const auto snap = args.getOptional<bool>("snap");

  if (size && exponent)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either 'size' or 'exponent', not both.",
      "E.g. {\"size\": 16} or {\"exponent\": 4}.");
  }
  if (!size && !exponent && !visible && !snap)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass at least one of 'size', 'exponent', 'visible' and 'snap'.");
  }

  const auto previous = gridState(grid);
  auto next = previous;
  if (size)
  {
    const auto e = std::log2(*size);
    if (
      !(*size > 0.0) || std::abs(e - std::round(e)) > 1e-9 || e < mdl::Grid::MinSize
      || e > mdl::Grid::MaxSize)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Grid size " + std::to_string(*size) + " is not a valid grid size.",
        "Use a power of two: " + validSizes() + ".");
    }
    next.exponent = int(std::round(e));
  }
  if (exponent)
  {
    next.exponent = *exponent;
  }
  next.visible = visible.value_or(previous.visible);
  next.snap = snap.value_or(previous.snap);

  if (!context.dryRun())
  {
    if (next.exponent != previous.exponent)
    {
      grid.setSize(next.exponent);
    }
    if (next.visible != grid.visible())
    {
      grid.toggleVisible();
    }
    if (next.snap != grid.snap())
    {
      grid.toggleSnap();
    }
  }

  auto result = toJson(next);
  result["previous"] = toJson(previous);
  return result;
}

// User views and cameras (camera_*, view_layout_set)

using mcp::toJson;

const auto ViewIdList = std::vector<std::string>{"3d", "xy", "xz", "yz"};

/** The box that camera_focus frames around a point. */
constexpr auto PointFocusSize = 128.0;

/** The 3D camera placement above a point file point, like MapView3D. */
constexpr auto TracePointHeight = 16.0;

/** The zoom range of the editor's cameras (gl::Camera::isValidZoom). */
constexpr auto MinViewZoom = 0.02;
constexpr auto MaxViewZoom = 100.0;

ToolError noViewHostError()
{
  return makeError(
    ErrorCode::UnsupportedInHost,
    "This host has no editor views.",
    "The user camera, view layout and view tools need the TrenchBroom editor; use "
    "agent cameras (agent_camera_set, view_snapshot) to look at the map instead.");
}

Result<ViewHost*, ToolError> requireViewHost(CallContext& context)
{
  if (auto* viewHost = context.host().viewHost())
  {
    return viewHost;
  }
  return noViewHostError();
}

Result<std::vector<UserView>, ToolError> userViews(
  CallContext& context, ViewHost& viewHost)
{
  auto views = viewHost.views(context.document());
  if (views.empty())
  {
    return makeError(
      ErrorCode::OperationFailed,
      "The document has no map window with views.",
      "Views exist only for documents shown in an editor window; use agent cameras "
      "(view_snapshot) for this document.");
  }
  return views;
}

const UserView* findView(const std::vector<UserView>& views, const std::string& id)
{
  const auto it =
    std::ranges::find_if(views, [&](const auto& view) { return view.id == id; });
  return it != views.end() ? &*it : nullptr;
}

ToolError unknownViewError(const std::vector<UserView>& views, const std::string& id)
{
  auto known = std::string{};
  for (const auto& view : views)
  {
    known += (known.empty() ? "" : ", ") + view.id;
  }
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("The document's window has no view '{}'.", id),
    fmt::format("Views: {}.", known));
}

Json viewJson(const UserView& view)
{
  return Json{
    {"id", view.id},
    {"visible", view.visible},
    {"width", view.width},
    {"height", view.height},
    {"camera", toJson(view.camera)},
  };
}

Json optionalJson(const std::optional<std::string>& value)
{
  return value ? Json(*value) : Json(nullptr);
}

int panes()
{
  return std::clamp(pref(Preferences::MapViewLayout), 0, 3) + 1;
}

Json layoutJson(const int paneCount, const ViewLayout& layout)
{
  return Json{
    {"panes", paneCount},
    {"maximizedView", optionalJson(layout.maximizedView)},
    {"currentView", optionalJson(layout.currentView)},
  };
}

Schema cameraSchema()
{
  return object({
    field("projection", enumOf({"perspective", "orthographic"})).required(),
    field("position", vec3())
      .required()
      .describe("Eye position (3D) or view center (2D; the axis coordinate is unused)"),
    field("direction", vec3()).required(),
    field("up", vec3()).required(),
    field("yaw", number()).describe("Degrees, counterclockwise from +x"),
    field("pitch", number()).describe("Degrees, positive looks up"),
    field("fov", number()).describe("3D: field of view in degrees"),
    field("view", any()).describe("2D: top, front or side"),
    field("zoom", number()).describe("2D: screen pixels per map unit"),
    field("near", number()),
    field("far", number()),
  });
}

Schema viewSchema()
{
  return object({
    field("id", enumOf(ViewIdList)).required(),
    field("visible", boolean())
      .required()
      .describe("Shown in the current layout (not hidden by the layout or maximizing)"),
    field("width", integer()).required().describe("Size in pixels"),
    field("height", integer()).required(),
    field("camera", cameraSchema()).required(),
  });
}

Schema layoutSchema()
{
  return object({
    field("panes", integer()).required().describe("1 to 4, a global preference"),
    field("maximizedView", any()).required().describe("3d, xy, xz, yz or null"),
    field("currentView", any())
      .required()
      .describe("The view that has (or last had) the keyboard focus, or null"),
  });
}

Schema viewIdSchema()
{
  return enumOf(ViewIdList);
}

// camera_get

ToolResult cameraGet(CallContext& context, const Args&)
{
  auto viewHost = requireViewHost(context);
  if (viewHost.is_error())
  {
    return errorOf(viewHost);
  }
  auto views = userViews(context, *viewHost.value());
  if (views.is_error())
  {
    return errorOf(views);
  }
  auto layout = viewHost.value()->layout(context.document());
  if (layout.is_error())
  {
    return context.operationFailed(errorMessage(layout));
  }

  auto viewsJson = Json::array();
  for (const auto& view : views.value())
  {
    viewsJson.push_back(viewJson(view));
  }

  auto result = Json{
    {"views", std::move(viewsJson)},
    {"layout", layoutJson(panes(), layout.value())},
    {"link2dCameras", pref(Preferences::Link2DCameras)},
    {"cameraFovPreference", roundForOutput(double(pref(Preferences::CameraFov)))},
  };
  if (const auto* trace = context.document().pointTrace())
  {
    auto first = *trace;
    auto index = size_t(0);
    while (first.hasPreviousPoint())
    {
      first.retreat();
      ++index;
    }
    result["pointFile"] = Json{{"index", index}, {"count", trace->points().size()}};
  }
  return result;
}

// camera_set

/** The views whose camera differs between the two lists, except `except`. */
Json changedViews(
  const std::vector<UserView>& before,
  const std::vector<UserView>& after,
  const std::string& except)
{
  auto result = Json::array();
  for (const auto& view : after)
  {
    if (view.id == except)
    {
      continue;
    }
    const auto* old = findView(before, view.id);
    if (!old || !(old->camera == view.camera))
    {
      result.push_back(Json{{"id", view.id}, {"camera", toJson(view.camera)}});
    }
  }
  return result;
}

Result<AgentCamera, ToolError> perspectiveCameraFromArgs(
  AgentCamera camera, const Args& args)
{
  if (args.has("zoom"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "zoom is only used by the 2D views.",
      "Use fov for the 3D view, or pass view: \"xy\", \"xz\" or \"yz\".");
  }

  const auto hasLookAt = args.has("lookAt");
  const auto hasDirection = args.has("direction");
  const auto hasAngles = args.has("yaw") || args.has("pitch");
  if (int(hasLookAt) + int(hasDirection) + int(hasAngles) > 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass only one of lookAt, direction and yaw/pitch.",
      "lookAt aims at a point, direction is a vector, yaw/pitch are angles.");
  }

  const auto current = anglesFromCamera(camera.direction, camera.up);
  auto yaw = current.yaw;
  camera.position = args.getOr("position", camera.position);

  auto direction = std::optional<vm::vec3d>{};
  if (hasLookAt)
  {
    const auto target = args.get<vm::vec3d>("lookAt");
    if (vm::length(target - camera.position) < 1e-6)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "lookAt must differ from the camera position.",
        "Pass a target away from the camera.");
    }
    direction = vm::normalize(target - camera.position);
  }
  else if (hasDirection)
  {
    const auto value = args.get<vm::vec3d>("direction");
    if (vm::length(value) < 1e-9)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "direction must not be zero.",
        "Pass a direction such as [1, 0, 0].");
    }
    direction = vm::normalize(value);
  }
  else if (hasAngles)
  {
    yaw = args.getOr("yaw", current.yaw);
    direction = directionFromAngles(yaw, args.getOr("pitch", current.pitch));
  }

  if (direction)
  {
    camera.direction = *direction;
  }

  if (const auto up = args.getOptional<vm::vec3d>("up"))
  {
    const auto orthogonal = *up - vm::dot(*up, camera.direction) * camera.direction;
    if (vm::length(orthogonal) < 1e-6)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "up must not be parallel to the view direction.",
        "Omit up to keep the camera upright.");
    }
    camera.up = vm::normalize(orthogonal);
  }
  else if (direction)
  {
    camera.up = upVector(camera.direction, yaw);
  }

  if (const auto fov = args.getOptional<double>("fov"))
  {
    camera.fov = *fov;
  }
  return camera;
}

Result<AgentCamera, ToolError> orthographicCameraFromArgs(
  AgentCamera camera, const Args& args)
{
  for (const auto* key : {"lookAt", "direction", "yaw", "pitch", "up", "fov"})
  {
    if (args.has(key))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("{} is not used by the 2D views; their direction is fixed.", key),
        "2D views take position (the new view center) and zoom.");
    }
  }

  if (const auto position = args.getOptional<vm::vec3d>("position"))
  {
    const auto& axis = camera.direction;
    camera.position =
      *position - vm::dot(*position, axis) * axis + vm::dot(camera.position, axis) * axis;
  }
  if (const auto zoom = args.getOptional<double>("zoom"))
  {
    camera.zoom = *zoom;
  }
  return camera;
}

ToolResult cameraSet(CallContext& context, const Args& args)
{
  auto viewHost = requireViewHost(context);
  if (viewHost.is_error())
  {
    return errorOf(viewHost);
  }

  const auto viewId = args.getOr<std::string>("view", "3d");
  if (!std::ranges::any_of(
        std::array{
          "position", "lookAt", "direction", "yaw", "pitch", "up", "fov", "zoom"},
        [&](const auto* key) { return args.has(key); }))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass position, an orientation (lookAt, direction or yaw/pitch), up or fov for "
      "the 3D view, or position and zoom for a 2D view.");
  }

  auto before = userViews(context, *viewHost.value());
  if (before.is_error())
  {
    return errorOf(before);
  }
  const auto* view = findView(before.value(), viewId);
  if (!view)
  {
    return unknownViewError(before.value(), viewId);
  }

  const auto previous = view->camera;
  auto camera = previous.projection == CameraProjection::Perspective
                  ? perspectiveCameraFromArgs(previous, args)
                  : orthographicCameraFromArgs(previous, args);
  if (camera.is_error())
  {
    return errorOf(camera);
  }

  auto result = Json{
    {"view", viewId},
    {"camera", toJson(camera.value())},
    {"previous", toJson(previous)},
    {"linkedViews", Json::array()},
  };
  if (context.dryRun())
  {
    return result;
  }

  if (auto set = viewHost.value()->setCamera(context.document(), viewId, camera.value());
      set.is_error())
  {
    return context.operationFailed(errorMessage(set));
  }

  const auto after = viewHost.value()->views(context.document());
  if (const auto* changed = findView(after, viewId))
  {
    result["camera"] = toJson(changed->camera);
  }
  result["linkedViews"] = changedViews(before.value(), after, viewId);
  return result;
}

// camera_focus

Result<vm::bbox3d, ToolError> focusBounds(CallContext& context, const Args& args)
{
  const auto sources =
    int(args.has("ids")) + int(args.has("box")) + int(args.has("point"));
  if (sources > 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass only one of ids, box and point.",
      "Without any of them the camera focuses on the selection.");
  }

  if (const auto box = args.getOptional<vm::bbox3d>("box"))
  {
    return *box;
  }
  if (const auto point = args.getOptional<vm::vec3d>("point"))
  {
    return vm::bbox3d{*point, *point}.expand(PointFocusSize / 2.0);
  }

  auto result = std::optional<vm::bbox3d>{};
  const auto add = [&](const vm::bbox3d& bounds) {
    result = result ? vm::merge(*result, bounds) : bounds;
  };

  if (const auto ids = args.getOptional<std::vector<std::string>>("ids"))
  {
    for (const auto& id : *ids)
    {
      if (id.find("/face:") != std::string::npos)
      {
        auto face = resolveFace(context, id);
        if (face.is_error())
        {
          return errorOf(face);
        }
        add(face.value().face().bounds());
      }
      else
      {
        auto node = context.ids().resolve(id);
        if (node.is_error())
        {
          return errorOf(node);
        }
        add(node.value()->logicalBounds());
      }
    }
    if (!result)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "ids must not be empty.",
        "Omit ids to focus on the selection.");
    }
    return *result;
  }

  const auto& map = context.map();
  if (const auto& bounds = map.selectionBounds())
  {
    return *bounds;
  }
  for (const auto& handle : map.selection().brushFaces)
  {
    add(handle.face().bounds());
  }
  if (!result)
  {
    return makeError(
      ErrorCode::NoSelection,
      "Nothing is selected.",
      "Pass ids, a box or a point, or select objects first.");
  }
  return *result;
}

size_t sizeOr(const size_t size, const size_t defaultSize)
{
  return size > 0 ? size : defaultSize;
}

/**
 * Frames the box in the 3D view keeping its direction, and centers the 2D views on it
 * keeping their zoom unless the box does not fit (then they zoom out just enough; linked
 * 2D cameras share the smallest zoom).
 */
std::vector<std::pair<std::string, AgentCamera>> focusCameras(
  const std::vector<UserView>& views,
  const std::vector<std::string>& viewIds,
  const vm::bbox3d& box,
  const double margin)
{
  auto result = std::vector<std::pair<std::string, AgentCamera>>{};
  for (const auto& view : views)
  {
    if (std::ranges::find(viewIds, view.id) == viewIds.end())
    {
      continue;
    }
    const auto width = sizeOr(view.width, DefaultImageWidth);
    const auto height = sizeOr(view.height, DefaultImageHeight);
    auto camera = view.camera;
    if (camera.projection == CameraProjection::Perspective)
    {
      const auto framed = frameBox(camera, box, width, height, margin);
      camera.position = framed.position;
    }
    else
    {
      const auto framed = frameBox(camera, box, width, height, margin);
      camera.position = framed.position;
      camera.zoom =
        std::clamp(std::min(camera.zoom, framed.zoom), MinViewZoom, MaxViewZoom);
    }
    result.emplace_back(view.id, camera);
  }

  if (pref(Preferences::Link2DCameras))
  {
    auto zoom = std::optional<double>{};
    for (const auto& [id, camera] : result)
    {
      if (camera.projection == CameraProjection::Orthographic)
      {
        zoom = zoom ? std::min(*zoom, camera.zoom) : camera.zoom;
      }
    }
    for (auto& [id, camera] : result)
    {
      if (camera.projection == CameraProjection::Orthographic)
      {
        camera.zoom = *zoom;
      }
    }
  }
  return result;
}

ToolResult cameraFocus(CallContext& context, const Args& args)
{
  auto viewHost = requireViewHost(context);
  if (viewHost.is_error())
  {
    return errorOf(viewHost);
  }
  auto views = userViews(context, *viewHost.value());
  if (views.is_error())
  {
    return errorOf(views);
  }

  const auto viewIds = args.getOr("views", ViewIdList);
  for (const auto& id : viewIds)
  {
    if (!findView(views.value(), id))
    {
      return unknownViewError(views.value(), id);
    }
  }

  auto box = focusBounds(context, args);
  if (box.is_error())
  {
    return errorOf(box);
  }

  const auto cameras =
    focusCameras(views.value(), viewIds, box.value(), args.getOr("margin", 1.1));
  if (!context.dryRun())
  {
    for (const auto& [id, camera] : cameras)
    {
      if (auto set = viewHost.value()->setCamera(context.document(), id, camera);
          set.is_error())
      {
        return context.operationFailed(errorMessage(set));
      }
    }
  }

  const auto after =
    context.dryRun() ? views.value() : viewHost.value()->views(context.document());
  auto viewsJson = Json::array();
  for (const auto& [id, camera] : cameras)
  {
    const auto* previous = findView(views.value(), id);
    const auto* current = findView(after, id);
    viewsJson.push_back(Json{
      {"id", id},
      {"camera", toJson(context.dryRun() || !current ? camera : current->camera)},
      {"previous", toJson(previous->camera)},
    });
  }
  return Json{
    {"box", toJson(box.value())},
    {"views", std::move(viewsJson)},
  };
}

// camera_step_pointfile

size_t traceIndex(mdl::PointTrace trace)
{
  auto index = size_t(0);
  while (trace.hasPreviousPoint())
  {
    trace.retreat();
    ++index;
  }
  return index;
}

void stepTrace(mdl::PointTrace& trace, const std::string& direction)
{
  if (direction == "next")
  {
    trace.advance();
  }
  else if (direction == "previous")
  {
    trace.retreat();
  }
  else if (direction == "first")
  {
    while (trace.hasPreviousPoint())
    {
      trace.retreat();
    }
  }
  else if (direction == "last")
  {
    while (trace.hasNextPoint())
    {
      trace.advance();
    }
  }
}

ToolResult cameraStepPointfile(CallContext& context, const Args& args)
{
  auto viewHost = requireViewHost(context);
  if (viewHost.is_error())
  {
    return errorOf(viewHost);
  }
  auto* trace = context.document().pointTrace();
  if (!trace)
  {
    return makeError(
      ErrorCode::OperationFailed,
      "No point file is loaded.",
      "Load the leak path with pointfile_load (compile_status names the point file "
      "after a leak).");
  }
  auto views = userViews(context, *viewHost.value());
  if (views.is_error())
  {
    return errorOf(views);
  }

  const auto direction = args.getOr<std::string>("direction", "next");
  const auto oldIndex = traceIndex(*trace);
  auto stepped = *trace;
  stepTrace(stepped, direction);
  const auto index = traceIndex(stepped);
  if (index == oldIndex && direction != "current")
  {
    context.warn(
      "END_OF_TRACE",
      fmt::format(
        "The camera is already at the {} point of the point file.",
        direction == "next" || direction == "last" ? "last" : "first"));
  }

  // Like MapView3D::moveCameraToCurrentTracePoint, without the animation
  const auto point = vm::vec3d{stepped.currentPoint()};
  const auto pointDirection = vm::vec3d{stepped.currentDirection()};
  auto cameras = std::vector<std::pair<std::string, AgentCamera>>{};
  for (const auto& view : views.value())
  {
    auto camera = view.camera;
    if (camera.projection == CameraProjection::Perspective)
    {
      camera.position = point + vm::vec3d{0, 0, TracePointHeight};
      camera.direction = pointDirection;
      camera.up = upVector(pointDirection);
    }
    else
    {
      camera.position = point;
    }
    cameras.emplace_back(view.id, camera);
  }

  if (!context.dryRun())
  {
    stepTrace(*trace, direction);
    for (const auto& [id, camera] : cameras)
    {
      if (auto set = viewHost.value()->setCamera(context.document(), id, camera);
          set.is_error())
      {
        return context.operationFailed(errorMessage(set));
      }
    }
  }

  auto camera3d = Json(nullptr);
  const auto after = context.dryRun() ? std::vector<UserView>{}
                                      : viewHost.value()->views(context.document());
  for (const auto& [id, camera] : cameras)
  {
    if (id == "3d")
    {
      const auto* current = findView(after, id);
      camera3d = toJson(current ? current->camera : camera);
    }
  }

  return Json{
    {"index", index},
    {"count", stepped.points().size()},
    {"moved", index != oldIndex},
    {"point", toJson(point)},
    {"direction", toJson(pointDirection)},
    {"hasNext", stepped.hasNextPoint()},
    {"hasPrevious", stepped.hasPreviousPoint()},
    {"camera", camera3d},
  };
}

// view_layout_set

ToolResult viewLayoutSet(CallContext& context, const Args& args)
{
  auto viewHost = requireViewHost(context);
  if (viewHost.is_error())
  {
    return errorOf(viewHost);
  }
  const auto newPanes = args.getOptional<int>("panes");
  const auto maximized = args.getOptional<std::string>("maximized");
  if (!newPanes && !maximized)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass panes (1 to 4) and / or maximized (3d, xy, xz, yz or none).");
  }

  auto views = userViews(context, *viewHost.value());
  if (views.is_error())
  {
    return errorOf(views);
  }
  auto layout = viewHost.value()->layout(context.document());
  if (layout.is_error())
  {
    return context.operationFailed(errorMessage(layout));
  }

  const auto oldPanes = panes();
  const auto paneCount = newPanes.value_or(oldPanes);
  const auto panesChanged = paneCount != oldPanes;
  const auto maximizedView = maximized && *maximized != "none"
                               ? std::optional{*maximized}
                               : std::optional<std::string>{};
  if (maximizedView && paneCount == 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The one-pane layout cannot maximize a view.",
      "Pass panes: 2, 3 or 4 together with maximized; with one pane, the pane shows "
      "one view at a time.");
  }
  if (maximizedView && !findView(views.value(), *maximizedView))
  {
    return unknownViewError(views.value(), *maximizedView);
  }

  auto expected = layout.value();
  if (panesChanged)
  {
    // the views are recreated and nothing is maximized
    expected.maximizedView = std::nullopt;
  }
  if (maximized)
  {
    expected.maximizedView = maximizedView;
    if (maximizedView)
    {
      expected.currentView = maximizedView;
    }
  }

  auto result = Json{
    {"panes", paneCount},
    {"maximizedView", optionalJson(expected.maximizedView)},
    {"currentView", optionalJson(expected.currentView)},
    {"viewsRecreated", panesChanged},
    {"previous", layoutJson(oldPanes, layout.value())},
  };
  if (context.dryRun())
  {
    return result;
  }

  if (panesChanged)
  {
    viewHost.value()->prepareForLayoutChange();
    setPref(Preferences::MapViewLayout, paneCount - 1);
  }
  if (maximized && (maximizedView || paneCount > 1))
  {
    if (auto set = viewHost.value()->setMaximizedView(context.document(), maximizedView);
        set.is_error())
    {
      return context.operationFailed(errorMessage(set));
    }
  }

  if (auto actual = viewHost.value()->layout(context.document()); actual.is_success())
  {
    result["maximizedView"] = optionalJson(actual.value().maximizedView);
    result["currentView"] = optionalJson(actual.value().currentView);
  }
  return result;
}

// view_options_get, view_options_set

struct BoolOption
{
  const char* key;
  Preference<bool>* preference;
  const char* description;
};

std::array<BoolOption, 12> boolOptions()
{
  return {{
    {"shading", &Preferences::ShadeFaces, "Shade faces by their orientation"},
    {"fog", &Preferences::ShowFog, "Fog in the 3D view"},
    {"edges", &Preferences::ShowEdges, "Draw brush edges"},
    {"classnames", &Preferences::ShowEntityClassnames, "Show entity classnames"},
    {"groupBounds", &Preferences::ShowGroupBounds, "Show group bounds"},
    {"brushEntityBounds",
     &Preferences::ShowBrushEntityBounds,
     "Show brush entity bounds"},
    {"pointEntityBounds",
     &Preferences::ShowPointEntityBounds,
     "Show point entity bounds"},
    {"pointEntities", &Preferences::ShowPointEntities, "Show point entities"},
    {"pointEntityModels",
     &Preferences::ShowPointEntityModels,
     "Show point entity models (else boxes)"},
    {"brushes", &Preferences::ShowBrushes, "Show brushes"},
    {"patches", &Preferences::ShowPatches, "Show patches"},
    {"softMapBounds", &Preferences::ShowSoftMapBounds, "Show the soft map bounds"},
  }};
}

/** The view options: preferences (global) and tag and class visibility (per document). */
struct ViewOptions
{
  std::string faceMode;
  std::string entityLinkMode;
  std::vector<bool> flags;
  mdl::TagType::Type hiddenTags = 0;
  std::vector<const mdl::EntityDefinition*> hiddenDefinitions;

  bool operator==(const ViewOptions&) const = default;
};

std::vector<const mdl::EntityDefinition*> allDefinitions(const mdl::Map& map)
{
  auto result = std::vector<const mdl::EntityDefinition*>{};
  for (const auto& group : map.entityDefinitionManager().groups())
  {
    result.insert(result.end(), group.definitions.begin(), group.definitions.end());
  }
  return result;
}

ViewOptions currentViewOptions(const mdl::Map& map)
{
  auto result = ViewOptions{
    pref(Preferences::FaceRenderMode),
    pref(Preferences::EntityLinkMode),
    {},
    map.editorContext().hiddenTags(),
    {},
  };
  for (const auto& option : boolOptions())
  {
    result.flags.push_back(pref(*option.preference));
  }
  for (const auto* definition : allDefinitions(map))
  {
    if (map.editorContext().entityDefinitionHidden(*definition))
    {
      result.hiddenDefinitions.push_back(definition);
    }
  }
  return result;
}

bool isHidden(const ViewOptions& options, const mdl::EntityDefinition& definition)
{
  return std::ranges::find(options.hiddenDefinitions, &definition)
         != options.hiddenDefinitions.end();
}

Json optionsJson(const ViewOptions& options, const mdl::Map& map)
{
  auto result = Json{
    {"faceMode", options.faceMode},
    {"entityLinkMode", options.entityLinkMode},
  };
  const auto bools = boolOptions();
  for (size_t i = 0; i < bools.size(); ++i)
  {
    result[bools[i].key] = bool(options.flags[i]);
  }

  auto tags = Json::array();
  for (const auto& tag : map.tagManager().smartTags())
  {
    tags.push_back(Json{
      {"name", tag.name()},
      {"visible", (options.hiddenTags & tag.type()) == 0},
    });
  }
  result["tags"] = std::move(tags);

  auto hidden = Json::array();
  for (const auto* definition : options.hiddenDefinitions)
  {
    hidden.push_back(definition->name);
  }
  result["hiddenClassnames"] = std::move(hidden);

  auto groups = Json::array();
  for (const auto& group : map.entityDefinitionManager().groups())
  {
    const auto hiddenCount = std::ranges::count_if(
      group.definitions, [&](const auto* d) { return isHidden(options, *d); });
    groups.push_back(Json{
      {"name", mdl::displayName(group)},
      {"classes", group.definitions.size()},
      {"hidden", hiddenCount},
    });
  }
  result["classGroups"] = std::move(groups);
  return result;
}

Schema viewOptionsSchema()
{
  auto fields = std::vector<Field>{
    field("faceMode", string()).required().describe("textured, flat or skip"),
    field("entityLinkMode", string())
      .required()
      .describe("all, transitive, direct, none"),
  };
  for (const auto& option : boolOptions())
  {
    fields.push_back(
      field(option.key, boolean()).required().describe(option.description));
  }
  fields.push_back(
    field(
      "tags",
      array(object({
        field("name", string()).required(),
        field("visible", boolean()).required(),
      })))
      .required()
      .describe("The game's smart tags and whether objects / faces with them are shown"));
  fields.push_back(field("hiddenClassnames", array(string()))
                     .required()
                     .describe("Entity classes hidden in the views"));
  fields.push_back(
    field(
      "classGroups",
      array(object({
        field("name", string()).required(),
        field("classes", integer()).required(),
        field("hidden", integer()).required(),
      })))
      .required()
      .describe("The entity definition groups (classname prefixes) with hidden counts"));
  return object(std::move(fields));
}

std::string tagNames(const mdl::Map& map)
{
  auto result = std::string{};
  for (const auto& tag : map.tagManager().smartTags())
  {
    result += (result.empty() ? "" : ", ") + tag.name();
  }
  return result.empty() ? "none (the game has no smart tags)" : result;
}

Result<mdl::TagType::Type, ToolError> tagMask(
  const mdl::Map& map, const std::vector<std::string>& names)
{
  auto result = mdl::TagType::Type{0};
  for (const auto& name : names)
  {
    const auto& tags = map.tagManager().smartTags();
    const auto it = std::ranges::find_if(
      tags, [&](const auto& tag) { return kdl::ci::str_is_equal(tag.name(), name); });
    if (it == tags.end())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The game has no smart tag '{}'.", name),
        fmt::format("Tags: {}.", tagNames(map)));
    }
    result |= it->type();
  }
  return result;
}

std::string groupNames(const mdl::Map& map)
{
  auto result = std::string{};
  for (const auto& group : map.entityDefinitionManager().groups())
  {
    result += (result.empty() ? "" : ", ") + mdl::displayName(group);
  }
  return result.empty() ? "none (no entity definitions are loaded)" : result;
}

/**
 * The entity definitions matching the patterns (case-insensitive): a classname or glob
 * matches classes; a pattern that matches no class may name a definition group (e.g.
 * "monster"), which matches all classes of the group.
 */
Result<std::vector<const mdl::EntityDefinition*>, ToolError> matchDefinitions(
  const mdl::Map& map, const std::vector<std::string>& patterns)
{
  auto result = std::vector<const mdl::EntityDefinition*>{};
  const auto& groups = map.entityDefinitionManager().groups();
  for (const auto& pattern : patterns)
  {
    auto matches = std::vector<const mdl::EntityDefinition*>{};
    for (const auto& group : groups)
    {
      for (const auto* definition : group.definitions)
      {
        if (kdl::ci::str_matches_glob(definition->name, pattern))
        {
          matches.push_back(definition);
        }
      }
    }
    if (matches.empty())
    {
      for (const auto& group : groups)
      {
        if (
          kdl::ci::str_is_equal(group.name, pattern)
          || kdl::ci::str_is_equal(mdl::displayName(group), pattern))
        {
          matches.insert(
            matches.end(), group.definitions.begin(), group.definitions.end());
        }
      }
    }
    if (matches.empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("No entity class matches '{}'.", pattern),
        fmt::format(
          "Pass classnames, globs such as 'light*' or group names: {}; "
          "entity_classes_list lists the classes.",
          groupNames(map)));
    }
    for (const auto* definition : matches)
    {
      if (std::ranges::find(result, definition) == result.end())
      {
        result.push_back(definition);
      }
    }
  }
  return result;
}

ToolResult viewOptionsGet(CallContext& context, const Args&)
{
  return optionsJson(currentViewOptions(context.map()), context.map());
}

ToolResult viewOptionsSet(CallContext& context, const Args& args)
{
  const auto bools = boolOptions();
  const auto hasOption =
    std::ranges::any_of(bools, [&](const auto& option) { return args.has(option.key); })
    || std::ranges::any_of(
      std::array{
        "faceMode",
        "entityLinkMode",
        "showTags",
        "hideTags",
        "showClassnames",
        "hideClassnames",
        "restoreDefaults"},
      [&](const auto* key) { return args.has(key); });
  if (!hasOption)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass at least one option, e.g. {\"faceMode\": \"flat\"} or {\"hideTags\": "
      "[\"trigger\"]}; view_options_get lists them.");
  }

  auto& map = context.map();
  const auto previous = currentViewOptions(map);
  auto next = previous;

  if (args.getOr("restoreDefaults", false))
  {
    next.faceMode = Preferences::FaceRenderMode.defaultValue;
    next.entityLinkMode = Preferences::EntityLinkMode.defaultValue;
    for (size_t i = 0; i < bools.size(); ++i)
    {
      next.flags[i] = bools[i].preference->defaultValue;
    }
  }

  next.faceMode = args.getOr("faceMode", next.faceMode);
  next.entityLinkMode = args.getOr("entityLinkMode", next.entityLinkMode);
  for (size_t i = 0; i < bools.size(); ++i)
  {
    next.flags[i] = args.getOr<bool>(bools[i].key, next.flags[i]);
  }

  const auto showTags = args.getOr("showTags", std::vector<std::string>{});
  const auto hideTags = args.getOr("hideTags", std::vector<std::string>{});
  auto showMask = tagMask(map, showTags);
  if (showMask.is_error())
  {
    return errorOf(showMask);
  }
  auto hideMask = tagMask(map, hideTags);
  if (hideMask.is_error())
  {
    return errorOf(hideMask);
  }
  if ((showMask.value() & hideMask.value()) != 0)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "A tag cannot be both shown and hidden.",
      "Pass each tag in either showTags or hideTags.");
  }
  next.hiddenTags = (next.hiddenTags & ~showMask.value()) | hideMask.value();

  const auto showClasses = args.getOr("showClassnames", std::vector<std::string>{});
  const auto hideClasses = args.getOr("hideClassnames", std::vector<std::string>{});
  auto shown = matchDefinitions(map, showClasses);
  if (shown.is_error())
  {
    return errorOf(shown);
  }
  auto hidden = matchDefinitions(map, hideClasses);
  if (hidden.is_error())
  {
    return errorOf(hidden);
  }
  for (const auto* definition : hidden.value())
  {
    if (std::ranges::find(shown.value(), definition) != shown.value().end())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("'{}' is both shown and hidden.", definition->name),
        "Pass each class in either showClassnames or hideClassnames.");
    }
  }
  std::erase_if(next.hiddenDefinitions, [&](const auto* definition) {
    return std::ranges::find(shown.value(), definition) != shown.value().end();
  });
  for (const auto* definition : hidden.value())
  {
    if (!isHidden(next, *definition))
    {
      next.hiddenDefinitions.push_back(definition);
    }
  }
  // keep the order of the entity definition groups
  const auto all = allDefinitions(map);
  auto ordered = std::vector<const mdl::EntityDefinition*>{};
  for (const auto* definition : all)
  {
    if (isHidden(next, *definition))
    {
      ordered.push_back(definition);
    }
  }
  next.hiddenDefinitions = std::move(ordered);

  auto changed = Json::array();
  if (next.faceMode != previous.faceMode)
  {
    changed.push_back("faceMode");
  }
  if (next.entityLinkMode != previous.entityLinkMode)
  {
    changed.push_back("entityLinkMode");
  }
  for (size_t i = 0; i < bools.size(); ++i)
  {
    if (next.flags[i] != previous.flags[i])
    {
      changed.push_back(bools[i].key);
    }
  }
  if (next.hiddenTags != previous.hiddenTags)
  {
    changed.push_back("tags");
  }
  if (next.hiddenDefinitions != previous.hiddenDefinitions)
  {
    changed.push_back("hiddenClassnames");
  }

  if (!context.dryRun())
  {
    if (next.faceMode != previous.faceMode)
    {
      setPref(Preferences::FaceRenderMode, next.faceMode);
    }
    if (next.entityLinkMode != previous.entityLinkMode)
    {
      setPref(Preferences::EntityLinkMode, next.entityLinkMode);
    }
    for (size_t i = 0; i < bools.size(); ++i)
    {
      if (next.flags[i] != previous.flags[i])
      {
        setPref(*bools[i].preference, bool(next.flags[i]));
      }
    }
    auto& editorContext = map.editorContext();
    if (next.hiddenTags != previous.hiddenTags)
    {
      editorContext.setHiddenTags(next.hiddenTags);
    }
    for (const auto* definition : all)
    {
      const auto hide = isHidden(next, *definition);
      if (hide != isHidden(previous, *definition))
      {
        editorContext.setEntityDefinitionHidden(*definition, hide);
      }
    }
  }

  auto result = optionsJson(next, map);
  result["changed"] = std::move(changed);
  result["previous"] = optionsJson(previous, map);
  return result;
}

} // namespace

void registerViewTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"grid_get"}
      .title("Get Grid")
      .description(
        "Returns the grid of the document: size in map units (a power of two from 0.125 "
        "to 256), exponent (size = 2^exponent), whether it is visible and whether "
        "snapping is on, and the rotation snap angle. Example: {}")
      .input(object({}))
      .output(gridSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(gridGet));

  registry.add(
    ToolDef{"grid_set"}
      .title("Set Grid")
      .description(
        "Changes the grid of the document, like the View > Grid menu: 'size' in map "
        "units "
        "(0.125, 0.25, ..., 256) or 'exponent' (-3 to 8), and whether the grid is "
        "'visible' and 'snap'ping is on. Not undoable. Returns the new and the previous "
        "grid. Example: {\"size\": 16} or {\"snap\": false}")
      .input(object({
        field("size", number()).describe("Grid size in map units, a power of two"),
        field("exponent", integer().min(mdl::Grid::MinSize).max(mdl::Grid::MaxSize))
          .describe("Alternative to size: size = 2^exponent"),
        field("visible", boolean()).describe("Show the grid"),
        field("snap", boolean()).describe("Snap to the grid"),
      }))
      .output(object({
        field("size", number()).required(),
        field("exponent", integer()).required(),
        field("effectiveSize", number()).required(),
        field("visible", boolean()).required(),
        field("snap", boolean()).required(),
        field("angle", number()).required(),
        field("previous", gridSchema()).required(),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(gridSet));

  registry.add(
    ToolDef{"camera_get"}
      .title("Get User Cameras")
      .description(
        "Returns the user's editor views of the document's window: the 3D view and the "
        "2D views xy (top), xz (front) and yz (side), each with its camera (3D: "
        "position, direction, up, yaw/pitch, fov; 2D: center position and zoom in "
        "screen pixels per map unit), size in pixels and whether the current layout "
        "shows it; the layout (panes, maximized and current view), whether 2D cameras "
        "are linked, and the loaded point file position. These are the cameras the "
        "user looks through; agent cameras (agent_camera_*) are separate. Example: {}")
      .input(object({}))
      .output(object({
        field("views", array(viewSchema())).required(),
        field("layout", layoutSchema()).required(),
        field("link2dCameras", boolean())
          .required()
          .describe("Moving or zooming one 2D view moves the others along"),
        field("cameraFovPreference", number())
          .required()
          .describe("The 'Controls/Camera/Field of vision' preference in degrees"),
        field(
          "pointFile",
          object({
            field("index", integer()).required(),
            field("count", integer()).required(),
          }))
          .describe("The current point of the loaded point file, if any"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(cameraGet));

  registry.add(
    ToolDef{"camera_set"}
      .title("Set User Camera")
      .description(
        "Moves the camera of one of the user's editor views without animation; this "
        "changes what the user sees. 3D view: position, and the orientation as lookAt "
        "(a point), direction (a vector) or yaw/pitch in degrees (yaw counterclockwise "
        "from +x, pitch positive up; one of them may be omitted to keep it); up is "
        "optional (default: upright); fov changes the view's field of view until the "
        "'Controls/Camera/Field of vision' preference changes or the layout is switched "
        "(use preferences_set for a permanent change). 2D views (xy, xz, yz): position "
        "moves the view center (the coordinate along the view axis is ignored) and zoom "
        "is screen pixels per map unit (0.02 to 100); their direction is fixed. With "
        "linked 2D cameras the other 2D views follow (linkedViews). Not undoable. "
        "Returns the new and the previous camera. Example: {\"position\": [0, -512, "
        "256], \"lookAt\": [0, 0, 64]} or {\"view\": \"xy\", \"position\": [512, 256, "
        "0], \"zoom\": 0.5}")
      .input(object({
        field("view", viewIdSchema().defaultsTo("3d")).describe("The view to move"),
        field("position", vec3())
          .describe("3D: the eye position; 2D: the point in the view center"),
        field("lookAt", vec3()).describe("3D: the point to look at"),
        field("direction", vec3()).describe("3D: the view direction"),
        field("yaw", number())
          .describe("3D: degrees counterclockwise from +x (0 = east, 90 = north)"),
        field("pitch", number().min(-90).max(90))
          .describe("3D: degrees, positive looks up, -90 looks straight down"),
        field("up", vec3()).describe("3D: the up vector (default: upright)"),
        field("fov", number().min(1).max(150)).describe("3D: field of view in degrees"),
        field("zoom", number().min(MinViewZoom).max(MaxViewZoom))
          .describe("2D: screen pixels per map unit"),
      }))
      .output(object({
        field("view", string()).required(),
        field("camera", cameraSchema()).required(),
        field("previous", cameraSchema()).required(),
        field(
          "linkedViews",
          array(object({
            field("id", string()).required(),
            field("camera", cameraSchema()).required(),
          })))
          .required()
          .describe("Other views whose camera changed with it (linked 2D cameras)"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(cameraSet));

  registry.add(
    ToolDef{"camera_focus"}
      .title("Focus User Cameras")
      .description(
        "Points the user's editor views at objects, a box or a point, like View > "
        "Focus Camera on Selection but without animation and without changing the "
        "selection: the 3D view keeps its direction and moves back until the whole "
        "target fits; the 2D views center on the target and keep their zoom unless the "
        "target does not fit (then they zoom out just enough; linked 2D cameras share "
        "one zoom). Targets: ids (objects or faces), box, point (a 128-unit box around "
        "it), or by default the current selection. Not undoable. Returns the new and "
        "previous camera of each view. Example: {\"ids\": [\"entity:12\"]} or "
        "{\"box\": {\"min\": [0, 0, 0], \"max\": [512, 512, 128]}, \"views\": "
        "[\"3d\"]}")
      .input(object({
        field("ids", array(objectId()))
          .describe("Objects or faces to focus on. Default: the current selection"),
        field("box", box()).describe("A box to focus on"),
        field("point", vec3()).describe("A point to focus on"),
        field("views", array(viewIdSchema()).nonEmpty())
          .describe("The views to move (default: all)"),
        field("margin", number().min(1).max(10))
          .describe("Scale of the fitted extent (1 = tight, default 1.1)"),
      }))
      .output(object({
        field("box", box()).required().describe("The focused box"),
        field(
          "views",
          array(object({
            field("id", string()).required(),
            field("camera", cameraSchema()).required(),
            field("previous", cameraSchema()).required(),
          })))
          .required(),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(cameraFocus));

  registry.add(
    ToolDef{"camera_step_pointfile"}
      .title("Step Through Point File")
      .description(
        "Moves the user's cameras along the loaded point file (leak path), like View > "
        "Move Camera to Next / Previous Point but without animation: the 3D camera is "
        "placed 16 units above the point looking along the path, the 2D views center on "
        "the point. direction: next (default), previous, first, last or current. At the "
        "end of the path the camera stays (moved: false, warning END_OF_TRACE). Needs "
        "a point file loaded with pointfile_load. Not undoable. Example: {} or "
        "{\"direction\": \"first\"}")
      .input(object({
        field(
          "direction",
          enumOf({"next", "previous", "first", "last", "current"}).defaultsTo("next")),
      }))
      .output(object({
        field("index", integer()).required().describe("The current point, from 0"),
        field("count", integer()).required().describe("The number of points"),
        field("moved", boolean()).required().describe("Whether the index changed"),
        field("point", vec3()).required(),
        field("direction", vec3()).required().describe("Along the path at the point"),
        field("hasNext", boolean()).required(),
        field("hasPrevious", boolean()).required(),
        field("camera", any()).required().describe("The 3D camera"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(cameraStepPointfile));

  registry.add(
    ToolDef{"view_options_get"}
      .title("Get View Options")
      .description(
        "Returns the view options of the editor's View Options popup: face render mode "
        "(textured, flat, skip), shading, fog, edges, entity display (classnames, "
        "group / brush entity / point entity bounds, point entities, models), brushes, "
        "patches, soft map bounds and the entity link mode (global preferences), and "
        "the per-document visibility of smart tags (trigger, clip, ...) and entity "
        "classes. These affect the user's views; snapshots have their own options. "
        "Example: {}")
      .input(object({}))
      .output(viewOptionsSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(viewOptionsGet));

  auto setFields = std::vector<Field>{
    field("faceMode", enumOf({"textured", "flat", "skip"}))
      .describe("How faces are drawn; skip draws no faces (wireframe)"),
    field("entityLinkMode", enumOf({"all", "transitive", "direct", "none"}))
      .describe("Which target / targetname links are drawn (direct: of the selection)"),
  };
  for (const auto& option : boolOptions())
  {
    setFields.push_back(field(option.key, boolean()).describe(option.description));
  }
  setFields.push_back(
    field("showTags", array(string()))
      .describe("Smart tags to show (case-insensitive), e.g. [\"trigger\"]"));
  setFields.push_back(
    field("hideTags", array(string()))
      .describe("Smart tags to hide, e.g. [\"clip\", \"skip\", \"hint\"]"));
  setFields.push_back(
    field("showClassnames", array(string()))
      .describe("Entity classes to show: classnames, globs (\"*\" shows all) or, if no "
                "class matches, group names such as \"monster\""));
  setFields.push_back(field("hideClassnames", array(string()))
                        .describe("Entity classes to hide, e.g. [\"light*\"]"));
  setFields.push_back(
    field("restoreDefaults", boolean())
      .describe("First reset the preference options to their defaults, like the "
                "popup's Restore Defaults button (tags and classes are kept)"));

  auto setOutput = viewOptionsSchema();
  setOutput.fields.push_back(
    field("changed", array(string())).required().describe("The options that changed"));
  setOutput.fields.push_back(field("previous", viewOptionsSchema()).required());

  registry.add(
    ToolDef{"view_options_set"}
      .title("Set View Options")
      .description(
        "Changes the view options of the editor's View Options popup, which changes "
        "what the user sees: faceMode, shading, fog, edges, classnames, groupBounds, "
        "brushEntityBounds, pointEntityBounds, pointEntities, pointEntityModels, "
        "brushes, patches, softMapBounds, entityLinkMode (global preferences, for all "
        "windows), and per document showTags / hideTags (smart tags) and "
        "showClassnames / hideClassnames (entity classes by classname, glob or group). "
        "Hidden objects stay in the map and are compiled. Not undoable. Returns the new "
        "and previous options. Example: {\"hideTags\": [\"trigger\", \"clip\"], "
        "\"faceMode\": \"flat\"} or {\"hideClassnames\": [\"light*\"]}")
      .input(object(std::move(setFields)))
      .output(std::move(setOutput))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(viewOptionsSet));

  registry.add(
    ToolDef{"view_layout_set"}
      .title("Set View Layout")
      .description(
        "Changes the layout of the user's editor views: panes (1 to 4; the global "
        "'Views/Map view layout' preference, applied to all windows; switching "
        "recreates the views, which resets their cameras and restores a maximized view) "
        "and the maximized view (3d, xy, xz, yz, or none to restore all views; needs 2 "
        "or more panes). Maximizing makes the view current; a view that shares a "
        "cycling pane is cycled into it first. Not undoable. Returns the new and "
        "previous layout. Example: {\"panes\": 4} or {\"maximized\": \"3d\"}")
      .input(object({
        field("panes", integer().min(1).max(4)).describe("The number of view panes"),
        field("maximized", enumOf({"3d", "xy", "xz", "yz", "none"}))
          .describe("The view to maximize, or none"),
      }))
      .output(object({
        field("panes", integer()).required(),
        field("maximizedView", any()).required(),
        field("currentView", any()).required(),
        field("viewsRecreated", boolean())
          .required()
          .describe("The pane count changed: the views were recreated"),
        field("previous", layoutSchema()).required(),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(viewLayoutSet));
}

} // namespace tb::mcp
