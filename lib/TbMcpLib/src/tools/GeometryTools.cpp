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

#include "EntityUtils.h"
#include "NodeJson.h"
#include "ToolUtils.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/UvTools.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/CircleShape.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Grid.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "ui/DrawShapeToolParameters.h"

#include "kd/result_fold.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
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
 * the new brushes are added inside a new group there (the group is selected then).
 * Returns the added brushes.
 */
Result<std::vector<mdl::Node*>, ToolError> addAndSelectBrushes(
  CallContext& context,
  std::vector<mdl::Brush> brushes,
  const std::optional<std::string>& groupName,
  mdl::GroupNode** group = nullptr)
{
  auto& map = context.map();
  mdl::deselectAll(map);

  if (!groupName)
  {
    const auto count = brushes.size();
    auto added = addBrushes(map, std::move(brushes));
    if (added.size() != count)
    {
      return context.operationFailed("The new brushes could not be added to the map.");
    }
    mdl::selectNodes(map, added);
    return added;
  }

  // The group is added with the brushes as its children, like pasted groups. Grouping
  // added brushes afterwards (mdl::groupSelectedNodes) would reparent them, and the
  // change report would list them as modified instead of created.
  auto added = std::vector<mdl::Node*>{};
  added.reserve(brushes.size());
  for (auto& brush : brushes)
  {
    added.push_back(new mdl::BrushNode{std::move(brush)});
  }
  auto* groupNode = new mdl::GroupNode{mdl::Group{*groupName}};
  groupNode->addChildren(added);
  if (mdl::addNodes(map, {{&mdl::parentForNodes(map), {groupNode}}}).empty())
  {
    return context.operationFailed("The new brushes could not be added to the map.");
  }

  mdl::selectNodes(map, {groupNode});
  if (group)
  {
    *group = groupNode;
  }
  return added;
}

