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

#include "mcp/tools/PlacementChecks.h"

#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mcp/tools/UvCheck.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace tb::mcp
{
namespace
{

/** Distances in world units below this are zero (planes, polygon clipping). */
constexpr auto PlaneEpsilon = 0.01;
/** Normals whose dot product exceeds this are parallel. */
constexpr auto ParallelDot = 1.0 - 1e-6;
/** Point entities this close to a changed brush are checked for model placement. */
constexpr auto PlacementNeighbourDistance = 2.0;

std::string lower(const std::string_view str)
{
  auto result = std::string{str};
  std::ranges::transform(
    result, result.begin(), [](const unsigned char c) { return char(std::tolower(c)); });
  return result;
}

double round2(const double value)
{
  return std::round(value * 100.0) / 100.0;
}

/** Identifies a plane (normal and distance) up to rounding, for signatures. */
std::string planeKey(const vm::plane3d& plane)
{
  const auto& n = plane.normal;
  const auto r = [](const double v) { return std::round(v * 1000.0) / 1000.0 + 0.0; };
  return fmt::format(
    "{:g},{:g},{:g},{:g}",
    r(n.x()),
    r(n.y()),
    r(n.z()),
    std::round(plane.distance * 10.0) / 10.0 + 0.0);
}

std::string planeKey(const mdl::BrushFace& face)
{
  return planeKey(face.boundary());
}

std::string classnameOf(const mdl::EntityNodeBase* entityNode)
{
  return entityNode ? entityNode->entity().classname() : std::string{};
}

std::string vecText(const vm::vec3d& v)
{
  return fmt::format(
    "({:g} {:g} {:g})",
    roundForOutput(v.x()),
    roundForOutput(v.y()),
    roundForOutput(v.z()));
}

// polygons in a plane

using Polygon = std::vector<vm::vec2d>;

struct PlaneBasis
{
  vm::vec3d origin;
  vm::vec3d u;
  vm::vec3d v;

  explicit PlaneBasis(const vm::plane3d& plane)
  {
    const auto& n = plane.normal;
    const auto helper = std::abs(n.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{1, 0, 0};
    u = vm::normalize(vm::cross(n, helper));
    v = vm::cross(n, u);
    origin = n * plane.distance;
  }

  vm::vec2d project(const vm::vec3d& point) const
  {
    const auto d = point - origin;
    return {vm::dot(d, u), vm::dot(d, v)};
  }

  vm::vec3d unproject(const vm::vec2d& point) const
  {
    return origin + u * point.x() + v * point.y();
  }

  Polygon project(const std::vector<vm::vec3d>& points) const
  {
    auto result = Polygon{};
    result.reserve(points.size());
    for (const auto& point : points)
    {
      result.push_back(project(point));
    }
    return result;
  }
};

double signedArea(const Polygon& polygon)
{
  auto sum = 0.0;
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1) % polygon.size()];
    sum += a.x() * b.y() - b.x() * a.y();
  }
  return sum / 2.0;
}

double area(const Polygon& polygon)
{
  return polygon.size() < 3 ? 0.0 : std::abs(signedArea(polygon));
}

Polygon counterClockwise(Polygon polygon)
{
  if (signedArea(polygon) < 0.0)
  {
    std::ranges::reverse(polygon);
  }
  return polygon;
}

double cross2(const vm::vec2d& a, const vm::vec2d& b, const vm::vec2d& p)
{
  return (b.x() - a.x()) * (p.y() - a.y()) - (b.y() - a.y()) * (p.x() - a.x());
}

/** Intersects two convex polygons (Sutherland-Hodgman). */
Polygon clip(const Polygon& subject, const Polygon& clipper)
{
  if (subject.size() < 3 || clipper.size() < 3)
  {
    return {};
  }

  auto result = counterClockwise(subject);
  const auto edges = counterClockwise(clipper);
  for (size_t i = 0; i < edges.size() && !result.empty(); ++i)
  {
    const auto& a = edges[i];
    const auto& b = edges[(i + 1) % edges.size()];
    const auto input = std::move(result);
    result.clear();
    for (size_t j = 0; j < input.size(); ++j)
    {
      const auto& p = input[j];
      const auto& q = input[(j + 1) % input.size()];
      const auto dp = cross2(a, b, p);
      const auto dq = cross2(a, b, q);
      const auto pInside = dp >= -1e-9;
      const auto qInside = dq >= -1e-9;
      if (pInside)
      {
        result.push_back(p);
      }
      if (pInside != qInside)
      {
        const auto t = dp / (dp - dq);
        result.push_back(p + (q - p) * t);
      }
    }
  }
  return result.size() >= 3 ? result : Polygon{};
}

