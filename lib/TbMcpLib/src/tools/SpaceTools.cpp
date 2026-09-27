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

#include "mcp/tools/SpaceTools.h"

#include "NodeJson.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Image.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/Map.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr auto MaxTextCells = size_t(200);

ToolError cancelledError()
{
  return makeError(ErrorCode::Cancelled, "The call was cancelled.");
}

/** A distance for the text description, rounded to whole units. */
std::string formatDistance(const double value)
{
  return fmt::format("{:.0f}", value);
}

std::string formatNumber(const double value)
{
  const auto rounded = roundForOutput(value);
  return rounded == std::floor(rounded) ? std::to_string(int64_t(rounded))
                                        : fmt::format("{}", rounded);
}

Json optionalNumber(const std::optional<double>& value)
{
  return value ? Json(roundForOutput(*value)) : Json(nullptr);
}

Json compactJson(const mdl::Node& node, const IdRegistry& ids)
{
  return Json{
    {"id", ids.format(node)},
    {"kind", std::string{toString(objectKindOf(node))}},
    {"label", nodeLabel(node)},
  };
}

SpaceOptions spaceOptions(const Args& args)
{
  auto options = SpaceOptions{};
  options.cellSize = args.getOr<double>("cellSize", 0.0);
  options.openingSize = args.getOr<double>("openingSize", 96.0);
  return options;
}

/**
 * Runs the space analysis in a deferred step, then `then` with the result in another
 * deferred step, reporting progress and honoring cancellation between the steps.
 */
void withSpaces(
  CallContext& context,
  const SpaceOptions& options,
  const ToolCompletion& completion,
  std::function<ToolResult(CallContext&, const SpaceMap&)> then,
  const double totalSteps = 2.0)
{
  context.progress(0.0, totalSteps, "Analyzing the empty space");
  context.defer([&context, options, completion, then = std::move(then), totalSteps]() {
    if (context.cancelled())
    {
      completion(cancelledError());
      return;
    }
    auto spaces = analyzeSpaces(context.map(), options);
    if (spaces.is_error())
    {
      completion(errorOf(spaces));
      return;
    }
    context.progress(1.0, totalSteps, "Describing the result");
    auto shared = std::make_shared<SpaceMap>(std::move(spaces).value());
    context.defer([&context, completion, then, shared, totalSteps]() {
      if (context.cancelled())
      {
        completion(cancelledError());
        return;
      }
      auto result = then(context, *shared);
      context.progress(totalSteps, totalSteps, "Done");
      completion(std::move(result));
    });
  });
}

Json heightStatsJson(const std::optional<HeightStats>& stats)
{
  if (!stats)
  {
    return nullptr;
  }
  return Json{
    {"min", roundForOutput(stats->min)},
    {"max", roundForOutput(stats->max)},
    {"typical", roundForOutput(stats->typical)},
  };
}

std::string openingId(const size_t index)
{
  return "opening:" + std::to_string(index);
}

Json openingJson(
  const SpaceMap& spaces,
  const size_t index,
  const OpeningDetails& details,
  const IdRegistry& ids)
{
  const auto& opening = spaces.openings[index];
  auto doors = Json::array();
  for (const auto* door : details.doors)
  {
    doors.push_back(ids.format(*door));
  }
  return Json{
    {"id", openingId(index)},
    {"kind", details.kind},
    {"spaces",
     Json::array(
       {spaces.spaces[opening.spaceA].id,
        opening.spaceB ? spaces.spaces[*opening.spaceB].id : std::string{"void"}})},
    {"center", toJson(details.center)},
    {"bounds", toJson(details.bounds)},
    {"width", roundForOutput(details.width)},
    {"height", roundForOutput(details.height)},
    {"bottom", roundForOutput(details.bottom)},
    {"normal", toJson(details.normal)},
    {"doors", std::move(doors)},
  };
}

// spaces_list

ToolResult spacesListResult(
  CallContext& context, const Args& args, const SpaceMap& spaces)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto region = args.getOptional<vm::bbox3d>("region");
  const auto full = args.getOr<std::string>("detail", "summary") == "full";
  const auto limit = size_t(args.getOr<int64_t>("limit", 100));
  const auto maxContents = size_t(50);

  auto selected = std::vector<size_t>{};
  for (size_t i = 0; i < spaces.spaces.size(); ++i)
  {
    const auto& bounds = spaces.spaces[i].bounds;
    if (
      !region
      || (bounds.min.x() < region->max.x() && bounds.max.x() > region->min.x()
          && bounds.min.y() < region->max.y() && bounds.max.y() > region->min.y()
          && bounds.min.z() < region->max.z() && bounds.max.z() > region->min.z()))
    {
      selected.push_back(i);
    }
  }
  const auto truncated = selected.size() > limit;
  if (truncated)
  {
    selected.resize(limit);
  }

  auto openingIndices = std::set<size_t>{};
  auto list = Json::array();
  for (const auto index : selected)
  {
    const auto& space = spaces.spaces[index];
    const auto details = describeSpace(map, spaces, index);

    auto neighbours = Json::array();
    for (const auto neighbour : space.neighbours)
    {
      neighbours.push_back(spaces.spaces[neighbour].id);
    }
    auto openings = Json::array();
    for (const auto opening : space.openings)
    {
      openings.push_back(openingId(opening));
      openingIndices.insert(opening);
    }

    auto classnames = std::map<std::string, size_t>{};
    auto counts = std::map<std::string, size_t>{
      {"pointEntities", 0}, {"brushEntities", 0}, {"groups", 0}, {"patches", 0}};
    auto contents = Json::array();
    for (const auto* node : details.contents)
    {
      if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node))
      {
        ++classnames[entityNode->entity().classname()];
        ++counts[entityNode->hasChildren() ? "brushEntities" : "pointEntities"];
      }
      else if (dynamic_cast<const mdl::GroupNode*>(node))
      {
        ++counts["groups"];
      }
      else
      {
        ++counts["patches"];
      }
      if (full && contents.size() < maxContents)
      {
        contents.push_back(compactJson(*node, ids));
      }
    }
    auto objects = Json(counts);
    objects["classnames"] = classnames;

    const auto size = space.bounds.size();
    auto item = Json{
      {"id", space.id},
      {"bounds", toJson(space.bounds)},
      {"size", toJson(size)},
      {"floor", heightStatsJson(details.floor)},
      {"ceiling", heightStatsJson(details.ceiling)},
      {"height",
       details.floor && details.ceiling
         ? Json(roundForOutput(details.ceiling->typical - details.floor->typical))
         : Json(nullptr)},
      {"floorArea", roundForOutput(details.floorArea)},
      {"volume", roundForOutput(space.volume)},
      {"sealed", space.sealed},
      {"openings", std::move(openings)},
      {"neighbours", std::move(neighbours)},
      {"layers", details.layers},
      {"groups", details.groups},
      {"objects", std::move(objects)},
    };
    if (full)
    {
      item["contents"] = std::move(contents);
      item["contentsTruncated"] = details.contents.size() > maxContents;
    }
    list.push_back(std::move(item));
  }

  auto openings = Json::array();
  auto outsideOpenings = Json::array();
  for (const auto index : openingIndices)
  {
    const auto details = describeOpening(map, spaces, index);
    openings.push_back(openingJson(spaces, index, details, ids));
    if (!spaces.openings[index].spaceB)
    {
      outsideOpenings.push_back(openingId(index));
    }
  }

  const auto unsealed = size_t(std::ranges::count_if(
    selected, [&](const auto index) { return !spaces.spaces[index].sealed; }));
  if (unsealed > 0)
  {
    context.warn(
      "SPACES_NOT_SEALED",
      fmt::format(
        "{} space(s) are connected to the void outside the map (see outsideOpenings).",
        unsealed));
  }

  return Json{
    {"cellSize", roundForOutput(spaces.grid.cellSize)},
    {"openingSize", roundForOutput(spaces.openingSize)},
    {"count", spaces.spaces.size()},
    {"spaces", std::move(list)},
    {"openings", std::move(openings)},
    {"outsideOpenings", std::move(outsideOpenings)},
    {"truncated", truncated},
  };
}

