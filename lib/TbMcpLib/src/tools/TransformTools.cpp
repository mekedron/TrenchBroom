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

#include "mcp/tools/TransformTools.h"

#include "NodeJson.h"
#include "base/NotifierConnection.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/CommandProcessor.h"
#include "mdl/EntityProperties.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/scalar.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

const auto TransformableKinds = std::vector<ObjectKind>{
  ObjectKind::Group,
  ObjectKind::Entity,
  ObjectKind::Brush,
  ObjectKind::Patch,
};

/** At most this many instances can be created by one objects_array call. */
constexpr size_t MaxArrayInstances = 1024;

/** At most this many object summaries are returned for created objects. */
constexpr size_t MaxSummaries = 100;

constexpr double Epsilon = 1e-9;

schema::Field transformIdsField()
{
  return idsField(
    TransformableKinds,
    "Object ids (groups, entities, brushes, patches) to act on. As in the editor, a "
    "group is transformed as a whole and a brush entity stands for all its brushes and "
    "patches (results list them, each with its 'entity'). Default: the current "
    "selection");
}

schema::Field alignmentLockField()
{
  return field("alignmentLock", boolean())
    .describe(
      "Override the texture lock (alignment lock) for this call; default: the "
      "editor's setting (locks_get)");
}

Schema axisSchema()
{
  return oneOf({
                 enumOf({"x", "y", "z"}).describe("A principal axis"),
                 vec3().describe("A direction vector [x, y, z], not zero"),
               })
    .describe("'x', 'y', 'z' or a direction vector [x, y, z]");
}

std::string toText(const vm::vec3d& v)
{
  return toJson(v).dump();
}

vm::bbox3d boundsOf(const std::vector<mdl::Node*>& nodes)
{
  auto bounds = nodes.front()->logicalBounds();
  for (const auto* node : nodes)
  {
    bounds = vm::merge(bounds, node->logicalBounds());
  }
  return bounds;
}

std::optional<vm::axis::type> axisIndex(const std::string& name)
{
  if (name == "x")
  {
    return vm::axis::x;
  }
  if (name == "y")
  {
    return vm::axis::y;
  }
  if (name == "z")
  {
    return vm::axis::z;
  }
  return std::nullopt;
}

/** Parses an axis argument ('x', 'y', 'z' or a vector) as a unit vector. */
Result<vm::vec3d, ToolError> axisArgument(
  const Args& args, const std::string_view key, const std::string& defaultAxis = "z")
{
  const auto value = args.getOptional<Json>(key).value_or(Json(defaultAxis));
  if (value.is_string())
  {
    const auto index = axisIndex(value.get<std::string>());
    return vm::vec3d::axis(*index);
  }

  const auto v = vec3FromJson(value);
  if (!v || vm::length(*v) < Epsilon)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The " + std::string{key} + " must not be the zero vector.",
      "Pass 'x', 'y', 'z' or a non-zero direction such as [0, 0, 1].");
  }
  return vm::normalize(*v);
}

/**
 * Sets the world's "update entity angle properties after transform" flag (the checkbox
 * on the editor's rotate tool page) for the lifetime of this object.
 */
class ScopedUpdateEntityAngles
{
private:
  mdl::Map& m_map;
  bool m_previous;

public:
  ScopedUpdateEntityAngles(mdl::Map& map, const bool updateEntityAngles)
    : m_map{map}
    , m_previous{map.worldNode().entityPropertyConfig().updateAnglePropertyAfterTransform}
  {
    m_map.worldNode().entityPropertyConfig().updateAnglePropertyAfterTransform =
      updateEntityAngles;
  }

  ~ScopedUpdateEntityAngles()
  {
    m_map.worldNode().entityPropertyConfig().updateAnglePropertyAfterTransform =
      m_previous;
  }

  ScopedUpdateEntityAngles(const ScopedUpdateEntityAngles&) = delete;
  ScopedUpdateEntityAngles& operator=(const ScopedUpdateEntityAngles&) = delete;
};

/** `{objects, bounds}` of the given nodes. */
Json objectsResult(CallContext& context, const std::vector<mdl::Node*>& nodes)
{
  return Json{
    {"objects", nodeSummaries(nodes, context.ids())},
    {"bounds", toJson(boundsOf(nodes))},
  };
}

/** Checks the world bounds and warns about non-integer vertices after a transform. */
std::optional<ToolError> checkTransformed(
  CallContext& context, const std::vector<mdl::Node*>& nodes)
{
  if (auto error = checkInsideWorldBounds(nodes, context.map(), context.ids()))
  {
    return error;
  }
  warnNonIntegerVertices(context, nodes);
  return std::nullopt;
}

/** The bounds of the given box after the transformation. */
vm::bbox3d transformBounds(const vm::bbox3d& bounds, const vm::mat4x4d& transformation)
{
  auto result = std::optional<vm::bbox3d>{};
  for (size_t i = 0; i < 8; ++i)
  {
    const auto corner = transformation
                        * vm::vec3d{
                          (i & 1) ? bounds.max.x() : bounds.min.x(),
                          (i & 2) ? bounds.max.y() : bounds.min.y(),
                          (i & 4) ? bounds.max.z() : bounds.min.z()};
    result = result ? vm::merge(*result, corner) : vm::bbox3d{corner, corner};
  }
  return *result;
}

