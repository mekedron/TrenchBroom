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

#pragma once

#include "mcp/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class Entity;
class EntityNode;
class Map;
struct GameConfig;
} // namespace tb::mdl

namespace tb::mcp
{
class IdRegistry;

// Game engine data for the checks that emulate the engine (E17.5, E17.6): the entity
// (edict) budget and how monsters spawn, per game family (Half-Life / GoldSrc, Quake,
// Quake 2, Quake 3). The values come from the engines and game code (Half-Life SDK,
// Quake and Quake 2 game sources); they are estimates where the game decides at run time.

/** An extra edict that an entity of a class allocates when it spawns. */
struct ExtraEdicts
{
  std::string classname;
  /** The extra edicts; `countKey` overrides it with the entity's value when positive. */
  size_t count = 1;
  /** A key whose positive value is the count (e.g. monstermaker's live children). */
  std::string countKey = {};
  /** Only if the entity has this key (e.g. env_laser's EndSprite); empty: always. */
  std::string requiredKey = {};
  /** Why, for the finding. */
  std::string reason;
};

/** The entity budget of an engine. */
struct EntityBudgetRules
{
  /** The engine, e.g. "GoldSrc". */
  std::string engine;
  /** The number of edicts the engine allocates by default. */
  size_t defaultLimit = 0;
  /** The most edicts the engine can allocate when asked (nullopt: fixed or unknown). */
  std::optional<size_t> maxLimit;
  /** How to raise the limit, for the finding (e.g. the -num_edicts option). */
  std::string raiseLimit;
  /** Edicts the engine reserves for the world and the players (single player). */
  size_t reservedSlots = 0;
  /** A reserve for entities created while the game runs (projectiles, gibs, items). */
  size_t runtimeReserve = 0;
  /** Classes the compile tools merge into the world or remove. */
  std::vector<std::string> compilerRemoved;
  /** Classes the game removes when they spawn. */
  std::vector<std::string> removedAtSpawn;
  /** Classes the game removes when they spawn without a targetname (static lights). */
  std::vector<std::string> removedWithoutTargetname;
  std::vector<ExtraEdicts> extraEdicts;
};

/** How a monster moves when it spawns. */
enum class MonsterMovement
{
  /** Raised by 1 unit, dropped to the floor (up to `dropDistance`), then tested. */
  Walk,
  /** Stays where it is (flying monsters, turrets). */
  Fly,
  /** Stays where it is, in water. */
  Swim,
  /** Hangs from the ceiling (barnacle). */
  Ceiling,
  /**
   * Dropped to the floor without the 1 unit raise and never moves, so overlapping a
   * solid is harmless (Half-Life's sitting scientist, sentry).
   */
  Static,
};

std::string_view toString(MonsterMovement movement);

/** Whether the game drops a monster of this movement to the floor when it spawns. */
bool dropsToFloor(MonsterMovement movement);

/** Whether a monster of this movement is stuck when its hull overlaps a solid. */
bool canBeStuck(MonsterMovement movement);

/** The size a monster class gives itself when it spawns. */
struct MonsterClass
{
  std::string classname;
  /** mins / maxs relative to the origin. */
  vm::bbox3d bounds;
  MonsterMovement movement = MonsterMovement::Walk;
};

/** A clipping hull of the engine: collision is traced with one of these boxes. */
struct EngineHull
{
  size_t index = 0;
  std::string name;
  /** The hull's size; 0 for the point hull. */
  vm::vec3d size;
};

struct MonsterSpawnRules
{
  /** Known monster classes with their size and movement. */
  std::vector<MonsterClass> classes;
  /** Classes starting with these prefixes are monsters, too (sized by the definition). */
  std::vector<std::string> prefixes;
  /** Classes with these suffixes are no spawning monsters (e.g. "_dead" corpses). */
  std::vector<std::string> excludedSuffixes;
  /** Point entity classes that are solid obstacles besides monsters (e.g. cycler). */
  std::vector<std::string> solidClasses;
  /** Brush entity classes that are not solid although their brushes are (ladders). */
  std::vector<std::string> nonSolidBrushClasses;
  /** The engine's clipping hulls: point, standing, large, crouching. */
  std::vector<EngineHull> hulls;
  /** GoldSrc chooses the hull by the width <= 8 / <= 36 and the height <= 36; Quake by
   * the width < 3 / <= 32. */
  bool goldSrcHullChoice = false;
  /** How far the game drops a walking monster to the floor. */
  double dropDistance = 256.0;
  /** What the engine does with a stuck monster, for the finding. */
  std::string stuckBehavior;
};

/** The engine data of a game family. */
struct EngineRules
{
  /** The family name, e.g. "halflife". */
  std::string family;
  EntityBudgetRules budget;
  MonsterSpawnRules monsters;
};

/** The engine rules of the game (by its compile family), or nullptr if there are none. */
const EngineRules* engineRules(const mdl::GameConfig& gameConfig);
/** The engine rules of a family ("halflife", "quake", "quake2", "quake3"), or nullptr. */
const EngineRules* engineRules(std::string_view family);

// Entity budget

/** Entities of one class that count or not. */
struct BudgetClass
{
  std::string classname;
  size_t count = 0;
};

struct EntityBudget
{
  /** Entities in the exported map, worldspawn included (layers omitted from export are
   * left out; groups and layers are exported as func_group). */
  size_t mapEntities = 0;
  /** Entities the compile tools remove. */
  size_t removedByCompiler = 0;
  /** Entities the game removes when they spawn. */
  size_t removedAtSpawn = 0;
  /** Entities that hold an edict while the level runs. */
  size_t spawned = 0;
  /** Extra edicts allocated by spawned entities. */
  size_t extra = 0;
  /** Reserved for the players and for entities created at run time. */
  size_t reserved = 0;
  /** spawned + extra + reserved. */
  size_t total = 0;
  size_t limit = 0;
  std::optional<size_t> maxLimit;
  /** The classes with the most spawned entities (at most 10). */
  std::vector<BudgetClass> topClasses;
  /** Extra edicts per class. */
  std::vector<BudgetClass> extraByClass;
};

/**
 * Counts the edicts the level needs. `limit` overrides the engine's default limit (e.g.
 * 2048 when the game runs with -num_edicts 2048), `runtimeReserve` the reserve.
 */
EntityBudget countEntityBudget(
  const mdl::Map& map,
  const EngineRules& rules,
  std::optional<size_t> limit = std::nullopt,
  std::optional<size_t> runtimeReserve = std::nullopt);

Json toJson(const EntityBudget& budget);

// Monster spawn

/** The spawn rule of an entity, or nullopt if it is no spawning monster. */
std::optional<MonsterClass> monsterClass(
  const EngineRules& rules,
  const mdl::Entity& entity,
  const vm::bbox3d& definitionBounds);

/**
 * The engine's clipping hull for a monster of the given size and its box in the world:
 * the hull box is placed at the monster's mins, like the engine does.
 */
struct HullBox
{
  EngineHull hull;
  vm::bbox3d box;
};

HullBox engineHullBox(
  const EngineRules& rules, const vm::vec3d& origin, const vm::bbox3d& classBounds);

/** The result of emulating a monster's spawn. */
struct SpawnCheck
{
  MonsterClass monster;
  HullBox hull;
  /** The objects the hull intersects where it spawns (brushes and entities). */
  std::vector<std::string> stuckIn;
  /** Walking monsters: how far the monster drops to the floor; nullopt if it finds no
   * floor within the drop distance. */
  std::optional<double> drop;
  /** The object it lands on. */
  std::optional<std::string> support;
  /** The hull box where the monster ends up. */
  vm::bbox3d spawnBox;
};

/**
 * Emulates a monster's spawn: the engine hull at the origin (walking monsters raised by
 * 1 unit), overlap with world brushes, clip brushes, solid brush entities and other solid
 * entities (monsters, solid point classes), and for walking monsters the drop to the
 * floor. Objects in layers omitted from export are ignored; hidden objects count.
 */
SpawnCheck checkMonsterSpawn(
  mdl::Map& map,
  const IdRegistry& ids,
  const EngineRules& rules,
  const mdl::EntityNode& entityNode,
  const MonsterClass& monster);

/** {classname, movement, hull {index, name, size}, box, stuckIn, drop, support}. */
Json toJson(const SpawnCheck& check);

} // namespace tb::mcp
