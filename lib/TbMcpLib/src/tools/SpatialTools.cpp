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

#include "mcp/tools/SpatialTools.h"

#include "NodeJson.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/Map.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/intersection.h"
#include "vm/plane.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** Offset used to make touching surfaces not count as overlapping, and to inset rays. */
constexpr auto Epsilon = 0.01;

/** Rays of space_check that hit within this distance of the box count as support. */
constexpr auto SupportTolerance = 1.0;

constexpr auto MaxPlanCells = size_t(200);

// Classification of brushes by their owning entity

enum class BrushClass
{
  /** World brushes and brushes of func_group / func_detail* entities. */
  Solid,
  /** Brushes of other brush entities (doors, platforms, ...). */
  Entity,
  /** Brushes of trigger_* entities. */
  Trigger,
};

const mdl::EntityNode* owningBrushEntity(const mdl::Node& node)
{
  return dynamic_cast<const mdl::EntityNode*>(mdl::findContainingEntity(&node));
}

BrushClass classifyBrush(const mdl::BrushNode& brushNode)
{
  if (const auto* entityNode = owningBrushEntity(brushNode))
  {
    const auto& classname = entityNode->entity().classname();
    if (classname.starts_with("trigger_"))
    {
      return BrushClass::Trigger;
    }
    if (classname == "func_group" || classname.starts_with("func_detail"))
    {
      return BrushClass::Solid;
    }
    return BrushClass::Entity;
  }
  return BrushClass::Solid;
}

bool isPointEntity(const mdl::Node& node)
{
  const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
  return entityNode && !entityNode->hasChildren();
}

/** Brush entities are in the node tree, too, but they are represented by their brushes.
 */
bool isContainer(const mdl::Node& node)
{
  const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
  return entityNode && entityNode->hasChildren();
}

// Common filters

struct NodeFilter
{
  const mdl::EditorContext& editorContext;
  bool includeHidden = false;
  std::vector<const mdl::Node*> ignored = {};

  bool visible(const mdl::Node& node) const
  {
    return includeHidden || editorContext.visible(node);
  }

  bool isIgnored(const mdl::Node& node) const
  {
    return std::ranges::any_of(ignored, [&](const auto* ignoredNode) {
      return &node == ignoredNode || node.isDescendantOf(*ignoredNode);
    });
  }

  bool accepts(const mdl::Node& node) const
  {
    return !isContainer(node) && visible(node) && !isIgnored(node);
  }
};

Result<std::vector<const mdl::Node*>, ToolError> resolveIds(
  CallContext& context, const Args& args, const std::string_view key)
{
  auto result = std::vector<const mdl::Node*>{};
  for (const auto& id : args.getOr<std::vector<std::string>>(key, {}))
  {
    auto node = context.ids().resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    result.push_back(node.value());
  }
  return result;
}

// Exact geometry

/**
 * Whether the interior of the convex brush intersects the given box (separating axis
 * test over the brush face normals, the box axes and the cross products of the brush
 * edges with the box axes). Surfaces that merely touch do not count. The box may be
 * degenerate (e.g. flat).
 */
bool intersectsInterior(const mdl::Brush& brush, const vm::bbox3d& box)
{
  const auto& bounds = brush.bounds();
  for (size_t i = 0; i < 3; ++i)
  {
    const auto separated = box.min[i] == box.max[i]
                             ? bounds.min[i] >= box.min[i] || bounds.max[i] <= box.max[i]
                             : bounds.max[i] <= box.min[i] || bounds.min[i] >= box.max[i];
    if (separated)
    {
      return false;
    }
  }

  const auto corners = box.vertices();
  for (const auto& face : brush.faces())
  {
    if (std::ranges::all_of(corners, [&](const auto& corner) {
          return face.boundary().point_distance(corner) >= 0.0;
        }))
    {
      return false;
    }
  }

  const auto vertices = brush.vertexPositions();
  const auto axes =
    std::array{vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}, vm::vec3d{0, 0, 1}};
  for (const auto* edge : brush.edges())
  {
    const auto direction =
      edge->secondVertex()->position() - edge->firstVertex()->position();
    for (const auto& axis : axes)
    {
      const auto normal = vm::cross(direction, axis);
      if (vm::squared_length(normal) < 1e-12)
      {
        continue;
      }

      auto brushMin = std::numeric_limits<double>::max();
      auto brushMax = std::numeric_limits<double>::lowest();
      for (const auto& vertex : vertices)
      {
        const auto d = vm::dot(normal, vertex);
        brushMin = std::min(brushMin, d);
        brushMax = std::max(brushMax, d);
      }
      auto boxMin = std::numeric_limits<double>::max();
      auto boxMax = std::numeric_limits<double>::lowest();
      for (const auto& corner : corners)
      {
        const auto d = vm::dot(normal, corner);
        boxMin = std::min(boxMin, d);
        boxMax = std::max(boxMax, d);
      }
      if (brushMax <= boxMin || boxMax <= brushMin)
      {
        return false;
      }
    }
  }
  return true;
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