void spacesList(CallContext& context, const Args& args, ToolCompletion completion)
{
  withSpaces(
    context, spaceOptions(args), completion, [args](CallContext& c, const SpaceMap& s) {
      return spacesListResult(c, args, s);
    });
}

// surroundings

std::string compassDirection(const vm::vec3d& delta)
{
  const auto horizontal = std::hypot(delta.x(), delta.y());
  if (horizontal < 1.0)
  {
    return delta.z() > 1.0 ? "above" : delta.z() < -1.0 ? "below" : "here";
  }
  static const auto names =
    std::array<std::string_view, 8>{"E", "NE", "N", "NW", "W", "SW", "S", "SE"};
  auto angle = std::atan2(delta.y(), delta.x()) * 180.0 / std::numbers::pi;
  if (angle < 0.0)
  {
    angle += 360.0;
  }
  return std::string{names[size_t(std::lround(angle / 45.0)) % 8]};
}

Json rayHitJson(const RayHit& hit, const IdRegistry& ids)
{
  auto result = Json{
    {"distance", roundForOutput(hit.distance)},
    {"point", toJson(hit.point)},
    {"object", ids.format(*hit.node)},
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
  if (const auto* entityNode = owningBrushEntity(*hit.node))
  {
    result["entity"] = ids.format(*entityNode);
    result["classname"] = entityNode->entity().classname();
  }
  return result;
}

double distanceToBox(const vm::vec3d& point, const vm::bbox3d& box)
{
  auto sum = 0.0;
  for (size_t i = 0; i < 3; ++i)
  {
    const auto d = std::max({box.min[i] - point[i], 0.0, point[i] - box.max[i]});
    sum += d * d;
  }
  return std::sqrt(sum);
}

const mdl::Node* outermostGroup(const mdl::Node& node)
{
  const mdl::Node* result = nullptr;
  for (const auto* current = node.parent(); current; current = current->parent())
  {
    if (dynamic_cast<const mdl::GroupNode*>(current))
    {
      result = current;
    }
  }
  return result;
}

ToolResult surroundingsResult(
  CallContext& context, const Args& args, const SpaceMap* spaces)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto point = args.get<vm::vec3d>("point");
  const auto radius = args.getOr<double>("radius", 512.0);
  const auto limit = size_t(args.getOr<int64_t>("limit", 20));
  const auto diagonals = args.getOr<bool>("diagonals", true);
  const auto maxDistance = args.getOr<double>("maxDistance", 4096.0);

  const auto acceptSolid = [](const mdl::Node& node) {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      return brushRole(*brushNode).blocksObjects;
    }
    return dynamic_cast<const mdl::PatchNode*>(&node) != nullptr;
  };
  const auto firstHit = [&](const vm::vec3d& direction) -> std::optional<RayHit> {
    const auto hits = castRay(map, vm::ray3d{point, direction}, acceptSolid, maxDistance);
    return hits.empty() ? std::nullopt : std::optional{hits.front()};
  };

  auto result = Json{{"point", toJson(point)}};
  auto text = fmt::format(
    "Point ({} {} {})",
    formatNumber(point.x()),
    formatNumber(point.y()),
    formatNumber(point.z()));

  // the space
  auto spaceJson = Json(nullptr);
  auto where = std::string{"unknown"};
  if (spaces)
  {
    const auto label = spaces->labelAt(point);
    where = label >= 0                  ? "space"
            : label == SpaceMap::Solid  ? "solid"
            : label == SpaceMap::Pocket ? "pocket"
                                        : "void";
    if (const auto index = spaces->spaceAt(point, 1))
    {
      const auto& space = spaces->spaces[*index];
      spaceJson = Json{
        {"id", space.id},
        {"bounds", toJson(space.bounds)},
        {"sealed", space.sealed},
      };
      const auto size = space.bounds.size();
      text += fmt::format(
        " is in {} ({} x {} x {}{})",
        space.id,
        formatNumber(size.x()),
        formatNumber(size.y()),
        formatNumber(size.z()),
        space.sealed ? "" : ", not sealed");
      if (label < 0)
      {
        text += " (the point itself is in solid space)";
      }
    }
    else
    {
      text += where == "solid" ? " is inside solid geometry" : " is outside all spaces";
    }
  }
  result["space"] = std::move(spaceJson);
  result["inside"] = where;
  text += ".";

  // floor and ceiling
  const auto floor = firstHit({0, 0, -1});
  const auto ceiling = firstHit({0, 0, 1});
  result["floor"] = floor ? rayHitJson(*floor, ids) : Json(nullptr);
  result["ceiling"] = ceiling ? rayHitJson(*ceiling, ids) : Json(nullptr);
  if (floor)
  {
    result["floor"]["z"] = roundForOutput(floor->point.z());
    text += fmt::format(
      " Floor at z {} ({} below",
      formatNumber(floor->point.z()),
      formatDistance(floor->distance));
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(floor->node);
        brushNode && floor->faceIndex)
    {
      text += ", " + brushNode->brush().face(*floor->faceIndex).materialName();
    }
    text += ")";
  }
  else
  {
    text += " No floor below";
  }
  if (ceiling)
  {
    result["ceiling"]["z"] = roundForOutput(ceiling->point.z());
    text += fmt::format(
      ", ceiling at z {} ({} above).",
      formatNumber(ceiling->point.z()),
      formatDistance(ceiling->distance));
  }
  else
  {
    text += ", no ceiling above.";
  }

  // walls
  auto walls = Json::array();
  auto wallTexts = std::vector<std::string>{};
  const auto s = std::sqrt(0.5);
  auto directions = std::vector<std::pair<std::string, vm::vec3d>>{
    {"N", {0, 1, 0}}, {"E", {1, 0, 0}}, {"S", {0, -1, 0}}, {"W", {-1, 0, 0}}};
  if (diagonals)
  {
    directions.insert(directions.begin() + 1, {"NE", {s, s, 0}});
    directions.insert(directions.begin() + 3, {"SE", {s, -s, 0}});
    directions.insert(directions.begin() + 5, {"SW", {-s, -s, 0}});
    directions.push_back({"NW", {-s, s, 0}});
  }
  for (const auto& [name, direction] : directions)
  {
    const auto hit = firstHit(direction);
    auto item = hit ? rayHitJson(*hit, ids) : Json::object();
    item["direction"] = name;
    item["vector"] = toJson(direction);
    if (!hit)
    {
      item["distance"] = nullptr;
      wallTexts.push_back(name + " open");
    }
    else
    {
      auto wallText = name + " " + formatDistance(hit->distance);
      if (const auto* entityNode = owningBrushEntity(*hit->node))
      {
        wallText += " (" + entityNode->entity().classname() + ")";
      }
      else if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(hit->node);
               brushNode && hit->faceIndex)
      {
        wallText += " (" + brushNode->brush().face(*hit->faceIndex).materialName() + ")";
      }
      wallTexts.push_back(std::move(wallText));
    }
    walls.push_back(std::move(item));
  }
  result["walls"] = std::move(walls);
  text += " Walls: " + fmt::format("{}", fmt::join(wallTexts, ", ")) + ".";

  // nearby objects
  struct Nearby
  {
    const mdl::Node* node;
    vm::vec3d position;
    double distance;
  };
  auto nearby = std::vector<Nearby>{};
  auto seen = std::set<const mdl::Node*>{};
  const auto search = vm::bbox3d{point, point}.expand(radius);
  for (const auto* node : map.worldNode().nodeTree().find_intersectors(search))
  {
    const mdl::Node* object = nullptr;
    if (const auto* group = outermostGroup(*node))
    {
      object = group;
    }
    else if (isPointEntity(*node) || dynamic_cast<const mdl::PatchNode*>(node))
    {
      object = node;
    }
    else if (const auto* entityNode = owningBrushEntity(*node))
    {
      object = entityNode;
    }
    if (!object || !seen.insert(object).second)
    {
      continue;
    }
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(object);
    const auto position = entityNode && !entityNode->hasChildren()
                            ? entityNode->entity().origin()
                            : object->logicalBounds().center();
    const auto distance = entityNode && !entityNode->hasChildren()
                            ? vm::distance(point, position)
                            : distanceToBox(point, object->logicalBounds());
    if (distance <= radius)
    {
      nearby.push_back({object, position, distance});
    }
  }
  std::ranges::sort(
    nearby, [](const auto& lhs, const auto& rhs) { return lhs.distance < rhs.distance; });
  auto objects = Json::array();
  auto objectTexts = std::vector<std::string>{};
  for (const auto& item : nearby)
  {
    if (objects.size() >= limit)
    {
      break;
    }
    auto json = compactJson(*item.node, ids);
    const auto direction = compassDirection(item.position - point);
    json["position"] = toJson(item.position);
    json["distance"] = roundForOutput(item.distance);
    json["direction"] = direction;
    json["dz"] = roundForOutput(item.position.z() - point.z());
    objectTexts.push_back(fmt::format(
      "{} ({}) {} {}",
      nodeLabel(*item.node),
      ids.format(*item.node),
      formatDistance(item.distance),
      direction));
    objects.push_back(std::move(json));
  }
  result["objects"] = std::move(objects);
  result["objectsTruncated"] = nearby.size() > limit;
  text += objectTexts.empty()
            ? fmt::format(" No objects within {}.", formatNumber(radius))
            : " Nearby: " + fmt::format("{}", fmt::join(objectTexts, "; ")) + ".";
  result["description"] = std::move(text);
  return result;
}

