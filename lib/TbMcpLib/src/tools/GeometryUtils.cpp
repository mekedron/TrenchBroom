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

#include "mcp/tools/GeometryUtils.h"

#include "NodeJson.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushGeometry.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Hit.h"
#include "mdl/Layer.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"

#include "vm/intersection.h"
#include "vm/scalar.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>

namespace tb::mcp
{
namespace
{

std::string lowercase(std::string str)
{
  std::ranges::transform(
    str, str.begin(), [](const unsigned char c) { return char(std::tolower(c)); });
  return str;
}

bool mentionsWorldBounds(const std::string& message)
{
  const auto lower = lowercase(message);
  return lower.find("world bounds") != std::string::npos
         || lower.find("worldbounds") != std::string::npos;
}

void collectBrushes(mdl::Node& node, std::vector<mdl::BrushNode*>& result)
{
  if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
  {
    result.push_back(brushNode);
  }
  for (auto* child : node.children())
  {
    collectBrushes(*child, result);
  }
}

bool isInteger(const double d)
{
  return std::abs(d - std::round(d)) < 0.001;
}

} // namespace

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

bool inOmittedLayer(const mdl::Node& node)
{
  for (const auto* current = &node; current; current = current->parent())
  {
    if (const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(current);
        layerNode && layerNode->layer().omitFromExport())
    {
      return true;
    }
  }
  return false;
}

std::vector<RayHit> castRay(
  mdl::Map& map,
  const vm::ray3d& ray,
  const std::function<bool(const mdl::Node&)>& accept,
  const std::optional<double> maxDistance)
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

mdl::BrushBuilder brushBuilder(const mdl::Map& map)
{
  const auto& faceAttribsConfig = map.gameInfo().gameConfig.faceAttribsConfig;
  return mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    faceAttribsConfig.defaultUvAttributes,
    faceAttribsConfig.defaultSurfaceAttributes};
}

std::string materialArgument(
  CallContext& context, const Args& args, const std::string_view key)
{
  const auto& map = context.map();
  if (const auto material = args.getOptional<std::string>(key))
  {
    if (!map.materialManager().material(*material))
    {
      context.warn(
        "UNKNOWN_MATERIAL",
        "Material '" + *material
          + "' is not loaded; the brush uses it anyway and shows as missing. Use "
            "materials_list to find available materials.");
    }
    return *material;
  }
  return map.currentMaterialName();
}

std::optional<ToolError> checkBox(
  const mdl::Map& map, const vm::bbox3d& box, const std::string_view what)
{
  const auto size = box.size();
  for (size_t i = 0; i < 3; ++i)
  {
    if (!(size[i] > 0.0))
    {
      static constexpr auto axes = "xyz";
      return makeError(
        ErrorCode::InvalidGeometry,
        std::string{what} + " is degenerate: its size along " + axes[i] + " is "
          + std::to_string(size[i]) + ".",
        "Make every component of max greater than the corresponding component of min.");
    }
  }

  if (!map.worldBounds().contains(box))
  {
    return makeError(
      ErrorCode::OutOfWorldBounds,
      std::string{what} + " " + toJson(box).dump() + " is not inside the world bounds "
        + toJson(map.worldBounds()).dump() + ".",
      "Move or shrink it so that it lies inside the world bounds.");
  }

  return std::nullopt;
}

std::optional<ToolError> checkInsideWorldBounds(
  const std::vector<mdl::Node*>& nodes,
  const mdl::Map& map,
  const IdRegistry& ids,
  std::string hint)
{
  const auto& worldBounds = map.worldBounds();
  auto offending = std::vector<std::string>{};
  for (const auto* node : nodes)
  {
    const auto& bounds = node->logicalBounds();
    bool inside = true;
    for (size_t i = 0; i < 3; ++i)
    {
      inside = inside && bounds.min[i] > worldBounds.min[i]
               && bounds.max[i] < worldBounds.max[i];
    }
    if (!inside)
    {
      offending.push_back(ids.format(*node));
    }
  }

  if (offending.empty())
  {
    return std::nullopt;
  }

  auto list = std::string{};
  for (const auto& id : offending)
  {
    list += (list.empty() ? "" : ", ") + id;
  }
  return makeError(
    ErrorCode::OutOfWorldBounds,
    "The result would reach or exceed the world bounds " + toJson(worldBounds).dump()
      + ": " + list + ".",
    hint.empty() ? "Use a smaller offset or keep the objects closer to the origin."
                 : std::move(hint),
    std::move(offending));
}