vm::bbox3d intersection(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  return vm::bbox3d{vm::max(lhs.min, rhs.min), vm::min(lhs.max, rhs.max)};
}

// Ray casting

struct RayHit
{
  double distance;
  vm::vec3d point;
  mdl::Node* node;
  std::optional<size_t> faceIndex;
};

/**
 * Casts the ray against brushes, patches and point entities accepted by the given
 * predicate. Like the editor's picking, brush faces are only hit from the front, so a ray
 * starting inside a brush does not hit that brush, and a ray starting inside the bounds
 * of a point entity does not hit that entity. Returns the hits sorted by distance.
 */
std::vector<RayHit> castRay(
  mdl::Map& map,
  const vm::ray3d& ray,
  const std::function<bool(const mdl::Node&)>& accept,
  const std::optional<double> maxDistance = std::nullopt)
{
  auto hits = std::vector<RayHit>{};
  const auto addHit = [&](
                        const double distance,
                        mdl::Node* node,
                        const std::optional<size_t> faceIndex = std::nullopt) {
    if (!maxDistance || distance <= *maxDistance)
    {
      hits.push_back({distance, vm::point_at_distance(ray, distance), node, faceIndex});
    }
  };

  for (auto* node : map.worldNode().nodeTree().find_intersectors(ray))
  {
    if (!accept(*node))
    {
      continue;
    }

    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
    {
      const auto& brush = brushNode->brush();
      for (size_t i = 0; i < brush.faceCount(); ++i)
      {
        if (const auto distance = brush.face(i).intersectWithRay(ray))
        {
          // a convex brush has at most one front facing hit
          addHit(*distance, brushNode, i);
          break;
        }
      }
    }
    else if (isPointEntity(*node))
    {
      const auto& bounds = node->logicalBounds();
      if (!bounds.contains(ray.origin))
      {
        if (const auto distance = vm::intersect_ray_bbox(ray, bounds))
        {
          addHit(*distance, node);
        }
      }
    }
    else if (auto* patchNode = dynamic_cast<mdl::PatchNode*>(node))
    {
      auto pickResult = mdl::PickResult{};
      patchNode->pick(map.editorContext(), ray, pickResult);
      for (const auto& hit : pickResult.all())
      {
        addHit(hit.distance(), patchNode);
      }
    }
  }

  std::ranges::sort(
    hits, [](const auto& lhs, const auto& rhs) { return lhs.distance < rhs.distance; });
  return hits;
}

Json hitJson(CallContext& context, const RayHit& hit)
{
  const auto& ids = context.ids();
  auto result = Json{
    {"object", ids.format(*hit.node)},
    {"kind", std::string{toString(objectKindOf(*hit.node))}},
    {"label", nodeLabel(*hit.node)},
  };
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(hit.node);
      brushNode && hit.faceIndex)
  {
    const auto& face = brushNode->brush().face(*hit.faceIndex);
    result["face"] = ids.formatFace(*brushNode, *hit.faceIndex);
    result["material"] = face.materialName();
    result["normal"] = toJson(face.normal());
  }
  result["point"] = toJson(hit.point);
  result["distance"] = roundForOutput(hit.distance);
  if (const auto* entityNode = owningBrushEntity(*hit.node))
  {
    result["entity"] = ids.format(*entityNode);
    result["classname"] = entityNode->entity().classname();
  }
  if (auto group = groupIdOf(*hit.node, ids); !group.is_null())
  {
    result["group"] = std::move(group);
  }
  return result;
}

Json compactJson(const mdl::Node& node, const IdRegistry& ids)
{
  return Json{
    {"id", ids.format(node)},
    {"kind", std::string{toString(objectKindOf(node))}},
    {"label", nodeLabel(node)},
  };
}

std::vector<std::string> nodeKindNames()
{
  return {"brush", "entity", "patch"};
}

// objects_at_point

enum class Relation
{
  None,
  Inside,
  Touching,
};

Relation relationToBrush(
  const mdl::Brush& brush, const vm::vec3d& point, const double tolerance)
{
  auto maxDistance = std::numeric_limits<double>::lowest();
  for (const auto& face : brush.faces())
  {
    maxDistance = std::max(maxDistance, face.boundary().point_distance(point));
  }
  return maxDistance < -tolerance   ? Relation::Inside
         : maxDistance <= tolerance ? Relation::Touching
                                    : Relation::None;
}

