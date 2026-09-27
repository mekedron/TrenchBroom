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

#include "mcp/tools/BrushEditTools.h"

#include "NodeJson.h"
#include "base/Macros.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/CsgUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Node.h"
#include "mdl/NodeContents.h"
#include "mdl/Selection.h"
#include "mdl/SurfaceAttributes.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"

#include "kd/vector_utils.h"

#include "vm/bbox.h"
#include "vm/mat_ext.h"
#include "vm/plane.h"
#include "vm/polygon.h"
#include "vm/segment.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tb::mcp
{
namespace
{

using namespace schema;

/** Handle positions given by an agent are matched within this distance. */
constexpr double HandleEpsilon = 0.01;

std::string formatPosition(const vm::vec3d& position)
{
  return toJson(position).dump();
}

template <typename R>
std::string errorMessage(const R& result)
{
  return std::visit([](const auto& error) { return error.msg; }, result.error());
}

std::string joinStrings(const std::vector<std::string>& strings)
{
  auto result = std::string{};
  for (const auto& str : strings)
  {
    result += (result.empty() ? "" : ", ") + str;
  }
  return result;
}

std::vector<std::string> existingIds(
  const std::vector<std::string>& candidates, const IdRegistry& ids)
{
  auto result = std::vector<std::string>{};
  for (const auto& id : candidates)
  {
    if (ids.resolve(id).is_success())
    {
      result.push_back(id);
    }
  }
  return result;
}

void collectEditableBrushes(
  const mdl::EditorContext& editorContext,
  mdl::Node& node,
  std::vector<mdl::Node*>& result)
{
  if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
  {
    if (editorContext.selectable(*brushNode))
    {
      result.push_back(brushNode);
    }
  }
  for (auto* child : node.children())
  {
    collectEditableBrushes(editorContext, *child, result);
  }
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

std::vector<mdl::Node*> uniqueBrushNodes(const std::vector<mdl::BrushFaceHandle>& faces)
{
  auto result = std::vector<mdl::Node*>{};
  for (const auto& handle : faces)
  {
    if (std::ranges::find(result, handle.node()) == result.end())
    {
      result.push_back(handle.node());
    }
  }
  return result;
}

/**
 * Whether the face, moved by delta, lies strictly inside the world bounds. Brush geometry
 * is clipped by the world bounds, so a face moved beyond them would silently be cut off.
 */
bool movedFaceInsideWorldBounds(
  const vm::bbox3d& worldBounds, const mdl::BrushFace& face, const vm::vec3d& delta)
{
  return std::ranges::all_of(face.vertexPositions(), [&](const auto& position) {
    const auto moved = position + delta;
    for (size_t i = 0; i < 3; ++i)
    {
      if (!(moved[i] > worldBounds.min[i] && moved[i] < worldBounds.max[i]))
      {
        return false;
      }
    }
    return true;
  });
}

/** New brushes grouped by the parent they are added to. */
using BrushesByParent = std::vector<std::pair<mdl::Node*, std::vector<mdl::Brush>>>;

void addToParent(BrushesByParent& brushes, mdl::Node* parent, mdl::Brush brush)
{
  auto it = std::ranges::find_if(
    brushes, [&](const auto& entry) { return entry.first == parent; });
  if (it == brushes.end())
  {
    brushes.emplace_back(parent, std::vector<mdl::Brush>{});
    it = std::prev(brushes.end());
  }
  it->second.push_back(std::move(brush));
}

/** Adds the brushes to their parents; returns the added nodes, or nullopt on failure. */
std::optional<std::vector<mdl::Node*>> addToParents(
  mdl::Map& map, BrushesByParent brushes)
{
  auto result = std::vector<mdl::Node*>{};
  for (auto& [parent, parentBrushes] : brushes)
  {
    const auto count = parentBrushes.size();
    const auto nodes = addBrushes(map, std::move(parentBrushes), parent);
    if (nodes.size() != count)
    {
      return std::nullopt;
    }
    result.insert(result.end(), nodes.begin(), nodes.end());
  }
  return result;
}

std::vector<mdl::Node*> selectedNodes(const mdl::Map& map)
{
  return map.selection().nodes;
}

// Faces

/**
 * The faces named by the argument `key` (face ids), or the selected faces if it is
 * absent.
 */
Result<std::vector<mdl::BrushFaceHandle>, ToolError> resolveFaces(
  CallContext& context, const Args& args, const std::string_view key = "faces")
{
  auto& map = context.map();
  const auto& editorContext = map.editorContext();

  auto result = std::vector<mdl::BrushFaceHandle>{};
  if (const auto faceIds = args.getOptional<std::vector<std::string>>(key))
  {
    for (const auto& id : *faceIds)
    {
      auto handle = resolveFace(context, id);
      if (handle.is_error())
      {
        return errorOf(handle);
      }
      const auto& face = handle.value();
      if (!editorContext.selectable(*face.node(), face.face()))
      {
        return makeError(
          ErrorCode::ObjectNotEditable,
          "Face " + id
            + " cannot be edited: its brush is hidden, locked, or inside a closed group.",
          "Show or unlock its layer (layer_set_state), or open its group (group_open).",
          {id});
      }
      if (std::ranges::find(result, face) == result.end())
      {
        result.push_back(face);
      }
    }
    return result;
  }

  result = map.selection().brushFaces;
  if (result.empty())
  {
    return makeError(
      ErrorCode::NoSelection,
      "No " + std::string{key} + " were given and no faces are selected.",
      "Pass face ids such as 'brush:12/face:4' in '" + std::string{key}
        + "' (object_get lists the faces of a brush), or select faces with "
          "select_faces_of.");
  }
  return result;
}

std::string faceId(const IdRegistry& ids, const mdl::BrushFaceHandle& handle)
{
  return ids.formatFace(*handle.node(), handle.faceIndex());
}

// Vertex, edge and face handles given by position

enum class HandleKind
{
  Vertex,
  Edge,
  Face,
};

struct Handles
{
  HandleKind kind;
  /** One entry per handle: one position for a vertex, two for an edge, n for a face. */
  std::vector<std::vector<vm::vec3d>> positions;
};

std::string formatHandle(const std::vector<vm::vec3d>& positions)
{
  if (positions.size() == 1)
  {
    return formatPosition(positions.front());
  }
  auto result = Json::array();
  for (const auto& position : positions)
  {
    result.push_back(toJson(position));
  }
  return result.dump();
}

Result<Handles, ToolError> parseHandles(const Args& args)
{
  const auto count =
    size_t(args.has("vertices")) + size_t(args.has("edges")) + size_t(args.has("faces"));
  if (count != 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass exactly one of 'vertices', 'edges' or 'faces'.",
      "Example: {\"vertices\": [[0, 0, 64]], ...} or {\"edges\": [[[0, 0, 64], [64, 0, "
      "64]]], ...}");
  }

  auto result = Handles{};
  if (const auto vertices = args.getOptional<Json>("vertices"))
  {
    result.kind = HandleKind::Vertex;
    for (const auto& vertex : *vertices)
    {
      result.positions.push_back({*vec3FromJson(vertex)});
    }
  }
  else
  {
    const auto isEdges = args.has("edges");
    result.kind = isEdges ? HandleKind::Edge : HandleKind::Face;
    const auto handlesJson = args.get<Json>(isEdges ? "edges" : "faces");
    for (const auto& handle : handlesJson)
    {
      auto positions = std::vector<vm::vec3d>{};
      for (const auto& position : handle)
      {
        positions.push_back(*vec3FromJson(position));
      }
      result.positions.push_back(std::move(positions));
    }
  }
  return result;
}

bool isClose(const vm::vec3d& lhs, const vm::vec3d& rhs)
{
  return vm::is_equal(lhs, rhs, HandleEpsilon);
}

/** A handle of a brush, matched to the brush's exact positions. */
struct MatchedHandles
{
  mdl::BrushNode* brushNode;
  std::vector<vm::vec3d> vertices;
  std::vector<vm::segment3d> edges;
  std::vector<vm::polygon3d> faces;

  bool empty() const { return vertices.empty() && edges.empty() && faces.empty(); }

  /** The exact positions of all vertices that are part of the matched handles. */
  std::vector<vm::vec3d> vertexPositions() const
  {
    auto result = vertices;
    for (const auto& edge : edges)
    {
      result.push_back(edge.start());
      result.push_back(edge.end());
    }
    for (const auto& face : faces)
    {
      for (const auto& vertex : face.vertices())
      {
        result.push_back(vertex);
      }
    }
    std::ranges::sort(result);
    const auto [first, last] = std::ranges::unique(result);
    result.erase(first, last);
    return result;
  }
};

/** Matches a handle to the given brush; returns false if the brush does not have it. */
bool matchHandle(
  const mdl::Brush& brush,
  const HandleKind kind,
  const std::vector<vm::vec3d>& positions,
  MatchedHandles& matched)
{
  switch (kind)
  {
  case HandleKind::Vertex:
    if (brush.hasVertex(positions.front(), HandleEpsilon))
    {
      matched.vertices.push_back(brush.findClosestVertexPosition(positions.front()));
      return true;
    }
    return false;
  case HandleKind::Edge:
    for (const auto* edge : brush.edges())
    {
      const auto& first = edge->firstVertex()->position();
      const auto& second = edge->secondVertex()->position();
      if (
        (isClose(first, positions[0]) && isClose(second, positions[1]))
        || (isClose(first, positions[1]) && isClose(second, positions[0])))
      {
        matched.edges.emplace_back(first, second);
        return true;
      }
    }
    return false;
  case HandleKind::Face:
    for (const auto& face : brush.faces())
    {
      const auto faceVertices = face.vertexPositions();
      if (
        faceVertices.size() == positions.size()
        && std::ranges::all_of(positions, [&](const auto& position) {
             return std::ranges::any_of(faceVertices, [&](const auto& vertex) {
               return isClose(vertex, position);
             });
           }))
      {
        matched.faces.push_back(face.polygon());
        return true;
      }
    }
    return false;
    switchDefault();
  }
}

std::vector<mdl::Node*> brushNodesOf(const std::vector<MatchedHandles>& matched)
{
  auto result = std::vector<mdl::Node*>{};
  for (const auto& m : matched)
  {
    result.push_back(m.brushNode);
  }
  return result;
}

std::vector<std::string> brushIdsOf(
  const std::vector<MatchedHandles>& matched, const IdRegistry& ids)
{
  return formatIds(brushNodesOf(matched), ids);
}

/** Handles matched to candidate brushes. */
struct HandleMatch
{
  /** The candidates that have at least one handle. */
  std::vector<MatchedHandles> matched;
  /** The indices of the handles that no candidate has. */
  std::vector<size_t> unknown;
};

HandleMatch matchHandles(
  const Handles& handles, const std::vector<mdl::Node*>& candidates)
{
  auto result = HandleMatch{};
  auto found = std::vector<bool>(handles.positions.size(), false);

  for (auto* node : candidates)
  {
    auto* brushNode = static_cast<mdl::BrushNode*>(node);
    auto matched = MatchedHandles{brushNode, {}, {}, {}};
    for (size_t i = 0; i < handles.positions.size(); ++i)
    {
      if (matchHandle(brushNode->brush(), handles.kind, handles.positions[i], matched))
      {
        found[i] = true;
      }
    }
    if (!matched.empty())
    {
      result.matched.push_back(std::move(matched));
    }
  }

  for (size_t i = 0; i < found.size(); ++i)
  {
    if (!found[i])
    {
      result.unknown.push_back(i);
    }
  }
  return result;
}

std::string brushCount(const size_t count)
{
  return std::to_string(count) + (count == 1 ? " brush" : " brushes");
}

/**
 * The error for handles that were not found in `searched` (a description of the searched
 * brushes). It names the brushes of the map that have the handles, so that the agent can
 * fix the call.
 */
ToolError handlesNotFound(
  CallContext& context,
  const Handles& handles,
  const std::vector<size_t>& unknown,
  const std::string& searched,
  const bool explicitIds)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto& editorContext = map.editorContext();

  auto allBrushes = std::vector<mdl::BrushNode*>{};
  collectBrushes(map.worldNode(), allBrushes);

  auto positions = std::vector<std::string>{};
  auto editableOwners = std::vector<std::string>{};
  auto otherOwners = std::vector<std::string>{};
  for (const auto i : unknown)
  {
    positions.push_back(formatHandle(handles.positions[i]));
    for (auto* brushNode : allBrushes)
    {
      auto matched = MatchedHandles{brushNode, {}, {}, {}};
      if (matchHandle(brushNode->brush(), handles.kind, handles.positions[i], matched))
      {
        auto& owners =
          editorContext.selectable(*brushNode) ? editableOwners : otherOwners;
        const auto id = ids.format(*brushNode);
        if (std::ranges::find(owners, id) == owners.end())
        {
          owners.push_back(id);
        }
      }
    }
  }

  static constexpr const char* kindNames[] = {"vertex", "edge", "face"};
  const auto message = std::string{"No "} + kindNames[size_t(handles.kind)] + " at "
                       + joinStrings(positions) + " was found in " + searched + ".";

  auto hint = std::string{};
  if (!editableOwners.empty())
  {
    hint += "Brushes " + joinStrings(editableOwners) + " have "
            + (unknown.size() == 1 ? "it" : "them") + ". ";
    if (explicitIds)
    {
      hint +=
        "Pass their ids in 'ids', or omit 'ids' to use the selected brushes or else "
        "every brush that has the handles. ";
    }
  }
  if (!otherOwners.empty())
  {
    hint += "Brushes " + joinStrings(otherOwners)
            + " have them but are hidden, locked, or inside a closed group: show or "
              "unlock their layer (layer_set_state), or open their group (group_open). ";
  }
  if (editableOwners.empty() && otherOwners.empty())
  {
    hint +=
      "No brush in the map has them. Use object_get with detail 'full' to list the "
      "vertices of a brush; positions are matched within 0.01 units. Edges are given by "
      "their two end points, faces by all their vertices (in any order).";
  }

  auto objectIds = editableOwners;
  objectIds.insert(objectIds.end(), otherOwners.begin(), otherOwners.end());
  const auto code = editableOwners.empty() && !otherOwners.empty()
                      ? ErrorCode::ObjectNotEditable
                      : ErrorCode::InvalidArgument;
  if (hint.ends_with(' '))
  {
    hint.pop_back();
  }
  return makeError(code, message, hint, std::move(objectIds));
}

/**
 * Finds the brushes that have the handles:
 * - with explicit `ids`, only these brushes, and each handle must belong to one of them;
 * - else, if the selected brushes have every handle, the selected brushes (a handle
 * shared with an unselected brush only changes the selected one, as in the editor);
 * - else every visible, unlocked brush that has one of the handles (warning
 *   HANDLES_OUTSIDE_SELECTION if brushes are selected).
 * Handles that are not found give an error that says where they were searched and which
 * brushes have them. Brushes without any handle are dropped.
 */
Result<std::vector<MatchedHandles>, ToolError> resolveHandles(
  CallContext& context, const Args& args, const Handles& handles)
{
  auto& map = context.map();
  const auto& ids = context.ids();

  if (args.has("ids"))
  {
    auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    auto match = matchHandles(handles, targets.value());
    if (!match.unknown.empty())
    {
      return handlesNotFound(
        context,
        handles,
        match.unknown,
        "the " + brushCount(targets.value().size()) + " given in 'ids' ("
          + joinStrings(formatIds(targets.value(), ids)) + ")",
        true);
    }
    return std::move(match.matched);
  }

  const auto& selectedBrushes = map.selection().brushes;
  const auto selected =
    std::vector<mdl::Node*>{selectedBrushes.begin(), selectedBrushes.end()};
  if (!selected.empty())
  {
    auto match = matchHandles(handles, selected);
    if (match.unknown.empty())
    {
      return std::move(match.matched);
    }
  }

  auto editableBrushes = std::vector<mdl::Node*>{};
  collectEditableBrushes(map.editorContext(), map.worldNode(), editableBrushes);
  auto match = matchHandles(handles, editableBrushes);
  if (!match.unknown.empty())
  {
    const auto count = editableBrushes.size();
    const auto whole = "the " + std::to_string(count) + " visible, unlocked "
                       + (count == 1 ? "brush" : "brushes") + " of the whole map";
    return handlesNotFound(
      context,
      handles,
      match.unknown,
      selected.empty()
        ? whole
        : "neither the selection (" + brushCount(selected.size()) + ") nor " + whole,
      false);
  }

  if (!selected.empty())
  {
    context.warn(
      "HANDLES_OUTSIDE_SELECTION",
      "The " + brushCount(selected.size())
        + " selected do not have all of the handles, so every visible, unlocked brush "
          "that has one of them was changed. Pass 'ids' to choose the brushes.",
      brushIdsOf(match.matched, ids));
  }
  return std::move(match.matched);
}


// brush_clip

void copyClosestFaceAttributes(const mdl::Brush& brush, mdl::BrushFace& clipFace)
{
  const auto& faces = brush.faces();
  const auto best = std::ranges::min_element(faces, {}, [&](const auto& face) {
    return vm::squared_length(face.boundary().normal - clipFace.boundary().normal);
  });
  if (best != faces.end())
  {
    clipFace.copyAttributes(*best);
  }
}

ToolResult brushClip(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  if (args.has("points") == args.has("face"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass exactly one of 'points' and 'face' to define the clip plane.",
      "Example: {\"points\": [[0, 0, 0], [64, 64, 0]]} or {\"face\": "
      "\"brush:12/face:3\"}");
  }

  auto points = std::vector<vm::vec3d>{};
  auto normal = vm::vec3d{};
  if (const auto pointsJson = args.getOptional<Json>("points"))
  {
    for (const auto& point : *pointsJson)
    {
      points.push_back(*vec3FromJson(point));
    }
    if (points.size() == 2)
    {
      const auto axis = args.getOr<std::string>("axis", "z");
      const auto axisVector = axis == "x"   ? vm::vec3d{1, 0, 0}
                              : axis == "y" ? vm::vec3d{0, 1, 0}
                                            : vm::vec3d{0, 0, 1};
      points.push_back(points[1] + 128.0 * axisVector);
    }
    else if (args.has("axis"))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "'axis' only applies when the plane is given by 2 points.",
        "Remove 'axis', or pass only 2 points.");
    }

    if (vm::is_colinear(points[0], points[1], points[2]))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The points do not define a plane: they coincide or lie on one line (with 2 "
        "points: the line is parallel to 'axis').",
        "Pass 3 points that are not on one line, or 2 distinct points whose line is not "
        "parallel to 'axis'.");
    }
    for (const auto& point : points)
    {
      if (!worldBounds.contains(point))
      {
        return makeError(
          ErrorCode::OutOfWorldBounds,
          "Clip point " + formatPosition(point) + " is outside the world bounds "
            + toJson(worldBounds).dump() + ".",
          "Pass points inside the world bounds; any points on the desired plane work.");
      }
    }
    normal = vm::normalize(vm::cross(points[1] - points[0], points[2] - points[0]));
  }
  else
  {
    auto handle = resolveFace(context, args.get<std::string>("face"));
    if (handle.is_error())
    {
      return errorOf(handle);
    }
    const auto& face = handle.value().face();
    points = {face.points()[0], face.points()[1], face.points()[2]};
    normal = face.normal();
  }

  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto clipFace = mdl::BrushFace::create(
    points[0],
    points[1],
    points[2],
    map.currentMaterialName(),
    mdl::UvAttributes{},
    mdl::SurfaceAttributes{},
    map.worldNode().mapFormat());
  if (clipFace.is_error())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The clip plane is invalid: " + errorMessage(clipFace),
      "Pass points that span a plane.");
  }
  // Brush::clip keeps the part behind the clip face, so this face keeps the back part.
  auto backFace = clipFace.value();
  if (vm::dot(backFace.normal(), normal) < 0.0)
  {
    backFace.invert();
  }
  auto frontFace = backFace;
  frontFace.invert();

  const auto keep = args.get<std::string>("keep");
  const auto keepFront = keep != "back";
  const auto keepBack = keep != "front";
  const auto plane = vm::plane3d{points[0], normal};
  const auto epsilon = vm::constants<double>::point_status_epsilon();

  auto unchanged = std::vector<mdl::Node*>{};
  auto toRemove = std::vector<mdl::Node*>{};
  auto toAdd = BrushesByParent{};
  auto discarded = std::vector<std::string>{};
  auto split = std::vector<std::string>{};

  for (auto* node : targets.value())
  {
    auto* brushNode = static_cast<mdl::BrushNode*>(node);
    const auto& brush = brushNode->brush();

    bool hasFront = false;
    bool hasBack = false;
    for (const auto& position : brush.vertexPositions())
    {
      const auto distance = plane.point_distance(position);
      hasFront = hasFront || distance > epsilon;
      hasBack = hasBack || distance < -epsilon;
    }

    if ((hasFront && !hasBack && keepFront) || (hasBack && !hasFront && keepBack))
    {
      unchanged.push_back(node);
      continue;
    }
    if (!hasFront || !hasBack)
    {
      discarded.push_back(ids.format(*node));
      toRemove.push_back(node);
      continue;
    }

    split.push_back(ids.format(*node));
    toRemove.push_back(node);
    for (const auto& [wanted, face] :
         {std::pair{keepFront, &frontFace}, std::pair{keepBack, &backFace}})
    {
      if (!wanted)
      {
        continue;
      }
      auto piece = brush;
      auto pieceFace = *face;
      copyClosestFaceAttributes(brush, pieceFace);
      if (const auto clipped = piece.clip(worldBounds, std::move(pieceFace));
          clipped.is_error())
      {
        return geometryError(
          "Clipping brush " + ids.format(*node) + " failed.",
          errorMessage(clipped),
          {ids.format(*node)},
          "Move the clip plane so that it does not cut off a sliver thinner than a "
          "unit.");
      }
      addToParent(toAdd, node->parent(), std::move(piece));
    }
  }

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      const auto added = addToParents(map, std::move(toAdd));
      if (!added)
      {
        return context.operationFailed("Could not add the clipped brushes.");
      }
      mdl::deselectAll(map);
      if (!toRemove.empty())
      {
        mdl::removeNodes(map, toRemove);
      }

      auto result = unchanged;
      result.insert(result.end(), added->begin(), added->end());
      if (!result.empty())
      {
        mdl::selectNodes(map, result);
      }

      if (split.empty() && discarded.empty())
      {
        context.warn(
          "NOTHING_CLIPPED",
          "The clip plane does not cut any target brush, and all of them lie on the "
          "kept side.");
      }

      return Json{
        {"brushes", nodeSummaries(result, ids)},
        {"split", split},
        {"discarded", discarded},
        {"unchanged", formatIds(unchanged, ids)},
        {"normal", toJson(normal)},
      };
    },
    SelectionAfter::Result);
}

