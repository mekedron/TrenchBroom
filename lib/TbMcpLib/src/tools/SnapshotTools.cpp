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

#include "mcp/tools/SnapshotTools.h"

#include "ToolUtils.h"
#include "mcp/AgentCamera.h"
#include "mcp/Annotations.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/CameraProjection.h"
#include "mcp/Image.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/ServerState.h"
#include "mcp/Session.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/string_compare.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;
using namespace std::chrono_literals;

/** The largest image size (per side). */
constexpr auto MaxImageSize = size_t(2048);
constexpr auto MinImageSize = size_t(16);

/** While resources load, a snapshot waits up to MaxResourcePolls * ResourcePoll. */
constexpr auto ResourcePoll = 50ms;
constexpr auto MaxResourcePolls = size_t(60);

/** The most images view_snapshots_around renders in one call. */
constexpr auto MaxAroundViews = size_t(12);

/** The gap between the images of view_snapshot_compare. */
constexpr auto CompareGap = size_t(8);

/** Side views of view_snapshots_around look slightly down at the target. */
constexpr auto AroundPitch = -20.0;

constexpr auto DefaultFrameYaw = 45.0;
constexpr auto DefaultFramePitch = -30.0;
constexpr auto DefaultOrbitPitch = -30.0;

const auto HighlightDefault = Json::array({1.0, 0.5, 0.0});

// Errors

ToolError noRendererError()
{
  return makeError(
    ErrorCode::UnsupportedInHost,
    "This host cannot render snapshots.",
    "Snapshots need the TrenchBroom editor with an OpenGL context; use map_plan_view "
    "(text form), objects_find or space_check to inspect the map instead.");
}

ToolError cancelledError()
{
  return makeError(ErrorCode::Cancelled, "The call was cancelled.");
}

ToolError noDocumentError(const std::string_view what)
{
  return makeError(
    ErrorCode::NoDocument,
    fmt::format("{} needs an open document.", what),
    "Open a document (document_open) or pass document.");
}

std::string cameraNames(const Session& session)
{
  auto result = std::string{};
  for (const auto& [name, camera] : session.agentCameras)
  {
    result += (result.empty() ? "" : ", ") + name;
  }
  return result.empty() ? "none" : result;
}

ToolError unknownCameraError(const Session& session, const std::string& name)
{
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("There is no agent camera named '{}'.", name),
    fmt::format(
      "Create it with agent_camera_set or pass an inline camera. Cameras: {}.",
      cameraNames(session)));
}

// Colors

Color colorFromJson(const Json& json)
{
  auto values = std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
  for (size_t i = 0; i < json.size() && i < 4; ++i)
  {
    values[i] = std::clamp(json[i].get<float>(), 0.0f, 1.0f);
  }
  return RgbaF{values[0], values[1], values[2], values[3]};
}

// Schemas

Schema colorSchema()
{
  return array(number().min(0).max(1)).minSize(3).maxSize(4);
}

Schema cameraSpecSchema()
{
  return object({
    field("view", enumOf({"top", "front", "side", "xy", "xz", "yz"}))
      .describe(
        "Orthographic view: top (xy, looking down), front (xz, looking +y), side (yz, "
        "looking -x). Without view the camera is perspective."),
    field("center", vec3()).describe("Orthographic: the point in the image center"),
    field("zoom", number().min(0.0001).max(1000))
      .describe("Orthographic: image pixels per map unit (default 1, or fit by frame)"),
    field("position", vec3()).describe("Perspective: the eye position"),
    field("lookAt", vec3()).describe("Perspective: the point to look at"),
    field("direction", vec3()).describe("Perspective: the view direction"),
    field("yaw", number())
      .describe("Degrees, counterclockwise from +x (0 = east, 90 = north)"),
    field("pitch", number().min(-90).max(90))
      .describe("Degrees, positive looks up, -90 looks straight down"),
    field("fov", number().min(1).max(170)).describe("Field of view in degrees (90)"),
    field("near", number().min(0)).describe("Near plane distance in map units"),
    field("far", number().min(1)).describe("Far plane distance in map units"),
    field(
      "frame",
      object({
        field("ids", array(objectId())).describe("Object ids to fit into the image"),
        field("box", box()).describe("Box to fit into the image"),
        field("margin", number().min(1).max(10))
          .describe("Scale of the fitted extent (1 = tight, default 1.1)"),
      }))
      .describe(
        "Fit objects, a box or (without both) everything drawn into the image; "
        "perspective cameras look in the direction yaw/pitch (default 45 / -30) or "
        "direction"),
    field(
      "orbit",
      object({
        field("target", vec3()).describe("The point to orbit"),
        field("ids", array(objectId())).describe("Orbit the center of these objects"),
        field("yaw", number()).describe("View direction yaw (default 45)"),
        field("pitch", number().min(-90).max(90)).describe("View pitch (default -30)"),
        field("distance", number().min(1))
          .describe(
            "Distance from the target in map units (default: fit the objects, or 512)"),
      }))
      .describe("Look at a target from yaw/pitch at a distance"),
    field(
      "eyeHeight",
      object({
        field("point", vec3())
          .required()
          .describe("A point inside a room; the camera stands on the floor below it"),
        field("height", number().min(0))
          .describe("Eye height above the floor (default: the game's player eye height)"),
      }))
      .describe(
        "Stand at a player's eye height on the floor below point; look with lookAt, "
        "direction or yaw/pitch (default: yaw 0, pitch 0)"),
  });
}

Schema cameraArgSchema()
{
  return oneOf({
    string().nonEmpty().describe("The name of an agent camera"),
    cameraSpecSchema().describe(
      "An inline camera: perspective (position with lookAt, direction or yaw/pitch), "
      "orthographic (view with center or frame) or a helper (frame, orbit, "
      "eyeHeight)"),
  });
}

Schema cameraOutputSchema()
{
  return object({
    field("projection", enumOf({"perspective", "orthographic"})),
    field("position", vec3()).describe("Camera position"),
    field("direction", vec3()).describe("View direction (unit vector)"),
    field("up", vec3()).describe("Up vector (unit vector)"),
    field("yaw", number()).describe("Degrees, counterclockwise from +x"),
    field("pitch", number()).describe("Degrees, positive looks up"),
    field("fov", number()).describe("Field of view in degrees (perspective only)"),
    field("view", any()).describe("top, front, side or null (orthographic only)"),
    field("zoom", number()).describe("Image pixels per map unit (orthographic only)"),
    field("near", number()).describe("Near plane distance in map units"),
    field("far", number()).describe("Far plane distance in map units"),
  });
}

Schema optionsSchema()
{
  return object({
    field("faceMode", enumOf({"textured", "flat", "wireframe"}).defaultsTo("textured"))
      .describe("textured, flat (material colors) or wireframe (edges only)"),
    field("shading", boolean().defaultsTo(true)).describe("Shade faces by orientation"),
    field("fog", boolean().defaultsTo(false)).describe("Fog (perspective only)"),
    field("edges", boolean().defaultsTo(true)).describe("Draw brush edges"),
    field("entityModels", boolean().defaultsTo(true))
      .describe("Draw entity models (else boxes)"),
    field("bounds", boolean().defaultsTo(false))
      .describe("Draw bounding boxes of entities and groups"),
    field("classnames", boolean().defaultsTo(false)).describe("Draw entity classnames"),
    field("entityLinks", boolean().defaultsTo(false))
      .describe("Draw target / targetname links"),
    field("leakPath", boolean().defaultsTo(false))
      .describe("Draw the loaded point file (leak path)"),
    field("grid", boolean().defaultsTo(false)).describe("Draw the grid"),
    field("gridSize", number().min(0.125).max(65536))
      .describe("Grid size (default: the document's grid)"),
    field("axes", boolean().defaultsTo(false)).describe("Draw the coordinate axes"),
    field("background", colorSchema())
      .describe("[r, g, b] 0..1 (default: the editor's background color)"),
    field("hideTags", array(string()))
      .describe(
        "Smart tags to hide (case-insensitive), e.g. [\"trigger\", \"clip\", \"skip\", "
        "\"hint\"]; see tags_list. Object tags hide objects, face tags hide faces"),
    field("hideClassnames", array(string()))
      .describe("Entity classname globs to hide, e.g. [\"light*\", \"info_*\"] "
                "(\"worldspawn\" hides world brushes)"),
    field("brushes", boolean().defaultsTo(true)).describe("Draw brushes"),
    field("pointEntities", boolean().defaultsTo(true)).describe("Draw point entities"),
    field("brushEntities", boolean().defaultsTo(true))
      .describe("Draw brush entities (doors, triggers, ...)"),
    field("patches", boolean().defaultsTo(true)).describe("Draw patches"),
    field("includeHidden", boolean().defaultsTo(false))
      .describe("Also draw objects and layers hidden in the editor"),
  });
}

Schema annotationsSchema()
{
  return object({
    field(
      "labels",
      oneOf({
        boolean(),
        object({
          field("ids", array(objectId())).describe("Label exactly these objects"),
          field("max", integer().min(1).max(100))
            .describe("The most labels (default 30)"),
        }),
      }))
      .describe("true: label the visible groups, entities and brushes (most important "
                "first) with "
                "id, classname or group name and size WxDxH; or {ids, max}"),
    field(
      "grid",
      oneOf({
        boolean(),
        object({
          field("step", number().min(1).max(65536))
            .describe("Distance between lines (default 64)"),
          field("planes", enumOf({"floor", "walls", "both"}).defaultsTo("both"))
            .describe("Draw the grid on the floor, the walls or both"),
          field("box", box())
            .describe(
              "Floor = box bottom, walls = box sides (default: the space around the "
              "image center)"),
          field("labelEvery", integer().min(1).max(64))
            .describe("Label every n-th line with its coordinate (default: automatic)"),
        }),
      }))
      .describe("Coordinate lines on the floor and the walls, with coordinate labels"),
    field("compass", boolean())
      .describe("A compass in the top right corner (+y is north, +x east)"),
    field(
      "player",
      object({
        field("point", vec3()).required().describe("Where the player stands"),
        field("onFloor", boolean().defaultsTo(true))
          .describe("Stand on the floor below the point"),
      }))
      .describe("A box of the game's player size with its eye height, for scale"),
  });
}

Schema highlightSchema()
{
  return object({
    field("ids", array(objectId())).required().describe("Objects to highlight"),
    field("color", colorSchema()).describe("[r, g, b] 0..1 (default orange)"),
  });
}

Schema imageOutputSchema()
{
  return object({
    field("width", integer()).describe("Width in pixels"),
    field("height", integer()).describe("Height in pixels"),
    field("format", enumOf({"png", "jpeg"})),
    field("bytes", integer()).describe("Encoded size in bytes"),
    field("savedTo", any()).describe("The file path, or null"),
  });
}