void surroundings(CallContext& context, const Args& args, ToolCompletion completion)
{
  if (!args.getOr<bool>("includeSpace", true))
  {
    context.defer([&context, args, completion]() {
      if (context.cancelled())
      {
        completion(cancelledError());
        return;
      }
      completion(surroundingsResult(context, args, nullptr));
    });
    return;
  }
  withSpaces(
    context, spaceOptions(args), completion, [args](CallContext& c, const SpaceMap& s) {
      return surroundingsResult(c, args, &s);
    });
}

// free_spots

Json clearanceJson(const std::array<std::optional<double>, 6>& clearance)
{
  static const auto names =
    std::array<std::string_view, 6>{"-x", "+x", "-y", "+y", "down", "up"};
  auto result = Json::object();
  for (size_t i = 0; i < 6; ++i)
  {
    result[std::string{names[i]}] = optionalNumber(clearance[i]);
  }
  return result;
}

ToolResult freeSpotsResult(CallContext& context, const Args& args, const SpaceMap& spaces)
{
  auto& map = context.map();
  const auto& ids = context.ids();

  auto options = FreeSpotOptions{};
  options.size = args.get<vm::vec3d>("size");
  options.placement = placementFromString(args.getOr<std::string>("placement", "floor"))
                        .value_or(Placement::Floor);
  if (const auto spaceId = args.getOptional<std::string>("space"))
  {
    options.space = spaces.findSpace(*spaceId);
    if (!options.space)
    {
      return makeError(
        ErrorCode::ObjectNotFound,
        "Unknown space: " + *spaceId,
        "Space ids change when the geometry around a space changes; call spaces_list "
        "for the current ids.",
        {*spaceId});
    }
  }
  options.region = args.getOptional<vm::bbox3d>("region");
  options.includeOutside = args.getOr<bool>("includeOutside", false);
  options.wallDistance = args.getOr<double>("wallDistance", 0.0);
  options.objectDistance = args.getOr<double>("objectDistance", 0.0);
  options.step = args.getOr<double>("step", 0.0);
  options.support = args.getOr<double>("support", 1.0);
  options.rotate = args.getOr<bool>("rotate", true);
  options.heightAboveFloor = args.getOptional<double>("heightAboveFloor");
  options.limit = size_t(args.getOr<int64_t>("limit", 10));
  options.near = args.getOptional<vm::vec3d>("near");
  options.sort =
    args.getOr<std::string>("sort", options.near ? "near" : "spread") == "near"
      ? SpotSort::Near
      : SpotSort::Spread;
  if (options.sort == SpotSort::Near && !options.near)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "sort: \"near\" needs a near point.",
      "Pass near: [x, y, z].");
  }

  auto loader = std::make_shared<EntityModelLoader>(map);
  options.entityBounds =
    [loader](const mdl::EntityNode& entityNode) -> std::optional<vm::bbox3d> {
    auto state = resolveEntityModel(entityNode.entity(), *loader);
    return state.is_success() ? state.value().worldBounds() : std::nullopt;
  };

  auto found = findFreeSpots(map, &spaces, options);
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& spots = found.value();

  auto list = Json::array();
  for (const auto& spot : spots.spots)
  {
    auto item = Json{
      {"min", toJson(spot.box.min)},
      {"max", toJson(spot.box.max)},
      {"center", toJson(spot.box.center())},
      {"origin", toJson(spot.origin)},
      {"size", toJson(spot.box.size())},
      {"space", spot.space ? Json(spaces.spaces[*spot.space].id) : Json(nullptr)},
      {"clearance", clearanceJson(spot.clearance)},
      {"floor", optionalNumber(spot.floor)},
    };
    if (spot.wallBrush)
    {
      const auto& face = spot.wallBrush->brush().face(spot.wallFace);
      item["wall"] = Json{
        {"face", ids.formatFace(*spot.wallBrush, spot.wallFace)},
        {"brush", ids.format(*spot.wallBrush)},
        {"normal", toJson(spot.wallNormal)},
        {"material", face.materialName()},
        {"heightRange",
         spot.heightRange ? Json::array(
                              {roundForOutput((*spot.heightRange)[0]),
                               roundForOutput((*spot.heightRange)[1])})
                          : Json(nullptr)},
      };
      item["rotated"] = spot.rotated;
    }
    list.push_back(std::move(item));
  }
  if (list.empty())
  {
    context.warn(
      "NO_FREE_SPOT",
      "No free position was found for the box.",
      options.space ? std::vector<std::string>{spaces.spaces[*options.space].id}
                    : std::vector<std::string>{});
  }
  return Json{
    {"placement", args.getOr<std::string>("placement", "floor")},
    {"size", toJson(options.size)},
    {"spots", std::move(list)},
    {"count", spots.spots.size()},
    {"candidates", spots.candidates},
    {"step", roundForOutput(spots.step)},
    {"coarsened", spots.coarsened},
  };
}