// face_extrude

struct NormalGroup
{
  vm::vec3d normal;
  std::vector<mdl::BrushFaceHandle> faces;
};

std::vector<NormalGroup> groupByNormal(const std::vector<mdl::BrushFaceHandle>& faces)
{
  auto groups = std::vector<NormalGroup>{};
  for (const auto& handle : faces)
  {
    const auto& normal = handle.face().normal();
    auto it = std::ranges::find_if(groups, [&](const auto& group) {
      return vm::is_equal(group.normal, normal, vm::constants<double>::almost_zero());
    });
    if (it == groups.end())
    {
      groups.push_back({normal, {handle}});
    }
    else
    {
      it->faces.push_back(handle);
    }
  }
  return groups;
}

Result<double, ToolError> nonZeroDistance(const Args& args)
{
  const auto distance = args.get<double>("distance");
  if (distance == 0.0)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'distance' must not be 0.",
      "Pass a positive distance to extrude outward or a negative one to push the face "
      "inward.");
  }
  return distance;
}

ToolResult faceExtrude(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  const auto distance = nonZeroDistance(args);
  if (distance.is_error())
  {
    return errorOf(distance);
  }
  auto faces = resolveFaces(context, args);
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  const auto lockOverride =
    ScopedLockOverride{map, args.getOptional<bool>("alignmentLock")};
  const auto alignmentLock = map.editorContext().alignmentLock();

  // Faces are identified by brush and normal, since face indices change when a brush's
  // faces are re-sorted.
  const auto groups = groupByNormal(faces.value());
  auto extruded = std::vector<std::pair<mdl::BrushNode*, vm::vec3d>>{};
  for (const auto& handle : faces.value())
  {
    extruded.emplace_back(handle.node(), handle.face().normal());
  }
  const auto brushNodes = uniqueBrushNodes(faces.value());

  return withTargets(context, brushNodes, [&]() -> ToolResult {
    for (const auto& group : groups)
    {
      const auto delta = group.normal * distance.value();
      auto polygons = std::vector<vm::polygon3d>{};
      auto groupBrushes = std::vector<mdl::Node*>{};

      for (const auto& handle : group.faces)
      {
        auto* brushNode = handle.node();
        const auto& brush = brushNode->brush();
        const auto brushId = ids.format(*brushNode);
        const auto faceIndex = brush.findFace(group.normal);
        if (!faceIndex)
        {
          return makeError(
            ErrorCode::InvalidGeometry,
            "Brush " + brushId + " no longer has a face with normal "
              + formatPosition(group.normal)
              + " after extruding its other faces in this call.",
            "Extrude the faces in separate calls.",
            {brushId});
        }
        const auto id = ids.formatFace(*brushNode, *faceIndex);

        if (!movedFaceInsideWorldBounds(worldBounds, brush.face(*faceIndex), delta))
        {
          return makeError(
            ErrorCode::OutOfWorldBounds,
            "Extruding face " + id + " would move brush " + brushId
              + " out of the world bounds " + toJson(worldBounds).dump() + ".",
            "Use a smaller distance.",
            {id, brushId});
        }

        auto moved = brush;
        if (const auto result =
              moved.moveBoundary(worldBounds, *faceIndex, delta, alignmentLock);
            result.is_error())
        {
          return geometryError(
            "Extruding face " + id + " by " + std::to_string(distance.value())
              + " would make brush " + brushId + " invalid.",
            errorMessage(result),
            {id, brushId},
            "A negative distance must be smaller than the brush's thickness along the "
            "face normal; use a smaller distance, or delete the brush instead.");
        }
        if (!worldBounds.contains(moved.bounds()))
        {
          return makeError(
            ErrorCode::OutOfWorldBounds,
            "Extruding face " + id + " would move brush " + brushId
              + " out of the world bounds " + toJson(worldBounds).dump() + ".",
            "Use a smaller distance.",
            {id, brushId});
        }

        polygons.push_back(brush.face(*faceIndex).polygon());
        groupBrushes.push_back(brushNode);
      }

      mdl::deselectAll(map);
      mdl::selectNodes(map, groupBrushes);
      if (!mdl::extrudeBrushes(map, polygons, delta))
      {
        return geometryOperationFailed(
          context,
          "Could not extrude the faces with normal " + formatPosition(group.normal) + ".",
          formatIds(groupBrushes, ids),
          "Use a smaller distance.");
      }
    }

    auto faceResults = Json::array();
    for (const auto& [brushNode, normal] : extruded)
    {
      if (const auto faceIndex = brushNode->brush().findFace(normal))
      {
        faceResults.push_back(faceJson(map, *brushNode, *faceIndex, ids));
      }
    }
    warnNonIntegerVertices(context, brushNodes);
    return Json{
      {"faces", std::move(faceResults)},
      {"brushes", nodeSummaries(brushNodes, ids)},
    };
  });
}

