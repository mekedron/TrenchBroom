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

#include "mcp/tools/SceneTools.h"

#include "NodeJson.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/ToolRegistry.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Grid.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeWriter.h"
#include "mdl/Object.h"
#include "mdl/PatchNode.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "mdl/WorldNode.h"

#include "kd/string_compare.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

// helpers

template <typename F>
void forEachNode(mdl::Node& node, const F& f)
{
  f(node);
  for (auto* child : node.children())
  {
    forEachNode(*child, f);
  }
}

template <typename F>
void forEachDescendant(mdl::Node& node, const F& f)
{
  for (auto* child : node.children())
  {
    forEachNode(*child, f);
  }
}

using Counts = std::map<std::string, size_t>;

/** Sorts by count (descending), then by name. */
std::vector<std::pair<std::string, size_t>> sortedByCount(const Counts& counts)
{
  auto result = std::vector<std::pair<std::string, size_t>>{counts.begin(), counts.end()};
  std::ranges::stable_sort(
    result, [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
  return result;
}

/** `{"name": count, ...}` sorted by count, at most `limit` entries. */
Json countsObject(const Counts& counts, const size_t limit)
{
  auto result = Json::object();
  for (const auto& [name, count] : sortedByCount(counts))
  {
    if (result.size() >= limit)
    {
      break;
    }
    result[name] = count;
  }
  return result;
}

/** `[{"name", "faces"}, ...]` sorted by usage, at most `limit` entries. */
Json materialUsageArray(const Counts& counts, const size_t limit)
{
  auto result = Json::array();
  for (const auto& [name, count] : sortedByCount(counts))
  {
    if (result.size() >= limit)
    {
      break;
    }
    result.push_back(Json{{"name", name}, {"faces", count}});
  }
  return result;
}

Json boundsJson(const std::optional<vm::bbox3d>& bounds)
{
  return bounds ? toJson(*bounds) : Json(nullptr);
}

void mergeBounds(std::optional<vm::bbox3d>& bounds, const vm::bbox3d& other)
{
  bounds = bounds ? vm::merge(*bounds, other) : other;
}

/** The objects in the given container's subtree (not counting the container). */
std::optional<vm::bbox3d> contentBounds(const mdl::Node& node)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto* child : node.children())
  {
    mergeBounds(result, child->logicalBounds());
  }
  return result;
}

bool isBrushEntity(const mdl::EntityNode& entityNode)
{
  return entityNode.hasChildren();
}

std::string classnameOf(const mdl::EntityNodeBase& entityNode)
{
  return entityNode.entity().classname();
}

/** The number of groups per link id. */
std::unordered_map<std::string, size_t> groupLinkCounts(mdl::Map& map)
{
  auto result = std::unordered_map<std::string, size_t>{};
  forEachNode(map.worldNode(), [&](mdl::Node& node) {
    if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
    {
      ++result[groupNode->linkId()];
    }
  });
  return result;
}

const std::vector<std::string>& objectKindNames()
{
  static const auto names =
    std::vector<std::string>{"world", "layer", "group", "entity", "brush", "patch"};
  return names;
}

std::optional<std::vector<ObjectKind>> kindsArgument(
  const Args& args, const std::string_view key)
{
  const auto names = args.getOptional<std::vector<std::string>>(key);
  if (!names)
  {
    return std::nullopt;
  }

  auto result = std::vector<ObjectKind>{};
  for (const auto& name : *names)
  {
    if (const auto kind = objectKindFromString(name))
    {
      result.push_back(*kind);
    }
  }
  return result;
}

bool containsKind(const std::vector<ObjectKind>& kinds, const ObjectKind kind)
{
  return std::ranges::find(kinds, kind) != kinds.end();
}

Json descendantCounts(mdl::Node& node)
{
  auto counts = Counts{};
  forEachDescendant(node, [&](mdl::Node& descendant) {
    ++counts[std::string{toString(objectKindOf(descendant))}];
  });

  auto result = Json::object();
  for (const auto& name : objectKindNames())
  {
    if (const auto it = counts.find(name); it != counts.end())
    {
      result[name] = it->second;
    }
  }
  return result;
}

Json propertiesJson(const mdl::Entity& entity)
{
  auto result = Json::object();
  for (const auto& property : entity.properties())
  {
    result[property.key()] = property.value();
  }
  return result;
}

std::optional<double> parseNumber(const std::string& str)
{
  auto value = 0.0;
  const auto* end = str.data() + str.size();
  const auto [ptr, ec] = std::from_chars(str.data(), end, value);
  return ec == std::errc{} && ptr == end ? std::optional{value} : std::nullopt;
}

// map_summary and map_stats

struct SceneStats
{
  size_t layers = 0;
  size_t groups = 0;
  size_t linkedGroups = 0;
  size_t pointEntities = 0;
  size_t brushEntities = 0;
  size_t brushes = 0;
  size_t patches = 0;
  size_t faces = 0;
  Counts entitiesByClass;
  Counts brushesByEntityClass;
  Counts materialUsage;
  std::optional<vm::bbox3d> bounds;
};