Schema countsSchema()
{
  return object({
    field("brushes", integer()),
    field("patches", integer()),
    field("pointEntities", integer()),
    field("brushEntities", integer()),
    field("groups", integer()),
    field("hiddenFaces", integer()).describe("Faces hidden by face tags"),
    field("highlighted", integer()),
  });
}

std::vector<Field> sizeFields(const size_t width, const size_t height)
{
  return {
    field("width", integer().min(double(MinImageSize)).max(double(MaxImageSize)))
      .defaultsTo(width)
      .describe(fmt::format("Image width in pixels ({}-{})", MinImageSize, MaxImageSize)),
    field("height", integer().min(double(MinImageSize)).max(double(MaxImageSize)))
      .defaultsTo(height)
      .describe(
        fmt::format("Image height in pixels ({}-{})", MinImageSize, MaxImageSize)),
  };
}

std::vector<Field> formatFields()
{
  return {
    field("format", enumOf({"png", "jpeg"}).defaultsTo("png"))
      .describe("Image format; jpeg is smaller"),
    field("quality", integer().min(1).max(100).defaultsTo(85)).describe("JPEG quality"),
    field("saveTo", string()).describe("Also save the image to this absolute file path"),
    field("overwrite", boolean().defaultsTo(false))
      .describe("Replace an existing file at saveTo"),
  };
}

std::vector<Field> viewFields()
{
  return {
    field("options", optionsSchema()).describe("Render and visibility options"),
    field("isolate", array(objectId()))
      .describe("Draw only these objects (groups and brush entities with their members)"),
    field("highlight", highlightSchema()).describe("Tint objects in a color"),
  };
}

// Visibility: which objects a snapshot draws

struct ViewSpec
{
  SnapshotOptions options;
  std::vector<std::string> hideTags;
  std::vector<std::string> hideClassnames;
  bool brushes = true;
  bool pointEntities = true;
  bool brushEntities = true;
  bool patches = true;
  bool includeHidden = false;
  std::vector<std::string> isolate;
  std::vector<std::string> highlight;
  Color highlightColor = RgbaF{1.0f, 0.5f, 0.0f, 1.0f};
};

/** The view arguments that view_snapshot keeps with a snapshot. */
Json viewJson(const Args& args)
{
  auto result = Json::object();
  for (const auto* key : {"options", "isolate", "highlight"})
  {
    if (const auto* value = findMember(args.json(), key))
    {
      result[key] = *value;
    }
  }
  return result;
}

ViewSpec parseViewSpec(const Json& view, const mdl::Map& map)
{
  auto spec = ViewSpec{};
  spec.options.gridSize = map.grid().actualSize();

  const auto options = view.value("options", Json::object());
  const auto get = [&](const char* key, const bool defaultValue) {
    return options.value(key, defaultValue);
  };

  const auto faceMode = options.value("faceMode", std::string{"textured"});
  spec.options.faceMode = faceMode == "flat"        ? FaceMode::Flat
                          : faceMode == "wireframe" ? FaceMode::Wireframe
                                                    : FaceMode::Textured;
  spec.options.shading = get("shading", true);
  spec.options.fog = get("fog", false);
  spec.options.edges = get("edges", true);
  spec.options.entityModels = get("entityModels", true);
  spec.options.bounds = get("bounds", false);
  spec.options.classnames = get("classnames", false);
  spec.options.entityLinks = get("entityLinks", false);
  spec.options.leakPath = get("leakPath", false);
  spec.options.grid = get("grid", false);
  spec.options.gridSize = options.value("gridSize", spec.options.gridSize);
  spec.options.axes = get("axes", false);
  if (const auto* background = findMember(options, "background"))
  {
    spec.options.background = colorFromJson(*background);
  }

  spec.hideTags = options.value("hideTags", std::vector<std::string>{});
  spec.hideClassnames = options.value("hideClassnames", std::vector<std::string>{});
  spec.brushes = get("brushes", true);
  spec.pointEntities = get("pointEntities", true);
  spec.brushEntities = get("brushEntities", true);
  spec.patches = get("patches", true);
  spec.includeHidden = get("includeHidden", false);

  spec.isolate = view.value("isolate", std::vector<std::string>{});
  if (const auto* highlight = findMember(view, "highlight"))
  {
    spec.highlight = highlight->value("ids", std::vector<std::string>{});
    spec.highlightColor = colorFromJson(highlight->value("color", HighlightDefault));
  }
  return spec;
}

struct SceneCounts
{
  size_t brushes = 0;
  size_t patches = 0;
  size_t pointEntities = 0;
  size_t brushEntities = 0;
  size_t groups = 0;
  size_t hiddenFaces = 0;
  size_t highlighted = 0;
};

Json countsJson(const SceneCounts& counts)
{
  return Json{
    {"brushes", counts.brushes},
    {"patches", counts.patches},
    {"pointEntities", counts.pointEntities},
    {"brushEntities", counts.brushEntities},
    {"groups", counts.groups},
    {"hiddenFaces", counts.hiddenFaces},
    {"highlighted", counts.highlighted},
  };
}

struct ResolvedScene
{
  SnapshotScene scene;
  SceneCounts counts;
  /** The bounds of the drawn brushes, patches and point entities. */
  std::optional<vm::bbox3d> bounds;
  std::vector<Warning> warnings;
};

struct SceneBuilder
{
  const ViewSpec& spec;
  mdl::TagType::Type objectMask = 0;
  mdl::TagType::Type faceMask = 0;
  std::unordered_set<const mdl::Node*> isolated = {};
  std::unordered_set<const mdl::Node*> highlighted = {};
  ResolvedScene result = {};

  bool hiddenClassname(const std::string& classname) const
  {
    return std::ranges::any_of(spec.hideClassnames, [&](const auto& pattern) {
      return kdl::ci::str_matches_glob(classname, pattern);
    });
  }

  /** Whether the node is hidden by the editor and should stay hidden. */
  bool editorHidden(const mdl::Node& node, const bool isolatedNode) const
  {
    return !spec.includeHidden && !isolatedNode && !node.visible();
  }

  void add(mdl::Node& node, const bool highlight)
  {
    result.scene.nodes.push_back(&node);
    if (highlight)
    {
      result.scene.highlighted.push_back(&node);
      ++result.counts.highlighted;
    }
  }

  void addBounds(const mdl::Node& node)
  {
    result.bounds = result.bounds ? vm::merge(*result.bounds, node.logicalBounds())
                                  : node.logicalBounds();
  }

  /** Visits the node; returns whether anything was drawn. */
  bool visit(mdl::Node& node, bool inIsolated, bool inHighlighted, bool inBrushEntity)
  {
    inIsolated = inIsolated || isolated.contains(&node);
    inHighlighted = inHighlighted || highlighted.contains(&node);
    const auto isolatedNode = isolated.empty() || inIsolated;

    if (dynamic_cast<mdl::WorldNode*>(&node) || dynamic_cast<mdl::LayerNode*>(&node))
    {
      auto any = false;
      for (auto* child : node.children())
      {
        any = visit(*child, inIsolated, inHighlighted, false) || any;
      }
      return any;
    }

    if (dynamic_cast<mdl::GroupNode*>(&node))
    {
      auto any = false;
      for (auto* child : node.children())
      {
        any = visit(*child, inIsolated, inHighlighted, false) || any;
      }
      if (any)
      {
        add(node, inHighlighted);
        ++result.counts.groups;
      }
      return any;
    }

    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node))
    {
      const auto& classname = entityNode->entity().classname();
      if (entityNode->hasChildren())
      {
        if (!spec.brushEntities || hiddenClassname(classname) || node.hasTag(objectMask))
        {
          return false;
        }
        auto any = false;
        for (auto* child : node.children())
        {
          any = visit(*child, inIsolated, inHighlighted, true) || any;
        }
        if (any)
        {
          add(node, inHighlighted);
          ++result.counts.brushEntities;
        }
        return any;
      }

      if (
        !isolatedNode || !spec.pointEntities || editorHidden(node, inIsolated)
        || hiddenClassname(classname) || node.hasTag(objectMask))
      {
        return false;
      }
      add(node, inHighlighted);
      addBounds(node);
      ++result.counts.pointEntities;
      return true;
    }

    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
    {
      if (
        !isolatedNode || !spec.brushes || editorHidden(node, inIsolated)
        || (!inBrushEntity && hiddenClassname("worldspawn")) || node.hasTag(objectMask)
        || brushNode->allFacesHaveAnyTagInMask(objectMask | faceMask))
      {
        return false;
      }
      if (faceMask != 0)
      {
        for (const auto& face : brushNode->brush().faces())
        {
          if (face.hasTag(faceMask))
          {
            ++result.counts.hiddenFaces;
          }
        }
      }
      add(node, inHighlighted);
      addBounds(node);
      ++result.counts.brushes;
      return true;
    }

    if (dynamic_cast<mdl::PatchNode*>(&node))
    {
      if (
        !isolatedNode || !spec.patches || editorHidden(node, inIsolated)
        || node.hasTag(objectMask | faceMask))
      {
        return false;
      }
      add(node, inHighlighted);
      addBounds(node);
      ++result.counts.patches;
      return true;
    }

    return false;
  }
};

/** Whether a smart tag applies to faces (material, surface or content flag matchers). */
bool isFaceTag(const mdl::SmartTag& tag)
{
  // Object tags match entity classnames; they never match a face
  auto str = std::ostringstream{};
  str << tag;
  return str.str().find("EntityClassNameMatcher") == std::string::npos;
}

/**
 * Resolves the view spec into the objects to draw. Unknown ids fail the call unless
 * `lenient` is set (then they are skipped, e.g. objects that do not exist before an
 * undone change).
 */