Relation relationToBounds(
  const vm::bbox3d& bounds, const vm::vec3d& point, const double tolerance)
{
  auto inside = true;
  for (size_t i = 0; i < 3; ++i)
  {
    if (point[i] < bounds.min[i] - tolerance || point[i] > bounds.max[i] + tolerance)
    {
      return Relation::None;
    }
    if (point[i] <= bounds.min[i] + tolerance || point[i] >= bounds.max[i] - tolerance)
    {
      inside = false;
    }
  }
  return inside ? Relation::Inside : Relation::Touching;
}

ToolResult objectsAtPoint(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto point = args.get<vm::vec3d>("point");
  const auto tolerance = args.get<double>("tolerance");
  const auto limit = size_t(args.get<int64_t>("limit"));
  const auto filter = NodeFilter{map.editorContext(), args.get<bool>("includeHidden")};

  struct Found
  {
    mdl::Node* node;
    Relation relation;
  };
  auto found = std::vector<Found>{};

  const auto query =
    vm::bbox3d{point - vm::vec3d::fill(tolerance), point + vm::vec3d::fill(tolerance)};
  for (auto* node : map.worldNode().nodeTree().find_intersectors(query))
  {
    if (!filter.accepts(*node))
    {
      continue;
    }
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    const auto relation = brushNode
                            ? relationToBrush(brushNode->brush(), point, tolerance)
                            : relationToBounds(node->logicalBounds(), point, tolerance);
    if (relation != Relation::None)
    {
      found.push_back({node, relation});
    }
  }
  std::ranges::stable_sort(found, [](const auto& lhs, const auto& rhs) {
    return lhs.relation == Relation::Inside && rhs.relation != Relation::Inside;
  });

  auto containing = Json::array();
  auto owners = Json::array();
  auto ownerNodes = std::vector<const mdl::Node*>{};
  auto insideSolid = false;
  for (const auto& [node, relation] : found)
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
        brushNode && relation == Relation::Inside
        && classifyBrush(*brushNode) != BrushClass::Trigger)
    {
      insideSolid = true;
    }
    if (containing.size() < limit)
    {
      auto summary = nodeSummary(*node, ids);
      summary["relation"] = relation == Relation::Inside ? "inside" : "touching";
      containing.push_back(std::move(summary));

      for (const auto* parent = node->parent(); parent; parent = parent->parent())
      {
        if (
          (isContainer(*parent) || dynamic_cast<const mdl::GroupNode*>(parent))
          && std::ranges::find(ownerNodes, parent) == ownerNodes.end())
        {
          ownerNodes.push_back(parent);
          owners.push_back(compactJson(*parent, ids));
        }
      }
    }
  }

  auto result = Json{
    {"point", toJson(point)},
    {"containing", std::move(containing)},
    {"owners", std::move(owners)},
    {"insideSolid", insideSolid},
    {"count", found.size()},
  };
  if (found.size() > limit)
  {
    result["truncated"] = true;
  }
  return result;
}

// ray_pick

ToolResult rayPick(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto filter = NodeFilter{map.editorContext(), args.get<bool>("includeHidden")};

  auto ignored = resolveIds(context, args, "ignore");
  if (ignored.is_error())
  {
    return errorOf(ignored);
  }
  filter.ignored = std::move(ignored.value());

  auto origin = args.getOptional<vm::vec3d>("origin");
  const auto from = args.getOptional<std::string>("from");
  if (origin.has_value() == from.has_value())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either origin or from.",
      "Example: {\"from\": \"entity:12\"} or {\"origin\": [0, 0, 64], \"direction\": "
      "[0, 0, -1]}");
  }
  if (from)
  {
    auto fromNode = context.ids().resolve(*from);
    if (fromNode.is_error())
    {
      return errorOf(fromNode);
    }
    origin = fromNode.value()->logicalBounds().center();
    filter.ignored.push_back(fromNode.value());
  }

  const auto direction = args.get<vm::vec3d>("direction");
  if (vm::is_zero(direction, vm::Cd::almost_zero()))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The direction must not be zero.",
      "Pass a direction such as [0, 0, -1].");
  }

  const auto kinds = args.getOr<std::vector<std::string>>("kinds", {});
  const auto accept = [&](const mdl::Node& node) {
    return filter.accepts(node)
           && (kinds.empty() || std::ranges::find(kinds, toString(objectKindOf(node))) != kinds.end());
  };

  const auto ray = vm::ray3d{*origin, vm::normalize(direction)};
  const auto hits = castRay(map, ray, accept, args.getOptional<double>("maxDistance"));

  auto result = Json{
    {"origin", toJson(ray.origin)},
    {"direction", toJson(ray.direction)},
    {"hit", hits.empty() ? Json(nullptr) : hitJson(context, hits.front())},
  };
  if (args.get<bool>("all"))
  {
    const auto limit = size_t(args.get<int64_t>("limit"));
    auto hitsJson = Json::array();
    for (size_t i = 0; i < hits.size() && i < limit; ++i)
    {
      hitsJson.push_back(hitJson(context, hits[i]));
    }
    result["hits"] = std::move(hitsJson);
    if (hits.size() > limit)
    {
      result["truncated"] = true;
    }
  }
  return result;
}

