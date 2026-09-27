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

#include "mcp/tools/ValidationTools.h"

#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ChangeCollector.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Schema.h"
#include "mcp/ServerState.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/EntityModelUtils.h"
#include "mcp/tools/MaterialKnowledge.h"
#include "mcp/tools/PlacementChecks.h"
#include "mcp/tools/SpaceAnalysis.h"
#include "mdl/Brush.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** The quick fix of the model placement codes: moves the entity by the suggestion. */
constexpr auto MoveFixName = std::string_view{"Apply Suggested Move"};
/** The quick fix of UV_ASPECT_DISTORTION: runs the suggested UV tool call. */
constexpr auto UvFixName = std::string_view{"Apply Suggested UV Fix"};
/** The prefix of MCP issue ids; the rest is the issue's signature. */
constexpr auto McpIssuePrefix = std::string_view{"mcp:"};
/** The prefix of editor issue ids: issue:<runtimeId>:<issueType>:<k>. */
constexpr auto EditorIssuePrefix = std::string_view{"issue:"};

/** Items of the issues resource. */
constexpr auto IssuesResourceLimit = size_t(200);
/** Entries of issue_fix's notFixed list. */
constexpr auto NotFixedLimit = size_t(100);

std::string upper(const std::string_view str)
{
  auto result = std::string{str};
  std::ranges::transform(
    result, result.begin(), [](const unsigned char c) { return char(std::toupper(c)); });
  return result;
}

/** The object id of a face id's brush ("brush:12/face:3" -> "brush:12"). */
std::string brushOfFace(const std::string& id)
{
  const auto slash = id.find("/face:");
  return slash == std::string::npos ? id : id.substr(0, slash);
}

bool isModelPlacementCode(const std::string_view code)
{
  return mcpCheckOfCode(code) == ModelPlacementCheck;
}

void collectDescendants(mdl::Node& node, std::vector<mdl::Node*>& result)
{
  result.push_back(&node);
  for (auto* child : node.children())
  {
    collectDescendants(*child, result);
  }
}

/** Whether the node is part of the map (not removed). */
bool inMap(const mdl::Node& node, const mdl::Map& map)
{
  auto* current = &node;
  while (current->parent())
  {
    current = current->parent();
  }
  return current == &map.worldNode();
}

/** Which issues a call is about. */
struct IssueFilter
{
  bool editor = true;
  bool mcp = true;
  /** Upper case codes or type names; empty: all. */
  std::set<std::string> codes;
  bool includeHidden = false;
  /** The ids of the requested objects and their descendants; nullopt: all objects. */
  std::optional<std::unordered_set<std::string>> objects;
  /** The requested objects and their descendants. */
  std::vector<mdl::Node*> objectNodes;
  /** Editor validator codes and MCP check names that are turned off. */
  std::set<std::string> disabled;

  bool matchesCode(const std::string& code, const std::string& type) const
  {
    return codes.empty() || codes.contains(upper(code)) || codes.contains(upper(type));
  }

  bool wantsCode(const std::string_view code) const
  {
    return codes.empty() || codes.contains(std::string{code});
  }

  bool checkEnabled(const std::string_view check) const
  {
    return !disabled.contains(std::string{check});
  }

  bool matchesObjects(const std::vector<std::string>& ids) const
  {
    return !objects || std::ranges::any_of(ids, [&](const auto& id) {
      return objects->contains(id) || objects->contains(brushOfFace(id));
    });
  }
};

Result<void, ToolError> restrictToObjects(
  IssueFilter& filter, const IdRegistry& ids, const std::vector<std::string>& objectIds)
{
  auto nodes = std::vector<mdl::Node*>{};
  for (const auto& id : objectIds)
  {
    auto node = ids.resolve(id);
    if (node.is_error())
    {
      return errorOf(node);
    }
    collectDescendants(*node.value(), nodes);
  }
  filter.objects.emplace();
  for (auto* node : nodes)
  {
    filter.objects->insert(ids.format(*node));
  }
  filter.objectNodes = std::move(nodes);
  return Result<void, ToolError>{};
}

/** The filter of the arguments sources, codes, includeHidden and ids. */
Result<IssueFilter, ToolError> issueFilter(CallContext& context, const Args& args)
{
  auto filter = IssueFilter{};
  filter.disabled = context.documentState().disabledValidators;
  if (const auto sources = args.getOptional<std::vector<std::string>>("sources"))
  {
    filter.editor = std::ranges::find(*sources, "editor") != sources->end();
    filter.mcp = std::ranges::find(*sources, "mcp") != sources->end();
  }
  for (const auto& code : args.getOr<std::vector<std::string>>("codes", {}))
  {
    filter.codes.insert(upper(code));
  }
  filter.includeHidden = args.getOr<bool>("includeHidden", false);

  if (const auto ids = args.getOptional<std::vector<std::string>>("ids"))
  {
    if (auto result = restrictToObjects(filter, context.ids(), *ids); result.is_error())
    {
      return errorOf(result);
    }
  }
  return filter;
}

std::string validatorName(
  const std::vector<const mdl::Validator*>& validators, const mdl::IssueType type)
{
  const auto it = std::ranges::find_if(
    validators, [&](const auto* validator) { return validator->type() == type; });
  return it != validators.end() ? (*it)->description() : std::to_string(type);
}

std::vector<const mdl::IssueQuickFix*> quickFixesOf(
  const std::vector<const mdl::Validator*>& validators, const mdl::IssueType type)
{
  auto result = std::vector<const mdl::IssueQuickFix*>{};
  for (const auto* validator : validators)
  {
    if (validator->type() == type)
    {
      for (const auto* fix : validator->quickFixes())
      {
        result.push_back(fix);
      }
    }
  }
  return result;
}

Json fixNames(const std::vector<const mdl::IssueQuickFix*>& fixes)
{
  auto result = Json::array();
  for (const auto* fix : fixes)
  {
    result.push_back(fix->description());
  }
  return result;
}

// Editor issues