/**
 * Explains why a transform failed: the editor does not log anything when a brush cannot
 * be transformed, which mostly happens when it would leave the world bounds. Returns
 * OUT_OF_WORLD_BOUNDS naming the objects whose transformed bounds reach the world bounds
 * (`reportedIds` replaces their ids, e.g. for copies that will not exist), or
 * INVALID_GEOMETRY otherwise.
 */
ToolError transformFailed(
  CallContext& context,
  const std::vector<mdl::Node*>& nodes,
  const vm::mat4x4d& transformation,
  const std::string& failure,
  const std::string& hint,
  const std::optional<std::vector<std::string>>& reportedIds = std::nullopt)
{
  const auto& worldBounds = context.map().worldBounds();
  auto offending = std::vector<std::string>{};
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    const auto bounds = transformBounds(nodes[i]->logicalBounds(), transformation);
    auto inside = true;
    for (size_t j = 0; j < 3; ++j)
    {
      inside = inside && bounds.min[j] > worldBounds.min[j]
               && bounds.max[j] < worldBounds.max[j];
    }
    if (!inside)
    {
      offending.push_back(
        reportedIds ? (*reportedIds)[i] : context.ids().format(*nodes[i]));
    }
  }

  if (!offending.empty())
  {
    auto list = std::string{};
    for (const auto& id : offending)
    {
      list += (list.empty() ? "" : ", ") + id;
    }
    return makeError(
      ErrorCode::OutOfWorldBounds,
      failure + " The result would reach or exceed the world bounds "
        + toJson(worldBounds).dump() + ": " + list + ".",
      hint,
      std::move(offending));
  }

  return geometryOperationFailed(
    context, failure, reportedIds ? *reportedIds : formatIds(nodes, context.ids()), hint);
}

/** A transform: the matrix (for diagnostics) and the editor function that applies it. */
struct Transform
{
  vm::mat4x4d matrix;
  std::function<bool(mdl::Map&)> apply;
};

using TransformFactory = std::function<Transform(const vm::bbox3d& bounds)>;

/**
 * Resolves the targets, selects them, applies the transform (with the alignment lock
 * override), checks the result and restores the selection.
 */
ToolResult transformTargets(
  CallContext& context,
  const Args& args,
  const TransformFactory& makeTransform,
  const std::string& failure,
  const std::string& hint)
{
  auto targets = resolveTargets(context, args, "ids", TransformableKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  const auto& nodes = targets.value();
  const auto bounds = boundsOf(nodes);
  const auto lock = ScopedLockOverride{map, args.getOptional<bool>("alignmentLock")};

  return withTargets(context, nodes, [&]() -> ToolResult {
    const auto transform = makeTransform(bounds);
    if (!transform.apply(map))
    {
      return transformFailed(context, nodes, transform.matrix, failure, hint);
    }
    if (auto error = checkTransformed(context, nodes))
    {
      return *error;
    }
    return objectsResult(context, nodes);
  });
}

// objects_move

ToolResult objectsMove(CallContext& context, const Args& args)
{
  const auto vector = args.get<vm::vec3d>("vector");
  if (vm::is_zero(vector, Epsilon))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The vector is zero, so nothing would move.",
      "Pass a non-zero vector, e.g. [64, 0, 0].");
  }

  auto result = transformTargets(
    context,
    args,
    [&](const vm::bbox3d&) {
      return Transform{vm::translation_matrix(vector), [&](mdl::Map& map) {
                         return mdl::translateSelection(map, vector);
                       }};
    },
    "The objects could not be moved by " + toText(vector) + ".",
    "Use a smaller vector or keep the objects inside the world bounds.");
  if (result.is_success())
  {
    // models that now reach into floors or furniture, or float
    if (const auto moved = resolveTargets(context, args, "ids", TransformableKinds);
        moved.is_success())
    {
      warnModelPlacement(context, moved.value());
    }
  }
  return result;
}

// objects_rotate

ToolResult objectsRotate(CallContext& context, const Args& args)
{
  const auto angle = args.get<double>("angle");
  if (std::abs(std::remainder(angle, 360.0)) < Epsilon)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "An angle of " + std::to_string(angle) + " degrees does not rotate anything.",
      "Pass a non-zero angle in degrees, e.g. 90.");
  }

  auto axis = axisArgument(args, "axis");
  if (axis.is_error())
  {
    return errorOf(axis);
  }

  const auto center = args.getOptional<vm::vec3d>("center");
  const auto updateEntityAngles = args.get<bool>("updateEntityAngles");
  auto& map = context.map();
  const auto angles = ScopedUpdateEntityAngles{map, updateEntityAngles};

  return transformTargets(
    context,
    args,
    [&](const vm::bbox3d& bounds) {
      const auto c = center.value_or(bounds.center());
      return Transform{
        vm::translation_matrix(c)
          * vm::rotation_matrix(axis.value(), vm::to_radians(angle))
          * vm::translation_matrix(-c),
        [&, c](mdl::Map& m) {
          return mdl::rotateSelection(m, c, axis.value(), vm::to_radians(angle));
        }};
    },
    "The objects could not be rotated.",
    "Rotate around a center closer to the objects, or keep them inside the world "
    "bounds.");
}

// objects_scale

std::optional<ToolError> checkScaleSource(const vm::bbox3d& bounds)
{
  const auto size = bounds.size();
  for (size_t i = 0; i < 3; ++i)
  {
    if (size[i] < Epsilon)
    {
      return makeError(
        ErrorCode::InvalidGeometry,
        "The objects are flat along " + std::string(1, "xyz"[i])
          + ", so they cannot be scaled to a box.",
        "Scale with 'factors' instead.");
    }
  }
  return std::nullopt;
}

