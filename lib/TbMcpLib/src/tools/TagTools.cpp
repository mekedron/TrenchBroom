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

#include "mcp/tools/TagTools.h"

#include "gl/Material.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ObjectIds.h"
#include "mcp/Schema.h"
#include "mcp/Targets.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GeometryUtils.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Brushes.h"
#include "mdl/NodeQueries.h"
#include "mdl/PatchNode.h"
#include "mdl/SurfaceAttributes.h"
#include "mdl/Tag.h"
#include "mdl/TagManager.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/string_compare.h"
#include "kd/string_compare_natural.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** The most options tags_list reports per tag. */
constexpr auto MaxListedOptions = size_t(50);

// Describing smart tags

/** What a smart tag matches, recovered from its matcher. */
struct MatcherInfo
{
  /**
   * "classname", "material", "surfaceparm", "contentflag", "surfaceflag" (the names of
   * the `match` keys in GameConfig.cfg), "invalidflags" for a flag matcher whose flags
   * the game does not define (its mask is 0, so it never matches), or "unknown".
   */
  std::string type = "unknown";
  /** Glob pattern of classname and material matchers. */
  std::string pattern;
  /** Material that a classname matcher sets when the tag is applied. */
  std::string material;
  /** Surface parameters of a surfaceparm matcher. */
  std::vector<std::string> parameters;
  /** Flag mask of content and surface flag matchers. */
  int flags = 0;
  /** The matcher as TrenchBroom prints it, for unknown matchers. */
  std::string raw;

  bool isObjectTag() const { return type == "classname"; }
  bool isFaceTag() const { return !isObjectTag() && type != "unknown"; }
  /** Whether tag_apply and tag_remove can work with the tag at all. */
  bool isUsable() const { return type != "unknown" && type != "invalidflags"; }
  const char* kind() const
  {
    return isObjectTag() ? "object" : isFaceTag() ? "face" : "unknown";
  }
};

/** The value of `key: ` in a printed struct body, up to `nextKey` or the end. */
std::string attributeValue(
  const std::string& body, const std::string& key, const std::string& nextKey = {})
{
  const auto prefix = key + ": ";
  const auto start = body.find(prefix);
  if (start == std::string::npos)
  {
    return {};
  }
  const auto valueStart = start + prefix.size();
  const auto end =
    nextKey.empty() ? std::string::npos : body.find(", " + nextKey + ": ", valueStart);
  return body.substr(
    valueStart, end == std::string::npos ? std::string::npos : end - valueStart);
}

/** A probe face that carries the given material name and surface attributes. */
std::optional<mdl::BrushFace> probeFace(
  const std::string& materialName, const mdl::SurfaceAttributes& surfaceAttributes = {})
{
  auto face = mdl::BrushFace::create(
    vm::vec3d{0, 0, 0},
    vm::vec3d{0, 1, 0},
    vm::vec3d{1, 0, 0},
    materialName,
    mdl::UvAttributes{},
    surfaceAttributes,
    mdl::MapFormat::Standard);
  return face.is_success() ? std::optional{std::move(face).value()} : std::nullopt;
}

bool matchesFace(const mdl::SmartTag& tag, const std::optional<mdl::BrushFace>& face)
{
  return face && tag.matches(*face);
}

/**
 * Recovers the matcher of a smart tag. mdl::SmartTag does not expose its matcher, so
 * this reads the matcher's printed form (e.g. `EntityClassNameMatcher{m_pattern:
 * trigger*, m_material: trigger}`, the format of mdl's appendToStream). Content and
 * surface flag matchers print the same; they are told apart by matching probe faces.
 */