// face_extrude_new

ToolResult faceExtrudeNew(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  const auto distance = nonZeroDistance(args);
  if (distance.is_error())
  {
    return errorOf(distance);
  }
  const auto mode = args.get<std::string>("mode");
  if (mode == "stamp" && distance.value() < 0.0)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "A stamp must be extruded outward (positive distance); inward it would overlap the "
      "original brush.",
      "Pass a positive distance, or use mode 'split' to split the brush.");
  }

  auto faces = resolveFaces(context, args);
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  const auto lockOverride =
    ScopedLockOverride{map, args.getOptional<bool>("alignmentLock")};
  const auto alignmentLock = map.editorContext().alignmentLock();
  const auto builder = brushBuilder(map);
  const auto inward = distance.value() < 0.0;

  auto toAdd = BrushesByParent{};
  auto toUpdate = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  auto updated = std::vector<mdl::Node*>{};

  for (const auto& handle : faces.value())
  {
    auto* brushNode = handle.node();
    const auto& brush = brushNode->brush();
    const auto& face = handle.face();
    const auto id = faceId(ids, handle);
    const auto brushId = ids.format(*brushNode);
    const auto delta = face.normal() * distance.value();

    const auto fail = [&](const std::string& mdlMessage) {
      return geometryError(
        "Extruding face " + id + " by " + std::to_string(distance.value())
          + " does not give a valid brush.",
        mdlMessage,
        {id, brushId},
        inward ? "The distance must be smaller than the brush's thickness along the face "
                 "normal; use a smaller distance."
               : "Use a smaller distance.");
    };

    if (!inward && !movedFaceInsideWorldBounds(worldBounds, face, delta))
    {
      return makeError(
        ErrorCode::OutOfWorldBounds,
        "Extruding face " + id + " by " + std::to_string(distance.value())
          + " would reach beyond the world bounds " + toJson(worldBounds).dump() + ".",
        "Use a smaller distance.",
        {id, brushId});
    }

    if (mode == "stamp")
    {
      auto points = face.vertexPositions();
      for (const auto& position : face.vertexPositions())
      {
        points.push_back(position + delta);
      }
      auto stamp = builder.createBrush(points, face.materialName());
      if (stamp.is_error())
      {
        return fail(errorMessage(stamp));
      }
      addToParent(toAdd, brushNode->parent(), stamp.value());
    }
    else if (!inward)
    {
      // The new brush spans from the face to its translated copy.
      auto newBrush = brush;
      auto clipFace = face;
      clipFace.invert();
      const auto result =
        newBrush.moveBoundary(worldBounds, handle.faceIndex(), delta, alignmentLock)
        | kdl::and_then(
          [&]() { return newBrush.clip(worldBounds, std::move(clipFace)); });
      if (result.is_error())
      {
        return fail(errorMessage(result));
      }
      addToParent(toAdd, brushNode->parent(), std::move(newBrush));
    }
    else
    {
      if (std::ranges::find(updated, brushNode) != updated.end())
      {
        return makeError(
          ErrorCode::InvalidArgument,
          "Brush " + brushId
            + " has several of the given faces; an inward split takes one face per "
              "brush.",
          "Split along one face per call.",
          {brushId});
      }

      // The original brush keeps the slab next to the face; the rest becomes a new
      // brush, as when ctrl-dragging a face inward in the editor.
      auto front = brush;
      auto back = brush;
      auto clipFace = face;
      if (const auto result =
            clipFace.transform(vm::translation_matrix(delta), alignmentLock);
          result.is_error())
      {
        return fail(errorMessage(result));
      }
      auto invertedClipFace = clipFace;
      invertedClipFace.invert();
      if (const auto result = front.clip(worldBounds, std::move(invertedClipFace));
          result.is_error())
      {
        return fail(errorMessage(result));
      }
      if (const auto result = back.clip(worldBounds, std::move(clipFace));
          result.is_error())
      {
        return fail(errorMessage(result));
      }
      toUpdate.emplace_back(brushNode, mdl::NodeContents{std::move(front)});
      updated.push_back(brushNode);
      addToParent(toAdd, brushNode->parent(), std::move(back));
    }
  }

  return withTargets(
    context,
    uniqueBrushNodes(faces.value()),
    [&]() -> ToolResult {
      if (
        !toUpdate.empty()
        && !mdl::updateNodeContents(map, "Resize Brushes", std::move(toUpdate)))
      {
        return context.operationFailed("Could not resize the split brushes.");
      }

      const auto added = addToParents(map, std::move(toAdd));
      if (!added)
      {
        return context.operationFailed("Could not add the new brushes.");
      }

      if (
        auto error = checkInsideWorldBounds(
          *added, context.map(), context.ids(), "Use a smaller distance."))
      {
        return std::move(*error);
      }

      auto toSelect = updated;
      toSelect.insert(toSelect.end(), added->begin(), added->end());
      mdl::deselectAll(map);
      mdl::selectNodes(map, toSelect);

      warnNonIntegerVertices(context, toSelect);
      return Json{
        {"brushes", nodeSummaries(*added, ids)},
        {"resized", formatIds(updated, ids)},
      };
    },
    SelectionAfter::Result);
}