/** What identifies an editor issue among the issues of its node. */
struct IssueKey
{
  mdl::IssueType type = 0;
  std::string description;
  std::optional<size_t> faceIndex;
  std::optional<std::string> propertyKey;

  auto operator<=>(const IssueKey&) const = default;
};

IssueKey issueKey(const mdl::Issue& issue)
{
  auto key = IssueKey{issue.type(), issue.description(), std::nullopt, std::nullopt};
  if (const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(&issue))
  {
    key.faceIndex = faceIssue->faceIndex();
  }
  if (const auto* propertyIssue = dynamic_cast<const mdl::EntityPropertyIssue*>(&issue))
  {
    key.propertyKey = propertyIssue->propertyKey();
  }
  return key;
}

/**
 * An issue of the editor's validators. Issues are recreated whenever their object
 * changes, so the issue is found again by its node and key.
 */
struct EditorIssue
{
  mdl::Node* node = nullptr;
  mdl::IssueType type = 0;
  std::string description;
  IssueKey key;
  /** The index among the node's issues with the same key. */
  size_t occurrence = 0;
  std::string id;
  std::string code;
  std::string typeName;
  std::string objectId;
  std::optional<std::string> face;
  size_t lineNumber = 0;
  bool hidden = false;
};

/** Finds the current issue object of the given issue, or null if it is gone. */
const mdl::Issue* findIssue(
  const EditorIssue& ref,
  const mdl::Map& map,
  const std::vector<const mdl::Validator*>& validators)
{
  if (!inMap(*ref.node, map))
  {
    return nullptr;
  }
  auto occurrence = size_t(0);
  for (const auto* issue : ref.node->issues(validators))
  {
    if (issueKey(*issue) == ref.key)
    {
      if (occurrence++ == ref.occurrence)
      {
        return issue;
      }
    }
  }
  return nullptr;
}

/** The issues of the node, in the order of Node::issues. */
std::vector<EditorIssue> editorIssuesOf(
  mdl::Node& node,
  const IdRegistry& ids,
  const std::vector<const mdl::Validator*>& validators)
{
  auto result = std::vector<EditorIssue>{};
  const auto objectId = ids.format(node);
  auto perType = std::map<mdl::IssueType, size_t>{};
  auto perKey = std::map<IssueKey, size_t>{};
  for (const auto* issue : node.issues(validators))
  {
    auto ref = EditorIssue{};
    ref.node = &node;
    ref.type = issue->type();
    ref.description = issue->description();
    ref.key = issueKey(*issue);
    ref.occurrence = perKey[ref.key]++;
    ref.id = fmt::format(
      "{}{}:{}:{}", EditorIssuePrefix, node.runtimeId(), ref.type, perType[ref.type]++);
    ref.typeName = validatorName(validators, ref.type);
    ref.code = issueCode(ref.typeName);
    ref.objectId = objectId;
    if (const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issue))
    {
      ref.face =
        ids.formatFace(static_cast<const mdl::BrushNode&>(node), faceIssue->faceIndex());
    }
    ref.lineNumber = issue->lineNumber();
    ref.hidden = issue->hidden();
    result.push_back(std::move(ref));
  }
  return result;
}