MatcherInfo matcherInfo(const mdl::SmartTag& tag)
{
  auto str = std::ostringstream{};
  str << tag;
  const auto printed = str.str();

  auto result = MatcherInfo{};
  const auto matcherKey = std::string{"m_matcher: "};
  const auto matcherStart = printed.find(matcherKey);
  if (matcherStart == std::string::npos)
  {
    result.raw = printed;
    return result;
  }
  // strip the closing brace of the SmartTag
  const auto matcher = printed.substr(
    matcherStart + matcherKey.size(),
    printed.size() - matcherStart - matcherKey.size() - 1);
  result.raw = matcher;

  const auto braceOpen = matcher.find('{');
  if (braceOpen == std::string::npos || matcher.back() != '}')
  {
    return result;
  }
  const auto typeName = matcher.substr(0, braceOpen);
  const auto body = matcher.substr(braceOpen + 1, matcher.size() - braceOpen - 2);

  if (typeName == "EntityClassNameMatcher")
  {
    result.type = "classname";
    result.pattern = attributeValue(body, "m_pattern", "m_material");
    result.material = attributeValue(body, "m_material");
  }
  else if (typeName == "MaterialNameTagMatcher")
  {
    result.type = "material";
    result.pattern = attributeValue(body, "m_pattern");
  }
  else if (typeName == "SurfaceParmTagMatcher")
  {
    result.type = "surfaceparm";
    auto list = attributeValue(body, "m_parameters");
    if (list.size() >= 2 && list.front() == '[' && list.back() == ']')
    {
      list = list.substr(1, list.size() - 2);
      auto start = size_t(0);
      while (!list.empty() && start <= list.size())
      {
        const auto end = list.find(", ", start);
        result.parameters.push_back(
          list.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
        {
          break;
        }
        start = end + 2;
      }
    }
  }
  else if (typeName == "FlagsTagMatcher")
  {
    try
    {
      result.flags = std::stoi(attributeValue(body, "m_flags"));
    }
    catch (const std::exception&)
    {
      return result;
    }
    if (result.flags == 0)
    {
      result.type = "invalidflags";
    }
    else if (matchesFace(tag, probeFace("probe", {.contents = result.flags})))
    {
      result.type = "contentflag";
    }
    else if (matchesFace(tag, probeFace("probe", {.flags = result.flags})))
    {
      result.type = "surfaceflag";
    }
  }
  return result;
}

const mdl::FlagsConfig* flagsConfig(const mdl::Map& map, const MatcherInfo& info)
{
  const auto& config = map.gameInfo().gameConfig.faceAttribsConfig;
  return info.type == "contentflag"   ? &config.contentFlags
         : info.type == "surfaceflag" ? &config.surfaceFlags
                                      : nullptr;
}

/**
 * The names of the flags of a flags matcher (e.g. ["lava", "slime", "water"]), by flag
 * value. (FlagsConfig::flagNames takes the list index as the bit, which is wrong for
 * configs with unused bits such as Quake 2's content flags.)
 */
std::vector<std::string> flagNames(const mdl::Map& map, const MatcherInfo& info)
{
  auto result = std::vector<std::string>{};
  if (const auto* config = flagsConfig(map, info))
  {
    for (const auto& flag : config->flags)
    {
      if ((flag.value & info.flags) != 0)
      {
        result.push_back(flag.name);
      }
    }
  }
  return result;
}

std::string joined(
  const std::vector<std::string>& strings,
  const std::string& separator = ", ",
  const std::string& lastSeparator = ", ")
{
  auto result = std::string{};
  for (size_t i = 0; i < strings.size(); ++i)
  {
    if (i > 0)
    {
      result += i + 1 == strings.size() ? lastSeparator : separator;
    }
    result += strings[i];
  }
  return result;
}

std::string quoted(const std::vector<std::string>& strings)
{
  auto result = std::vector<std::string>{};
  for (const auto& str : strings)
  {
    result.push_back("'" + str + "'");
  }
  return joined(result);
}

/** A sentence describing what the tag matches and what applying it does. */
std::string matchDescription(const mdl::Map& map, const MatcherInfo& info)
{
  if (info.type == "classname")
  {
    return "Brushes and patches of entities whose classname matches '" + info.pattern
           + "'. Applying turns them into a brush entity of a matching class"
           + (info.material.empty() ? std::string{}
                                    : " and gives them the material '" + info.material
                                        + "'")
           + "; removing moves them back to the world.";
  }
  if (info.type == "material")
  {
    return "Faces whose material name matches '" + info.pattern + "'"
           + (info.pattern.find('/') == std::string::npos ? std::string{" (the last path component)"} : std::string{})
           + ". Applying sets a matching material.";
  }
  if (info.type == "surfaceparm")
  {
    return "Faces whose material has the surface parameter "
           + joined(info.parameters, ", ", " or ")
           + ". Applying sets a material with that parameter.";
  }
  if (info.type == "contentflag" || info.type == "surfaceflag")
  {
    const auto what = info.type == "contentflag" ? "content" : "surface";
    return std::string{"Faces with the "} + what + " flag "
           + joined(flagNames(map, info), ", ", " or ") + ". Applying sets the flag"
           + ", removing clears the flags.";
  }
  if (info.type == "invalidflags")
  {
    return "Faces with flags that the game configuration names but the game does not "
           "define, so the tag never matches and cannot be applied or removed.";
  }
  return "Unknown matcher " + info.raw + ".";
}

Json matchJson(const mdl::Map& map, const MatcherInfo& info)
{
  auto result = Json{{"type", info.type}};
  if (info.type == "classname" || info.type == "material")
  {
    result["pattern"] = info.pattern;
  }
  if (info.type == "classname" && !info.material.empty())
  {
    result["material"] = info.material;
  }
  if (info.type == "surfaceparm")
  {
    result["parameters"] = info.parameters;
  }
  if (
    info.type == "contentflag" || info.type == "surfaceflag"
    || info.type == "invalidflags")
  {
    result["flags"] = flagNames(map, info);
    result["mask"] = info.flags;
  }
  return result;
}

/** The brush entity classes a classname tag offers, sorted like the editor does. */
std::vector<std::string> classnameOptions(const mdl::Map& map, const MatcherInfo& info)
{
  auto result = std::vector<std::string>{};
  for (const auto& definition : map.entityDefinitionManager().definitions())
  {
    if (
      mdl::getType(definition) == mdl::EntityDefinitionType::Brush
      && kdl::ci::str_matches_glob(definition.name, info.pattern))
    {
      result.push_back(definition.name);
    }
  }
  std::ranges::sort(result, kdl::ci::string_less_natural{});
  return result;
}

/** The loaded materials a material or surfaceparm tag offers, sorted. */
std::vector<std::string> materialOptions(
  const mdl::Map& map, const mdl::SmartTag& tag, const MatcherInfo& info)
{
  auto result = std::vector<std::string>{};
  for (const auto* material : map.materialManager().materials())
  {
    if (!material)
    {
      continue;
    }
    const auto matches = info.type == "material"
                           ? matchesFace(tag, probeFace(material->name()))
                           : std::ranges::any_of(info.parameters, [&](const auto& p) {
                               return material->surfaceParms().contains(p);
                             });
    if (matches)
    {
      result.push_back(material->name());
    }
  }
  std::ranges::sort(result, kdl::ci::string_less_natural{});
  return result;
}

/** The choices tag_apply offers for the tag (the `option` argument). */
std::vector<std::string> tagOptions(
  const mdl::Map& map, const mdl::SmartTag& tag, const MatcherInfo& info)
{
  if (info.type == "classname")
  {
    return classnameOptions(map, info);
  }
  if (info.type == "material" || info.type == "surfaceparm")
  {
    return materialOptions(map, tag, info);
  }
  return flagNames(map, info);
}

std::vector<mdl::Node*> allGeometryNodes(mdl::Map& map)
{
  return mdl::collectDescendants(
    std::vector<mdl::Node*>{&map.worldNode()},
    kdl::overload(
      [](const mdl::BrushNode&) { return true; },
      [](const mdl::PatchNode&) { return true; }));
}

// Resolving tags and targets

const mdl::SmartTag* findTag(const mdl::Map& map, const std::string& name)
{
  const auto& tags = map.tagManager().smartTags();
  if (const auto it = std::ranges::find(tags, name, &mdl::SmartTag::name);
      it != tags.end())
  {
    return &*it;
  }
  const auto it = std::ranges::find_if(
    tags, [&](const auto& tag) { return kdl::ci::str_is_equal(tag.name(), name); });
  return it != tags.end() ? &*it : nullptr;
}

std::vector<std::string> tagNames(const mdl::Map& map)
{
  auto result = std::vector<std::string>{};
  for (const auto& tag : map.tagManager().smartTags())
  {
    result.push_back(tag.name());
  }
  return result;
}

Result<const mdl::SmartTag*, ToolError> tagArgument(
  CallContext& context, const Args& args)
{
  const auto& map = context.map();
  const auto name = args.get<std::string>("tag");
  if (const auto* tag = findTag(map, name))
  {
    return tag;
  }

  const auto names = tagNames(map);
  return makeError(
    ErrorCode::InvalidArgument,
    names.empty()
      ? "The game '" + map.gameInfo().gameConfig.name
          + "' defines no smart tags, so there is no tag '" + name + "'."
      : "Unknown smart tag '" + name + "'. The game defines: " + joined(names) + ".",
    "Use tags_list to see the tags and what they match.");
}

/**
 * The brushes and patches an object tag acts on: the given ids (brush entity ids stand
 * for their brushes and patches) or the selection.
 */
Result<std::vector<mdl::Node*>, ToolError> resolveObjectTargets(
  CallContext& context, const Args& args, const std::string& tagName)
{
  auto& ids = context.ids();
  const auto explicitIds = args.getOptional<std::vector<std::string>>("ids");
  if (!explicitIds)
  {
    return resolveTargets(context, args, "ids", {ObjectKind::Brush, ObjectKind::Patch});
  }

  auto expanded = std::vector<std::string>{};
  for (const auto& id : *explicitIds)
  {
    const auto ref = parseObjectRef(id);
    if (ref && ref->faceIndex)
    {
      return makeError(
        ErrorCode::WrongObjectKind,
        "'" + tagName + "' is an object tag; it applies to whole brushes, not to the "
          + "face " + id + ".",
        "Pass the brush id (e.g. '" + id.substr(0, id.find('/')) + "').",
        {id});
    }
    if (ref && ref->kind == ObjectKind::Entity)
    {
      const auto node = ids.resolve(id);
      if (node.is_error())
      {
        return errorOf(node);
      }
      if (!node.value()->hasChildren())
      {
        return makeError(
          ErrorCode::WrongObjectKind,
          "Object " + id + " is a point entity; tags apply to brushes and patches.",
          "Pass brush ids or the id of a brush entity.",
          {id});
      }
      for (const auto* child : node.value()->children())
      {
        expanded.push_back(ids.format(*child));
      }
      continue;
    }
    expanded.push_back(id);
  }

  return resolveTargets(
    context,
    Args{Json{{"ids", expanded}}},
    "ids",
    {ObjectKind::Brush, ObjectKind::Patch});
}

/** The faces a face tag acts on (see resolveFaceTargets); patches are rejected. */
Result<std::vector<mdl::BrushFaceHandle>, ToolError> resolveFaceTagTargets(
  CallContext& context, const Args& args, const std::string& tagName)
{
  if (const auto explicitIds = args.getOptional<std::vector<std::string>>("ids"))
  {
    for (const auto& id : *explicitIds)
    {
      if (const auto ref = parseObjectRef(id); ref && ref->kind == ObjectKind::Patch)
      {
        return makeError(
          ErrorCode::WrongObjectKind,
          "'" + tagName + "' is a face tag; patch " + id + " has no brush faces.",
          "Pass face ids ('brush:12/face:3') or brush, group or entity ids.",
          {id});
      }
    }
  }
  return resolveFaceTargets(context, args, "ids");
}

// The option callback

/**
 * Chooses among the options a tag matcher offers (brush entity classes, materials or
 * flags): the one named by the `option` argument (case-insensitive), or else the
 * first. Never blocks. `names`, if it has as many entries as the matcher offers, replaces
 * the matcher's option names (see flagNames).
 */
class OptionCallback : public mdl::TagMatcherCallback
{
public:
  std::optional<std::string> option;
  std::vector<std::string> names;
  std::vector<std::string> offered;
  std::optional<std::string> chosen;
  bool asked = false;
  bool rejected = false;

  OptionCallback(std::optional<std::string> option_, std::vector<std::string> names_)
    : option{std::move(option_)}
    , names{std::move(names_)}
  {
  }

  size_t selectOption(const std::vector<std::string>& matcherOptions) override
  {
    asked = true;
    offered = names.size() == matcherOptions.size() ? names : matcherOptions;
    const auto& options = offered;
    if (options.empty())
    {
      return 0;
    }
    if (!option)
    {
      chosen = options.front();
      return 0;
    }
    for (size_t i = 0; i < options.size(); ++i)
    {
      if (kdl::ci::str_is_equal(options[i], *option))
      {
        chosen = options[i];
        return i;
      }
    }
    rejected = true;
    return options.size();
  }
};

bool hasGlobCharacters(const std::string& pattern)
{
  return pattern.find_first_of("*?[]\\") != std::string::npos;
}

/**
 * Applies a material tag whose matching materials are not loaded: sets the material
 * named by `option` (if the tag matches it) or the tag's literal pattern, with an
 * UNKNOWN_MATERIAL warning. Fails if neither names a material.
 */
ToolResult applyUnloadedMaterial(
  CallContext& context,
  const mdl::SmartTag& tag,
  const MatcherInfo& info,
  const std::optional<std::string>& option,
  std::optional<std::string>& chosen)
{
  auto material = std::optional<std::string>{};
  if (option && info.type == "material" && matchesFace(tag, probeFace(*option)))
  {
    material = *option;
  }
  else if (option)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The material '" + *option + "' does not match the tag '" + tag.name() + "' ("
        + matchDescription(context.map(), info) + ")",
      "Use tags_list to see what the tag matches.");
  }
  else if (info.type == "material" && !hasGlobCharacters(info.pattern))
  {
    material = info.pattern;
  }

  if (!material)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "No loaded material matches the tag '" + tag.name() + "' ("
        + matchDescription(context.map(), info) + ")",
      info.type == "material"
        ? "Pass the material name in 'option', or load the game's materials "
          "(materials_collections_set)."
        : "Load materials that have the surface parameter (materials_collections_set).");
  }

  context.warn(
    "UNKNOWN_MATERIAL",
    "No loaded material matches the tag '" + tag.name() + "'; the faces use '" + *material
      + "' anyway and show as missing until the material is loaded.");
  if (!mdl::setBrushFaceAttributes(context.map(), {.materialName = *material}))
  {
    return context.operationFailed("The material could not be set.");
  }
  chosen = *material;
  return Json::object();
}