vm::vec2d centroid(const Polygon& polygon)
{
  const auto a = signedArea(polygon);
  if (std::abs(a) < 1e-9)
  {
    auto sum = vm::vec2d{0, 0};
    for (const auto& p : polygon)
    {
      sum = sum + p;
    }
    return sum / double(polygon.size());
  }
  auto cx = 0.0;
  auto cy = 0.0;
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    const auto& p = polygon[i];
    const auto& q = polygon[(i + 1) % polygon.size()];
    const auto f = p.x() * q.y() - q.x() * p.y();
    cx += (p.x() + q.x()) * f;
    cy += (p.y() + q.y()) * f;
  }
  return {cx / (6.0 * a), cy / (6.0 * a)};
}

vm::bbox3d boundsOf(const Polygon& polygon, const PlaneBasis& basis)
{
  auto result =
    vm::bbox3d{basis.unproject(polygon.front()), basis.unproject(polygon.front())};
  for (const auto& point : polygon)
  {
    result = vm::merge(result, basis.unproject(point));
  }
  return result;
}

bool sameFacing(const mdl::BrushFace& a, const mdl::BrushFace& b)
{
  return vm::dot(a.boundary().normal, b.boundary().normal) > ParallelDot
         && std::abs(a.boundary().distance - b.boundary().distance) < PlaneEpsilon;
}

bool oppositeFacing(const mdl::BrushFace& a, const mdl::BrushFace& b)
{
  return vm::dot(a.boundary().normal, b.boundary().normal) < -ParallelDot
         && std::abs(a.boundary().distance + b.boundary().distance) < PlaneEpsilon;
}

bool checkedFace(const mdl::BrushFace& face)
{
  return face.geometry() && !isToolMaterial(face.materialName());
}

using FaceKey = std::pair<mdl::IdType, size_t>;

FaceKey keyOf(const mdl::BrushNode& brushNode, const size_t faceIndex)
{
  return {brushNode.runtimeId(), faceIndex};
}

class ZFightingFinder
{
private:
  const mdl::Map& m_map;
  std::set<std::pair<FaceKey, FaceKey>> m_found;
  std::vector<ZFighting> m_result;

public:
  explicit ZFightingFinder(const mdl::Map& map)
    : m_map{map}
  {
  }

  void check(const mdl::BrushNode& brushNode)
  {
    if (ignoredForZFighting(brushNode))
    {
      return;
    }
    const auto& faces = brushNode.brush().faces();
    for (size_t i = 0; i < faces.size(); ++i)
    {
      if (checkedFace(faces[i]))
      {
        checkFace(brushNode, i);
      }
    }
  }

  std::vector<ZFighting> result() && { return std::move(m_result); }

private:
  /** The brushes whose bounds intersect the bounds (the tree returns candidates). */
  std::vector<const mdl::BrushNode*> brushesNear(const vm::bbox3d& bounds) const
  {
    const auto expanded = bounds.expand(PlaneEpsilon);
    auto result = std::vector<const mdl::BrushNode*>{};
    for (const auto* node : m_map.worldNode().nodeTree().find_intersectors(expanded))
    {
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
          brushNode && brushNode->logicalBounds().intersects(expanded)
          && !ignoredForZFighting(*brushNode))
      {
        result.push_back(brushNode);
      }
    }
    return result;
  }

  void checkFace(const mdl::BrushNode& brushNode, const size_t faceIndex)
  {
    const auto& face = brushNode.brush().face(faceIndex);
    const auto basis = PlaneBasis{face.boundary()};
    const auto polygon = basis.project(face.vertexPositions());

    for (const auto* other : brushesNear(face.bounds()))
    {
      if (other == &brushNode)
      {
        continue;
      }
      const auto& otherFaces = other->brush().faces();
      for (size_t j = 0; j < otherFaces.size(); ++j)
      {
        const auto& otherFace = otherFaces[j];
        if (!checkedFace(otherFace) || !sameFacing(face, otherFace))
        {
          continue;
        }

        auto first = keyOf(brushNode, faceIndex);
        auto second = keyOf(*other, j);
        const auto pairKey =
          first < second ? std::pair{first, second} : std::pair{second, first};
        if (m_found.contains(pairKey))
        {
          continue;
        }

        const auto overlap = clip(polygon, basis.project(otherFace.vertexPositions()));
        const auto overlapArea = area(overlap);
        if (overlapArea <= MinZFightingArea || hidden(overlap, overlapArea, face, basis))
        {
          continue;
        }

        m_found.insert(pairKey);
        m_result.push_back(ZFighting{
          mdl::BrushFaceHandle{const_cast<mdl::BrushNode*>(&brushNode), faceIndex},
          mdl::BrushFaceHandle{const_cast<mdl::BrushNode*>(other), j},
          overlapArea,
          basis.unproject(centroid(overlap)),
          face.boundary(),
        });
      }
    }
  }

  /** Whether touching faces facing the other way cover the overlap. */
  bool hidden(
    const Polygon& overlap,
    const double overlapArea,
    const mdl::BrushFace& face,
    const PlaneBasis& basis) const
  {
    auto covered = 0.0;
    for (const auto* brushNode : brushesNear(boundsOf(overlap, basis)))
    {
      for (const auto& other : brushNode->brush().faces())
      {
        if (checkedFace(other) && oppositeFacing(face, other))
        {
          covered += area(clip(overlap, basis.project(other.vertexPositions())));
        }
      }
    }
    return covered >= overlapArea - MinZFightingArea;
  }
};

