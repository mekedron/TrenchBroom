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

#include "mcp/tools/EngineRules.h"

#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/tools/CompileUtils.h"
#include "mcp/tools/GeometryUtils.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/NodeTree.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/string_compare.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace tb::mcp
{
namespace
{

// Half-Life (GoldSrc): the engine allocates 900 edicts unless started with -num_edicts
// (up to 2048); the monster sizes are those the Half-Life SDK's Spawn functions set with
// UTIL_SetSize (VEC_HUMAN_HULL_MIN/MAX is -16 -16 0, 16 16 72).

const auto HumanHull = vm::bbox3d{{-16, -16, 0}, {16, 16, 72}};
const auto LargeHull = vm::bbox3d{{-32, -32, 0}, {32, 32, 64}};

EngineRules makeHalfLifeRules()
{
  auto rules = EngineRules{};
  rules.family = "halflife";

  auto& budget = rules.budget;
  budget.engine = "GoldSrc";
  budget.defaultLimit = 900;
  budget.maxLimit = 2048;
  budget.raiseLimit =
    "Start Half-Life with -num_edicts <n> (at most 2048), e.g. hl.exe -game valve "
    "-num_edicts 2048 +map <map>; add it to the engine profile's parameters "
    "(engine_profile_save) or pass it to engine_launch. Players of the released map need "
    "the same option, so staying below 900 is safer";
  // worldspawn and the single player
  budget.reservedSlots = 2;
  budget.runtimeReserve = 64;
  budget.compilerRemoved = {
    "func_group",
    "func_detail",
    "info_texlights",
    "info_compile_parameters",
    "info_hullshape",
    "info_smoothvalue",
    "info_translucent",
    "info_angularfade",
    "info_minlights",
    "light_surface",
  };
  budget.removedAtSpawn = {"info_null", "info_node", "info_node_air"};
  budget.removedWithoutTargetname = {
    "light", "light_spot", "light_environment", "infodecal"};
  budget.extraEdicts = {
    {"env_laser", 1, {}, "EndSprite", "its end sprite is an entity of its own"},
    {"monstermaker",
     5,
     "m_imaxlivechildren",
     {},
     "the monsters it spawns (m_imaxlivechildren, default 5)"},
    {"func_tanklaser", 1, {}, {}, "its laser is an entity of its own"},
  };

  auto& monsters = rules.monsters;
  using enum MonsterMovement;
  monsters.classes = {
    {"monster_alien_controller", LargeHull, Fly},
    {"monster_alien_grunt", LargeHull, Walk},
    {"monster_alien_slave", HumanHull, Walk},
    {"monster_apache", {{-32, -32, -64}, {32, 32, 0}}, Fly},
    {"monster_babycrab", {{-12, -12, 0}, {12, 12, 24}}, Walk},
    {"monster_barnacle", {{-16, -16, -32}, {16, 16, 0}}, Ceiling},
    {"monster_barney", HumanHull, Walk},
    {"monster_bigmomma", LargeHull, Walk},
    {"monster_bloater", LargeHull, Fly},
    {"monster_bullchicken", LargeHull, Walk},
    {"monster_cockroach", {{-1, -1, 0}, {1, 1, 2}}, Walk},
    {"monster_gargantua", LargeHull, Walk},
    {"monster_generic", HumanHull, Walk},
    {"monster_gman", HumanHull, Walk},
    {"monster_headcrab", {{-12, -12, 0}, {12, 12, 24}}, Walk},
    {"monster_houndeye", {{-16, -16, 0}, {16, 16, 36}}, Walk},
    {"monster_human_assassin", HumanHull, Walk},
    {"monster_human_grunt", HumanHull, Walk},
    {"monster_ichthyosaur", {{-32, -32, -32}, {32, 32, 32}}, Swim},
    {"monster_leech", {{-1, -1, 0}, {1, 1, 2}}, Swim},
    {"monster_nihilanth", LargeHull, Fly},
    {"monster_osprey", {{-400, -400, -100}, {400, 400, 32}}, Fly},
    {"monster_scientist", HumanHull, Walk},
    {"monster_sitting_scientist", {{-14, -14, 0}, {14, 14, 36}}, Static},
    {"monster_snark", {{-4, -4, 0}, {4, 4, 8}}, Walk},
    {"monster_tentacle", LargeHull, Fly},
    {"monster_zombie", HumanHull, Walk},
    {"monster_miniturret", {{-16, -16, -16}, {16, 16, 16}}, Fly},
    {"monster_turret", {{-32, -32, -16}, {32, 32, 16}}, Fly},
    {"monster_sentry", {{-16, -16, 0}, {16, 16, 64}}, Static},
  };
  monsters.prefixes = {"monster_"};
  monsters.excludedSuffixes = {"_dead"};
  monsters.solidClasses = {
    "cycler", "cycler_sprite", "cycler_weapon", "monster_furniture"};
  monsters.nonSolidBrushClasses = {"func_ladder", "func_monsterclip"};
  monsters.hulls = {
    {0, "point", {0, 0, 0}},
    {1, "human", {32, 32, 72}},
    {2, "large", {64, 64, 64}},
    {3, "head", {32, 32, 36}},
  };
  monsters.goldSrcHullChoice = true;
  monsters.dropDistance = 256.0;
  monsters.stuckBehavior =
    "The game logs \"Monster <class> stuck in wall--level design error\", the monster "
    "glows (EF_BRIGHTFIELD) and cannot move";
  return rules;
}

// Quake: 600 edicts in the original engine (source ports allow thousands); monsters
// drop to the floor like in Half-Life (walkmonster_start_go, "walkmonster in wall").

const auto QuakeHull = vm::bbox3d{{-16, -16, -24}, {16, 16, 40}};
const auto QuakeLargeHull = vm::bbox3d{{-32, -32, -24}, {32, 32, 64}};

EngineRules makeQuakeRules()
{
  auto rules = EngineRules{};
  rules.family = "quake";

  auto& budget = rules.budget;
  budget.engine = "Quake";
  budget.defaultLimit = 600;
  budget.maxLimit = std::nullopt;
  budget.raiseLimit =
    "The original Quake engine has a fixed limit of 600 edicts; source ports such as "
    "QuakeSpasm or ironwail allow thousands (the map then needs such a port)";
  budget.reservedSlots = 2;
  budget.runtimeReserve = 64;
  budget.compilerRemoved = {"func_group", "func_detail", "func_detail_illusionary"};
  budget.removedAtSpawn = {"info_null", "info_notnull"};
  budget.removedWithoutTargetname = {
    "light",
    "light_fluoro",
    "light_fluorospark",
    "light_globe",
    "light_torch_small_walltorch",
    "light_flame_large_yellow",
    "light_flame_small_yellow",
    "light_flame_small_white"};

  auto& monsters = rules.monsters;
  using enum MonsterMovement;
  monsters.classes = {
    {"monster_army", QuakeHull, Walk},
    {"monster_dog", {{-32, -32, -24}, {32, 32, 40}}, Walk},
    {"monster_ogre", QuakeLargeHull, Walk},
    {"monster_knight", QuakeHull, Walk},
    {"monster_hell_knight", QuakeHull, Walk},
    {"monster_demon1", QuakeLargeHull, Walk},
    {"monster_shambler", QuakeLargeHull, Walk},
    {"monster_zombie", QuakeHull, Walk},
    {"monster_enforcer", QuakeHull, Walk},
    {"monster_tarbaby", QuakeHull, Walk},
    {"monster_shalrath", {{-32, -32, -24}, {32, 32, 48}}, Walk},
    {"monster_wizard", QuakeHull, Fly},
    {"monster_fish", {{-16, -16, -24}, {16, 16, 24}}, Swim},
  };
  monsters.prefixes = {"monster_"};
  monsters.excludedSuffixes = {};
  monsters.solidClasses = {};
  monsters.nonSolidBrushClasses = {};
  monsters.hulls = {
    {0, "point", {0, 0, 0}},
    {1, "player", {32, 32, 56}},
    {2, "large", {64, 64, 88}},
  };
  monsters.goldSrcHullChoice = false;
  monsters.dropDistance = 256.0;
  monsters.stuckBehavior =
    "The game logs \"walkmonster in wall at: <position>\" and the monster cannot move";
  return rules;
}

EngineRules makeQuake2Rules()
{
  auto rules = EngineRules{};
  rules.family = "quake2";
  auto& budget = rules.budget;
  budget.engine = "Quake 2";
  budget.defaultLimit = 1024;
  budget.maxLimit = std::nullopt;
  budget.raiseLimit =
    "Quake 2 allocates maxentities edicts (default 1024); +set maxentities <n> on the "
    "command line raises it where the game supports it";
  // worldspawn and the player (maxclients 1)
  budget.reservedSlots = 2;
  budget.runtimeReserve = 64;
  budget.compilerRemoved = {"func_group"};
  budget.removedAtSpawn = {"info_null"};
  budget.removedWithoutTargetname = {"light"};
  return rules;
}

EngineRules makeQuake3Rules()
{
  auto rules = EngineRules{};
  rules.family = "quake3";
  auto& budget = rules.budget;
  budget.engine = "Quake 3";
  budget.defaultLimit = 1024;
  budget.maxLimit = std::nullopt;
  budget.raiseLimit =
    "Quake 3 has a fixed limit of 1024 entities (MAX_GENTITIES), of which 64 belong to "
    "the clients";
  budget.reservedSlots = 65;
  budget.runtimeReserve = 64;
  budget.compilerRemoved = {"func_group", "light", "misc_model", "_decal", "_skybox"};
  budget.removedAtSpawn = {"info_null"};
  return rules;
}

const std::vector<EngineRules>& allRules()
{
  static const auto rules = std::vector<EngineRules>{
    makeHalfLifeRules(), makeQuakeRules(), makeQuake2Rules(), makeQuake3Rules()};
  return rules;
}

bool contains(const std::vector<std::string>& list, const std::string_view str)
{
  return std::ranges::any_of(
    list, [&](const auto& item) { return kdl::ci::str_is_equal(item, str); });
}

std::optional<size_t> positiveNumber(const std::string* value)
{
  if (!value)
  {
    return std::nullopt;
  }
  auto result = long{0};
  const auto* end = value->data() + value->size();
  const auto [ptr, ec] = std::from_chars(value->data(), end, result);
  return ec == std::errc{} && result > 0 ? std::optional{size_t(result)} : std::nullopt;
}

bool hasValue(const mdl::Entity& entity, const std::string& key)
{
  const auto* value = entity.property(key);
  return value && !value->empty();
}

void collectExported(
  const mdl::Node& node,
  std::vector<const mdl::EntityNode*>& entities,
  size_t& containers)
{
  node.accept(kdl::overload(
    [&](auto&& thisLambda, const mdl::WorldNode& worldNode) {
      worldNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::LayerNode& layerNode) {
      if (layerNode.layer().omitFromExport())
      {
        return;
      }
      if (!layerNode.isDefaultLayer())
      {
        ++containers;
      }
      layerNode.visitChildren(thisLambda);
    },
    [&](auto&& thisLambda, const mdl::GroupNode& groupNode) {
      ++containers;
      groupNode.visitChildren(thisLambda);
    },
    [&](const mdl::EntityNode& entityNode) { entities.push_back(&entityNode); },
    [](const mdl::BrushNode&) {},
    [](const mdl::PatchNode&) {}));
}

/** Whether the brush stops a monster's hull. */
bool blocksMonsters(const EngineRules& rules, const mdl::BrushNode& brushNode)
{
  if (const auto* entityNode = owningBrushEntity(brushNode);
      entityNode
      && contains(rules.monsters.nonSolidBrushClasses, entityNode->entity().classname()))
  {
    return false;
  }
  const auto role = brushRole(brushNode);
  return role.blocksPlayer || (role.door && role.blocksObjects);
}

/**
 * The distance the box can move down before its interior meets the brush's interior
 * (bisection over the swept box), or nullopt if it does not meet it within `maxDistance`.
 */
std::optional<double> dropDistanceOnto(
  const mdl::Brush& brush, const vm::bbox3d& box, const double maxDistance)
{
  const auto swept = [&](const double distance) {
    return vm::bbox3d{box.min - vm::vec3d{0, 0, distance}, box.max};
  };
  if (!intersectsInterior(brush, swept(maxDistance)))
  {
    return std::nullopt;
  }
  auto low = 0.0;
  auto high = maxDistance;
  for (auto i = 0; i < 30 && high - low > 1.0 / 256.0; ++i)
  {
    const auto mid = (low + high) / 2.0;
    if (intersectsInterior(brush, swept(mid)))
    {
      high = mid;
    }
    else
    {
      low = mid;
    }
  }
  return low;
}

bool overlapsInterior(const vm::bbox3d& lhs, const vm::bbox3d& rhs)
{
  for (size_t i = 0; i < 3; ++i)
  {
    if (lhs.min[i] >= rhs.max[i] || rhs.min[i] >= lhs.max[i])
    {
      return false;
    }
  }
  return true;
}

double roundTo(const double value, const double step)
{
  return std::round(value / step) * step;
}

} // namespace

std::string_view toString(const MonsterMovement movement)
{
  switch (movement)
  {
  case MonsterMovement::Walk:
    return "walk";
  case MonsterMovement::Fly:
    return "fly";
  case MonsterMovement::Swim:
    return "swim";
  case MonsterMovement::Ceiling:
    return "ceiling";
  case MonsterMovement::Static:
    return "static";
  }
  return "walk";
}

bool dropsToFloor(const MonsterMovement movement)
{
  return movement == MonsterMovement::Walk || movement == MonsterMovement::Static;
}

bool canBeStuck(const MonsterMovement movement)
{
  return movement != MonsterMovement::Static;
}

const EngineRules* engineRules(const mdl::GameConfig& gameConfig)
{
  const auto family = compileFamily(gameConfig);
  return family ? engineRules(toString(*family)) : nullptr;
}

const EngineRules* engineRules(const std::string_view family)
{
  const auto& rules = allRules();
  const auto it =
    std::ranges::find_if(rules, [&](const auto& r) { return r.family == family; });
  return it != rules.end() ? &*it : nullptr;
}

EntityBudget countEntityBudget(
  const mdl::Map& map,
  const EngineRules& rules,
  const std::optional<size_t> limit,
  const std::optional<size_t> runtimeReserve)
{
  const auto& budgetRules = rules.budget;
  auto budget = EntityBudget{};
  budget.limit = limit.value_or(budgetRules.defaultLimit);
  budget.maxLimit = budgetRules.maxLimit;

  auto entities = std::vector<const mdl::EntityNode*>{};
  auto containers = size_t(0);
  collectExported(map.worldNode(), entities, containers);

  // worldspawn, and layers and groups exported as func_group
  budget.mapEntities = 1 + containers + entities.size();
  if (contains(budgetRules.compilerRemoved, "func_group"))
  {
    budget.removedByCompiler += containers;
  }
  else
  {
    budget.spawned += containers;
  }

  auto perClass = std::map<std::string, size_t>{};
  auto extraPerClass = std::map<std::string, size_t>{};
  for (const auto* entityNode : entities)
  {
    const auto& entity = entityNode->entity();
    const auto& classname = entity.classname();
    if (contains(budgetRules.compilerRemoved, classname))
    {
      ++budget.removedByCompiler;
      continue;
    }
    if (
      contains(budgetRules.removedAtSpawn, classname)
      || (contains(budgetRules.removedWithoutTargetname, classname) && !hasValue(entity, mdl::EntityPropertyKeys::Targetname)))
    {
      ++budget.removedAtSpawn;
      continue;
    }

    ++budget.spawned;
    ++perClass[classname];
    for (const auto& extra : budgetRules.extraEdicts)
    {
      if (
        kdl::ci::str_is_equal(extra.classname, classname)
        && (extra.requiredKey.empty() || hasValue(entity, extra.requiredKey)))
      {
        const auto count =
          extra.countKey.empty()
            ? extra.count
            : positiveNumber(entity.property(extra.countKey)).value_or(extra.count);
        budget.extra += count;
        extraPerClass[classname] += count;
      }
    }
  }

  budget.reserved =
    budgetRules.reservedSlots + runtimeReserve.value_or(budgetRules.runtimeReserve);
  // worldspawn is one of the reserved slots
  budget.total = budget.spawned + budget.extra + budget.reserved;

  for (const auto& [classname, count] : perClass)
  {
    budget.topClasses.push_back({classname, count});
  }
  std::ranges::stable_sort(budget.topClasses, [](const auto& lhs, const auto& rhs) {
    return lhs.count > rhs.count;
  });
  if (budget.topClasses.size() > 10)
  {
    budget.topClasses.resize(10);
  }
  for (const auto& [classname, count] : extraPerClass)
  {
    budget.extraByClass.push_back({classname, count});
  }
  return budget;
}

Json toJson(const EntityBudget& budget)
{
  const auto classes = [](const std::vector<BudgetClass>& list) {
    auto json = Json::array();
    for (const auto& item : list)
    {
      json.push_back(Json{{"classname", item.classname}, {"count", item.count}});
    }
    return json;
  };
  return Json{
    {"mapEntities", budget.mapEntities},
    {"removedByCompiler", budget.removedByCompiler},
    {"removedAtSpawn", budget.removedAtSpawn},
    {"spawned", budget.spawned},
    {"extra", budget.extra},
    {"reserved", budget.reserved},
    {"total", budget.total},
    {"limit", budget.limit},
    {"maxLimit", budget.maxLimit ? Json(*budget.maxLimit) : Json(nullptr)},
    {"topClasses", classes(budget.topClasses)},
    {"extraByClass", classes(budget.extraByClass)},
  };
}

std::optional<MonsterClass> monsterClass(
  const EngineRules& rules, const mdl::Entity& entity, const vm::bbox3d& definitionBounds)
{
  const auto& monsters = rules.monsters;
  const auto& classname = entity.classname();
  if (std::ranges::any_of(monsters.excludedSuffixes, [&](const auto& suffix) {
        return classname.size() > suffix.size() && classname.ends_with(suffix);
      }))
  {
    return std::nullopt;
  }
  const auto it = std::ranges::find_if(monsters.classes, [&](const auto& monster) {
    return kdl::ci::str_is_equal(monster.classname, classname);
  });
  if (it != monsters.classes.end())
  {
    return *it;
  }
  if (std::ranges::any_of(monsters.prefixes, [&](const auto& prefix) {
        return classname.starts_with(prefix);
      }))
  {
    return MonsterClass{classname, definitionBounds, MonsterMovement::Walk};
  }
  return std::nullopt;
}

HullBox engineHullBox(
  const EngineRules& rules, const vm::vec3d& origin, const vm::bbox3d& classBounds)
{
  const auto& monsters = rules.monsters;
  const auto size = classBounds.size();
  const auto hullIndex = [&]() -> size_t {
    if (monsters.goldSrcHullChoice)
    {
      // SV_HullForBsp in GoldSrc
      if (size.x() <= 8)
      {
        return 0;
      }
      if (size.x() <= 36)
      {
        return size.z() <= 36 ? 3 : 1;
      }
      return 2;
    }
    // SV_HullForEntity in Quake
    if (size.x() < 3)
    {
      return 0;
    }
    return size.x() <= 32 ? 1 : 2;
  }();

  const auto it = std::ranges::find_if(
    monsters.hulls, [&](const auto& hull) { return hull.index == hullIndex; });
  const auto hull = it != monsters.hulls.end() ? *it : EngineHull{0, "point", {0, 0, 0}};
  const auto min = origin + classBounds.min;
  return HullBox{hull, vm::bbox3d{min, min + hull.size}};
}

SpawnCheck checkMonsterSpawn(
  mdl::Map& map,
  const IdRegistry& ids,
  const EngineRules& rules,
  const mdl::EntityNode& entityNode,
  const MonsterClass& monster)
{
  const auto& monsters = rules.monsters;
  const auto origin = entityNode.entity().origin();
  const auto drops = dropsToFloor(monster.movement);
  // a walking monster is raised by 1 unit before it drops to the floor
  const auto raise = monster.movement == MonsterMovement::Walk ? 1.0 : 0.0;

  auto result = SpawnCheck{};
  result.monster = monster;
  result.hull = engineHullBox(rules, origin, monster.bounds);

  const auto start = result.hull.box.translate(vm::vec3d{0, 0, raise});
  const auto dropDistance = drops ? monsters.dropDistance + raise : 0.0;
  const auto region =
    vm::bbox3d{start.min - vm::vec3d{0, 0, dropDistance}, start.max}.expand(1.0);

  // the boxes of other solid entities: monsters by their hull, solid classes by bounds
  const auto entityBox = [&](const mdl::EntityNode& other) -> std::optional<vm::bbox3d> {
    const auto& otherEntity = other.entity();
    if (!isPointEntity(other) || inOmittedLayer(other))
    {
      return std::nullopt;
    }
    const auto otherOrigin = otherEntity.origin();
    const auto definitionBounds = other.logicalBounds().translate(-otherOrigin);
    if (const auto otherMonster = monsterClass(rules, otherEntity, definitionBounds))
    {
      return engineHullBox(rules, otherOrigin, otherMonster->bounds).box;
    }
    if (contains(monsters.solidClasses, otherEntity.classname()))
    {
      return other.logicalBounds();
    }
    return std::nullopt;
  };

  auto bestDrop = std::optional<double>{};
  auto bestSupport = std::optional<std::string>{};
  const auto landOn = [&](const double distance, const mdl::Node& node) {
    if (!bestDrop || distance < *bestDrop)
    {
      bestDrop = distance;
      bestSupport = ids.format(node);
    }
  };

  for (auto* node : map.worldNode().nodeTree().find_intersectors(region))
  {
    if (node == &entityNode || inOmittedLayer(*node))
    {
      continue;
    }
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node))
    {
      if (!blocksMonsters(rules, *brushNode))
      {
        continue;
      }
      const auto& brush = brushNode->brush();
      if (intersectsInterior(brush, start))
      {
        result.stuckIn.push_back(ids.format(*brushNode));
      }
      else if (drops)
      {
        if (const auto distance = dropDistanceOnto(brush, start, dropDistance))
        {
          landOn(*distance, *brushNode);
        }
      }
    }
    else if (const auto* otherNode = dynamic_cast<const mdl::EntityNode*>(node))
    {
      const auto box = entityBox(*otherNode);
      if (!box)
      {
        continue;
      }
      if (overlapsInterior(start, *box))
      {
        result.stuckIn.push_back(ids.format(*otherNode));
      }
      else if (
        drops && box->max.z() <= start.min.z()
        && box->max.z() >= start.min.z() - dropDistance
        && overlapsInterior(
          vm::bbox3d{
            {start.min.x(), start.min.y(), 0}, {start.max.x(), start.max.y(), 1}},
          vm::bbox3d{{box->min.x(), box->min.y(), 0}, {box->max.x(), box->max.y(), 1}}))
      {
        landOn(start.min.z() - box->max.z(), *otherNode);
      }
    }
  }
  std::ranges::sort(result.stuckIn);

  result.spawnBox = start;
  if (drops && result.stuckIn.empty())
  {
    if (bestDrop)
    {
      // the drop from the placed position (a walking monster was raised first)
      result.drop = roundTo(*bestDrop - raise, 1.0 / 64.0);
      result.support = bestSupport;
      result.spawnBox = start.translate(vm::vec3d{0, 0, -*bestDrop});
    }
  }
  else if (!drops)
  {
    result.drop = 0.0;
  }
  return result;
}

Json toJson(const SpawnCheck& check)
{
  const auto& hull = check.hull.hull;
  return Json{
    {"classname", check.monster.classname},
    {"movement", toString(check.monster.movement)},
    {"classBounds", toJson(check.monster.bounds)},
    {"hull",
     Json{
       {"index", hull.index},
       {"name", hull.name},
       {"size", toJson(hull.size)},
     }},
    {"hullBox", toJson(check.hull.box)},
    {"stuckIn", check.stuckIn},
    {"drop", check.drop ? Json(*check.drop) : Json(nullptr)},
    {"support", check.support ? Json(*check.support) : Json(nullptr)},
    {"spawnBox", toJson(check.spawnBox)},
  };
}

} // namespace tb::mcp