ToolResult objectsScale(CallContext& context, const Args& args)
{
  const auto factors = args.getOptional<vm::vec3d>("factors");
  const auto box = args.getOptional<vm::bbox3d>("box");
  if (factors.has_value() == box.has_value())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass exactly one of 'factors' and 'box'.",
      "E.g. {\"factors\": [2, 2, 1]} or {\"box\": {\"min\": [0, 0, 0], \"max\": [128, "
      "128, 64]}}.");
  }

  if (box)
  {
    if (args.has("anchor"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "'anchor' is only used with 'factors'; the target box fixes the position.",
        "Remove 'anchor'.");
    }
    if (auto error = checkBox(context.map(), *box, "The target box"))
    {
      return *error;
    }

    auto targets = resolveTargets(context, args, "ids", TransformableKinds);
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    if (auto error = checkScaleSource(boundsOf(targets.value())))
    {
      error->objectIds = formatIds(targets.value(), context.ids());
      return *error;
    }

    return transformTargets(
      context,
      args,
      [&](const vm::bbox3d& bounds) {
        return Transform{vm::scale_bbox_matrix(bounds, *box), [&, bounds](mdl::Map& map) {
                           return mdl::scaleSelection(map, bounds, *box);
                         }};
      },
      "The objects could not be scaled to " + toJson(*box).dump() + ".",
      "Choose a target box inside the world bounds.");
  }

  for (size_t i = 0; i < 3; ++i)
  {
    if (!((*factors)[i] > 0.0))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Scale factors must be positive, but the factor for " + std::string(1, "xyz"[i])
          + " is " + std::to_string((*factors)[i]) + ".",
        "Use factors > 0; to mirror objects use objects_flip.");
    }
  }
  if (vm::is_equal(*factors, vm::vec3d{1, 1, 1}, Epsilon))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The factors [1, 1, 1] do not change anything.",
      "Pass factors other than 1, e.g. [2, 2, 1].");
  }

  const auto anchor = args.getOr<Json>("anchor", Json("center"));
  return transformTargets(
    context,
    args,
    [&](const vm::bbox3d& bounds) {
      const auto center = anchor == "min"     ? bounds.min
                          : anchor == "max"   ? bounds.max
                          : anchor.is_array() ? *vec3FromJson(anchor)
                                              : bounds.center();
      return Transform{
        vm::translation_matrix(center) * vm::scaling_matrix(*factors)
          * vm::translation_matrix(-center),
        [&, center](mdl::Map& map) {
          return mdl::scaleSelection(map, center, *factors);
        }};
    },
    "The objects could not be scaled by " + toText(*factors) + ".",
    "Use smaller factors or keep the objects inside the world bounds.");
}

// objects_shear

ToolResult objectsShear(CallContext& context, const Args& args)
{
  const auto side = args.get<std::string>("side");
  const auto offset = args.get<vm::vec3d>("offset");
  const auto axis = *axisIndex(side.substr(1));
  const auto normal = side[0] == '+' ? vm::vec3d::axis(axis) : -vm::vec3d::axis(axis);

  if (vm::is_zero(offset, Epsilon))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The offset is zero, so nothing would change.",
      "Pass a non-zero offset perpendicular to the side, e.g. [0, 32, 0] for side '+x'.");
  }
  if (std::abs(offset[axis]) > Epsilon)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The offset " + toText(offset) + " is not perpendicular to the side '" + side
        + "': its " + side.substr(1) + " component must be 0.",
      "Shearing moves the side within its plane. To make objects larger, use "
      "objects_scale.");
  }

  return transformTargets(
    context,
    args,
    [&](const vm::bbox3d& bounds) {
      if (bounds.size()[axis] < Epsilon)
      {
        // the shear matrix is undefined for flat objects
        return Transform{vm::mat4x4d::identity(), [](mdl::Map&) { return false; }};
      }
      return Transform{
        vm::shear_bbox_matrix(bounds, normal, offset), [&, bounds](mdl::Map& map) {
          return mdl::shearSelection(map, bounds, normal, offset);
        }};
    },
    "The objects could not be sheared.",
    "Use a smaller offset; objects that are flat along the side's axis cannot be "
    "sheared.");
}

// objects_flip

ToolResult objectsFlip(CallContext& context, const Args& args)
{
  const auto axis = *axisIndex(args.get<std::string>("axis"));
  const auto center = args.getOptional<vm::vec3d>("center");

  return transformTargets(
    context,
    args,
    [&](const vm::bbox3d& bounds) {
      const auto c = center.value_or(bounds.center());
      return Transform{
        vm::translation_matrix(c) * vm::mirror_matrix<double>(axis)
          * vm::translation_matrix(-c),
        [&, c](mdl::Map& map) { return mdl::flipSelection(map, c, axis); }};
    },
    "The objects could not be flipped.",
    "Flip around a center closer to the objects.");
}

// objects_duplicate

/**
 * Duplicates the selection. Returns the copies in the order of the originals (the new
 * selection), or an empty vector if duplicating failed.
 */
std::vector<mdl::Node*> duplicateSelection(mdl::Map& map)
{
  const auto originals = map.selection().nodes;
  mdl::duplicateSelectedNodes(map);
  auto copies = map.selection().nodes;
  if (copies.size() != originals.size() || copies == originals)
  {
    return {};
  }
  return copies;
}