/** The issues of the editor's validators that pass the filter, in tree order. */
std::vector<EditorIssue> findEditorIssues(
  mdl::Map& map, const IdRegistry& ids, const IssueFilter& filter)
{
  const auto validators = map.worldNode().registeredValidators();
  auto result = std::vector<EditorIssue>{};

  const auto visit = [&](const auto& self, mdl::Node& node) -> void {
    for (auto& ref : editorIssuesOf(node, ids, validators))
    {
      auto objectIds = std::vector<std::string>{ref.objectId};
      if (ref.face)
      {
        objectIds.push_back(*ref.face);
      }
      if (
        (ref.hidden && !filter.includeHidden) || !filter.checkEnabled(ref.code)
        || !filter.matchesCode(ref.code, ref.typeName)
        || !filter.matchesObjects(objectIds))
      {
        continue;
      }
      result.push_back(std::move(ref));
    }
    for (auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());
  return result;
}

/** Resolves an editor issue id (issue:<runtimeId>:<issueType>:<k>). */
std::optional<EditorIssue> resolveEditorIssue(
  const std::string& id, mdl::Map& map, const IdRegistry& ids)
{
  const auto parts = kdl::str_split(id.substr(EditorIssuePrefix.size()), ":");
  if (parts.size() != 3)
  {
    return std::nullopt;
  }
  const auto runtimeId = kdl::str_to_size(parts[0]);
  if (!runtimeId)
  {
    return std::nullopt;
  }
  auto* node = ids.findByRuntimeId(mdl::IdType(*runtimeId));
  if (!node || !inMap(*node, map))
  {
    return std::nullopt;
  }
  const auto validators = map.worldNode().registeredValidators();
  for (auto& ref : editorIssuesOf(*node, ids, validators))
  {
    if (ref.id == id)
    {
      return std::move(ref);
    }
  }
  return std::nullopt;
}

Json toJson(const EditorIssue& ref, const std::vector<const mdl::Validator*>& validators)
{
  auto item = Json{
    {"id", ref.id},
    {"source", "editor"},
    {"code", ref.code},
    {"type", ref.typeName},
    {"description", ref.description},
    {"objectId", ref.objectId},
  };
  if (ref.face)
  {
    item["face"] = *ref.face;
  }
  item["lineNumber"] = ref.lineNumber > 0 ? Json(ref.lineNumber) : Json(nullptr);
  item["hidden"] = ref.hidden;
  item["fixes"] = fixNames(quickFixesOf(validators, ref.type));
  return item;
}

// MCP issues

std::string mcpIssueId(const McpIssue& issue)
{
  return std::string{McpIssuePrefix} + issue.signature;
}

/** The name of the MCP fix for the issue, if it has one. */
std::optional<std::string_view> mcpFixName(const McpIssue& issue)
{
  if (isModelPlacementCode(issue.code) && issue.details.contains("suggestedMove"))
  {
    return MoveFixName;
  }
  if (issue.code == UvDistortionCode && issue.details.contains("fix"))
  {
    return UvFixName;
  }
  return std::nullopt;
}

Json toJson(const McpIssue& issue)
{
  auto fixes = Json::array();
  if (const auto fix = mcpFixName(issue))
  {
    fixes.push_back(*fix);
  }
  return Json{
    {"id", mcpIssueId(issue)},
    {"source", "mcp"},
    {"code", issue.code},
    {"type", issue.type},
    {"description", issue.description},
    {"objectId", issue.objectId},
    {"lineNumber", nullptr},
    {"hidden", false},
    {"fixes", std::move(fixes)},
    {"details", issue.details},
  };
}

struct McpIssues
{
  std::vector<McpIssue> issues;
  /** {analyzed, skippedReason} of the leak prediction, or null if it did not run. */
  Json leakCheck = nullptr;
};

/** The MCP checks on the whole map or the filter's objects. */
McpIssues findMcpIssues(
  mdl::Map& map,
  const IdRegistry& ids,
  const std::optional<std::filesystem::path>& knowledgeDirectory,
  const IssueFilter& filter)
{
  auto result = McpIssues{};
  auto& issues = result.issues;

  const auto wants = [&](const McpCheck& check) {
    return filter.checkEnabled(check.name)
           && std::ranges::any_of(
             check.codes, [&](const auto& code) { return filter.wantsCode(code); });
  };
  const auto checks = mcpChecks();
  const auto wantsCheck = [&](const std::string_view name) {
    const auto it =
      std::ranges::find_if(checks, [&](const auto& check) { return check.name == name; });
    return it != checks.end() && wants(*it);
  };

  // the objects to check
  auto brushes = std::vector<const mdl::BrushNode*>{};
  auto uvBrushes = std::vector<mdl::BrushNode*>{};
  auto entities = std::vector<mdl::EntityNode*>{};
  auto allNodes = std::vector<mdl::Node*>{};
  if (!filter.objects)
  {
    collectDescendants(map.worldNode(), allNodes);
  }
  for (auto* node : filter.objects ? filter.objectNodes : allNodes)
  {
    if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
    {
      brushes.push_back(brushNode);
      uvBrushes.push_back(brushNode);
    }
  }
  auto world = std::vector<mdl::Node*>{&map.worldNode()};
  entities = pointEntities(filter.objects ? filter.objectNodes : world);

  if (wantsCheck(ZFightingCode))
  {
    const auto pairs = filter.objects ? findZFighting(map, &brushes) : findZFighting(map);
    auto found = zFightingIssues(pairs, ids);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  if (wantsCheck(EntityOutsideHullCode))
  {
    const auto report = predictLeaks(map);
    result.leakCheck = Json{
      {"analyzed", report.analyzed},
      {"skippedReason", report.analyzed ? Json(nullptr) : Json(report.skippedReason)},
    };
    auto found = leakIssues(report, ids);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  if (wantsCheck(ModelPlacementCheck) && !entities.empty())
  {
    auto loader = EntityModelLoader{map};
    auto found = modelPlacementIssues(map, ids, entities, loader);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  if (wantsCheck(UvDistortionCode) && !uvBrushes.empty())
  {
    auto faces = std::vector<mdl::BrushFaceHandle>{};
    for (auto* brushNode : uvBrushes)
    {
      for (size_t i = 0; i < brushNode->brush().faceCount(); ++i)
      {
        faces.emplace_back(brushNode, i);
      }
    }
    auto knowledge = MaterialKnowledge{map, knowledgeDirectory};
    auto found = uvDistortionIssues(faces, map, knowledge, ids);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  std::erase_if(issues, [&](const auto& issue) {
    return !filter.matchesCode(issue.code, issue.type)
           || !filter.matchesObjects(issue.objectIds);
  });
  return result;
}

/** The code and object of an MCP issue id (mcp:<code>|<object>|...). */
std::optional<std::pair<std::string, std::string>> parseMcpIssueId(const std::string& id)
{
  const auto parts = kdl::str_split(id.substr(McpIssuePrefix.size()), "|");
  if (parts.size() < 2)
  {
    return std::nullopt;
  }
  return std::pair{parts[0], parts[1]};
}

/** Finds the MCP issue with the given id by running its check on its object. */
std::optional<McpIssue> resolveMcpIssue(
  const std::string& id,
  mdl::Map& map,
  const IdRegistry& ids,
  const std::optional<std::filesystem::path>& knowledgeDirectory)
{
  const auto parsed = parseMcpIssueId(id);
  if (!parsed)
  {
    return std::nullopt;
  }
  auto filter = IssueFilter{};
  filter.codes.insert(parsed->first);
  if (parsed->first != EntityOutsideHullCode)
  {
    if (restrictToObjects(filter, ids, {parsed->second}).is_error())
    {
      return std::nullopt;
    }
  }
  for (auto& issue : findMcpIssues(map, ids, knowledgeDirectory, filter).issues)
  {
    if (mcpIssueId(issue) == id)
    {
      return std::move(issue);
    }
  }
  return std::nullopt;
}

// issues_list

void addCounts(Json& result, const std::map<std::string, size_t>& counts)
{
  auto countsJson = Json::object();
  for (const auto& [code, count] : counts)
  {
    countsJson[code] = count;
  }
  result["counts"] = std::move(countsJson);
}

Json disabledJson(const std::set<std::string>& disabled)
{
  auto result = Json::array();
  for (const auto& name : disabled)
  {
    result.push_back(name);
  }
  return result;
}

/** All issues that pass the filter as JSON, with counts per code. */
Json listIssues(
  mdl::Map& map,
  const IdRegistry& ids,
  const std::optional<std::filesystem::path>& knowledgeDirectory,
  const IssueFilter& filter,
  std::vector<Json>& items,
  std::map<std::string, size_t>& counts)
{
  auto leakCheck = Json(nullptr);
  if (filter.editor)
  {
    const auto validators = map.worldNode().registeredValidators();
    for (const auto& ref : findEditorIssues(map, ids, filter))
    {
      counts[ref.code] += 1;
      items.push_back(toJson(ref, validators));
    }
  }
  if (filter.mcp)
  {
    auto found = findMcpIssues(map, ids, knowledgeDirectory, filter);
    for (const auto& issue : found.issues)
    {
      counts[issue.code] += 1;
      items.push_back(toJson(issue));
    }
    leakCheck = std::move(found.leakCheck);
  }
  return leakCheck;
}

ToolResult issuesList(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }
  auto filter = issueFilter(context, args);
  if (filter.is_error())
  {
    return errorOf(filter);
  }

  auto items = std::vector<Json>{};
  auto counts = std::map<std::string, size_t>{};
  auto leakCheck = listIssues(
    map,
    context.ids(),
    context.host().knowledgeDirectory(),
    filter.value(),
    items,
    counts);

  auto result = makePage(items, request.value(), map.modificationCount());
  addCounts(result, counts);
  if (!leakCheck.is_null())
  {
    result["leakCheck"] = std::move(leakCheck);
  }
  if (!filter.value().disabled.empty())
  {
    result["disabledValidators"] = disabledJson(filter.value().disabled);
  }
  return result;
}

// issue_fix, issue_hide, issue_show

/** The issues named by the arguments issues, codes, ids and includeHidden. */
struct IssueTargets
{
  std::vector<EditorIssue> editor;
  std::vector<McpIssue> mcp;
  /** Issue ids that were not found. */
  std::vector<std::string> notFound;
  /** Requested MCP codes whose checks have no fix and were not run. */
  std::vector<std::string> unfixableCodes;
};

/**
 * Resolves the issues the arguments name. With `forFix`, only the MCP checks that have
 * fixes run (model placement and texture distortion) unless an unfixable code is named.
 */
Result<IssueTargets, ToolError> issueTargets(
  CallContext& context, const Args& args, const bool forFix, const bool includeHidden)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto knowledgeDirectory = context.host().knowledgeDirectory();

  const auto issueIds = args.getOr<std::vector<std::string>>("issues", {});
  if (issueIds.empty() && !args.has("codes") && !args.has("ids"))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Name the issues: pass issue ids (issues), codes or object ids (ids).",
      "Use issues_list to find the issues, their ids and their fixes.");
  }

  auto targets = IssueTargets{};
  auto seen = std::unordered_set<std::string>{};
  for (const auto& id : issueIds)
  {
    if (!seen.insert(id).second)
    {
      continue;
    }
    if (id.starts_with(EditorIssuePrefix))
    {
      if (auto ref = resolveEditorIssue(id, map, ids))
      {
        targets.editor.push_back(std::move(*ref));
        continue;
      }
    }
    else if (id.starts_with(McpIssuePrefix))
    {
      if (auto issue = resolveMcpIssue(id, map, ids, knowledgeDirectory))
      {
        targets.mcp.push_back(std::move(*issue));
        continue;
      }
    }
    targets.notFound.push_back(id);
  }

  if (args.has("codes") || args.has("ids"))
  {
    auto filterResult = issueFilter(context, args);
    if (filterResult.is_error())
    {
      return errorOf(filterResult);
    }
    auto filter = filterResult.value();
    filter.includeHidden = includeHidden;
    for (auto& ref : findEditorIssues(map, ids, filter))
    {
      if (seen.insert(ref.id).second)
      {
        targets.editor.push_back(std::move(ref));
      }
    }

    if (forFix)
    {
      // the checks without fixes are expensive and would only be skipped
      for (const auto& check : mcpChecks())
      {
        if (check.name == ZFightingCode || check.name == EntityOutsideHullCode)
        {
          if (!filter.codes.empty() && filter.wantsCode(check.name))
          {
            targets.unfixableCodes.push_back(check.name);
          }
          filter.disabled.insert(check.name);
        }
      }
    }
    for (auto& issue : findMcpIssues(map, ids, knowledgeDirectory, filter).issues)
    {
      if (seen.insert(mcpIssueId(issue)).second)
      {
        targets.mcp.push_back(std::move(issue));
      }
    }
  }

  if (
    targets.editor.empty() && targets.mcp.empty() && !targets.notFound.empty()
    && !args.has("codes") && !args.has("ids"))
  {
    return makeError(
      ErrorCode::ObjectNotFound,
      fmt::format(
        "No issue with the id(s) {} exists; they were fixed already or the ids are "
        "stale.",
        kdl::str_join(targets.notFound, ", ")),
      "Use issues_list to get the current issues and their ids.");
  }
  return targets;
}

/** Runs another map tool inside the current call (the suggested fix of an MCP issue). */
ToolResult runNestedTool(CallContext& context, const std::string& name, const Json& args)
{
  const auto* tool = context.server().tools.find(name);
  if (!tool || tool->mutation() != Mutation::Map || tool->isAsync())
  {
    return makeError(
      ErrorCode::InternalError, fmt::format("The fix tool {} is not available.", name));
  }
  auto errors = std::vector<SchemaError>{};
  auto validated = tool->inputSchema().validate(args, errors);
  if (!validated)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The suggested {} call is invalid: {}",
        name,
        errors.empty() ? std::string{} : errors.front().message),
      fmt::format(
        "Make the change by hand with {} or turn the check off with validators_set.",
        name));
  }
  return tool->handler()(context, Args{std::move(*validated)});
}