// space_check

std::optional<RayHit> findSurface(
  mdl::Map& map,
  const vm::bbox3d& box,
  const double inset,
  const bool downwards,
  const std::function<bool(const mdl::Node&)>& accept,
  size_t& supportedCorners)
{
  const auto z = downwards ? box.min.z() + inset : box.max.z() - inset;
  const auto boxZ = downwards ? box.min.z() : box.max.z();
  const auto direction = vm::vec3d{0, 0, downwards ? -1.0 : 1.0};
  const auto samples = std::array{
    vm::vec3d{box.center().x(), box.center().y(), z},
    vm::vec3d{box.min.x() + inset, box.min.y() + inset, z},
    vm::vec3d{box.max.x() - inset, box.min.y() + inset, z},
    vm::vec3d{box.min.x() + inset, box.max.y() - inset, z},
    vm::vec3d{box.max.x() - inset, box.max.y() - inset, z},
  };

  auto best = std::optional<RayHit>{};
  supportedCorners = 0;
  for (size_t i = 0; i < samples.size(); ++i)
  {
    const auto hits = castRay(map, vm::ray3d{samples[i], direction}, accept);
    if (hits.empty())
    {
      continue;
    }
    const auto& hit = hits.front();
    if (i > 0 && std::abs(hit.point.z() - boxZ) <= SupportTolerance)
    {
      ++supportedCorners;
    }
    if (!best || hit.distance < best->distance)
    {
      best = hit;
    }
  }
  return best;
}

Json surfaceJson(
  CallContext& context,
  const std::optional<RayHit>& hit,
  const double boxZ,
  const size_t supportedCorners)
{
  if (!hit)
  {
    return nullptr;
  }
  auto result = hitJson(context, *hit);
  result.erase("distance");
  result.erase("normal");
  result["z"] = roundForOutput(hit->point.z());
  result["distance"] = roundForOutput(std::abs(boxZ - hit->point.z()));
  result["supportedCorners"] = supportedCorners;
  return result;
}

ToolResult spaceCheck(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto box = args.get<vm::bbox3d>("box");
  const auto includeEntities = args.get<bool>("includeEntities");
  const auto solidOnly = args.get<bool>("solidOnly");
  const auto limit = size_t(args.get<int64_t>("limit"));

  for (size_t i = 0; i < 3; ++i)
  {
    if (box.min[i] >= box.max[i])
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The box must have a positive size in every dimension.",
        "Example: {\"box\": {\"min\": [0, 0, 0], \"max\": [32, 32, 56]}}");
    }
  }

  auto filter = NodeFilter{map.editorContext(), args.get<bool>("includeHidden")};
  auto ignored = resolveIds(context, args, "ignore");
  if (ignored.is_error())
  {
    return errorOf(ignored);
  }
  filter.ignored = std::move(ignored.value());

  const auto countsAsSolid = [&](const mdl::Node& node) {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    return !solidOnly || !brushNode || classifyBrush(*brushNode) != BrushClass::Trigger;
  };

  // overlaps: interiors must intersect, touching surfaces do not count
  const auto inner = shrink(box, Epsilon);
  auto overlaps = Json::array();
  auto overlapCount = size_t(0);
  for (auto* node : map.worldNode().nodeTree().find_intersectors(inner))
  {
    if (!filter.accepts(*node) || !countsAsSolid(*node))
    {
      continue;
    }

    auto overlapping = false;
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node))
    {
      overlapping = intersectsInterior(brushNode->brush(), inner);
    }
    else if (isPointEntity(*node))
    {
      overlapping = includeEntities && node->logicalBounds().intersects(inner);
    }
    else
    {
      overlapping = node->logicalBounds().intersects(inner);
    }

    if (overlapping)
    {
      ++overlapCount;
      if (overlaps.size() < limit)
      {
        auto summary = nodeSummary(*node, ids);
        summary["overlap"] = toJson(intersection(node->logicalBounds(), box));
        overlaps.push_back(std::move(summary));
      }
    }
  }

  // floor and ceiling: brushes and patches only
  const auto acceptSurface = [&](const mdl::Node& node) {
    return filter.accepts(node) && !isPointEntity(node) && countsAsSolid(node);
  };
  const auto inset = std::min(
    {Epsilon,
     (box.max.x() - box.min.x()) / 4.0,
     (box.max.y() - box.min.y()) / 4.0,
     (box.max.z() - box.min.z()) / 4.0});

  auto floorCorners = size_t(0);
  const auto floor = findSurface(map, box, inset, true, acceptSurface, floorCorners);
  auto ceilingCorners = size_t(0);
  const auto ceiling = findSurface(map, box, inset, false, acceptSurface, ceilingCorners);

  auto result = Json{
    {"box", toJson(box)},
    {"free", overlapCount == 0},
    {"overlaps", std::move(overlaps)},
    {"overlapCount", overlapCount},
    {"floor", surfaceJson(context, floor, box.min.z(), floorCorners)},
    {"ceiling", surfaceJson(context, ceiling, box.max.z(), ceilingCorners)},
    {"clearance",
     floor && ceiling ? Json(roundForOutput(ceiling->point.z() - floor->point.z()))
                      : Json(nullptr)},
    {"insideWorldBounds", map.worldBounds().contains(box)},
  };
  if (overlapCount > limit)
  {
    result["truncated"] = true;
  }
  return result;
}