SceneStats sceneStats(mdl::Map& map)
{
  auto stats = SceneStats{};
  const auto linkCounts = groupLinkCounts(map);

  auto& worldNode = map.worldNode();
  for (auto* layerNode : worldNode.allLayers())
  {
    ++stats.layers;
    if (const auto bounds = contentBounds(*layerNode))
    {
      mergeBounds(stats.bounds, *bounds);
    }
  }

  forEachNode(worldNode, [&](mdl::Node& node) {
    if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
    {
      ++stats.groups;
      if (linkCounts.at(groupNode->linkId()) > 1)
      {
        ++stats.linkedGroups;
      }
    }
    else if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node))
    {
      ++stats.entitiesByClass[classnameOf(*entityNode)];
      if (isBrushEntity(*entityNode))
      {
        ++stats.brushEntities;
      }
      else
      {
        ++stats.pointEntities;
      }
    }
    else if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      ++stats.brushes;
      const auto* owner = brushNode->entity();
      ++stats.brushesByEntityClass[owner ? classnameOf(*owner) : "worldspawn"];
      for (const auto& face : brushNode->brush().faces())
      {
        ++stats.faces;
        ++stats.materialUsage[face.materialName()];
      }
    }
    else if (const auto* patchNode = dynamic_cast<const mdl::PatchNode*>(&node))
    {
      ++stats.patches;
      ++stats.materialUsage[patchNode->patch().materialName()];
    }
  });

  return stats;
}

size_t countIssues(mdl::Map& map)
{
  auto& worldNode = map.worldNode();
  const auto validators = worldNode.registeredValidators();
  auto count = size_t{0};
  forEachNode(worldNode, [&](mdl::Node& node) {
    for (const auto* issue : node.issues(validators))
    {
      if (!issue->hidden())
      {
        ++count;
      }
    }
  });
  return count;
}

constexpr auto MaxSummaryClasses = size_t{30};
constexpr auto MaxSummaryMaterials = size_t{20};

ToolResult mapSummaryTool(CallContext& context, const Args&)
{
  return mapSummary(context.map(), context.ids());
}

ToolResult mapStats(CallContext& context, const Args& args)
{
  const auto limit = size_t(args.get<int64_t>("limit"));
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto stats = sceneStats(map);

  auto layers = Json::array();
  for (auto* layerNode : map.worldNode().allLayersUserSorted())
  {
    auto brushes = size_t{0}, patches = size_t{0}, entities = size_t{0},
         groups = size_t{0};
    forEachDescendant(*layerNode, [&](mdl::Node& node) {
      switch (objectKindOf(node))
      {
      case ObjectKind::Brush:
        ++brushes;
        break;
      case ObjectKind::Patch:
        ++patches;
        break;
      case ObjectKind::Entity:
        ++entities;
        break;
      case ObjectKind::Group:
        ++groups;
        break;
      case ObjectKind::World:
      case ObjectKind::Layer:
        break;
      }
    });
    layers.push_back(Json{
      {"id", ids.format(*layerNode)},
      {"name", layerNode->layer().name()},
      {"bounds", boundsJson(contentBounds(*layerNode))},
      {"brushes", brushes},
      {"patches", patches},
      {"entities", entities},
      {"groups", groups},
    });
  }

  return Json{
    {"brushes", stats.brushes},
    {"faces", stats.faces},
    {"patches", stats.patches},
    {"brushCountByEntity", countsObject(stats.brushesByEntityClass, SIZE_MAX)},
    {"entitiesByClass", countsObject(stats.entitiesByClass, SIZE_MAX)},
    {"materials", materialUsageArray(stats.materialUsage, limit)},
    {"distinctMaterials", stats.materialUsage.size()},
    {"materialsTruncated", stats.materialUsage.size() > limit},
    {"layers", std::move(layers)},
    {"groups", Json{{"count", stats.groups}, {"linked", stats.linkedGroups}}},
  };
}

// map_tree

struct TreeOptions
{
  size_t maxDepth = 2;
  std::optional<std::vector<ObjectKind>> kinds;
  bool visibleOnly = false;
  Detail detail = Detail::Summary;
};

void collectTree(
  const mdl::Map& map,
  mdl::Node& node,
  const IdRegistry& ids,
  const TreeOptions& options,
  const size_t depth,
  std::vector<Json>& items)
{
  if (options.visibleOnly && !map.editorContext().visible(node))
  {
    return;
  }

  const auto kind = objectKindOf(node);
  if (!options.kinds || containsKind(*options.kinds, kind))
  {
    auto item = Json{};
    if (options.detail == Detail::Full)
    {
      item = nodeSummary(node, ids);
      item["state"] = nodeState(map, node);
    }
    else
    {
      item = Json{
        {"id", ids.format(node)},
        {"kind", std::string{toString(kind)}},
        {"label", nodeLabel(node)},
      };
    }
    item["depth"] = depth;
    item["parent"] = node.parent() ? Json(ids.format(*node.parent())) : Json(nullptr);
    item["childCount"] = node.childCount();
    if (depth == options.maxDepth && node.hasChildren())
    {
      item["descendants"] = descendantCounts(node);
    }
    items.push_back(std::move(item));
  }

  if (depth < options.maxDepth)
  {
    for (auto* child : node.children())
    {
      collectTree(map, *child, ids, options, depth + 1, items);
    }
  }
}

