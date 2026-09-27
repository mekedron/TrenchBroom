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

#include "mcp/tools/MapCheckTools.h"

#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Schema.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/PlacementChecks.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNode.h"
#include "mdl/Map.h"
#include "mdl/NodeTree.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

// Check names, in the order in which they run and their findings are listed.
constexpr auto PlacementCheckName = std::string_view{"placement"};
constexpr auto PlayerStartCheckName = std::string_view{"player_start"};
constexpr auto LinksCheckName = std::string_view{"links"};
constexpr auto MaterialsCheckName = std::string_view{"materials"};
constexpr auto RoomsCheckName = std::string_view{"rooms"};

const auto CheckNames = std::vector<std::string>{
  std::string{PlacementCheckName},
  std::string{PlayerStartCheckName},
  std::string{LinksCheckName},
  std::string{MaterialsCheckName},
  std::string{RoomsCheckName},
};

/** An entity without model floats if its box is more than this above the floor. */
constexpr auto FloatingThreshold = 16.0;
/** An origin is inside a brush if it is at least this deep inside all its faces. */
constexpr auto InsideDepth = 1.0;
/** Free spot searches for suggested moves per call (each costs a few milliseconds). */
constexpr auto MaxFreeSpotSearches = size_t(25);
/** Face and brush ids listed per MISSING_MATERIAL finding. */
constexpr auto MaxListedFaces = size_t(50);

const auto DefaultPlayerBounds = vm::bbox3d{{-16, -16, -24}, {16, 16, 32}};

std::string lower(const std::string_view str)
{
  auto result = std::string{str};
  std::ranges::transform(
    result, result.begin(), [](const unsigned char c) { return char(std::tolower(c)); });
  return result;
}

bool startsWith(const std::string_view str, const std::string_view prefix)
{
  return str.substr(0, prefix.size()) == prefix;
}

bool isOneOf(const std::string_view str, std::initializer_list<std::string_view> list)
{
  return std::ranges::find(list, str) != list.end();
}