Result<ResolvedScene, ToolError> buildScene(
  mdl::Map& map, const IdRegistry& ids, const ViewSpec& spec, const bool lenient)
{
  auto builder = SceneBuilder{spec};

  for (const auto& name : spec.hideTags)
  {
    const auto& tags = map.tagManager().smartTags();
    const auto it = std::ranges::find_if(
      tags, [&](const auto& tag) { return kdl::ci::str_is_equal(tag.name(), name); });
    if (it == tags.end())
    {
      auto known = std::string{};
      for (const auto& tag : tags)
      {
        known += (known.empty() ? "" : ", ") + tag.name();
      }
      builder.result.warnings.push_back(Warning{
        "UNKNOWN_TAG",
        fmt::format(
          "The game has no smart tag '{}'; known tags: {}.",
          name,
          known.empty() ? "none" : known)});
      continue;
    }
    (isFaceTag(*it) ? builder.faceMask : builder.objectMask) |= it->type();
  }

  const auto resolveAll =
    [&](const std::vector<std::string>& idList, auto& set) -> std::optional<ToolError> {
    for (const auto& id : idList)
    {
      auto node = ids.resolve(id);
      if (node.is_error())
      {
        if (lenient)
        {
          continue;
        }
        return errorOf(node);
      }
      set.insert(node.value());
    }
    return std::nullopt;
  };
  if (auto error = resolveAll(spec.isolate, builder.isolated))
  {
    return *error;
  }
  if (auto error = resolveAll(spec.highlight, builder.highlighted))
  {
    return *error;
  }
  if (!spec.isolate.empty() && builder.isolated.empty())
  {
    // all isolated objects are gone (lenient): draw nothing rather than everything
    builder.isolated.insert(nullptr);
  }

  builder.visit(map.worldNode(), false, false, false);

  auto& scene = builder.result.scene;
  scene.highlightColor = spec.highlightColor;
  if (builder.faceMask != 0)
  {
    scene.faceFilter = [mask = builder.faceMask](
                         const mdl::BrushNode&, const mdl::BrushFace& face) {
      return !face.hasTag(mask);
    };
  }
  return std::move(builder.result);
}

// Cameras

using BoundsProvider = std::function<std::optional<vm::bbox3d>()>;

struct ResolvedCamera
{
  AgentCamera camera;
  /** The name of the agent camera, if one was used. */
  std::optional<std::string> name;
  /** Details of eye height placement, if used. */
  Json placement = nullptr;
};

Result<vm::bbox3d, ToolError> boundsOfIds(
  CallContext& context, const std::vector<std::string>& idList)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto& id : idList)
  {
    auto node = context.ids().resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    const auto& bounds = node.value()->logicalBounds();
    result = result ? vm::merge(*result, bounds) : bounds;
  }
  if (!result)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "ids must not be empty.",
      "Pass object ids such as \"brush:12\", or a box instead.");
  }
  return *result;
}

/** The box to frame: ids, box, or the default bounds. */
Result<vm::bbox3d, ToolError> frameTarget(
  CallContext& context, const Json& frame, const BoundsProvider& defaultBounds)
{
  if (const auto* ids = findMember(frame, "ids"); ids && !ids->empty())
  {
    if (!context.hasDocument())
    {
      return noDocumentError("Framing objects");
    }
    return boundsOfIds(context, ids->get<std::vector<std::string>>());
  }
  if (const auto* box = findMember(frame, "box"))
  {
    return *boxFromJson(*box);
  }
  if (const auto bounds = defaultBounds ? defaultBounds() : std::nullopt)
  {
    return *bounds;
  }
  return makeError(
    ErrorCode::InvalidArgument,
    "There is nothing to frame.",
    "Pass frame.ids or frame.box, or position / center.");
}

vm::bbox3d outerBounds(CallContext& context, const std::optional<vm::bbox3d>& target)
{
  if (context.hasDocument())
  {
    auto bounds = context.map().worldBounds();
    return target ? vm::merge(bounds, *target) : bounds;
  }
  return target ? vm::merge(vm::bbox3d{32768.0}, *target) : vm::bbox3d{32768.0};
}

/** The view direction of a perspective camera spec, if one is given. */
Result<std::optional<vm::vec3d>, ToolError> specDirection(
  const Json& spec, const std::optional<vm::vec3d>& position)
{
  const auto hasLookAt = findMember(spec, "lookAt") != nullptr;
  const auto hasDirection = findMember(spec, "direction") != nullptr;
  const auto hasAngles =
    findMember(spec, "yaw") != nullptr || findMember(spec, "pitch") != nullptr;
  if (int(hasLookAt) + int(hasDirection) + int(hasAngles) > 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass only one of lookAt, direction and yaw/pitch.",
      "lookAt aims at a point, direction is a vector, yaw/pitch are angles.");
  }
  if (hasLookAt)
  {
    const auto target = *vec3FromJson(spec["lookAt"]);
    if (!position)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "lookAt needs a camera position.",
        "Pass position, or use frame / orbit.");
    }
    if (vm::length(target - *position) < 1e-6)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "lookAt must differ from the camera position.",
        "Pass a target away from the camera.");
    }
    return std::optional{vm::normalize(target - *position)};
  }
  if (hasDirection)
  {
    const auto direction = *vec3FromJson(spec["direction"]);
    if (vm::length(direction) < 1e-9)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "direction must not be zero.",
        "Pass a direction such as [1, 0, 0].");
    }
    return std::optional{vm::normalize(direction)};
  }
  if (hasAngles)
  {
    return std::optional{
      directionFromAngles(spec.value("yaw", 0.0), spec.value("pitch", 0.0))};
  }
  return std::optional<vm::vec3d>{};
}

Result<ResolvedCamera, ToolError> resolveCameraSpec(
  CallContext& context,
  const Json& spec,
  const size_t width,
  const size_t height,
  const BoundsProvider& defaultBounds)
{
  const auto has = [&](const char* key) { return findMember(spec, key) != nullptr; };
  const auto helpers = int(has("frame")) + int(has("orbit")) + int(has("eyeHeight"));
  if (helpers > 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass only one of frame, orbit and eyeHeight.",
      "Each of them places the camera.");
  }

  auto result = ResolvedCamera{};
  auto& camera = result.camera;
  const auto fov = spec.value("fov", DefaultFov);

  const auto finish = [&]() -> Result<ResolvedCamera, ToolError> {
    if (has("near"))
    {
      camera.nearPlane = spec["near"].get<double>();
    }
    if (has("far"))
    {
      camera.farPlane = spec["far"].get<double>();
    }
    if (camera.farPlane <= camera.nearPlane)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "far must be greater than near.",
        "Pass a larger far or a smaller near.");
    }
    return result;
  };

  // orthographic
  if (has("view"))
  {
    for (const auto* key :
         {"position", "lookAt", "direction", "yaw", "pitch", "fov", "orbit", "eyeHeight"})
    {
      if (has(key))
      {
        return makeError(
          ErrorCode::InvalidArgument,
          fmt::format("{} is not used by orthographic views.", key),
          "Orthographic views take view, center, zoom and frame.");
      }
    }
    const auto view = *parseOrthoView(spec["view"].get<std::string>());
    if (has("frame") || !has("center"))
    {
      auto target =
        frameTarget(context, spec.value("frame", Json::object()), defaultBounds);
      if (target.is_error())
      {
        return errorOf(target);
      }
      const auto margin = spec.value("frame", Json::object()).value("margin", 1.1);
      camera = orthographicCamera(
        view, target.value().center(), 1.0, outerBounds(context, target.value()));
      camera = frameBox(camera, target.value(), width, height, margin);
      if (has("center"))
      {
        return makeError(
          ErrorCode::InvalidArgument,
          "Pass either center or frame.",
          "frame computes the center.");
      }
    }
    else
    {
      camera = orthographicCamera(
        view, *vec3FromJson(spec["center"]), 1.0, outerBounds(context, std::nullopt));
    }
    if (has("zoom"))
    {
      camera.zoom = spec["zoom"].get<double>();
    }
    return finish();
  }

  if (has("center") || has("zoom"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "center and zoom are only used by orthographic views.",
      "Pass view: \"top\", \"front\" or \"side\", or use position for perspective.");
  }

  // perspective: orbit
  if (has("orbit"))
  {
    if (
      has("position") || has("lookAt") || has("direction") || has("yaw") || has("pitch"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "orbit places the camera; do not pass position or a direction.",
        "Set orbit.yaw and orbit.pitch instead.");
    }
    const auto& orbit = spec["orbit"];
    auto target = std::optional<vm::vec3d>{};
    auto distance = orbit.contains("distance")
                      ? std::optional{orbit["distance"].get<double>()}
                      : std::nullopt;
    if (const auto* ids = findMember(orbit, "ids"); ids && !ids->empty())
    {
      if (!context.hasDocument())
      {
        return noDocumentError("Orbiting objects");
      }
      auto bounds = boundsOfIds(context, ids->get<std::vector<std::string>>());
      if (bounds.is_error())
      {
        return errorOf(bounds);
      }
      target = bounds.value().center();
      if (!distance)
      {
        distance = framingDistance(bounds.value(), fov, width, height);
      }
    }
    if (orbit.contains("target"))
    {
      if (target)
      {
        return makeError(
          ErrorCode::InvalidArgument,
          "Pass either orbit.target or orbit.ids.",
          "ids orbit the center of the objects.");
      }
      target = *vec3FromJson(orbit["target"]);
    }
    if (!target)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "orbit needs a target or ids.",
        "Pass orbit.target: [x, y, z] or orbit.ids.");
    }
    camera = orbitCamera(
      *target,
      orbit.value("yaw", DefaultFrameYaw),
      orbit.value("pitch", DefaultOrbitPitch),
      distance.value_or(512.0),
      fov);
    return finish();
  }

  // perspective: eye height
  if (has("eyeHeight"))
  {
    if (has("position"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "eyeHeight places the camera; do not pass position.",
        "Pass eyeHeight.point inside the room.");
    }
    if (!context.hasDocument())
    {
      return noDocumentError("Eye height placement");
    }
    const auto& eye = spec["eyeHeight"];
    const auto point = *vec3FromJson(eye["point"]);
    const auto floor = findFloor(context.map(), point);
    if (!floor)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "There is no floor below ({}, {}, {}).", point.x(), point.y(), point.z()),
        "Pass a point inside a room, above its floor (space_check reports the floor).");
    }
    const auto player = playerSize(context.map().gameInfo().gameConfig);
    const auto eyeHeight = eye.value("height", player.eyeHeight);
    const auto position = vm::vec3d{point.x(), point.y(), *floor + eyeHeight};
    auto direction = specDirection(spec, position);
    if (direction.is_error())
    {
      return errorOf(direction);
    }
    camera =
      perspectiveCamera(position, direction.value().value_or(vm::vec3d{1, 0, 0}), fov);
    if (has("yaw"))
    {
      camera.up = upVector(camera.direction, spec["yaw"].get<double>());
    }
    result.placement = Json{
      {"floor", roundForOutput(*floor)},
      {"eyeHeight", roundForOutput(eyeHeight)},
      {"game", player.family},
    };
    return finish();
  }

  // perspective: frame, or position with a direction
  auto position =
    has("position") ? std::optional{*vec3FromJson(spec["position"])} : std::nullopt;
  if (has("frame") || !position)
  {
    if (position)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Pass either position or frame.",
        "frame computes the position.");
    }
    if (has("lookAt"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "lookAt needs a camera position.",
        "Pass position, or use frame with yaw / pitch or direction.");
    }
    auto direction = specDirection(spec, std::nullopt);
    if (direction.is_error())
    {
      return errorOf(direction);
    }
    auto target =
      frameTarget(context, spec.value("frame", Json::object()), defaultBounds);
    if (target.is_error())
    {
      return errorOf(target);
    }
    const auto yaw = spec.value("yaw", DefaultFrameYaw);
    const auto dir =
      direction.value().value_or(directionFromAngles(yaw, DefaultFramePitch));
    camera = perspectiveCamera(target.value().center(), dir, fov);
    camera.up = upVector(dir, yaw);
    const auto margin = spec.value("frame", Json::object()).value("margin", 1.1);
    camera = frameBox(camera, target.value(), width, height, margin);
    return finish();
  }

  auto direction = specDirection(spec, position);
  if (direction.is_error())
  {
    return errorOf(direction);
  }
  camera =
    perspectiveCamera(*position, direction.value().value_or(vm::vec3d{1, 0, 0}), fov);
  if (has("yaw"))
  {
    camera.up = upVector(camera.direction, spec["yaw"].get<double>());
  }
  return finish();
}

