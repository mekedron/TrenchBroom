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

#include "mcp/tools/MaterialTools.h"

#include "ToolUtils.h"
#include "base/Logger.h"
#include "base/PreferenceManager.h"
#include "fs/File.h"
#include "gl/GlUtils.h"
#include "gl/Material.h"
#include "gl/MaterialCollection.h"
#include "gl/MaterialManager.h"
#include "gl/Texture.h"
#include "gl/TextureBuffer.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Image.h"
#include "mcp/ObjectIds.h"
#include "mcp/Pagination.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/AssetUtils.h"
#include "mcp/tools/UvTools.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityProperties.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/GameFileSystem.h"
#include "mdl/GameInfo.h"
#include "mdl/LayerNode.h"
#include "mdl/LoadTexture.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Map_World.h"
#include "mdl/NodeQueries.h"
#include "mdl/Palette.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/WadPropertyUtils.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"

#include "kd/result.h"
#include "kd/string_compare.h"
#include "kd/string_format.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

Json currentLocks()
{
  return Json{
    {"alignmentLock", pref(Preferences::AlignmentLock)},
    {"uvLock", pref(Preferences::UvLock)},
  };
}

Schema locksSchema()
{
  return object({
    field("alignmentLock", boolean())
      .required()
      .describe("Texture lock: transforms keep the texture alignment on brush faces"),
    field("uvLock", boolean())
      .required()
      .describe("UV lock: vertex editing keeps the UV coordinates"),
  });
}

ToolResult locksGet(CallContext&, const Args&)
{
  return currentLocks();
}

ToolResult locksSet(CallContext& context, const Args& args)
{
  const auto alignmentLock = args.getOptional<bool>("alignmentLock");
  const auto uvLock = args.getOptional<bool>("uvLock");
  if (!alignmentLock && !uvLock)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass 'alignmentLock', 'uvLock' or both.");
  }

  const auto previous = currentLocks();
  auto result = previous;
  if (alignmentLock)
  {
    result["alignmentLock"] = *alignmentLock;
  }
  if (uvLock)
  {
    result["uvLock"] = *uvLock;
  }

  if (!context.dryRun())
  {
    // MapDocument applies the preferences to the editor context of every open document
    if (alignmentLock)
    {
      setPref(Preferences::AlignmentLock, *alignmentLock);
    }
    if (uvLock)
    {
      setPref(Preferences::UvLock, *uvLock);
    }
  }

  result["previous"] = previous;
  return result;
}

// materials

/** Iterates over all brushes and patches below the given node. */
template <typename BrushF, typename PatchF>
void forEachBrushAndPatch(
  const mdl::Node& node, const BrushF& brushF, const PatchF& patchF)
{
  if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
  {
    brushF(*brushNode);
  }
  else if (const auto* patchNode = dynamic_cast<const mdl::PatchNode*>(&node))
  {
    patchF(*patchNode);
  }
  for (const auto* child : node.children())
  {
    forEachBrushAndPatch(*child, brushF, patchF);
  }
}

struct MaterialUsage
{
  /** The name as it appears in the map (the first spelling found). */
  std::string name;
  size_t count = 0;
};

/**
 * Counts brush faces and patches per material, keyed by the lower case name (material
 * names are case-insensitive, like gl::MaterialManager). The counts are computed from
 * the map because gl::Material::usageCount also counts faces held by the undo history.
 */
std::unordered_map<std::string, MaterialUsage> materialUsage(const mdl::Map& map)
{
  auto result = std::unordered_map<std::string, MaterialUsage>{};
  const auto add = [&](const std::string& name) {
    auto& usage = result[kdl::str_to_lower(name)];
    if (usage.count == 0)
    {
      usage.name = name;
    }
    ++usage.count;
  };
  forEachBrushAndPatch(
    map.worldNode(),
    [&](const mdl::BrushNode& brushNode) {
      for (const auto& face : brushNode.brush().faces())
      {
        add(face.materialName());
      }
    },
    [&](const mdl::PatchNode& patchNode) { add(patchNode.patch().materialName()); });
  return result;
}

size_t usageOf(
  const std::unordered_map<std::string, MaterialUsage>& usage, const std::string& name)
{
  const auto it = usage.find(kdl::str_to_lower(name));
  return it != usage.end() ? it->second.count : 0;
}