// Reporting which targets carry the tag

struct TagState
{
  std::vector<std::string> tagged;
  std::vector<std::string> untagged;
};

TagState objectTagState(
  CallContext& context, const mdl::SmartTag& tag, const std::vector<mdl::Node*>& nodes)
{
  auto result = TagState{};
  for (const auto* node : nodes)
  {
    (node->hasTag(tag) ? result.tagged : result.untagged)
      .push_back(context.ids().format(*node));
  }
  return result;
}

TagState faceTagState(
  CallContext& context,
  const mdl::SmartTag& tag,
  const std::vector<mdl::BrushFaceHandle>& faces)
{
  auto result = TagState{};
  for (const auto& handle : faces)
  {
    (handle.face().hasTag(tag) ? result.tagged : result.untagged)
      .push_back(context.ids().formatFace(*handle.node(), handle.faceIndex()));
  }
  return result;
}

/** The brush entities that own the given nodes, without duplicates. */
std::vector<std::string> owningEntities(
  CallContext& context, const std::vector<mdl::Node*>& nodes)
{
  auto result = std::vector<std::string>{};
  for (const auto* node : nodes)
  {
    if (const auto* entityNode = owningBrushEntity(*node))
    {
      const auto id = context.ids().format(*entityNode);
      if (std::ranges::find(result, id) == result.end())
      {
        result.push_back(id);
      }
    }
  }
  return result;
}