/** A camera argument: the name of an agent camera, an inline camera, or null. */
Result<ResolvedCamera, ToolError> resolveCameraArg(
  CallContext& context,
  const Json& arg,
  const size_t width,
  const size_t height,
  const BoundsProvider& defaultBounds)
{
  if (arg.is_string())
  {
    const auto name = arg.get<std::string>();
    const auto& cameras = context.session().agentCameras;
    const auto it = cameras.find(name);
    if (it == cameras.end())
    {
      return unknownCameraError(context.session(), name);
    }
    return ResolvedCamera{it->second, name};
  }
  return resolveCameraSpec(
    context, arg.is_object() ? arg : Json::object(), width, height, defaultBounds);
}

// Image output

struct OutputSpec
{
  std::string format = "png";
  int quality = 85;
  std::optional<std::filesystem::path> saveTo;
  bool overwrite = false;
};

Result<OutputSpec, ToolError> parseOutputSpec(const Args& args)
{
  auto spec = OutputSpec{};
  spec.format = args.getOr<std::string>("format", "png");
  spec.quality = int(args.getOr<int64_t>("quality", 85));
  spec.overwrite = args.getOr("overwrite", false);
  if (args.has("saveTo"))
  {
    auto path = absolutePathArgument(args, "saveTo");
    if (path.is_error())
    {
      return errorOf(path);
    }
    spec.saveTo = path.value();
    if (!spec.overwrite && pathExists(*spec.saveTo))
    {
      return makeError(
        ErrorCode::FileExists,
        fmt::format("{} already exists.", spec.saveTo->string()),
        "Pass overwrite: true to replace it, or choose another path.");
    }
    if (!pathExists(spec.saveTo->parent_path()))
    {
      return makeError(
        ErrorCode::IoError,
        fmt::format("The folder {} does not exist.", spec.saveTo->parent_path().string()),
        "Save into an existing folder; the server does not create folders.");
    }
  }
  return spec;
}

/**
 * Encodes the image, saves it if requested, adds it to the call's content and returns
 * its description.
 */
Result<Json, ToolError> outputImage(
  CallContext& context,
  SnapshotRenderer& renderer,
  const RgbaImage& image,
  const OutputSpec& spec)
{
  auto format = spec.format;
  auto bytes = std::optional<std::string>{};
  if (format == "jpeg")
  {
    bytes = renderer.encodeJpeg(image, spec.quality);
    if (!bytes)
    {
      context.warn(
        "JPEG_UNSUPPORTED", "This host cannot encode JPEG; the image is a PNG instead.");
      format = "png";
    }
  }
  if (!bytes)
  {
    bytes = encodePng(image);
  }
  if (!bytes)
  {
    return makeError(ErrorCode::InternalError, "Could not encode the image.");
  }

  auto savedTo = Json(nullptr);
  if (spec.saveTo)
  {
    if (!spec.overwrite && pathExists(*spec.saveTo))
    {
      return makeError(
        ErrorCode::FileExists,
        fmt::format("{} already exists.", spec.saveTo->string()),
        "Pass overwrite: true to replace it, or choose another path.");
    }
    auto stream = std::ofstream{*spec.saveTo, std::ios::binary | std::ios::trunc};
    stream.write(bytes->data(), std::streamsize(bytes->size()));
    stream.close();
    if (!stream)
    {
      return makeError(
        ErrorCode::IoError,
        fmt::format("Could not write {}.", spec.saveTo->string()),
        "Check that the folder is writable.");
    }
    savedTo = spec.saveTo->string();
  }

  context.addImage(*bytes, format == "jpeg" ? "image/jpeg" : "image/png");
  return Json{
    {"width", image.width},
    {"height", image.height},
    {"format", format},
    {"bytes", bytes->size()},
    {"savedTo", std::move(savedTo)},
  };
}

// Rendering

struct RenderedView
{
  RgbaImage image;
  ResolvedCamera camera;
  SceneCounts counts;
  /** The drawn objects; only valid in the step that rendered them. */
  std::vector<mdl::Node*> nodes;
  std::function<bool(const mdl::BrushNode&, const mdl::BrushFace&)> faceFilter;
};

using CameraResolver = std::function<Result<ResolvedCamera, ToolError>(
  const std::optional<vm::bbox3d>& sceneBounds)>;

/**
 * Builds the scene, resolves the camera (framing defaults to the drawn objects) and
 * renders one image. Scene warnings are reported if `warn` is set.
 */
Result<RenderedView, ToolError> renderView(
  CallContext& context,
  SnapshotRenderer& renderer,
  const ViewSpec& spec,
  const CameraResolver& resolveCamera,
  const size_t width,
  const size_t height,
  const bool lenient,
  const bool warn)
{
  auto scene = buildScene(context.map(), context.ids(), spec, lenient);
  if (scene.is_error())
  {
    return errorOf(scene);
  }
  auto resolved = std::move(scene).value();
  if (warn)
  {
    for (auto& warning : resolved.warnings)
    {
      context.warn(warning.code, warning.message, warning.objectIds);
    }
    if (resolved.scene.nodes.empty())
    {
      context.warn(
        "NOTHING_DRAWN",
        "No objects match the view options; the image shows only the background.");
    }
  }

  auto camera = resolveCamera(resolved.bounds);
  if (camera.is_error())
  {
    return errorOf(camera);
  }

  auto request = SnapshotRequest{};
  request.camera = camera.value().camera;
  request.width = width;
  request.height = height;
  request.options = spec.options;
  request.scene = std::move(resolved.scene);

  auto image = renderer.render(context.document(), request);
  if (image.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format("Rendering failed: {}", errorMessage(image)),
      "The editor may have no OpenGL context yet; try again when a map window is "
      "shown.");
  }
  auto rendered = std::move(image).value();
  if (rendered.width != width || rendered.height != height)
  {
    return makeError(
      ErrorCode::InternalError,
      fmt::format(
        "The renderer returned a {}x{} image instead of {}x{}.",
        rendered.width,
        rendered.height,
        width,
        height));
  }
  return RenderedView{
    std::move(rendered),
    camera.value(),
    resolved.counts,
    std::move(request.scene.nodes),
    std::move(request.scene.faceFilter)};
}

/**
 * Runs `step` in a deferred step once the document's resources are loaded, or after
 * waiting MaxResourcePolls * ResourcePoll (then `pending` is true). Completes the call
 * with CANCELLED or UNSUPPORTED_IN_HOST as needed.
 */
void whenResourcesReady(
  CallContext& context,
  ToolCompletion completion,
  std::function<void(SnapshotRenderer&, bool pending)> step,
  const size_t attempt = 0)
{
  context.defer(
    [&context,
     completion = std::move(completion),
     step = std::move(step),
     attempt]() mutable {
      if (context.cancelled())
      {
        completion(cancelledError());
        return;
      }
      auto* renderer = context.host().snapshotRenderer();
      if (!renderer)
      {
        completion(noRendererError());
        return;
      }
      const auto pending = renderer->resourcesPending(context.document());
      if (pending && attempt < MaxResourcePolls)
      {
        whenResourcesReady(context, std::move(completion), std::move(step), attempt + 1);
        return;
      }
      step(*renderer, pending);
    },
    attempt == 0 ? 0ms : ResourcePoll);
}

void warnPending(CallContext& context, const bool pending)
{
  if (pending)
  {
    context.warn(
      "RESOURCES_LOADING",
      "Materials or entity models were still loading; the image may miss some of "
      "them. Take the snapshot again in a moment.");
  }
}

// Snapshot ids and annotations

/** Remembers the camera of a rendered image for view_pick and returns its id. */
std::string recordSnapshot(
  CallContext& context,
  const AgentCamera& camera,
  const size_t width,
  const size_t height,
  Json view)
{
  return context.session().recordSnapshot(SnapshotRecord{
    {}, context.documentInfo().id, camera, width, height, std::move(view)});
}

/** Whether the node is drawn by the image and would be hit by a ray. */
std::function<bool(const mdl::Node&)> drawnPredicate(const std::vector<mdl::Node*>& nodes)
{
  auto set =
    std::make_shared<std::unordered_set<const mdl::Node*>>(nodes.begin(), nodes.end());
  return [set](const mdl::Node& node) { return set->contains(&node); };
}

/**
 * Draws the requested annotations onto the image of the rendered view and returns what
 * was drawn, or null if no annotations were requested.
 */
Result<Json, ToolError> annotate(
  CallContext& context, RenderedView& view, const Json& annotations, const bool warn)
{
  if (annotations.is_null() || annotations.empty())
  {
    return Json(nullptr);
  }
  auto projection =
    ImageProjection::create(view.camera.camera, view.image.width, view.image.height);
  if (projection.is_error())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("Cannot annotate the image: {}", errorMessage(projection)),
      "Check the camera.");
  }
  auto warnings = std::vector<Warning>{};
  auto spec = buildAnnotations(
    context.map(),
    context.ids(),
    projection.value(),
    view.nodes,
    drawnPredicate(view.nodes),
    annotations,
    warnings);
  if (spec.is_error())
  {
    return errorOf(spec);
  }
  if (warn)
  {
    for (const auto& warning : warnings)
    {
      context.warn(warning.code, warning.message, warning.objectIds);
    }
  }
  const auto report = drawAnnotations(view.image, projection.value(), spec.value());
  return annotationsJson(spec.value(), report);
}

// agent_camera_*

