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

#include "mcp/tools/GeometryTools.h"

#include "GeometryUtils.h"
#include "NodeJson.h"
#include "ToolUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/CircleShape.h"
#include "mdl/EditorContext.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "ui/DrawShapeToolExtensions.h"
#include "ui/DrawShapeToolParameters.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

// Common helpers

/** The box given by the arguments `min` and `max`. */
vm::bbox3d boxArgument(const Args& args)
{
  return vm::bbox3d{args.get<vm::vec3d>("min"), args.get<vm::vec3d>("max")};
}

ToolError invalidArgument(std::string message, std::string hint)
{
  return makeError(ErrorCode::InvalidArgument, std::move(message), std::move(hint));
}

/**
 * Adds new brushes like the editor's brush tools do: deselects everything, adds the
 * brushes to the open group or the current layer and selects them. With a group name,
 * the new brushes are grouped (the group is selected then). Returns the added brushes.
 */
Result<std::vector<mdl::Node*>, ToolError> addAndSelectBrushes(
  CallContext& context,
  std::vector<mdl::Brush> brushes,
  const std::optional<std::string>& groupName,
  mdl::GroupNode** group = nullptr)
{
  auto& map = context.map();
  mdl::deselectAll(map);

  const auto count = brushes.size();
  auto added = addBrushes(map, std::move(brushes));
  if (added.size() != count)
  {
    return context.operationFailed("The new brushes could not be added to the map.");
  }

  mdl::selectNodes(map, added);
  if (groupName)
  {
    auto* groupNode = mdl::groupSelectedNodes(map, *groupName);
    if (!groupNode)
    {
      return context.operationFailed("The new brushes could not be grouped.");
    }
    if (group)
    {
      *group = groupNode;
    }
  }
  return added;
}

Json groupJson(CallContext& context, const mdl::GroupNode* group)
{
  return group ? Json(context.ids().format(*group)) : Json(nullptr);
}

void setMaterial(std::vector<mdl::Brush>& brushes, const std::string& material)
{
  for (auto& brush : brushes)
  {
    for (auto& face : brush.faces())
    {
      face.setMaterialName(material);
    }
  }
}

vm::bbox3d shrink(const vm::bbox3d& box, const double amount)
{
  auto result = box;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto delta = std::min(amount, (box.max[i] - box.min[i]) / 4.0);
    result.min[i] += delta;
    result.max[i] -= delta;
  }
  return result;
}

Schema createdOutput(std::vector<Field> fields)
{
  return object(std::move(fields));
}

Field objectsField()
{
  return field("objects", array(any()))
    .required()
    .describe("Summaries of the created brushes (id, kind, label, bounds, layer, ...)");
}

Field groupField()
{
  return field("group", any()).describe("Id of the group created with `group`, or null");
}

// brush_create_box

ToolResult brushCreateBox(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto box = boxArgument(args);
  if (auto error = checkBox(map, box))
  {
    error->details["min"] = toJson(box.min);
    error->details["max"] = toJson(box.max);
    return *error;
  }

  const auto material = materialArgument(context, args);
  auto brush = brushBuilder(map).createCuboid(box, material);
  if (brush.is_error())
  {
    return geometryError(
      "The box " + toJson(box).dump() + " does not make a valid brush.",
      errorMessage(brush),
      {},
      "Check that min is smaller than max on every axis.");
  }

  auto brushes = std::vector<mdl::Brush>{};
  brushes.push_back(std::move(brush).value());
  auto added = addAndSelectBrushes(context, std::move(brushes), std::nullopt);
  if (added.is_error())
  {
    return errorOf(added);
  }

  const auto& node = *added.value().front();
  return Json{
    {"brush", context.ids().format(node)},
    {"bounds", toJson(node.logicalBounds())},
    {"object", nodeSummary(node, context.ids())},
  };
}

// brush_create_shape

constexpr auto ShapeParameterKeys = std::array<std::string_view, 11>{
  "axis",
  "circleMode",
  "sides",
  "precision",
  "hollow",
  "thickness",
  "spandrel",
  "rings",
  "subdivision",
  "stepHeight",
  "stairDirection",
};