ToolResult mapTree(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto* root = static_cast<mdl::Node*>(&map.worldNode());
  if (const auto rootId = args.getOptional<std::string>("root"))
  {
    auto resolved = ids.resolve(*rootId);
    if (resolved.is_error())
    {
      return errorOf(resolved);
    }
    root = resolved.value();
  }

  const auto options = TreeOptions{
    size_t(args.get<int64_t>("depth")),
    kindsArgument(args, "kinds"),
    args.getOr<bool>("visibleOnly", false),
    request.value().detail,
  };

  auto items = std::vector<Json>{};
  collectTree(map, *root, ids, options, 0, items);
  return makePage(items, request.value(), map.modificationCount());
}

// object_get

Json objectJson(
  mdl::Map& map,
  mdl::Node& node,
  const IdRegistry& ids,
  const Detail detail,
  const std::unordered_map<std::string, size_t>& linkCounts)
{
  auto result = nodeSummary(node, ids);
  result["parent"] = node.parent() ? Json(ids.format(*node.parent())) : Json(nullptr);
  result["state"] = nodeState(map, node);
  if (detail == Detail::Summary)
  {
    return result;
  }

  const auto kind = objectKindOf(node);
  if (kind != ObjectKind::World && kind != ObjectKind::Layer)
  {
    result["group"] = groupIdOf(node, ids);
  }
  result["childCount"] = node.childCount();
  result["tags"] = nodeTagNames(map, node);
  if (node.lineCount() > 0)
  {
    result["lineNumber"] = node.lineNumber();
    result["lineCount"] = node.lineCount();
  }
  if (const auto* object = dynamic_cast<const mdl::Object*>(&node))
  {
    result["linkId"] = object->linkId();
  }

  if (const auto* entityNode = dynamic_cast<const mdl::EntityNodeBase*>(&node))
  {
    const auto& entity = entityNode->entity();
    result["classname"] = entity.classname();
    result["properties"] = propertiesJson(entity);
    if (const auto* definition = entity.definition())
    {
      result["definition"] = Json{
        {"name", definition->name},
        {"type",
         mdl::getType(*definition) == mdl::EntityDefinitionType::Point ? "point"
                                                                       : "brush"},
      };
    }
    else
    {
      result["definition"] = nullptr;
    }

    if (kind == ObjectKind::Entity)
    {
      auto brushes = size_t{0}, patches = size_t{0};
      for (const auto* child : node.children())
      {
        if (dynamic_cast<const mdl::BrushNode*>(child))
        {
          ++brushes;
        }
        else if (dynamic_cast<const mdl::PatchNode*>(child))
        {
          ++patches;
        }
      }

      if (!node.hasChildren())
      {
        result["origin"] = toJson(entity.origin());
        if (const auto* angle = entity.property(mdl::EntityPropertyKeys::Angle))
        {
          if (const auto value = parseNumber(*angle))
          {
            result["angle"] = roundForOutput(*value);
          }
        }
      }
      else
      {
        result["brushes"] = brushes;
        result["patches"] = patches;
      }
    }
  }

  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    const auto* owner = dynamic_cast<const mdl::EntityNode*>(brushNode->entity());
    result["entity"] = owner ? Json(ids.format(*owner)) : Json(nullptr);
    auto faces = Json::array();
    for (size_t i = 0; i < brushNode->brush().faceCount(); ++i)
    {
      faces.push_back(faceJson(map, *brushNode, i, ids, Detail::Full));
    }
    result["faces"] = std::move(faces);
  }
  else if (const auto* patchNode = dynamic_cast<const mdl::PatchNode*>(&node))
  {
    const auto* owner = dynamic_cast<const mdl::EntityNode*>(patchNode->entity());
    result["entity"] = owner ? Json(ids.format(*owner)) : Json(nullptr);
    result["material"] = patchNode->patch().materialName();
    result["rows"] = patchNode->patch().pointRowCount();
    result["columns"] = patchNode->patch().pointColumnCount();
  }
  else if (const auto* groupNode = dynamic_cast<const mdl::GroupNode*>(&node))
  {
    result["name"] = groupNode->group().name();
    result["persistentId"] =
      groupNode->persistentId() ? Json(*groupNode->persistentId()) : Json(nullptr);
    const auto it = linkCounts.find(groupNode->linkId());
    result["linked"] = it != linkCounts.end() && it->second > 1;
    result["linkedCopies"] = it != linkCounts.end() ? it->second : size_t{1};
    result["open"] = groupNode->opened();
  }
  else if (const auto* layerNode = dynamic_cast<const mdl::LayerNode*>(&node))
  {
    const auto& layer = layerNode->layer();
    result["name"] = layer.name();
    result["default"] = layerNode->isDefaultLayer();
    result["persistentId"] =
      layerNode->persistentId() ? Json(*layerNode->persistentId()) : Json(nullptr);
    result["sortIndex"] = layer.sortIndex();
    result["omitFromExport"] = layer.omitFromExport();
    result["current"] = map.editorContext().currentLayer() == layerNode;
    result["bounds"] = boundsJson(contentBounds(*layerNode));
  }
  else if (kind == ObjectKind::World)
  {
    result["layers"] = map.worldNode().allLayers().size();
  }

  return result;
}