void collectBrushes(const mdl::Node& node, std::vector<const mdl::BrushNode*>& result)
{
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    result.push_back(brushNode);
    return;
  }
  for (const auto* child : node.children())
  {
    collectBrushes(*child, result);
  }
}

/**
 * The brushes and point entities among the nodes and their descendants. The world and
 * layers are skipped: they change when their properties or children change, not their
 * contents.
 */
void collectObjects(
  const std::vector<mdl::Node*>& nodes,
  std::vector<mdl::BrushNode*>& brushes,
  std::vector<mdl::EntityNode*>& entities)
{
  const auto visit = [&](const auto& self, mdl::Node& node) -> void {
    if (
      dynamic_cast<const mdl::WorldNode*>(&node)
      || dynamic_cast<const mdl::LayerNode*>(&node))
    {
      return;
    }
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node))
    {
      brushes.push_back(brushNode);
      return;
    }
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node);
        entityNode && !entityNode->hasChildren())
    {
      entities.push_back(entityNode);
      return;
    }
    for (auto* child : node.children())
    {
      self(self, *child);
    }
  };
  for (auto* node : nodes)
  {
    visit(visit, *node);
  }
}

std::vector<mdl::BrushFaceHandle> facesOf(const std::vector<mdl::BrushNode*>& brushes)
{
  auto result = std::vector<mdl::BrushFaceHandle>{};
  for (auto* brushNode : brushes)
  {
    for (size_t i = 0; i < brushNode->brush().faceCount(); ++i)
    {
      result.emplace_back(brushNode, i);
    }
  }
  return result;
}

/** Point entities whose bounds are near the brush. */
std::vector<mdl::EntityNode*> pointEntitiesNear(
  const mdl::Map& map, const mdl::BrushNode& brushNode)
{
  auto result = std::vector<mdl::EntityNode*>{};
  const auto bounds = brushNode.logicalBounds().expand(PlacementNeighbourDistance);
  for (auto* node : map.worldNode().nodeTree().find_intersectors(bounds))
  {
    if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
        entityNode && !entityNode->hasChildren()
        && entityNode->logicalBounds().intersects(bounds))
    {
      result.push_back(entityNode);
    }
  }
  return result;
}

/** Whether the change affects what leak prediction sees. */
bool relevantForLeaks(const std::string& id)
{
  return id.starts_with("brush:") || id.starts_with("entity:") || id.starts_with("patch:")
         || id.starts_with("group:");
}

bool relevantForLeaks(const mdl::Node& node)
{
  return dynamic_cast<const mdl::BrushNode*>(&node)
         || dynamic_cast<const mdl::EntityNode*>(&node)
         || dynamic_cast<const mdl::PatchNode*>(&node)
         || dynamic_cast<const mdl::GroupNode*>(&node);
}

/** Whether a point entity is inside the hull. */
bool anyEnclosed(const LeakReport& report)
{
  return report.checkedEntities > report.findings.size();
}

