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

#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mdl/BrushFaceHandle.h"

#include "vm/plane.h"
#include "vm/vec.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class EntityNode;
class Map;
class Node;
} // namespace tb::mdl

namespace tb::mcp
{
class EntityModelLoader;
class IdRegistry;
class MaterialKnowledge;
struct LeakReport;

// MCP placement checks (E12.7 - E12.9): z-fighting, entities outside the sealed hull,
// model placement and texture distortion, as issues with a machine code and details.
// They are reported per call in `issuesIntroduced` (PlacementTracker) and for the whole
// map by issues_list; they are not registered as editor validators.

// Issue codes
inline constexpr auto ZFightingCode = std::string_view{"Z_FIGHTING"};
inline constexpr auto EntityOutsideHullCode = std::string_view{"ENTITY_OUTSIDE_HULL"};
inline constexpr auto UvDistortionCode = std::string_view{"UV_ASPECT_DISTORTION"};
// Model placement codes are those of PlacementFinding (EntityModelUtils.h):
// MODEL_BELOW_FLOOR, MODEL_FLOATING, MODEL_PENETRATES_BRUSHES, MODEL_NO_FLOOR.

/** The codes of the MCP checks, for schemas and filters. */
std::vector<std::string> mcpIssueCodes();

/** The name of the model placement check, which reports the four MODEL_* codes. */
inline constexpr auto ModelPlacementCheck = std::string_view{"MODEL_PLACEMENT"};

/**
 * An MCP check as a validator (validators_list / validators_set): its name, a title
 * and the issue codes it reports.
 */
struct McpCheck
{
  std::string name;
  std::string title;
  std::vector<std::string> codes;
};

/** The MCP checks: Z_FIGHTING, ENTITY_OUTSIDE_HULL, MODEL_PLACEMENT,
 * UV_ASPECT_DISTORTION. */
std::vector<McpCheck> mcpChecks();

/** The name of the MCP check that reports the given code (the code itself if unknown). */
std::string mcpCheckOfCode(std::string_view code);

/** A problem found by an MCP check. */
struct McpIssue
{
  /** Machine code, e.g. Z_FIGHTING. */
  std::string code = {};
  /** Human-readable type, e.g. "Z-fighting". */
  std::string type = {};
  /** The object the issue is about: a face id, an entity id or a brush id. */
  std::string objectId = {};
  std::string description = {};
  /** Face ids, positions, bounds, measured values etc. */
  Json details = Json::object();
  /**
   * Identifies the issue across calls (e.g. both brushes and the plane of a z-fighting
   * pair), so that a call reports only issues that were not there before.
   */
  std::string signature = {};
  /** All objects involved (the objectId first). */
  std::vector<std::string> objectIds = {};
  /**
   * The plane of the face the issue is about (Z_FIGHTING, UV_ASPECT_DISTORTION), for
   * sameIssue.
   */
  std::optional<vm::plane3d> plane = std::nullopt;
};

/** Face planes whose normals differ by at most this angle (degrees) match. */
constexpr auto SameIssueMaxAngle = 10.0;
/** Z-fighting planes whose distances differ by at most this (units) match. */
constexpr auto SameIssueMaxDistance = 1.0;

/**
 * Whether an issue found after a change is one found before it. The signatures contain
 * the face plane, which changes slightly when a call snaps vertices or splits a face,
 * although the problem stays. So issues about a face match if they have the same code
 * and objects (Z_FIGHTING: both brushes; UV_ASPECT_DISTORTION: the brush and the
 * material) and their plane normals differ by at most SameIssueMaxAngle (Z_FIGHTING: and
 * the distances by at most SameIssueMaxDistance); other issues match by signature.
 */
bool sameIssue(const McpIssue& before, const McpIssue& after);

// Z-fighting

/**
 * Whether the material is a tool material that is not rendered in the game (clip,
 * trigger, skip, hint, origin, null, nodraw, caulk, ... in any game; the name after the
 * last '/' is compared case-insensitively, so Quake 2 'e1u1/clip' and Quake 3
 * 'common/caulk' are tool materials too).
 */
bool isToolMaterial(std::string_view materialName);

/** Whether z-fighting checks ignore the brush: it belongs to a trigger_* entity. */
bool ignoredForZFighting(const mdl::BrushNode& brushNode);

/** Two faces that z-fight. */
struct ZFighting
{
  mdl::BrushFaceHandle first;
  mdl::BrushFaceHandle second;
  /** The area of the visible overlap in square units. */
  double area = 0.0;
  /** The center of the overlap. */
  vm::vec3d center;
  /** The common plane (the faces' boundary). */
  vm::plane3d plane;
};

/** Overlaps up to this area (square units) are ignored. */
constexpr auto MinZFightingArea = 1.0;

/**
 * Finds z-fighting: faces of different brushes that lie in the same plane, face the
 * same way and overlap by more than MinZFightingArea, unless the overlap is hidden by
 * touching faces of other brushes that lie in the same plane and face the other way.
 * Faces with tool materials and brushes of trigger_* entities are ignored (also as
 * hiding faces). Neighbours are found through the world's node tree. With `brushes`,
 * only pairs that involve one of these brushes are returned (the first face belongs to
 * one of them); otherwise the whole map is checked. Each pair is returned once.
 */
std::vector<ZFighting> findZFighting(
  const mdl::Map& map, const std::vector<const mdl::BrushNode*>* brushes = nullptr);

/**
 * Z_FIGHTING issues about the given pairs. The objectId is the face id of the first
 * face; details: {faces: [a, b], brushes: [a, b], materials, area, center, plane:
 * {normal, distance}}.
 */
std::vector<McpIssue> zFightingIssues(
  const std::vector<ZFighting>& pairs, const IdRegistry& ids);

// Leaks

/**
 * ENTITY_OUTSIDE_HULL issues of the report's findings; details: {classname, position,
 * gap: {min, max} or null, gapBrushes, cellSize}.
 */
std::vector<McpIssue> leakIssues(const LeakReport& report, const IdRegistry& ids);

// Model placement

/**
 * The model placement findings (checkModelPlacement) of the given point entities whose
 * models can be loaded and that the placement rule checks (checksModelPlacement: standing
 * classes, no sprites; MODEL_FLOATING and MODEL_NO_FLOOR not for entities that may
 * float), as issues with the finding code; details: {modelBounds, distance?,
 * suggestedMove?, surface, relatedIds}. map_check uses the same rule.
 */
std::vector<McpIssue> modelPlacementIssues(
  mdl::Map& map,
  const IdRegistry& ids,
  const std::vector<mdl::EntityNode*>& entities,
  EntityModelLoader& loader);

// Texture distortion

/**
 * UV_ASPECT_DISTORTION issues (checkUv) of the given faces. Faces whose texel aspect
 * is square within the tolerance and whose material has no note or corpus entry cannot
 * be distorted and are skipped without computing a profile, so that checking a few
 * faces stays cheap. The objectId is the face id; details are the finding's measured
 * values, material and suggested fix.
 */
std::vector<McpIssue> uvDistortionIssues(
  const std::vector<mdl::BrushFaceHandle>& faces,
  const mdl::Map& map,
  MaterialKnowledge& knowledge,
  const IdRegistry& ids);

// Per-call reporting

/**
 * The per-document state of the per-call checks, owned by the server's DocumentState.
 * Leak prediction is global and expensive, so its last result is kept and reused as the
 * "before" state of the next call as long as nothing changed.
 */
struct PlacementCache
{
  /**
   * Counts the changes of the document's objects (additions, removals, changes of
   * anything but layers and the world), made by the agent or the user. Unlike the
   * map's modification count it never goes back (undo), so equal values mean an
   * unchanged map.
   */
  size_t changeCount = 0;
  /** The changeCount at which the leak result below was computed. */
  std::optional<size_t> leakKey = std::nullopt;
  /** The signatures (entity ids) of the entities outside the hull at leakKey. */
  std::unordered_set<std::string> leakSignatures = {};
  /** Whether a point entity was inside the hull at leakKey. */
  bool leakAnyEnclosed = false;
  /** Leak prediction is not run per call on this document (too large or too slow). */
  bool leakChecksDisabled = false;