struct NotFixed
{
  std::string id;
  std::string code;
  std::string objectId;
  std::string reason;
};

Json toJson(const NotFixed& entry)
{
  return Json{
    {"id", entry.id},
    {"code", entry.code},
    {"objectId", entry.objectId},
    {"reason", entry.reason},
  };
}

/** The fixes applied by issue_fix, per fix name and code. */
using AppliedCounts = std::map<std::pair<std::string, std::string>, size_t>;

void fixEditorIssues(
  CallContext& context,
  const std::vector<EditorIssue>& targets,
  const std::optional<std::string>& fixName,
  AppliedCounts& applied,
  std::vector<std::string>& fixed,
  std::vector<NotFixed>& notFixed)
{
  auto& map = context.map();
  const auto validators = map.worldNode().registeredValidators();

  // choose the fix of each issue; group the issues by fix
  auto groups =
    std::vector<std::pair<const mdl::IssueQuickFix*, std::vector<EditorIssue>>>{};
  for (const auto& ref : targets)
  {
    const auto fixes = quickFixesOf(validators, ref.type);
    const auto* fix = static_cast<const mdl::IssueQuickFix*>(nullptr);
    if (fixes.empty())
    {
      notFixed.push_back(
        {ref.id, ref.code, ref.objectId, "The editor has no quick fix for this issue."});
      continue;
    }
    if (fixName)
    {
      const auto it = std::ranges::find_if(fixes, [&](const auto* candidate) {
        return kdl::ci::str_is_equal(candidate->description(), *fixName);
      });
      if (it == fixes.end())
      {
        notFixed.push_back(
          {ref.id,
           ref.code,
           ref.objectId,
           fmt::format(
             "The fix '{}' does not apply to this issue; its fixes: {}.",
             *fixName,
             fixNames(fixes).dump())});
        continue;
      }
      fix = *it;
    }
    else if (fixes.size() > 1)
    {
      notFixed.push_back(
        {ref.id,
         ref.code,
         ref.objectId,
         fmt::format(
           "The issue has several fixes {}; choose one with 'fix'.",
           fixNames(fixes).dump())});
      continue;
    }
    else
    {
      fix = fixes.front();
    }

    auto group =
      std::ranges::find_if(groups, [&](const auto& entry) { return entry.first == fix; });
    if (group == groups.end())
    {
      groups.emplace_back(fix, std::vector<EditorIssue>{});
      group = std::prev(groups.end());
    }
    group->second.push_back(ref);
  }

  for (const auto& [fix, refs] : groups)
  {
    // Fixes invalidate the issues of the objects they change, so each round applies the
    // fix to at most one issue per object, like the Issues view would with issues
    // selected in different rows. The Issues view selects nothing when a world issue is
    // among the selected ones, so world issues are fixed alone.
    auto remaining = refs;
    while (!remaining.empty())
    {
      auto batch = std::vector<const mdl::Issue*>{};
      auto next = std::vector<EditorIssue>{};
      auto nodes = std::unordered_set<const mdl::Node*>{};
      auto worldBatch = false;
      for (const auto& ref : remaining)
      {
        if (const auto* issue = findIssue(ref, map, validators))
        {
          const auto isWorld = ref.node == &map.worldNode();
          if (!worldBatch && (!isWorld || batch.empty()) && nodes.insert(ref.node).second)
          {
            batch.push_back(issue);
            worldBatch = isWorld;
          }
          else
          {
            next.push_back(ref);
          }
        }
      }
      if (batch.empty())
      {
        break;
      }

      // select the issues' objects as the Issues view does
      auto selectable = std::vector<mdl::Node*>{};
      for (const auto* issue : batch)
      {
        if (!issue->addSelectableNodes(selectable))
        {
          selectable.clear();
          break;
        }
      }
      std::ignore = withTargets(context, selectable, [&]() -> ToolResult {
        fix->apply(map, batch);
        return Json::object();
      });
      remaining = std::move(next);
    }

    for (const auto& ref : refs)
    {
      if (findIssue(ref, map, validators))
      {
        notFixed.push_back(
          {ref.id,
           ref.code,
           ref.objectId,
           fmt::format("The issue is still present after '{}'.", fix->description())});
      }
      else
      {
        applied[{fix->description(), ref.code}] += 1;
        fixed.push_back(ref.id);
      }
    }
  }
}