/** Whether a skipped analysis was skipped because the map is too large. */
bool skippedForSize(const LeakReport& report)
{
  return !report.analyzed
         && lower(report.skippedReason).find("cell") != std::string::npos;
}

} // namespace

std::vector<std::string> mcpIssueCodes()
{
  return {
    std::string{ZFightingCode},
    std::string{EntityOutsideHullCode},
    "MODEL_BELOW_FLOOR",
    "MODEL_FLOATING",
    "MODEL_PENETRATES_BRUSHES",
    "MODEL_NO_FLOOR",
    std::string{UvDistortionCode},
  };
}

bool isToolMaterial(const std::string_view materialName)
{
  static const auto exact = std::set<std::string, std::less<>>{
    "origin",         "aaatrigger", "skip",       "null",      "bevel",
    "caulk",          "areaportal", "weapclip",   "botclip",   "playerclip",
    "monsterclip",    "full_clip",  "donotenter", "lightgrid", "clusterportal",
    "nodrawnonsolid", "invisible",  "noshadow",   "missing",
  };
  static const auto prefixes =
    std::array<std::string_view, 5>{"clip", "trigger", "hint", "nodraw", "skip"};

  const auto slash = materialName.find_last_of("/\\");
  auto name = lower(
    slash == std::string_view::npos ? materialName : materialName.substr(slash + 1));
  if (name.empty())
  {
    return false;
  }
  return exact.contains(name) || std::ranges::any_of(prefixes, [&](const auto prefix) {
           return name.starts_with(prefix);
         });
}

bool ignoredForZFighting(const mdl::BrushNode& brushNode)
{
  const auto classname = lower(classnameOf(brushNode.entity()));
  return classname.starts_with("trigger_");
}

std::vector<ZFighting> findZFighting(
  const mdl::Map& map, const std::vector<const mdl::BrushNode*>* brushes)
{
  auto finder = ZFightingFinder{map};
  if (brushes)
  {
    for (const auto* brushNode : *brushes)
    {
      finder.check(*brushNode);
    }
  }
  else
  {
    auto all = std::vector<const mdl::BrushNode*>{};
    collectBrushes(map.worldNode(), all);
    for (const auto* brushNode : all)
    {
      finder.check(*brushNode);
    }
  }
  return std::move(finder).result();
}

std::vector<McpIssue> zFightingIssues(
  const std::vector<ZFighting>& pairs, const IdRegistry& ids)
{
  auto result = std::vector<McpIssue>{};
  for (const auto& pair : pairs)
  {
    const auto& firstFace = pair.first.face();
    const auto& secondFace = pair.second.face();
    const auto firstId = ids.formatFace(*pair.first.node(), pair.first.faceIndex());
    const auto secondId = ids.formatFace(*pair.second.node(), pair.second.faceIndex());
    const auto firstBrush = ids.format(*pair.first.node());
    const auto secondBrush = ids.format(*pair.second.node());

    auto issue = McpIssue{};
    issue.code = std::string{ZFightingCode};
    issue.type = "Z-fighting";
    issue.objectId = firstId;
    issue.description = fmt::format(
      "{} ('{}') and {} ('{}') lie in the same plane, face the same way and overlap by "
      "{:g} square units around {}: they flicker (z-fight) in the game. Move or resize "
      "one of the brushes so that the faces do not overlap, or give both the same "
      "face.",
      firstId,
      firstFace.materialName(),
      secondId,
      secondFace.materialName(),
      std::round(pair.area),
      vecText(pair.center));
    issue.details = Json{
      {"faces", Json{firstId, secondId}},
      {"brushes", Json{firstBrush, secondBrush}},
      {"materials", Json{firstFace.materialName(), secondFace.materialName()}},
      {"area", round2(pair.area)},
      {"center", toJson(pair.center)},
      {"plane",
       Json{
         {"normal", toJson(pair.plane.normal)},
         {"distance", roundForOutput(pair.plane.distance)},
       }},
    };
    const auto [a, b] = std::minmax(firstBrush, secondBrush);
    issue.signature =
      fmt::format("{}|{}|{}|{}", ZFightingCode, a, b, planeKey(pair.plane));
    issue.objectIds = {firstId, secondId, firstBrush, secondBrush};
    result.push_back(std::move(issue));
  }
  return result;
}