/** The shape parameters that affect the given shape with the given arguments. */
std::vector<std::string_view> relevantShapeParameters(
  const std::string& shape, const std::string& circleMode, const bool hollow)
{
  auto result = std::vector<std::string_view>{};
  const auto addCircle = [&]() {
    result.push_back("axis");
    result.push_back("circleMode");
    result.push_back(circleMode == "scalable" ? "precision" : "sides");
  };

  if (shape == "stairs")
  {
    result = {"stepHeight", "stairDirection"};
  }
  else if (shape == "arch")
  {
    addCircle();
    result.push_back("thickness");
    result.push_back("spandrel");
  }
  else if (shape == "cylinder")
  {
    addCircle();
    result.push_back("hollow");
    if (hollow)
    {
      result.push_back("thickness");
    }
  }
  else if (shape == "cone")
  {
    addCircle();
  }
  else if (shape == "uvSphere")
  {
    addCircle();
    if (circleMode != "scalable")
    {
      result.push_back("rings");
    }
  }
  else if (shape == "icoSphere")
  {
    result = {"subdivision"};
  }
  return result;
}

vm::axis::type axisFromString(const std::string& axis)
{
  return axis == "x" ? vm::axis::x : axis == "y" ? vm::axis::y : vm::axis::z;
}

ui::DrawShapeToolParameters::StairDirection stairDirectionFromString(
  const std::string& direction)
{
  using StairDirection = ui::DrawShapeToolParameters::StairDirection;
  return direction == "-x"   ? StairDirection::NegX
         : direction == "+y" ? StairDirection::PosY
         : direction == "-y" ? StairDirection::NegY
                             : StairDirection::PosX;
}

std::unique_ptr<ui::DrawShapeToolExtension> shapeExtension(
  const std::string& shape, ui::MapDocument& document)
{
  if (shape == "stairs")
  {
    return std::make_unique<ui::DrawShapeToolStairsExtension>(document);
  }
  if (shape == "arch")
  {
    return std::make_unique<ui::DrawShapeToolArchExtension>(document);
  }
  if (shape == "cylinder")
  {
    return std::make_unique<ui::DrawShapeToolCylinderExtension>(document);
  }
  if (shape == "cone")
  {
    return std::make_unique<ui::DrawShapeToolConeExtension>(document);
  }
  if (shape == "uvSphere")
  {
    return std::make_unique<ui::DrawShapeToolUvSphereExtension>(document);
  }
  if (shape == "icoSphere")
  {
    return std::make_unique<ui::DrawShapeToolIcoSphereExtension>(document);
  }
  return std::make_unique<ui::DrawShapeToolCuboidExtension>(document);
}

/** The extents of the cross section of a circular shape, i.e. perpendicular to `axis`. */
std::array<double, 2> crossSection(const vm::bbox3d& box, const vm::axis::type axis)
{
  const auto size = box.size();
  return axis == vm::axis::x   ? std::array{size.y(), size.z()}
         : axis == vm::axis::y ? std::array{size.x(), size.z()}
                               : std::array{size.x(), size.y()};
}

/** Builds and validates the shape tool parameters from the arguments. */
Result<ui::DrawShapeToolParameters, ToolError> shapeParameters(
  const Args& args, const std::string& shape, const vm::bbox3d& box)
{
  auto parameters = ui::DrawShapeToolParameters{};

  // Arches stand upright by default: the tunnel runs along x and the arch rises along z.
  const auto axis =
    axisFromString(args.getOr<std::string>("axis", shape == "arch" ? "x" : "z"));
  parameters.setAxis(axis);

  const auto circleMode = args.getOr<std::string>("circleMode", "edgeAligned");
  const auto sides = size_t(args.getOr<int64_t>("sides", 8));
  const auto precision = size_t(args.getOr<int64_t>("precision", 0));
  parameters.setCircleShape(
    circleMode == "scalable"        ? mdl::CircleShape{mdl::ScalableCircle{precision}}
    : circleMode == "vertexAligned" ? mdl::CircleShape{mdl::VertexAlignedCircle{sides}}
                                    : mdl::CircleShape{mdl::EdgeAlignedCircle{sides}});

  const auto hollow = args.getOr<bool>("hollow", false);
  parameters.setHollow(hollow);

  const auto thickness = args.getOr<double>("thickness", 16.0);
  if (!(thickness > 0.0))
  {
    return invalidArgument(
      "thickness must be greater than 0, got " + std::to_string(thickness) + ".",
      "Pass a wall thickness such as 16.");
  }
  parameters.setThickness(thickness);

  const auto extents = crossSection(box, axis);
  if (shape == "cylinder" && hollow)
  {
    const auto limit = std::min(extents[0], extents[1]) / 2.0;
    if (!(thickness < limit))
    {
      return invalidArgument(
        "thickness " + std::to_string(thickness)
          + " is too large for a hollow cylinder with a cross section of "
          + std::to_string(extents[0]) + " x " + std::to_string(extents[1])
          + "; it must be less than half the smaller extent (" + std::to_string(limit)
          + ").",
        "Use a smaller thickness or larger bounds, or set hollow to false.");
    }
  }
  if (shape == "arch")
  {
    // the arch is the upper half of an ellipse spanning the horizontal extent and twice
    // the vertical extent of the cross section
    const auto span = extents[0];
    const auto height = extents[1];
    const auto limit = std::min(span / 2.0, height);
    if (!(thickness < limit))
    {
      return invalidArgument(
        "thickness " + std::to_string(thickness)
          + " is too large for an arch with a span of " + std::to_string(span)
          + " and a height of " + std::to_string(height)
          + "; it must be less than half the span and less than the height ("
          + std::to_string(limit) + ").",
        "Use a smaller thickness or larger bounds.");
    }
  }

  parameters.setCreateSpandrel(args.getOr<bool>("spandrel", false));
  parameters.setNumRings(size_t(args.getOr<int64_t>("rings", 8)));
  parameters.setAccuracy(size_t(args.getOr<int64_t>("subdivision", 1)));

  const auto stepHeight = args.getOr<double>("stepHeight", 16.0);
  if (!(stepHeight > 0.0))
  {
    return invalidArgument(
      "stepHeight must be greater than 0, got " + std::to_string(stepHeight) + ".",
      "Pass the rise of one step, e.g. 16.");
  }
  parameters.setStepHeight(stepHeight);
  parameters.setStairDirection(
    stairDirectionFromString(args.getOr<std::string>("stairDirection", "+x")));

  return parameters;
}

