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

#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

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

/** The issues_list filters. */
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

  bool matchesCode(const std::string& code, const std::string& type) const
  {
    return codes.empty() || codes.contains(upper(code)) || codes.contains(upper(type));
  }

  bool wantsCode(const std::string_view code) const
  {
    return codes.empty() || codes.contains(std::string{code});
  }

  bool matchesObjects(const std::vector<std::string>& ids) const
  {
    return !objects || std::ranges::any_of(ids, [&](const auto& id) {
      return objects->contains(id) || objects->contains(brushOfFace(id));
    });
  }
};

void collectDescendants(mdl::Node& node, std::vector<mdl::Node*>& result)
{
  result.push_back(&node);
  for (auto* child : node.children())
  {
    collectDescendants(*child, result);
  }
}

Result<IssueFilter, ToolError> issueFilter(CallContext& context, const Args& args)
{
  auto filter = IssueFilter{};
  if (const auto sources = args.getOptional<std::vector<std::string>>("sources"))
  {
    filter.editor = std::ranges::find(*sources, "editor") != sources->end();
    filter.mcp = std::ranges::find(*sources, "mcp") != sources->end();
  }
  for (const auto& code : args.getOr<std::vector<std::string>>("codes", {}))
  {
    filter.codes.insert(upper(code));
  }
  filter.includeHidden = args.get<bool>("includeHidden");

  if (const auto ids = args.getOptional<std::vector<std::string>>("ids"))
  {
    auto nodes = std::vector<mdl::Node*>{};
    for (const auto& id : *ids)
    {
      auto node = context.ids().resolve(id);
      if (node.is_error())
      {
        return errorOf(node);
      }
      collectDescendants(*node.value(), nodes);
    }
    filter.objects.emplace();
    for (auto* node : nodes)
    {
      filter.objects->insert(context.ids().format(*node));
    }
    filter.objectNodes = std::move(nodes);
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

Json fixNames(
  const std::vector<const mdl::Validator*>& validators, const mdl::IssueType type)
{
  auto result = Json::array();
  for (const auto* validator : validators)
  {
    if (validator->type() == type)
    {
      for (const auto* fix : validator->quickFixes())
      {
        result.push_back(fix->description());
      }
    }
  }
  return result;
}

/** The issues of the editor's validators, in tree order. */
void editorIssues(
  CallContext& context,
  const IssueFilter& filter,
  std::vector<Json>& items,
  std::map<std::string, size_t>& counts)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto validators = map.worldNode().registeredValidators();

  const auto visit = [&](const auto& self, mdl::Node& node) -> void {
    const auto objectId = ids.format(node);
    auto perType = std::map<mdl::IssueType, size_t>{};
    for (const auto* issue : node.issues(validators))
    {
      const auto k = perType[issue->type()]++;
      if (issue->hidden() && !filter.includeHidden)
      {
        continue;
      }
      const auto type = validatorName(validators, issue->type());
      const auto code = issueCode(type);
      auto objectIds = std::vector<std::string>{objectId};
      auto face = std::optional<std::string>{};
      if (const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issue))
      {
        face = ids.formatFace(
          static_cast<const mdl::BrushNode&>(node), faceIssue->faceIndex());
        objectIds.push_back(*face);
      }
      if (!filter.matchesCode(code, type) || !filter.matchesObjects(objectIds))
      {
        continue;
      }

      auto item = Json{
        {"id", fmt::format("issue:{}:{}:{}", node.runtimeId(), issue->type(), k)},
        {"source", "editor"},
        {"code", code},
        {"type", type},
        {"description", issue->description()},
        {"objectId", objectId},
      };
      if (face)
      {
        item["face"] = *face;
      }
      const auto line = issue->lineNumber();
      item["lineNumber"] = line > 0 ? Json(line) : Json(nullptr);
      item["hidden"] = issue->hidden();
      item["fixes"] = fixNames(validators, issue->type());
      counts[code] += 1;
      items.push_back(std::move(item));
    }
    for (auto* child : node.children())
    {
      self(self, *child);
    }
  };
  visit(visit, map.worldNode());
}