/** Width and height of the material's texture, or nulls if it is not loaded yet. */
std::pair<Json, Json> materialSize(const gl::Material& material)
{
  if (const auto* texture = material.texture())
  {
    return {texture->width(), texture->height()};
  }
  return {nullptr, nullptr};
}

/** The loaded materials sorted by name (case-insensitive), then by collection. */
std::vector<const gl::Material*> sortedMaterials(const mdl::Map& map)
{
  auto materials = map.materialManager().materials();
  std::ranges::sort(materials, [](const auto* lhs, const auto* rhs) {
    const auto lhsName = kdl::str_to_lower(lhs->name());
    const auto rhsName = kdl::str_to_lower(rhs->name());
    return lhsName != rhsName ? lhsName < rhsName
                              : lhs->collectionName() < rhs->collectionName();
  });
  return materials;
}

bool hasWildcards(const std::string_view pattern)
{
  return pattern.find_first_of("*?") != std::string_view::npos;
}

size_t wildcardCount(const std::string_view pattern)
{
  return size_t(
    std::ranges::count_if(pattern, [](const char c) { return c == '*' || c == '?'; }));
}

bool matchGlobAt(
  const std::string_view pattern,
  const size_t p,
  const std::string_view str,
  const size_t s,
  std::vector<std::string>& captures)
{
  if (p == pattern.size())
  {
    return s == str.size();
  }

  if (pattern[p] == '*')
  {
    // greedy: the longest match first, like the regular expression .*
    for (auto length = str.size() - s + 1; length-- > 0;)
    {
      captures.emplace_back(str.substr(s, length));
      if (matchGlobAt(pattern, p + 1, str, s + length, captures))
      {
        return true;
      }
      captures.pop_back();
    }
    return false;
  }

  if (s == str.size())
  {
    return false;
  }

  if (pattern[p] == '?')
  {
    captures.emplace_back(str.substr(s, 1));
    if (matchGlobAt(pattern, p + 1, str, s + 1, captures))
    {
      return true;
    }
    captures.pop_back();
    return false;
  }

  return std::tolower(static_cast<unsigned char>(pattern[p]))
           == std::tolower(static_cast<unsigned char>(str[s]))
         && matchGlobAt(pattern, p + 1, str, s + 1, captures);
}

/**
 * Matches a material name against a pattern with '*' (any text) and '?' (one
 * character), ignoring case. Returns the text matched by each wildcard, in order.
 */
std::optional<std::vector<std::string>> matchGlob(
  const std::string_view pattern, const std::string_view str)
{
  auto captures = std::vector<std::string>{};
  if (matchGlobAt(pattern, 0, str, 0, captures))
  {
    return captures;
  }
  return std::nullopt;
}

/** Replaces the wildcards of the pattern with the given captures, in order. */
std::string fillWildcards(
  const std::string_view pattern, const std::vector<std::string>& captures)
{
  auto result = std::string{};
  auto next = size_t{0};
  for (const auto c : pattern)
  {
    if ((c == '*' || c == '?') && next < captures.size())
    {
      result += captures[next++];
    }
    else
    {
      result += c;
    }
  }
  return result;
}

/**
 * A name filter: a case-insensitive substring, or a glob if it contains '*' or '?'.
 */
bool matchesNameFilter(const std::string_view name, const std::string_view filter)
{
  return hasWildcards(filter) ? kdl::ci::str_matches_glob(name, filter)
                              : kdl::ci::str_contains(name, filter);
}

void warnUnknownMaterial(CallContext& context, const std::string& name, std::string what)
{
  context.warn(
    "UNKNOWN_MATERIAL",
    fmt::format(
      "Material '{}' is not loaded; {} it anyway and it shows as missing. Use "
      "materials_list to find available materials.",
      name,
      what));
}

/** The name of the loaded material with the given name (case-insensitive) or nullopt. */
std::optional<std::string> loadedMaterialName(
  const mdl::Map& map, const std::string& name)
{
  if (const auto* material = map.materialManager().material(name))
  {
    return material->name();
  }
  return std::nullopt;
}

ToolError unknownMaterialError(const mdl::Map& map, const std::string& name)
{
  auto similar = std::vector<std::string>{};
  for (const auto* material : sortedMaterials(map))
  {
    if (
      similar.size() < 5
      && (kdl::ci::str_contains(material->name(), name) || kdl::ci::str_contains(name, material->name())))
    {
      similar.push_back(material->name());
    }
  }
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("Material '{}' is not loaded.", name),
    similar.empty()
      ? std::string{"Use materials_list to find available materials."}
      : fmt::format(
          "Similar materials: {}. Use materials_list to find available materials.",
          fmt::join(similar, ", ")));
}