ToolResult objectGet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto idStrings = args.get<std::vector<std::string>>("ids");
  const auto fields = args.getOr<std::vector<std::string>>("fields", {});
  const auto detail =
    args.get<std::string>("detail") == "summary" ? Detail::Summary : Detail::Full;

  auto missing = std::vector<std::string>{};
  auto firstError = std::optional<ToolError>{};
  auto objects = Json::array();
  auto fullObjects = std::vector<Json>{};
  const auto linkCounts = groupLinkCounts(map);

  for (const auto& id : idStrings)
  {
    const auto ref = parseObjectRef(id);
    auto resolved = ids.resolve(id);
    if (resolved.is_error())
    {
      missing.push_back(id);
      if (!firstError)
      {
        firstError = errorOf(resolved);
      }
      continue;
    }

    auto* node = resolved.value();
    auto object = Json{};
    if (ref && ref->faceIndex)
    {
      const auto& brushNode = static_cast<const mdl::BrushNode&>(*node);
      object = faceJson(map, brushNode, *ref->faceIndex, ids, Detail::Full);
      object["kind"] = "face";
      object["brush"] = ids.format(brushNode);
    }
    else
    {
      object = objectJson(map, *node, ids, detail, linkCounts);
    }
    objects.push_back(selectFields(object, fields));
    fullObjects.push_back(std::move(object));
  }

  if (!missing.empty())
  {
    auto error = *firstError;
    if (missing.size() > 1)
    {
      error.message = std::to_string(missing.size()) + " of the requested objects were "
                      "not found. " + error.message;
    }
    error.objectIds = missing;
    return error;
  }

  if (const auto unknown = unknownFields(fullObjects, fields); !unknown.empty())
  {
    auto names = std::string{};
    for (const auto& path : unknown)
    {
      names += (names.empty() ? "'" : ", '") + path + "'";
    }
    context.warn(
      "UNKNOWN_FIELD",
      "None of the objects has the field" + std::string{unknown.size() > 1 ? "s " : " "}
        + names + "; " + (unknown.size() > 1 ? "they were" : "it was")
        + " ignored. Nested keys need dotted paths (e.g. 'faces.vertices'); detail "
          "'summary' omits most keys.");
  }

  return Json{{"objects", std::move(objects)}};
}

// objects_find

struct FindFilter
{
  std::optional<std::vector<ObjectKind>> kinds;
  std::optional<std::string> classname;
  std::optional<std::string> propertyKey;
  std::optional<std::string> propertyValue;
  std::optional<std::string> material;
  const mdl::LayerNode* layer = nullptr;
  const mdl::GroupNode* group = nullptr;
  std::optional<std::string> tag;
  std::optional<vm::bbox3d> region;
  bool regionInside = false;
  std::optional<bool> visible;
  std::optional<bool> selected;
};

bool matchesMaterial(const mdl::Node& node, const std::string& pattern)
{
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    return std::ranges::any_of(brushNode->brush().faces(), [&](const auto& face) {
      return kdl::ci::str_matches_glob(face.materialName(), pattern);
    });
  }
  if (const auto* patchNode = dynamic_cast<const mdl::PatchNode*>(&node))
  {
    return kdl::ci::str_matches_glob(patchNode->patch().materialName(), pattern);
  }
  return false;
}

bool containsName(const std::vector<std::string>& names, const std::string& name)
{
  return std::ranges::find(names, name) != names.end();
}

bool matchesTag(const mdl::Map& map, const mdl::Node& node, const std::string& tag)
{
  if (containsName(nodeTagNames(map, node), tag))
  {
    return true;
  }
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    return std::ranges::any_of(brushNode->brush().faces(), [&](const auto& face) {
      return containsName(faceTagNames(map, face), tag);
    });
  }
  return false;
}