void fixMcpIssues(
  CallContext& context,
  const std::vector<McpIssue>& targets,
  const std::optional<std::string>& fixName,
  AppliedCounts& applied,
  std::vector<std::string>& fixed,
  std::vector<NotFixed>& notFixed)
{
  auto& map = context.map();
  const auto knowledgeDirectory = context.host().knowledgeDirectory();

  for (const auto& issue : targets)
  {
    const auto id = mcpIssueId(issue);
    const auto fix = mcpFixName(issue);
    if (!fix)
    {
      notFixed.push_back(
        {id,
         issue.code,
         issue.objectId,
         "This MCP check has no automatic fix; follow the advice in the description."});
      continue;
    }
    if (fixName && !kdl::ci::str_is_equal(*fix, *fixName))
    {
      notFixed.push_back(
        {id,
         issue.code,
         issue.objectId,
         fmt::format(
           "The fix '{}' does not apply to this issue; its fix: '{}'.", *fixName, *fix)});
      continue;
    }

    const auto result =
      *fix == MoveFixName
        ? runNestedTool(
            context,
            "objects_move",
            Json{
              {"ids", Json{issue.objectId}}, {"vector", issue.details["suggestedMove"]}})
        : runNestedTool(
            context,
            issue.details["fix"]["tool"].get<std::string>(),
            issue.details["fix"]["arguments"]);
    if (result.is_error())
    {
      notFixed.push_back({id, issue.code, issue.objectId, errorOf(result).message});
      continue;
    }

    if (resolveMcpIssue(id, map, context.ids(), knowledgeDirectory))
    {
      notFixed.push_back(
        {id,
         issue.code,
         issue.objectId,
         fmt::format("The issue is still present after '{}'.", *fix)});
    }
    else
    {
      applied[{std::string{*fix}, issue.code}] += 1;
      fixed.push_back(id);
    }
  }
}

