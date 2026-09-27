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

#include "mcp/ChangeCollector.h"

#include "mcp/ObjectIds.h"
#include "mdl/BrushNode.h"
#include "mdl/Issue.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/Selection.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <iterator>
#include <map>
#include <utility>

namespace tb::mcp
{
namespace
{

std::string validatorName(
  const std::vector<const mdl::Validator*>& validators, const mdl::IssueType type)
{
  const auto it = std::ranges::find_if(
    validators, [&](const auto* validator) { return validator->type() == type; });
  return it != validators.end() ? (*it)->description() : std::to_string(type);
}

std::string issueSignature(const mdl::Issue& issue)
{
  return std::to_string(issue.type()) + "|" + issue.description();
}

Json cappedIds(const std::vector<std::string>& ids, const size_t limit)
{
  auto result = Json::array();
  for (size_t i = 0; i < ids.size() && i < limit; ++i)
  {
    result.push_back(ids[i]);
  }
  return result;
}

} // namespace

std::string issueCode(const std::string_view validatorName)
{
  auto result = std::string{};
  auto separator = false;
  for (const auto c : validatorName)
  {
    if (std::isalnum(static_cast<unsigned char>(c)))
    {
      if (separator && !result.empty())
      {
        result.push_back('_');
      }
      separator = false;
      result.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    }
    else
    {
      separator = true;
    }
  }
  return result;
}

IntroducedIssue introducedIssue(McpIssue issue)
{
  return IntroducedIssue{
    std::move(issue.objectId),
    std::move(issue.type),
    std::move(issue.description),
    std::move(issue.code),
    "mcp",
    std::move(issue.details),
  };
}

void removeIssuesWarnedAbout(
  std::vector<IntroducedIssue>& issues, const std::vector<Warning>& warnings)
{
  std::erase_if(issues, [&](const auto& issue) {
    return issue.source == "mcp"
           && std::ranges::any_of(warnings, [&](const auto& warning) {
                return warning.code == issue.code
                       && std::ranges::find(warning.objectIds, issue.objectId)
                            != warning.objectIds.end();
              });
  });
}

bool IdList::add(std::string id)
{
  if (m_set.insert(id).second)
  {
    m_ids.push_back(std::move(id));
    return true;
  }
  return false;
}

bool IdList::contains(const std::string& id) const
{
  return m_set.contains(id);
}

const std::vector<std::string>& IdList::ids() const
{
  return m_ids;
}

size_t IdList::size() const
{
  return m_ids.size();
}

bool IdList::empty() const
{
  return m_ids.empty();
}

bool ChangeReport::empty() const
{
  return created.empty() && modified.empty() && removed.empty();
}

Json changesToJson(
  const ChangeReport& report,
  const ListDetail detail,
  std::vector<TruncatedList>* truncated)
{
  const auto limit = listLimit(detail);
  auto result = Json::object();
  auto counts = Json::object();
  auto byKind = Json::object();
  auto cut = false;
  for (const auto& [name, ids] :
       std::array<std::pair<std::string, const std::vector<std::string>*>, 3>{{
         {"created", &report.created},
         {"modified", &report.modified},
         {"removed", &report.removed},
       }})
  {
    auto all = Json(*ids);
    counts[name] = ids->size();
    if (!ids->empty())
    {
      byKind[name] = countsByKind(all);
    }
    if (ids->size() > limit)
    {
      cut = true;
      auto shown = Json::array();
      for (size_t i = 0; i < limit; ++i)
      {
        shown.push_back((*ids)[i]);
      }
      result[name] = std::move(shown);
      if (truncated)
      {
        truncated->push_back(TruncatedList{"changes." + name, std::move(all)});
      }
    }
    else
    {
      result[name] = std::move(all);
    }
  }
  if (cut)
  {
    result["truncated"] = true;
  }
  if (cut || detail == ListDetail::Summary)
  {
    result["counts"] = std::move(counts);
    result["countsByKind"] = std::move(byKind);
  }
  return result;
}

Json issuesToJson(const std::vector<IntroducedIssue>& issues, const size_t limit)
{
  auto result = Json::array();
  for (size_t i = 0; i < issues.size() && i < limit; ++i)
  {
    auto json = Json{
      {"objectId", issues[i].objectId},
      {"type", issues[i].type},
      {"description", issues[i].description},
      {"code", issues[i].code},
      {"source", issues[i].source},
    };
    if (!issues[i].details.is_null())
    {
      json["details"] = issues[i].details;
    }
    result.push_back(std::move(json));
  }
  return result;
}

Json issuesSummaryJson(
  const std::vector<IntroducedIssue>& issues, const size_t examplesPerCode)
{
  struct CodeSummary
  {
    std::string code;
    std::string source;
    size_t count = 0;
    Json examples = Json::array();
  };

  auto codes = std::vector<CodeSummary>{};
  auto bySource = std::map<std::string, size_t>{};
  for (const auto& issue : issues)
  {
    ++bySource[issue.source];
    auto it = std::ranges::find_if(codes, [&](const auto& summary) {
      return summary.code == issue.code && summary.source == issue.source;
    });
    if (it == codes.end())
    {
      codes.push_back(CodeSummary{issue.code, issue.source});
      it = std::prev(codes.end());
    }
    ++it->count;
    if (it->examples.size() < examplesPerCode)
    {
      it->examples.push_back(
        Json{{"objectId", issue.objectId}, {"description", issue.description}});
    }
  }
  std::ranges::stable_sort(
    codes, std::greater<>{}, [](const auto& summary) { return summary.count; });

  auto byCode = Json::array();
  for (auto& summary : codes)
  {
    byCode.push_back(Json{
      {"code", std::move(summary.code)},
      {"source", std::move(summary.source)},
      {"count", summary.count},
      {"examples", std::move(summary.examples)},
    });
  }
  auto sources = Json::object();
  for (const auto& [source, count] : bySource)
  {
    sources[source] = count;
  }
  return Json{
    {"total", issues.size()},
    {"bySource", std::move(sources)},
    {"byCode", std::move(byCode)},
  };
}

Json selectionSummary(const mdl::Map& map, const IdRegistry& ids, const size_t limit)
{
  const auto& selection = map.selection();
  auto idList = Json::array();
  auto count = size_t{0};
  auto mode = std::string{"none"};

  if (selection.hasBrushFaces())
  {
    mode = "faces";
    count = selection.brushFaces.size();
    for (const auto& handle : selection.brushFaces)
    {
      if (idList.size() >= limit)
      {
        break;
      }
      idList.push_back(ids.formatFace(*handle.node(), handle.faceIndex()));
    }
  }
  else if (selection.hasNodes())
  {
    mode = "objects";
    count = selection.nodes.size();
    for (const auto* node : selection.nodes)
    {
      if (idList.size() >= limit)
      {
        break;
      }
      idList.push_back(ids.format(*node));
    }
  }

  return Json{
    {"mode", mode},
    {"count", count},
    {"ids", std::move(idList)},
    {"truncated", count > limit},
  };
}

ChangeCollector::ChangeCollector(
  ui::MapDocument& document,
  IdRegistry& ids,
  std::optional<PlacementTrackerOptions> placement,
  const bool collectIssues)
  : m_document{document}
  , m_ids{ids}
  , m_collectIssues{collectIssues}
{
  if (placement)
  {
    m_disabledValidators = placement->disabledValidators;
    m_placement =
      std::make_unique<PlacementTracker>(document.map(), ids, std::move(*placement));
  }

  m_notifierConnection +=
    m_document.nodesWereAddedNotifier.connect(this, &ChangeCollector::nodesWereAdded);
  m_notifierConnection += m_document.nodesWillBeRemovedNotifier.connect(
    this, &ChangeCollector::nodesWillBeRemoved);
  m_notifierConnection +=
    m_document.nodesWereRemovedNotifier.connect(this, &ChangeCollector::nodesWereRemoved);
  m_notifierConnection +=
    m_document.nodesWillChangeNotifier.connect(this, &ChangeCollector::nodesWillChange);
  m_notifierConnection +=
    m_document.nodesDidChangeNotifier.connect(this, &ChangeCollector::nodesDidChange);
  m_notifierConnection += m_document.nodeVisibilityDidChangeNotifier.connect(
    this, &ChangeCollector::nodeStateDidChange);
  m_notifierConnection += m_document.nodeLockingDidChangeNotifier.connect(
    this, &ChangeCollector::nodeStateDidChange);
  m_notifierConnection += m_document.selectionDidChangeNotifier.connect(
    this, &ChangeCollector::selectionDidChange);
  m_notifierConnection += m_document.currentLayerDidChangeNotifier.connect(
    this, &ChangeCollector::contextDidChange);
  m_notifierConnection +=
    m_document.groupWasOpenedNotifier.connect(this, &ChangeCollector::contextDidChange);
  m_notifierConnection +=
    m_document.groupWasClosedNotifier.connect(this, &ChangeCollector::contextDidChange);
}

ChangeCollector::~ChangeCollector() = default;

ChangeReport ChangeCollector::finish()
{
  m_notifierConnection.disconnect();

  auto report = ChangeReport{};
  report.selectionChanged = m_selectionChanged;
  report.contextChanged = m_contextChanged;

  for (const auto& id : m_added.ids())
  {
    if (m_removed.contains(id))
    {
      // removed and re-added, e.g. a linked group clone that kept its id
      if (m_ids.resolve(id).is_success())
      {
        m_changed.add(id);
      }
    }
    else
    {
      report.created.push_back(id);
    }
  }

  for (const auto& id : m_removed.ids())
  {
    if (!m_added.contains(id))
    {
      report.removed.push_back(id);
    }
  }

  const auto created =
    std::unordered_set<std::string>{report.created.begin(), report.created.end()};
  const auto removed =
    std::unordered_set<std::string>{report.removed.begin(), report.removed.end()};
  for (const auto& id : m_changed.ids())
  {
    if (!created.contains(id) && !removed.contains(id))
    {
      report.modified.push_back(id);
    }
  }

  // introduced issues
  auto& map = m_document.map();
  const auto validators = map.worldNode().registeredValidators();
  const auto collectIssues = [&](const std::string& id) {
    auto resolved = m_ids.resolve(id);
    if (resolved.is_error())
    {
      return;
    }
    auto* node = resolved.value();

    const auto beforeIt = m_issuesBefore.find(id);
    for (const auto* issue : node->issues(validators))
    {
      if (
        beforeIt == m_issuesBefore.end()
        || !beforeIt->second.contains(issueSignature(*issue)))
      {
        auto name = validatorName(validators, issue->type());
        auto code = issueCode(name);
        if (m_disabledValidators.contains(code))
        {
          continue;
        }
        report.issuesIntroduced.push_back(
          IntroducedIssue{id, std::move(name), issue->description(), std::move(code)});
      }
    }
  };

  if (m_collectIssues)
  {
    for (const auto& id : report.created)
    {
      collectIssues(id);
    }
    for (const auto& id : report.modified)
    {
      collectIssues(id);
    }
  }

  if (m_placement)
  {
    auto placement = m_placement->finish(report.created, report.modified, report.removed);
    for (auto& issue : placement.issues)
    {
      report.issuesIntroduced.push_back(introducedIssue(std::move(issue)));
    }
    report.warnings = std::move(placement.warnings);
    m_placement.reset();
  }

  return report;
}

void ChangeCollector::snapshotIssues(const std::vector<mdl::Node*>& nodes)
{
  if (!m_collectIssues)
  {
    return;
  }
  const auto validators = m_document.map().worldNode().registeredValidators();
  for (auto* node : nodes)
  {
    auto id = m_ids.format(*node);
    if (m_issuesBefore.contains(id) || m_added.contains(id))
    {
      continue;
    }

    auto signatures = std::unordered_set<std::string>{};
    for (const auto* issue : node->issues(validators))
    {
      signatures.insert(issueSignature(*issue));
    }
    m_issuesBefore.emplace(std::move(id), std::move(signatures));
  }
}

void ChangeCollector::addRecursively(IdList& list, const mdl::Node& node)
{
  list.add(m_ids.format(node));
  for (const auto* child : node.children())
  {
    addRecursively(list, *child);
  }
}

void ChangeCollector::addParent(const mdl::Node& node)
{
  if (const auto* parent = node.parent())
  {
    m_changed.add(m_ids.format(*parent));
  }
}

void ChangeCollector::nodesWereAdded(const std::vector<mdl::Node*>& nodes)
{
  if (m_placement)
  {
    m_placement->nodesWereAdded(nodes);
  }
  for (const auto* node : nodes)
  {
    addRecursively(m_added, *node);
    addParent(*node);
  }
}

void ChangeCollector::nodesWereRemoved(const std::vector<mdl::Node*>&)
{
  if (m_placement)
  {
    m_placement->nodesDidChange();
  }
}

void ChangeCollector::nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes)
{
  snapshotIssues(nodes);
  if (m_placement)
  {
    m_placement->nodesWillBeRemoved(nodes);
  }
  for (const auto* node : nodes)
  {
    addRecursively(m_removed, *node);
    addParent(*node);
  }
}

void ChangeCollector::nodesWillChange(const std::vector<mdl::Node*>& nodes)
{
  snapshotIssues(nodes);
  if (m_placement)
  {
    m_placement->nodesWillChange(nodes);
  }
}

void ChangeCollector::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  if (m_placement)
  {
    m_placement->nodesDidChange();
  }
  for (const auto* node : nodes)
  {
    m_changed.add(m_ids.format(*node));
  }
}

void ChangeCollector::nodeStateDidChange(const std::vector<mdl::Node*>& nodes)
{
  for (const auto* node : nodes)
  {
    m_changed.add(m_ids.format(*node));
  }
}

void ChangeCollector::selectionDidChange(const mdl::SelectionChange&)
{
  m_selectionChanged = true;
}

void ChangeCollector::contextDidChange()
{
  m_contextChanged = true;
}

} // namespace tb::mcp
