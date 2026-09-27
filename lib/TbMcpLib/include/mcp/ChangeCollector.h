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

#include "base/NotifierConnection.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/tools/PlacementChecks.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb
{
namespace mdl
{
class Map;
class Node;
struct SelectionChange;
} // namespace mdl

namespace ui
{
class MapDocument;
}

namespace mcp
{
class IdRegistry;

/** An ordered set of ids. */
class IdList
{
private:
  std::vector<std::string> m_ids;
  std::unordered_set<std::string> m_set;

public:
  bool add(std::string id);
  bool contains(const std::string& id) const;
  const std::vector<std::string>& ids() const;
  size_t size() const;
  bool empty() const;
};

struct IntroducedIssue
{
  std::string objectId;
  /** The editor validator's name, or the MCP check's type (e.g. "Z-fighting"). */
  std::string type;
  std::string description;
  /**
   * Machine code: the MCP check's code (e.g. Z_FIGHTING), or for editor issues the
   * validator name in UPPER_SNAKE case (issueCode).
   */
  std::string code = {};
  /** "editor" (the editor's validators) or "mcp" (PlacementChecks.h). */
  std::string source = "editor";
  /** Details of MCP issues (face ids, positions, bounds, ...); null for editor issues. */
  Json details = nullptr;
};

/** The validator name in UPPER_SNAKE case, e.g. "Empty brush entity" ->
 * EMPTY_BRUSH_ENTITY. */
std::string issueCode(std::string_view validatorName);

/** An introduced issue from an MCP check. */
IntroducedIssue introducedIssue(McpIssue issue);

/**
 * Removes the MCP issues that the call already reported as a warning with the same code
 * about the same object (e.g. the model placement warnings of entity_create_point, the
 * UV warnings of the material tools), so that each problem is reported once.
 */
void removeIssuesWarnedAbout(
  std::vector<IntroducedIssue>& issues, const std::vector<Warning>& warnings);

struct ChangeReport
{
  std::vector<std::string> created;
  std::vector<std::string> modified;
  std::vector<std::string> removed;
  std::vector<IntroducedIssue> issuesIntroduced;
  /** Warnings of the placement checks, e.g. LEAK_CHECK_SKIPPED. */
  std::vector<Warning> warnings;
  bool selectionChanged = false;
  bool contextChanged = false;

  bool empty() const;
};

/**
 * Serializes the change lists. Each list is capped at `limit` ids; if any list is
 * truncated, the result contains `"truncated": true` and the full counts.
 */
Json changesToJson(const ChangeReport& report, size_t limit = 500);
Json issuesToJson(const std::vector<IntroducedIssue>& issues, size_t limit = 100);

/**
 * Summarizes the current selection:
 * `{"mode": "none"|"objects"|"faces", "count": n, "ids": [...], "truncated": bool}`.
 */
Json selectionSummary(const mdl::Map& map, const IdRegistry& ids, size_t limit = 50);

/**
 * Collects the changes a call makes to a document by observing its notifiers.
 *
 * Nodes are identified by their canonical ids, so a node in a linked group that was
 * replaced by a clone (and kept its id through aliasing) is reported as modified rather
 * than removed and created.
 *
 * With placement options, the introduced issues also contain the MCP placement problems
 * the call introduced (PlacementTracker).
 */
class ChangeCollector
{
private:
  ui::MapDocument& m_document;
  IdRegistry& m_ids;

  IdList m_added;
  IdList m_removed;
  IdList m_changed;
  /** issue signatures (type + description) of nodes before they changed */
  std::unordered_map<std::string, std::unordered_set<std::string>> m_issuesBefore;
  bool m_selectionChanged = false;
  bool m_contextChanged = false;
  std::unique_ptr<PlacementTracker> m_placement;
  /** Editor validator codes whose issues are not reported (validators_set). */
  std::set<std::string> m_disabledValidators;

  NotifierConnection m_notifierConnection;

public:
  ChangeCollector(
    ui::MapDocument& document,
    IdRegistry& ids,
    std::optional<PlacementTrackerOptions> placement = std::nullopt);
  ~ChangeCollector();

  ChangeCollector(const ChangeCollector&) = delete;
  ChangeCollector& operator=(const ChangeCollector&) = delete;

  /**
   * Stops observing and returns the net changes. Issues are computed against the
   * current state of the document, so call this before rolling back a dry run.
   */
  ChangeReport finish();

private:
  void snapshotIssues(const std::vector<mdl::Node*>& nodes);
  void addRecursively(IdList& list, const mdl::Node& node);
  void addParent(const mdl::Node& node);

  void nodesWereAdded(const std::vector<mdl::Node*>& nodes);
  void nodesWereRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes);
  void nodesWillChange(const std::vector<mdl::Node*>& nodes);
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);
  void nodeStateDidChange(const std::vector<mdl::Node*>& nodes);
  void selectionDidChange(const mdl::SelectionChange& change);
  void contextDidChange();
};

} // namespace mcp
} // namespace tb