ToolResult issueFix(CallContext& context, const Args& args)
{
  auto targets = issueTargets(context, args, true, args.get<bool>("includeHidden"));
  if (targets.is_error())
  {
    return errorOf(targets);
  }
  const auto fixName = args.getOptional<std::string>("fix");

  auto applied = AppliedCounts{};
  auto fixed = std::vector<std::string>{};
  auto notFixed = std::vector<NotFixed>{};
  for (const auto& id : targets.value().notFound)
  {
    notFixed.push_back(
      {id, "", "", "No such issue: it was fixed already or the id is stale."});
  }
  for (const auto& code : targets.value().unfixableCodes)
  {
    notFixed.push_back(
      {"",
       code,
       "",
       "This MCP check has no automatic fix; list its issues with issues_list and follow "
       "the advice in their descriptions."});
  }

  fixEditorIssues(context, targets.value().editor, fixName, applied, fixed, notFixed);
  fixMcpIssues(context, targets.value().mcp, fixName, applied, fixed, notFixed);

  if (fixed.empty())
  {
    context.warn(
      "NOTHING_FIXED",
      targets.value().editor.empty() && targets.value().mcp.empty()
        ? "No issue matches the request."
        : "None of the issues could be fixed; see notFixed for the reasons.");
  }

  auto appliedJson = Json::array();
  for (const auto& [key, count] : applied)
  {
    appliedJson.push_back(
      Json{{"fix", key.first}, {"code", key.second}, {"count", count}});
  }
  auto notFixedJson = Json::array();
  for (size_t i = 0; i < notFixed.size() && i < NotFixedLimit; ++i)
  {
    notFixedJson.push_back(toJson(notFixed[i]));
  }
  return Json{
    {"fixedCount", fixed.size()},
    {"fixed", fixed},
    {"applied", std::move(appliedJson)},
    {"notFixedCount", notFixed.size()},
    {"notFixed", std::move(notFixedJson)},
  };
}

ToolResult setIssuesHidden(CallContext& context, const Args& args, const bool hidden)
{
  auto targets = issueTargets(context, args, false, true);
  if (targets.is_error())
  {
    return errorOf(targets);
  }

  auto& map = context.map();
  const auto validators = map.worldNode().registeredValidators();
  auto changed = std::vector<std::string>{};
  auto unchanged = std::vector<std::string>{};
  auto skipped = Json::array();
  for (const auto& id : targets.value().notFound)
  {
    skipped.push_back(Json{{"id", id}, {"reason", "No such issue."}});
  }
  for (const auto& ref : targets.value().editor)
  {
    const auto* issue = findIssue(ref, map, validators);
    if (!issue || issue->hidden() == hidden)
    {
      unchanged.push_back(ref.id);
      continue;
    }
    if (!context.dryRun())
    {
      map.setIssueHidden(*issue, hidden);
    }
    changed.push_back(ref.id);
  }
  for (const auto& issue : targets.value().mcp)
  {
    skipped.push_back(Json{
      {"id", mcpIssueId(issue)},
      {"reason",
       "MCP issues cannot be hidden; turn their check off with validators_set."},
    });
  }

  if (!changed.empty() && !context.dryRun())
  {
    context.server().scheduleDocumentUpdate(context.document(), DocumentAspect::Issues);
  }
  return Json{
    {hidden ? "hidden" : "shown", changed},
    {"unchanged", unchanged},
    {"skipped", std::move(skipped)},
  };
}

// validators_list, validators_set

struct ValidatorInfo
{
  std::string name;
  std::string source;
  std::string title;
  std::vector<std::string> codes;
  Json fixes;
};

std::vector<ValidatorInfo> allValidators(const mdl::Map& map)
{
  auto result = std::vector<ValidatorInfo>{};
  const auto validators = map.worldNode().registeredValidators();
  for (const auto* validator : validators)
  {
    const auto code = issueCode(validator->description());
    result.push_back(
      {code,
       "editor",
       validator->description(),
       {code},
       fixNames(validator->quickFixes())});
  }
  for (const auto& check : mcpChecks())
  {
    auto fixes = Json::array();
    if (check.name == ModelPlacementCheck)
    {
      fixes.push_back(MoveFixName);
    }
    else if (check.name == UvDistortionCode)
    {
      fixes.push_back(UvFixName);
    }
    result.push_back({check.name, "mcp", check.title, check.codes, std::move(fixes)});
  }
  return result;
}

Json validatorsJson(const mdl::Map& map, const std::set<std::string>& disabled)
{
  auto items = Json::array();
  for (const auto& validator : allValidators(map))
  {
    items.push_back(Json{
      {"name", validator.name},
      {"source", validator.source},
      {"title", validator.title},
      {"codes", validator.codes},
      {"enabled", !disabled.contains(validator.name)},
      {"fixes", validator.fixes},
    });
  }
  return Json{{"validators", std::move(items)}, {"disabled", disabledJson(disabled)}};
}

ToolResult validatorsList(CallContext& context, const Args&)
{
  return validatorsJson(context.map(), context.documentState().disabledValidators);
}

ToolResult validatorsSet(CallContext& context, const Args& args)
{
  const auto validators = allValidators(context.map());
  const auto find = [&](const std::string& name) -> std::optional<std::string> {
    for (const auto& validator : validators)
    {
      if (
        kdl::ci::str_is_equal(validator.name, name)
        || kdl::ci::str_is_equal(validator.title, name))
      {
        return validator.name;
      }
    }
    return std::nullopt;
  };

  auto disabled = args.get<bool>("enableAll")
                    ? std::set<std::string>{}
                    : context.documentState().disabledValidators;
  auto unknown = std::vector<std::string>{};
  for (const auto& [key, enable] :
       {std::pair{"enable", true}, std::pair{"disable", false}})
  {
    for (const auto& name : args.getOr<std::vector<std::string>>(key, {}))
    {
      if (const auto found = find(name))
      {
        if (enable)
        {
          disabled.erase(*found);
        }
        else
        {
          disabled.insert(*found);
        }
      }
      else
      {
        unknown.push_back(name);
      }
    }
  }
  if (!unknown.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      fmt::format("Unknown validators: {}.", kdl::str_join(unknown, ", ")),
      "Use validators_list for the validator names.");
  }

  auto& current = context.documentState().disabledValidators;
  auto changed = Json::array();
  for (const auto& validator : validators)
  {
    if (current.contains(validator.name) != disabled.contains(validator.name))
    {
      changed.push_back(validator.name);
    }
  }
  if (!context.dryRun() && !changed.empty())
  {
    current = disabled;
    context.server().scheduleDocumentUpdate(context.document(), DocumentAspect::Issues);
  }

  auto result = validatorsJson(context.map(), disabled);
  result["changed"] = std::move(changed);
  return result;
}