std::vector<McpIssue> leakIssues(const LeakReport& report, const IdRegistry& ids)
{
  auto result = std::vector<McpIssue>{};
  if (!report.analyzed)
  {
    return result;
  }

  for (const auto& finding : report.findings)
  {
    if (!finding.entity)
    {
      continue;
    }
    const auto id = ids.format(*finding.entity);
    const auto classname = finding.entity->entity().classname();

    auto gapBrushes = Json::array();
    auto objectIds = std::vector<std::string>{id};
    for (const auto* brushNode : finding.gapBrushes)
    {
      auto brushId = ids.format(*brushNode);
      gapBrushes.push_back(brushId);
      objectIds.push_back(std::move(brushId));
    }

    auto issue = McpIssue{};
    issue.code = std::string{EntityOutsideHullCode};
    issue.type = "Entity outside the hull";
    issue.objectId = id;
    issue.description =
      finding.gap
        ? fmt::format(
            "{} ({}) at {} is outside the sealed hull: the void outside the map reaches "
            "it through a gap around {} (between {}). The map leaks; close the gap or "
            "move the entity inside.",
            id,
            classname,
            vecText(finding.position),
            vecText(finding.gap->center()),
            gapBrushes.empty() ? std::string{"no brushes"} : gapBrushes.dump())
        : fmt::format(
            "{} ({}) at {} is outside the sealed hull: it is not enclosed by sealing "
            "brushes. The map leaks; move the entity inside a sealed room.",
            id,
            classname,
            vecText(finding.position));
    issue.details = Json{
      {"classname", classname},
      {"position", toJson(finding.position)},
      {"gap", finding.gap ? toJson(*finding.gap) : Json(nullptr)},
      {"gapBrushes", std::move(gapBrushes)},
      {"cellSize", roundForOutput(report.cellSize)},
    };
    issue.signature = fmt::format("{}|{}", EntityOutsideHullCode, id);
    issue.objectIds = std::move(objectIds);
    result.push_back(std::move(issue));
  }
  return result;
}

std::vector<McpIssue> modelPlacementIssues(
  mdl::Map& map,
  const IdRegistry& ids,
  const std::vector<mdl::EntityNode*>& entities,
  EntityModelLoader& loader)
{
  auto result = std::vector<McpIssue>{};
  for (const auto* entityNode : entities)
  {
    const auto state = resolveEntityModel(entityNode->entity(), loader);
    if (state.is_error())
    {
      continue;
    }
    const auto bounds = state.value().worldBounds();
    if (!bounds)
    {
      continue;
    }

    const auto id = ids.format(*entityNode);
    const auto check =
      checkModelPlacement(*bounds, map, ids, id, placementSubject(id, state.value()));
    for (const auto& finding : check.findings)
    {
      auto related = std::vector<std::string>{};
      for (const auto& objectId : finding.objectIds)
      {
        if (objectId != id)
        {
          related.push_back(objectId);
        }
      }
      std::ranges::sort(related);

      auto issue = McpIssue{};
      issue.code = finding.code;
      issue.type = "Model placement";
      issue.objectId = id;
      issue.description = finding.message;
      issue.details = Json{
        {"classname", entityNode->entity().classname()},
        {"modelBounds", toJson(*bounds)},
        {"surface", placementSurfaceJson(check.surface, ids)},
        {"relatedIds", related},
      };
      if (finding.distance)
      {
        issue.details["distance"] = roundForOutput(*finding.distance);
      }
      if (finding.suggestedMove)
      {
        issue.details["suggestedMove"] = toJson(*finding.suggestedMove);
      }
      issue.signature =
        fmt::format("{}|{}|{}", finding.code, id, fmt::join(related, ","));
      issue.objectIds = finding.objectIds;
      result.push_back(std::move(issue));
    }
  }
  return result;
}