ToolResult objectsDuplicate(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", TransformableKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto offset = args.getOptional<vm::vec3d>("offset");
  const auto lock = ScopedLockOverride{map, args.getOptional<bool>("alignmentLock")};

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      const auto originals = map.selection().nodes;
      const auto copies = duplicateSelection(map);
      if (copies.empty())
      {
        return context.operationFailed(
          "The objects could not be duplicated.",
          "details.editorMessages holds the editor's reason; check that the objects are "
          "editable (object_get).");
      }

      if (offset && !vm::is_zero(*offset, Epsilon))
      {
        if (!mdl::translateSelection(map, *offset))
        {
          return transformFailed(
            context,
            copies,
            vm::translation_matrix(*offset),
            "The copies could not be moved by " + toText(*offset) + ".",
            "Use a smaller offset.",
            formatIds(originals, ids));
        }
      }

      if (auto error = checkTransformed(context, copies))
      {
        return *error;
      }

      auto pairs = Json::array();
      for (size_t i = 0; i < originals.size(); ++i)
      {
        pairs.push_back(Json{
          {"original", ids.format(*originals[i])}, {"copy", ids.format(*copies[i])}});
      }
      return Json{
        {"copies", std::move(pairs)},
        {"objects", nodeSummaries(copies, ids)},
      };
    },
    SelectionAfter::Result);
}

// objects_delete

ToolResult objectsDelete(CallContext& context, const Args& args)
{
  auto targets = resolveTargets(context, args, "ids", TransformableKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  const auto removed = formatIds(targets.value(), context.ids());

  return withTargets(context, targets.value(), [&]() -> ToolResult {
    mdl::removeSelectedNodes(map);
    return Json{{"removed", removed}, {"count", removed.size()}};
  });
}

// objects_array

/** A unit vector perpendicular to the given unit axis (x for the z axis). */
vm::vec3d perpendicular(const vm::vec3d& axis)
{
  const auto reference =
    std::abs(axis.x()) < 0.9 ? vm::vec3d{1, 0, 0} : vm::vec3d{0, 1, 0};
  return vm::normalize(reference - axis * vm::dot(reference, axis));
}

vm::vec3d rotatePoint(
  const vm::vec3d& point,
  const vm::vec3d& center,
  const vm::vec3d& axis,
  const double angleDegrees)
{
  const auto rotation = vm::translation_matrix(center)
                        * vm::rotation_matrix(axis, vm::to_radians(angleDegrees))
                        * vm::translation_matrix(-center);
  return rotation * point;
}

struct ArrayPlan
{
  std::string pattern;
  size_t count = 0;
  // line
  vm::vec3d offset;
  // grid
  std::vector<size_t> counts;
  vm::vec3d spacing;
  // circle
  vm::vec3d center;
  vm::vec3d axis;
  double angleStep = 0.0;
  std::optional<double> radius;
  double startAngle = 0.0;
  bool rotate = true;
  double rise = 0.0;
};

Result<ArrayPlan, ToolError> arrayPlan(const Args& args)
{
  auto plan = ArrayPlan{};
  plan.pattern = args.get<std::string>("pattern");
  const auto count = args.getOptional<size_t>("count");

  const auto require = [&](const std::string& key) -> std::optional<ToolError> {
    if (!args.has(key))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Pattern '" + plan.pattern + "' requires '" + key + "'.",
        "See the tool description for the parameters of each pattern.");
    }
    return std::nullopt;
  };

  if (plan.pattern == "grid")
  {
    if (auto error = require("counts"))
    {
      return *error;
    }
    if (auto error = require("spacing"))
    {
      return *error;
    }
    plan.counts = args.get<std::vector<size_t>>("counts");
    plan.spacing = args.get<vm::vec3d>("spacing");
    plan.count = plan.counts[0] * plan.counts[1] * plan.counts[2];
    if (plan.count < 2)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The grid [1, 1, 1] contains only the original.",
        "Use counts with a total of at least 2, e.g. [4, 1, 1].");
    }
    if (count && *count != plan.count)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "count " + std::to_string(*count) + " does not match the grid counts (total "
          + std::to_string(plan.count) + ").",
        "Omit 'count' for the grid pattern.");
    }
    for (size_t i = 0; i < 3; ++i)
    {
      if (plan.counts[i] > 1 && std::abs(plan.spacing[i]) < Epsilon)
      {
        return makeError(
          ErrorCode::InvalidArgument,
          "The spacing along " + std::string(1, "xyz"[i])
            + " is 0, so the copies along that axis would overlap.",
          "Pass a non-zero spacing for every axis with a count greater than 1.");
      }
    }
  }
  else
  {
    if (auto error = require("count"))
    {
      return *error;
    }
    plan.count = *count;
  }

  if (plan.count > MaxArrayInstances)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "At most " + std::to_string(MaxArrayInstances)
        + " instances can be created at once.",
      "Use a smaller count, or call objects_array several times.");
  }

  if (plan.pattern == "line")
  {
    if (auto error = require("offset"))
    {
      return *error;
    }
    plan.offset = args.get<vm::vec3d>("offset");
    if (vm::is_zero(plan.offset, Epsilon))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The offset is zero, so all copies would overlap.",
        "Pass the offset between neighboring instances, e.g. [64, 0, 0].");
    }
  }
  else if (plan.pattern == "circle")
  {
    if (auto error = require("center"))
    {
      return *error;
    }
    plan.center = args.get<vm::vec3d>("center");
    auto axis = axisArgument(args, "axis");
    if (axis.is_error())
    {
      return errorOf(axis);
    }
    plan.axis = axis.value();
    plan.angleStep = args.getOr<double>("angleStep", 360.0 / double(plan.count));
    plan.radius = args.getOptional<double>("radius");
    plan.startAngle = args.get<double>("startAngle");
    plan.rotate = args.get<bool>("rotate");
    plan.rise = args.get<double>("rise");
    if (std::abs(plan.angleStep) < Epsilon && std::abs(plan.rise) < Epsilon)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "With angleStep 0 and rise 0 all copies would overlap.",
        "Pass a non-zero angleStep or rise.");
    }
  }

  return plan;
}