// vertices_move

ToolResult verticesMove(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  const auto vector = args.get<vm::vec3d>("vector");
  if (vm::is_zero(vector, vm::constants<double>::almost_zero()))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'vector' must not be zero.",
      "Pass the offset to move the handles by, e.g. [0, 0, 16].");
  }

  auto handles = parseHandles(args);
  if (handles.is_error())
  {
    return errorOf(handles);
  }
  auto matched = resolveHandles(context, args, handles.value());
  if (matched.is_error())
  {
    return errorOf(matched);
  }

  const auto kind = handles.value().kind;
  const auto transform = vm::translation_matrix(vector);

  // Pre-check every brush so that the error names the offending ones.
  auto outOfBounds = std::vector<std::string>{};
  auto invalid = std::vector<std::string>{};
  auto allVertices = std::vector<vm::vec3d>{};
  auto allEdges = std::vector<vm::segment3d>{};
  auto allFaces = std::vector<vm::polygon3d>{};
  for (const auto& m : matched.value())
  {
    const auto& brush = m.brushNode->brush();
    const auto moving = m.vertexPositions();

    auto bounds = std::optional<vm::bbox3d>{};
    for (const auto& position : brush.vertexPositions())
    {
      const auto moved =
        std::ranges::binary_search(moving, position) ? position + vector : position;
      bounds = bounds ? vm::merge(*bounds, moved) : vm::bbox3d{moved, moved};
    }
    if (bounds && !worldBounds.contains(*bounds))
    {
      outOfBounds.push_back(ids.format(*m.brushNode));
      continue;
    }

    const auto canTransform =
      kind == HandleKind::Vertex
        ? brush.canTransformVertices(worldBounds, m.vertices, transform)
      : kind == HandleKind::Edge
        ? brush.canTransformEdges(worldBounds, m.edges, transform)
        : brush.canTransformFaces(worldBounds, m.faces, transform);
    if (!canTransform)
    {
      invalid.push_back(ids.format(*m.brushNode));
    }

    allVertices.insert(allVertices.end(), m.vertices.begin(), m.vertices.end());
    allEdges.insert(allEdges.end(), m.edges.begin(), m.edges.end());
    allFaces.insert(allFaces.end(), m.faces.begin(), m.faces.end());
  }

  if (!outOfBounds.empty())
  {
    return makeError(
      ErrorCode::OutOfWorldBounds,
      "Moving the handles by " + formatPosition(vector)
        + " would move vertices of brushes " + joinStrings(outOfBounds)
        + " out of the world bounds " + toJson(worldBounds).dump() + ".",
      "Use a smaller vector.",
      outOfBounds);
  }
  if (!invalid.empty())
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "Moving the handles by " + formatPosition(vector) + " would make brushes "
        + joinStrings(invalid)
        + " non-convex or degenerate (a moved vertex would pass through the rest of the "
          "brush, or the brush would collapse).",
      "Brushes must stay convex: use a smaller vector, move a whole edge or face "
      "instead of a single vertex, or move in several smaller steps. Pass 'ids' to move "
      "the handles of some brushes only.",
      invalid);
  }

  kdl::vec_sort_and_remove_duplicates(allVertices);
  kdl::vec_sort_and_remove_duplicates(allEdges);

  const auto lockOverride =
    ScopedLockOverride{map, std::nullopt, args.getOptional<bool>("uvLock")};
  const auto brushNodes = brushNodesOf(matched.value());

  return withTargets(context, brushNodes, [&]() -> ToolResult {
    auto hasRemainingVertices = true;
    auto success = true;
    switch (kind)
    {
    case HandleKind::Vertex: {
      const auto result = mdl::transformVertices(map, allVertices, transform);
      success = result.success;
      hasRemainingVertices = result.hasRemainingVertices;
      break;
    }
    case HandleKind::Edge:
      success = mdl::transformEdges(map, allEdges, transform);
      break;
    case HandleKind::Face:
      success = mdl::transformFaces(map, allFaces, transform);
      break;
      switchDefault();
    }

    if (!success)
    {
      return geometryOperationFailed(
        context,
        "Could not move the handles by " + formatPosition(vector) + ".",
        formatIds(brushNodes, ids),
        "Use a smaller vector, or move an edge or face instead of a single vertex.");
    }

    if (!hasRemainingVertices)
    {
      context.warn(
        "VERTICES_MERGED",
        "The moved vertices were merged into other vertices or removed; no vertex "
        "remains at the target positions.",
        formatIds(brushNodes, ids));
    }
    warnNonIntegerVertices(context, brushNodes);

    return Json{
      {"brushes", nodeSummaries(brushNodes, ids)},
      {"hasRemainingVertices", hasRemainingVertices},
    };
  });
}