std::string unsupportedHint(const MatcherInfo& info, const bool apply)
{
  if (info.type == "invalidflags")
  {
    return "The game configuration defines this tag with flags the game does not "
           "have; set the flags directly with face_attributes_set.";
  }
  if (!apply && (info.type == "material" || info.type == "surfaceparm"))
  {
    return "The tag follows from the material: apply a different material with "
           "material_apply.";
  }
  return "Use tags_list to see which tags can be applied and removed.";
}

// tags_list

ToolResult tagsList(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto kindFilter = args.getOptional<std::string>("kind");
  const auto& tags = map.tagManager().smartTags();

  // count the tagged objects and faces in one pass
  auto objectCounts = std::vector<size_t>(tags.size(), 0);
  auto faceCounts = std::vector<size_t>(tags.size(), 0);
  auto faceBrushCounts = std::vector<size_t>(tags.size(), 0);
  for (const auto* node : allGeometryNodes(map))
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    for (size_t i = 0; i < tags.size(); ++i)
    {
      if (node->hasTag(tags[i]))
      {
        ++objectCounts[i];
      }
      if (brushNode && brushNode->anyFacesHaveAnyTagInMask(tags[i].type()))
      {
        ++faceBrushCounts[i];
        for (const auto& face : brushNode->brush().faces())
        {
          if (face.hasTag(tags[i]))
          {
            ++faceCounts[i];
          }
        }
      }
    }
  }

  auto items = Json::array();
  for (size_t i = 0; i < tags.size(); ++i)
  {
    const auto& tag = tags[i];
    const auto info = matcherInfo(tag);
    if (kindFilter && *kindFilter != info.kind())
    {
      continue;
    }

    auto attributes = std::vector<std::string>{};
    for (const auto& attribute : tag.attributes())
    {
      attributes.push_back(attribute.name);
    }

    const auto options = tagOptions(map, tag, info);
    auto listedOptions = options;
    if (listedOptions.size() > MaxListedOptions)
    {
      listedOptions.resize(MaxListedOptions);
    }

    auto item = Json{
      {"name", tag.name()},
      {"kind", info.kind()},
      {"match", matchJson(map, info)},
      {"description", matchDescription(map, info)},
      {"attributes", attributes},
      {"canApply", tag.canEnable() && info.isUsable()},
      {"canRemove", tag.canDisable() && info.isUsable()},
      {"options", listedOptions},
      {"optionCount", options.size()},
      {"count", info.isFaceTag() ? faceCounts[i] : objectCounts[i]},
    };
    if (info.isFaceTag())
    {
      item["brushes"] = faceBrushCounts[i];
    }
    items.push_back(std::move(item));
  }

  return Json{
    {"game", map.gameInfo().gameConfig.name},
    {"tags", std::move(items)},
  };
}