/** The transform that moves a copy of the originals into place as instance `index`. */
Transform instanceTransform(
  const ArrayPlan& plan, const size_t index, const vm::vec3d& originalCenter)
{
  const auto i = double(index);
  const auto translate = [](const vm::vec3d& delta) {
    return Transform{vm::translation_matrix(delta), [=](mdl::Map& map) {
                       return vm::is_zero(delta, Epsilon)
                              || mdl::translateSelection(map, delta);
                     }};
  };

  if (plan.pattern == "line")
  {
    return translate(plan.offset * i);
  }
  if (plan.pattern == "grid")
  {
    const auto nx = plan.counts[0];
    const auto ny = plan.counts[1];
    const auto ix = double(index % nx);
    const auto iy = double((index / nx) % ny);
    const auto iz = double(index / (nx * ny));
    return translate(
      vm::vec3d{ix * plan.spacing.x(), iy * plan.spacing.y(), iz * plan.spacing.z()});
  }

  // circle
  const auto angle = plan.angleStep * i;
  const auto rise = plan.axis * (plan.rise * i);
  if (!plan.rotate)
  {
    const auto target = rotatePoint(originalCenter, plan.center, plan.axis, angle) + rise;
    return translate(target - originalCenter);
  }

  const auto rotate = std::abs(std::remainder(angle, 360.0)) > Epsilon;
  const auto rotation = vm::translation_matrix(plan.center)
                        * vm::rotation_matrix(plan.axis, vm::to_radians(angle))
                        * vm::translation_matrix(-plan.center);
  return Transform{
    vm::translation_matrix(rise) * rotation, [=, &plan](mdl::Map& map) {
      return (!rotate
              || mdl::rotateSelection(map, plan.center, plan.axis, vm::to_radians(angle)))
             && (vm::is_zero(rise, Epsilon) || mdl::translateSelection(map, rise));
    }};
}

ToolResult objectsArray(CallContext& context, const Args& args)
{
  auto plan = arrayPlan(args);
  if (plan.is_error())
  {
    return errorOf(plan);
  }

  auto targets = resolveTargets(context, args, "ids", TransformableKinds);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  auto& ids = context.ids();
  const auto lock = ScopedLockOverride{map, args.getOptional<bool>("alignmentLock")};
  const auto angles = ScopedUpdateEntityAngles{map, args.get<bool>("updateEntityAngles")};
  const auto& p = plan.value();

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      const auto originals = map.selection().nodes;
      const auto originalIds = formatIds(originals, ids);

      if (p.pattern == "circle" && p.radius)
      {
        // move the originals radially to the requested distance from the center
        const auto center = boundsOf(originals).center();
        const auto v = center - p.center;
        const auto radial = v - p.axis * vm::dot(v, p.axis);
        const auto direction =
          vm::length(radial) > Epsilon
            ? vm::normalize(radial)
            : vm::normalize(rotatePoint(
                perpendicular(p.axis), vm::vec3d{0, 0, 0}, p.axis, p.startAngle));
        const auto delta = direction * *p.radius - radial;
        if (!vm::is_zero(delta, Epsilon) && !mdl::translateSelection(map, delta))
        {
          return transformFailed(
            context,
            originals,
            vm::translation_matrix(delta),
            "The objects could not be moved to radius " + std::to_string(*p.radius) + ".",
            "Use a smaller radius.");
        }
      }

      const auto originalCenter = boundsOf(originals).center();
      auto instances = std::vector<std::vector<mdl::Node*>>{originals};
      auto created = std::vector<mdl::Node*>{};
      for (size_t i = 1; i < p.count; ++i)
      {
        mdl::deselectAll(map);
        mdl::selectNodes(map, originals);

        const auto copies = duplicateSelection(map);
        if (copies.empty())
        {
          return context.operationFailed(
            "Instance " + std::to_string(i) + " could not be created.",
            "details.editorMessages holds the editor's reason; try a smaller count or "
            "check that the copies stay inside the world bounds.");
        }
        const auto transform = instanceTransform(p, i, originalCenter);
        if (!transform.apply(map))
        {
          return transformFailed(
            context,
            copies,
            transform.matrix,
            "Instance " + std::to_string(i) + " could not be placed.",
            "Use a smaller count, offset or radius so that all instances stay inside "
            "the world bounds.",
            originalIds);
        }
        instances.push_back(copies);
        created.insert(created.end(), copies.begin(), copies.end());
      }

      auto all = std::vector<mdl::Node*>{};
      for (const auto& instance : instances)
      {
        all.insert(all.end(), instance.begin(), instance.end());
      }
      if (
        auto error = checkInsideWorldBounds(
          all,
          map,
          ids,
          "Use a smaller count, offset or radius so that all instances stay inside the "
          "world bounds."))
      {
        return *error;
      }
      warnNonIntegerVertices(context, all);

      // leave the whole array selected, e.g. for group_create
      mdl::deselectAll(map);
      mdl::selectNodes(map, all);

      auto instanceIds = Json::array();
      for (const auto& instance : instances)
      {
        instanceIds.push_back(formatIds(instance, ids));
      }
      auto summaries = std::vector<mdl::Node*>{
        created.begin(),
        created.begin() + std::ptrdiff_t(std::min(created.size(), MaxSummaries))};

      return Json{
        {"pattern", p.pattern},
        {"count", p.count},
        {"instances", std::move(instanceIds)},
        {"created", formatIds(created, ids)},
        {"objects", nodeSummaries(summaries, ids)},
        {"truncated", created.size() > MaxSummaries},
      };
    },
    SelectionAfter::Result);
}