// materials_list

ToolResult materialsList(CallContext& context, const Args& args)
{
  const auto& map = context.map();
  const auto request = pageRequest(args, map.modificationCount());
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto search = args.getOptional<std::string>("search");
  const auto collection = args.getOptional<std::string>("collection");
  const auto usedOnly = args.getOr("usedOnly", false);
  const auto includeMissing = args.getOr("includeMissing", false);
  const auto full = request.value().detail == Detail::Full;

  auto usage = materialUsage(map);

  auto items = std::vector<Json>{};
  for (const auto* material : sortedMaterials(map))
  {
    const auto count = usageOf(usage, material->name());
    if (
      (search && !matchesNameFilter(material->name(), *search))
      || (collection && !matchesNameFilter(material->collectionName(), *collection))
      || (usedOnly && count == 0))
    {
      continue;
    }

    const auto [width, height] = materialSize(*material);
    auto item = Json{
      {"name", material->name()},
      {"collection", material->collectionName()},
      {"width", width},
      {"height", height},
      {"usage", count},
    };
    if (!material->surfaceParms().empty())
    {
      item["surfaceParms"] = std::vector<std::string>{
        material->surfaceParms().begin(), material->surfaceParms().end()};
    }
    if (full)
    {
      item["path"] = material->relativePath().generic_string();
      item["loaded"] = material->texture() != nullptr;
    }
    items.push_back(std::move(item));
  }

  // materials used by the map that are not loaded
  auto missing = std::vector<const MaterialUsage*>{};
  for (const auto& [key, entry] : usage)
  {
    if (
      entry.name != mdl::BrushFace::NoMaterialName
      && !map.materialManager().material(entry.name))
    {
      missing.push_back(&entry);
    }
  }
  std::ranges::sort(missing, [](const auto* lhs, const auto* rhs) {
    return kdl::str_to_lower(lhs->name) < kdl::str_to_lower(rhs->name);
  });

  if (includeMissing && !collection)
  {
    for (const auto* entry : missing)
    {
      if (!search || matchesNameFilter(entry->name, *search))
      {
        items.push_back(Json{
          {"name", entry->name},
          {"collection", nullptr},
          {"width", nullptr},
          {"height", nullptr},
          {"usage", entry->count},
          {"missing", true},
        });
      }
    }
  }

  auto result = makePage(items, request.value(), map.modificationCount());
  result["missingCount"] = missing.size();
  result["currentMaterial"] = map.currentMaterialName();
  return result;
}

// material_apply

ToolResult materialApply(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto name = args.get<std::string>("material");
  if (const auto loaded = loadedMaterialName(map, name))
  {
    name = *loaded;
  }
  else
  {
    warnUnknownMaterial(context, name, "the faces use");
  }

  const auto faces = resolveFaceTargets(context, args);
  if (faces.is_error())
  {
    return errorOf(faces);
  }

  return withFaces(context, faces.value(), [&]() -> ToolResult {
    if (!mdl::setBrushFaceAttributes(map, {.materialName = name}))
    {
      return context.operationFailed(fmt::format("Could not apply material '{}'.", name));
    }
    warnUvFindings(context, faces.value());
    return Json{
      {"material", name},
      {"faces", faces.value().size()},
    };
  });
}

// material_set_current

ToolResult materialSetCurrent(CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto name = args.get<std::string>("material");
  const auto loaded = loadedMaterialName(map, name);
  if (loaded)
  {
    name = *loaded;
  }
  else
  {
    warnUnknownMaterial(context, name, "new brushes use");
  }

  const auto previous = map.currentMaterialName();
  if (!context.dryRun())
  {
    map.setCurrentMaterialName(name);
  }
  return Json{
    {"material", name},
    {"loaded", loaded.has_value()},
    {"previous", previous},
  };
}

// material_replace

struct ReplaceRule
{
  std::string from;
  std::string to;
};