// map_plan_view

std::string formatCoordinate(const double value)
{
  const auto rounded = roundForOutput(value);
  return rounded == std::floor(rounded) ? std::to_string(int64_t(rounded))
                                        : fmt::format("{}", rounded);
}

char entityChar(const std::string& classname)
{
  if (classname.starts_with("info_player_"))
  {
    return 'P';
  }
  if (classname.starts_with("monster_"))
  {
    return 'M';
  }
  if (classname.starts_with("light"))
  {
    return 'L';
  }
  if (classname.starts_with("item_") || classname.starts_with("weapon_"))
  {
    return 'I';
  }
  return 'E';
}

int entityPriority(const char c)
{
  switch (c)
  {
  case 'P':
    return 5;
  case 'M':
    return 4;
  case 'I':
    return 3;
  case 'E':
    return 2;
  case 'L':
    return 1;
  default:
    return 0;
  }
}

std::string_view legendText(const char c)
{
  switch (c)
  {
  case '#':
    return "solid brush";
  case '+':
    return "brush entity (door, platform, ...)";
  case 't':
    return "trigger brush";
  case '.':
    return "open, floor below";
  case ' ':
    return "open, no floor below (void or outside)";
  case 'P':
    return "player start (info_player_*)";
  case 'M':
    return "monster (monster_*)";
  case 'L':
    return "light (light*)";
  case 'I':
    return "item or weapon (item_*, weapon_*)";
  case 'E':
    return "other point entity";
  default:
    return "";
  }
}

template <typename F>
void visitPlanNodes(const mdl::Node& node, const NodeFilter& filter, const F& f)
{
  for (const auto* child : node.children())
  {
    if (
      dynamic_cast<const mdl::BrushNode*>(child)
      || dynamic_cast<const mdl::PatchNode*>(child) || isPointEntity(*child))
    {
      if (filter.visible(*child))
      {
        f(*child);
      }
    }
    else
    {
      visitPlanNodes(*child, filter, f);
    }
  }
}