bool matches(const mdl::Map& map, mdl::Node& node, const FindFilter& filter)
{
  const auto kind = objectKindOf(node);
  if (filter.kinds)
  {
    if (!containsKind(*filter.kinds, kind))
    {
      return false;
    }
  }
  else if (kind == ObjectKind::World || kind == ObjectKind::Layer)
  {
    return false;
  }

  if (filter.classname || filter.propertyKey)
  {
    const auto* entityNode = dynamic_cast<const mdl::EntityNodeBase*>(&node);
    if (!entityNode)
    {
      return false;
    }
    const auto& entity = entityNode->entity();
    if (
      filter.classname
      && !kdl::ci::str_matches_glob(entity.classname(), *filter.classname))
    {
      return false;
    }
    if (filter.propertyKey)
    {
      const auto* value = entity.property(*filter.propertyKey);
      if (
        !value
        || (filter.propertyValue && !kdl::ci::str_matches_glob(*value, *filter.propertyValue)))
      {
        return false;
      }
    }
  }

  if (filter.material && !matchesMaterial(node, *filter.material))
  {
    return false;
  }
  if (filter.layer && mdl::findContainingLayer(&node) != filter.layer)
  {
    return false;
  }
  if (filter.group && !node.isDescendantOf(*filter.group))
  {
    return false;
  }
  if (filter.tag && !matchesTag(map, node, *filter.tag))
  {
    return false;
  }
  if (filter.region)
  {
    const auto& bounds = node.logicalBounds();
    if (
      filter.regionInside ? !filter.region->contains(bounds)
                          : !filter.region->intersects(bounds))
    {
      return false;
    }
  }
  if (filter.visible && map.editorContext().visible(node) != *filter.visible)
  {
    return false;
  }
  if (filter.selected && node.selected() != *filter.selected)
  {
    return false;
  }
  return true;
}

template <typename NodeType>
Result<const NodeType*, ToolError> resolveOptional(
  const IdRegistry& ids, const Args& args, const std::string_view key)
{
  const auto id = args.getOptional<std::string>(key);
  if (!id)
  {
    return static_cast<const NodeType*>(nullptr);
  }
  auto resolved = ids.resolve(*id);
  if (resolved.is_error())
  {
    return errorOf(resolved);
  }
  return static_cast<const NodeType*>(dynamic_cast<NodeType*>(resolved.value()));
}

ToolResult objectsFind(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto filter = FindFilter{};
  filter.kinds = kindsArgument(args, "kinds");
  filter.classname = args.getOptional<std::string>("classname");
  if (const auto property = args.getOptional<Json>("property"))
  {
    filter.propertyKey = property->at("key").get<std::string>();
    if (const auto* value = findMember(*property, "value"); value && !value->is_null())
    {
      filter.propertyValue = value->get<std::string>();
    }
  }
  filter.material = args.getOptional<std::string>("material");
  filter.tag = args.getOptional<std::string>("tag");
  filter.region = args.getOptional<vm::bbox3d>("region");
  filter.regionInside = args.get<std::string>("regionMode") == "inside";
  filter.visible = args.getOptional<bool>("visible");
  filter.selected = args.getOptional<bool>("selected");

  auto layer = resolveOptional<mdl::LayerNode>(ids, args, "layer");
  if (layer.is_error())
  {
    return errorOf(layer);
  }
  filter.layer = layer.value();

  auto group = resolveOptional<mdl::GroupNode>(ids, args, "group");
  if (group.is_error())
  {
    return errorOf(group);
  }
  filter.group = group.value();

  if (filter.tag && !map.tagManager().isRegisteredSmartTag(*filter.tag))
  {
    auto names = std::string{};
    for (const auto& tag : map.tagManager().smartTags())
    {
      names += (names.empty() ? "" : ", ") + tag.name();
    }
    context.warn(
      "UNKNOWN_TAG",
      "The game defines no smart tag '" + *filter.tag
        + "'. Known tags: " + (names.empty() ? "none" : names) + ".");
  }

  const auto detail = request.value().detail;
  auto items = std::vector<Json>{};
  auto counts = Counts{};
  forEachNode(map.worldNode(), [&](mdl::Node& node) {
    if (!matches(map, node, filter))
    {
      return;
    }

    auto item = nodeSummary(node, ids);
    if (detail == Detail::Full)
    {
      item["state"] = nodeState(map, node);
      item["tags"] = nodeTagNames(map, node);
      item["group"] = groupIdOf(node, ids);
      if (const auto* entityNode = dynamic_cast<const mdl::EntityNodeBase*>(&node))
      {
        item["properties"] = propertiesJson(entityNode->entity());
      }
      if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
      {
        item["faceCount"] = brushNode->brush().faceCount();
      }
    }
    ++counts[std::string{toString(objectKindOf(node))}];
    items.push_back(std::move(item));
  });

  auto result = makePage(items, request.value(), map.modificationCount());
  auto countsJson = Json::object();
  for (const auto& name : objectKindNames())
  {
    if (const auto it = counts.find(name); it != counts.end())
    {
      countsJson[name] = it->second;
    }
  }
  result["counts"] = std::move(countsJson);
  return result;
}

// map_text_get