Result<std::vector<ReplaceRule>, ToolError> replaceRules(const Args& args)
{
  auto rules = std::vector<ReplaceRule>{};
  const auto from = args.getOptional<std::string>("from");
  const auto to = args.getOptional<std::string>("to");
  const auto rulesJson = args.getOptional<Json>("rules");
  if (rulesJson && (from || to))
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass either 'from' and 'to', or 'rules', not both.",
      "E.g. {\"from\": \"wall_old*\", \"to\": \"wall_new*\"}.");
  }
  if (rulesJson)
  {
    for (const auto& rule : *rulesJson)
    {
      rules.push_back({rule["from"].get<std::string>(), rule["to"].get<std::string>()});
    }
  }
  else if (from && to)
  {
    rules.push_back({*from, *to});
  }
  else
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to replace.",
      "Pass 'from' and 'to', e.g. {\"from\": \"wall_old*\", \"to\": \"wall_new*\"}, or "
      "a list of 'rules'.");
  }

  for (const auto& rule : rules)
  {
    if (wildcardCount(rule.to) > wildcardCount(rule.from))
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "The target '{}' has more wildcards than the pattern '{}'.",
          rule.to,
          rule.from),
        "Each '*' or '?' in 'to' is filled with the text matched by the wildcard at the "
        "same position in 'from'.");
    }
  }
  return rules;
}

struct ReplaceScope
{
  std::string kind;
  Json layer = nullptr;
  std::vector<mdl::BrushFaceHandle> faces;
};

Result<mdl::LayerNode*, ToolError> resolveLayerArgument(
  CallContext& context, const std::string& layer)
{
  auto& map = context.map();
  auto& ids = context.ids();
  if (const auto ref = parseObjectRef(layer); ref && ref->kind == ObjectKind::Layer)
  {
    auto node = ids.resolve(*ref);
    if (node.is_error())
    {
      return errorOf(node);
    }
    return static_cast<mdl::LayerNode*>(node.value());
  }

  auto matches = std::vector<mdl::LayerNode*>{};
  for (auto* layerNode : map.worldNode().allLayersUserSorted())
  {
    if (kdl::ci::str_is_equal(layerNode->layer().name(), layer))
    {
      matches.push_back(layerNode);
    }
  }
  if (matches.size() == 1)
  {
    return matches.front();
  }

  auto layerIds = std::vector<std::string>{};
  auto names = std::vector<std::string>{};
  for (auto* layerNode :
       matches.empty() ? map.worldNode().allLayersUserSorted() : matches)
  {
    layerIds.push_back(ids.format(*layerNode));
    names.push_back(
      fmt::format("'{}' ({})", layerNode->layer().name(), ids.format(*layerNode)));
  }
  if (matches.empty())
  {
    return makeError(
      ErrorCode::ObjectNotFound,
      fmt::format("There is no layer named '{}'.", layer),
      fmt::format("Layers: {}.", fmt::join(names, ", ")));
  }
  return makeError(
    ErrorCode::InvalidArgument,
    fmt::format("Several layers are named '{}'.", layer),
    fmt::format("Pass the layer id: {}.", fmt::join(names, ", ")),
    layerIds);
}

Result<ReplaceScope, ToolError> resolveReplaceScope(
  CallContext& context, const Args& args)
{
  auto& map = context.map();
  auto& ids = context.ids();
  const auto scope = args.getOptional<std::string>("scope");
  const auto layer = args.getOptional<std::string>("layer");
  const auto explicitIds = args.getOptional<std::vector<std::string>>("ids");

  if (int(scope.has_value()) + int(layer.has_value()) + int(explicitIds.has_value()) > 1)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass only one of 'scope', 'layer' and 'ids'.",
      "E.g. {\"layer\": \"Castle\"} or {\"scope\": \"map\"}.");
  }

  const auto allFaces = [](const std::vector<mdl::Node*>& nodes) {
    return mdl::collectBrushFaces(nodes);
  };

  if (layer)
  {
    auto layerNode = resolveLayerArgument(context, *layer);
    if (layerNode.is_error())
    {
      return errorOf(layerNode);
    }
    return ReplaceScope{
      "layer",
      Json{
        {"id", ids.format(*layerNode.value())},
        {"name", layerNode.value()->layer().name()},
      },
      allFaces({layerNode.value()}),
    };
  }

  if (explicitIds)
  {
    auto faces = std::vector<mdl::BrushFaceHandle>{};
    for (const auto& id : *explicitIds)
    {
      const auto ref = parseObjectRef(id);
      if (!ref)
      {
        return makeError(ErrorCode::InvalidArgument, "'" + id + "' is not a valid id.");
      }
      auto node = ids.resolve(*ref);
      if (node.is_error())
      {
        return errorOf(node);
      }
      if (ref->faceIndex)
      {
        faces.emplace_back(static_cast<mdl::BrushNode*>(node.value()), *ref->faceIndex);
      }
      else
      {
        const auto nodeFaces = allFaces({node.value()});
        faces.insert(faces.end(), nodeFaces.begin(), nodeFaces.end());
      }
    }

    // remove duplicates, keeping the order
    auto unique = std::vector<mdl::BrushFaceHandle>{};
    auto seen = std::set<std::pair<const mdl::BrushNode*, size_t>>{};
    for (const auto& handle : faces)
    {
      if (seen.emplace(handle.node(), handle.faceIndex()).second)
      {
        unique.push_back(handle);
      }
    }
    return ReplaceScope{"ids", nullptr, std::move(unique)};
  }

  const auto& selection = map.selection();
  const auto& selectedFaces = selection.allBrushFaces();
  if (scope == "selection" || (!scope && !selectedFaces.empty()))
  {
    if (selectedFaces.empty())
    {
      return makeError(
        ErrorCode::NoSelection,
        "No brushes or faces are selected.",
        "Select brushes or faces first, or use {\"scope\": \"map\"}, 'layer' or 'ids'.");
    }
    return ReplaceScope{"selection", nullptr, selectedFaces};
  }

  return ReplaceScope{"map", nullptr, allFaces({&map.worldNode()})};
}