// schemas

Schema issueSchema()
{
  return object(
           {
             field("id", string())
               .required()
               .describe(
                 "For issue_fix / issue_hide / issue_show. Editor issues: "
                 "issue:<runtimeId>:<issueType>:<k>; MCP issues: mcp:<signature>"),
             field("source", enumOf({"editor", "mcp"})).required(),
             field("code", string()).required(),
             field("type", string()).required(),
             field("description", string()).required(),
             field("objectId", string()).required(),
             field("face", string()).describe("The face of a face issue"),
             field("lineNumber", any()).describe("Line in the map file, or null"),
             field("hidden", boolean()),
             field("fixes", array(string()))
               .describe("The names of the fixes issue_fix can apply to this issue"),
             field("details", any()).describe("MCP checks: face ids, positions, bounds"),
           })
    .allowAdditionalProperties();
}

std::vector<Field> issueTargetFields(const std::string& verb)
{
  return {
    field("issues", array(string()).nonEmpty())
      .describe("Issue ids from issues_list (editor issue:... and MCP mcp:... ids)"),
    field("codes", array(string()).nonEmpty())
      .describe(
        "All issues with these codes or editor type names (case-insensitive), e.g. "
        "[\"EMPTY_PROPERTY_VALUE\"]; combined with ids: only issues of those objects"),
    field("ids", array(string()).nonEmpty())
      .describe(fmt::format(
        "All issues of these objects and their contents (brush, entity, group, layer "
        "ids) that {}",
        verb)),
  };
}

} // namespace

Json issuesResource(ServerState& state, const DocumentInfo& document)
{
  auto& documentState = state.documentState(*document.document);
  auto& map = document.document->map();
  auto filter = IssueFilter{};
  filter.disabled = documentState.disabledValidators;

  auto items = std::vector<Json>{};
  auto counts = std::map<std::string, size_t>{};
  auto leakCheck = listIssues(
    map, documentState.ids, state.host.knowledgeDirectory(), filter, items, counts);

  const auto total = items.size();
  if (items.size() > IssuesResourceLimit)
  {
    items.resize(IssuesResourceLimit);
  }
  auto result = Json{
    {"document", document.id},
    {"total", total},
    {"truncated", total > IssuesResourceLimit},
    {"items", std::move(items)},
    {"leakCheck", std::move(leakCheck)},
    {"disabledValidators", disabledJson(filter.disabled)},
  };
  addCounts(result, counts);
  return result;
}