ToolResult brushCreateShape(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto shape = args.get<std::string>("shape");
  const auto box = boxArgument(args);
  if (auto error = checkBox(map, box))
  {
    error->details["min"] = toJson(box.min);
    error->details["max"] = toJson(box.max);
    return *error;
  }

  const auto circleMode = args.getOr<std::string>("circleMode", "edgeAligned");
  const auto relevant =
    relevantShapeParameters(shape, circleMode, args.getOr<bool>("hollow", false));
  for (const auto key : ShapeParameterKeys)
  {
    if (args.has(key) && std::ranges::find(relevant, key) == relevant.end())
    {
      context.warn(
        "IGNORED_ARGUMENT",
        "'" + std::string{key} + "' has no effect on shape '" + shape
          + "' with these arguments.");
    }
  }

  auto parameters = shapeParameters(args, shape, box);
  if (parameters.is_error())
  {
    return errorOf(parameters);
  }
  if (shape == "stairs" && parameters.value().stepHeight() >= box.size().z())
  {
    context.warn(
      "SINGLE_STEP",
      "stepHeight " + std::to_string(parameters.value().stepHeight())
        + " is not less than the height of the bounds (" + std::to_string(box.size().z())
        + "), so the stairs have a single step.");
  }

  const auto material = materialArgument(context, args);
  const auto extension = shapeExtension(shape, context.document());
  auto brushes = extension->createBrushes(box, parameters.value());
  if (brushes.is_error())
  {
    return geometryError(
      "The " + shape + " in " + toJson(box).dump() + " could not be built.",
      errorMessage(brushes),
      {},
      "Make the bounds larger or use fewer sides, rings or subdivisions.");
  }
  if (brushes.value().empty())
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "The " + shape + " in " + toJson(box).dump() + " yields no brushes.",
      "Make the bounds larger along the axes of the cross section.");
  }

  // The extensions use the current material; apply the requested one to the result
  // instead of changing the editor's current material.
  auto result = std::move(brushes).value();
  setMaterial(result, material);

  auto* group = static_cast<mdl::GroupNode*>(nullptr);
  auto added = addAndSelectBrushes(
    context, std::move(result), args.getOptional<std::string>("group"), &group);
  if (added.is_error())
  {
    return errorOf(added);
  }

  warnNonIntegerVertices(context, added.value());
  return Json{
    {"brushes", formatIds(added.value(), context.ids())},
    {"count", added.value().size()},
    {"group", groupJson(context, group)},
    {"objects", nodeSummaries(added.value(), context.ids())},
  };
}

// brush_create_hull

/**
 * Explains why the points do not span a volume, or returns nullopt if they do: the
 * points must not all coincide, lie on one line or lie in one plane.
 */