/** The Levenshtein distance of two strings. */
size_t editDistance(const std::string_view a, const std::string_view b)
{
  auto previous = std::vector<size_t>(b.size() + 1);
  auto current = std::vector<size_t>(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
  {
    previous[j] = j;
  }
  for (size_t i = 1; i <= a.size(); ++i)
  {
    current[0] = i;
    for (size_t j = 1; j <= b.size(); ++j)
    {
      const auto substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[b.size()];
}

size_t commonPrefixLength(const std::string_view a, const std::string_view b)
{
  auto i = size_t(0);
  while (i < a.size() && i < b.size() && a[i] == b[i])
  {
    ++i;
  }
  return i;
}

/**
 * The candidate most similar to the name (case-insensitive): an equal name, else the
 * nearest by edit distance (at most max(2, length / 3)), else the one sharing the
 * longest prefix of at least minPrefix characters. Ties are broken alphabetically.
 */
std::optional<std::string> similarName(
  const std::string& name, const std::vector<std::string>& candidates, size_t minPrefix)
{
  const auto lowerName = lower(name);
  const auto maxDistance = std::max(size_t(2), lowerName.size() / 3);
  auto best = std::optional<std::string>{};
  // candidates within the distance first (nearest first), then by the longest prefix
  using Key = std::tuple<bool, size_t, size_t, std::string>;
  auto bestKey = std::optional<Key>{};
  for (const auto& candidate : candidates)
  {
    const auto lowerCandidate = lower(candidate);
    const auto distance = editDistance(lowerName, lowerCandidate);
    const auto prefix = commonPrefixLength(lowerName, lowerCandidate);
    const auto near = distance <= maxDistance;
    if (!near && prefix < minPrefix)
    {
      continue;
    }
    const auto key =
      Key{!near, near ? distance : size_t(0), near ? size_t(0) : ~prefix, candidate};
    if (!bestKey || key < *bestKey)
    {
      bestKey = key;
      best = candidate;
    }
  }
  return best;
}

/** "(x y z)" with rounded coordinates. */
std::string formatPoint(const vm::vec3d& point)
{
  return fmt::format(
    "({} {} {})",
    roundForOutput(point.x()),
    roundForOutput(point.y()),
    roundForOutput(point.z()));
}

Json vectorJson(const vm::vec3d& vector)
{
  return Json::array(
    {roundForOutput(vector.x()), roundForOutput(vector.y()), roundForOutput(vector.z())});
}

// Findings

struct Finding
{
  std::string code;
  /** "error", "warning" or "info". */
  std::string severity;
  std::string description;
  std::string objectId;
  std::vector<std::string> objectIds;
  std::optional<vm::vec3d> position = std::nullopt;
  Json details = Json::object();
  Json suggestedFix = nullptr;
  /** The last part of the finding id; the objectId if empty. */
  std::string key = {};
};

Json fixJson(std::string description, const std::string& tool, Json args)
{
  return Json{
    {"description", std::move(description)},
    {"tool", tool},
    {"args", std::move(args)},
  };
}

Json fixJson(std::string description)
{
  return Json{
    {"description", std::move(description)},
    {"tool", nullptr},
    {"args", nullptr},
  };
}

Json moveFix(std::string description, const std::string& id, const vm::vec3d& vector)
{
  return fixJson(
    std::move(description),
    "objects_move",
    Json{{"ids", Json::array({id})}, {"vector", vectorJson(vector)}});
}

// Scope

/** The objects the object-based checks look at; all objects if `nodes` is empty. */
struct Scope
{
  std::optional<std::unordered_set<const mdl::Node*>> nodes;

  bool contains(const mdl::Node& node) const { return !nodes || nodes->contains(&node); }
};

void collectDescendants(mdl::Node& node, std::unordered_set<const mdl::Node*>& result)
{
  result.insert(&node);
  for (auto* child : node.children())
  {
    collectDescendants(*child, result);
  }
}

Result<Scope, ToolError> resolveScope(
  CallContext& context, const std::optional<std::vector<std::string>>& ids)
{
  auto scope = Scope{};
  if (ids)
  {
    scope.nodes.emplace();
    for (const auto& id : *ids)
    {
      auto node = context.ids().resolve(id);
      if (node.is_error())
      {
        return errorOf(node);
      }
      collectDescendants(*node.value(), *scope.nodes);
    }
  }
  return scope;
}

void collectEntities(mdl::Node& node, std::vector<mdl::EntityNode*>& result)
{
  if (auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node))
  {
    result.push_back(entityNode);
  }
  for (auto* child : node.children())
  {
    collectEntities(*child, result);
  }
}

/** All entities (point and brush entities, not worldspawn) in tree order. */
std::vector<mdl::EntityNode*> allEntities(mdl::Map& map)
{
  auto result = std::vector<mdl::EntityNode*>{};
  collectEntities(map.worldNode(), result);
  return result;
}

std::vector<mdl::EntityNode*> pointEntitiesIn(mdl::Map& map, const Scope& scope)
{
  auto result = std::vector<mdl::EntityNode*>{};
  for (auto* entityNode : allEntities(map))
  {
    if (isPointEntity(*entityNode) && scope.contains(*entityNode))
    {
      result.push_back(entityNode);
    }
  }
  return result;
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

// Classes

/**
 * Should the entity stand on a floor (PlacementRule::standing, shared with the model
 * placement checks)?
 */
bool standingClass(const mdl::Map& map, const mdl::Entity& entity)
{
  return placementRule(map, entity).standing;
}

/**
 * Point entities without a body or a position-dependent effect: relays, counters,
 * managers, master switches, global state, messages and screen effects. The game never
 * tests them against solid geometry and they work anywhere, also inside a wall (the
 * Half-Life FGD gives them no model, sprite or size: multisource, multi_manager,
 * trigger_relay, trigger_auto, scripted_sentence, env_global, game_*, ...; Quake 2 and
 * Quake 3 relay, delay, message and score targets).
 */
bool logicClass(const std::string_view classname)
{
  return startsWith(classname, "game_")
         || isOneOf(
           classname,
           {"multisource",
            "multi_manager",
            "trigger_relay",
            "trigger_auto",
            "trigger_changetarget",
            "trigger_counter",
            "scripted_sentence",
            "env_global",
            "env_render",
            "env_fade",
            "env_message",
            "target_relay",
            "target_delay",
            "target_print",
            "target_score",
            "target_give",
            "target_remove_powerups",
            "target_kill",
            "target_help",
            "target_secret",
            "target_goal",
            "target_changelevel",
            "target_crosslevel_trigger",
            "target_crosslevel_target"});
}

/** Classes that do nothing unless another entity triggers them by name. */
bool needsTargetname(const std::string_view classname)
{
  return isOneOf(
           classname,
           {"trigger_relay",
            "trigger_counter",
            "multi_manager",
            "path_corner",
            "info_teleport_destination"})
         || (startsWith(classname, "target_") && !isOneOf(classname, {"target_location", "target_speaker", "target_cdaudio"}));
}

/** Classes that wait for a trigger when they have a name. */
bool waitsForTrigger(const std::string_view classname)
{
  return needsTargetname(classname)
         || isOneOf(
           classname,
           {"func_door", "func_door_rotating", "func_train", "target_speaker"});
}

// Geometry

/** The first material of the brushes' faces that is not a tool material. */
std::optional<std::string> sealingMaterial(
  const std::vector<const mdl::BrushNode*>& brushes)
{
  for (const auto* brushNode : brushes)
  {
    for (const auto& face : brushNode->brush().faces())
    {
      if (!isToolMaterial(face.materialName()))
      {
        return face.materialName();
      }
    }
  }
  return std::nullopt;
}

/** Whether the point lies at least `depth` inside all faces of the brush. */
bool insideBrush(const mdl::Brush& brush, const vm::vec3d& point, const double depth)
{
  if (!brush.bounds().contains(point))
  {
    return false;
  }
  return std::ranges::all_of(brush.faces(), [&](const auto& face) {
    return face.boundary().point_distance(point) <= -depth;
  });
}

std::vector<const mdl::BrushNode*> brushesContaining(
  const mdl::Map& map,
  const vm::vec3d& point,
  const std::function<bool(const BrushRole&)>& accept)
{
  auto result = std::vector<const mdl::BrushNode*>{};
  const auto box = vm::bbox3d{point - vm::vec3d{1, 1, 1}, point + vm::vec3d{1, 1, 1}};
  for (const auto* node : map.worldNode().nodeTree().find_intersectors(box))
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (
      brushNode && accept(brushRole(*brushNode))
      && insideBrush(brushNode->brush(), point, InsideDepth))
    {
      result.push_back(brushNode);
    }
  }
  return result;
}

/**
 * The free cells of the grid slice perpendicular to `axis` that are connected to `start`
 * within the slice, or nullopt if they reach the border of the grid (the region is not
 * bounded within the grid) or `start` is solid.
 */
std::optional<std::vector<CellIndex>> sliceRegion(
  const VoxelGrid& grid, const CellIndex& start, const size_t axis)
{
  if (grid.solid[grid.index(start)])
  {
    return std::nullopt;
  }
  const auto u = (axis + 1) % 3;
  const auto v = (axis + 2) % 3;
  auto result = std::vector<CellIndex>{start};
  auto visited = std::unordered_set<size_t>{grid.index(start)};
  for (size_t i = 0; i < result.size(); ++i)
  {
    const auto cell = result[i];
    for (const auto d : {u, v})
    {
      if (cell[d] == 0 || cell[d] + 1 >= grid.dims[d])
      {
        return std::nullopt;
      }
      for (const auto next : {cell[d] - 1, cell[d] + 1})
      {
        auto neighbour = cell;
        neighbour[d] = next;
        const auto index = grid.index(neighbour);
        if (!grid.solid[index] && visited.insert(index).second)
        {
          result.push_back(neighbour);
        }
      }
    }
  }
  return result;
}

/**
 * A box brush that seals a leak gap (two cells of the leak prediction's grid along the
 * path to the outside), or nullopt if the gap is no hole. The hole is the free region
 * around the gap in the slice perpendicular to the path that widens (or opens) further
 * inside, followed inwards over the wall's thickness; a missing wall has no such
 * constriction. The box covers the hole's cells plus one cell into the rim (the cells
 * are conservative, the real hole may be up to a cell larger) and is recessed by a unit
 * into the wall on both sides, so that it does not z-fight with the wall faces.
 */
std::optional<vm::bbox3d> gapSealBox(
  const mdl::Map& map, const vm::bbox3d& gap, const double cellSize)
{
  constexpr auto Radius = 16.0;
  auto grid = makeGrid(gap.expand(Radius * cellSize), cellSize, 0, 1'000'000);
  if (grid.is_error())
  {
    return std::nullopt;
  }
  auto voxels = std::move(grid).value();
  rasterize(voxels, map, [](const auto&, const BrushRole& role) { return role.seals; });

  const auto size = gap.size();
  const auto axis = size.x() >= size.y() && size.x() >= size.z() ? size_t(0)
                    : size.y() >= size.z()                       ? size_t(1)
                                                                 : size_t(2);
  const auto first = voxels.cellAt(gap.min + vm::vec3d{0.5, 0.5, 0.5} * cellSize);
  if (!first || size[axis] < 1.5 * cellSize || first->at(axis) + 1 >= voxels.dims[axis])
  {
    return std::nullopt;
  }
  auto second = *first;
  ++second[axis];

  // either cell may be the inner one; walk inwards from it
  for (const auto& [start, inwards] : {std::pair{*first, -1}, std::pair{second, 1}})
  {
    const auto base = sliceRegion(voxels, start, axis);
    if (!base)
    {
      continue;
    }
    auto bounds = voxels.cellBounds(base->front());
    for (const auto& cell : *base)
    {
      bounds = vm::merge(bounds, voxels.cellBounds(cell));
    }
    auto cell = start;
    for (size_t step = 1; step <= size_t(Radius); ++step)
    {
      if (
        (inwards < 0 && cell[axis] == 0)
        || (inwards > 0 && cell[axis] + 1 >= voxels.dims[axis]))
      {
        break;
      }
      cell[axis] = size_t(int64_t(cell[axis]) + inwards);
      if (voxels.solid[voxels.index(cell)])
      {
        break;
      }
      const auto region = sliceRegion(voxels, cell, axis);
      if (!region || region->size() > base->size())
      {
        // the hole opens into the room: seal the hole's slices
        auto box = bounds;
        for (size_t i = 0; i < 3; ++i)
        {
          if (i != axis)
          {
            box.min[i] -= cellSize;
            box.max[i] += cellSize;
          }
        }
        box = vm::bbox3d{vm::floor(box.min), vm::ceil(box.max)};
        if (box.max[axis] - box.min[axis] > 2.0)
        {
          box.min[axis] += 1.0;
          box.max[axis] -= 1.0;
        }
        return box;
      }
      for (const auto& other : *region)
      {
        bounds = vm::merge(bounds, voxels.cellBounds(other));
      }
    }
  }
  return std::nullopt;
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

/** The space nearest to the point, sealed spaces first. */
std::optional<size_t> nearestSpace(const SpaceMap& spaces, const vm::vec3d& point)
{
  auto result = std::optional<size_t>{};
  auto bestKey = std::pair{true, std::numeric_limits<double>::max()};
  for (size_t i = 0; i < spaces.spaces.size(); ++i)
  {
    const auto key =
      std::pair{!spaces.spaces[i].sealed, distanceToBox(point, spaces.spaces[i].bounds)};
    if (key < bestKey)
    {
      bestKey = key;
      result = i;
    }
  }
  return result;
}

// The run

struct CheckRun
{
  CallContext* context = nullptr;
  ToolCompletion completion;
  /** The checks to run, in canonical order. */
  std::vector<std::string> checks;
  std::optional<std::vector<std::string>> ids;
  SpaceOptions spaceOptions;
  PageRequest page;

  bool spacesTried = false;
  std::optional<SpaceMap> spaces;
  std::string spacesError;
  size_t freeSpotSearches = 0;

  std::map<std::string, std::vector<Finding>> findings;
  std::vector<std::string> checksRun;
  Json skipped = Json::array();

  /** The space analysis, run on first use; null if it failed. */
  const SpaceMap* ensureSpaces()
  {
    if (!spacesTried)
    {
      spacesTried = true;
      auto result = analyzeSpaces(context->map(), spaceOptions);
      if (result.is_success())
      {
        spaces = std::move(result).value();
        if (auto message = enlargedCellSizeMessage(*spaces))
        {
          context->warn("CELL_SIZE_ENLARGED", std::move(*message));
        }
      }
      else
      {
        spacesError = errorOf(result).message;
      }
    }
    return spaces ? &*spaces : nullptr;
  }

  void skip(const std::string_view check, std::string reason)
  {
    skipped.push_back(Json{{"check", check}, {"reason", std::move(reason)}});
  }
};

/**
 * The move that puts a box into the nearest free spot of the space containing it (or the
 * nearest space): standing on the floor, or anywhere in the free volume. Nullopt if the
 * space analysis failed, no spot was found or too many searches were made.
 */
std::optional<vm::vec3d> freeSpotMove(
  CheckRun& run, const vm::bbox3d& box, const Placement placement)
{
  if (run.freeSpotSearches >= MaxFreeSpotSearches)
  {
    return std::nullopt;
  }
  const auto* spaces = run.ensureSpaces();
  if (!spaces)
  {
    return std::nullopt;
  }
  ++run.freeSpotSearches;

  const auto center = box.center();
  auto space = spaces->spaceAt(center, 2);
  if (!space)
  {
    space = nearestSpace(*spaces, center);
  }
  if (!space)
  {
    return std::nullopt;
  }

  auto options = FreeSpotOptions{};
  options.size = box.size();
  options.placement = placement;
  options.space = space;
  options.sort = SpotSort::Near;
  options.near = center;
  options.limit = 1;
  const auto found = findFreeSpots(run.context->map(), spaces, options);
  if (found.is_error() || found.value().spots.empty())
  {
    return std::nullopt;
  }
  return found.value().spots.front().box.min - box.min;
}

// placement

Finding inSolidFinding(
  const std::string& id,
  const mdl::EntityNode& entityNode,
  const std::vector<std::string>& brushes,
  const std::optional<double> depthBelowFloor,
  const std::string& reason)
{
  const auto& entity = entityNode.entity();
  auto finding = Finding{};
  finding.code = "ENTITY_IN_SOLID";
  finding.severity = "error";
  finding.objectId = id;
  finding.objectIds = {id};
  finding.objectIds.insert(finding.objectIds.end(), brushes.begin(), brushes.end());
  finding.position = entity.origin();
  if (reason == "origin")
  {
    finding.description = fmt::format(
      "{} ({}) at {} is inside solid geometry: its origin lies inside {}. It will not "
      "work in the game (a light inside a wall lights nothing, a path corner inside a "
      "wall cannot be reached).",
      id,
      entity.classname(),
      formatPoint(entity.origin()),
      fmt::join(brushes, ", "));
  }
  else if (!brushes.empty())
  {
    finding.description = fmt::format(
      "{} ({}) is stuck in solid geometry: its bounding box intersects {}. It will be "
      "stuck in the game.",
      id,
      entity.classname(),
      fmt::join(brushes, ", "));
  }
  else
  {
    finding.description = fmt::format(
      "{} ({}) reaches {} units into the floor. It will be stuck in the game.",
      id,
      entity.classname(),
      roundForOutput(depthBelowFloor.value_or(0.0)));
  }
  finding.details = Json{
    {"classname", entity.classname()},
    {"origin", toJson(entity.origin())},
    {"bounds", toJson(entityNode.logicalBounds())},
    {"reason", reason},
    {"brushes", brushes},
  };
  if (depthBelowFloor)
  {
    finding.details["depthBelowFloor"] = roundForOutput(*depthBelowFloor);
  }
  return finding;
}

void checkPlacement(CheckRun& run, const Scope& scope)
{
  auto& map = run.context->map();
  const auto& ids = run.context->ids();
  auto& out = run.findings[std::string{PlacementCheckName}];
  auto loader = EntityModelLoader{map};

  for (auto* entityNode : pointEntitiesIn(map, scope))
  {
    const auto& entity = entityNode->entity();
    const auto& classname = entity.classname();
    const auto rule = placementRule(map, entity);
    if (rule.positionIndependent || logicClass(classname))
    {
      continue;
    }
    const auto id = ids.format(*entityNode);
    const auto origin = entity.origin();

    if (!rule.standing)
    {
      // only the origin matters
      const auto solid =
        brushesContaining(map, origin, [](const auto& role) { return role.spaceSolid; });
      if (!solid.empty())
      {
        auto brushes = std::vector<std::string>{};
        for (const auto* brushNode : solid)
        {
          brushes.push_back(ids.format(*brushNode));
        }
        std::ranges::sort(brushes);
        auto finding = inSolidFinding(id, *entityNode, brushes, std::nullopt, "origin");
        const auto move = freeSpotMove(run, entityNode->logicalBounds(), Placement::Any);
        finding.suggestedFix =
          move
            ? moveFix("Move it out of the brush to the nearest free position.", id, *move)
            : fixJson("Move it out of the brush (free_spots finds a free position).");
        out.push_back(std::move(finding));
      }
      continue;
    }

    // entities with a loadable model: the model placement checks (without the findings
    // the rule exempts)
    if (const auto state = resolveEntityModel(entity, loader);
        state.is_success() && state.value().worldBounds()
        && checksModelPlacement(rule, state.value()))
    {
      const auto bounds = *state.value().worldBounds();
      for (auto& issue : modelPlacementIssues(map, ids, {entityNode}, loader))
      {
        auto finding = Finding{};
        finding.code = issue.code;
        finding.severity = "warning";
        finding.description = issue.description;
        finding.objectId = id;
        finding.objectIds = issue.objectIds;
        finding.position = origin;
        finding.details = std::move(issue.details);
        if (
          const auto move = vec3FromJson(finding.details.value("suggestedMove", Json{})))
        {
          finding.suggestedFix = moveFix(
            issue.code == "MODEL_FLOATING" ? "Move it down onto the floor."
                                           : "Move it up onto the floor.",
            id,
            *move);
        }
        else if (issue.code == "MODEL_PENETRATES_BRUSHES")
        {
          const auto spotMove = freeSpotMove(run, bounds, Placement::Floor);
          finding.suggestedFix =
            spotMove
              ? moveFix("Move it to the nearest free floor spot.", id, *spotMove)
              : fixJson(
                  "Move it away from the brushes (free_spots finds a free floor spot "
                  "for its model bounds).");
        }
        else
        {
          finding.suggestedFix = fixJson(
            "Move it above a floor (find one with ray_pick, direction [0, 0, -1]) unless "
            "it is meant to fly.");
        }
        out.push_back(std::move(finding));
      }
      continue;
    }

    // other standing entities: the definition's bounding box
    const auto box = entityNode->logicalBounds();
    const auto check = checkModelPlacement(
      box,
      map,
      ids,
      id,
      fmt::format("The bounding box of {} ({})", id, classname),
      PlacementTolerance);
    const auto findingOf = [&](const std::string_view code) -> const PlacementFinding* {
      const auto it = std::ranges::find_if(
        check.findings, [&](const auto& finding) { return finding.code == code; });
      return it != check.findings.end() ? &*it : nullptr;
    };
    const auto* below = findingOf("MODEL_BELOW_FLOOR");
    const auto* penetrates = findingOf("MODEL_PENETRATES_BRUSHES");
    const auto* floating = findingOf("MODEL_FLOATING");
    const auto* noFloor = findingOf("MODEL_NO_FLOOR");

    if (below || penetrates)
    {
      auto brushes = std::vector<std::string>{};
      if (penetrates)
      {
        for (const auto& objectId : penetrates->objectIds)
        {
          if (objectId != id)
          {
            brushes.push_back(objectId);
          }
        }
      }
      auto finding = inSolidFinding(
        id,
        *entityNode,
        brushes,
        below ? below->distance : std::nullopt,
        penetrates ? "box" : "floor");
      finding.details["surface"] = placementSurfaceJson(check.surface, ids);
      if (!penetrates && below->suggestedMove)
      {
        finding.suggestedFix =
          moveFix("Move it up onto the floor.", id, *below->suggestedMove);
      }
      else
      {
        const auto move = freeSpotMove(run, box, Placement::Floor);
        finding.suggestedFix =
          move
            ? moveFix("Move it to the nearest free floor spot.", id, *move)
            : fixJson("Move it out of the brushes (free_spots finds a free floor spot).");
      }
      out.push_back(std::move(finding));
    }
    else if (
      !rule.mayFloat
      && ((floating && floating->distance.value_or(0.0) > FloatingThreshold) || noFloor))
    {
      auto finding = Finding{};
      finding.code = "ENTITY_FLOATING";
      finding.severity = "warning";
      finding.objectId = id;
      finding.objectIds = floating ? floating->objectIds : std::vector<std::string>{id};
      finding.position = origin;
      finding.details = Json{
        {"classname", classname},
        {"origin", toJson(origin)},
        {"bounds", toJson(box)},
        {"gap", floating ? Json(roundForOutput(*floating->distance)) : Json(nullptr)},
        {"surface", placementSurfaceJson(check.surface, ids)},
      };
      if (floating)
      {
        finding.description = fmt::format(
          "{} ({}) floats {} units above the floor at z={}. It should stand on the "
          "floor.",
          id,
          classname,
          roundForOutput(*floating->distance),
          roundForOutput(check.surface->z));
        finding.suggestedFix =
          moveFix("Move it down onto the floor.", id, *floating->suggestedMove);
      }
      else
      {
        finding.description = fmt::format(
          "{} ({}) has no floor below it. It should stand on a floor.", id, classname);
        const auto move = freeSpotMove(run, box, Placement::Floor);
        finding.suggestedFix =
          move ? moveFix("Move it to the nearest free floor spot.", id, *move)
               : fixJson("Move it above a floor (free_spots finds a free floor spot).");
      }
      out.push_back(std::move(finding));
    }
  }
}

// player_start

void checkPlayerStart(CheckRun& run)
{
  auto& map = run.context->map();
  const auto& ids = run.context->ids();
  auto& out = run.findings[std::string{PlayerStartCheckName}];
  const auto& definitionManager = map.entityDefinitionManager();

  auto startClasses = std::vector<std::string>{};
  auto gameDefinesSingle = false;
  for (const auto& definition : definitionManager.definitions())
  {
    if (
      startsWith(definition.name, "info_player_start")
      || startsWith(definition.name, "info_player_deathmatch")
      || startsWith(definition.name, "info_player_coop"))
    {
      startClasses.push_back(definition.name);
      gameDefinesSingle = gameDefinesSingle || definition.name == "info_player_start";
    }
  }
  const auto fromDefinitions = !startClasses.empty();
  if (!fromDefinitions)
  {
    startClasses = {"info_player_start", "info_player_deathmatch"};
  }
  std::ranges::sort(startClasses);

  auto starts = std::vector<const mdl::EntityNode*>{};
  auto singleStarts = size_t(0);
  for (const auto* entityNode : allEntities(map))
  {
    const auto& classname = entityNode->entity().classname();
    if (std::ranges::find(startClasses, classname) != startClasses.end())
    {
      starts.push_back(entityNode);
      singleStarts += classname == "info_player_start" ? size_t(1) : size_t(0);
    }
  }

  const auto worldId = ids.format(map.worldNode());
  if (starts.empty())
  {
    const auto classname =
      std::ranges::find(startClasses, "info_player_start") != startClasses.end()
        ? std::string{"info_player_start"}
        : startClasses.front();
    const auto* pointDefinition =
      mdl::getPointEntityDefinition(definitionManager.definition(classname));
    const auto bounds = pointDefinition ? pointDefinition->bounds : DefaultPlayerBounds;

    auto finding = Finding{};
    finding.code = "MISSING_PLAYER_START";
    finding.severity = "error";
    finding.description = fmt::format(
      "The map has no player start ({}), so the game cannot spawn the player.",
      fmt::join(startClasses, ", "));
    finding.objectId = worldId;
    finding.objectIds = {worldId};
    finding.key = "map";
    finding.details = Json{
      {"startClasses", startClasses},
      {"fromDefinitions", fromDefinitions},
      {"classname", classname},
    };

    // a free floor spot near the center of the largest sealed space
    auto position = std::optional<vm::vec3d>{};
    if (const auto* spaces = run.ensureSpaces(); spaces && !spaces->spaces.empty())
    {
      auto order = std::vector<size_t>(spaces->spaces.size());
      for (size_t i = 0; i < order.size(); ++i)
      {
        order[i] = i;
      }
      std::ranges::sort(order, [&](const auto lhs, const auto rhs) {
        const auto& a = spaces->spaces[lhs];
        const auto& b = spaces->spaces[rhs];
        return std::tuple{!a.sealed, -a.volume, a.id}
               < std::tuple{!b.sealed, -b.volume, b.id};
      });
      for (size_t i = 0; i < order.size() && i < 3 && !position; ++i)
      {
        auto options = FreeSpotOptions{};
        options.size = bounds.size();
        options.placement = Placement::Floor;
        options.space = order[i];
        options.sort = SpotSort::Near;
        options.near = spaces->spaces[order[i]].bounds.center();
        options.limit = 1;
        const auto found = findFreeSpots(map, spaces, options);
        if (found.is_success() && !found.value().spots.empty())
        {
          const auto& box = found.value().spots.front().box;
          position =
            vm::vec3d{box.center().x(), box.center().y(), box.min.z() - bounds.min.z()};
          finding.details["space"] = spaces->spaces[order[i]].id;
        }
      }
    }
    if (position)
    {
      finding.suggestedFix = fixJson(
        fmt::format("Create an {} entity on a free floor spot.", classname),
        "entity_create_point",
        Json{
          {"classname", classname},
          {"position", vectorJson(*position)},
          {"dropToFloor", true},
        });
    }
    else
    {
      finding.suggestedFix = fixJson(fmt::format(
        "Create a {} with entity_create_point (free_spots finds a free floor spot).",
        classname));
    }
    out.push_back(std::move(finding));
  }
  else if (singleStarts == 0 && gameDefinesSingle)
  {
    const auto* first = starts.front();
    auto startIds = std::vector<std::string>{};
    for (const auto* start : starts)
    {
      startIds.push_back(ids.format(*start));
    }

    auto finding = Finding{};
    finding.code = "MISSING_SINGLE_PLAYER_START";
    finding.severity = "info";
    finding.description = fmt::format(
      "The map has no info_player_start, only {} other player start(s) ({}); the "
      "single player game needs an info_player_start.",
      starts.size(),
      first->entity().classname());
    finding.objectId = worldId;
    finding.objectIds = startIds;
    finding.position = first->entity().origin();
    finding.key = "map";
    finding.details = Json{{"starts", startIds}};
    finding.suggestedFix = fixJson(
      fmt::format("Create an info_player_start where {} is.", startIds.front()),
      "entity_create_point",
      Json{
        {"classname", "info_player_start"},
        {"position", vectorJson(first->entity().origin())},
      });
    out.push_back(std::move(finding));
  }
}

// links

struct LinkKeys
{
  /** Keys whose values name other entities (target, killtarget, ...). */
  std::vector<std::string> source;
  /** Keys that name the entity (targetname). */
  std::vector<std::string> target;
};

LinkKeys linkKeys(const mdl::Entity& entity)
{
  const auto* definition = entity.definition();
  if (!definition)
  {
    return {{"target", "killtarget"}, {"targetname"}};
  }
  auto keys = LinkKeys{};
  for (const auto* property : mdl::getLinkSourcePropertyDefinitions(definition))
  {
    keys.source.push_back(property->key);
  }
  for (const auto* property : mdl::getLinkTargetPropertyDefinitions(definition))
  {
    keys.target.push_back(property->key);
  }
  return keys;
}

/**
 * The target names of a Half-Life multi_manager: its keys other than the standard ones
 * and those of its definition, with the "#n" suffix of duplicate keys removed. Returns
 * {key, name} pairs.
 */
std::vector<std::pair<std::string, std::string>> multiManagerTargets(
  const mdl::Entity& entity)
{
  auto result = std::vector<std::pair<std::string, std::string>>{};
  if (entity.classname() != "multi_manager")
  {
    return result;
  }
  for (const auto& property : entity.properties())
  {
    const auto& key = property.key();
    if (
      key.empty() || key.front() == '_'
      || isOneOf(
        key,
        {"classname", "targetname", "origin", "spawnflags", "wait", "angle", "angles"})
      || mdl::getPropertyDefinition(entity.definition(), key))
    {
      continue;
    }
    auto name = key;
    if (const auto hash = name.rfind('#'); hash != std::string::npos && hash > 0)
    {
      const auto suffix = std::string_view{name}.substr(hash + 1);
      if (!suffix.empty() && std::ranges::all_of(suffix, [](const unsigned char c) {
            return std::isdigit(c);
          }))
      {
        name.resize(hash);
      }
    }
    result.emplace_back(key, name);
  }
  return result;
}

std::string uniqueName(
  const std::string& classname, const std::unordered_set<std::string>& names)
{
  for (size_t i = 1;; ++i)
  {
    auto name = fmt::format("{}_{}", classname, i);
    if (!names.contains(name))
    {
      return name;
    }
  }
}

void checkLinks(CheckRun& run, const Scope& scope)
{
  auto& map = run.context->map();
  const auto& ids = run.context->ids();
  auto& out = run.findings[std::string{LinksCheckName}];

  const auto entities = allEntities(map);

  // the names of all entities and all values that may refer to a name
  auto names = std::unordered_set<std::string>{};
  auto referenced = std::unordered_set<std::string>{};
  for (const auto* entityNode : entities)
  {
    const auto& entity = entityNode->entity();
    auto nameKeys = std::unordered_set<std::string>{};
    for (const auto& key : linkKeys(entity).target)
    {
      for (const auto& property : entity.numberedProperties(key))
      {
        nameKeys.insert(property.key());
        if (!property.value().empty())
        {
          names.insert(property.value());
        }
      }
    }
    for (const auto& property : entity.properties())
    {
      if (property.key() != "classname" && !nameKeys.contains(property.key()))
      {
        referenced.insert(property.value());
      }
    }
    for (const auto& [key, name] : multiManagerTargets(entity))
    {
      referenced.insert(name);
    }
  }
  auto sortedNames = std::vector<std::string>{names.begin(), names.end()};
  std::ranges::sort(sortedNames);

  const auto missingTarget = [&](
                               const std::string& id,
                               const mdl::Entity& entity,
                               const std::string& key,
                               const std::string& name,
                               const bool multiManagerKey) {
    const auto similar = similarName(name, sortedNames, size_t(-1));
    auto finding = Finding{};
    finding.code = "LINK_TARGET_MISSING";
    finding.severity = "warning";
    finding.description = fmt::format(
      "{} ({}) {} '{}' ({}), but no entity has that name{}",
      id,
      entity.classname(),
      multiManagerKey ? "triggers" : "targets",
      name,
      multiManagerKey ? fmt::format("multi_manager key '{}'", key)
                      : fmt::format("key '{}'", key),
      similar ? fmt::format("; did you mean '{}'?", *similar) : std::string{"."});
    finding.objectId = id;
    finding.objectIds = {id};
    finding.position = entity.origin();
    finding.key = id + ":" + key;
    finding.details = Json{
      {"classname", entity.classname()},
      {"key", key},
      {"name", name},
      {"similar", similar ? Json(*similar) : Json(nullptr)},
    };
    if (similar && !multiManagerKey && lower(*similar) == lower(name))
    {
      finding.suggestedFix = fixJson(
        fmt::format("Point {} to the existing name '{}'.", key, *similar),
        "entity_properties_set",
        Json{{"ids", Json::array({id})}, {"properties", Json{{key, *similar}}}});
    }
    else
    {
      finding.suggestedFix = fixJson(
        fmt::format(
          "Remove the broken link, or give the intended entity the name '{}' "
          "(entity_properties_set with targetname, or entity_link).",
          name),
        "entity_property_remove",
        Json{{"ids", Json::array({id})}, {"keys", Json::array({key})}});
    }
    out.push_back(std::move(finding));
  };

  for (auto* entityNode : entities)
  {
    if (!scope.contains(*entityNode))
    {
      continue;
    }
    const auto& entity = entityNode->entity();
    const auto& classname = entity.classname();
    const auto id = ids.format(*entityNode);
    const auto keys = linkKeys(entity);

    // LINK_TARGET_MISSING
    for (const auto& key : keys.source)
    {
      for (const auto& property : entity.numberedProperties(key))
      {
        const auto& name = property.value();
        if (!name.empty() && name.front() != '!' && !names.contains(name))
        {
          missingTarget(id, entity, property.key(), name, false);
        }
      }
    }
    for (const auto& [key, name] : multiManagerTargets(entity))
    {
      if (!names.contains(name))
      {
        missingTarget(id, entity, key, name, true);
      }
    }

    // the entity's own name
    auto nameKey = std::optional<std::string>{};
    auto name = std::optional<std::string>{};
    const auto targetKeys =
      keys.target.empty() ? std::vector<std::string>{"targetname"} : keys.target;
    for (const auto& key : targetKeys)
    {
      for (const auto& property : entity.numberedProperties(key))
      {
        if (!property.value().empty() && !name)
        {
          nameKey = property.key();
          name = property.value();
        }
      }
    }

    // LINK_SOURCE_MISSING
    if (name && waitsForTrigger(classname) && !referenced.contains(*name))
    {
      const auto isDoor = startsWith(classname, "func_door");
      auto finding = Finding{};
      finding.code = "LINK_SOURCE_MISSING";
      finding.severity = "warning";
      finding.description = fmt::format(
        "{} ({}) is named '{}', but no entity triggers it, so it {}.",
        id,
        classname,
        *name,
        isDoor ? "stays locked (a named door only opens when triggered)"
               : "never does anything");
      finding.objectId = id;
      finding.objectIds = {id};
      finding.position = entityNode->logicalBounds().center();
      finding.key = id;
      finding.details = Json{
        {"classname", classname},
        {"key", *nameKey},
        {"name", *name},
      };
      finding.suggestedFix =
        isDoor ? fixJson(
                   "Remove the name so that the door opens when touched, or trigger it "
                   "from a button or trigger (entity_link).",
                   "entity_property_remove",
                   Json{{"ids", Json::array({id})}, {"keys", Json::array({*nameKey})}})
               : fixJson(fmt::format(
                   "Trigger it from a trigger or button: entity_link with source "
                   "<trigger id> and target {}, or delete it.",
                   id));
      out.push_back(std::move(finding));
    }

    // NEEDS_TARGETNAME
    if (!name && needsTargetname(classname))
    {
      const auto key = targetKeys.front();
      const auto suggested = uniqueName(classname, names);
      auto finding = Finding{};
      finding.code = "NEEDS_TARGETNAME";
      finding.severity = "warning";
      finding.description = fmt::format(
        "{} ({}) has no {}; it only acts when triggered by name, so it can never act.",
        id,
        classname,
        key);
      finding.objectId = id;
      finding.objectIds = {id};
      finding.position = entityNode->logicalBounds().center();
      finding.key = id;
      finding.details = Json{
        {"classname", classname},
        {"key", key},
        {"suggestedName", suggested},
      };
      finding.suggestedFix = fixJson(
        "Give it a name, then trigger it from a trigger or button (entity_link), or "
        "delete it.",
        "entity_properties_set",
        Json{{"ids", Json::array({id})}, {"properties", Json{{key, suggested}}}});
      out.push_back(std::move(finding));
    }
  }
}

// materials

struct MaterialUse
{
  std::vector<std::string> faces;
  std::vector<std::string> brushes;
  size_t faceCount = 0;
};

void checkMaterials(CheckRun& run, const Scope& scope)
{
  auto& map = run.context->map();
  const auto& ids = run.context->ids();
  auto& out = run.findings[std::string{MaterialsCheckName}];

  auto brushNodes = std::vector<mdl::BrushNode*>{};
  collectBrushes(map.worldNode(), brushNodes);

  auto uses = std::map<std::string, MaterialUse>{};
  for (const auto* brushNode : brushNodes)
  {
    if (!scope.contains(*brushNode))
    {
      continue;
    }
    const auto& brush = brushNode->brush();
    auto brushListed = std::set<std::string>{};
    for (size_t i = 0; i < brush.faceCount(); ++i)
    {
      const auto& face = brush.face(i);
      const auto& name = face.materialName();
      if (name.empty() || name == mdl::BrushFace::NoMaterialName || face.material())
      {
        continue;
      }
      auto& use = uses[name];
      ++use.faceCount;
      if (use.faces.size() < MaxListedFaces)
      {
        use.faces.push_back(ids.formatFace(*brushNode, i));
      }
      const auto brushId = ids.format(*brushNode);
      if (brushListed.insert(brushId).second && use.brushes.size() < MaxListedFaces)
      {
        use.brushes.push_back(brushId);
      }
    }
  }
  if (uses.empty())
  {
    return;
  }

  auto loaded = std::vector<std::string>{};
  for (const auto* material : map.materialManager().materials())
  {
    loaded.push_back(material->name());
  }

  for (const auto& [name, use] : uses)
  {
    const auto similar = similarName(name, loaded, 4);
    const auto toolMaterial = isToolMaterial(name);

    auto finding = Finding{};
    finding.code = "MISSING_MATERIAL";
    finding.severity = toolMaterial ? "info" : "warning";
    finding.description = fmt::format(
      "{} face(s) of {} brush(es) use the material '{}', which is not in any loaded "
      "material collection (WAD or folder); {}.",
      use.faceCount,
      use.brushes.size(),
      name,
      toolMaterial ? "compilers recognize this tool material by its name, but the "
                     "editor cannot show it"
                   : "the editor shows it as missing and the compiler cannot use it");
    finding.objectId = use.faces.front();
    finding.objectIds = use.faces;
    finding.key = name;
    finding.details = Json{
      {"material", name},
      {"faceCount", use.faceCount},
      {"faces", use.faces},
      {"facesTruncated", use.faceCount > use.faces.size()},
      {"brushes", use.brushes},
      {"similar", similar ? Json(*similar) : Json(nullptr)},
      {"loadedMaterials", loaded.size()},
    };

    if (similar && name.find_first_of("*?") == std::string::npos)
    {
      auto args = Json{{"from", name}, {"to", *similar}};
      if (scope.nodes)
      {
        args["ids"] = *run.ids;
      }
      else
      {
        args["scope"] = "map";
      }
      finding.suggestedFix = fixJson(
        fmt::format(
          "Replace it with the loaded material '{}', or add the collection that "
          "contains '{}' (materials_collections_set).",
          *similar,
          name),
        "material_replace",
        std::move(args));
    }
    else if (similar)
    {
      finding.suggestedFix = fixJson(
        fmt::format(
          "Apply the loaded material '{}' to the faces, or add the collection that "
          "contains '{}' (materials_collections_set).",
          *similar,
          name),
        "material_apply",
        Json{{"material", *similar}, {"ids", use.faces}});
    }
    else
    {
      finding.suggestedFix = fixJson(fmt::format(
        "Add the WAD or material collection that contains '{}' "
        "(materials_collections_get / materials_collections_set), or replace it with a "
        "loaded material (materials_list, material_replace).",
        name));
    }
    out.push_back(std::move(finding));
  }
}

// rooms

void checkRooms(CheckRun& run, const Scope& scope)
{
  auto& map = run.context->map();
  const auto& ids = run.context->ids();
  auto& out = run.findings[std::string{RoomsCheckName}];

  // ENTITY_OUTSIDE_HULL
  const auto report = predictLeaks(map);
  if (!report.analyzed)
  {
    run.skip(RoomsCheckName, "Leak prediction skipped: " + report.skippedReason);
  }
  auto outside = std::unordered_set<const mdl::Node*>{};
  auto entityOf = std::unordered_map<std::string, const LeakFinding*>{};
  for (const auto& leak : report.findings)
  {
    entityOf[ids.format(*leak.entity)] = &leak;
  }
  for (auto& issue : leakIssues(report, ids))
  {
    const auto it = entityOf.find(issue.objectId);
    if (it == entityOf.end() || !scope.contains(*it->second->entity))
    {
      continue;
    }
    const auto& leak = *it->second;
    outside.insert(leak.entity);

    auto finding = Finding{};
    finding.code = "ENTITY_OUTSIDE_HULL";
    finding.severity = "error";
    finding.description = issue.description;
    finding.objectId = issue.objectId;
    finding.objectIds = issue.objectIds;
    finding.position = leak.position;
    finding.details = std::move(issue.details);
    if (leak.gap)
    {
      auto brushes = std::vector<std::string>{};
      for (const auto* brushNode : leak.gapBrushes)
      {
        brushes.push_back(ids.format(*brushNode));
      }
      if (const auto box = gapSealBox(map, *leak.gap, report.cellSize))
      {
        auto args = Json{{"min", vectorJson(box->min)}, {"max", vectorJson(box->max)}};
        if (const auto material = sealingMaterial(leak.gapBrushes))
        {
          args["material"] = *material;
        }
        finding.suggestedFix = fixJson(
          fmt::format(
            "Seal the hole between the brushes {} with a box brush from {} to {} (the "
            "compiler reports a leak); it overlaps the rim of the hole. Alternatively "
            "move the entity into a sealed room.",
            fmt::join(brushes, ", "),
            formatPoint(box->min),
            formatPoint(box->max)),
          "brush_create_box",
          std::move(args));
      }
      else
      {
        // no single call fixes it
        finding.description += fmt::format(
          " The gap from {} to {} is no small hole (e.g. a missing wall), so there is "
          "no suggested fix: close the room with brushes (brush_create_box) or move "
          "the entity into a sealed room.",
          formatPoint(leak.gap->min),
          formatPoint(leak.gap->max));
      }
    }
    else if (
      const auto move = freeSpotMove(
        run,
        leak.entity->logicalBounds(),
        standingClass(map, leak.entity->entity()) ? Placement::Floor : Placement::Any))
    {
      finding.suggestedFix =
        moveFix("Move it into the nearest room.", issue.objectId, *move);
    }
    else
    {
      // no single call fixes it
      finding.description +=
        " No free position inside a room was found for a suggested move; build a "
        "sealed room around it or move it into one (free_spots finds a free position).";
    }
    out.push_back(std::move(finding));
  }

  // ENTITY_OUTSIDE_SPACES
  const auto* spaces = run.ensureSpaces();
  if (!spaces)
  {
    run.skip(RoomsCheckName, "Space analysis failed: " + run.spacesError);
    return;
  }
  for (auto* entityNode : pointEntitiesIn(map, scope))
  {
    const auto& entity = entityNode->entity();
    if (outside.contains(entityNode) || placementRule(map, entity).positionIndependent)
    {
      continue;
    }
    const auto origin = entity.origin();
    if (spaces->region && !spaces->region->contains(origin))
    {
      continue;
    }
    const auto label = spaces->labelAt(origin);
    if (label >= 0 || spaces->spaceAt(origin, 1))
    {
      continue;
    }
    const auto where = label == SpaceMap::Solid    ? "solid"
                       : label == SpaceMap::Pocket ? "pocket"
                                                   : "void";
    const auto id = ids.format(*entityNode);
    const auto nearest = nearestSpace(*spaces, origin);

    auto finding = Finding{};
    finding.code = "ENTITY_OUTSIDE_SPACES";
    finding.severity = "warning";
    finding.description = fmt::format(
      "{} ({}) at {} is not inside any room found by the space analysis: it is {}.",
      id,
      entity.classname(),
      formatPoint(origin),
      label == SpaceMap::Solid ? "inside solid geometry"
      : label == SpaceMap::Pocket
        ? "in a small enclosed pocket (smaller than the analysis resolution)"
        : "outside the enclosed rooms");
    finding.objectId = id;
    finding.objectIds = {id};
    finding.position = origin;
    finding.details = Json{
      {"classname", entity.classname()},
      {"position", toJson(origin)},
      {"where", where},
      {"nearestSpace",
       nearest ? Json{
                   {"id", spaces->spaces[*nearest].id},
                   {"bounds", toJson(spaces->spaces[*nearest].bounds)},
                   {"distance",
                    roundForOutput(distanceToBox(origin, spaces->spaces[*nearest].bounds))},
                 }
               : Json(nullptr)},
    };
    if (nearest)
    {
      finding.objectIds.push_back(spaces->spaces[*nearest].id);
    }
    const auto move = freeSpotMove(
      run,
      entityNode->logicalBounds(),
      standingClass(map, entity) ? Placement::Floor : Placement::Any);
    finding.suggestedFix =
      move ? moveFix("Move it into the nearest room.", id, *move)
           : fixJson("Move it into a room (spaces_list and free_spots find one).");
    out.push_back(std::move(finding));
  }
}

// the tool

ToolError cancelledError()
{
  return makeError(ErrorCode::Cancelled, "The call was cancelled.");
}

Json findingJson(const Finding& finding, const std::string& check, const std::string& id)
{
  auto result = Json{
    {"id", id},
    {"check", check},
    {"code", finding.code},
    {"severity", finding.severity},
    {"description", finding.description},
    {"objectId", finding.objectId},
    {"objectIds", finding.objectIds},
  };
  if (finding.position)
  {
    result["position"] = vectorJson(*finding.position);
  }
  result["details"] = finding.details;
  result["suggestedFix"] = finding.suggestedFix;
  return result;
}

ToolResult finish(CheckRun& run)
{
  auto items = std::vector<Json>{};
  auto counts = std::map<std::string, size_t>{};
  auto seen = std::unordered_set<std::string>{};
  for (const auto& check : CheckNames)
  {
    const auto it = run.findings.find(check);
    if (it == run.findings.end())
    {
      continue;
    }
    for (const auto& finding : it->second)
    {
      const auto id = fmt::format(
        "check:{}:{}",
        finding.code,
        finding.key.empty() ? finding.objectId : finding.key);
      if (!seen.insert(id).second)
      {
        continue;
      }
      counts[finding.code] += 1;
      items.push_back(findingJson(finding, check, id));
    }
  }

  auto result = makePage(items, run.page, run.context->map().modificationCount());
  auto countsJson = Json::object();
  for (const auto& [code, count] : counts)
  {
    countsJson[code] = count;
  }
  result["counts"] = std::move(countsJson);
  result["checksRun"] = run.checksRun;
  result["skipped"] = run.skipped;
  return result;
}

void runStep(const std::shared_ptr<CheckRun>& run, const size_t index)
{
  auto& context = *run->context;
  const auto total = double(run->checks.size());
  context.progress(
    double(index),
    total,
    index < run->checks.size() ? "Checking " + run->checks[index] : "Done");
  context.defer([run, index]() {
    auto& stepContext = *run->context;
    if (stepContext.cancelled())
    {
      run->completion(cancelledError());
      return;
    }
    if (index == run->checks.size())
    {
      run->completion(finish(*run));
      return;
    }

    auto scope = resolveScope(stepContext, run->ids);
    if (scope.is_error())
    {
      run->completion(errorOf(scope));
      return;
    }
    const auto& check = run->checks[index];
    if (check == PlacementCheckName)
    {
      checkPlacement(*run, scope.value());
    }
    else if (check == PlayerStartCheckName)
    {
      checkPlayerStart(*run);
    }
    else if (check == LinksCheckName)
    {
      checkLinks(*run, scope.value());
    }
    else if (check == MaterialsCheckName)
    {
      checkMaterials(*run, scope.value());
    }
    else if (check == RoomsCheckName)
    {
      checkRooms(*run, scope.value());
    }
    run->checksRun.push_back(check);
    runStep(run, index + 1);
  });
}

void mapCheck(CallContext& context, const Args& args, ToolCompletion completion)
{
  auto request = pageRequest(args, context.map().modificationCount());
  if (request.is_error())
  {
    completion(errorOf(request));
    return;
  }

  auto run = std::make_shared<CheckRun>();
  run->context = &context;
  run->completion = std::move(completion);
  run->page = std::move(request).value();
  run->ids = args.getOptional<std::vector<std::string>>("ids");
  run->spaceOptions.region = args.getOptional<vm::bbox3d>("region");
  run->spaceOptions.cellSize = args.getOr<double>("cellSize", 0.0);
  run->spaceOptions.openingSize = args.getOr<double>("openingSize", 96.0);

  if (const auto scope = resolveScope(context, run->ids); scope.is_error())
  {
    run->completion(errorOf(scope));
    return;
  }

  const auto requested = args.getOptional<std::vector<std::string>>("checks");
  for (const auto& check : CheckNames)
  {
    if (requested && std::ranges::find(*requested, check) == requested->end())
    {
      continue;
    }
    if (check == PlayerStartCheckName && run->ids && !requested)
    {
      run->skip(
        check, "A map-level check; with ids it runs only when requested in checks.");
      continue;
    }
    run->checks.push_back(check);
  }

  runStep(run, 0);
}

Schema findingSchema()
{
  return object(
           {
             field("id", string())
               .required()
               .describe("check:<code>:<object id or signature>, stable across calls"),
             field("check", enumOf(CheckNames)).required(),
             field("code", string()).required(),
             field("severity", enumOf({"error", "warning", "info"})).required(),
             field("description", string()).required(),
             field("objectId", string()).required(),
             field("objectIds", array(string())).required(),
             field("position", vec3())
               .describe("Where the problem is, if it has a place"),
             field("details", any()).required(),
             field("suggestedFix", any())
               .required()
               .describe(
                 "{description, tool, args}: the MCP call that fixes the finding (tool "
                 "and args null if there is no single call), or null"),
           })
    .allowAdditionalProperties();
}

} // namespace

void registerMapCheckTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"map_check"}
      .title("Check Map")
      .description(
        "Runs agent-oriented checks beyond the editor's validators (issues_list) and "
        "returns findings, each with a severity, a plain description and a "
        "suggestedFix (the MCP tool call with its args that fixes it, when there is "
        "one). Read-only. Checks: placement (ENTITY_IN_SOLID: a point entity inside "
        "solid brushes, hidden ones included; logic entities such as multi_manager, "
        "multisource or trigger_relay work anywhere and are skipped; ENTITY_FLOATING: "
        "a player start, monster or item more than 16 map units above the floor; "
        "MODEL_BELOW_FLOOR / MODEL_FLOATING / MODEL_PENETRATES_BRUSHES / "
        "MODEL_NO_FLOOR for standing entities whose model can be loaded, by the same "
        "rule as issues_list; lights, sounds, targets and path corners get no model "
        "checks, flying or swimming monsters may float), player_start "
        "(MISSING_PLAYER_START; MISSING_SINGLE_PLAYER_START "
        "(info) when only deathmatch or coop starts exist), links "
        "(LINK_TARGET_MISSING: target, killtarget or a multi_manager key names no "
        "entity; LINK_SOURCE_MISSING: a named door, relay, counter, train, path corner "
        "or target_* that nothing triggers; NEEDS_TARGETNAME: such an entity without a "
        "name), materials (MISSING_MATERIAL: one finding per material in no loaded "
        "collection) and rooms (ENTITY_OUTSIDE_HULL: the void reaches the entity, the "
        "map leaks; the fix is a brush_create_box over the gap or a move into a room; "
        "ENTITY_OUTSIDE_SPACES: a point entity in no room of spaces_list). "
        "Finding ids (check:<code>:<object>) are stable across calls. Also returns "
        "counts per code, checksRun and skipped. Runs the space analysis when needed "
        "(about a quarter of a second on 20 rooms), reports progress and can be "
        "cancelled between checks. Examples: {\"checks\": [\"links\", "
        "\"player_start\"]}; {\"ids\": [\"entity:7\", \"group:3\"], \"checks\": "
        "[\"placement\"]}")
      .input(object({
        field("checks", array(enumOf(CheckNames)).nonEmpty())
          .describe(
            "The checks to run: placement, player_start, links, materials, rooms. "
            "Default: all"),
        field("ids", array(objectId()).nonEmpty())
          .describe(
            "Limit the object-based checks to these objects and their contents "
            "(entities, brushes, groups, layers). player_start is a map-level check "
            "and runs with ids only when listed in checks. Default: the whole map"),
        field("region", box())
          .describe(
            "Limit the space analysis of the rooms check to this box; "
            "ENTITY_OUTSIDE_SPACES then checks only the entities inside it. Default: the "
            "whole map"),
        field("cellSize", number().min(2).max(1024))
          .describe(
            "Cell size of the space analysis in map units (default: half the player "
            "width, enlarged with warning CELL_SIZE_ENLARGED when the map needs too many "
            "cells, e.g. under a tall sky); smaller is more precise and slower"),
        field("openingSize", number().min(8).defaultsTo(96))
          .describe(
            "Openings up to this size in map units separate spaces (rooms check), as "
            "in spaces_list"),
      }))
      .output(object({
        field("items", array(findingSchema())).required(),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("counts", any()).required().describe("Findings per code (all pages)"),
        field("checksRun", array(string())).required(),
        field("skipped", array(any()))
          .required()
          .describe("{check, reason}: checks or parts of checks that did not run"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .asyncHandler(mapCheck));
}

} // namespace tb::mcp