/** The MCP placement checks on the whole map or the requested objects. */
Json mcpIssues(
  CallContext& context,
  const IssueFilter& filter,
  std::vector<Json>& items,
  std::map<std::string, size_t>& counts)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  auto issues = std::vector<McpIssue>{};
  auto summary = Json::object();

  const auto wantsAny = [&](const std::vector<std::string>& codes) {
    return std::ranges::any_of(
      codes, [&](const auto& code) { return filter.wantsCode(code); });
  };
  const auto placementCodes = std::vector<std::string>{
    "MODEL_BELOW_FLOOR", "MODEL_FLOATING", "MODEL_PENETRATES_BRUSHES", "MODEL_NO_FLOOR"};

  // the objects to check
  auto brushes = std::vector<const mdl::BrushNode*>{};
  auto uvBrushes = std::vector<mdl::BrushNode*>{};
  auto entities = std::vector<mdl::EntityNode*>{};
  if (filter.objects)
  {
    for (auto* node : filter.objectNodes)
    {
      if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
      {
        brushes.push_back(brushNode);
        uvBrushes.push_back(brushNode);
      }
    }
    entities = pointEntities(filter.objectNodes);
  }
  else
  {
    auto all = std::vector<mdl::Node*>{&map.worldNode()};
    entities = pointEntities(all);
    auto nodes = std::vector<mdl::Node*>{};
    collectDescendants(map.worldNode(), nodes);
    for (auto* node : nodes)
    {
      if (auto* brushNode = dynamic_cast<mdl::BrushNode*>(node))
      {
        uvBrushes.push_back(brushNode);
      }
    }
  }

  if (filter.wantsCode(ZFightingCode))
  {
    const auto pairs = filter.objects ? findZFighting(map, &brushes) : findZFighting(map);
    auto found = zFightingIssues(pairs, ids);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  if (filter.wantsCode(EntityOutsideHullCode))
  {
    const auto report = predictLeaks(map);
    summary["leakCheck"] = Json{
      {"analyzed", report.analyzed},
      {"skippedReason", report.analyzed ? Json(nullptr) : Json(report.skippedReason)},
    };
    auto found = leakIssues(report, ids);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  if (wantsAny(placementCodes) && !entities.empty())
  {
    auto loader = EntityModelLoader{map};
    auto found = modelPlacementIssues(map, ids, entities, loader);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  if (filter.wantsCode(UvDistortionCode) && !uvBrushes.empty())
  {
    auto faces = std::vector<mdl::BrushFaceHandle>{};
    for (auto* brushNode : uvBrushes)
    {
      for (size_t i = 0; i < brushNode->brush().faceCount(); ++i)
      {
        faces.emplace_back(brushNode, i);
      }
    }
    auto knowledge = MaterialKnowledge{map, context.host().knowledgeDirectory()};
    auto found = uvDistortionIssues(faces, map, knowledge, ids);
    issues.insert(issues.end(), found.begin(), found.end());
  }

  for (auto& issue : issues)
  {
    if (
      !filter.matchesCode(issue.code, issue.type)
      || !filter.matchesObjects(issue.objectIds))
    {
      continue;
    }
    counts[issue.code] += 1;
    items.push_back(Json{
      {"source", "mcp"},
      {"code", issue.code},
      {"type", issue.type},
      {"description", issue.description},
      {"objectId", issue.objectId},
      {"lineNumber", nullptr},
      {"hidden", false},
      {"fixes", Json::array()},
      {"details", std::move(issue.details)},
    });
  }
  return summary;
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
  if (filter.value().editor)
  {
    editorIssues(context, filter.value(), items, counts);
  }
  auto summary = Json::object();
  if (filter.value().mcp)
  {
    summary = mcpIssues(context, filter.value(), items, counts);
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  auto countsJson = Json::object();
  for (const auto& [code, count] : counts)
  {
    countsJson[code] = count;
  }
  result["counts"] = std::move(countsJson);
  if (summary.contains("leakCheck"))
  {
    result["leakCheck"] = summary["leakCheck"];
  }
  return result;
}

Schema issueSchema()
{
  return object(
           {
             field("id", string())
               .describe(
                 "Editor issues: issue:<runtimeId>:<issueType>:<k>, for issue_fix"),
             field("source", enumOf({"editor", "mcp"})).required(),
             field("code", string()).required(),
             field("type", string()).required(),
             field("description", string()).required(),
             field("objectId", string()).required(),
             field("face", string()).describe("The face of a face issue"),
             field("lineNumber", any()).describe("Line in the map file, or null"),
             field("hidden", boolean()),
             field("fixes", array(string()))
               .describe("Names of the editor's quick fixes for this issue type"),
             field("details", any()).describe("MCP checks: face ids, positions, bounds"),
           })
    .allowAdditionalProperties();
}

} // namespace

void registerValidationTools(ToolRegistry& registry)
{
  auto codes = mcpIssueCodes();

  registry.add(
    ToolDef{"issues_list"}
      .title("List Issues")
      .description(
        "Lists the problems of the map: the issues of the editor's validators (source "
        "\"editor\", as the Issues view shows them: missing classnames, empty brush "
        "entities, invalid properties, ...) with the object, the line number in the "
        "map file, whether the issue is hidden and the names of the editor's quick "
        "fixes, and the MCP placement checks on the whole map (source \"mcp\"): "
        "Z_FIGHTING (coplanar overlapping faces of different brushes facing the same "
        "way; details: both face ids, area, center, plane), ENTITY_OUTSIDE_HULL (point "
        "entities the void reaches, found by a flood fill: the map leaks; details: "
        "position, nearest gap and its brushes), MODEL_BELOW_FLOOR / MODEL_FLOATING / "
        "MODEL_PENETRATES_BRUSHES / MODEL_NO_FLOOR (models against brushes; details: "
        "model bounds, suggested move) and UV_ASPECT_DISTORTION (stretched textures; "
        "details: measures and a fix). Every modifying call already reports the "
        "problems it introduced in issuesIntroduced; use this tool for the whole map. "
        "Filters: sources, codes (codes or editor type names, case-insensitive), ids "
        "(objects and their contents; face issues match their brush), includeHidden. "
        "Editor codes are the validator names in UPPER_SNAKE case, e.g. "
        "EMPTY_BRUSH_ENTITY. Example: {\"codes\": [\"Z_FIGHTING\"]} -> {\"items\": "
        "[{\"source\": \"mcp\", \"code\": \"Z_FIGHTING\", \"type\": \"Z-fighting\", "
        "\"objectId\": \"brush:12/face:3\", \"description\": \"...\", \"details\": "
        "{\"faces\": [\"brush:12/face:3\", \"brush:14/face:1\"], \"area\": 1024, ...}}], "
        "\"total\": 1, \"nextCursor\": null, \"counts\": {\"Z_FIGHTING\": 1}}")
      .input(object({
        field("sources", array(enumOf({"editor", "mcp"})).nonEmpty())
          .describe("Default: both"),
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
          .describe("{analyzed, skippedReason} of the leak prediction, if it ran"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(issuesList));
}

} // namespace tb::mcp