std::optional<std::string> degeneratePointsReason(const std::vector<vm::vec3d>& points)
{
  constexpr auto epsilon = 0.001;

  const auto& p0 = points.front();
  const auto farthest = [&](const auto& distance) {
    auto best = size_t{0};
    auto bestDistance = 0.0;
    for (size_t i = 0; i < points.size(); ++i)
    {
      if (const auto d = distance(points[i]); d > bestDistance)
      {
        best = i;
        bestDistance = d;
      }
    }
    return std::pair{best, bestDistance};
  };

  const auto [i1, d1] = farthest([&](const auto& p) { return vm::length(p - p0); });
  if (d1 < epsilon)
  {
    return "all points coincide";
  }

  const auto direction = vm::normalize(points[i1] - p0);
  const auto [i2, d2] =
    farthest([&](const auto& p) { return vm::length(vm::cross(p - p0, direction)); });
  if (d2 < epsilon)
  {
    return "all points lie on one line";
  }

  const auto normal = vm::normalize(vm::cross(direction, points[i2] - p0));
  const auto [i3, d3] =
    farthest([&](const auto& p) { return std::abs(vm::dot(p - p0, normal)); });
  if (d3 < epsilon)
  {
    return "all points lie in one plane";
  }

  return std::nullopt;
}

ToolResult brushCreateHull(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto pointsJson = args.get<Json>("points");
  auto points = std::vector<vm::vec3d>{};
  for (const auto& point : pointsJson)
  {
    points.push_back(*vec3FromJson(point));
  }

  if (points.size() < 4)
  {
    auto error = makeError(
      ErrorCode::InvalidGeometry,
      "A convex hull needs at least 4 points that do not lie in one plane, got "
        + std::to_string(points.size()) + ".",
      "Pass at least 4 points, e.g. the corners of a tetrahedron.");
    error.details["points"] = pointsJson;
    return error;
  }

  const auto& worldBounds = map.worldBounds();
  auto outside = Json::array();
  for (size_t i = 0; i < points.size(); ++i)
  {
    if (!worldBounds.contains(points[i]))
    {
      outside.push_back(i);
    }
  }
  if (!outside.empty())
  {
    auto error = makeError(
      ErrorCode::OutOfWorldBounds,
      "The points at indices " + outside.dump() + " lie outside the world bounds "
        + toJson(worldBounds).dump() + ".",
      "Move the points inside the world bounds.");
    error.details["pointIndices"] = std::move(outside);
    return error;
  }

  if (const auto reason = degeneratePointsReason(points))
  {
    auto error = makeError(
      ErrorCode::InvalidGeometry,
      "The points do not enclose a volume: " + *reason + ".",
      "Add a point off that " + std::string{reason->ends_with("plane") ? "plane" : "line"}
        + " so that the points span all three dimensions.");
    error.details["points"] = pointsJson;
    return error;
  }

  const auto material = materialArgument(context, args);
  auto brush = brushBuilder(map).createBrush(points, material);
  if (brush.is_error())
  {
    auto error = geometryError(
      "The convex hull of the points does not make a valid brush.",
      errorMessage(brush),
      {},
      "Spread the points further apart so that the hull has no tiny faces.");
    error.details["points"] = pointsJson;
    return error;
  }

  auto brushes = std::vector<mdl::Brush>{};
  brushes.push_back(std::move(brush).value());
  auto added = addAndSelectBrushes(context, std::move(brushes), std::nullopt);
  if (added.is_error())
  {
    return errorOf(added);
  }

  const auto& node = static_cast<const mdl::BrushNode&>(*added.value().front());
  const auto vertexCount = node.brush().vertexCount();
  if (vertexCount < points.size())
  {
    context.warn(
      "POINTS_INSIDE_HULL",
      std::to_string(points.size() - vertexCount)
        + " of the points are not vertices of the hull (they lie inside it or on its "
          "faces or edges).",
      {context.ids().format(node)});
  }
  warnNonIntegerVertices(context, added.value());

  return Json{
    {"brush", context.ids().format(node)},
    {"bounds", toJson(node.logicalBounds())},
    {"vertexCount", vertexCount},
    {"object", nodeSummary(node, context.ids())},
  };
}

// room_create

struct RoomPart
{
  std::string role;
  vm::bbox3d bounds;
  std::string material;
};