const auto CameraNamePattern = std::string{"^[A-Za-z0-9_.-]{1,64}$"};

ToolResult agentCameraSet(CallContext& context, const Args& args)
{
  const auto name = args.get<std::string>("name");
  auto& cameras = context.session().agentCameras;
  const auto replaced = cameras.contains(name);
  if (!replaced && cameras.size() >= Session::MaxAgentCameras)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "A session can have at most {} agent cameras.", Session::MaxAgentCameras),
      "Delete cameras with agent_camera_delete or reuse a name.");
  }

  const auto width = size_t(args.get<int64_t>("width"));
  const auto height = size_t(args.get<int64_t>("height"));
  const auto defaultBounds = [&]() -> std::optional<vm::bbox3d> {
    if (!context.hasDocument())
    {
      return std::nullopt;
    }
    auto spec = ViewSpec{};
    auto scene = buildScene(context.map(), context.ids(), spec, true);
    return scene.is_success() ? scene.value().bounds : std::nullopt;
  };

  auto resolved =
    resolveCameraSpec(context, args.get<Json>("camera"), width, height, defaultBounds);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  cameras[name] = resolved.value().camera;

  auto result = Json{
    {"name", name},
    {"camera", toJson(resolved.value().camera)},
    {"replaced", replaced},
  };
  if (!resolved.value().placement.is_null())
  {
    result["placement"] = resolved.value().placement;
  }
  return result;
}

ToolResult agentCameraGet(CallContext& context, const Args& args)
{
  const auto name = args.get<std::string>("name");
  const auto& cameras = context.session().agentCameras;
  const auto it = cameras.find(name);
  if (it == cameras.end())
  {
    return unknownCameraError(context.session(), name);
  }
  return Json{{"name", name}, {"camera", toJson(it->second)}};
}

ToolResult agentCameraList(CallContext& context, const Args&)
{
  auto list = Json::array();
  for (const auto& [name, camera] : context.session().agentCameras)
  {
    list.push_back(Json{{"name", name}, {"camera", toJson(camera)}});
  }
  auto kept = Json::array();
  for (const auto& snapshot : context.session().keptSnapshots)
  {
    kept.push_back(Json{
      {"name", snapshot.name},
      {"document", snapshot.documentId},
      {"width", snapshot.width},
      {"height", snapshot.height},
    });
  }
  return Json{{"cameras", std::move(list)}, {"keptSnapshots", std::move(kept)}};
}

ToolResult agentCameraDelete(CallContext& context, const Args& args)
{
  const auto name = args.get<std::string>("name");
  auto& cameras = context.session().agentCameras;
  if (!cameras.erase(name))
  {
    return unknownCameraError(context.session(), name);
  }
  return Json{{"deleted", name}};
}

// view_snapshot

void keepSnapshot(CallContext& context, KeptSnapshot snapshot)
{
  auto& kept = context.session().keptSnapshots;
  std::erase_if(kept, [&](const auto& other) { return other.name == snapshot.name; });
  if (kept.size() >= Session::MaxKeptSnapshots)
  {
    context.warn(
      "SNAPSHOT_DROPPED",
      fmt::format(
        "At most {} snapshots are kept; the oldest ('{}') was dropped.",
        Session::MaxKeptSnapshots,
        kept.front().name));
    kept.erase(kept.begin());
  }
  kept.push_back(std::move(snapshot));
}

void viewSnapshot(CallContext& context, const Args& args, ToolCompletion completion)
{
  if (!context.host().snapshotRenderer())
  {
    completion(noRendererError());
    return;
  }
  auto output = parseOutputSpec(args);
  if (output.is_error())
  {
    completion(errorOf(output));
    return;
  }
  if (args.has("camera") && args.get<Json>("camera").is_string())
  {
    const auto name = args.get<Json>("camera").get<std::string>();
    if (!context.session().agentCameras.contains(name))
    {
      completion(unknownCameraError(context.session(), name));
      return;
    }
  }

  whenResourcesReady(
    context,
    completion,
    [&context, args, completion, output = output.value()](
      SnapshotRenderer& renderer, const bool pending) {
      const auto width = size_t(args.get<int64_t>("width"));
      const auto height = size_t(args.get<int64_t>("height"));
      const auto view = viewJson(args);
      const auto spec = parseViewSpec(view, context.map());
      const auto cameraArg = args.getOr<Json>("camera", Json(nullptr));

      auto rendered = renderView(
        context,
        renderer,
        spec,
        [&](const auto& bounds) {
          return resolveCameraArg(
            context, cameraArg, width, height, [&]() { return bounds; });
        },
        width,
        height,
        false,
        true);
      if (rendered.is_error())
      {
        completion(errorOf(rendered));
        return;
      }
      auto result = std::move(rendered).value();
      warnPending(context, pending);

      // kept snapshots keep the image without annotations, for comparisons
      auto kept = Json(nullptr);
      if (args.has("keepAs"))
      {
        const auto name = args.get<std::string>("keepAs");
        keepSnapshot(
          context,
          KeptSnapshot{
            name,
            context.documentInfo().id,
            result.camera.camera,
            width,
            height,
            view,
            result.image});
        kept = name;
      }

      auto annotations =
        annotate(context, result, args.getOr<Json>("annotations", Json(nullptr)), true);
      if (annotations.is_error())
      {
        completion(errorOf(annotations));
        return;
      }

      auto image = outputImage(context, renderer, result.image, output);
      if (image.is_error())
      {
        completion(errorOf(image));
        return;
      }

      auto json = Json{
        {"snapshotId",
         recordSnapshot(context, result.camera.camera, width, height, view)},
        {"image", std::move(image).value()},
        {"camera", toJson(result.camera.camera)},
        {"cameraName", result.camera.name ? Json(*result.camera.name) : Json(nullptr)},
        {"counts", countsJson(result.counts)},
        {"keptAs", std::move(kept)},
        {"resourcesPending", pending},
      };
      if (!annotations.value().is_null())
      {
        json["annotations"] = std::move(annotations).value();
      }
      completion(std::move(json));
    });
}

// view_snapshots_around

struct AroundView
{
  std::string label;
  Json camera;
};

std::optional<Json> namedAroundCamera(const std::string& name)
{
  // side views: the camera stands on that side and looks at the target
  static const auto yaws = std::vector<std::pair<std::string, double>>{
    {"north", -90.0},
    {"east", 180.0},
    {"south", 90.0},
    {"west", 0.0},
    {"northeast", -135.0},
    {"northwest", -45.0},
    {"southeast", 135.0},
    {"southwest", 45.0},
  };
  for (const auto& [viewName, yaw] : yaws)
  {
    if (viewName == name)
    {
      return Json{{"yaw", yaw}, {"pitch", AroundPitch}};
    }
  }
  if (name == "above")
  {
    return Json{{"yaw", 90.0}, {"pitch", -90.0}};
  }
  if (name == "top" || name == "front" || name == "side")
  {
    return Json{{"view", name}};
  }
  return std::nullopt;
}

std::string aroundLabel(const std::string& name)
{
  if (name == "top" || name == "front" || name == "side")
  {
    return name + " (orthographic)";
  }
  if (name == "above")
  {
    return "above (looking down, north up)";
  }
  return "from " + name;
}

struct AroundJob
{
  Args args;
  ToolCompletion completion;
  std::vector<AroundView> views;
  std::optional<vm::bbox3d> target;
  Json images = Json::array();
  size_t next = 0;
  bool anyPending = false;
};

/** Renders the next image of view_snapshots_around, one image per deferred step. */
void aroundStep(CallContext& context, std::shared_ptr<AroundJob> job)
{
  whenResourcesReady(
    context,
    job->completion,
    [&context, job](SnapshotRenderer& renderer, const bool pending) {
      const auto& args = job->args;
      const auto width = size_t(args.get<int64_t>("width"));
      const auto height = size_t(args.get<int64_t>("height"));
      const auto view = viewJson(args);
      const auto spec = parseViewSpec(view, context.map());
      const auto index = job->next;
      const auto& aroundView = job->views[index];
      context.progress(
        double(index),
        double(job->views.size()),
        fmt::format("Rendering {}", aroundView.label));

      auto rendered = renderView(
        context,
        renderer,
        spec,
        [&](const auto& bounds) {
          const auto frameBounds = job->target ? job->target : bounds;
          return resolveCameraArg(
            context, aroundView.camera, width, height, [&]() { return frameBounds; });
        },
        width,
        height,
        false,
        index == 0);
      if (rendered.is_error())
      {
        job->completion(errorOf(rendered));
        return;
      }
      job->anyPending = job->anyPending || pending;
      auto result = std::move(rendered).value();

      auto annotations = annotate(
        context, result, args.getOr<Json>("annotations", Json(nullptr)), index == 0);
      if (annotations.is_error())
      {
        job->completion(errorOf(annotations));
        return;
      }

      context.addText(aroundView.label);
      auto image = outputImage(context, renderer, result.image, OutputSpec{});
      if (image.is_error())
      {
        job->completion(errorOf(image));
        return;
      }
      auto imageJson = Json{
        {"label", aroundView.label},
        {"snapshotId",
         recordSnapshot(context, result.camera.camera, width, height, view)},
        {"camera", toJson(result.camera.camera)},
        {"image", std::move(image).value()},
        {"counts", countsJson(result.counts)},
      };
      if (!annotations.value().is_null())
      {
        imageJson["annotations"] = std::move(annotations).value();
      }
      job->images.push_back(std::move(imageJson));

      job->next += 1;
      if (job->next < job->views.size())
      {
        aroundStep(context, job);
        return;
      }

      context.progress(double(job->views.size()), double(job->views.size()), "Done");
      warnPending(context, job->anyPending);
      job->completion(Json{
        {"images", std::move(job->images)},
        {"target", job->target ? toJson(*job->target) : Json(nullptr)},
      });
    });
}

