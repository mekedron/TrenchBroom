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

Json changesToJson(const ChangeReport& report, const size_t limit)
{
  auto result = Json{
    {"created", cappedIds(report.created, limit)},
    {"modified", cappedIds(report.modified, limit)},
    {"removed", cappedIds(report.removed, limit)},
  };
  if (
    report.created.size() > limit || report.modified.size() > limit
    || report.removed.size() > limit)
  {
    result["truncated"] = true;
    result["counts"] = Json{
      {"created", report.created.size()},
      {"modified", report.modified.size()},
      {"removed", report.removed.size()},
    };
  }
  return result;
}

Json issuesToJson(const std::vector<IntroducedIssue>& issues, const size_t limit)
{
  auto result = Json::array();
  for (size_t i = 0; i < issues.size() && i < limit; ++i)
  {
    result.push_back(Json{
      {"objectId", issues[i].objectId},
      {"type", issues[i].type},
      {"description", issues[i].description},
    });
  }
  return result;
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

ChangeCollector::ChangeCollector(ui::MapDocument& document, IdRegistry& ids)
  : m_document{document}
  , m_ids{ids}
{
  m_notifierConnection +=
    m_document.nodesWereAddedNotifier.connect(this, &ChangeCollector::nodesWereAdded);
  m_notifierConnection += m_document.nodesWillBeRemovedNotifier.connect(
    this, &ChangeCollector::nodesWillBeRemoved);
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
        report.issuesIntroduced.push_back(IntroducedIssue{
          id, validatorName(validators, issue->type()), issue->description()});
      }
    }
  };

  for (const auto& id : report.created)
  {
    collectIssues(id);
  }
  for (const auto& id : report.modified)
  {
    collectIssues(id);
  }

  return report;
}

void ChangeCollector::snapshotIssues(const std::vector<mdl::Node*>& nodes)
{
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
  for (const auto* node : nodes)
  {
    addRecursively(m_added, *node);
    addParent(*node);
  }
}

void ChangeCollector::nodesWillBeRemoved(const std::vector<mdl::Node*>& nodes)
{
  snapshotIssues(nodes);
  for (const auto* node : nodes)
  {
    addRecursively(m_removed, *node);
    addParent(*node);
  }
}

void ChangeCollector::nodesWillChange(const std::vector<mdl::Node*>& nodes)
{
  snapshotIssues(nodes);
}

void ChangeCollector::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
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