ToolResult roomCreate(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto inner = boxArgument(args);
  if (auto error = checkBox(map, inner, "The inner box"))
  {
    error->details["min"] = toJson(inner.min);
    error->details["max"] = toJson(inner.max);
    return *error;
  }

  const auto thickness = args.getOr<double>("thickness", map.grid().actualSize());
  if (!(thickness > 0.0))
  {
    return invalidArgument(
      "thickness must be greater than 0, got " + std::to_string(thickness) + ".",
      "Pass a wall thickness such as 16.");
  }

  const auto delta = vm::vec3d{thickness, thickness, thickness};
  const auto outer = vm::bbox3d{inner.min - delta, inner.max + delta};
  if (auto error = checkBox(map, outer, "The room including its walls"))
  {
    error->details["outer"] = toJson(outer);
    return *error;
  }

  const auto material = materialArgument(context, args);
  const auto surfaceMaterial = [&](const std::string_view key) {
    return args.has(key) ? materialArgument(context, args, key) : material;
  };
  const auto floorMaterial = surfaceMaterial("floor");
  const auto ceilingMaterial = surfaceMaterial("ceiling");
  const auto wallMaterial = surfaceMaterial("walls");

  const auto& a = inner.min;
  const auto& b = inner.max;
  const auto& o = outer.min;
  const auto& p = outer.max;

  // Floor and ceiling cover the whole outer footprint; the walls along x (west, east)
  // run the full outer depth and the walls along y (south, north) fit between them, so
  // the corners are sealed and no two brushes overlap.
  const auto parts = std::vector<RoomPart>{
    {"floor", {{o.x(), o.y(), o.z()}, {p.x(), p.y(), a.z()}}, floorMaterial},
    {"ceiling", {{o.x(), o.y(), b.z()}, {p.x(), p.y(), p.z()}}, ceilingMaterial},
    {"wallWest", {{o.x(), o.y(), a.z()}, {a.x(), p.y(), b.z()}}, wallMaterial},
    {"wallEast", {{b.x(), o.y(), a.z()}, {p.x(), p.y(), b.z()}}, wallMaterial},
    {"wallSouth", {{a.x(), o.y(), a.z()}, {b.x(), a.y(), b.z()}}, wallMaterial},
    {"wallNorth", {{a.x(), b.y(), a.z()}, {b.x(), p.y(), b.z()}}, wallMaterial},
  };

  const auto builder = brushBuilder(map);
  auto brushes = std::vector<mdl::Brush>{};
  for (const auto& part : parts)
  {
    auto brush = builder.createCuboid(part.bounds, part.material);
    if (brush.is_error())
    {
      return geometryError(
        "The " + part.role + " " + toJson(part.bounds).dump()
          + " does not make a valid brush.",
        errorMessage(brush),
        {},
        "Use a larger inner box or a different thickness.");
    }
    brushes.push_back(std::move(brush).value());
  }

  auto* group = static_cast<mdl::GroupNode*>(nullptr);
  auto added = addAndSelectBrushes(
    context, std::move(brushes), args.getOptional<std::string>("group"), &group);
  if (added.is_error())
  {
    return errorOf(added);
  }

  auto roles = Json::object();
  for (size_t i = 0; i < parts.size(); ++i)
  {
    roles[parts[i].role] = context.ids().format(*added.value()[i]);
  }

  return Json{
    {"brushes", std::move(roles)},
    {"inner", toJson(inner)},
    {"outer", toJson(outer)},
    {"thickness", thickness},
    {"group", groupJson(context, group)},
    {"objects", nodeSummaries(added.value(), context.ids())},
  };
}

// opening_cut

/** The most frequent material of the brush's faces. */
std::string dominantMaterial(const mdl::Brush& brush)
{
  auto counts = std::map<std::string, size_t>{};
  for (const auto& face : brush.faces())
  {
    ++counts[face.materialName()];
  }
  return std::ranges::max_element(
           counts,
           [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; })
    ->first;
}

void collectIntersectingBrushes(
  const mdl::Map& map,
  mdl::Node& node,
  const vm::bbox3d& box,
  std::vector<mdl::Node*>& editable,
  std::vector<mdl::Node*>& notEditable)
{
  if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
  {
    if (intersectsInterior(brushNode->brush(), box))
    {
      (map.editorContext().selectable(*brushNode) ? editable : notEditable)
        .push_back(brushNode);
    }
    return;
  }
  for (auto* child : node.children())
  {
    collectIntersectingBrushes(map, *child, box, editable, notEditable);
  }
}