void viewSnapshotsAround(
  CallContext& context, const Args& args, ToolCompletion completion)
{
  if (!context.host().snapshotRenderer())
  {
    completion(noRendererError());
    return;
  }

  if (args.has("ids") && args.has("box"))
  {
    completion(makeError(
      ErrorCode::InvalidArgument,
      "Pass either ids or box.",
      "ids use the bounds of the objects."));
    return;
  }
  auto target = std::optional<vm::bbox3d>{};
  if (args.has("ids"))
  {
    auto bounds = boundsOfIds(context, args.get<std::vector<std::string>>("ids"));
    if (bounds.is_error())
    {
      completion(errorOf(bounds));
      return;
    }
    target = bounds.value();
  }
  else if (args.has("box"))
  {
    target = args.get<vm::bbox3d>("box");
  }

  auto views = std::vector<AroundView>{};
  for (const auto& entry : args.get<Json>("views"))
  {
    if (entry.is_string())
    {
      const auto name = entry.get<std::string>();
      views.push_back(AroundView{aroundLabel(name), *namedAroundCamera(name)});
    }
    else
    {
      if (
        entry["camera"].is_string()
        && !context.session().agentCameras.contains(entry["camera"].get<std::string>()))
      {
        completion(
          unknownCameraError(context.session(), entry["camera"].get<std::string>()));
        return;
      }
      views.push_back(AroundView{entry["label"].get<std::string>(), entry["camera"]});
    }
  }
  if (views.empty() || views.size() > MaxAroundViews)
  {
    completion(makeError(
      ErrorCode::InvalidArgument,
      fmt::format("Pass 1 to {} views.", MaxAroundViews),
      "Split the views over several calls."));
    return;
  }

  auto job = std::make_shared<AroundJob>(
    AroundJob{args, std::move(completion), std::move(views), target});
  aroundStep(context, job);
}

// view_snapshot_compare

const KeptSnapshot* findKept(const Session& session, const std::string& name)
{
  const auto it = std::ranges::find_if(
    session.keptSnapshots, [&](const auto& snapshot) { return snapshot.name == name; });
  return it != session.keptSnapshots.end() ? &*it : nullptr;
}

ToolError unknownKeptError(const Session& session, const std::string& name)
{
  auto names = std::string{};
  for (const auto& snapshot : session.keptSnapshots)
  {
    names += (names.empty() ? "" : ", ") + snapshot.name;
  }
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("There is no kept snapshot named '{}'.", name),
    fmt::format(
      "Take one with view_snapshot keepAs: \"{}\". Kept snapshots: {}.",
      name,
      names.empty() ? "none" : names));
}

std::optional<ToolError> checkHistoryAvailable(CallContext& context, const size_t steps)
{
  if (const auto& transaction = context.documentState().transaction)
  {
    return makeError(
      ErrorCode::TransactionActive,
      fmt::format("The agent transaction '{}' is open.", transaction->name),
      "Commit or roll back the transaction first, or compare two kept snapshots.");
  }
  auto& map = context.map();
  if (map.commandProcessor().transactionDepth() > 0)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "A transaction is open in the editor (the user may be dragging).",
      "Try again when the user has finished the current action.");
  }
  if (context.host().busyState(context.document()) == BusyState::Busy)
  {
    return makeError(
      ErrorCode::OperationFailed,
      "The user is busy in the editor (e.g. a dialog is open).",
      "Try again when the user has finished the current action.");
  }
  const auto available = map.commandProcessor().undoCommandNames().size();
  if (available < steps)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "Only {} undo steps exist; cannot compare with {} steps back.", available, steps),
      "Use history_get to see the undo history.");
  }
  return std::nullopt;
}

Result<Json, ToolError> compareResult(
  CallContext& context,
  const RgbaImage& before,
  const RgbaImage& after,
  const int threshold)
{
  const auto diff = diffImages(before, after, threshold);
  if (!diff)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The images differ in size ({}x{} and {}x{}).",
        before.width,
        before.height,
        after.width,
        after.height),
      "Compare snapshots taken with the same width and height.");
  }

  const auto sideBySide =
    composeSideBySide({before, after}, CompareGap, Rgba8{255, 255, 255, 255});
  const auto sideBySidePng = encodePng(sideBySide);
  const auto maskPng = encodePng(diff->mask);
  if (!sideBySidePng || !maskPng)
  {
    return makeError(ErrorCode::InternalError, "Could not encode the images.");
  }
  context.addText("before | after");
  context.addImage(*sideBySidePng, "image/png");
  context.addText("changed pixels (magenta) on the dimmed after image");
  context.addImage(*maskPng, "image/png");

  auto bounds = Json(nullptr);
  if (diff->changedBounds)
  {
    bounds = Json{
      {"x", diff->changedBounds->x},
      {"y", diff->changedBounds->y},
      {"width", diff->changedBounds->width},
      {"height", diff->changedBounds->height},
    };
  }
  return Json{
    {"width", after.width},
    {"height", after.height},
    {"changedPixels", diff->changedPixels},
    {"changedRatio", roundForOutput(diff->changedRatio)},
    {"changedBounds", std::move(bounds)},
  };
}

/** Compares two kept snapshots, or a kept snapshot with the current state. */
void compareKept(CallContext& context, const Args& args, ToolCompletion completion)
{
  const auto beforeName = args.get<std::string>("before");
  const auto* before = findKept(context.session(), beforeName);
  if (!before)
  {
    completion(unknownKeptError(context.session(), beforeName));
    return;
  }
  const auto threshold = int(args.get<int64_t>("threshold"));

  if (args.has("after"))
  {
    const auto afterName = args.get<std::string>("after");
    const auto* after = findKept(context.session(), afterName);
    if (!after)
    {
      completion(unknownKeptError(context.session(), afterName));
      return;
    }
    auto result = compareResult(context, before->image, after->image, threshold);
    if (result.is_error())
    {
      completion(errorOf(result));
      return;
    }
    auto json = std::move(result).value();
    json["mode"] = "kept";
    json["before"] = beforeName;
    json["after"] = afterName;
    json["camera"] = toJson(after->camera);
    json["snapshotId"] =
      recordSnapshot(context, after->camera, after->width, after->height, after->view);
    completion(std::move(json));
    return;
  }

  if (before->documentId != context.documentInfo().id)
  {
    completion(makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The snapshot '{}' shows the document {}, not {}.",
        beforeName,
        before->documentId,
        context.documentInfo().id),
      fmt::format("Pass document: \"{}\".", before->documentId)));
    return;
  }
  if (!context.host().snapshotRenderer())
  {
    completion(noRendererError());
    return;
  }

  whenResourcesReady(
    context,
    completion,
    [&context, beforeName, threshold, completion](
      SnapshotRenderer& renderer, const bool pending) {
      const auto* kept = findKept(context.session(), beforeName);
      if (!kept)
      {
        completion(unknownKeptError(context.session(), beforeName));
        return;
      }
      const auto spec = parseViewSpec(kept->view, context.map());
      const auto camera = kept->camera;
      auto rendered = renderView(
        context,
        renderer,
        spec,
        [&](const auto&) -> Result<ResolvedCamera, ToolError> {
          return ResolvedCamera{camera, std::nullopt, nullptr};
        },
        kept->width,
        kept->height,
        true,
        false);
      if (rendered.is_error())
      {
        completion(errorOf(rendered));
        return;
      }
      warnPending(context, pending);

      auto result =
        compareResult(context, kept->image, rendered.value().image, threshold);
      if (result.is_error())
      {
        completion(errorOf(result));
        return;
      }
      auto json = std::move(result).value();
      json["mode"] = "kept";
      json["before"] = beforeName;
      json["after"] = nullptr;
      json["camera"] = toJson(camera);
      json["snapshotId"] =
        recordSnapshot(context, camera, kept->width, kept->height, kept->view);
      completion(std::move(json));
    });
}

/**
 * Renders the current state, undoes the given number of steps, renders again and redoes
 * them, all in one step so that no event loop runs in between.
 */
void compareUndo(CallContext& context, const Args& args, ToolCompletion completion)
{
  const auto steps = size_t(args.get<int64_t>("undoSteps"));
  if (auto error = checkHistoryAvailable(context, steps))
  {
    completion(*error);
    return;
  }
  if (!context.host().snapshotRenderer())
  {
    completion(noRendererError());
    return;
  }
  if (args.has("camera") && args.get<Json>("camera").is_string())
  {
    const auto name = args.get<Json>("camera").get<std::string>();
    if (!context.session().agentCameras.contains(name))
    {
      completion(unknownCameraError(context.session(), name));
      return;
    }
  }

  whenResourcesReady(
    context,
    completion,
    [&context, args, steps, completion](SnapshotRenderer& renderer, const bool pending) {
      if (auto error = checkHistoryAvailable(context, steps))
      {
        completion(*error);
        return;
      }

      const auto width = size_t(args.get<int64_t>("width"));
      const auto height = size_t(args.get<int64_t>("height"));
      const auto threshold = int(args.get<int64_t>("threshold"));
      const auto spec = parseViewSpec(viewJson(args), context.map());
      const auto cameraArg = args.getOr<Json>("camera", Json(nullptr));

      // after: the current state; its camera is used for both images
      auto after = renderView(
        context,
        renderer,
        spec,
        [&](const auto& bounds) {
          return resolveCameraArg(
            context, cameraArg, width, height, [&]() { return bounds; });
        },
        width,
        height,
        false,
        true);
      if (after.is_error())
      {
        completion(errorOf(after));
        return;
      }
      const auto camera = after.value().camera;

      auto& map = context.map();
      const auto& commandProcessor = map.commandProcessor();
      const auto modificationCount = map.modificationCount();
      const auto undoNames = commandProcessor.undoCommandNames();
      const auto redoNames = commandProcessor.redoCommandNames();

      auto undone = size_t(0);
      for (; undone < steps && map.canUndoCommand(); ++undone)
      {
        map.undoCommand();
      }
      auto before = renderView(
        context,
        renderer,
        spec,
        [&](const auto&) -> Result<ResolvedCamera, ToolError> { return camera; },
        width,
        height,
        true,
        false);
      for (size_t i = 0; i < undone && map.canRedoCommand(); ++i)
      {
        map.redoCommand();
      }

      const auto restored = map.modificationCount() == modificationCount
                            && commandProcessor.undoCommandNames() == undoNames
                            && commandProcessor.redoCommandNames() == redoNames;
      if (!restored)
      {
        context.warn(
          "HISTORY_NOT_RESTORED",
          "The document's history differs from before the comparison; check "
          "history_get.");
      }
      if (before.is_error())
      {
        completion(errorOf(before));
        return;
      }
      warnPending(context, pending);

      auto result =
        compareResult(context, before.value().image, after.value().image, threshold);
      if (result.is_error())
      {
        completion(errorOf(result));
        return;
      }
      auto json = std::move(result).value();
      json["mode"] = "undo";
      json["undoSteps"] = steps;
      json["undone"] =
        std::vector<std::string>(undoNames.begin(), undoNames.begin() + long(undone));
      json["camera"] = toJson(camera.camera);
      json["historyRestored"] = restored;
      json["snapshotId"] =
        recordSnapshot(context, camera.camera, width, height, viewJson(args));
      completion(std::move(json));
    });
}