void freeSpots(CallContext& context, const Args& args, ToolCompletion completion)
{
  const auto size = args.get<vm::vec3d>("size");
  if (!(size.x() > 0.0 && size.y() > 0.0 && size.z() > 0.0))
  {
    completion(makeError(
      ErrorCode::InvalidArgument,
      "The size must be positive on every axis.",
      "Pass the box size, e.g. [32, 32, 32]."));
    return;
  }
  withSpaces(
    context, spaceOptions(args), completion, [args](CallContext& c, const SpaceMap& s) {
      return freeSpotsResult(c, args, s);
    });
}

// walkable_plan

std::string_view walkLegend(const char c)
{
  switch (c)
  {
  case 'S':
    return "start";
  case '.':
    return "reachable (and the start can be reached again)";
  case 'v':
    return "reachable only by dropping down (no way back to the start)";
  case ',':
    return "walkable but not reachable from the start";
  case 'D':
    return "door (passable), reachable";
  case '-':
    return "floor without room for the player (low ceiling or too narrow)";
  case '#':
    return "blocked at the start's height";
  case 'o':
    return "walkable outside all spaces (e.g. a roof), not reachable";
  case ' ':
    return "no floor (void or outside)";
  default:
    return "";
  }
}

Rgba8 walkColor(const char c)
{
  switch (c)
  {
  case 'S':
    return {255, 255, 255, 255};
  case '.':
    return {70, 190, 90, 255};
  case 'v':
    return {230, 150, 40, 255};
  case ',':
    return {90, 120, 200, 255};
  case 'D':
    return {170, 110, 50, 255};
  case '-':
    return {140, 60, 60, 255};
  case '#':
    return {90, 90, 90, 255};
  case 'o':
    return {50, 60, 80, 255};
  default:
    return {20, 20, 20, 255};
  }
}

struct WalkRequest
{
  WalkOptions options;
  std::optional<std::array<double, 2>> heightRange;
  std::string format;
  size_t maxAreas = 20;
};