ToolResult openingCut(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto opening = boxArgument(args);
  if (auto error = checkBox(map, opening, "The opening"))
  {
    error->details["min"] = toJson(opening.min);
    error->details["max"] = toJson(opening.max);
    return *error;
  }

  auto targets = std::vector<mdl::Node*>{};
  if (args.has("ids"))
  {
    auto resolved = resolveTargets(context, args, "ids", {ObjectKind::Brush});
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    for (auto* node : resolved.value())
    {
      const auto& brush = static_cast<mdl::BrushNode*>(node)->brush();
      if (intersectsInterior(brush, opening))
      {
        targets.push_back(node);
      }
      else
      {
        context.warn(
          "NOT_INTERSECTING",
          "Brush " + ids.format(*node) + " does not intersect the opening; left as is.",
          {ids.format(*node)});
      }
    }
    if (targets.empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The opening " + toJson(opening).dump()
          + " does not intersect any of the given brushes.",
        "Move the opening into the wall (check the wall bounds with object_get) or omit "
        "ids to cut every brush the opening intersects.",
        formatIds(resolved.value(), ids));
    }
  }
  else
  {
    auto notEditable = std::vector<mdl::Node*>{};
    collectIntersectingBrushes(map, map.worldNode(), opening, targets, notEditable);
    if (targets.empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The opening " + toJson(opening).dump()
          + " does not intersect any editable brush.",
        notEditable.empty()
          ? "Move the opening so that it overlaps a wall; space_check with the same box "
            "lists what it overlaps."
          : "The intersecting brushes are hidden, locked or in a closed group: show or "
            "unlock their layer (layer_set_state) or open the group (group_open).",
        formatIds(notEditable, ids));
    }
  }

  const auto builder = brushBuilder(map);
  const auto explicitMaterial =
    args.has("material") ? std::optional{materialArgument(context, args)} : std::nullopt;

  struct Cut
  {
    mdl::Node* wall;
    std::string wallId;
    std::vector<mdl::Brush> fragments;
  };
  auto cuts = std::vector<Cut>{};
  for (auto* node : targets)
  {
    const auto& wall = static_cast<mdl::BrushNode*>(node)->brush();
    const auto wallId = ids.format(*node);
    // The faces of the opening become the reveals of the cut, so they get the given
    // material or else the wall's dominant material.
    const auto material = explicitMaterial.value_or(dominantMaterial(wall));
    auto subtrahend = builder.createCuboid(opening, material);
    if (subtrahend.is_error())
    {
      return geometryError(
        "The opening " + toJson(opening).dump() + " does not make a valid brush.",
        errorMessage(subtrahend),
        {wallId},
        "Make every component of max greater than the corresponding component of min.");
    }

    auto fragments = std::vector<mdl::Brush>{};
    for (auto& fragment : wall.subtract(
           map.worldNode().mapFormat(), map.worldBounds(), material, subtrahend.value()))
    {
      if (fragment.is_error())
      {
        return geometryError(
          "Cutting the opening " + toJson(opening).dump() + " out of " + wallId
            + " produced an invalid fragment.",
          errorMessage(fragment),
          {wallId},
          "Align the opening with the grid or make it slightly larger so that it does "
          "not leave slivers.");
      }
      // Brush::subtract copies the attributes of the opening to fragment faces that are
      // coplanar with it, which would retexture the wall's own faces if the opening is
      // flush with them; restore the wall's attributes on those faces.
      auto brush = std::move(fragment).value();
      brush.cloneFaceAttributesFrom(wall);
      fragments.push_back(std::move(brush));
    }
    cuts.push_back({node, wallId, std::move(fragments)});
  }

  mdl::deselectAll(map);

  auto result = Json::array();
  auto allFragments = std::vector<mdl::Node*>{};
  auto walls = std::vector<mdl::Node*>{};
  for (auto& cut : cuts)
  {
    const auto count = cut.fragments.size();
    auto added = count > 0 ? addBrushes(map, std::move(cut.fragments), cut.wall->parent())
                           : std::vector<mdl::Node*>{};
    if (added.size() != count)
    {
      return geometryOperationFailed(
        context,
        "The fragments of " + cut.wallId + " could not be added.",
        {cut.wallId},
        "Check that the wall's layer is not locked.");
    }
    result.push_back(Json{
      {"removed", cut.wallId},
      {"fragments", formatIds(added, ids)},
    });
    allFragments.insert(allFragments.end(), added.begin(), added.end());
    walls.push_back(cut.wall);
  }

  mdl::removeNodes(map, walls);
  if (!allFragments.empty())
  {
    mdl::selectNodes(map, allFragments);
  }

  // Sanity check: nothing that was cut may remain inside the opening.
  const auto probe = shrink(opening, 0.01);
  auto remaining = std::vector<std::string>{};
  for (const auto* node : allFragments)
  {
    if (intersectsInterior(static_cast<const mdl::BrushNode*>(node)->brush(), probe))
    {
      remaining.push_back(ids.format(*node));
    }
  }
  if (!remaining.empty())
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "The fragments still overlap the opening " + toJson(opening).dump() + ".",
      "Align the opening with the grid and retry.",
      std::move(remaining));
  }

  warnNonIntegerVertices(context, allFragments);
  return Json{
    {"opening", toJson(opening)},
    {"cuts", std::move(result)},
    {"objects", nodeSummaries(allFragments, ids)},
  };
}

} // namespace

void registerGeometryTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"brush_create_box"}
      .title("Create Box Brush")
      .description(
        "Creates a cuboid brush from its min and max corners in the open group or the "
        "current layer, and selects it like the editor does. Without material, the "
        "current material is used; an unknown material is used anyway and warned about "
        "(UNKNOWN_MATERIAL). A degenerate box fails with INVALID_GEOMETRY, a box outside "
        "the world bounds with OUT_OF_WORLD_BOUNDS. "
        "Example: {\"min\": [0, 0, 0], \"max\": [128, 128, 16], \"material\": "
        "\"floor_stone\"}")
      .input(object({
        field("min", vec3()).required().describe("Minimum corner [x, y, z]"),
        field("max", vec3()).required().describe("Maximum corner [x, y, z]"),
        field("material", string().nonEmpty())
          .describe("Material of all faces. Default: the current material"),
      }))
      .output(createdOutput({
        field("brush", objectId()).required().describe("Id of the new brush"),
        field("bounds", box()).required(),
        field("object", any()).required().describe("Summary of the new brush"),
      }))
      .mutation(Mutation::Map)
      .handler(brushCreateBox));

  registry.add(
    ToolDef{"brush_create_shape"}
      .title("Create Shape")
      .description(
        "Creates a shape inside the bounds [min, max] with the editor's shape tool and "
        "selects the new brushes. Shapes and their parameters: 'cuboid'; 'stairs' "
        "(stepHeight, stairDirection: the direction in which the stairs ascend; steps = "
        "ceil(height / stepHeight)); 'arch' (axis = tunnel direction, default 'x'; "
        "thickness; spandrel fills the corners above the arch; circle parameters); "
        "'cylinder' (axis, hollow + thickness makes a ring of one brush per side; circle "
        "parameters); 'cone' (axis, circle parameters); 'uvSphere' (axis, rings, circle "
        "parameters); 'icoSphere' (subdivision). Circle parameters: circleMode "
        "'edgeAligned' (edges touch the bounds), 'vertexAligned' (vertices touch the "
        "bounds) or 'scalable' (a shape that keeps integer vertices when scaled), with "
        "sides (3-96) or, for 'scalable', precision (0-3 = 12, 24, 48 or 96 sides). "
        "Parameters that do not apply are ignored with an IGNORED_ARGUMENT warning. "
        "Example: {\"shape\": \"cylinder\", \"min\": [0, 0, 0], \"max\": [128, 128, "
        "256], \"sides\": 12, \"hollow\": true, \"thickness\": 16}")
      .input(object({
        field(
          "shape",
          enumOf(
            {"cuboid", "stairs", "arch", "cylinder", "cone", "uvSphere", "icoSphere"}))
          .required()
          .describe("The shape to create"),
        field("min", vec3()).required().describe("Minimum corner of the bounds"),
        field("max", vec3()).required().describe("Maximum corner of the bounds"),
        field("material", string().nonEmpty())
          .describe("Material of all faces. Default: the current material"),
        field("axis", enumOf({"x", "y", "z"}))
          .describe(
            "Axis of circular shapes (arch: tunnel direction). Default: 'z', for arch "
            "'x'"),
        field("circleMode", enumOf({"edgeAligned", "vertexAligned", "scalable"}))
          .describe("How the circle fits the bounds. Default: 'edgeAligned'"),
        field("sides", integer().min(3).max(96))
          .describe("Number of sides (edgeAligned, vertexAligned). Default: 8"),
        field("precision", integer().min(0).max(3))
          .describe("scalable only: 0-3 for 12, 24, 48 or 96 sides. Default: 0"),
        field("hollow", boolean())
          .describe("cylinder: make a hollow ring. Default: false"),
        field("thickness", number())
          .describe("Wall thickness of hollow cylinders and arches. Default: 16"),
        field("spandrel", boolean())
          .describe("arch: also fill the space above the arch. Default: false"),
        field("rings", integer().min(1).max(256))
          .describe("uvSphere: number of rings. Default: 8"),
        field("subdivision", integer().min(1).max(4))
          .describe("icoSphere: number of subdivisions. Default: 1"),
        field("stepHeight", number())
          .describe("stairs: rise of one step (> 0). Default: 16"),
        field("stairDirection", enumOf({"+x", "-x", "+y", "-y"}))
          .describe("stairs: direction in which the stairs ascend. Default: '+x'"),
        field("group", string().nonEmpty())
          .describe("Put the new brushes into a new group with this name"),
      }))
      .output(createdOutput({
        field("brushes", array(objectId())).required().describe("Ids of the new brushes"),
        field("count", integer()).required(),
        groupField(),
        objectsField(),
      }))
      .mutation(Mutation::Map)
      .handler(brushCreateShape));

  registry.add(
    ToolDef{"brush_create_hull"}
      .title("Create Hull Brush")
      .description(
        "Creates a brush as the convex hull of the given points (at least 4, spanning a "
        "volume) and selects it. Points inside the hull are dropped (POINTS_INSIDE_HULL "
        "warning); non-integer vertices are reported (NON_INTEGER_VERTICES). Points that "
        "coincide or lie on one line or plane fail with INVALID_GEOMETRY, points "
        "outside the world bounds with OUT_OF_WORLD_BOUNDS. "
        "Example: {\"points\": [[0, 0, 0], [128, 0, 0], [0, 128, 0], [0, 0, 64]]}")
      .input(object({
        field("points", array(vec3()).nonEmpty())
          .required()
          .describe("Points [x, y, z] whose convex hull becomes the brush"),
        field("material", string().nonEmpty())
          .describe("Material of all faces. Default: the current material"),
      }))
      .output(createdOutput({
        field("brush", objectId()).required().describe("Id of the new brush"),
        field("bounds", box()).required(),
        field("vertexCount", integer()).required(),
        field("object", any()).required().describe("Summary of the new brush"),
      }))
      .mutation(Mutation::Map)
      .handler(brushCreateHull));

  registry.add(
    ToolDef{"room_create"}
      .title("Create Room")
      .description(
        "Creates a sealed hollow room around the inner box [min, max]: a floor, a "
        "ceiling and four walls of the given thickness that enclose the inner space "
        "without gaps or overlaps (floor and ceiling cover the whole outer footprint, "
        "the west/east walls the full outer depth). Materials: 'material' for all "
        "surfaces, overridden by 'floor', 'ceiling' and 'walls'. The brushes are "
        "selected, or grouped with 'group'. The result names each brush by role "
        "(floor, ceiling, wallWest (-x), wallEast (+x), wallSouth (-y), wallNorth (+y)). "
        "Example: {\"min\": [0, 0, 0], \"max\": [512, 384, 192], \"thickness\": 16, "
        "\"floor\": \"floor_stone\", \"walls\": \"wall_brick\"}")
      .input(object({
        field("min", vec3()).required().describe("Minimum corner of the inner space"),
        field("max", vec3()).required().describe("Maximum corner of the inner space"),
        field("thickness", number())
          .describe(
            "Thickness of floor, ceiling and walls (> 0). Default: the grid size"),
        field("material", string().nonEmpty())
          .describe("Material of all surfaces. Default: the current material"),
        field("floor", string().nonEmpty()).describe("Material of the floor"),
        field("ceiling", string().nonEmpty()).describe("Material of the ceiling"),
        field("walls", string().nonEmpty()).describe("Material of the walls"),
        field("group", string().nonEmpty())
          .describe("Put the room's brushes into a new group with this name"),
      }))
      .output(createdOutput({
        field("brushes", object({}).allowAdditionalProperties())
          .required()
          .describe("Brush id per role: floor, ceiling, wallWest, wallEast, wallSouth, "
                    "wallNorth"),
        field("inner", box()).required(),
        field("outer", box()).required(),
        field("thickness", number()).required(),
        groupField(),
        objectsField(),
      }))
      .mutation(Mutation::Map)
      .handler(roomCreate));

  registry.add(
    ToolDef{"opening_cut"}
      .title("Cut Opening")
      .description(
        "Cuts a doorway or window [min, max] through wall brushes (CSG subtraction): "
        "each wall is replaced by fragments that keep its materials and parent; the "
        "reveal faces get 'material' or else the wall's dominant material. Without ids, "
        "every editable brush that the opening intersects is cut. The fragments are "
        "selected. An opening that intersects none of the targets fails with "
        "INVALID_ARGUMENT. Example: {\"ids\": [\"brush:12\"], \"min\": [240, -16, 0], "
        "\"max\": [304, 0, 112]}")
      .input(object({
        field("ids", array(objectId({ObjectKind::Brush})).nonEmpty())
          .describe("Wall brushes to cut. Default: all editable brushes intersecting the "
                    "opening"),
        field("min", vec3()).required().describe("Minimum corner of the opening"),
        field("max", vec3()).required().describe("Maximum corner of the opening"),
        field("material", string().nonEmpty())
          .describe(
            "Material of the reveal faces. Default: the wall's dominant material"),
      }))
      .output(createdOutput({
        field("opening", box()).required(),
        field("cuts", array(any()))
          .required()
          .describe("Per wall: {removed: wall id, fragments: [fragment ids]}"),
        objectsField(),
      }))
      .mutation(Mutation::Map)
      .handler(openingCut));
}

} // namespace tb::mcp