// vertex_add

ToolResult vertexAdd(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  const auto brushId = args.get<std::string>("brush");
  const auto position = args.get<vm::vec3d>("position");

  auto target = resolveTargets(
    context, Args{Json{{"ids", Json::array({brushId})}}}, "ids", {ObjectKind::Brush});
  if (target.is_error())
  {
    return errorOf(target);
  }
  auto* brushNode = static_cast<mdl::BrushNode*>(target.value().front());
  const auto& brush = brushNode->brush();

  if (!worldBounds.contains(position))
  {
    return makeError(
      ErrorCode::OutOfWorldBounds,
      "Position " + formatPosition(position) + " is outside the world bounds "
        + toJson(worldBounds).dump() + ".",
      "Pass a position inside the world bounds.",
      {brushId});
  }
  if (brush.hasVertex(position, HandleEpsilon))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Brush " + brushId + " already has a vertex at " + formatPosition(position) + ".",
      "Use vertices_move to move an existing vertex.",
      {brushId});
  }
  if (!brush.canAddVertex(worldBounds, position))
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "Adding a vertex at " + formatPosition(position) + " to brush " + brushId
        + " has no effect: the position lies inside or on the surface of the brush.",
      "Pass a position outside the brush. The brush becomes the convex hull of its "
      "vertices and the new one; vertices inside the hull are removed.",
      {brushId});
  }

  return withTargets(context, target.value(), [&]() -> ToolResult {
    if (!mdl::addVertex(map, position))
    {
      return geometryOperationFailed(
        context,
        "Could not add a vertex at " + formatPosition(position) + " to brush " + brushId
          + ".",
        {brushId},
        "Pass a position outside the brush.");
    }
    warnNonIntegerVertices(context, target.value());
    return Json{
      {"brush", nodeSummary(*brushNode, ids)},
      {"vertexCount", brushNode->brush().vertexCount()},
    };
  });
}

// vertices_remove