std::vector<McpIssue> uvDistortionIssues(
  const std::vector<mdl::BrushFaceHandle>& faces,
  const mdl::Map& map,
  MaterialKnowledge& knowledge,
  const IdRegistry& ids)
{
  auto candidates = std::vector<mdl::BrushFaceHandle>{};
  for (const auto& handle : faces)
  {
    const auto& face = handle.face();
    if (!face.geometry() || isToolMaterial(face.materialName()))
    {
      continue;
    }
    const auto sample = sampleFace(face, textureSizeOf(face.material()));
    if (!sample)
    {
      continue;
    }
    const auto aspect = sample->texelDensity.x() / sample->texelDensity.y();
    const auto off = std::max(aspect, 1.0 / aspect) - 1.0;
    const auto key = lower(face.materialName());
    if (off > MaxAspectDeviation || knowledge.note(key) || knowledge.corpusEntry(key))
    {
      candidates.push_back(handle);
    }
  }
  if (candidates.empty())
  {
    return {};
  }

  auto result = std::vector<McpIssue>{};
  for (const auto& finding :
       checkUv(candidates, map, profileProvider(knowledge), {UvIssue::AspectDistortion}))
  {
    auto json = toJson(finding, ids);
    const auto faceId = json["face"].get<std::string>();
    const auto brushId = ids.format(*finding.face.node());

    auto issue = McpIssue{};
    issue.code = std::string{UvDistortionCode};
    issue.type = "Texture distortion";
    issue.objectId = faceId;
    issue.description = finding.message;
    issue.details = Json{
      {"face", faceId},
      {"brush", brushId},
      {"material", finding.material},
      {"measured", json["measured"]},
    };
    if (json.contains("fix"))
    {
      issue.details["fix"] = json["fix"];
    }
    issue.signature =
      fmt::format("{}|{}|{}", UvDistortionCode, brushId, planeKey(finding.face.face()));
    issue.objectIds = {faceId, brushId};
    result.push_back(std::move(issue));
  }
  return result;
}

// PlacementCache

void PlacementCache::nodesAddedOrRemoved()
{
  ++changeCount;
}

void PlacementCache::nodesChanged(const std::vector<mdl::Node*>& nodes)
{
  if (std::ranges::any_of(nodes, [](const auto* node) {
        return !dynamic_cast<const mdl::WorldNode*>(node)
               && !dynamic_cast<const mdl::LayerNode*>(node);
      }))
  {
    ++changeCount;
  }
}

bool PlacementCache::leakCacheValid() const
{
  return leakKey == changeCount;
}

void PlacementCache::rolledBack(const size_t changeCountAtStart)
{
  if (leakKey == changeCountAtStart)
  {
    leakKey = changeCount;
  }
}

// PlacementTracker

PlacementTracker::PlacementTracker(
  mdl::Map& map, const IdRegistry& ids, PlacementTrackerOptions options)
  : m_map{map}
  , m_ids{ids}
  , m_options{std::move(options)}
{
  if (auto* cache = m_options.cache)
  {
    m_changeCountAtStart = cache->changeCount;
    if (cache->leakCacheValid())
    {
      m_leaksBefore = cache->leakSignatures;
      m_leaksBeforeEnclosed = cache->leakAnyEnclosed;
    }
  }
}

PlacementTracker::~PlacementTracker() = default;

EntityModelLoader& PlacementTracker::loader()
{
  if (!m_loader)
  {
    m_loader = std::make_unique<EntityModelLoader>(m_map);
  }
  return *m_loader;
}

MaterialKnowledge& PlacementTracker::knowledge()
{
  if (!m_knowledge)
  {
    m_knowledge =
      std::make_unique<MaterialKnowledge>(m_map, m_options.knowledgeDirectory);
  }
  return *m_knowledge;
}

void PlacementTracker::nodesWillChange(const std::vector<mdl::Node*>& nodes)
{
  snapshot(nodes);
}

void PlacementTracker::nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes)
{
  snapshot(nodes);
  m_changed = true;
}

void PlacementTracker::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  const auto visit = [&](const auto& self, const mdl::Node& node) -> void {
    m_added.insert(m_ids.format(node));
    for (const auto* child : node.children())
    {
      self(self, *child);
    }
  };
  for (const auto* node : nodes)
  {
    visit(visit, *node);
  }
  m_changed = true;
}

void PlacementTracker::nodesDidChange()
{
  m_changed = true;
}