// command_repeat

ToolResult commandRepeat(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto times = args.get<size_t>("times");

  if (map.commandProcessor().transactionDepth() > 0)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "Commands cannot be repeated while a transaction is open.",
      "Commit the transaction with transaction_commit first.");
  }

  if (!map.canRepeatCommands())
  {
    context.warn(
      "NOTHING_TO_REPEAT",
      "There is nothing to repeat: the repeat list is empty (it is cleared by undo, redo "
      "and command_repeat_clear).");
    return Json{{"times", 0}, {"objects", Json::array()}};
  }

  // The repeat stack does not repeat while a map transaction is open, so this tool does
  // not run in the call runner's transaction. Instead, it collects the repeated commands
  // in a command processor transaction, which does not involve the repeat stack. The
  // scope is long running so that nested transactions update linked groups on commit.
  auto& commandProcessor = map.commandProcessor();
  const auto undoStepName = "AI: " + context.tool().title();
  commandProcessor.startTransaction(undoStepName, mdl::TransactionScope::LongRunning);

  for (size_t i = 0; i < times; ++i)
  {
    map.repeatCommands();
  }

  const auto& selected = map.selection().nodes;
  auto result = Json{
    {"times", times},
    {"objects",
     nodeSummaries(
       std::vector<mdl::Node*>{
         selected.begin(),
         selected.begin() + std::ptrdiff_t(std::min(selected.size(), MaxSummaries))},
       ids)},
  };

  const auto problems = context.loggedProblems();
  const auto failed = std::ranges::any_of(
    problems, [](const auto& message) { return message.level == LogLevel::Error; });
  auto outOfBounds = checkInsideWorldBounds(selected, map, ids);

  if (failed || outOfBounds || context.dryRun())
  {
    commandProcessor.rollbackTransaction();
    commandProcessor.commitTransaction();
    if (outOfBounds)
    {
      return *outOfBounds;
    }
    if (failed)
    {
      return context.operationFailed(
        "Repeating the commands failed, so the changes were rolled back.",
        "Check the selection; repeated commands act on the current selection.");
    }
    return result;
  }

  auto stored = false;
  auto connection = NotifierConnection{};
  connection += context.document().transactionDoneNotifier.connect(
    [&](const std::string& name, bool, bool) {
      if (name == undoStepName)
      {
        stored = true;
      }
    });

  const auto collationEnabled = map.isCommandCollationEnabled();
  map.setIsCommandCollationEnabled(false);
  commandProcessor.commitTransaction();
  map.setIsCommandCollationEnabled(collationEnabled);

  if (stored)
  {
    context.setUndoStep(undoStepName);
  }
  warnNonIntegerVertices(context, selected);
  return result;
}

ToolResult commandRepeatClear(CallContext& context, const Args&)
{
  auto& map = context.map();
  if (map.commandProcessor().transactionDepth() > 0)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "The repeat list cannot be cleared while a transaction is open.",
      "Commit the transaction with transaction_commit first.");
  }

  const auto hadCommands = map.canRepeatCommands();
  if (!context.dryRun())
  {
    map.clearRepeatableCommands();
  }
  return Json{{"cleared", hadCommands}};
}

Schema objectsResultSchema()
{
  return object({
    field("objects", array(any())).required().describe("Summaries of the objects"),
    field("bounds", box()).required().describe("Bounds of the objects afterwards"),
  });
}

} // namespace

void registerTransformTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"objects_move"}
      .title("Move Objects")
      .description(
        "Moves objects by a vector in map units, like dragging them or using the move "
        "shortcuts (one undo step). Respects texture lock unless alignmentLock is false. "
        "Fails with "
        "OUT_OF_WORLD_BOUNDS if an object would reach the world bounds. Moved point "
        "entities whose model can be loaded are checked like entity_placement_check "
        "(warnings MODEL_BELOW_FLOOR, MODEL_FLOATING, MODEL_PENETRATES_BRUSHES, "
        "MODEL_NO_FLOOR). Use objects_duplicate to move copies instead. "
        "Example: {\"ids\": [\"brush:12\"], \"vector\": [64, 0, 0]}")
      .input(object({
        transformIdsField(),
        field("vector", vec3()).required().describe("Offset [x, y, z] in map units"),
        alignmentLockField(),
      }))
      .output(objectsResultSchema())
      .mutation(Mutation::Map)
      .handler(objectsMove));

  registry.add(
    ToolDef{"objects_rotate"}
      .title("Rotate Objects")
      .description(
        "Rotates objects counter-clockwise (right-handed) by an angle in degrees around "
        "an axis through a center (default: the center of the objects' bounds), in one "
        "undo step. Like the "
        "editor's rotate tool, it updates entity angle properties (angle, angles, "
        "mangle) unless updateEntityAngles is false, and respects texture lock. "
        "Rotations "
        "by angles other than multiples of 90 produce non-integer vertices, which are "
        "reported as NON_INTEGER_VERTICES. "
        "Example: {\"ids\": [\"entity:7\"], \"angle\": 90, \"axis\": \"z\"}")
      .input(object({
        transformIdsField(),
        field("angle", angle())
          .required()
          .describe(
            "Angle in degrees; positive turns counter-clockwise seen from the tip of "
            "the axis"),
        field("axis", axisSchema().defaultsTo("z")),
        field("center", vec3())
          .describe(
            "A point on the rotation axis (map units). Default: the center of the "
            "objects' bounds"),
        field("updateEntityAngles", boolean().defaultsTo(true))
          .describe("Update the angle properties of rotated entities"),
        alignmentLockField(),
      }))
      .output(objectsResultSchema())
      .mutation(Mutation::Map)
      .handler(objectsRotate));

  registry.add(
    ToolDef{"objects_scale"}
      .title("Scale Objects")
      .description(
        "Scales objects either by 'factors' [x, y, z] (all > 0) around an 'anchor' "
        "('center' of the bounds (default), 'min' or 'max' corner, or a point), or so "
        "that their bounds fit the target 'box' (map units); pass exactly one of "
        "'factors' and 'box'. One undo step. Respects texture lock. To mirror, use "
        "objects_flip. "
        "Examples: {\"factors\": [2, 2, 1], \"anchor\": \"min\"}; {\"box\": {\"min\": "
        "[0, 0, 0], \"max\": [256, 128, 128]}}")
      .input(object({
        transformIdsField(),
        field("factors", vec3()).describe("Scale factors per axis, all > 0"),
        field("box", schema::box()).describe("Target bounds of the objects (map units)"),
        field(
          "anchor",
          oneOf({
            enumOf({"center", "min", "max"})
              .describe("The center or the min / max corner of the objects' bounds"),
            vec3().describe("A point [x, y, z] in map units"),
          }))
          .describe(
            "With factors: the fixed point, 'center' (default), 'min', 'max' or [x, y, "
            "z]"),
        alignmentLockField(),
      }))
      .output(objectsResultSchema())
      .mutation(Mutation::Map)
      .handler(objectsScale));

  registry.add(
    ToolDef{"objects_shear"}
      .title("Shear Objects")
      .description(
        "Shears objects like the editor's shear tool: the given side of the objects' "
        "bounds box moves by 'offset' within its plane while the opposite side stays "
        "fixed (one undo step). The offset must be perpendicular to the side's normal; "
        "e.g. side '+z' with offset [32, 0, 0] leans the top 32 units towards +x. "
        "Example: {\"ids\": [\"brush:12\"], \"side\": \"+z\", \"offset\": [32, 0, 0]}")
      .input(object({
        transformIdsField(),
        field("side", enumOf({"+x", "-x", "+y", "-y", "+z", "-z"}))
          .required()
          .describe("The side of the bounds box that moves"),
        field("offset", vec3())
          .required()
          .describe("How far the side moves (map units); the component along the side's "
                    "axis must be 0"),
        alignmentLockField(),
      }))
      .output(objectsResultSchema())
      .mutation(Mutation::Map)
      .handler(objectsShear));

  registry.add(
    ToolDef{"objects_flip"}
      .title("Flip Objects")
      .description(
        "Mirrors objects along an axis through a center (default: the center of their "
        "bounds, so they stay in place), in one undo step. Entity angles are mirrored as "
        "well. "
        "Example: {\"ids\": [\"brush:12\"], \"axis\": \"x\"}")
      .input(object({
        transformIdsField(),
        field("axis", enumOf({"x", "y", "z"}))
          .required()
          .describe("Axis to mirror along ('x' swaps -x and +x)"),
        field("center", vec3())
          .describe("A point on the mirror plane (map units). Default: the center of the "
                    "objects' bounds"),
      }))
      .output(objectsResultSchema())
      .mutation(Mutation::Map)
      .handler(objectsFlip));

  registry.add(
    ToolDef{"objects_duplicate"}
      .title("Duplicate Objects")
      .description(
        "Duplicates objects like Edit > Duplicate, optionally moving the copies by an "
        "offset, and selects the copies (one undo step). Returns which copy belongs to "
        "which original. "
        "Duplicate with an offset, then command_repeat, to make more copies with the "
        "same spacing. "
        "Example: {\"ids\": [\"brush:12\"], \"offset\": [0, 128, 0]}")
      .input(object({
        transformIdsField(),
        field("offset", vec3())
          .describe("Move the copies by this vector (map units). Default: no offset"),
        alignmentLockField(),
      }))
      .output(object({
        field("copies", array(any()))
          .required()
          .describe("[{original, copy}] in the order of the originals"),
        field("objects", array(any())).required().describe("Summaries of the copies"),
      }))
      .mutation(Mutation::Map)
      .handler(objectsDuplicate));

  registry.add(
    ToolDef{"objects_delete"}
      .title("Delete Objects")
      .description(
        "Deletes objects like Edit > Delete. Brush entities and groups that become empty "
        "are removed as well (see changes.removed). One undo step. "
        "Example: {\"ids\": [\"brush:12\", \"entity:40\"]}")
      .input(object({idsField(
        TransformableKinds,
        "Objects to delete (groups with their contents, entities, brushes, patches). "
        "Default: the current selection")}))
      .output(object({
        field("removed", array(string())).required().describe("The deleted objects"),
        field("count", integer()).required().describe("Number of deleted objects"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(objectsDelete));

  registry.add(
    ToolDef{"objects_array"}
      .title("Array Objects")
      .description(
        "Makes copies of objects in a pattern, as one undo step. 'count' is the total "
        "number of instances INCLUDING the originals (count 12 = the original plus 11 "
        "copies). Patterns: "
        "'line': instance i is moved by i * 'offset'. "
        "'grid': 'counts' [nx, ny, nz] instances spaced by 'spacing' [x, y, z]. "
        "'circle': instance i is rotated by i * 'angleStep' (default 360 / count) around "
        "the 'axis' (default z) through 'center' and raised by i * 'rise' along the axis "
        "(spiral staircases). With 'radius', the originals are first moved radially so "
        "that their bounds center is at that distance from the center (at 'startAngle' "
        "if they lie on the axis). 'rotate' (default true) turns the copies so that they "
        "keep facing the center, including entity angles; false only moves them. "
        "Leaves all instances selected. Non-integer vertices are reported as "
        "NON_INTEGER_VERTICES. "
        "Examples: {\"pattern\": \"circle\", \"count\": 12, \"center\": [0, 0, 0], "
        "\"radius\": 384}; {\"pattern\": \"line\", \"count\": 5, \"offset\": [64, 0, "
        "0]}; {\"pattern\": \"grid\", \"counts\": [4, 2, 1], \"spacing\": [128, 128, 0]}")
      .input(object({
        transformIdsField(),
        field("pattern", enumOf({"line", "grid", "circle"}))
          .required()
          .describe("Layout of the instances; see the description for the parameters "
                    "of each"),
        field("count", integer().min(2).max(double(MaxArrayInstances)))
          .describe("Total number of instances including the originals (line, circle)"),
        field("offset", vec3())
          .describe("line: offset between neighboring instances (map units)"),
        field("counts", array(integer().min(1)).minSize(3).maxSize(3))
          .describe(
            "grid: instances along x, y and z; their product is the total, so omit "
            "'count'"),
        field("spacing", vec3())
          .describe("grid: distance between instances per axis (map units)"),
        field("center", vec3())
          .describe("circle: a point on the rotation axis (map units)"),
        field("axis", axisSchema()).describe("circle: rotation axis, default 'z'"),
        field("angleStep", angle())
          .describe("circle: rotation between neighboring instances in degrees. Default: "
                    "360 / count"),
        field("radius", number().min(0))
          .describe("circle: move the originals to this distance (map units) from the "
                    "axis first"),
        field("startAngle", angle().defaultsTo(0))
          .describe(
            "circle: angle in degrees of the originals around the axis when they lie on "
            "it (0 = +x for the z axis)"),
        field("rotate", boolean().defaultsTo(true))
          .describe("circle: rotate the copies to keep facing the center"),
        field("rise", number().defaultsTo(0))
          .describe("circle: offset along the axis per instance (map units)"),
        field("updateEntityAngles", boolean().defaultsTo(true))
          .describe("Update the angle properties of rotated entities"),
        alignmentLockField(),
      }))
      .output(object({
        field("pattern", string()).required(),
        field("count", integer())
          .required()
          .describe("Instances including the originals"),
        field("instances", array(array(string())))
          .required()
          .describe("Object ids per instance; the first instance is the originals"),
        field("created", array(string())).required().describe("Ids of all copies"),
        field("objects", array(any()))
          .required()
          .describe("Summaries of the copies (at most 100)"),
        field("truncated", boolean())
          .required()
          .describe("Whether 'objects' was cut at 100"),
      }))
      .mutation(Mutation::Map)
      .handler(objectsArray));

  registry.add(
    ToolDef{"command_repeat"}
      .title("Repeat Last Commands")
      .description(
        "Repeats the recorded transforms and duplications (Edit > Repeat) on the CURRENT "
        "selection, 'times' times, as one undo step. The editor records every move, "
        "rotate, scale, shear, flip and duplicate; each agent call is recorded as one "
        "entry, even if it named its objects with 'ids'. A selection change by the human "
        "starts a new recording with the next recorded command; undo, redo and "
        "command_repeat_clear empty it. Agent calls that change the selection "
        "(selection_set, ...) do not start a new recording, so call command_repeat_clear "
        "before a new sequence. Typical use: objects_duplicate {offset} (the copy is "
        "selected), then command_repeat {times: 5} makes 5 more copies with the same "
        "spacing. Warns with NOTHING_TO_REPEAT if nothing was recorded. Not available "
        "inside an agent transaction. "
        "Example: {\"times\": 3}")
      .input(object({
        field("times", integer().min(1).max(1000).defaultsTo(1))
          .describe("How often to repeat the recorded commands"),
      }))
      .output(object({
        field("times", integer()).required().describe("0 if there was nothing to repeat"),
        field("objects", array(any()))
          .required()
          .describe("Summaries of the selected objects afterwards (at most 100)"),
      }))
      .mutation(Mutation::Map)
      .transactional(false)
      .handler(commandRepeat));

  registry.add(
    ToolDef{"command_repeat_clear"}
      .title("Clear Repeatable Commands")
      .description(
        "Empties the list of recorded commands that command_repeat repeats, like Edit > "
        "Clear Repeatable Commands. Not undoable. Example: {}")
      .input(object({}))
      .output(object({
        field("cleared", boolean())
          .required()
          .describe("Whether there were recorded commands"),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .handler(commandRepeatClear));
}

} // namespace tb::mcp