struct Replacement
{
  std::string from;
  std::string to;
  bool targetLoaded = false;
  std::vector<mdl::BrushFaceHandle> faces;
};

ToolResult materialReplace(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& editorContext = map.editorContext();
  const auto allowMissingTargets = args.getOr("allowMissingTargets", false);

  const auto rules = replaceRules(args);
  if (rules.is_error())
  {
    return errorOf(rules);
  }

  auto scope = resolveReplaceScope(context, args);
  if (scope.is_error())
  {
    return errorOf(scope);
  }

  // source material name (as in the map) -> replacement; std::nullopt: no rule matched
  auto replacements = std::map<std::string, std::optional<Replacement>>{};
  auto ruleMatched = std::vector<bool>(rules.value().size(), false);
  auto skippedFaces = size_t{0};

  for (const auto& handle : scope.value().faces)
  {
    const auto& name = handle.face().materialName();
    auto it = replacements.find(name);
    if (it == replacements.end())
    {
      auto replacement = std::optional<Replacement>{};
      for (size_t i = 0; i < rules.value().size() && !replacement; ++i)
      {
        const auto& rule = rules.value()[i];
        if (const auto captures = matchGlob(rule.from, name))
        {
          ruleMatched[i] = true;
          auto target = fillWildcards(rule.to, *captures);
          const auto loaded = loadedMaterialName(map, target);
          if (loaded)
          {
            target = *loaded;
          }
          if (!kdl::ci::str_is_equal(target, name))
          {
            replacement = Replacement{name, std::move(target), loaded.has_value(), {}};
          }
          else
          {
            // the material stays the same
            break;
          }
        }
      }
      it = replacements.emplace(name, std::move(replacement)).first;
    }

    if (auto& replacement = it->second)
    {
      if (editorContext.selectable(*handle.node(), handle.face()))
      {
        replacement->faces.push_back(handle);
      }
      else
      {
        ++skippedFaces;
      }
    }
  }

  auto replaced = Json::array();
  auto unmatched = Json::array();
  auto byTarget = std::map<std::string, std::vector<mdl::BrushFaceHandle>>{};
  auto totalFaces = size_t{0};
  for (const auto& [name, replacement] : replacements)
  {
    if (!replacement || replacement->faces.empty())
    {
      continue;
    }

    const auto entry = Json{
      {"from", replacement->from},
      {"to", replacement->to},
      {"faces", replacement->faces.size()},
    };
    if (!replacement->targetLoaded && !allowMissingTargets)
    {
      unmatched.push_back(entry);
      continue;
    }
    if (!replacement->targetLoaded)
    {
      warnUnknownMaterial(
        context,
        replacement->to,
        fmt::format(
          "{} faces of '{}' use", replacement->faces.size(), replacement->from));
    }

    replaced.push_back(entry);
    totalFaces += replacement->faces.size();
    auto& faces = byTarget[replacement->to];
    faces.insert(faces.end(), replacement->faces.begin(), replacement->faces.end());
  }

  auto noMatch = Json::array();
  for (size_t i = 0; i < rules.value().size(); ++i)
  {
    if (!ruleMatched[i])
    {
      noMatch.push_back(rules.value()[i].from);
    }
  }

  auto scopeJson = Json{{"kind", scope.value().kind}};
  if (!scope.value().layer.is_null())
  {
    scopeJson["layer"] = scope.value().layer;
  }
  scopeJson["faces"] = scope.value().faces.size();

  auto result = Json{
    {"scope", std::move(scopeJson)},
    {"replaced", std::move(replaced)},
    {"totalFaces", totalFaces},
    {"unmatched", std::move(unmatched)},
    {"noMatch", std::move(noMatch)},
    {"skippedFaces", skippedFaces},
  };

  if (byTarget.empty())
  {
    return result;
  }

  auto allFaces = std::vector<mdl::BrushFaceHandle>{};
  for (const auto& [target, faces] : byTarget)
  {
    allFaces.insert(allFaces.end(), faces.begin(), faces.end());
  }

  return withFaces(context, allFaces, [&]() -> ToolResult {
    for (const auto& [target, faces] : byTarget)
    {
      mdl::deselectAll(map);
      mdl::selectBrushFaces(map, faces);
      if (!mdl::setBrushFaceAttributes(map, {.materialName = target}))
      {
        return context.operationFailed(
          fmt::format("Could not replace materials with '{}'.", target));
      }
    }
    warnUvFindings(context, allFaces);
    return result;
  });
}