  void nodesAddedOrRemoved();
  void nodesChanged(const std::vector<mdl::Node*>& nodes);
  bool leakCacheValid() const;
  /**
   * After a call was rolled back (dry run, failure): the map is as it was at the given
   * change count, so a leak result of that state is valid again.
   */
  void rolledBack(size_t changeCountAtStart);
};

/** A leak prediction per call that takes longer disables it for the document. */
constexpr auto MaxPerCallLeakTime = std::chrono::milliseconds{500};

struct PlacementTrackerOptions
{
  /** Null disables leak prediction. */
  PlacementCache* cache = nullptr;
  /** The knowledge folder of MaterialKnowledge (UV checks). */
  std::optional<std::filesystem::path> knowledgeDirectory;
  /** The call is a dry run: its results are not cached. */
  bool dryRun = false;
  /**
   * The validators turned off for the document (validators_set): editor validator codes
   * and MCP check names. Their issues are not reported and turned-off MCP checks are
   * not run.
   */
  std::set<std::string> disabledValidators = {};
};

/** The placement problems a call introduced. */
struct PlacementReport
{
  std::vector<McpIssue> issues;
  /** E.g. LEAK_CHECK_SKIPPED (reported once per document). */
  std::vector<Warning> warnings;
};

/**
 * Tracks the placement problems a call introduces, like ChangeCollector tracks editor
 * issues: the findings of objects are snapshotted before they change or are removed, and
 * the findings after the call that were not there before (sameIssue) are introduced.
 *
 * - Z-fighting: pairs involving created or modified brushes; before = the pairs of the
 *   brushes that changed or were removed, snapshotted before the change.
 * - Model placement: created and modified point entities, point entities next to
 *   changed or removed brushes (snapshotted before), and point entities next to created
 *   or modified brushes (only findings that name such a brush, e.g. a new brush through
 *   a model).
 * - Texture distortion: the faces of created and modified brushes.
 * - Entities outside the hull: the whole map, only if the call created, modified or
 *   removed brushes, patches, entities or groups; before = the cached result, or a
 *   prediction made before the first change of the call if the cache is stale. While
 *   no point entity is inside the hull (before or after the call), the map has no
 *   sealed room yet, so only entities next to a gap in the sealing brushes are
 *   reported, not entities outside all brushes of an unfinished map.
 */
class PlacementTracker
{
private:
  mdl::Map& m_map;
  const IdRegistry& m_ids;
  PlacementTrackerOptions m_options;
  size_t m_changeCountAtStart = 0;