ToolResult walkableResult(
  CallContext& context,
  const WalkRequest& request,
  const WalkPlan& plan,
  const SpaceMap* spaces)
{
  const auto wantText = request.format != "image";
  const auto wantImage = request.format != "text";
  const auto inRange = [&](const WalkNode& node) {
    return !request.heightRange
           || (node.z >= (*request.heightRange)[0] && node.z <= (*request.heightRange)[1]);
  };
  // nodes outside all spaces, e.g. on roofs
  auto outside = std::vector<uint8_t>(plan.nodes.size(), 0);
  auto nodeSpace = std::vector<std::optional<size_t>>(plan.nodes.size());
  if (spaces)
  {
    for (size_t i = 0; i < plan.nodes.size(); ++i)
    {
      nodeSpace[i] =
        spaces->spaceAt(plan.position(plan.nodes[i]) + vm::vec3d{0, 0, 1}, 1);
      outside[i] = !nodeSpace[i];
    }
  }

  // classify the columns
  auto grid = std::vector<std::string>(plan.rows, std::string(plan.columns, ' '));
  auto reachableCells = size_t(0);
  auto oneWayCells = size_t(0);
  auto walkableCells = size_t(0);
  for (size_t row = 0; row < plan.rows; ++row)
  {
    for (size_t column = 0; column < plan.columns; ++column)
    {
      const auto index = column + row * plan.columns;
      auto c = plan.blocked[index] ? '#' : plan.cramped[index] ? '-' : ' ';
      auto rank = 0;
      for (const auto nodeIndex : plan.columnNodes[index])
      {
        const auto& node = plan.nodes[nodeIndex];
        if (!inRange(node))
        {
          continue;
        }
        const auto nodeChar = plan.startNode == nodeIndex        ? 'S'
                              : node.reachable && node.canReturn ? '.'
                              : node.reachable                   ? 'v'
                              : outside[nodeIndex]               ? 'o'
                                                                 : ',';
        const auto nodeRank = nodeChar == 'S'   ? 5
                              : nodeChar == '.' ? 4
                              : nodeChar == 'v' ? 3
                              : nodeChar == ',' ? 2
                                                : 1;
        if (nodeChar == 'o' && (c == '#' || c == '-'))
        {
          continue;
        }
        if (nodeRank > rank)
        {
          rank = nodeRank;
          c = nodeChar;
        }
      }
      if (plan.door[index] && (c == '.' || c == 'v'))
      {
        c = 'D';
      }
      // row 0 is the northernmost row
      grid[plan.rows - 1 - row][column] = c;
    }
  }
  auto outsideCells = size_t(0);
  for (size_t i = 0; i < plan.nodes.size(); ++i)
  {
    const auto& node = plan.nodes[i];
    if (!inRange(node))
    {
      continue;
    }
    if (outside[i] && !node.reachable)
    {
      ++outsideCells;
      continue;
    }
    ++walkableCells;
    if (node.reachable)
    {
      ++reachableCells;
      if (!node.canReturn)
      {
        ++oneWayCells;
      }
    }
  }
  const auto cellArea = plan.cellSize * plan.cellSize;

  // unreachable walkable areas: connected groups of unreachable nodes
  auto areas = Json::array();
  {
    auto visited = std::vector<uint8_t>(plan.nodes.size(), 0);
    struct Area
    {
      size_t cells;
      vm::bbox3d bounds;
      vm::vec3d position;
    };
    auto found = std::vector<Area>{};
    for (size_t start = 0; start < plan.nodes.size(); ++start)
    {
      if (
        visited[start] || plan.nodes[start].reachable || outside[start]
        || !inRange(plan.nodes[start]))
      {
        continue;
      }
      auto component = std::vector<size_t>{start};
      visited[start] = 1;
      for (size_t head = 0; head < component.size(); ++head)
      {
        const auto& node = plan.nodes[component[head]];
        const auto column = node.column % plan.columns;
        const auto row = node.column / plan.columns;
        for (const auto& [dx, dy] :
             std::array<std::array<int64_t, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
        {
          const auto cx = int64_t(column) + dx;
          const auto cy = int64_t(row) + dy;
          if (cx < 0 || cy < 0 || cx >= int64_t(plan.columns) || cy >= int64_t(plan.rows))
          {
            continue;
          }
          for (const auto other :
               plan.columnNodes[size_t(cx) + size_t(cy) * plan.columns])
          {
            if (
              !visited[other] && !plan.nodes[other].reachable && !outside[other]
              && inRange(plan.nodes[other])
              && std::abs(plan.nodes[other].z - node.z) <= plan.stepHeight)
            {
              visited[other] = 1;
              component.push_back(other);
            }
          }
        }
      }
      auto bounds = std::optional<vm::bbox3d>{};
      auto sum = vm::vec3d{0, 0, 0};
      for (const auto nodeIndex : component)
      {
        const auto position = plan.position(plan.nodes[nodeIndex]);
        const auto half = plan.cellSize / 2.0;
        const auto cell = vm::bbox3d{
          {position.x() - half, position.y() - half, position.z()},
          {position.x() + half, position.y() + half, position.z()}};
        bounds = bounds ? vm::merge(*bounds, cell) : cell;
        sum = sum + position;
      }
      const auto centroid = sum / double(component.size());
      auto position = plan.position(plan.nodes[component.front()]);
      for (const auto nodeIndex : component)
      {
        const auto candidate = plan.position(plan.nodes[nodeIndex]);
        if (
          vm::squared_distance(candidate, centroid)
          < vm::squared_distance(position, centroid))
        {
          position = candidate;
        }
      }
      found.push_back({component.size(), *bounds, position});
    }
    std::ranges::sort(
      found, [](const auto& lhs, const auto& rhs) { return lhs.cells > rhs.cells; });
    for (const auto& area : found)
    {
      if (areas.size() >= request.maxAreas)
      {
        break;
      }
      auto item = Json{
        {"cells", area.cells},
        {"area", roundForOutput(double(area.cells) * cellArea)},
        {"bounds", toJson(area.bounds)},
        {"position", toJson(area.position)},
      };
      if (
        const auto space =
          spaces ? spaces->spaceAt(area.position + vm::vec3d{0, 0, 1}, 1) : std::nullopt)
      {
        item["space"] = spaces->spaces[*space].id;
      }
      areas.push_back(std::move(item));
    }
  }

  auto spacesReached = Json::array();
  if (spaces)
  {
    auto reached = std::set<size_t>{};
    for (size_t i = 0; i < plan.nodes.size(); ++i)
    {
      if (plan.nodes[i].reachable && nodeSpace[i])
      {
        reached.insert(*nodeSpace[i]);
      }
    }
    for (const auto space : reached)
    {
      spacesReached.push_back(spaces->spaces[space].id);
    }
  }

  auto result = Json{
    {"origin", toJson(plan.origin)},
    {"cellSize", roundForOutput(plan.cellSize)},
    {"columns", plan.columns},
    {"rows", plan.rows},
    {"player",
     Json{
       {"width", roundForOutput(plan.playerWidth)},
       {"height", roundForOutput(plan.playerHeight)},
       {"stepHeight", roundForOutput(plan.stepHeight)},
       {"jumpHeight", roundForOutput(plan.jumpHeight)},
     }},
    {"start",
     plan.start
       ? Json{
           {"point", toJson(*plan.start)},
           {"floor",
            plan.startNode ? Json(toJson(plan.position(plan.nodes[*plan.startNode])))
                           : Json(nullptr)},
         }
       : Json(nullptr)},
    {"walkableCells", walkableCells},
    {"outsideCells", outsideCells},
    {"reachableCells", reachableCells},
    {"oneWayCells", oneWayCells},
    {"reachableArea", roundForOutput(double(reachableCells) * cellArea)},
    {"unreachableAreas", std::move(areas)},
    {"spacesReached", std::move(spacesReached)},
  };

  if (!plan.start)
  {
    context.warn(
      "NO_START",
      "There is no info_player_start or info_player_deathmatch; nothing is reachable.",
      {});
  }
  else if (!plan.startNode)
  {
    context.warn(
      "START_NOT_ON_FLOOR",
      "The player does not fit on a floor at the start; nothing is reachable.",
      {});
  }

  if (wantText)
  {
    auto rowLabels = std::vector<std::string>{};
    auto labelWidth = size_t(0);
    const auto topY = plan.origin.y() + double(plan.rows) * plan.cellSize;
    for (size_t row = 0; row < plan.rows; ++row)
    {
      rowLabels.push_back(formatNumber(topY - double(row + 1) * plan.cellSize));
      labelWidth = std::max(labelWidth, rowLabels.back().size());
    }
    const auto prefixWidth = labelWidth + 2;
    auto header = std::string(prefixWidth + plan.columns, ' ');
    auto nextFree = size_t(0);
    for (size_t column = 0; column < plan.columns; column += 8)
    {
      const auto label = formatNumber(plan.origin.x() + double(column) * plan.cellSize);
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
    for (size_t row = 0; row < plan.rows; ++row)
    {
      text += "\n" + std::string(labelWidth - rowLabels[row].size(), ' ') + rowLabels[row]
              + " |" + grid[row];
    }
    auto legend = Json::object();
    for (const auto c : std::string_view{"S.vD,-#o "})
    {
      if (std::ranges::any_of(
            grid, [&](const auto& line) { return line.find(c) != std::string::npos; }))
      {
        legend[std::string(1, c)] = walkLegend(c);
      }
    }
    result["text"] = std::move(text);
    result["legend"] = std::move(legend);
  }

  if (wantImage)
  {
    const auto scale =
      std::clamp(size_t(1024) / std::max(plan.columns, plan.rows), size_t(1), size_t(12));
    auto image =
      makeImage(plan.columns * scale, plan.rows * scale, Rgba8{20, 20, 20, 255});
    for (size_t row = 0; row < plan.rows; ++row)
    {
      for (size_t column = 0; column < plan.columns; ++column)
      {
        const auto color = walkColor(grid[row][column]);
        for (size_t py = row * scale; py < (row + 1) * scale; ++py)
        {
          for (size_t px = column * scale; px < (column + 1) * scale; ++px)
          {
            const auto offset = (py * image.width + px) * 4;
            std::copy(color.begin(), color.end(), image.pixels.begin() + long(offset));
          }
        }
      }
    }
    const auto png = encodePng(image);
    if (!png)
    {
      return makeError(ErrorCode::InternalError, "The plan image could not be encoded.");
    }
    context.addImage(*png, "image/png");
    result["image"] = Json{
      {"width", image.width},
      {"height", image.height},
      {"format", "png"},
      {"pixelsPerCell", scale},
    };
  }
  return result;
}

Result<WalkRequest, ToolError> walkRequest(CallContext& context, const Args& args)
{
  auto request = WalkRequest{};
  auto& options = request.options;
  options.region = args.getOptional<vm::bbox3d>("region");
  options.cellSize = args.getOr<double>("cellSize", 0.0);
  options.stepHeight = args.getOr<double>("stepHeight", DefaultStepHeight);
  options.jumpHeight = args.getOr<double>("jumpHeight", DefaultJumpHeight);
  options.playerWidth = args.getOr<double>("playerWidth", 0.0);
  options.playerHeight = args.getOr<double>("playerHeight", 0.0);
  options.start = args.getOptional<vm::vec3d>("start");
  request.format = args.getOr<std::string>("format", "text");
  request.maxAreas = size_t(args.getOr<int64_t>("maxAreas", 20));
  if (const auto range = args.getOptional<vm::vec2d>("heightRange"))
  {
    request.heightRange = std::array<double, 2>{range->x(), range->y()};
  }

  if (const auto from = args.getOptional<std::string>("from"))
  {
    auto node = context.ids().resolve(*from);
    if (node.is_error())
    {
      return errorOf(node);
    }
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node.value());
    options.start = entityNode && !entityNode->hasChildren()
                      ? entityNode->entity().origin()
                      : node.value()->logicalBounds().center();
  }

  // keep the text plan readable
  if (request.format != "image" && options.cellSize <= 0.0)
  {
    auto region = options.region;
    if (!region)
    {
      region = brushBounds(
        context.map(), [](const auto&, const auto& role) { return role.blocksPlayer; });
    }
    options.cellSize = defaultCellSize(context.map());
    if (region)
    {
      while (std::max(region->size().x(), region->size().y()) / options.cellSize
               > double(MaxTextCells - 2)
             && options.cellSize < 65536.0)
      {
        options.cellSize *= 2.0;
      }
    }
  }
  return request;
}

void walkablePlan(CallContext& context, const Args& args, ToolCompletion completion)
{
  auto request = walkRequest(context, args);
  if (request.is_error())
  {
    completion(errorOf(request));
    return;
  }
  const auto shared = std::make_shared<WalkRequest>(std::move(request).value());

  context.progress(0.0, 3.0, "Finding floors");
  context.defer([&context, args, completion, shared]() {
    if (context.cancelled())
    {
      completion(cancelledError());
      return;
    }
    auto plan = planWalk(context.map(), shared->options);
    if (plan.is_error())
    {
      completion(errorOf(plan));
      return;
    }
    const auto& value = plan.value();
    if (
      shared->format != "image"
      && (value.columns > MaxTextCells || value.rows > MaxTextCells))
    {
      completion(makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "The text plan would have {} x {} cells; at most {} x {} are allowed.",
          value.columns,
          value.rows,
          MaxTextCells,
          MaxTextCells),
        "Pass a larger cellSize or a smaller region, or format: \"image\"."));
      return;
    }
    auto sharedPlan = std::make_shared<WalkPlan>(std::move(plan).value());
    auto options = SpaceOptions{};
    options.openingSize = args.getOr<double>("openingSize", 96.0);
    withSpaces(
      context,
      options,
      completion,
      [shared, sharedPlan](CallContext& c, const SpaceMap& spaces) {
        return walkableResult(c, *shared, *sharedPlan, &spaces);
      });
  });
}

} // namespace