ToolResult mapPlanView(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto filter = NodeFilter{map.editorContext(), args.get<bool>("includeHidden")};
  const auto showEntities = args.get<bool>("showEntities");
  const auto floorDepth = args.get<double>("floorDepth");
  const auto maxEntities = size_t(args.get<int64_t>("maxEntities"));

  auto region = args.getOptional<vm::bbox3d>("region");
  if (!region)
  {
    visitPlanNodes(map.worldNode(), filter, [&](const mdl::Node& node) {
      region = region ? vm::merge(*region, node.logicalBounds()) : node.logicalBounds();
    });
    if (!region)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "There are no visible objects to draw.",
        "Pass a region, or includeHidden: true.");
    }
  }
  const auto height = args.getOr<double>("height", region->min.z() + 48.0);

  const auto countCells = [&](const double cellSize) {
    const auto originX = std::floor(region->min.x() / cellSize) * cellSize;
    const auto originY = std::floor(region->min.y() / cellSize) * cellSize;
    const auto columns =
      std::max(size_t(1), size_t(std::ceil((region->max.x() - originX) / cellSize)));
    const auto rows =
      std::max(size_t(1), size_t(std::ceil((region->max.y() - originY) / cellSize)));
    return std::tuple{originX, originY, columns, rows};
  };

  auto cellSize = args.getOptional<double>("cellSize");
  if (cellSize)
  {
    const auto [originX, originY, columns, rows] = countCells(*cellSize);
    if (columns > MaxPlanCells || rows > MaxPlanCells)
    {
      const auto extent =
        std::max(region->max.x() - region->min.x(), region->max.y() - region->min.y());
      return makeError(
        ErrorCode::InvalidArgument,
        "The plan view would have " + std::to_string(columns) + " x "
          + std::to_string(rows) + " cells; at most " + std::to_string(MaxPlanCells)
          + " x " + std::to_string(MaxPlanCells) + " are allowed.",
        "Pass a larger cellSize (at least "
          + std::to_string(int(std::ceil(extent / double(MaxPlanCells - 1))))
          + ") or a smaller region, or omit cellSize.");
    }
  }
  else
  {
    cellSize = 8.0;
    while (true)
    {
      const auto [originX, originY, columns, rows] = countCells(*cellSize);
      if ((columns <= 80 && rows <= 60) || *cellSize >= 65536.0)
      {
        break;
      }
      *cellSize *= 2.0;
    }
  }

  const auto [originX, originY, columns, rows] = countCells(*cellSize);
  const auto topY = originY + double(rows) * *cellSize;
  const auto cellInset = std::min(Epsilon, *cellSize / 4.0);

  auto grid = std::vector<std::string>(rows, std::string(columns, ' '));
  const auto& nodeTree = map.worldNode().nodeTree();
  const auto acceptFloor = [&](const mdl::Node& node) {
    if (!filter.accepts(node) || isPointEntity(node))
    {
      return false;
    }
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node);
    return !brushNode || classifyBrush(*brushNode) != BrushClass::Trigger;
  };

  for (size_t row = 0; row < rows; ++row)
  {
    const auto y1 = topY - double(row) * *cellSize;
    const auto y0 = y1 - *cellSize;
    for (size_t column = 0; column < columns; ++column)
    {
      const auto x0 = originX + double(column) * *cellSize;
      const auto x1 = x0 + *cellSize;

      // the cell's area at the given height, slightly inset so that brushes that only
      // touch the cell's border do not count
      const auto slab = vm::bbox3d{
        {x0 + cellInset, y0 + cellInset, height},
        {x1 - cellInset, y1 - cellInset, height}};

      auto best = std::optional<BrushClass>{};
      for (const auto* node : nodeTree.find_intersectors(slab))
      {
        const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
        if (!brushNode || !filter.accepts(*node))
        {
          continue;
        }
        const auto brushClass = classifyBrush(*brushNode);
        if (best && int(*best) <= int(brushClass))
        {
          continue;
        }
        if (intersectsInterior(brushNode->brush(), slab))
        {
          best = brushClass;
        }
      }

      auto c = ' ';
      if (best)
      {
        c = *best == BrushClass::Solid ? '#' : *best == BrushClass::Entity ? '+' : 't';
      }
      else
      {
        const auto center = vm::vec3d{(x0 + x1) / 2.0, (y0 + y1) / 2.0, height};
        const auto hits =
          castRay(map, vm::ray3d{center, vm::vec3d{0, 0, -1}}, acceptFloor, floorDepth);
        c = hits.empty() ? ' ' : '.';
      }
      grid[row][column] = c;
    }
  }

  auto entities = Json::array();
  auto entityCount = size_t(0);
  if (showEntities)
  {
    visitPlanNodes(map.worldNode(), filter, [&](const mdl::Node& node) {
      const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
      if (!entityNode)
      {
        return;
      }
      const auto& origin = entityNode->entity().origin();
      if (origin.x() < originX || origin.y() < originY)
      {
        return;
      }
      // the cell whose min corner is at or below the origin, like the labels
      const auto column = size_t((origin.x() - originX) / *cellSize);
      const auto rowFromSouth = size_t((origin.y() - originY) / *cellSize);
      if (column >= columns || rowFromSouth >= rows)
      {
        return;
      }
      const auto row = rows - 1 - rowFromSouth;

      const auto& classname = entityNode->entity().classname();
      const auto c = entityChar(classname);
      auto& cell = grid[row][column];
      if (entityPriority(c) > entityPriority(cell))
      {
        cell = c;
      }

      ++entityCount;
      if (entities.size() < maxEntities)
      {
        entities.push_back(Json{
          {"char", std::string(1, c)},
          {"id", ids.format(node)},
          {"classname", classname},
          {"cell", Json::array({column, row})},
          {"origin", toJson(origin)},
        });
      }
    });
  }

  // render: a header with x coordinates every 8 columns, a y label per row
  auto rowLabels = std::vector<std::string>{};
  auto labelWidth = size_t(0);
  for (size_t row = 0; row < rows; ++row)
  {
    rowLabels.push_back(formatCoordinate(topY - double(row + 1) * *cellSize));
    labelWidth = std::max(labelWidth, rowLabels.back().size());
  }

  const auto prefixWidth = labelWidth + 2;
  auto header = std::string(prefixWidth + columns, ' ');
  auto nextFree = size_t(0);
  for (size_t column = 0; column < columns; column += 8)
  {
    const auto label = formatCoordinate(originX + double(column) * *cellSize);
    const auto position = prefixWidth + column;
    if (position >= nextFree)
    {
      header.resize(std::max(header.size(), position + label.size()), ' ');
      header.replace(position, label.size(), label);
      nextFree = position + label.size() + 1;
    }
  }
  while (!header.empty() && header.back() == ' ')
  {
    header.pop_back();
  }

  auto text = header;
  for (size_t row = 0; row < rows; ++row)
  {
    text += "\n" + std::string(labelWidth - rowLabels[row].size(), ' ') + rowLabels[row]
            + " |" + grid[row];
  }

  auto legend = Json::object();
  for (const auto c : std::string_view{"#+t. PMLIE"})
  {
    if (std::ranges::any_of(
          grid, [&](const auto& line) { return line.find(c) != std::string::npos; }))
    {
      legend[std::string(1, c)] = legendText(c);
    }
  }

  auto result = Json{
    {"text", std::move(text)},
    {"legend", std::move(legend)},
    {"entities", std::move(entities)},
    {"origin", toJson(vm::vec2d{originX, originY})},
    {"cellSize", roundForOutput(*cellSize)},
    {"columns", columns},
    {"rows", rows},
    {"height", roundForOutput(height)},
    {"region", toJson(*region)},
  };
  if (entityCount > entities.size())
  {
    result["entitiesTruncated"] = true;
  }
  return result;
}

} // namespace