ToolResult verticesRemove(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  auto handles = parseHandles(args);
  if (handles.is_error())
  {
    return errorOf(handles);
  }
  auto matched = resolveHandles(context, args, handles.value());
  if (matched.is_error())
  {
    return errorOf(matched);
  }

  auto invalid = std::vector<std::string>{};
  auto positions = std::vector<vm::vec3d>{};
  for (const auto& m : matched.value())
  {
    const auto vertices = m.vertexPositions();
    if (!m.brushNode->brush().canRemoveVertices(worldBounds, vertices))
    {
      invalid.push_back(ids.format(*m.brushNode));
    }
    positions.insert(positions.end(), vertices.begin(), vertices.end());
  }
  if (!invalid.empty())
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "Removing these handles would leave brushes " + joinStrings(invalid)
        + " degenerate (fewer than 4 vertices that span a volume).",
      "Remove fewer vertices, or delete the brushes with objects_delete.",
      invalid);
  }
  kdl::vec_sort_and_remove_duplicates(positions);

  static constexpr const char* commandNames[] = {
    "Remove Brush Vertices", "Remove Brush Edges", "Remove Brush Faces"};
  const auto brushNodes = brushNodesOf(matched.value());

  return withTargets(context, brushNodes, [&]() -> ToolResult {
    if (!mdl::removeVertices(
          map, commandNames[size_t(handles.value().kind)], std::move(positions)))
    {
      return geometryOperationFailed(
        context,
        "Could not remove the handles.",
        formatIds(brushNodes, ids),
        "Remove fewer vertices.");
    }
    warnNonIntegerVertices(context, brushNodes);
    return Json{{"brushes", nodeSummaries(brushNodes, ids)}};
  });
}

// vertices_snap

ToolResult verticesSnap(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto& worldBounds = map.worldBounds();

  if (args.has("mode") && args.has("snapTo"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either 'mode' or 'snapTo', not both.",
      "Example: {\"mode\": \"integer\"} or {\"snapTo\": 8}");
  }
  const auto mode = args.getOr<std::string>("mode", "grid");
  const auto snapTo = args.has("snapTo")  ? args.get<double>("snapTo")
                      : mode == "integer" ? 1.0
                                          : map.grid().actualSize();
  if (!(snapTo > 0.0))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'snapTo' must be positive.",
      "Pass the snap distance in map units, e.g. 1 or 8.");
  }

  auto targets = resolveTargets(
    context, args, "ids", {ObjectKind::Brush, ObjectKind::Entity, ObjectKind::Group});
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto brushNodes = std::vector<mdl::BrushNode*>{};
  for (auto* node : targets.value())
  {
    collectBrushes(*node, brushNodes);
  }
  if (brushNodes.empty())
  {
    return makeError(
      ErrorCode::WrongObjectKind,
      "The targets contain no brushes.",
      "Pass brushes, or groups or brush entities that contain brushes.",
      formatIds(targets.value(), ids));
  }

  auto snappable = std::vector<mdl::Node*>{};
  auto failed = std::vector<std::string>{};
  for (auto* brushNode : brushNodes)
  {
    if (brushNode->brush().canSnapVertices(worldBounds, snapTo))
    {
      snappable.push_back(brushNode);
    }
    else
    {
      failed.push_back(ids.format(*brushNode));
    }
  }
  if (snappable.empty())
  {
    return makeError(
      ErrorCode::InvalidGeometry,
      "Snapping to " + std::to_string(snapTo) + " would make every target brush ("
        + joinStrings(failed) + ") degenerate.",
      "Use a smaller snap distance, e.g. mode 'integer'.",
      failed);
  }

  const auto lockOverride =
    ScopedLockOverride{map, std::nullopt, args.getOptional<bool>("uvLock")};

  return withTargets(context, targets.value(), [&]() -> ToolResult {
    if (!mdl::snapVertices(map, snapTo))
    {
      return geometryOperationFailed(
        context,
        "Could not snap the vertices.",
        formatIds(snappable, ids),
        "Use a smaller snap distance.");
    }
    if (!failed.empty())
    {
      context.warn(
        "SNAP_FAILED",
        "The vertices of " + std::to_string(failed.size())
          + " brushes were not snapped, because the result would be degenerate. Use a "
            "smaller snap distance for them.",
        failed);
    }
    return Json{
      {"snapTo", snapTo},
      {"snapped", formatIds(snappable, ids)},
      {"failed", failed},
    };
  });
}

// CSG

ToolResult csgMerge(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  if (args.has("ids") && args.has("faces"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either brush ids in 'ids' or face ids in 'faces', not both.");
  }

  auto faces = std::vector<mdl::BrushFaceHandle>{};
  auto brushes = std::vector<mdl::Node*>{};
  if (args.has("faces") || (!args.has("ids") && map.selection().hasBrushFaces()))
  {
    auto resolved = resolveFaces(context, args);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    faces = std::move(resolved.value());
    brushes = uniqueBrushNodes(faces);
    if (faces.size() < 2)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Merging faces needs at least 2 faces.",
        "Pass 2 or more face ids; the new brush is the convex hull of their vertices.");
    }
  }
  else
  {
    auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
    if (targets.is_error())
    {
      return errorOf(targets);
    }
    brushes = std::move(targets.value());
    if (brushes.size() < 2)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Merging needs at least 2 brushes.",
        "Pass 2 or more brush ids.",
        formatIds(brushes, ids));
    }
  }

  const auto involved = faces.empty() ? formatIds(brushes, ids) : [&]() {
    auto result = std::vector<std::string>{};
    for (const auto& face : faces)
    {
      result.push_back(faceId(ids, face));
    }
    return result;
  }();

  return withTargets(
    context,
    brushes,
    [&]() -> ToolResult {
      if (!faces.empty())
      {
        mdl::deselectAll(map);
        mdl::selectBrushFaces(map, faces);
      }
      if (!mdl::csgConvexMerge(map) || map.selection().nodes.empty())
      {
        return geometryOperationFailed(
          context,
          "Could not merge: the convex hull of the vertices is not a valid brush.",
          involved,
          "Pass brushes or faces whose vertices span a volume (faces must not all lie "
          "in one plane).");
      }
      auto* merged = map.selection().nodes.front();
      warnNonIntegerVertices(context, {merged});
      return Json{
        {"brush", nodeSummary(*merged, ids)},
        {"removed", faces.empty() ? formatIds(brushes, ids) : std::vector<std::string>{}},
      };
    },
    SelectionAfter::Result);
}

ToolResult csgSubtract(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  auto cutters = resolveTargets(context, args, "ids", {ObjectKind::Brush});
  if (cutters.is_error())
  {
    return errorOf(cutters);
  }
  const auto cutterIds = formatIds(cutters.value(), ids);

  return withTargets(
    context,
    cutters.value(),
    [&]() -> ToolResult {
      // Find the brushes that the subtraction will cut, as csgSubtract does.
      mdl::selectTouchingNodes(map, false);
      const auto& touchedBrushes = map.selection().brushes;
      const auto cut = formatIds(
        std::vector<mdl::Node*>{touchedBrushes.begin(), touchedBrushes.end()}, ids);
      mdl::deselectAll(map);
      mdl::selectNodes(map, cutters.value());

      if (!mdl::csgSubtract(map))
      {
        return geometryOperationFailed(
          context, "Could not subtract the brushes.", cutterIds, {});
      }
      if (cut.empty())
      {
        context.warn(
          "NOTHING_SUBTRACTED",
          "The cutters touch no other visible, unlocked brush, so nothing was cut. As in "
          "the editor, the cutters were removed; undo to get them back.",
          cutterIds);
      }

      const auto fragments = selectedNodes(map);
      warnNonIntegerVertices(context, fragments);
      return Json{
        {"cutters", cutterIds},
        {"cut", cut},
        {"fragments", nodeSummaries(fragments, ids)},
      };
    },
    SelectionAfter::Result);
}

ToolResult csgIntersect(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  const auto targetIds = formatIds(targets.value(), ids);
  if (targets.value().size() < 2)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Intersecting needs at least 2 brushes.",
      "Pass 2 or more brush ids.",
      targetIds);
  }

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      if (!mdl::csgIntersect(map))
      {
        return geometryOperationFailed(
          context, "Could not intersect the brushes.", targetIds, {});
      }

      const auto result = selectedNodes(map);
      if (result.empty())
      {
        context.warn(
          "EMPTY_INTERSECTION",
          "The brushes have no common volume. As in the editor, all of them were "
          "removed; undo to get them back, or use dryRun first.",
          targetIds);
        return Json{{"brush", nullptr}};
      }
      warnNonIntegerVertices(context, result);
      return Json{{"brush", nodeSummary(*result.front(), ids)}};
    },
    SelectionAfter::Result);
}