void registerSpaceTools(ToolRegistry& registry)
{
  const auto cellSizeField =
    field("cellSize", number().min(2).max(1024))
      .describe(
        "Edge length of the analysis cells (default: half the player width, 16 for "
        "Quake and Half-Life); larger cells are faster but coarser");
  const auto openingSizeField =
    field("openingSize", number().min(8).defaultsTo(96))
      .describe(
        "Openings whose smaller side is at most this size (doorways, windows) separate "
        "spaces; larger openings join them into one space");

  registry.add(
    ToolDef{"spaces_list"}
      .title("List Spaces")
      .description(
        "Lists the enclosed spaces (rooms) of the map, found by a flood fill of the "
        "empty volume at player-size resolution: world, func_group and func_detail "
        "brushes are solid (tool-only brushes such as clip, hint and trigger are not; "
        "doors are openings). Each space has a stable id (space:<hash of its bounds>, "
        "unchanged until the geometry around it changes), its inner bounds, floor and "
        "ceiling heights (min, max, typical), floor area, volume, sealed (false if it "
        "is connected to the void outside the map), openings, neighbours, the layers "
        "and groups inside and object counts. Openings are doorways (reaching the "
        "floor), windows or holes (in floors) with size, center, bottom height, the "
        "two space ids (\"void\" for openings to the outside) and func_door entities "
        "in them. detail: \"full\" also lists the objects inside each space. May take "
        "a moment on large maps; reports progress and can be cancelled. Example: "
        "{\"detail\": \"full\"}")
      .input(object({
        field("region", box()).describe("Only spaces whose bounds intersect this box"),
        cellSizeField,
        openingSizeField,
        field("detail", enumOf({"summary", "full"}).defaultsTo("summary"))
          .describe("full: also list the objects in each space (at most 50 each)"),
        field("limit", integer().min(1).max(1000).defaultsTo(100))
          .describe("Maximum number of spaces listed"),
      }))
      .output(object({
        field("cellSize", number()),
        field("openingSize", number()),
        field("count", integer()).describe("Number of spaces in the map"),
        field("spaces", array(any()))
          .describe(
            "{id, bounds, size, floor {min, max, typical}, ceiling, height, floorArea, "
            "volume, sealed, openings: [opening ids], neighbours: [space ids], layers, "
            "groups, objects {pointEntities, brushEntities, groups, patches, "
            "classnames}, contents (full)}"),
        field("openings", array(any()))
          .describe(
            "{id, kind: doorway | window | hole, spaces: [id, id | \"void\"], center, "
            "bounds, width, height, bottom, normal (from the first to the second "
            "space), doors: [entity ids]}; opening ids are only valid in this result"),
        field("outsideOpenings", array(string()))
          .describe("Ids of the openings to the void (leaks)"),
        field("truncated", boolean()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .asyncHandler(spacesList));

  registry.add(
    ToolDef{"surroundings"}
      .title("Surroundings")
      .description(
        "Describes what is around a point, as structured data and as a short text: the "
        "space it is in, the distance to the walls in the 4 compass directions (and the "
        "diagonals) with the hit face ids and materials, the floor and ceiling below "
        "and above, and nearby objects (point entities, brush entities, groups) with "
        "compass direction (+y is north, +x east) and distance. Example: {\"point\": "
        "[256, 192, 48], \"radius\": 256}")
      .input(object({
        field("point", vec3()).required().describe("The point to describe"),
        field("radius", number().min(0).defaultsTo(512))
          .describe("Objects within this distance are listed"),
        field("limit", integer().min(0).max(200).defaultsTo(20))
          .describe("Maximum number of objects listed"),
        field("diagonals", boolean().defaultsTo(true))
          .describe("Also cast rays to NE, SE, SW and NW"),
        field("maxDistance", number().min(1).defaultsTo(4096))
          .describe("Walls farther away count as open"),
        field("includeSpace", boolean().defaultsTo(true))
          .describe("Find the space of the point (runs the space analysis)"),
        cellSizeField,
        openingSizeField,
      }))
      .output(object({
        field("point", vec3()),
        field("space", any()).describe("{id, bounds, sealed} or null"),
        field("inside", string())
          .describe("space, solid (inside geometry), void, pocket or unknown"),
        field("floor", any())
          .describe(
            "{z, distance, point, object, label, face, material, normal} or null"),
        field("ceiling", any()).describe("Like floor, above the point"),
        field("walls", array(any()))
          .describe(
            "{direction, vector, distance (null: open), point, object, face, material, "
            "normal, entity, classname}"),
        field("objects", array(any()))
          .describe(
            "{id, kind, label, position, distance, direction, dz}, nearest first"),
        field("objectsTruncated", boolean()),
        field("description", string()).describe("The same as a short text"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .asyncHandler(surroundings));

  registry.add(
    ToolDef{"free_spots"}
      .title("Find Free Spots")
      .description(
        "Finds free positions for a box of the given size in a space (from "
        "spaces_list), a region or the whole map (only inside spaces unless "
        "includeOutside). placement: floor (standing on a floor; support = fraction of "
        "the 5 support rays that must hit), wall (the box's back side lies flat on an "
        "axis-aligned wall face; with rotate, x and y of the size are swapped for walls "
        "facing along x so that y is always the depth; returns the wall face id, "
        "normal and the range of bottom heights that fit), ceiling (hanging from it) "
        "or any (free volume). Every candidate is checked against the real brushes "
        "(touching surfaces are fine), point entities (with their model bounds) and "
        "patches. wallDistance and objectDistance keep distance to walls and to other "
        "objects (entities, brush entities, groups). Results are spread out, or "
        "nearest to `near`. Each spot has min/max, center, origin (bottom center, "
        "where a point entity of that size would go) and the clearance on each side. "
        "Example: {\"size\": [64, 4, 64], \"placement\": \"wall\", \"space\": "
        "\"space:1a2b3c4d\", \"heightAboveFloor\": 64}")
      .input(object({
        field("size", vec3()).required().describe("Box size [x, y, z]"),
        field(
          "placement", enumOf({"floor", "wall", "ceiling", "any"}).defaultsTo("floor")),
        field("space", string()).describe("A space id from spaces_list"),
        field("region", box()).describe("Only boxes inside this region"),
        field("includeOutside", boolean().defaultsTo(false))
          .describe("Also spots outside all spaces (e.g. on roofs)"),
        field("wallDistance", number().min(0).defaultsTo(0))
          .describe("Free distance to walls around the box"),
        field("objectDistance", number().min(0).defaultsTo(0))
          .describe("Free distance to other objects around the box"),
        field("step", number().min(1))
          .describe("Candidate positions are multiples of this (default 8, coarser for "
                    "large regions)"),
        field("support", number().min(0).max(1).defaultsTo(1))
          .describe("floor / ceiling: fraction of the support rays that must hit"),
        field("rotate", boolean().defaultsTo(true))
          .describe("wall: swap x and y of the size for walls facing along x"),
        field("heightAboveFloor", number())
          .describe("wall: preferred height of the box bottom above the floor"),
        field("limit", integer().min(1).max(100).defaultsTo(10)),
        field("sort", enumOf({"spread", "near"}))
          .describe("spread (default) or near (needs near)"),
        field("near", vec3()).describe("Prefer spots near this point"),
        cellSizeField,
        openingSizeField,
      }))
      .output(object({
        field("placement", string()),
        field("size", vec3()),
        field("spots", array(any()))
          .describe(
            "{min, max, center, origin, size, space, floor, clearance {-x, +x, -y, +y, "
            "down, up} (null: more than 1024), wall {face, brush, normal, material, "
            "heightRange}, rotated}"),
        field("count", integer()),
        field("candidates", integer()).describe("Number of candidate boxes checked"),
        field("step", number()),
        field("coarsened", boolean())
          .describe("Whether the step was enlarged because the region is large"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .asyncHandler(freeSpots));

  registry.add(
    ToolDef{"walkable_plan"}
      .title("Walkable Plan")
      .description(
        "Draws a top-down plan of where the player can stand and what can be reached "
        "from the start (default: info_player_start, then info_player_deathmatch; or "
        "start / from). A cell is walkable if the player's box (the game's player "
        "size) fits standing on a floor at the cell center; moves between neighbouring "
        "cells climb up to stepHeight (18), jump up to jumpHeight (45; use 63 for "
        "Half-Life crouch jumps) or drop down any height. Doors are passable, clip "
        "brushes and solid brush entities block, water is ignored (its bottom is "
        "walkable). Legend: S start, . reachable, v reachable only by dropping (no way "
        "back), D door, , walkable but unreachable, - floor without room for the "
        "player, # blocked at the start's height, o walkable outside all spaces (roofs), "
        "' ' no floor. North (+y) is up. "
        "Also lists unreachable walkable areas and the spaces reached; format image "
        "adds a PNG of the plan. Example: {\"format\": \"both\"}")
      .input(object({
        field("start", vec3()).describe("Start point (default: the player start)"),
        field(
          "from", objectId({ObjectKind::Entity, ObjectKind::Brush, ObjectKind::Group}))
          .describe("Start at this object (an entity's origin, else its bounds center)"),
        field("region", box()).describe("Area to analyze"),
        field("cellSize", number().min(2).max(1024))
          .describe("Plan cell size (default: half the player width, larger for big "
                    "maps so that the text fits 200 x 200 cells)"),
        field("heightRange", vec2())
          .describe("[min, max] floor heights to show, e.g. one level"),
        field("stepHeight", number().min(0).defaultsTo(18)),
        field("jumpHeight", number().min(0).defaultsTo(45)),
        field("playerWidth", number().min(1)).describe("Default: the game's player size"),
        field("playerHeight", number().min(1)),
        field("format", enumOf({"text", "image", "both"}).defaultsTo("text")),
        field("maxAreas", integer().min(0).max(200).defaultsTo(20))
          .describe("Maximum number of unreachable areas listed"),
        openingSizeField,
      }))
      .output(object({
        field("text", string()),
        field("legend", any()),
        field("origin", vec2()).describe("x, y of the first cell's min corner"),
        field("cellSize", number()),
        field("columns", integer()),
        field("rows", integer()),
        field("player", any()).describe("{width, height, stepHeight, jumpHeight}"),
        field("start", any()).describe("{point, floor} or null"),
        field("walkableCells", integer())
          .describe("Cells where the player can stand inside spaces (or reached)"),
        field("outsideCells", integer())
          .describe("Unreachable cells where the player could stand outside all spaces"),
        field("reachableCells", integer()),
        field("oneWayCells", integer()),
        field("reachableArea", number()).describe("Area of the reachable cells"),
        field("unreachableAreas", array(any()))
          .describe("{cells, area, bounds, position, space}, largest first"),
        field("spacesReached", array(string())),
        field("image", any()).describe("{width, height, format, pixelsPerCell}"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .asyncHandler(walkablePlan));
}

} // namespace tb::mcp