void registerValidationTools(ToolRegistry& registry)
{
  auto codes = mcpIssueCodes();

  registry.add(
    ToolDef{"issues_list"}
      .title("List Issues")
      .description(
        "Lists the problems of the whole map (read-only): the issues of the editor's "
        "validators (source \"editor\", as the Issues view shows them; codes are the "
        "validator names in UPPER_SNAKE case, e.g. EMPTY_BRUSH_ENTITY) and the MCP "
        "placement checks (source \"mcp\"): Z_FIGHTING (coplanar overlapping faces of "
        "different brushes), ENTITY_OUTSIDE_HULL (point entities the void reaches: the "
        "map leaks), MODEL_BELOW_FLOOR / MODEL_FLOATING / MODEL_PENETRATES_BRUSHES / "
        "MODEL_NO_FLOOR (entity models against brushes) and UV_ASPECT_DISTORTION "
        "(stretched textures). Each item has an id for issue_fix / issue_hide / "
        "issue_show, the object id, the map file line, whether it is hidden, the names "
        "of its fixes and, for MCP issues, details (face ids, the nearest gap, a "
        "suggested move or fix); the result also has counts per code and leakCheck. "
        "Modifying calls already report the problems they introduce in "
        "issuesIntroduced; use this tool for the whole map and map_check for gameplay "
        "problems. Validators turned off with validators_set are skipped. Examples: "
        "{\"codes\": [\"Z_FIGHTING\"]}; {\"sources\": [\"editor\"], \"ids\": "
        "[\"entity:7\"], \"includeHidden\": true}")
      .input(object({
        field("sources", array(enumOf({"editor", "mcp"})).nonEmpty())
          .describe(
            "Only issues of these sources: 'editor' (the editor's validators), 'mcp' "
            "(the MCP placement checks). Default: both"),
        field("codes", array(string()).nonEmpty())
          .describe(
            "Only these codes or editor type names, e.g. [\"Z_FIGHTING\", "
            "\"EMPTY_BRUSH_ENTITY\"]. MCP codes: "
            + kdl::str_join(codes, ", ")
            + ". Default: all (the leak check is only run when requested or with no "
              "filter)"),
        field("ids", array(string()).nonEmpty())
          .describe(
            "Only issues of these objects and their contents (brush, entity, group, "
            "layer ids). Default: the whole map"),
        field("includeHidden", boolean().defaultsTo(false))
          .describe("Include editor issues hidden in the Issues view"),
      }))
      .output(object({
        field("items", array(issueSchema())).required(),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("counts", any()).required().describe("Issues per code (all pages)"),
        field("leakCheck", any())
          .describe("{analyzed, skippedReason} of the leak prediction behind "
                    "ENTITY_OUTSIDE_HULL, if it ran"),
        field("disabledValidators", array(string()))
          .describe("Validators turned off with validators_set, if any"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(issuesList));

  auto fixFields = issueTargetFields("have a fix");
  fixFields.push_back(
    field("fix", string())
      .describe("The fix to apply (a name from the issues' fixes, "
                "case-insensitive). Default: the only fix of each issue; "
                "issues with several fixes are skipped"));
  fixFields.push_back(field("includeHidden", boolean().defaultsTo(false))
                        .describe("With codes or ids: also fix hidden editor issues"));
  registry.add(
    ToolDef{"issue_fix"}
      .title("Fix Issues")
      .description(
        "Applies quick fixes to issues in one undo step: the editor's quick fixes, as "
        "the Issues view applies them (Delete Objects, Delete Property, Snap Vertices, "
        "Move Brushes to World, Remove Mod, Reset UV Scale, Replace quotation marks, "
        "Truncate Value, ...), and the fixes of the MCP checks: '"
        + std::string{MoveFixName}
        + "' (MODEL_* issues with a suggested move: objects_move by "
          "details.suggestedMove) and '"
        + std::string{UvFixName}
        + "' (UV_ASPECT_DISTORTION: runs details.fix). Z_FIGHTING and "
          "ENTITY_OUTSIDE_HULL have no automatic fix. Name the issues by their ids from "
          "issues_list (issues), by codes and/or by object ids; an issue with several "
          "fixes is skipped unless 'fix' names one. Returns fixedCount, fixed, applied "
          "(count per fix and code) and notFixed with the reason for each issue that was "
          "not fixed; the change report lists every deleted or changed object. "
          "Examples: {\"codes\": [\"EMPTY_BRUSH_ENTITY\"]}; {\"issues\": "
          "[\"issue:41:16:0\"], \"fix\": \"Delete Property\"}; {\"ids\": [\"entity:7\"], "
          "\"fix\": \"Apply Suggested Move\"}")
      .input(object(std::move(fixFields)))
      .output(object({
        field("fixedCount", integer()).required(),
        field("fixed", array(string())).required().describe("Ids of the fixed issues"),
        field(
          "applied",
          array(object({
            field("fix", string()).required(),
            field("code", string()).required(),
            field("count", integer()).required(),
          })))
          .required(),
        field("notFixedCount", integer()).required(),
        field(
          "notFixed",
          array(object({
            field("id", string()).required(),
            field("code", string()).required(),
            field("objectId", string()).required(),
            field("reason", string()).required(),
          })))
          .required()
          .describe("At most 100 entries"),
      }))
      .mutation(Mutation::Map)
      .destructive()
      .handler(issueFix));

  for (const auto hide : {true, false})
  {
    auto fields = issueTargetFields(hide ? "are shown" : "are hidden");
    registry.add(
      ToolDef{hide ? "issue_hide" : "issue_show"}
        .title(hide ? "Hide Issues" : "Show Issues")
        .description(
          hide
            ? "Hides editor issues, as 'Hide' in the Issues view does: hidden issues "
              "are no longer listed by issues_list (unless includeHidden) and the "
              "Issues view shows them only with 'Show hidden issues'. The editor "
              "hides an issue type per object, so all issues of that type on the "
              "object are hidden. Not undoable and not saved in the map, as in the "
              "editor; MCP issues cannot be hidden (turn their check off with "
              "validators_set). Returns the hidden, unchanged and skipped issues. "
              "Examples: {\"issues\": [\"issue:41:16:0\"]}; {\"codes\": "
              "[\"EMPTY_PROPERTY_VALUE\"], \"ids\": [\"entity:7\"]}"
            : "Shows hidden editor issues again, as 'Show' in the Issues view does, so "
              "that issues_list lists them without includeHidden. Not undoable. "
              "Returns the shown, unchanged and skipped issues. Examples: "
              "{\"codes\": [\"EMPTY_PROPERTY_VALUE\"]}; {\"issues\": "
              "[\"issue:41:512:0\"]}")
        .input(object(std::move(fields)))
        .output(object({
          field(hide ? "hidden" : "shown", array(string())).required(),
          field("unchanged", array(string())).required(),
          field(
            "skipped",
            array(object({
              field("id", string()).required(),
              field("reason", string()).required(),
            })))
            .required(),
        }))
        .mutation(Mutation::External)
        .documentUse(DocumentUse::Required)
        .idempotent()
        .handler([hide](CallContext& context, const Args& args) {
          return setIssuesHidden(context, args, hide);
        }));
  }

  const auto validatorSchema = object({
    field("name", string()).required().describe("For validators_set"),
    field("source", enumOf({"editor", "mcp"})).required(),
    field("title", string()).required(),
    field("codes", array(string())).required().describe("The issue codes it reports"),
    field("enabled", boolean()).required(),
    field("fixes", array(string())).required(),
  });

  registry.add(
    ToolDef{"validators_list"}
      .title("List Validators")
      .description(
        "Lists the validators of the document: the editor's validators (one per issue "
        "type) and the MCP checks (Z_FIGHTING, ENTITY_OUTSIDE_HULL, MODEL_PLACEMENT for "
        "the four MODEL_* codes, UV_ASPECT_DISTORTION), with the codes they report, "
        "whether they are on and their fixes, plus the names of the turned-off ones "
        "(disabled). Read-only; turn validators on or off with validators_set. "
        "Example: {}")
      .input(object({}))
      .output(object({
        field("validators", array(validatorSchema)).required(),
        field("disabled", array(string())).required(),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(validatorsList));

  registry.add(
    ToolDef{"validators_set"}
      .title("Set Validators")
      .description(
        "Turns validators on or off for the document (names or titles from "
        "validators_list, case-insensitive). The issues of a turned-off validator are "
        "neither listed by issues_list and the issues resource nor reported in "
        "issuesIntroduced, and turned-off MCP checks do not run after each call (e.g. "
        "turn off ENTITY_OUTSIDE_HULL while blocking out an unsealed map). The setting "
        "lasts while the document is open and does not change the editor's Issues "
        "view (not undoable). Returns all validators, the turned-off ones (disabled) "
        "and the ones this call changed. Examples: {\"disable\": [\"Z_FIGHTING\", "
        "\"EMPTY_PROPERTY_VALUE\"]}; {\"enableAll\": true}")
      .input(object({
        field("enable", array(string()).nonEmpty())
          .describe("Validators to turn on (names or titles from validators_list)"),
        field("disable", array(string()).nonEmpty())
          .describe("Validators to turn off (names or titles from validators_list)"),
        field("enableAll", boolean().defaultsTo(false))
          .describe("Turn all validators on before applying enable and disable"),
      }))
      .output(object({
        field("validators", array(validatorSchema)).required(),
        field("disabled", array(string())).required(),
        field("changed", array(string())).required(),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(validatorsSet));
}

} // namespace tb::mcp