// tag_apply and tag_remove

ToolResult tagApply(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto tagResult = tagArgument(context, args);
  if (tagResult.is_error())
  {
    return errorOf(tagResult);
  }
  const auto& tag = *tagResult.value();
  const auto info = matcherInfo(tag);
  const auto option = args.getOptional<std::string>("option");

  if (!tag.canEnable() || !info.isUsable())
  {
    return makeError(
      ErrorCode::Unsupported,
      "The tag '" + tag.name() + "' cannot be applied.",
      unsupportedHint(info, true));
  }

  const auto options = tagOptions(map, tag, info);
  auto callback = OptionCallback{option, options};
  auto chosen = std::optional<std::string>{};
  auto state = TagState{};
  auto entities = std::vector<std::string>{};

  if (info.isObjectTag())
  {
    const auto nodes = resolveObjectTargets(context, args, tag.name());
    if (nodes.is_error())
    {
      return errorOf(nodes);
    }
    if (options.empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "No brush entity class matches the tag '" + tag.name() + "' (pattern '"
          + info.pattern + "').",
        "Load entity definitions that contain such a class, or create the entity with "
        "entity_create_brush.",
        formatIds(nodes.value(), context.ids()));
    }
    if (auto result = withTargets(
          context,
          nodes.value(),
          [&]() -> ToolResult {
            tag.enable(callback, map);
            return Json::object();
          });
        result.is_error())
    {
      return result;
    }
    state = objectTagState(context, tag, nodes.value());
    entities = owningEntities(context, nodes.value());
  }
  else
  {
    const auto faces = resolveFaceTagTargets(context, args, tag.name());
    if (faces.is_error())
    {
      return errorOf(faces);
    }
    const auto isMaterialTag = info.type == "material" || info.type == "surfaceparm";
    if (auto result = withFaces(
          context,
          faces.value(),
          [&]() -> ToolResult {
            if (isMaterialTag && options.empty())
            {
              return applyUnloadedMaterial(context, tag, info, option, chosen);
            }
            tag.enable(callback, map);
            return Json::object();
          });
        result.is_error())
    {
      return result;
    }
    state = faceTagState(context, tag, faces.value());
    for (const auto& handle : faces.value())
    {
      if (const auto* entityNode = owningBrushEntity(*handle.node()))
      {
        const auto id = context.ids().format(*entityNode);
        if (std::ranges::find(entities, id) == entities.end())
        {
          entities.push_back(id);
        }
      }
    }
  }

  if (callback.rejected)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "The tag '" + tag.name() + "' does not offer the option '" + *option
        + "'. Options: " + joined(callback.offered) + ".",
      "Pass one of the options in 'option', or omit it to use the first.");
  }
  if (callback.asked)
  {
    chosen = callback.chosen;
    if (!option && callback.offered.size() > 1)
    {
      auto alternatives = callback.offered;
      alternatives.erase(alternatives.begin());
      context.warn(
        "TAG_OPTION_CHOSEN",
        "The tag '" + tag.name() + "' offers several options; used '"
          + callback.offered.front() + "'. Alternatives: " + quoted(alternatives)
          + ". Pass 'option' to choose another.",
        state.tagged);
    }
  }
  else if (!chosen && options.size() == 1)
  {
    chosen = options.front();
  }

  if (!callback.asked && option && (!chosen || !kdl::ci::str_is_equal(*chosen, *option)))
  {
    context.warn(
      "OPTION_IGNORED",
      "The tag '" + tag.name() + "' offers no choice here; 'option' was ignored.");
  }

  if (state.tagged.empty())
  {
    return context.operationFailed(
      "The tag '" + tag.name() + "' could not be applied.",
      "Check the editor messages and tags_list.");
  }
  if (!state.untagged.empty())
  {
    context.warn(
      "TAG_NOT_APPLIED",
      "Some targets do not carry the tag '" + tag.name() + "' afterwards.",
      state.untagged);
  }

  return Json{
    {"tag", tag.name()},
    {"kind", info.kind()},
    {"option", chosen ? Json(*chosen) : Json(nullptr)},
    {"tagged", state.tagged},
    {"untagged", state.untagged},
    {"entities", entities},
  };
}