Json groupJson(CallContext& context, const mdl::GroupNode* group)
{
  return group ? Json(context.ids().format(*group)) : Json(nullptr);
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

Result<std::vector<mdl::Brush>> single(Result<mdl::Brush> brush)
{
  return std::move(brush).transform([](auto b) { return std::vector{std::move(b)}; });
}

/**
 * Builds stairs like the editor's stairs shape: steps of `stepHeight` that rise in the
 * stair direction, each reaching down to the bottom of the box.
 */
Result<std::vector<mdl::Brush>> buildStairs(
  const mdl::BrushBuilder& builder,
  const vm::bbox3d& box,
  const ui::DrawShapeToolParameters& parameters,
  const std::string& material)
{
  using StairDirection = ui::DrawShapeToolParameters::StairDirection;
  const auto direction = parameters.stairDirection();
  const auto axis = direction == StairDirection::PosY || direction == StairDirection::NegY
                      ? vm::axis::y
                      : vm::axis::x;
  const auto positive =
    direction == StairDirection::PosX || direction == StairDirection::PosY;

  const auto stepHeight = std::max(1.0, std::abs(parameters.stepHeight()));
  const auto size = box.size();
  const auto numSteps = std::max(size_t{1}, size_t(std::ceil(size.z() / stepHeight)));
  const auto treadDepth = size[axis] / double(numSteps);

  auto steps = std::vector<Result<mdl::Brush>>{};
  for (size_t i = 0; i < numSteps; ++i)
  {
    auto stepBox = vm::bbox3d{
      box.min,
      {box.max.xy(), std::min(box.min.z() + stepHeight * double(i + 1), box.max.z())},
    };
    stepBox.min[axis] = positive ? box.min[axis] + treadDepth * double(i)
                                 : box.max[axis] - treadDepth * double(i + 1);
    stepBox.max[axis] = positive ? box.min[axis] + treadDepth * double(i + 1)
                                 : box.max[axis] - treadDepth * double(i);

    steps.push_back(builder.createCuboid(stepBox, material));
  }
  return std::move(steps) | kdl::fold;
}

/**
 * Builds the brushes of a shape with the same brush builder calls as the editor's shape
 * tool extensions, which need a ui::MapDocument.
 */
Result<std::vector<mdl::Brush>> buildShape(
  const mdl::Map& map,
  const std::string& shape,
  const vm::bbox3d& box,
  const ui::DrawShapeToolParameters& parameters,
  const std::string& material)
{
  const auto builder = brushBuilder(map);
  const auto& circleShape = parameters.circleShape();
  const auto axis = parameters.axis();

  if (shape == "stairs")
  {
    return buildStairs(builder, box, parameters, material);
  }
  if (shape == "arch")
  {
    const auto thickness = parameters.thickness();
    auto arch = builder.createArch(box, thickness, circleShape, axis, material);
    if (arch.is_error() || !parameters.createSpandrel())
    {
      return arch;
    }
    auto spandrel =
      builder.createSpandrelForArch(box, thickness, circleShape, axis, material);
    if (spandrel.is_error())
    {
      return spandrel;
    }
    auto brushes = std::move(arch).value();
    for (auto& brush : std::move(spandrel).value())
    {
      brushes.push_back(std::move(brush));
    }
    return brushes;
  }
  if (shape == "cylinder")
  {
    return parameters.hollow()
             ? builder.createHollowCylinder(
                 box, parameters.thickness(), circleShape, axis, material)
             : single(builder.createCylinder(box, circleShape, axis, material));
  }
  if (shape == "cone")
  {
    return single(builder.createCone(box, circleShape, axis, material));
  }
  if (shape == "uvSphere")
  {
    return single(
      builder.createUvSphere(box, circleShape, parameters.numRings(), axis, material));
  }
  if (shape == "icoSphere")
  {
    return single(builder.createIcoSphere(box, parameters.accuracy(), material));
  }
  return single(builder.createCuboid(box, material));
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
  auto brushes = buildShape(context.map(), shape, box, parameters.value(), material);
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

  auto* group = static_cast<mdl::GroupNode*>(nullptr);
  auto added = addAndSelectBrushes(
    context, std::move(brushes).value(), args.getOptional<std::string>("group"), &group);
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

/**
 * The hint for brushes that cannot be cut because they are inside closed groups: like the
 * editor, tools edit the members of a group only while it is open, so it names the groups
 * to open. Empty if none of the brushes is inside a closed group.
 */
std::string closedGroupHint(const IdRegistry& ids, const std::vector<mdl::Node*>& nodes)
{
  auto groups = std::vector<std::string>{};
  for (const auto* node : nodes)
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (brushNode && !brushNode->containingGroupOpened())
    {
      const auto groupId = ids.format(*brushNode->containingGroup());
      if (std::ranges::find(groups, groupId) == groups.end())
      {
        groups.push_back(groupId);
      }
    }
  }
  if (groups.empty())
  {
    return {};
  }
  auto names = std::string{};
  for (const auto& groupId : groups)
  {
    names += (names.empty() ? "" : ", ") + groupId;
  }
  return "The walls are inside the closed group" + std::string{groups.size() > 1 ? "s " : " "}
         + names + (groups.size() > 1 ? ": open one at a time" : ": open it")
         + " with group_open {\"group\": \"" + groups.front()
         + "\"}, cut the opening, then group_close. Openings through walls of two groups "
           "need one cut per group.";
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
      auto error = errorOf(resolved);
      if (error.code == ErrorCode::ObjectNotEditable)
      {
        auto nodes = std::vector<mdl::Node*>{};
        for (const auto& id : error.objectIds)
        {
          if (auto node = ids.resolve(id); node.is_success())
          {
            nodes.push_back(node.value());
          }
        }
        if (auto hint = closedGroupHint(ids, nodes); !hint.empty())
        {
          error.hint = std::move(hint);
        }
      }
      return error;
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
      auto hint = closedGroupHint(ids, notEditable);
      if (notEditable.empty())
      {
        hint =
          "Move the opening so that it overlaps a wall; space_check with the same box "
          "lists what it overlaps.";
      }
      else if (hint.empty())
      {
        hint =
          "The intersecting brushes are hidden or locked: show or unlock their layer "
          "(layer_set_state).";
      }
      return makeError(
        ErrorCode::InvalidArgument,
        "The opening " + toJson(opening).dump()
          + " does not intersect any editable brush.",
        std::move(hint),
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

// brushes_create

/** The keys of per-face materials (`faces`) by face direction. */
constexpr auto FaceDirections = std::array<std::string_view, 7>{
  "top", "bottom", "north", "south", "east", "west", "sides"};

/**
 * The direction key of a face normal: the axis with the largest component, top / bottom
 * (z), east / west (x), north / south (y).
 */
std::string_view faceDirection(const vm::vec3d& normal)
{
  const auto a = vm::abs(normal);
  if (a.z() >= a.x() && a.z() >= a.y())
  {
    return normal.z() > 0.0 ? "top" : "bottom";
  }
  if (a.x() >= a.y())
  {
    return normal.x() > 0.0 ? "east" : "west";
  }
  return normal.y() > 0.0 ? "north" : "south";
}

/**
 * The material of a face from per-face materials: its direction, then "sides" for faces
 * that are not top or bottom; nullopt if neither is given.
 */
std::optional<std::string> faceMaterial(const Json& faces, const vm::vec3d& normal)
{
  const auto direction = std::string{faceDirection(normal)};
  if (const auto it = faces.find(direction); it != faces.end())
  {
    return it->get<std::string>();
  }
  if (direction != "top" && direction != "bottom")
  {
    if (const auto it = faces.find("sides"); it != faces.end())
    {
      return it->get<std::string>();
    }
  }
  return std::nullopt;
}

/** The brushes of one item of brushes_create. */
Result<std::vector<mdl::Brush>, ToolError> buildItemBrushes(
  CallContext& context,
  const Args& item,
  const std::string& material,
  std::vector<std::string>& ignored,
  size_t& pointsInside)
{
  auto& map = context.map();
  if (item.has("points"))
  {
    if (item.has("shape") || item.has("min") || item.has("max"))
    {
      return invalidArgument(
        "A hull item takes only 'points', not 'shape', 'min' or 'max'.",
        "Pass either points or min and max.");
    }
    auto points = std::vector<vm::vec3d>{};
    for (const auto& point : item.get<Json>("points"))
    {
      points.push_back(*vec3FromJson(point));
    }
    if (points.size() < 4)
    {
      return makeError(
        ErrorCode::InvalidGeometry,
        "A convex hull needs at least 4 points, got " + std::to_string(points.size())
          + ".");
    }
    if (std::ranges::any_of(
          points, [&](const auto& p) { return !map.worldBounds().contains(p); }))
    {
      return makeError(
        ErrorCode::OutOfWorldBounds, "Some points lie outside the world bounds.");
    }
    if (const auto reason = degeneratePointsReason(points))
    {
      return makeError(
        ErrorCode::InvalidGeometry,
        "The points do not enclose a volume: " + *reason + ".");
    }
    auto brush = brushBuilder(map).createBrush(points, material);
    if (brush.is_error())
    {
      return makeError(
        ErrorCode::InvalidGeometry,
        "The convex hull does not make a valid brush: " + errorMessage(brush));
    }
    pointsInside += points.size() - std::min(points.size(), brush.value().vertexCount());
    return std::vector{std::move(brush).value()};
  }

  if (!item.has("min") || !item.has("max"))
  {
    return invalidArgument(
      "An item needs 'min' and 'max' (a box, or a shape with 'shape') or 'points' (a "
      "hull).",
      "E.g. {\"min\": [0, 0, 0], \"max\": [64, 64, 16]}.");
  }
  const auto box = boxArgument(item);
  if (auto error = checkBox(map, box))
  {
    return *error;
  }

  const auto shape = item.getOr<std::string>("shape", "cuboid");
  const auto circleMode = item.getOr<std::string>("circleMode", "edgeAligned");
  const auto relevant =
    relevantShapeParameters(shape, circleMode, item.getOr<bool>("hollow", false));
  for (const auto key : ShapeParameterKeys)
  {
    if (item.has(key) && std::ranges::find(relevant, key) == relevant.end())
    {
      ignored.push_back(std::string{key});
    }
  }
  if (shape == "cuboid")
  {
    auto brush = brushBuilder(map).createCuboid(box, material);
    if (brush.is_error())
    {
      return makeError(
        ErrorCode::InvalidGeometry,
        "The box does not make a valid brush: " + errorMessage(brush));
    }
    return std::vector{std::move(brush).value()};
  }

  auto parameters = shapeParameters(item, shape, box);
  if (parameters.is_error())
  {
    return errorOf(parameters);
  }
  auto brushes = buildShape(map, shape, box, parameters.value(), material);
  if (brushes.is_error())
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "The " + shape + " could not be built: " + errorMessage(brushes),
      "Make the bounds larger or use fewer sides, rings or subdivisions.");
  }
  if (brushes.value().empty())
  {
    return makeError(ErrorCode::InvalidGeometry, "The " + shape + " yields no brushes.");
  }
  return std::move(brushes).value();
}

/** A brush entity of brushes_create. */
struct BulkBrushEntity
{
  mdl::Entity entity;
  /** The group of the first item that uses the entity. */
  std::optional<std::string> group;
  bool used = false;
  mdl::EntityNode* node = nullptr;
};

Result<std::map<std::string, BulkBrushEntity>, ToolError> brushEntitiesArgument(
  CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto result = std::map<std::string, BulkBrushEntity>{};
  const auto entities = args.getOptional<Json>("brushEntities");
  if (!entities)
  {
    return result;
  }
  const auto setDefaults = map.worldNode().entityPropertyConfig().setDefaultProperties;
  for (const auto& [key, value] : entities->items())
  {
    const auto classname = value.value("classname", std::string{});
    if (
      classname.empty() || containsQuote(classname)
      || classname.find_first_of(" \t\n") != std::string::npos)
    {
      return invalidArgument(
        "brushEntities." + key + ": the classname must be non-empty and must not "
        "contain quotes or whitespace.",
        "E.g. {\"door\": {\"classname\": \"func_door\", \"properties\": {\"speed\": "
        "100}}}.");
    }
    const auto* definition = findEntityDefinition(map, classname);
    if (definition && mdl::getType(*definition) != mdl::EntityDefinitionType::Brush)
    {
      return invalidArgument(
        "brushEntities." + key + ": '" + classname + "' is a point entity class.",
        "Create point entities with entities_create.");
    }
    auto properties = propertiesFromJson(value.value("properties", Json::object()), true);
    if (properties.is_error())
    {
      auto error = errorOf(properties);
      error.message = "brushEntities." + key + ": " + error.message;
      return error;
    }

    auto entity = mdl::Entity{{{mdl::EntityPropertyKeys::Classname, classname}}};
    if (definition && setDefaults)
    {
      mdl::setDefaultProperties(*definition, entity, mdl::SetDefaultPropertyMode::SetAll);
    }
    for (const auto& [propertyKey, propertyValue] : properties.value())
    {
      if (propertyValue)
      {
        validateProperty(context, definition, propertyKey, *propertyValue, {});
        entity.addOrUpdateProperty(propertyKey, *propertyValue);
      }
      else
      {
        entity.removeProperty(propertyKey);
      }
    }
    if (!definition)
    {
      warnUnknownClassname(context, classname, {});
    }
    result.emplace(key, BulkBrushEntity{std::move(entity)});
  }
  return result;
}

ToolResult brushesCreate(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto items = args.get<Json>("items");
  const auto defaultFaces = args.getOr<Json>("faces", Json::object());
  const auto defaultGroup = args.getOptional<std::string>("group");
  const auto defaultMaterial =
    args.getOr<std::string>("material", map.currentMaterialName());
  const auto uvMode =
    uvModeFromString(args.get<std::string>("uv")).value_or(UvMode::Keep);

  auto brushEntities = brushEntitiesArgument(context, args);
  if (brushEntities.is_error())
  {
    return errorOf(brushEntities);
  }
  auto entities = std::move(brushEntities).value();

  auto* parent = &mdl::parentForNodes(map);
  if (const auto layerId = args.getOptional<std::string>("layer"))
  {
    auto layer = ids.resolve(*layerId);
    if (layer.is_error())
    {
      return errorOf(layer);
    }
    parent = layer.value();
  }

  // build and validate every item before anything is added
  auto errors = ItemErrors{};
  auto itemBrushes = std::vector<std::vector<mdl::Brush>>{};
  auto itemGroups = std::vector<std::optional<std::string>>{};
  auto itemEntities = std::vector<BulkBrushEntity*>{};
  auto ignored = std::map<std::string, std::vector<size_t>>{};
  auto materials = std::set<std::string>{};
  auto pointsInside = size_t(0);
  for (size_t i = 0; i < items.size(); ++i)
  {
    const auto item = Args{items[i]};
    const auto material = item.getOr<std::string>("material", defaultMaterial);
    auto faces = defaultFaces;
    faces.update(item.getOr<Json>("faces", Json::object()));

    auto ignoredKeys = std::vector<std::string>{};
    auto brushes = buildItemBrushes(context, item, material, ignoredKeys, pointsInside);
    for (auto& key : ignoredKeys)
    {
      ignored[std::move(key)].push_back(i);
    }
    if (brushes.is_error())
    {
      errors.add(i, errorOf(brushes));
      brushes = std::vector<mdl::Brush>{};
    }
    auto itemBrushList = std::move(brushes).value();
    for (auto& brush : itemBrushList)
    {
      for (size_t f = 0; f < brush.faceCount(); ++f)
      {
        auto& face = brush.face(f);
        if (const auto name = faceMaterial(faces, face.boundary().normal))
        {
          face.setMaterialName(*name);
        }
        materials.insert(face.materialName());
      }
    }
    itemBrushes.push_back(std::move(itemBrushList));

    // an empty group name keeps the item out of the call's group
    auto group = item.getOptional<std::string>("group");
    if (!group)
    {
      group = defaultGroup;
    }
    else if (group->empty())
    {
      group = std::nullopt;
    }
    auto* entity = static_cast<BulkBrushEntity*>(nullptr);
    if (const auto key = item.getOptional<std::string>("entity"))
    {
      if (const auto it = entities.find(*key); it == entities.end())
      {
        errors.add(i, "The entity '" + *key + "' is not defined in brushEntities.");
      }
      else
      {
        entity = &it->second;
        if (!entity->used)
        {
          entity->used = true;
          entity->group = group;
        }
        else if (entity->group != group)
        {
          errors.add(
            i,
            "The brushes of entity '" + *key
              + "' must all be in the same group, but this item names another group.");
        }
      }
    }
    itemGroups.push_back(std::move(group));
    itemEntities.push_back(entity);
  }
  for (const auto& [key, entity] : entities)
  {
    if (!entity.used)
    {
      context.warn(
        "UNUSED_BRUSH_ENTITY",
        "brushEntities." + key + " is not used by any item and was not created.");
    }
  }
  if (!errors.empty())
  {
    return errors.error(
      items.size(),
      "Fix the listed items (details.errors) and call brushes_create again with all "
      "items.");
  }

  // the node tree: layer -> groups -> brush entities -> brushes
  auto groups = std::map<std::string, mdl::GroupNode*>{};
  auto topLevel = std::vector<mdl::Node*>{};
  auto brushNodes = std::vector<mdl::Node*>{};
  auto brushesPerItem = std::vector<size_t>{};
  for (size_t i = 0; i < items.size(); ++i)
  {
    auto* container = static_cast<mdl::Node*>(nullptr);
    if (const auto& groupName = itemGroups[i])
    {
      auto [it, inserted] = groups.try_emplace(*groupName, nullptr);
      if (inserted)
      {
        it->second = new mdl::GroupNode{mdl::Group{*groupName}};
        topLevel.push_back(it->second);
      }
      container = it->second;
    }
    if (auto* entity = itemEntities[i])
    {
      if (!entity->node)
      {
        entity->node = new mdl::EntityNode{entity->entity};
        if (container)
        {
          container->addChild(entity->node);
        }
        else
        {
          topLevel.push_back(entity->node);
        }
      }
      container = entity->node;
    }

    brushesPerItem.push_back(itemBrushes[i].size());
    for (auto& brush : itemBrushes[i])
    {
      auto* brushNode = new mdl::BrushNode{std::move(brush)};
      brushNodes.push_back(brushNode);
      if (container)
      {
        container->addChild(brushNode);
      }
      else
      {
        topLevel.push_back(brushNode);
      }
    }
  }

  mdl::deselectAll(map);
  if (mdl::addNodes(map, {{parent, topLevel}}).empty())
  {
    return context.operationFailed("The new brushes could not be added to the map.");
  }
  if (
    auto error = checkInsideWorldBounds(
      brushNodes, map, ids, "Move the items inside the world bounds."))
  {
    return *error;
  }
  auto selectable = std::vector<mdl::Node*>{};
  std::ranges::copy_if(topLevel, std::back_inserter(selectable), [&](const auto* node) {
    return map.editorContext().selectable(*node);
  });
  mdl::selectNodes(map, selectable);

  auto unknown = std::vector<std::string>{};
  std::ranges::copy_if(materials, std::back_inserter(unknown), [&](const auto& name) {
    return !map.materialManager().material(name);
  });
  if (!unknown.empty())
  {
    context.warn(
      "UNKNOWN_MATERIAL",
      "Materials not loaded: " + kdl::str_join(unknown, ", ")
        + "; the brushes use them anyway and show them as missing. Use materials_list "
          "to find available materials.");
  }
  for (const auto& [key, indices] : ignored)
  {
    context.warn(
      "IGNORED_ARGUMENT",
      fmt::format(
        "'{}' has no effect on the shapes of items {}.",
        key,
        kdl::str_join(
          indices | std::views::transform([](auto i) { return std::to_string(i); }),
          ", ")));
  }
  if (pointsInside > 0)
  {
    context.warn(
      "POINTS_INSIDE_HULL",
      std::to_string(pointsInside)
        + " hull points are not vertices of their hulls (they lie inside them or on "
          "their faces or edges).");
  }
  warnNonIntegerVertices(context, brushNodes);

  auto result = Json{
    {"ids", formatIds(brushNodes, ids)},
    {"count", brushNodes.size()},
  };
  if (std::ranges::any_of(brushesPerItem, [](const auto n) { return n != 1; }))
  {
    result["brushesPerItem"] = brushesPerItem;
  }
  auto groupIds = Json::object();
  for (const auto& [name, groupNode] : groups)
  {
    groupIds[name] = ids.format(*groupNode);
  }
  result["groups"] = std::move(groupIds);
  auto entityIds = Json::object();
  for (const auto& [key, entity] : entities)
  {
    if (entity.node)
    {
      entityIds[key] = ids.format(*entity.node);
    }
  }
  result["entities"] = std::move(entityIds);
  result["bounds"] = toJson(mdl::computeLogicalBounds(brushNodes));
  if (uvMode != UvMode::Keep)
  {
    auto aligned = alignNodeUvs(context, brushNodes, uvMode);
    if (aligned.is_error())
    {
      return errorOf(aligned);
    }
    result["uv"] = std::move(aligned).value();
  }
  return result;
}

} // namespace

void registerGeometryTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"brush_create_box"}
      .title("Create Box Brush")
      .description(
        "Creates a cuboid brush from its min and max corners (map units, Z up) in the "
        "open group or the current layer, and selects it like the editor does (one undo "
        "step). Without material, the "
        "current material is used; an unknown material is used anyway and warned about "
        "(UNKNOWN_MATERIAL). A degenerate box fails with INVALID_GEOMETRY, a box outside "
        "the world bounds with OUT_OF_WORLD_BOUNDS. "
        "Example: {\"min\": [0, 0, 0], \"max\": [128, 128, 16], \"material\": "
        "\"floor_stone\"}")
      .input(object({
        field("min", vec3()).required().describe("Minimum corner [x, y, z] in map units"),
        field("max", vec3())
          .required()
          .describe("Maximum corner [x, y, z] in map units; each component > min"),
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
        "Creates a shape inside the bounds [min, max] (map units) with the editor's "
        "shape "
        "tool and selects the new brushes (one undo step). Shapes and their parameters: "
        "'cuboid'; 'stairs' "
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
        field("min", vec3())
          .required()
          .describe("Minimum corner of the bounds (map units)"),
        field("max", vec3())
          .required()
          .describe("Maximum corner of the bounds (map units)"),
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
          .describe(
            "Wall thickness of hollow cylinders and arches in map units. Default: 16"),
        field("spandrel", boolean())
          .describe("arch: also fill the space above the arch. Default: false"),
        field("rings", integer().min(1).max(256))
          .describe("uvSphere: number of rings. Default: 8"),
        field("subdivision", integer().min(1).max(4))
          .describe("icoSphere: number of subdivisions. Default: 1"),
        field("stepHeight", number())
          .describe("stairs: rise of one step in map units (> 0). Default: 16"),
        field("stairDirection", enumOf({"+x", "-x", "+y", "-y"}))
          .describe("stairs: direction in which the stairs ascend. Default: '+x'"),
        field("group", string().nonEmpty())
          .describe("Put the new brushes into a new group with this name"),
      }))
      .output(createdOutput({
        field("brushes", array(objectId())).required().describe("Ids of the new brushes"),
        field("count", integer()).required().describe("Number of new brushes"),
        groupField(),
        objectsField(),
      }))
      .mutation(Mutation::Map)
      .handler(brushCreateShape));

  registry.add(
    ToolDef{"brush_create_hull"}
      .title("Create Hull Brush")
      .description(
        "Creates a brush as the convex hull of the given points (map units; at least 4, "
        "spanning a volume) and selects it (one undo step). Points inside the hull are "
        "dropped (POINTS_INSIDE_HULL "
        "warning); non-integer vertices are reported (NON_INTEGER_VERTICES). Points that "
        "coincide or lie on one line or plane fail with INVALID_GEOMETRY, points "
        "outside the world bounds with OUT_OF_WORLD_BOUNDS. "
        "Example: {\"points\": [[0, 0, 0], [128, 0, 0], [0, 128, 0], [0, 0, 64]]}")
      .input(object({
        field("points", array(vec3()).nonEmpty())
          .required()
          .describe("Points [x, y, z] in map units whose convex hull becomes the brush"),
        field("material", string().nonEmpty())
          .describe("Material of all faces. Default: the current material"),
      }))
      .output(createdOutput({
        field("brush", objectId()).required().describe("Id of the new brush"),
        field("bounds", box()).required(),
        field("vertexCount", integer())
          .required()
          .describe("Number of vertices of the hull (points inside it are dropped)"),
        field("object", any()).required().describe("Summary of the new brush"),
      }))
      .mutation(Mutation::Map)
      .handler(brushCreateHull));

  registry.add(
    ToolDef{"room_create"}
      .title("Create Room")
      .description(
        "Creates a sealed hollow room around the inner box [min, max] (map units) in one "
        "undo step: a floor, a "
        "ceiling and four walls of the given thickness that enclose the inner space "
        "without gaps or overlaps (floor and ceiling cover the whole outer footprint, "
        "the west/east walls the full outer depth). Materials: 'material' for all "
        "surfaces, overridden by 'floor', 'ceiling' and 'walls'. The brushes are "
        "selected, or grouped with 'group'. The result names each brush by role "
        "(floor, ceiling, wallWest (-x), wallEast (+x), wallSouth (-y), wallNorth (+y)). "
        "Example: {\"min\": [0, 0, 0], \"max\": [512, 384, 192], \"thickness\": 16, "
        "\"floor\": \"floor_stone\", \"walls\": \"wall_brick\"}")
      .input(object({
        field("min", vec3())
          .required()
          .describe("Minimum corner of the inner space (map units)"),
        field("max", vec3())
          .required()
          .describe("Maximum corner of the inner space (map units)"),
        field("thickness", number())
          .describe("Thickness of floor, ceiling and walls in map units (> 0). Default: "
                    "the grid size"),
        field("material", string().nonEmpty())
          .describe("Material of all surfaces. Default: the current material"),
        field("floor", string().nonEmpty())
          .describe("Material of the floor. Default: 'material'"),
        field("ceiling", string().nonEmpty())
          .describe("Material of the ceiling. Default: 'material'"),
        field("walls", string().nonEmpty())
          .describe("Material of the four walls. Default: 'material'"),
        field("group", string().nonEmpty())
          .describe("Put the room's brushes into a new group with this name"),
      }))
      .output(createdOutput({
        field("brushes", object({}).allowAdditionalProperties())
          .required()
          .describe("Brush id per role: floor, ceiling, wallWest, wallEast, wallSouth, "
                    "wallNorth"),
        field("inner", box()).required().describe("The enclosed inner space"),
        field("outer", box()).required().describe("Bounds of all room brushes"),
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
        "Cuts a doorway or window box [min, max] (map units) through wall brushes (CSG "
        "subtraction, one undo step): "
        "each wall is replaced by fragments that keep its materials and parent; the "
        "reveal faces get 'material' or else the wall's dominant material. Without ids, "
        "every editable brush that the opening intersects is cut. The fragments are "
        "selected. An opening that intersects none of the targets fails with "
        "INVALID_ARGUMENT. Like the editor, walls inside a closed group (e.g. a "
        "room_create group) are cut only while the group is open; the error's hint "
        "names the group to open with group_open (group_close afterwards). Example: "
        "{\"ids\": [\"brush:12\"], \"min\": [240, -16, 0], "
        "\"max\": [304, 0, 112]}")
      .input(object({
        idsField(
          {ObjectKind::Brush},
          "Wall brushes to cut. Default: all editable brushes intersecting the opening"),
        field("min", vec3())
          .required()
          .describe("Minimum corner of the opening (map units)"),
        field("max", vec3())
          .required()
          .describe("Maximum corner of the opening (map units). Let the opening span "
                    "the wall's thickness to cut all the way through"),
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

  auto facesSchema = object({
    field("top", string().nonEmpty()).describe("+z"),
    field("bottom", string().nonEmpty()).describe("-z"),
    field("north", string().nonEmpty()).describe("+y"),
    field("south", string().nonEmpty()).describe("-y"),
    field("east", string().nonEmpty()).describe("+x"),
    field("west", string().nonEmpty()).describe("-x"),
    field("sides", string().nonEmpty())
      .describe("All faces that are not top or bottom and have no material of their own"),
  });
  registry.add(
    ToolDef{"brushes_create"}
      .title("Create Brushes")
      .description(
        "Creates many brushes in one call and one undo step, e.g. the shell of a "
        "generated building, without writing a map file. Each item is a box {min, max}, "
        "a shape {shape, min, max, shape parameters as in brush_create_shape} or a "
        "convex hull {points}, with an optional material, per-face materials 'faces' "
        "(top, bottom, north +y, south -y, east +x, west -x, sides; by the face normal's "
        "main axis), 'group' (items with the same name share one new group; the call's "
        "'group' is the default) and 'entity' (a key of 'brushEntities', e.g. a "
        "func_door made of several items). The call's material and faces are the "
        "defaults. 'uv' aligns the new faces from the material profiles like map_import "
        "(typical, fit, world; default keep). All items are validated first; if any is "
        "invalid nothing is created and INVALID_ARGUMENT lists every problem with its "
        "item index in details.errors. The new objects go into 'layer' (default: the "
        "open group or current layer) and are selected. Result ids list the brushes in "
        "item order (brushesPerItem tells how many each item made when shapes make "
        "several); lists are cut to the detail level. Example: {\"material\": "
        "\"wall_brick\", \"faces\": {\"top\": \"floor_stone\"}, \"items\": [{\"min\": "
        "[0, 0, 0], \"max\": [512, 512, 16]}, {\"min\": [0, 0, 16], \"max\": [16, 512, "
        "192]}, {\"shape\": \"cylinder\", \"min\": [240, 240, 16], \"max\": [272, 272, "
        "192], \"sides\": 12, \"material\": \"pillar\"}, {\"min\": [256, 0, 16], "
        "\"max\": [320, 8, 128], \"entity\": \"door\"}], \"brushEntities\": {\"door\": "
        "{\"classname\": \"func_door\", \"properties\": {\"angle\": 90}}}, \"uv\": "
        "\"world\"}")
      .input(object({
        field(
          "items",
          array(
            object({
              field("min", vec3()).describe("Box or shape: minimum corner"),
              field("max", vec3()).describe("Box or shape: maximum corner"),
              field(
                "shape",
                enumOf(
                  {"cuboid",
                   "stairs",
                   "arch",
                   "cylinder",
                   "cone",
                   "uvSphere",
                   "icoSphere"}))
                .describe("A shape of brush_create_shape. Default: cuboid (a box)"),
              field("points", array(vec3()).nonEmpty())
                .describe("Hull: points whose convex hull becomes the brush"),
              field("axis", enumOf({"x", "y", "z"}))
                .describe("Shape parameter as in brush_create_shape"),
              field("circleMode", enumOf({"edgeAligned", "vertexAligned", "scalable"}))
                .describe("Shape parameter as in brush_create_shape"),
              field("sides", integer().min(3).max(96))
                .describe("Shape parameter as in brush_create_shape"),
              field("precision", integer().min(0).max(3))
                .describe("Shape parameter as in brush_create_shape"),
              field("hollow", boolean())
                .describe("Shape parameter as in brush_create_shape"),
              field("thickness", number())
                .describe("Shape parameter as in brush_create_shape"),
              field("spandrel", boolean())
                .describe("Shape parameter as in brush_create_shape"),
              field("rings", integer().min(1).max(256))
                .describe("Shape parameter as in brush_create_shape"),
              field("subdivision", integer().min(1).max(4))
                .describe("Shape parameter as in brush_create_shape"),
              field("stepHeight", number())
                .describe("Shape parameter as in brush_create_shape"),
              field("stairDirection", enumOf({"+x", "-x", "+y", "-y"}))
                .describe("Shape parameter as in brush_create_shape"),
              field("material", string().nonEmpty())
                .describe("Material of the item's faces. Default: the call's"),
              field("faces", facesSchema)
                .describe("Per-face materials, merged over the call's 'faces'"),
              field("group", string())
                .describe("Name of a new group for the item; \"\" keeps it out of the "
                          "call's group"),
              field("entity", string().nonEmpty())
                .describe("Key of the brush entity in 'brushEntities'"),
            }))
            .nonEmpty()
            .maxSize(5000))
          .required()
          .describe("The brushes to create"),
        field("material", string().nonEmpty())
          .describe("Default material of all faces. Default: the current material"),
        field("faces", facesSchema).describe("Default per-face materials"),
        field("group", string().nonEmpty())
          .describe("Default group name of the items: one new group"),
        field("brushEntities", object({}).allowAdditionalProperties())
          .describe(
            "Brush entities by key: {classname, properties}; items join them with "
            "'entity'"),
        field("layer", objectId({ObjectKind::Layer}))
          .describe("Layer of the new objects. Default: the open group or current layer"),
        field("uv", enumOf({"keep", "typical", "fit", "world"}).defaultsTo("keep"))
          .describe(
            "Texture alignment of the new faces: keep (the game's defaults), typical, "
            "fit or world (see map_import)"),
      }))
      .output(object({
        field("ids", array(objectId()))
          .required()
          .describe("The new brushes in item order"),
        field("count", integer()).required().describe("Number of new brushes"),
        field("brushesPerItem", array(integer()))
          .describe("Only if an item made more or less than one brush"),
        field("groups", object({}).allowAdditionalProperties())
          .required()
          .describe("Group id per group name"),
        field("entities", object({}).allowAdditionalProperties())
          .required()
          .describe("Entity id per brushEntities key"),
        field("bounds", box()).describe("Bounds of the new brushes"),
        field("uv", any()).describe("With uv other than keep: as in map_import"),
      }))
      .mutation(Mutation::Map)
      .handler(brushesCreate));
}

} // namespace tb::mcp