ToolResult csgHollowTool(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();

  const auto thickness = args.getOr<double>("thickness", double(map.grid().actualSize()));
  if (!(thickness > 0.0))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "'thickness' must be positive.",
      "Pass the wall thickness in map units, e.g. 8.");
  }

  auto targets = resolveTargets(context, args, "ids", {ObjectKind::Brush});
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  const auto targetIds = formatIds(targets.value(), ids);

  return withTargets(
    context,
    targets.value(),
    [&]() -> ToolResult {
      if (!mcp::csgHollow(map, thickness))
      {
        return geometryOperationFailed(
          context,
          "Could not hollow the brushes with walls of thickness "
            + std::to_string(thickness) + ".",
          targetIds,
          "Each brush must be more than twice the thickness along every axis; use a "
          "smaller thickness.");
      }

      const auto notHollowed = existingIds(targetIds, ids);
      if (!notHollowed.empty())
      {
        context.warn(
          "NOT_HOLLOWED",
          "Some brushes are too small for walls of this thickness and were left "
          "unchanged.",
          notHollowed);
      }

      const auto fragments = selectedNodes(map);
      warnNonIntegerVertices(context, fragments);
      return Json{
        {"thickness", thickness},
        {"walls", nodeSummaries(fragments, ids)},
        {"notHollowed", notHollowed},
      };
    },
    SelectionAfter::Result);
}

// Schemas

Schema brushListSchema(std::string description)
{
  return array(any()).describe(std::move(description));
}

Schema idListSchema(std::string description)
{
  return array(string()).describe(std::move(description));
}

Field facesField(std::string description)
{
  return field("faces", array(objectId({ObjectKind::Brush})).nonEmpty())
    .describe(std::move(description));
}

std::vector<Field> handleFields()
{
  return {
    idsField(
      {ObjectKind::Brush},
      "Brushes whose handles are affected; each handle must belong to one of them. "
      "Default: the selected brushes if they have every handle, else every visible, "
      "unlocked brush that has one of the handles"),
    field("vertices", array(vec3()).nonEmpty())
      .describe("Vertex positions in map units, e.g. [[0, 0, 64]]"),
    field("edges", array(array(vec3()).minSize(2).maxSize(2)).nonEmpty())
      .describe(
        "Edges, each given by its two end points, e.g. [[[0, 0, 64], [64, 0, 64]]]"),
    field("faces", array(array(vec3()).minSize(3)).nonEmpty())
      .describe("Faces, each given by all its vertex positions (any order)"),
  };
}

} // namespace

void registerBrushEditTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"brush_clip"}
      .title("Clip Brushes")
      .description(
        "Cuts brushes with a plane, like the Clip tool, in one undo step. The plane is "
        "given by 'points' (map units) or by an existing 'face'. With 3 points p0, p1, "
        "p2 the plane normal is "
        "normalize(cross(p1 - p0, p2 - p0)); with 2 points a, b the plane contains the "
        "line a-b and is parallel to 'axis' (default z, like clipping in the top view), "
        "and the normal is cross(b - a, axis), i.e. for axis z the right-hand side when "
        "walking from a to b seen from above. With 'face' the normal is the face's "
        "outward normal. 'front' is the side the normal points to. 'keep' chooses "
        "'front', 'back' or 'both' (split). Brushes cut by the plane are replaced by "
        "new brushes (new ids); brushes entirely on the discarded side are removed; "
        "brushes entirely on the kept side stay unchanged. New faces copy the "
        "attributes of the brush's face with the closest normal. As in the editor, the "
        "resulting brushes are selected afterwards. "
        "Example: {\"ids\": [\"brush:12\"], \"points\": [[0, 0, 0], [64, 64, 0]], "
        "\"keep\": \"back\"}")
      .input(object({
        idsField({ObjectKind::Brush}, "Brushes to clip. Default: the selected brushes"),
        field("points", array(vec3()).minSize(2).maxSize(3))
          .describe("2 or 3 points on the clip plane (see the description for the "
                    "normal)"),
        field("axis", enumOf({"x", "y", "z"}))
          .describe("With 2 points: the plane is parallel to this axis. Default: 'z'"),
        field("face", objectId({ObjectKind::Brush}))
          .describe("A face id such as 'brush:7/face:2' whose plane is used"),
        field("keep", enumOf({"front", "back", "both"}).defaultsTo("both"))
          .describe("Which side to keep; 'both' splits the brushes"),
      }))
      .output(object({
        field("brushes", brushListSchema("The resulting brushes (nodeSummary)"))
          .required(),
        field("split", idListSchema("Brushes that were cut (replaced by new brushes)")),
        field(
          "discarded",
          idListSchema("Brushes removed because they lay entirely on "
                       "the discarded side")),
        field("unchanged", idListSchema("Brushes entirely on the kept side")),
        field("normal", vec3()).describe("The plane normal; 'front' is where it points"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(brushClip));

  registry.add(
    ToolDef{"face_extrude"}
      .title("Extrude Faces")
      .description(
        "Moves faces along their normals by 'distance' (positive: outward, the brush "
        "grows; negative: inward, it shrinks), like dragging faces with the Extrude "
        "tool (one undo step). Faces with different normals each move along their own "
        "normal. Adjacent faces stretch; the brush keeps its id. The selection is "
        "restored afterwards. Use face_extrude_new to create new brushes instead. "
        "Example: {\"faces\": [\"brush:12/face:4\"], \"distance\": 32}")
      .input(object({
        facesField("Face ids such as 'brush:12/face:4'. Default: the selected faces"),
        field("distance", number())
          .required()
          .describe("Distance along each face normal in map units; not 0"),
        field("alignmentLock", boolean())
          .describe("Keep textures aligned. Default: the editor's alignment lock "
                    "(locks_get)"),
      }))
      .output(object({
        field("faces", array(any())).describe("The extruded faces (faceJson)"),
        field("brushes", brushListSchema("The changed brushes (nodeSummary)")),
      }))
      .mutation(Mutation::Map)
      .handler(faceExtrude));

  registry.add(
    ToolDef{"face_extrude_new"}
      .title("Extrude Faces to New Brushes")
      .description(
        "Creates new brushes from faces (one undo step). mode 'split' works like "
        "ctrl-dragging a face "
        "with the Extrude tool: with a positive distance a new brush of that thickness "
        "is created in front of the face (the original is unchanged); with a negative "
        "distance the brush is split: the original keeps a slab of |distance| next to "
        "the face and the rest becomes a new brush. mode 'stamp' (positive distance "
        "only) creates the convex hull of the face and its copy moved by distance along "
        "the normal, using the face's material; the original is unchanged. The new "
        "brushes (and, for an inward split, the resized originals) are selected "
        "afterwards. "
        "Example: {\"faces\": [\"brush:12/face:4\"], \"distance\": 64, \"mode\": "
        "\"split\"}")
      .input(object({
        facesField("Face ids such as 'brush:12/face:4'. Default: the selected faces"),
        field("distance", number())
          .required()
          .describe("Distance along each face normal in map units; not 0"),
        field("mode", enumOf({"split", "stamp"}).defaultsTo("split"))
          .describe("'split' (extrude to a new brush / split inward) or 'stamp' (hull "
                    "of the face and its moved copy)"),
        field("alignmentLock", boolean())
          .describe("Keep textures aligned. Default: the editor's alignment lock "
                    "(locks_get)"),
      }))
      .output(object({
        field("brushes", brushListSchema("The new brushes (nodeSummary)")).required(),
        field("resized", idListSchema("Original brushes shrunk by an inward split")),
      }))
      .mutation(Mutation::Map)
      .handler(faceExtrudeNew));

  registry.add(
    ToolDef{"vertices_move"}
      .title("Move Vertices")
      .description(
        "Moves vertices, edges or faces of brushes by 'vector' (map units), like the "
        "Vertex, Edge and Face tools, in one undo step. Pass exactly one of 'vertices', "
        "'edges' or 'faces', given by "
        "positions (matched within 0.01 units; use object_get with detail 'full' to "
        "list them). Target brushes: 'ids' if given; else the selected brushes if they "
        "have every handle; else every visible, unlocked brush that has one of the "
        "handles (warning HANDLES_OUTSIDE_SELECTION if brushes are selected). Shared "
        "handles move together: every target brush that has the handle is changed. "
        "Handles that are not found fail with INVALID_ARGUMENT (OBJECT_NOT_EDITABLE if "
        "only hidden or locked brushes have them); the error says where they were "
        "searched and which brushes have them. A move that would make a brush non-convex "
        "or degenerate "
        "fails with INVALID_GEOMETRY naming the brushes. Moving a vertex onto another "
        "merges them (hasRemainingVertices false). The selection is restored "
        "afterwards. "
        "Example: {\"vertices\": [[64, 64, 64]], \"vector\": [0, 0, 32]}")
      .input(object([] {
        auto fields = handleFields();
        fields.push_back(field("vector", vec3())
                           .required()
                           .describe("Offset [x, y, z] in map units; not zero"));
        fields.push_back(
          field("uvLock", boolean())
            .describe("Keep UVs locked. Default: the editor's UV lock (locks_get)"));
        return fields;
      }()))
      .output(object({
        field("brushes", brushListSchema("The changed brushes (nodeSummary)")),
        field("hasRemainingVertices", boolean())
          .describe("False if the moved vertices were merged away"),
      }))
      .mutation(Mutation::Map)
      .handler(verticesMove));

  registry.add(
    ToolDef{"vertex_add"}
      .title("Add Vertex")
      .description(
        "Adds a vertex to a brush, like double-clicking with the Vertex tool: the brush "
        "becomes the convex hull of its vertices and the new position, which must lie "
        "outside the brush (one undo step). The selection is restored afterwards. Use "
        "vertices_move to move an existing vertex. "
        "Example: {\"brush\": \"brush:12\", \"position\": [32, 32, 96]}")
      .input(object({
        field("brush", objectId({ObjectKind::Brush})).required().describe("Brush id"),
        field("position", vec3())
          .required()
          .describe("Position of the new vertex in map units; outside the brush"),
      }))
      .output(object({
        field("brush", any()).describe("The changed brush (nodeSummary)"),
        field("vertexCount", integer()).describe("Vertices of the brush afterwards"),
      }))
      .mutation(Mutation::Map)
      .handler(vertexAdd));

  registry.add(
    ToolDef{"vertices_remove"}
      .title("Remove Vertices")
      .description(
        "Removes vertices, edges (both end points) or faces (all their vertices) from "
        "brushes in one undo step, like Delete in the Vertex, Edge and Face tools; each "
        "brush becomes the "
        "convex hull of its remaining vertices. Handles are given and target brushes "
        "chosen as in vertices_move. Fails with INVALID_GEOMETRY if a brush would "
        "become degenerate. The selection is restored afterwards. "
        "Example: {\"vertices\": [[64, 64, 64]]}")
      .input(object(handleFields()))
      .output(object({
        field("brushes", brushListSchema("The changed brushes (nodeSummary)")),
      }))
      .mutation(Mutation::Map)
      .handler(verticesRemove));

  registry.add(
    ToolDef{"vertices_snap"}
      .title("Snap Vertices")
      .description(
        "Snaps all vertices of brushes to the grid ('grid', default), to integers "
        "('integer') or to multiples of 'snapTo' (map units), like Edit > Snap Vertices "
        "(one undo step); pass 'mode' or 'snapTo', not both. Brushes "
        "that would become degenerate are left unchanged and reported in 'failed'. "
        "Groups and brush entities act on their brushes. The selection is restored "
        "afterwards. Example: {\"ids\": [\"brush:12\"], \"mode\": \"integer\"}")
      .input(object({
        idsField(
          {ObjectKind::Brush, ObjectKind::Entity, ObjectKind::Group},
          "Brushes, brush entities or groups. Default: the selection"),
        field("mode", enumOf({"grid", "integer"}))
          .describe("Snap to the current grid size or to integers. Default: 'grid'"),
        field("snapTo", number())
          .describe("Snap to multiples of this distance in map units (> 0) instead of "
                    "'mode'"),
        field("uvLock", boolean())
          .describe("Keep UVs locked. Default: the editor's UV lock (locks_get)"),
      }))
      .output(object({
        field("snapTo", number()).describe("The snap distance used (map units)"),
        field("snapped", idListSchema("Brushes whose vertices were snapped")),
        field("failed", idListSchema("Brushes left unchanged")),
      }))
      .mutation(Mutation::Map)
      .handler(verticesSnap));

  registry.add(
    ToolDef{"csg_merge"}
      .title("CSG Convex Merge")
      .description(
        "Replaces brushes by one brush that is the convex hull of all their vertices "
        "(the gap between disjoint brushes is filled), like CSG > Convex Merge, in one "
        "undo step. With "
        "'faces', a new brush is built from the convex hull of the faces' vertices and "
        "the original brushes are kept. Default: the selected brushes or faces. As in "
        "the editor, the new brush is selected afterwards. "
        "Example: {\"ids\": [\"brush:12\", \"brush:13\"]}")
      .input(object({
        idsField({ObjectKind::Brush}, "At least 2 brushes. Default: the selection"),
        facesField("At least 2 face ids, instead of 'ids'"),
      }))
      .output(object({
        field("brush", any()).describe("The new brush (nodeSummary)"),
        field("removed", idListSchema("The merged brushes (removed)")),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(csgMerge));

  registry.add(
    ToolDef{"csg_subtract"}
      .title("CSG Subtract")
      .description(
        "Subtracts the cutter brushes from all other visible, unlocked brushes they "
        "touch, like CSG > Subtract, in one undo step: the touched brushes are replaced "
        "by fragments "
        "(new ids) and the cutters are removed. If the cutters touch nothing, they are "
        "still removed (warning NOTHING_SUBTRACTED). As in the editor, the fragments "
        "are selected afterwards. "
        "Example: {\"ids\": [\"brush:40\"]}")
      .input(object({
        idsField({ObjectKind::Brush}, "Cutter brushes. Default: the selected brushes"),
      }))
      .output(object({
        field("cutters", idListSchema("The removed cutter brushes")),
        field("cut", idListSchema("Brushes that were cut (replaced by fragments)")),
        field("fragments", brushListSchema("The new brushes (nodeSummary)")),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(csgSubtract));

  registry.add(
    ToolDef{"csg_intersect"}
      .title("CSG Intersect")
      .description(
        "Replaces brushes by their common volume, like CSG > Intersect, in one undo "
        "step. If they do not "
        "overlap, all of them are removed as in the editor (warning "
        "EMPTY_INTERSECTION, brush null); use dryRun to check first. The new brush is "
        "selected afterwards. Example: {\"ids\": [\"brush:12\", \"brush:13\"]}")
      .input(object({
        idsField({ObjectKind::Brush}, "At least 2 brushes. Default: the selection"),
      }))
      .output(object({
        field("brush", any()).describe("The intersection (nodeSummary) or null"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(csgIntersect));

  registry.add(
    ToolDef{"csg_hollow"}
      .title("CSG Hollow")
      .description(
        "Hollows brushes into walls of the given thickness (default: the grid size), "
        "like CSG > Hollow, in one undo step: each brush is replaced by wall brushes "
        "around its former "
        "interior. Brushes too small for the thickness stay unchanged (warning "
        "NOT_HOLLOWED). As in the editor, the walls are selected afterwards. "
        "Example: {\"ids\": [\"brush:12\"], \"thickness\": 16}")
      .input(object({
        idsField({ObjectKind::Brush}, "Brushes to hollow. Default: the selection"),
        field("thickness", number())
          .describe("Wall thickness in map units (default: the grid size)"),
      }))
      .output(object({
        field("thickness", number()).describe("The wall thickness used (map units)"),
        field("walls", brushListSchema("The new wall brushes (nodeSummary)")),
        field("notHollowed", idListSchema("Brushes that were too small")),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(csgHollowTool));
}

} // namespace tb::mcp