// material_preview

std::string averageColor(const RgbaImage& image)
{
  size_t sum[3] = {0, 0, 0};
  const auto count = image.width * image.height;
  for (size_t i = 0; i < count; ++i)
  {
    for (size_t c = 0; c < 3; ++c)
    {
      sum[c] += image.pixels[i * 4 + c];
    }
  }
  return fmt::format(
    "#{:02x}{:02x}{:02x}",
    (sum[0] + count / 2) / count,
    (sum[1] + count / 2) / count,
    (sum[2] + count / 2) / count);
}

ToolResult materialPreview(CallContext& context, const Args& args)
{
  const auto& map = context.map();
  const auto name = args.get<std::string>("material");
  const auto maxSize = size_t(std::clamp(args.getOr("maxSize", 128), 1, 512));

  const auto* material = map.materialManager().material(name);
  if (!material)
  {
    return unknownMaterialError(map, name);
  }

  const auto image = loadMaterialImage(map, *material);
  if (image.is_error())
  {
    return errorOf(image);
  }

  const auto preview = downscale(image.value().image, maxSize);
  const auto png = encodePng(preview);
  if (!png)
  {
    return makeError(ErrorCode::InternalError, "Could not encode the preview image.");
  }
  context.addImage(*png, "image/png");

  return Json{
    {"name", material->name()},
    {"collection", material->collectionName()},
    {"width", image.value().image.width},
    {"height", image.value().image.height},
    {"previewWidth", preview.width},
    {"previewHeight", preview.height},
    {"averageColor", averageColor(image.value().image)},
    {"source", image.value().fromMemory ? "memory" : "file"},
  };
}

} // namespace

Json materialsResource(const mdl::Map& map)
{
  auto collections = Json::array();
  for (const auto& collection : map.materialManager().collections())
  {
    collections.push_back(Json{
      {"path", collection.path().generic_string()},
      {"materialCount", collection.materials().size()},
    });
  }

  auto materials = Json::array();
  for (const auto* material : sortedMaterials(map))
  {
    const auto [width, height] = materialSize(*material);
    materials.push_back(Json{
      {"name", material->name()},
      {"collection", material->collectionName()},
      {"width", width},
      {"height", height},
    });
  }

  return Json{
    {"collections", std::move(collections)},
    {"count", materials.size()},
    {"materials", std::move(materials)},
  };
}

void registerMaterialTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"locks_get"}
      .title("Get Locks")
      .description(
        "Returns the texture lock ('alignmentLock': moving, rotating or scaling brushes "
        "keeps their texture alignment) and the UV lock ('uvLock': vertex editing keeps "
        "UV coordinates). Both are editor preferences shared by all documents. "
        "Example: {}")
      .input(object({}))
      .output(locksSchema())
      .mutation(Mutation::None)
      .idempotent()
      .handler(locksGet));

  registry.add(
    ToolDef{"locks_set"}
      .title("Set Locks")
      .description(
        "Turns the texture lock (alignmentLock) and/or the UV lock (uvLock) on or off, "
        "like the toolbar buttons. These are editor preferences for all documents and "
        "are "
        "not undoable. Transform tools also accept a per-call 'alignmentLock' override. "
        "Returns the new and the previous values. Example: {\"alignmentLock\": false}")
      .input(object({
        field("alignmentLock", boolean()).describe("Texture lock"),
        field("uvLock", boolean()).describe("UV lock"),
      }))
      .output(object({
        field("alignmentLock", boolean()).required(),
        field("uvLock", boolean()).required(),
        field("previous", locksSchema()).required(),
      }))
      .mutation(Mutation::External)
      .idempotent()
      .handler(locksSet));

  registry.add(
    ToolDef{"materials_list"}
      .title("List Materials")
      .description(
        "Lists the loaded materials (textures) sorted by name: name, collection (WAD "
        "file or folder), width and height in pixels (null while the image is not "
        "loaded yet), usage (brush faces and patches using it in the map) and "
        "surfaceParms (Quake 3 shaders, if any); detail \"full\" adds path and loaded. "
        "Filters: search (case-insensitive substring, or a glob with '*' and '?'), "
        "collection (same matching), usedOnly. includeMissing adds materials the map "
        "uses that are not loaded ({missing: true}); missingCount always counts them. "
        "Material names are case-insensitive. Example: {\"search\": \"wall_*\", "
        "\"usedOnly\": true} -> {\"items\": [{\"name\": \"wall_brick\", \"collection\": "
        "\"base.wad\", \"width\": 64, \"height\": 64, \"usage\": 36}], \"total\": 1, "
        "\"nextCursor\": null, \"missingCount\": 0, \"currentMaterial\": \"wall_brick\"}")
      .input(object({
        field("search", string().nonEmpty())
          .describe(
            "Case-insensitive substring of the name, or a glob such as \"wall_*\""),
        field("collection", string().nonEmpty())
          .describe("Collection (WAD file or folder) substring or glob"),
        field("usedOnly", boolean()).describe("Only materials used in the map"),
        field("includeMissing", boolean())
          .describe("Also list materials the map uses that are not loaded"),
      }))
      .output(object({
        field("items", array(any()))
          .required()
          .describe(
            "[{name, collection, width, height, usage, surfaceParms?, missing?}]"),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("missingCount", integer())
          .required()
          .describe("Materials used in the map that are not loaded"),
        field("currentMaterial", string())
          .required()
          .describe("The material that new brushes get"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialsList));

  registry.add(
    ToolDef{"material_apply"}
      .title("Apply Material")
      .description(
        "Applies a material to faces, like clicking a material in the material browser: "
        "'ids' are face ids ('brush:12/face:3') or brush, group and entity ids (all "
        "their faces); without ids, the selected faces or all faces of the selected "
        "objects. Only the material changes; the alignment (offset, scale, rotation) "
        "stays. A material that is not loaded is applied anyway with an "
        "UNKNOWN_MATERIAL warning. The uv_check findings on the faces (UV_* codes, e.g. "
        "a panel that does not fit) are added as warnings. Example: {\"material\": "
        "\"wall_brick\", \"ids\": "
        "[\"brush:12\", \"brush:14/face:2\"]} -> {\"material\": \"wall_brick\", "
        "\"faces\": 7}")
      .input(object({
        field("material", string().nonEmpty()).required().describe("Material name"),
        faceTargetsField(),
      }))
      .output(object({
        field("material", string()).required(),
        field("faces", integer()).required().describe("Number of faces"),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(materialApply));

  registry.add(
    ToolDef{"material_set_current"}
      .title("Set Current Material")
      .description(
        "Sets the current material, which new brushes get (like selecting a material "
        "in the material browser with nothing selected). Not undoable. Warns with "
        "UNKNOWN_MATERIAL if the material is not loaded. Returns the new and the "
        "previous material. Example: {\"material\": \"floor_tile\"} -> {\"material\": "
        "\"floor_tile\", \"loaded\": true, \"previous\": \"wall_brick\"}")
      .input(object({
        field("material", string().nonEmpty()).required().describe("Material name"),
      }))
      .output(object({
        field("material", string()).required(),
        field("loaded", boolean()).required().describe("Whether the material is loaded"),
        field("previous", string()).required(),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialSetCurrent));

  registry.add(
    ToolDef{"material_replace"}
      .title("Replace Materials")
      .description(
        "Replaces materials on brush faces by name, keeping the alignment. 'from' is a "
        "material name or a pattern (case-insensitive; '*' matches any text, '?' one "
        "character); each wildcard in 'to' is filled with the text matched by the "
        "wildcard at the same position in 'from', so {\"from\": \"wall_old*\", \"to\": "
        "\"wall_new*\"} turns wall_old_2 into wall_new_2. 'rules' ([{from, to}], the "
        "first matching rule wins) replaces several materials at once. Scope: 'layer' "
        "(id or name), 'ids' (faces, brushes, groups, entities, layers) or 'scope' "
        "(\"selection\" or \"map\"); by default the selection, or the whole map if "
        "nothing is selected, like the Replace Material dialog. Hidden and locked faces "
        "are skipped (skippedFaces). Targets that are not loaded are not applied but "
        "reported in 'unmatched' unless allowMissingTargets is true (then they are "
        "applied with an UNKNOWN_MATERIAL warning). 'noMatch' lists patterns that "
        "matched no face. The uv_check findings on the changed faces are added as "
        "warnings (UV_* codes). One undo step. Example: {\"from\": \"wall_old*\", "
        "\"to\": "
        "\"wall_new*\", \"layer\": \"Castle\"} -> {\"scope\": {\"kind\": \"layer\", "
        "\"layer\": {\"id\": \"layer:3\", \"name\": \"Castle\"}, \"faces\": 120}, "
        "\"replaced\": [{\"from\": \"wall_old_a\", \"to\": \"wall_new_a\", \"faces\": "
        "24}], \"totalFaces\": 24, \"unmatched\": [{\"from\": \"wall_old_c\", \"to\": "
        "\"wall_new_c\", \"faces\": 6}], \"noMatch\": [], \"skippedFaces\": 0}")
      .input(object({
        field("from", string().nonEmpty())
          .describe("Material name or pattern with '*' and '?'"),
        field("to", string().nonEmpty())
          .describe("Replacement name; its wildcards are filled from 'from'"),
        field(
          "rules",
          array(object({
                  field("from", string().nonEmpty()).required(),
                  field("to", string().nonEmpty()).required(),
                }))
            .nonEmpty())
          .describe("Several {from, to} rules instead of 'from' and 'to'"),
        field("scope", enumOf({"selection", "map"}))
          .describe("Default: the selection, or the map if nothing is selected"),
        field("layer", string().nonEmpty()).describe("Only this layer (id or name)"),
        field("ids", array(objectId()).nonEmpty())
          .describe("Only these faces, brushes, groups, entities or layers"),
        field("allowMissingTargets", boolean())
          .describe("Also apply target materials that are not loaded"),
      }))
      .output(object({
        field("scope", any())
          .required()
          .describe("{kind: selection|map|layer|ids, layer?: {id, name}, faces}"),
        field("replaced", array(any())).required().describe("[{from, to, faces}]"),
        field("totalFaces", integer()).required(),
        field("unmatched", array(any()))
          .required()
          .describe("[{from, to, faces}] not replaced: the target is not loaded"),
        field("noMatch", array(string()))
          .required()
          .describe("Patterns that matched no face in the scope"),
        field("skippedFaces", integer())
          .required()
          .describe("Matching faces that are hidden or locked"),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(materialReplace));

  registry.add(
    ToolDef{"material_preview"}
      .title("Preview Material")
      .description(
        "Returns a small PNG image of a material (as image content) and its name, "
        "collection, size in pixels, preview size, average color and where the image "
        "came from ('memory' or 'file'). The image is scaled down to at most maxSize "
        "pixels (default 128, at most 512) along its longer side, keeping the aspect "
        "ratio; smaller images keep their size. Example: {\"material\": \"wall_brick\", "
        "\"maxSize\": 64} -> {\"name\": \"wall_brick\", \"collection\": \"base.wad\", "
        "\"width\": 128, \"height\": 64, \"previewWidth\": 64, \"previewHeight\": 32, "
        "\"averageColor\": \"#7a5c43\", \"source\": \"file\"}")
      .input(object({
        field("material", string().nonEmpty()).required().describe("Material name"),
        field("maxSize", integer().min(1))
          .describe("Longest side of the preview in pixels (default 128, at most 512)"),
      }))
      .output(object({
        field("name", string()).required(),
        field("collection", string()).required(),
        field("width", integer()).required(),
        field("height", integer()).required(),
        field("previewWidth", integer()).required(),
        field("previewHeight", integer()).required(),
        field("averageColor", string()).required().describe("\"#rrggbb\""),
        field("source", enumOf({"memory", "file"})).required(),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialPreview));
}

} // namespace tb::mcp