ToolError geometryError(
  std::string message,
  const std::string& mdlMessage,
  std::vector<std::string> objectIds,
  std::string hint)
{
  const auto code = mentionsWorldBounds(mdlMessage) ? ErrorCode::OutOfWorldBounds
                                                    : ErrorCode::InvalidGeometry;
  if (!mdlMessage.empty())
  {
    message += " Editor said: " + mdlMessage;
  }
  auto error = makeError(code, std::move(message), std::move(hint), std::move(objectIds));
  if (!mdlMessage.empty())
  {
    error.details["editorMessages"] = Json::array({mdlMessage});
  }
  return error;
}

ToolError geometryOperationFailed(
  const CallContext& context,
  std::string message,
  std::vector<std::string> objectIds,
  std::string hint)
{
  auto texts = std::vector<std::string>{};
  for (const auto& logMessage : context.loggedProblems())
  {
    texts.push_back(logMessage.text);
  }

  const auto code = std::ranges::any_of(texts, mentionsWorldBounds)
                      ? ErrorCode::OutOfWorldBounds
                      : ErrorCode::InvalidGeometry;
  if (!texts.empty())
  {
    message += " Editor said: " + texts.back();
  }
  auto error = makeError(code, std::move(message), std::move(hint), std::move(objectIds));
  if (!texts.empty())
  {
    error.details["editorMessages"] = texts;
  }
  return error;
}

std::vector<mdl::Node*> addBrushes(
  mdl::Map& map, std::vector<mdl::Brush> brushes, mdl::Node* parent)
{
  auto nodes = std::vector<mdl::Node*>{};
  nodes.reserve(brushes.size());
  for (auto& brush : brushes)
  {
    nodes.push_back(new mdl::BrushNode{std::move(brush)});
  }
  auto& parentNode = parent ? *parent : mdl::parentForNodes(map);
  return mdl::addNodes(map, {{&parentNode, std::move(nodes)}});
}

Json nodeSummaries(const std::vector<mdl::Node*>& nodes, const IdRegistry& ids)
{
  auto result = Json::array();
  for (const auto* node : nodes)
  {
    result.push_back(nodeSummary(*node, ids));
  }
  return result;
}

std::vector<std::string> formatIds(
  const std::vector<mdl::Node*>& nodes, const IdRegistry& ids)
{
  auto result = std::vector<std::string>{};
  result.reserve(nodes.size());
  for (const auto* node : nodes)
  {
    result.push_back(ids.format(*node));
  }
  return result;
}

void warnNonIntegerVertices(CallContext& context, const std::vector<mdl::Node*>& nodes)
{
  auto brushNodes = std::vector<mdl::BrushNode*>{};
  for (auto* node : nodes)
  {
    collectBrushes(*node, brushNodes);
  }

  auto offending = std::vector<std::string>{};
  for (const auto* brushNode : brushNodes)
  {
    const auto positions = brushNode->brush().vertexPositions();
    if (!std::ranges::all_of(positions, [](const auto& p) {
          return isInteger(p.x()) && isInteger(p.y()) && isInteger(p.z());
        }))
    {
      offending.push_back(context.ids().format(*brushNode));
    }
  }

  if (!offending.empty())
  {
    const auto count = offending.size();
    context.warn(
      "NON_INTEGER_VERTICES",
      std::to_string(count) + (count == 1 ? " brush has" : " brushes have")
        + " vertices with non-integer coordinates. They are rounded when the map is "
          "saved in a format with integer plane points; use vertices_snap to snap them "
          "to the grid or to integers.",
      std::move(offending));
  }
}

ScopedLockOverride::ScopedLockOverride(
  mdl::Map& map,
  const std::optional<bool> alignmentLock,
  const std::optional<bool> uvLock)
  : m_map{map}
  , m_previousAlignmentLock{map.editorContext().alignmentLock()}
  , m_previousUvLock{map.editorContext().uvLock()}
{
  if (alignmentLock)
  {
    m_map.editorContext().setAlignmentLock(*alignmentLock);
  }
  if (uvLock)
  {
    m_map.editorContext().setUvLock(*uvLock);
  }
}

ScopedLockOverride::~ScopedLockOverride()
{
  m_map.editorContext().setAlignmentLock(m_previousAlignmentLock);
  m_map.editorContext().setUvLock(m_previousUvLock);
}

} // namespace tb::mcp