ToolResult tagRemove(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto tagResult = tagArgument(context, args);
  if (tagResult.is_error())
  {
    return errorOf(tagResult);
  }
  const auto& tag = *tagResult.value();
  const auto info = matcherInfo(tag);

  if (!tag.canDisable() || !info.isUsable())
  {
    return makeError(
      ErrorCode::Unsupported,
      "The tag '" + tag.name() + "' cannot be removed directly.",
      unsupportedHint(info, false));
  }
  auto callback = OptionCallback{std::nullopt, {}};
  auto before = TagState{};
  auto after = TagState{};

  if (info.isObjectTag())
  {
    const auto nodes = resolveObjectTargets(context, args, tag.name());
    if (nodes.is_error())
    {
      return errorOf(nodes);
    }
    auto carrying = std::vector<mdl::Node*>{};
    std::ranges::copy_if(nodes.value(), std::back_inserter(carrying), [&](auto* node) {
      return node->hasTag(tag);
    });
    before = objectTagState(context, tag, nodes.value());
    if (!carrying.empty())
    {
      if (auto result = withTargets(
            context,
            carrying,
            [&]() -> ToolResult {
              tag.disable(callback, map);
              return Json::object();
            });
          result.is_error())
      {
        return result;
      }
    }
    after = objectTagState(context, tag, carrying);
  }
  else
  {
    const auto faces = resolveFaceTagTargets(context, args, tag.name());
    if (faces.is_error())
    {
      return errorOf(faces);
    }
    auto carrying = std::vector<mdl::BrushFaceHandle>{};
    std::ranges::copy_if(
      faces.value(), std::back_inserter(carrying), [&](const auto& handle) {
        return handle.face().hasTag(tag);
      });
    before = faceTagState(context, tag, faces.value());
    if (!carrying.empty())
    {
      if (auto result = withFaces(
            context,
            carrying,
            [&]() -> ToolResult {
              tag.disable(callback, map);
              return Json::object();
            });
          result.is_error())
      {
        return result;
      }
    }
    after = faceTagState(context, tag, carrying);
  }

  if (before.tagged.empty())
  {
    context.warn(
      "TAG_NOT_PRESENT",
      "None of the targets carries the tag '" + tag.name() + "'; nothing was changed.",
      before.untagged);
  }
  if (!after.tagged.empty())
  {
    context.warn(
      "TAG_STILL_PRESENT",
      "Some targets still carry the tag '" + tag.name() + "'.",
      after.tagged);
  }

  return Json{
    {"tag", tag.name()},
    {"kind", info.kind()},
    {"removed", after.untagged},
    {"tagged", after.tagged},
    {"skipped", before.untagged},
  };
}