void viewSnapshotCompare(
  CallContext& context, const Args& args, ToolCompletion completion)
{
  const auto kept = args.has("before");
  const auto undo = args.has("undoSteps");
  if (kept == undo)
  {
    completion(makeError(
      ErrorCode::InvalidArgument,
      "Pass either before (kept snapshots) or undoSteps.",
      "Keep a snapshot with view_snapshot keepAs, make the change, then compare with "
      "before; or compare the current state with undoSteps: 1."));
    return;
  }
  if (kept)
  {
    for (const auto* key : {"camera", "options", "isolate", "highlight"})
    {
      if (args.has(key))
      {
        completion(makeError(
          ErrorCode::InvalidArgument,
          fmt::format("{} is not used when comparing kept snapshots.", key),
          "The before snapshot's camera and options are used."));
        return;
      }
    }
    compareKept(context, args, std::move(completion));
  }
  else
  {
    compareUndo(context, args, std::move(completion));
  }
}

// view_snapshot_user

Json userViewJson(const UserView& view)
{
  return Json{
    {"id", view.id},
    {"visible", view.visible},
    {"width", view.width},
    {"height", view.height},
    {"camera", toJson(view.camera)},
  };
}

void viewSnapshotUser(CallContext& context, const Args& args, ToolCompletion completion)
{
  if (!context.host().snapshotRenderer())
  {
    completion(noRendererError());
    return;
  }
  auto output = parseOutputSpec(args);
  if (output.is_error())
  {
    completion(errorOf(output));
    return;
  }

  context.defer([&context, args, completion, output = output.value()]() {
    if (context.cancelled())
    {
      completion(cancelledError());
      return;
    }
    auto* renderer = context.host().snapshotRenderer();
    if (!renderer)
    {
      completion(noRendererError());
      return;
    }

    const auto views = renderer->userViews(context.document());
    auto list = Json::array();
    for (const auto& view : views)
    {
      list.push_back(userViewJson(view));
    }
    if (args.get<bool>("listOnly"))
    {
      completion(Json{{"views", std::move(list)}, {"view", nullptr}});
      return;
    }
    if (views.empty())
    {
      completion(makeError(
        ErrorCode::OperationFailed,
        "The document has no editor window with views.",
        "Use view_snapshot with an agent camera instead."));
      return;
    }

    const auto viewId = args.get<std::string>("view");
    const auto it =
      std::ranges::find_if(views, [&](const auto& view) { return view.id == viewId; });
    if (it == views.end())
    {
      auto ids = std::string{};
      for (const auto& view : views)
      {
        ids += (ids.empty() ? "" : ", ") + view.id;
      }
      completion(makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The editor window has no view '{}'.", viewId),
        fmt::format("Available views: {}.", ids)));
      return;
    }
    if (!it->visible)
    {
      completion(makeError(
        ErrorCode::OperationFailed,
        fmt::format("The view '{}' is not shown in the window's layout.", viewId),
        "Capture a visible view (see views), or copy its camera into an agent camera and "
        "use view_snapshot."));
      return;
    }

    auto image = renderer->captureUserView(context.document(), viewId);
    if (image.is_error())
    {
      completion(makeError(
        ErrorCode::OperationFailed,
        fmt::format("Could not capture the view '{}': {}", viewId, errorMessage(image)),
        "Use view_snapshot with an agent camera instead."));
      return;
    }

    auto imageJson = outputImage(context, *renderer, image.value(), output);
    if (imageJson.is_error())
    {
      completion(errorOf(imageJson));
      return;
    }
    completion(Json{
      {"snapshotId",
       recordSnapshot(
         context, it->camera, image.value().width, image.value().height, Json::object())},
      {"views", std::move(list)},
      {"view", viewId},
      {"camera", toJson(it->camera)},
      {"image", std::move(imageJson).value()},
    });
  });
}

} // namespace

bool SnapshotVisibility::drawsNode(const mdl::Node& node) const
{
  return nodes.contains(&node);
}

bool SnapshotVisibility::drawsFace(
  const mdl::BrushNode& brushNode, const size_t faceIndex) const
{
  return !faceFilter || faceFilter(brushNode, brushNode.brush().face(faceIndex));
}

SnapshotVisibility snapshotVisibility(
  mdl::Map& map, const IdRegistry& ids, const Json& view)
{
  const auto spec = parseViewSpec(view, map);
  auto scene = buildScene(map, ids, spec, true);
  auto result = SnapshotVisibility{};
  if (scene.is_success())
  {
    auto& resolved = scene.value();
    result.nodes.insert(resolved.scene.nodes.begin(), resolved.scene.nodes.end());
    result.faceFilter = std::move(resolved.scene.faceFilter);
  }
  return result;
}

// map_plan_view image form

ToolResult renderPlanImage(CallContext& context, const PlanImageRequest& request)
{
  auto* renderer = context.host().snapshotRenderer();
  if (!renderer)
  {
    auto error = noRendererError();
    error.hint = "Use map_plan_view with format: \"text\".";
    return error;
  }

  const auto regionWidth = double(request.columns) * request.cellSize;
  const auto regionHeight = double(request.rows) * request.cellSize;
  auto width = size_t(0);
  auto height = size_t(0);
  auto zoom = 1.0;
  if (request.width || request.imageHeight)
  {
    width = request.width.value_or(request.imageHeight.value_or(0));
    height = request.imageHeight.value_or(request.width.value_or(0));
    zoom = std::min(double(width) / regionWidth, double(height) / regionHeight);
  }
  else
  {
    // up to 16 pixels per cell, at most 1024 pixels per side
    const auto longest = std::max(request.columns, request.rows);
    const auto pixelsPerCell = std::clamp(size_t(1024) / longest, size_t(1), size_t(16));
    width = std::clamp(request.columns * pixelsPerCell, MinImageSize, MaxImageSize);
    height = std::clamp(request.rows * pixelsPerCell, MinImageSize, MaxImageSize);
    zoom = std::min(double(width) / regionWidth, double(height) / regionHeight);
  }

  auto spec = ViewSpec{};
  spec.options.gridSize = context.map().grid().actualSize();
  spec.options.edges = true;
  spec.includeHidden = request.includeHidden;

  auto scene = buildScene(context.map(), context.ids(), spec, false);
  if (scene.is_error())
  {
    return errorOf(scene);
  }
  auto resolved = std::move(scene).value();
  resolved.scene.markers = request.markers;

  // looking down from the slice: geometry above it is behind the camera
  auto camera = AgentCamera{};
  camera.projection = CameraProjection::Orthographic;
  camera.position = vm::vec3d{
    request.originX + regionWidth / 2.0,
    request.originY + regionHeight / 2.0,
    request.height};
  camera.direction = vm::vec3d{0, 0, -1};
  camera.up = vm::vec3d{0, 1, 0};
  camera.zoom = zoom;
  camera.nearPlane = 0.0;
  camera.farPlane = request.floorDepth;

  auto snapshotRequest = SnapshotRequest{};
  snapshotRequest.camera = camera;
  snapshotRequest.width = width;
  snapshotRequest.height = height;
  snapshotRequest.options = spec.options;
  snapshotRequest.scene = std::move(resolved.scene);

  warnPending(context, renderer->resourcesPending(context.document()));
  auto image = renderer->render(context.document(), snapshotRequest);
  if (image.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format("Rendering failed: {}", errorMessage(image)),
      "Use map_plan_view with format: \"text\".");
  }

  auto imageJson = outputImage(context, *renderer, image.value(), OutputSpec{});
  if (imageJson.is_error())
  {
    return errorOf(imageJson);
  }
  return Json{
    {"snapshotId",
     recordSnapshot(
       context,
       camera,
       width,
       height,
       Json{{"options", Json{{"includeHidden", request.includeHidden}}}})},
    {"image", std::move(imageJson).value()},
    {"camera", toJson(camera)},
    {"counts", countsJson(resolved.counts)},
  };
}