  /** The map was changed during the call (a change notification was received). */
  bool m_changed = false;
  /** The canonical ids of the nodes added during the call. */
  std::unordered_set<std::string> m_added;
  std::unordered_set<std::string> m_zSnapshotted;
  std::vector<McpIssue> m_zBefore;
  std::unordered_set<std::string> m_entitiesSnapshotted;
  std::unordered_set<std::string> m_placementBefore;
  std::unordered_set<std::string> m_uvSnapshotted;
  std::vector<McpIssue> m_uvBefore;
  /** The leak signatures before the call; nullopt if unknown. */
  std::optional<std::unordered_set<std::string>> m_leaksBefore;
  bool m_leaksBeforeEnclosed = false;
  bool m_leakBeforeTried = false;
  std::vector<Warning> m_warnings;

  std::unique_ptr<EntityModelLoader> m_loader;
  std::unique_ptr<MaterialKnowledge> m_knowledge;

public:
  PlacementTracker(mdl::Map& map, const IdRegistry& ids, PlacementTrackerOptions options);
  ~PlacementTracker();

  PlacementTracker(const PlacementTracker&) = delete;
  PlacementTracker& operator=(const PlacementTracker&) = delete;

  /** Before the nodes change. */
  void nodesWillChange(const std::vector<mdl::Node*>& nodes);
  /** Before the nodes are removed. */
  void nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes);
  /** After nodes were added. */
  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  /** After nodes changed or were removed. */
  void nodesDidChange();

  /**
   * The problems introduced by the call, given its net changes (canonical ids). Call
   * this while the changes still exist (before rolling back a dry run).
   */
  PlacementReport finish(
    const std::vector<std::string>& created,
    const std::vector<std::string>& modified,
    const std::vector<std::string>& removed);

private:
  void snapshot(const std::vector<mdl::Node*>& nodes);
  void snapshotLeaks();
  void disableLeakChecks(const std::string& reason);
  bool enabled(std::string_view check) const;
  EntityModelLoader& loader();
  MaterialKnowledge& knowledge();
};

} // namespace tb::mcp