void registerSpatialTools(ToolRegistry& registry)
{
  const auto includeHiddenField =
    field("includeHidden", boolean().defaultsTo(false))
      .describe("Also consider hidden objects (default: only visible objects count)");
  const auto ignoreField =
    field("ignore", array(objectId()))
      .describe("Objects to ignore; ignoring an entity or group ignores its members");

  registry.add(
    ToolDef{"objects_at_point"}
      .title("Objects at Point")
      .description(
        "Lists the objects at a point: brushes whose convex volume contains it (exact), "
        "point entities and patches whose bounds contain it, each with relation "
        "'inside' or 'touching' (the point lies within tolerance of the surface), plus "
        "the brush entities and groups that own them. insideSolid tells whether the "
        "point is inside a non-trigger brush. Only visible objects count unless "
        "includeHidden is set. Example: {\"point\": [208, 208, 64]}")
      .input(object({
        field("point", vec3()).required().describe("The point to test"),
        field("tolerance", number().min(0).max(64).defaultsTo(0.001))
          .describe("Points within this distance of a surface count as touching"),
        field("limit", integer().min(1).max(500).defaultsTo(50))
          .describe("Maximum number of objects listed"),
        includeHiddenField,
      }))
      .output(object({
        field("point", vec3()),
        field("containing", array(any()))
          .describe(
            "Object summaries with relation 'inside' or 'touching', inside first"),
        field("owners", array(any()))
          .describe("Brush entities and groups owning the objects: {id, kind, label}"),
        field("insideSolid", boolean()),
        field("count", integer()).describe("Total number of objects found"),
        field("truncated", boolean()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(objectsAtPoint));

  registry.add(
    ToolDef{"ray_pick"}
      .title("Ray Pick")
      .description(
        "Casts a ray and reports what it hits, like clicking in the 3D view. Give origin "
        "+ direction (normalized for you, default straight down), or from: an object id "
        "whose bounds center is the origin; that object and its members are ignored, so "
        "{\"from\": \"entity:12\"} answers 'what is under this entity?'. Brush faces are "
        "only hit from the front: a ray starting inside a brush passes through it, and a "
        "ray starting inside a point entity's bounds does not hit that entity. Hidden "
        "objects are not hit unless includeHidden. Returns hit: null when nothing is "
        "hit. "
        "Example: {\"origin\": [256, 256, 128], \"direction\": [1, 0, 0], \"kinds\": "
        "[\"brush\"]}")
      .input(object({
        field("origin", vec3()).describe("Ray origin; alternative to from"),
        field("direction", vec3().defaultsTo(Json::array({0, 0, -1})))
          .describe("Ray direction, need not be normalized"),
        field(
          "from",
          objectId(
            {ObjectKind::Brush,
             ObjectKind::Entity,
             ObjectKind::Patch,
             ObjectKind::Group}))
          .describe("Start at the center of this object's bounds and ignore it"),
        field("maxDistance", number().min(0)).describe("Ignore hits farther away"),
        ignoreField,
        field("kinds", array(enumOf(nodeKindNames())))
          .describe("Only hit these kinds of objects"),
        includeHiddenField,
        field("all", boolean().defaultsTo(false))
          .describe("Also return all hits up to limit, nearest first"),
        field("limit", integer().min(1).max(100).defaultsTo(10))
          .describe("Maximum number of hits returned with all"),
      }))
      .output(object({
        field("origin", vec3()),
        field("direction", vec3()),
        field("hit", any())
          .describe(
            "{object, kind, label, face, material, normal, point, distance, entity, "
            "classname, group} (face, material and normal for brushes; entity and "
            "classname for brushes of brush entities) or null"),
        field("hits", array(any())).describe("With all: every hit, nearest first"),
        field("truncated", boolean()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(rayPick));

  registry.add(
    ToolDef{"space_check"}
      .title("Check Space")
      .description(
        "Checks whether a box is free, e.g. before placing an entity or a brush. Lists "
        "the objects whose interior overlaps the box (brushes exactly; point entities "
        "and "
        "patches by bounds; surfaces that only touch the box do not count), and finds "
        "the "
        "floor and ceiling by casting rays down from the box's bottom and up from its "
        "top "
        "(at the center and the four corners): the highest surface below and the lowest "
        "above, with supportedCorners = corners with a surface within 1 unit. With "
        "solidOnly, brushes of trigger_* entities are ignored. Example: {\"box\": "
        "{\"min\": [48, 48, 0], \"max\": [80, 80, 56]}}")
      .input(object({
        field("box", box()).required().describe("The space to check"),
        ignoreField,
        field("includeEntities", boolean().defaultsTo(true))
          .describe("Whether point entities count as obstacles"),
        field("solidOnly", boolean().defaultsTo(true))
          .describe("Ignore brushes of trigger_* entities"),
        includeHiddenField,
        field("limit", integer().min(1).max(200).defaultsTo(20))
          .describe("Maximum number of overlaps listed"),
      }))
      .output(object({
        field("box", box()),
        field("free", boolean()).describe("Whether nothing overlaps the box"),
        field("overlaps", array(any()))
          .describe("Object summaries plus overlap: the overlapping part of the bounds"),
        field("overlapCount", integer()),
        field("floor", any())
          .describe("{z, distance, object, kind, label, face, material, point, entity, "
                    "supportedCorners} or null"),
        field("ceiling", any()).describe("Like floor, above the box, or null"),
        field("clearance", any()).describe("ceiling z - floor z, or null"),
        field("insideWorldBounds", boolean()),
        field("truncated", boolean()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(spaceCheck));

  registry.add(
    ToolDef{"map_plan_view"}
      .title("Map Plan View")
      .description(
        "Draws a top-down text map of a horizontal slice at the given height. Each cell "
        "shows '#' if a solid brush overlaps the cell at that height, '+' for a brush "
        "entity (door, platform), 't' for a trigger, '.' for open space with a floor "
        "below (a brush or patch surface within floorDepth under the cell center), and ' "
        "' "
        "for open space with nothing below. Point entities are drawn at their origin: "
        "'P' "
        "player start, 'M' monster, 'L' light, 'I' item/weapon, 'E' other. North (+y) is "
        "up, x grows to the right; the header shows the min x of every 8th column, each "
        "row starts with its min y. The grid is aligned to multiples of cellSize. "
        "Defaults: region = bounds of all visible objects (only x and y are used), "
        "height "
        "= region min z + 48, cellSize = the smallest power of two >= 8 that fits 80 x "
        "60 "
        "cells. Example: {\"height\": 64, \"cellSize\": 32}")
      .input(object({
        field("region", box()).describe("Area to draw; only x and y are used"),
        field("height", number()).describe("Height (z) of the slice"),
        field("cellSize", number().min(1))
          .describe("Cell size in units; at most 200 x 200 cells"),
        field("showEntities", boolean().defaultsTo(true)).describe("Draw point entities"),
        field("floorDepth", number().min(1).defaultsTo(1024))
          .describe("How far below the slice a floor may be"),
        field("maxEntities", integer().min(0).max(1000).defaultsTo(100))
          .describe("Maximum number of entities listed"),
        includeHiddenField,
      }))
      .output(object({
        field("text", string()).describe("The plan, rows separated by newlines"),
        field("legend", any()).describe("{char: meaning} for the characters used"),
        field("entities", array(any()))
          .describe("{char, id, classname, cell: [column, row], origin}"),
        field("entitiesTruncated", boolean()),
        field("origin", vec2()).describe("x, y of the first cell's min corner"),
        field("cellSize", number()),
        field("columns", integer()),
        field("rows", integer()),
        field("height", number()),
        field("region", box()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(mapPlanView));
}

} // namespace tb::mcp