Field tagField()
{
  return field("tag", string().nonEmpty())
    .required()
    .describe("Smart tag name (see tags_list), e.g. 'Trigger', 'Clip'; case-insensitive");
}

Field tagTargetsField()
{
  return field(
           "ids",
           array(objectId(
                   {ObjectKind::Brush,
                    ObjectKind::Patch,
                    ObjectKind::Entity,
                    ObjectKind::Group}))
             .nonEmpty())
    .describe(
      "Object tags (kind 'object'): brush and patch ids, or brush entity ids (their "
      "brushes). Face tags (kind 'face'): face ids ('brush:12/face:3') and brush, group "
      "or entity ids (all faces of their brushes). Default: the selection (selected "
      "faces, else all faces of the selected brushes for face tags)");
}

} // namespace

void registerTagTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"tags_list"}
      .title("List Smart Tags")
      .description(
        "Lists the smart tags of the document's game (GameConfig.cfg): object tags "
        "(kind 'object', e.g. Trigger or Detail, matching brushes by the classname of "
        "their entity) and face tags (kind 'face', e.g. Clip, Skip, Hint, matching "
        "faces by material name, material surface parameter, or content / surface "
        "flags). For each tag: what it matches ('match' with type, pattern, material, "
        "parameters or flag names, and a 'description'), attributes (e.g. "
        "'transparent'), whether tag_apply / tag_remove can apply or remove it, the "
        "'options' tag_apply chooses from (brush entity classes, loaded materials or "
        "flags; at most 50, optionCount is the total), and how many objects (object "
        "tags) or faces and brushes (face tags) in the map carry it. Example: {\"kind\": "
        "\"face\"}")
      .input(object({
        field("kind", enumOf({"object", "face"}))
          .describe("Only object tags or only face tags. Default: all"),
      }))
      .output(object({
        field("game", string()).required(),
        field(
          "tags",
          array(object({
            field("name", string()).required(),
            field("kind", enumOf({"object", "face", "unknown"})).required(),
            field("match", object({}).allowAdditionalProperties())
              .required()
              .describe("{type: classname | material | surfaceparm | contentflag | "
                        "surfaceflag | invalidflags | unknown, pattern?, material?, "
                        "parameters?, flags?, "
                        "mask?}"),
            field("description", string()).required(),
            field("attributes", array(string())).required(),
            field("canApply", boolean()).required(),
            field("canRemove", boolean()).required(),
            field("options", array(string())).required(),
            field("optionCount", integer()).required(),
            field("count", integer())
              .required()
              .describe("Objects (object tags) or faces (face tags) carrying the tag"),
            field("brushes", integer())
              .describe("Face tags: brushes with at least one such face"),
          })))
          .required(),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(tagsList));

  registry.add(
    ToolDef{"tag_apply"}
      .title("Apply Smart Tag")
      .description(
        "Turns objects or faces into a smart tag, like the editor's 'Turn Selection "
        "into <tag>': an object tag such as Trigger turns the brushes into a brush "
        "entity of a matching class (and sets the tag's material); a face tag sets a "
        "matching material (Quake Clip, Skip, ...) or the content / surface flag (Quake "
        "2 Clip, Hint, ...). If the tag offers several options (e.g. trigger_once vs. "
        "trigger_multiple, lava / slime / water), 'option' chooses one; without it the "
        "first is used with a TAG_OPTION_CHOSEN warning naming the alternatives. A "
        "material tag whose materials are not loaded sets 'option' or the literal "
        "pattern with an UNKNOWN_MATERIAL warning. Unknown tags fail with "
        "INVALID_ARGUMENT listing the tags. The result lists the targets that carry "
        "the tag afterwards ('tagged'), those that do not ('untagged', with a "
        "TAG_NOT_APPLIED warning), and the brush entities that own the targets; new "
        "entities also appear in the change report. The selection is restored. "
        "Example: {\"tag\": \"Trigger\", \"ids\": [\"brush:1042\"], \"option\": "
        "\"trigger_once\"}")
      .input(object({
        tagField(),
        tagTargetsField(),
        field("option", string().nonEmpty())
          .describe(
            "The option to use when the tag offers several (a brush entity class, "
            "material or flag name; see tags_list 'options'); case-insensitive"),
      }))
      .output(object({
        field("tag", string()).required(),
        field("kind", enumOf({"object", "face"})).required(),
        field("option", any())
          .required()
          .describe("The class, material or flag used, or null"),
        field("tagged", array(string()))
          .required()
          .describe("Target object or face ids that carry the tag afterwards"),
        field("untagged", array(string()))
          .required()
          .describe("Target ids that do not carry the tag afterwards"),
        field("entities", array(objectId()))
          .required()
          .describe("Brush entities that own the targets afterwards"),
      }))
      .mutation(Mutation::Map)
      .handler(tagApply));

  registry.add(
    ToolDef{"tag_remove"}
      .title("Remove Smart Tag")
      .description(
        "Turns objects or faces into non-<tag>, like the editor's 'Turn Selection "
        "into non-<tag>': an object tag moves the brushes out of their brush entity "
        "back to the world (like Make Structural; empty entities are removed), a flag "
        "tag clears the tag's flags. Tags given by materials cannot be removed "
        "(UNSUPPORTED; apply another material with material_apply). Only targets that "
        "carry the tag are changed; if none does, the call succeeds with a "
        "TAG_NOT_PRESENT warning. Example: {\"tag\": \"Detail\", \"ids\": "
        "[\"brush:1042/face:0\", \"brush:1043\"]}")
      .input(object({
        tagField(),
        tagTargetsField(),
      }))
      .output(object({
        field("tag", string()).required(),
        field("kind", enumOf({"object", "face"})).required(),
        field("removed", array(string()))
          .required()
          .describe("Target ids that carried the tag and no longer do"),
        field("tagged", array(string()))
          .required()
          .describe("Target ids that still carry the tag"),
        field("skipped", array(string()))
          .required()
          .describe("Target ids that did not carry the tag"),
      }))
      .mutation(Mutation::Map)
      .handler(tagRemove));
}

} // namespace tb::mcp