void PlacementTracker::snapshotLeaks()
{
  auto* cache = m_options.cache;
  if (
    !cache || cache->leakChecksDisabled || m_leaksBefore || m_leakBeforeTried
    || m_changed)
  {
    return;
  }
  m_leakBeforeTried = true;

  const auto start = std::chrono::steady_clock::now();
  const auto report = predictLeaks(m_map);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (!report.analyzed)
  {
    if (skippedForSize(report))
    {
      disableLeakChecks(report.skippedReason);
    }
    return;
  }

  auto signatures = std::unordered_set<std::string>{};
  for (const auto& issue : leakIssues(report, m_ids))
  {
    signatures.insert(issue.signature);
  }
  cache->leakKey = cache->changeCount;
  cache->leakSignatures = signatures;
  cache->leakAnyEnclosed = anyEnclosed(report);
  m_leaksBefore = std::move(signatures);
  m_leaksBeforeEnclosed = cache->leakAnyEnclosed;

  if (elapsed > MaxPerCallLeakTime)
  {
    disableLeakChecks(fmt::format(
      "leak prediction took {} ms",
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
  }
}

void PlacementTracker::disableLeakChecks(const std::string& reason)
{
  auto* cache = m_options.cache;
  if (!cache || cache->leakChecksDisabled)
  {
    return;
  }
  cache->leakChecksDisabled = true;
  m_warnings.push_back(Warning{
    "LEAK_CHECK_SKIPPED",
    fmt::format(
      "Entities outside the hull are no longer checked after every call on this map "
      "({}). Run issues_list with codes [\"{}\"] to check for leaks.",
      reason,
      EntityOutsideHullCode),
  });
}

void PlacementTracker::snapshot(const std::vector<mdl::Node*>& nodes)
{
  if (std::ranges::any_of(
        nodes, [](const auto* node) { return relevantForLeaks(*node); }))
  {
    snapshotLeaks();
  }

  auto brushes = std::vector<const mdl::BrushNode*>{};
  auto uvBrushes = std::vector<mdl::BrushNode*>{};
  auto entities = std::vector<mdl::EntityNode*>{};
  const auto addEntity = [&](mdl::EntityNode* entityNode) {
    auto id = m_ids.format(*entityNode);
    if (!m_added.contains(id) && m_entitiesSnapshotted.insert(std::move(id)).second)
    {
      entities.push_back(entityNode);
    }
  };

  auto changedBrushes = std::vector<mdl::BrushNode*>{};
  auto changedEntities = std::vector<mdl::EntityNode*>{};
  collectObjects(nodes, changedBrushes, changedEntities);

  for (auto* brushNode : changedBrushes)
  {
    auto id = m_ids.format(*brushNode);
    if (m_added.contains(id) || !m_zSnapshotted.insert(std::move(id)).second)
    {
      continue;
    }
    brushes.push_back(brushNode);
    uvBrushes.push_back(brushNode);
    for (auto* entityNode : pointEntitiesNear(m_map, *brushNode))
    {
      addEntity(entityNode);
    }
  }
  for (auto* entityNode : changedEntities)
  {
    addEntity(entityNode);
  }

  if (!brushes.empty())
  {
    for (const auto& issue : zFightingIssues(findZFighting(m_map, &brushes), m_ids))
    {
      m_zBefore.insert(issue.signature);
    }
    for (const auto& issue :
         uvDistortionIssues(facesOf(uvBrushes), m_map, knowledge(), m_ids))
    {
      m_uvBefore.insert(issue.signature);
    }
  }
  if (!entities.empty())
  {
    for (const auto& issue : modelPlacementIssues(m_map, m_ids, entities, loader()))
    {
      m_placementBefore.insert(issue.signature);
    }
  }
}

PlacementReport PlacementTracker::finish(
  const std::vector<std::string>& created,
  const std::vector<std::string>& modified,
  const std::vector<std::string>& removed)
{
  auto report = PlacementReport{};

  const auto createdIds = std::unordered_set<std::string>{created.begin(), created.end()};
  // the changed objects: created nodes are listed with their descendants; modified
  // groups and brush entities (e.g. moved as a whole) stand for their contents, while
  // layers and the world are modified whenever a child is added or removed
  auto changedNodes = std::vector<mdl::Node*>{};
  auto seenNodes = std::unordered_set<const mdl::Node*>{};
  const auto addChanged = [&](const auto& self, mdl::Node& node) -> void {
    if (
      dynamic_cast<const mdl::WorldNode*>(&node)
      || dynamic_cast<const mdl::LayerNode*>(&node) || !seenNodes.insert(&node).second)
    {
      return;
    }
    changedNodes.push_back(&node);
    for (auto* child : node.children())
    {
      self(self, *child);
    }
  };
  for (const auto* list : {&created, &modified})
  {
    for (const auto& id : *list)
    {
      if (auto node = m_ids.resolve(id); node.is_success())
      {
        addChanged(addChanged, *node.value());
      }
    }
  }

  // brushes: only the brushes themselves, descendants of created nodes are listed
  auto brushes = std::vector<const mdl::BrushNode*>{};
  auto uvBrushes = std::vector<mdl::BrushNode*>{};
  auto brushIds = std::unordered_set<std::string>{};
  for (auto* node : changedNodes)
  {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
    {
      brushes.push_back(brushNode);
      uvBrushes.push_back(brushNode);
      brushIds.insert(m_ids.format(*brushNode));
    }
  }

  const auto involvesCreated = [&](const McpIssue& issue) {
    return std::ranges::any_of(
      issue.objectIds, [&](const auto& id) { return createdIds.contains(id); });
  };

  // z-fighting
  if (!brushes.empty())
  {
    for (auto& issue : zFightingIssues(findZFighting(m_map, &brushes), m_ids))
    {
      if (involvesCreated(issue) || !m_zBefore.contains(issue.signature))
      {
        report.issues.push_back(std::move(issue));
      }
    }
  }

  // model placement
  {
    auto entities = std::vector<mdl::EntityNode*>{};
    auto direct = std::unordered_set<std::string>{m_entitiesSnapshotted};
    auto seen = std::unordered_set<const mdl::EntityNode*>{};
    const auto add = [&](mdl::EntityNode* entityNode) {
      if (seen.insert(entityNode).second)
      {
        entities.push_back(entityNode);
      }
    };
    for (auto* node : changedNodes)
    {
      auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
      if (!entityNode || entityNode->hasChildren())
      {
        continue;
      }
      direct.insert(m_ids.format(*entityNode));
      add(entityNode);
    }
    for (const auto& id : m_entitiesSnapshotted)
    {
      if (auto node = m_ids.resolve(id); node.is_success())
      {
        if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(node.value()))
        {
          add(entityNode);
        }
      }
    }
    for (const auto* brushNode : brushes)
    {
      for (auto* entityNode : pointEntitiesNear(m_map, *brushNode))
      {
        add(entityNode);
      }
    }

    if (!entities.empty())
    {
      for (auto& issue : modelPlacementIssues(m_map, m_ids, entities, loader()))
      {
        const auto namesChangedBrush = std::ranges::any_of(
          issue.objectIds, [&](const auto& id) { return brushIds.contains(id); });
        if (
          involvesCreated(issue)
          || (!m_placementBefore.contains(issue.signature) && (direct.contains(issue.objectId) || namesChangedBrush)))
        {
          report.issues.push_back(std::move(issue));
        }
      }
    }
  }

  // texture distortion
  if (!uvBrushes.empty())
  {
    for (auto& issue : uvDistortionIssues(facesOf(uvBrushes), m_map, knowledge(), m_ids))
    {
      if (involvesCreated(issue) || !m_uvBefore.contains(issue.signature))
      {
        report.issues.push_back(std::move(issue));
      }
    }
  }

  // entities outside the hull
  auto* cache = m_options.cache;
  const auto leakRelevant = [&]() {
    for (const auto* list : {&created, &modified, &removed})
    {
      if (std::ranges::any_of(*list, [](const auto& id) { return relevantForLeaks(id); }))
      {
        return true;
      }
    }
    return false;
  };
  if (cache && !cache->leakChecksDisabled && leakRelevant())
  {
    const auto start = std::chrono::steady_clock::now();
    const auto leaks = predictLeaks(m_map);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    if (leaks.analyzed)
    {
      // without a sealed room, only entities next to a gap are reported
      const auto sealed = anyEnclosed(leaks) || m_leaksBeforeEnclosed;
      auto signatures = std::unordered_set<std::string>{};
      for (auto& issue : leakIssues(leaks, m_ids))
      {
        signatures.insert(issue.signature);
        const auto introduced = m_leaksBefore ? !m_leaksBefore->contains(issue.signature)
                                              : createdIds.contains(issue.objectId);
        if (introduced && (sealed || !issue.details["gap"].is_null()))
        {
          report.issues.push_back(std::move(issue));
        }
      }
      if (!m_options.dryRun)
      {
        cache->leakKey = cache->changeCount;
        cache->leakSignatures = std::move(signatures);
        cache->leakAnyEnclosed = anyEnclosed(leaks);
      }
      if (elapsed > MaxPerCallLeakTime)
      {
        disableLeakChecks(fmt::format(
          "leak prediction took {} ms",
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
      }
    }
    else if (skippedForSize(leaks))
    {
      disableLeakChecks(leaks.skippedReason);
    }
  }

  report.warnings = std::move(m_warnings);
  return report;
}

} // namespace tb::mcp