void registerSnapshotTools(ToolRegistry& registry)
{
  const auto nameField = field("name", string().matching(CameraNamePattern))
                           .required()
                           .describe("Camera name (letters, digits, _ . -)");
  const auto cameraField =
    field("camera", cameraArgSchema())
      .describe(
        "An agent camera name or an inline camera (same forms as agent_camera_set); "
        "default: frame everything drawn from yaw 45, pitch -30");
  const auto countsField = field("counts", countsSchema()).describe("Drawn objects");
  const auto snapshotIdField =
    field("snapshotId", string())
      .describe(
        "Pass as 'snapshot' to view_pick to find what a pixel of the image shows");
  const auto annotationsOutputField =
    field("annotations", any())
      .describe(
        "What was drawn: {labels, labelled, labelsSkipped, grid: {step, box, floor, "
        "walls, lines, labels} | null, compass, player: {feet, width, height, eyeHeight, "
        "visible} | null}");

  registry.add(
    ToolDef{"agent_camera_set"}
      .title("Set Agent Camera")
      .description(
        "Creates or replaces a named camera of this session for view_snapshot and "
        "the other snapshot tools; it never moves the user's views (camera_set "
        "does). Forms: perspective with position and lookAt, direction or "
        "yaw/pitch (degrees; yaw counterclockwise from +x, pitch positive up) and "
        "fov; orthographic with view top/front/side, center and zoom (image pixels "
        "per map unit); or a helper: frame {ids | box} (fit objects, looking along "
        "yaw/pitch), orbit {target | ids, yaw, pitch, distance}, eyeHeight {point} "
        "(stand on the floor below point at the game's player eye height). Framing "
        "assumes the image size width x height. Returns the resolved camera "
        "(position, direction, yaw, pitch, ...). Store cameras across sessions "
        "with map_manifest_set saveCameras. Examples: {\"name\": \"hall\", "
        "\"camera\": {\"eyeHeight\": {\"point\": [256, 256, 64]}, \"yaw\": 90}}; "
        "{\"name\": \"plan\", \"camera\": {\"view\": \"top\", \"frame\": {\"ids\": "
        "[\"group:3\"]}}}")
      .input(object({
        nameField,
        field("camera", cameraSpecSchema())
          .required()
          .describe(
            "The camera: perspective (position with lookAt, direction or yaw/pitch), "
            "orthographic (view with center or frame) or a helper (frame, orbit, "
            "eyeHeight)"),
        sizeFields(DefaultImageWidth, DefaultImageHeight)[0],
        sizeFields(DefaultImageWidth, DefaultImageHeight)[1],
      }))
      .output(object({
        field("name", string()),
        field("camera", cameraOutputSchema()).describe("The resolved camera"),
        field("replaced", boolean()).describe("Whether a camera of that name existed"),
        field("placement", any()).describe("eyeHeight: {floor, eyeHeight, game}"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(agentCameraSet));

  registry.add(
    ToolDef{"agent_camera_get"}
      .title("Get Agent Camera")
      .description(
        "Returns a named agent camera of this session (read-only), resolved to "
        "position, direction, up, yaw, pitch, fov or view and zoom. Example: "
        "{\"name\": \"hall\"}")
      .input(object({nameField}))
      .output(object({
        field("name", string()),
        field("camera", cameraOutputSchema()).describe("The resolved camera"),
      }))
      .mutation(Mutation::None)
      .idempotent()
      .handler(agentCameraGet));

  registry.add(
    ToolDef{"agent_camera_list"}
      .title("List Agent Cameras")
      .description(
        "Lists the agent cameras and the kept snapshots (view_snapshot keepAs) of "
        "this session (read-only). Example: {}")
      .input(object({}))
      .output(object({
        field("cameras", array(any())).describe("{name, camera}"),
        field("keptSnapshots", array(any()))
          .describe("{name, document, width, height}; names for view_snapshot_compare "
                    "and view_pick"),
      }))
      .mutation(Mutation::None)
      .idempotent()
      .handler(agentCameraList));

  registry.add(
    ToolDef{"agent_camera_delete"}
      .title("Delete Agent Camera")
      .description(
        "Deletes a named agent camera of this session (session state only; the map "
        "and the manifest are not changed). Example: {\"name\": \"hall\"}")
      .input(object({nameField}))
      .output(object({field("deleted", string()).describe("The deleted camera's name")}))
      .mutation(Mutation::None)
      .handler(agentCameraDelete));

  auto snapshotInput = std::vector<Field>{
    cameraField,
    sizeFields(DefaultImageWidth, DefaultImageHeight)[0],
    sizeFields(DefaultImageWidth, DefaultImageHeight)[1],
  };
  for (const auto& f : formatFields())
  {
    snapshotInput.push_back(f);
  }
  for (const auto& f : viewFields())
  {
    snapshotInput.push_back(f);
  }
  snapshotInput.push_back(
    field("keepAs", string().matching(CameraNamePattern))
      .describe("Keep the image under this name for view_snapshot_compare and view_pick "
                "(at most 8 are kept; the image is kept without annotations)"));
  snapshotInput.push_back(
    field("annotations", annotationsSchema())
      .describe("Draw labels, a coordinate grid, a compass or a player box for scale"));

  registry.add(
    ToolDef{"view_snapshot"}
      .title("View Snapshot")
      .description(
        "Renders an image of the map offscreen and returns it as image content, "
        "with a snapshotId; read-only, the user's views, filters and camera are "
        "not changed. camera: an agent camera name (agent_camera_set) or an inline "
        "camera of the same forms; default: frame everything drawn from yaw 45, "
        "pitch -30. Objects hidden in the editor are not drawn unless "
        "options.includeHidden, but the editor's view filters (view_options_set) "
        "do not apply. options: faceMode, shading, fog, edges, hideTags (e.g. "
        "trigger, clip, skip, hint), hideClassnames, pointEntities, brushEntities, "
        "patches, entityModels, bounds, classnames, entityLinks, leakPath, grid, "
        "axes; isolate draws only the given objects; highlight tints objects. "
        "annotations draws object labels (id, classname or group name, size), a "
        "coordinate grid on floor and walls, a compass and a player-sized box. "
        "keepAs keeps the image for view_snapshot_compare. Pass the snapshotId as "
        "'snapshot' to view_pick to find what a pixel shows. Example: {\"camera\": "
        "{\"frame\": {\"ids\": [\"brush:12\"]}, \"yaw\": 30}, \"options\": "
        "{\"hideTags\": [\"trigger\"]}, \"annotations\": {\"labels\": true, "
        "\"compass\": true}}")
      .input(object(snapshotInput))
      .output(object({
        snapshotIdField,
        field("image", imageOutputSchema()).describe("The image's size and format"),
        field("camera", cameraOutputSchema()).describe("The resolved camera"),
        field("cameraName", any()).describe("The agent camera used, or null"),
        countsField,
        field("keptAs", any()).describe("keepAs, or null"),
        field("resourcesPending", boolean())
          .describe("Materials or models were still loading"),
        annotationsOutputField,
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .asyncHandler(viewSnapshot));

  auto aroundInput = std::vector<Field>{
    field("ids", array(objectId()).nonEmpty()).describe("Objects to look at"),
    field("box", box()).describe("Region to look at (default: everything drawn)"),
    field(
      "views",
      array(oneOf({
        enumOf(
          {"north",
           "east",
           "south",
           "west",
           "northeast",
           "northwest",
           "southeast",
           "southwest",
           "above",
           "top",
           "front",
           "side"}),
        object({
          field("label", string().nonEmpty()).required().describe("The image's label"),
          field("camera", cameraArgSchema())
            .required()
            .describe("An agent camera name or an inline camera"),
        }),
      })))
      .defaultsTo(Json::array({"north", "east", "south", "west", "top"}))
      .describe(
        "Views: side names (camera on that side, looking at the target from slightly "
        "above), above (perspective, looking down), top/front/side (orthographic), or "
        "{label, camera}; at most 12"),
    sizeFields(640, 480)[0],
    sizeFields(640, 480)[1],
  };
  for (const auto& f : viewFields())
  {
    aroundInput.push_back(f);
  }
  aroundInput.push_back(
    field("annotations", annotationsSchema())
      .describe("Annotations drawn on every image, as view_snapshot"));

  registry.add(
    ToolDef{"view_snapshots_around"}
      .title("View Snapshots Around")
      .description(
        "Renders several labelled images around objects or a region in one call "
        "(read-only), e.g. from the four sides and from the top; each image is "
        "preceded by a text label. Target: ids or box (default: everything drawn). "
        "views: side names (the camera stands on that side looking at the target "
        "from slightly above), above (perspective looking down), top/front/side "
        "(orthographic), or {label, camera} with an agent camera name or inline "
        "camera; at most 12. Same options, isolate, highlight and annotations as "
        "view_snapshot. Returns per image its label, snapshotId (for view_pick), "
        "camera and counts. Reports progress per image and can be cancelled. "
        "Example: {\"ids\": [\"group:3\"], \"views\": [\"north\", \"east\", "
        "\"south\", \"west\", \"top\"]}")
      .input(object(aroundInput))
      .output(object({
        field("images", array(any()))
          .describe("{label, snapshotId, camera, image, counts, annotations}"),
        field("target", any()).describe("The framed box, or null for everything"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .asyncHandler(viewSnapshotsAround));

  auto compareInput = std::vector<Field>{
    field("before", string()).describe("A kept snapshot (view_snapshot keepAs)"),
    field("after", string())
      .describe(
        "Another kept snapshot; default: render the current state with the before "
        "snapshot's camera and options"),
    field("undoSteps", integer().min(1).max(100))
      .describe(
        "Compare the current state with the state this many undo steps back (renders, "
        "undoes, renders, redoes; the history is left unchanged)"),
    cameraField,
    sizeFields(DefaultImageWidth, DefaultImageHeight)[0],
    sizeFields(DefaultImageWidth, DefaultImageHeight)[1],
    field("threshold", integer().min(0).max(255).defaultsTo(8))
      .describe("A pixel changed if a channel differs by more than this"),
  };
  for (const auto& f : viewFields())
  {
    compareInput.push_back(f);
  }

  registry.add(
    ToolDef{"view_snapshot_compare"}
      .title("Compare Snapshots")
      .description(
        "Shows the same camera before and after a change (read-only): returns the "
        "two images side by side (before | after) and a changed-pixel mask, with "
        "the number, ratio and bounding box of changed pixels. Either compare kept "
        "snapshots ('before' from view_snapshot keepAs, optional 'after'; default: "
        "the current state rendered with the before snapshot's camera and "
        "options), or pass undoSteps to compare the current state with an earlier "
        "one (renders, undoes, renders and redoes; the history and the map are "
        "left as they were; refused while a transaction is open). camera and "
        "options apply only to undoSteps. The returned snapshotId is the after "
        "image for view_pick, in the coordinates of one image, not the "
        "side-by-side picture. Examples: {\"undoSteps\": 1, \"camera\": \"hall\"}; "
        "{\"before\": \"hall_before\"}")
      .input(object(compareInput))
      .output(object({
        field("mode", enumOf({"kept", "undo"}))
          .describe("kept: kept snapshots compared; undo: undoSteps compared"),
        field("width", integer()).describe("Width of one image in pixels"),
        field("height", integer()).describe("Height of one image in pixels"),
        field("changedPixels", integer()).describe("Number of changed pixels"),
        field("changedRatio", number()).describe("Changed pixels / all pixels (0-1)"),
        field("changedBounds", any())
          .describe("{x, y, width, height} in pixels, or null"),
        field("camera", cameraOutputSchema()).describe("The camera of both images"),
        field("before", any()).describe("The before snapshot's name, or null"),
        field("after", any()).describe("The after snapshot's name, or null"),
        field("undoSteps", integer()).describe("undo mode: the steps compared across"),
        field("undone", array(string())).describe("The undo steps compared across"),
        field("historyRestored", boolean())
          .describe("undo mode: the undo history was restored afterwards"),
        field("snapshotId", string())
          .describe(
            "The after image, for view_pick; pixels are in the coordinates of one "
            "image, not the side-by-side picture"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .asyncHandler(viewSnapshotCompare));

  auto userInput = std::vector<Field>{
    field("view", enumOf({"3d", "xy", "xz", "yz"}).defaultsTo("3d"))
      .describe("The editor view to capture"),
    field("listOnly", boolean().defaultsTo(false))
      .describe("Only list the views and their cameras"),
  };
  for (const auto& f : formatFields())
  {
    userInput.push_back(f);
  }

  registry.add(
    ToolDef{"view_snapshot_user"}
      .title("Capture User View")
      .description(
        "Captures what the user currently sees in one of the editor views (3d, xy, "
        "xz, yz) of the document's window, without changing anything, and lists "
        "the views with their cameras. Returns the image as image content and a "
        "snapshotId for view_pick. Copy a listed camera into agent_camera_set to "
        "render it with your own options through view_snapshot. listOnly returns "
        "only the list. Examples: {\"view\": \"3d\"}; {\"listOnly\": true}")
      .input(object(userInput))
      .output(object({
        field("views", array(any())).describe("{id, visible, width, height, camera}"),
        field("view", any()).describe("The captured view, or null"),
        field("camera", cameraOutputSchema()).describe("The captured view's camera"),
        field("image", imageOutputSchema()).describe("The image's size and format"),
        field("snapshotId", string()).describe("Pass as 'snapshot' to view_pick"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .asyncHandler(viewSnapshotUser));
}

} // namespace tb::mcp