ToolResult mapTextGet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& ids = context.ids();

  auto stream = std::stringstream{};
  auto writer = mdl::NodeWriter{map.worldNode(), stream};

  if (const auto idStrings = args.getOptional<std::vector<std::string>>("ids"))
  {
    auto nodes = std::vector<mdl::Node*>{};
    auto wholeMap = false;
    for (const auto& id : *idStrings)
    {
      auto resolved = ids.resolve(id);
      if (resolved.is_error())
      {
        return errorOf(resolved);
      }
      auto* node = resolved.value();
      switch (objectKindOf(*node))
      {
      case ObjectKind::World:
        wholeMap = true;
        break;
      case ObjectKind::Layer:
        std::ranges::copy(node->children(), std::back_inserter(nodes));
        break;
      default:
        nodes.push_back(node);
        break;
      }
    }

    if (wholeMap)
    {
      writer.writeMap(map.taskManager());
    }
    else
    {
      // drop duplicates and nodes whose ancestor is also written
      std::ranges::sort(nodes);
      const auto [first, last] = std::ranges::unique(nodes);
      nodes.erase(first, last);
      std::erase_if(nodes, [&](const auto* node) {
        return std::ranges::any_of(
          nodes, [&](const auto* other) { return node->isDescendantOf(*other); });
      });
      writer.writeNodes(nodes, map.taskManager());
    }
  }
  else
  {
    writer.writeMap(map.taskManager());
  }

  const auto text = stream.str();
  auto lineStarts = std::vector<size_t>{};
  for (size_t i = 0; i < text.size(); ++i)
  {
    if (i == 0 || text[i - 1] == '\n')
    {
      lineStarts.push_back(i);
    }
  }

  const auto totalLines = lineStarts.size();
  const auto startLine = size_t(args.get<int64_t>("startLine"));
  const auto maxLines = size_t(args.get<int64_t>("maxLines"));
  if (startLine > totalLines && !(startLine == 1 && totalLines == 0))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "startLine " + std::to_string(startLine) + " is past the end of the text ("
        + std::to_string(totalLines) + " lines).",
      "Pass a startLine between 1 and " + std::to_string(totalLines) + ".");
  }

  const auto firstIndex = startLine - 1;
  const auto endIndex = std::min(totalLines, firstIndex + maxLines);
  const auto begin = totalLines > 0 ? lineStarts[firstIndex] : size_t{0};
  const auto end = endIndex < totalLines ? lineStarts[endIndex] : text.size();
  const auto truncated = endIndex < totalLines;

  return Json{
    {"text", text.substr(begin, end - begin)},
    {"startLine", startLine},
    {"lineCount", endIndex - firstIndex},
    {"totalLines", totalLines},
    {"truncated", truncated},
    {"nextStartLine", truncated ? Json(endIndex + 1) : Json(nullptr)},
  };
}

Schema kindsSchema(std::string description)
{
  return array(enumOf(objectKindNames())).minSize(1).describe(std::move(description));
}

} // namespace

Json mapSummary(mdl::Map& map, const IdRegistry& ids)
{
  const auto stats = sceneStats(map);
  const auto& worldNode = map.worldNode();
  const auto* currentLayer = map.editorContext().currentLayer();

  auto layers = Json::array();
  for (const auto* layerNode : worldNode.allLayersUserSorted())
  {
    layers.push_back(Json{
      {"id", ids.format(*layerNode)},
      {"name", layerNode->layer().name()},
      {"objects", layerNode->descendantCount()},
      {"hidden", layerNode->hidden()},
      {"locked", layerNode->locked()},
      {"current", layerNode == currentLayer},
    });
  }

  const auto otherClasses = stats.entitiesByClass.size() > MaxSummaryClasses
                              ? stats.entitiesByClass.size() - MaxSummaryClasses
                              : size_t{0};

  auto worldspawn = Json::object();
  for (const auto* key : {"message", "wad", "_tb_mod"})
  {
    if (const auto* value = worldNode.entity().property(key))
    {
      worldspawn[key] = *value;
    }
  }

  return Json{
    {"game", map.gameInfo().gameConfig.name},
    {"format", mdl::formatName(worldNode.mapFormat())},
    {"gridSize", roundForOutput(map.grid().actualSize())},
    {"counts",
     Json{
       {"layers", stats.layers},
       {"groups", stats.groups},
       {"linkedGroups", stats.linkedGroups},
       {"entities", stats.pointEntities + stats.brushEntities},
       {"pointEntities", stats.pointEntities},
       {"brushEntities", stats.brushEntities},
       {"brushes", stats.brushes},
       {"patches", stats.patches},
       {"faces", stats.faces},
     }},
    {"entitiesByClass", countsObject(stats.entitiesByClass, MaxSummaryClasses)},
    {"otherClasses", otherClasses},
    {"layers", std::move(layers)},
    {"materials",
     Json{
       {"distinct", stats.materialUsage.size()},
       {"top", materialUsageArray(stats.materialUsage, MaxSummaryMaterials)},
     }},
    {"bounds", boundsJson(stats.bounds)},
    {"issues", countIssues(map)},
    {"worldspawn", std::move(worldspawn)},
  };
}

void registerSceneTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"map_summary"}
      .title("Map Summary")
      .description(
        "Returns a high-level overview of the map (read-only): game, format, grid size, "
        "object counts, entities by class (the 30 most frequent, e.g. {\"light\": 12}; "
        "otherClasses counts the rest), layers {id, name, objects, hidden, locked, "
        "current}, the 20 most used materials by face count, the bounds of all objects "
        "in map units (null if the map is empty), the number of visible validation "
        "issues (issues_list) and the worldspawn message, wad and _tb_mod. Start here "
        "to orient yourself; map_stats has full counts, map_tree the hierarchy. "
        "Example: {}")
      .input(object({}))
      .output(object({
        field("game", string()).required(),
        field("format", string()).required(),
        field("gridSize", number()).required().describe("Grid size in map units"),
        field("counts", any())
          .required()
          .describe(
            "{layers, groups, linkedGroups, entities, pointEntities, brushEntities, "
            "brushes, patches, faces}"),
        field("entitiesByClass", any())
          .required()
          .describe("classname -> count, most frequent first"),
        field("otherClasses", integer())
          .required()
          .describe("Number of classes not listed in entitiesByClass"),
        field("layers", array(any()))
          .required()
          .describe("[{id, name, objects, hidden, locked, current}]"),
        field("materials", any()).required().describe("{distinct, top: [{name, faces}]}"),
        field("bounds", any()).required().describe("{min, max} in map units, or null"),
        field("issues", integer()).required().describe("Visible validation issues"),
        field("worldspawn", any()).describe("message, wad and _tb_mod if set"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(mapSummaryTool));

  registry.add(
    ToolDef{"map_tree"}
      .title("Map Tree")
      .description(
        "Lists the object hierarchy world -> layers -> groups -> entities -> "
        "brushes/patches as a flat depth-first list (read-only). Each item has id, "
        "kind, label (classname or name), depth (root = 0), parent and childCount; items "
        "at the depth limit that have children also report descendants by kind, e.g. "
        "{\"brush\": 2}. detail 'full' adds bounds, materials, layer, entity, "
        "classname/name and state. kinds filters the listed items; containers are "
        "traversed anyway. Use objects_find to search by criteria. Example: "
        "{\"root\": \"layer:default\", \"depth\": 1}")
      .input(object({
        field("root", objectId()).describe("The object to start from. Default: world"),
        field("depth", integer().min(1).max(16).defaultsTo(2))
          .describe("Levels below the root to list"),
        field("kinds", kindsSchema("List only these kinds of objects")),
        field("visibleOnly", boolean().defaultsTo(false))
          .describe("Skip hidden objects and their contents"),
      }))
      .output(object({
        field("items", array(any())).required().describe("Tree items, depth-first"),
        field("total", integer()).required().describe("Number of listed items"),
        field("nextCursor", any())
          .required()
          .describe("Cursor of the next page, or null"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(mapTree));

  registry.add(
    ToolDef{"object_get"}
      .title("Get Objects")
      .description(
        "Returns details of up to 50 objects or faces (read-only), in the order of ids. "
        "All: id, kind, label, parent, layer, bounds (map units), state {visible, "
        "hidden, locked, selected, selectable}; detail 'full' (default) adds group, "
        "childCount, tags, linkId, lineNumber/lineCount (position in the file when "
        "loaded or saved), and per kind: entities (and world) classname, properties (in "
        "file order), definition {name, type} or null, origin and angle (point "
        "entities) or brushes/patches counts (brush entities); brushes entity (owner or "
        "null), materials, faces with alignment (offset, scale, rotation), normal, "
        "center, area, tags and vertices; patches material, rows, columns; groups name, "
        "persistentId, linked, linkedCopies, open; layers name, default, persistentId, "
        "sortIndex, omitFromExport, current. A face id ('brush:12/face:3') returns that "
        "face with kind 'face' and its brush. Fails with OBJECT_NOT_FOUND listing the "
        "unknown ids. Use fields to trim the output; fields that no object has are "
        "ignored with an UNKNOWN_FIELD warning. Example: {\"ids\": [\"entity:40\", "
        "\"brush:12/face:3\"], \"fields\": [\"id\", \"classname\", \"properties\", "
        "\"material\"]}")
      .input(object({
        field("ids", array(objectId()).minSize(1).maxSize(50))
          .required()
          .describe("Object ids or face ids ('brush:12/face:3'), at most 50"),
        field("fields", array(string()))
          .describe("Top-level keys or dotted paths to return, e.g. \"faces.material\""),
        field("detail", enumOf({"summary", "full"}).defaultsTo("full"))
          .describe("'summary': the list item shape plus parent and state; 'full': "
                    "everything"),
      }))
      .output(object({
        field("objects", array(any())).required().describe("In the order of ids"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(objectGet));

  registry.add(
    ToolDef{"objects_find"}
      .title("Find Objects")
      .description(
        "Finds objects matching all given filters (read-only), in tree order, including "
        "objects in closed groups and brushes of brush entities. Without kinds, groups, "
        "entities, brushes and patches are searched (world and layers only when kinds "
        "lists them). Globs use * and ? and ignore case. classname and property only "
        "match entities; material matches brushes with any face using it and patches; "
        "layer and group match their contents; tag matches smart tags of objects or of "
        "any face of a brush; region matches objects whose bounds intersect (default) "
        "or lie inside the box. Items are {id, kind, label, bounds, layer, "
        "classname|name|materials, entity}; detail 'full' adds state, tags, group, "
        "properties (entities) and faceCount (brushes). counts gives the matches by "
        "kind. Examples: {\"material\": \"wall_*\", \"layer\": \"layer:7\"}; "
        "{\"region\": {\"min\": [0,0,0], \"max\": [512,512,256]}, \"regionMode\": "
        "\"inside\", \"kinds\": [\"entity\"]}")
      .input(object({
        field("kinds", kindsSchema("Kinds of objects to find")),
        field("classname", string().nonEmpty()).describe("Entity classname glob"),
        field(
          "property",
          object({
            field("key", string().nonEmpty()).required().describe("Property key"),
            field("value", string()).describe("Value glob; omit to match any value"),
          }))
          .describe("Entities having this property"),
        field("material", string().nonEmpty()).describe("Material name glob"),
        field("layer", objectId({ObjectKind::Layer})).describe("Objects in this layer"),
        field("group", objectId({ObjectKind::Group}))
          .describe("Objects inside this group (at any depth)"),
        field("tag", string().nonEmpty())
          .describe("Smart tag name, e.g. \"trigger\" or \"detail\""),
        field("region", box()).describe("Box in map units"),
        field("regionMode", enumOf({"intersects", "inside"}).defaultsTo("intersects"))
          .describe("Match objects whose bounds intersect the region or lie inside it"),
        field("visible", boolean())
          .describe("true: only visible objects, false: only hidden ones"),
        field("selected", boolean())
          .describe("true: only selected objects, false: only unselected ones"),
      }))
      .output(object({
        field("items", array(any())).required().describe("Matching objects"),
        field("total", integer()).required().describe("Number of matches"),
        field("nextCursor", any())
          .required()
          .describe("Cursor of the next page, or null"),
        field("counts", any()).required().describe("Matches by kind"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(objectsFind));

  registry.add(
    ToolDef{"map_text_get"}
      .title("Get Map Text")
      .description(
        "Returns the whole map, or the given objects, as map file text in the "
        "document's format (read-only; like copy to the clipboard). Layer ids stand for "
        "their contents, world for the whole map. The text comes in pages of lines with "
        "startLine, lineCount, totalLines and truncated: pass nextStartLine as "
        "startLine to continue. Line numbers of the whole map match the file on disk "
        "only right after saving. Example: {\"ids\": [\"entity:40\"], \"maxLines\": "
        "100}")
      .input(object({
        field("ids", array(objectId()).minSize(1).maxSize(1000))
          .describe("Objects to serialize. Default: the whole map"),
        field("startLine", integer().min(1).defaultsTo(1))
          .describe("First line to return (1-based)"),
        field("maxLines", integer().min(1).max(5000).defaultsTo(400))
          .describe("Maximum number of lines to return"),
      }))
      .output(object({
        field("text", string()).required(),
        field("startLine", integer())
          .required()
          .describe("First returned line (1-based)"),
        field("lineCount", integer()).required().describe("Lines returned"),
        field("totalLines", integer()).required().describe("Lines of the whole text"),
        field("truncated", boolean()).required().describe("Whether more lines follow"),
        field("nextStartLine", any()).required().describe("Next startLine or null"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(mapTextGet));

  registry.add(
    ToolDef{"map_stats"}
      .title("Map Statistics")
      .description(
        "Returns detailed map statistics (read-only): brush, face and patch counts, "
        "brushes per entity class (e.g. {\"worldspawn\": 22, \"func_door\": 1}), "
        "entities by class, materials by usage (face count, a patch counts once; the "
        "top 'limit', materialsTruncated if there are more), per layer bounds and "
        "counts, and groups (linked = groups sharing a link id with another group). "
        "material_usage finds the faces of one material. Example: {\"limit\": 10}")
      .input(object({
        field("limit", integer().min(1).max(1000).defaultsTo(50))
          .describe("Maximum number of materials to list"),
      }))
      .output(object({
        field("brushes", integer()).required(),
        field("faces", integer()).required(),
        field("patches", integer()).required(),
        field("brushCountByEntity", any()).required().describe("classname -> brushes"),
        field("entitiesByClass", any()).required().describe("classname -> count"),
        field("materials", array(any())).required().describe("[{name, faces}]"),
        field("distinctMaterials", integer()).required(),
        field("materialsTruncated", boolean()).required(),
        field("layers", array(any()))
          .required()
          .describe("[{id, name, bounds, brushes, patches, entities, groups}]"),
        field("groups", any()).required().describe("{count, linked}"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(mapStats));
}

} // namespace tb::mcp
