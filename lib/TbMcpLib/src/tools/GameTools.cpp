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

#include "mcp/tools/GameTools.h"

#include "ToolUtils.h"
#include "base/PreferenceManager.h"
#include "fs/DiskIO.h"
#include "fs/PathInfo.h"
#include "fs/PathMatcher.h"
#include "fs/TraversalMode.h"
#include "gl/MaterialCollection.h"
#include "gl/MaterialManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/JsonVm.h"
#include "mcp/Pagination.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinitionFileSpec.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/Map_Assets.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_World.h"
#include "mdl/NodeContents.h"
#include "mdl/SoftMapBounds.h"
#include "mdl/Tag.h"
#include "mdl/WadPropertyUtils.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include "kd/string_compare.h"
#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <sstream>

namespace tb::mcp
{
namespace
{
using namespace schema;

Json pathOrNull(const std::filesystem::path& path)
{
  return path.empty() ? Json(nullptr) : Json(path.string());
}

Json pathsJson(const std::vector<std::filesystem::path>& paths)
{
  auto result = Json::array();
  for (const auto& path : paths)
  {
    result.push_back(path.string());
  }
  return result;
}

Json flagsJson(const mdl::FlagsConfig& flags)
{
  auto result = Json::array();
  for (const auto& flag : flags.flags)
  {
    result.push_back(Json{
      {"name", flag.name},
      {"value", flag.value},
      {"description", flag.description},
    });
  }
  return result;
}

Json tagsJson(const std::vector<mdl::SmartTag>& tags)
{
  auto result = Json::array();
  for (const auto& tag : tags)
  {
    auto attributes = Json::array();
    for (const auto& attribute : tag.attributes())
    {
      attributes.push_back(attribute.name);
    }

    auto str = std::stringstream{};
    tag.appendToStream(str);
    result.push_back(Json{
      {"name", tag.name()},
      {"attributes", std::move(attributes)},
      {"definition", str.str()},
    });
  }
  return result;
}

bool usesWads(const mdl::GameInfo& gameInfo)
{
  return gameInfo.gameConfig.materialConfig.property.has_value();
}

std::vector<std::string> findAvailableMods(const mdl::GameInfo& gameInfo)
{
  if (!isGamePathValid(gameInfo))
  {
    return {};
  }

  const auto defaultMod =
    gameInfo.gameConfig.fileSystemConfig.searchPath.filename().string();
  return fs::Disk::find(
           gamePath(gameInfo),
           fs::TraversalMode::Flat,
           fs::makePathInfoPathMatcher({fs::PathInfo::Directory}))
         | kdl::transform([&](const auto& subDirs) {
             auto mods = std::vector<std::string>{};
             for (const auto& subDir : subDirs)
             {
               auto mod = subDir.filename().string();
               if (!kdl::ci::str_is_equal(mod, defaultMod))
               {
                 mods.push_back(std::move(mod));
               }
             }
             std::ranges::sort(mods, kdl::ci::string_less{});
             return mods;
           })
         | kdl::value_or(std::vector<std::string>{});
}

std::vector<std::string> wadPaths(const mdl::Map& map)
{
  if (const auto& property = map.gameInfo().gameConfig.materialConfig.property)
  {
    if (const auto* value = map.worldNode().entity().property(*property))
    {
      return mdl::splitWadProperty(*value);
    }
  }
  return {};
}

std::optional<std::filesystem::path> resolveExternalPath(
  const mdl::Map& map, const std::filesystem::path& path)
{
  if (path.is_absolute())
  {
    return pathExists(path) ? std::optional{path} : std::nullopt;
  }

  for (const auto& searchPath : mdl::externalSearchPaths(map))
  {
    if (pathExists(searchPath / path))
    {
      return searchPath / path;
    }
  }
  return std::nullopt;
}

std::string softBoundsMode(const mdl::SoftMapBounds& bounds)
{
  return bounds.source == mdl::SoftMapBoundsType::Game ? "game"
         : bounds.bounds                               ? "custom"
                                                       : "unlimited";
}

void warnAboutLoggedProblems(CallContext& context)
{
  for (const auto& message : context.loggedProblems())
  {
    context.warn(
      message.level == LogLevel::Error ? "LOAD_ERROR" : "LOAD_WARNING", message.text);
  }
}

// game_list, game_info, game_set_path

ToolResult gameList(CallContext& context, const Args& args)
{
  auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto active = context.server().defaultDocument(context.session());
  const auto* activeGame =
    active ? &active->document->map().gameInfo().gameConfig.name : nullptr;

  auto items = std::vector<Json>{};
  for (const auto& gameInfo : context.host().gameManager().gameInfos())
  {
    const auto& config = gameInfo.gameConfig;
    auto formats = Json::array();
    for (const auto& format : config.fileFormats)
    {
      formats.push_back(format.format);
    }

    items.push_back(Json{
      {"name", config.name},
      {"formats", std::move(formats)},
      {"gamePath", gamePath(gameInfo).string()},
      {"gamePathValid", isGamePathValid(gameInfo)},
      {"experimental", config.experimental},
      {"activeDocumentGame", activeGame && *activeGame == config.name},
    });
  }

  return makePage(items, request.value(), 0);
}

ToolResult gameInfo(CallContext& context, const Args& args)
{
  if (const auto gameName = args.getOptional<std::string>("game"))
  {
    const auto* gameInfo = findGame(context.host(), *gameName);
    if (!gameInfo)
    {
      return unknownGameError(context.host(), *gameName);
    }
    return gameConfigJson(*gameInfo);
  }

  if (!context.hasDocument())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "No game was given and no document is open.",
      "Pass 'game', e.g. {\"game\": \"Quake\"}; game_list lists the games.");
  }
  return gameConfigJson(context.map().gameInfo());
}

ToolResult gameSetPath(CallContext& context, const Args& args)
{
  auto& host = context.host();
  const auto gameName = args.get<std::string>("game");
  const auto* found = findGame(host, gameName);
  if (!found)
  {
    return unknownGameError(host, gameName);
  }
  auto* gameInfo = host.gameManager().gameInfo(found->gameConfig.name);

  auto path = std::filesystem::path{};
  if (!args.get<std::string>("path").empty())
  {
    auto pathResult = absolutePathArgument(args, "path");
    if (pathResult.is_error())
    {
      return errorOf(pathResult);
    }
    path = pathResult.value();

    if (fs::Disk::pathInfo(path) != fs::PathInfo::Directory)
    {
      return makeError(
        ErrorCode::IoError,
        fmt::format("{} is not an existing folder.", path),
        "Pass the game's installation folder (the one that contains e.g. 'id1' for "
        "Quake), or '' to clear the path.");
    }
  }

  auto affected = Json::array();
  for (const auto& document : host.documents())
  {
    if (document.document->map().gameInfo().gameConfig.name == gameInfo->gameConfig.name)
    {
      affected.push_back(document.id);
    }
  }

  if (context.dryRun())
  {
    return Json{
      {"wouldDo",
       fmt::format("set the game folder of {} to '{}'", gameInfo->gameConfig.name, path)},
      {"game", gameInfo->gameConfig.name},
      {"path", path.string()},
      {"affectedDocuments", std::move(affected)},
    };
  }

  const auto previous = gamePath(*gameInfo);
  setPref(gameInfo->gamePathPreference, path);
  context.server().notifyResourceUpdated(gameConfigUri(gameInfo->gameConfig.name));

  return Json{
    {"game", gameInfo->gameConfig.name},
    {"path", path.string()},
    {"previousPath", previous.string()},
    {"valid", isGamePathValid(*gameInfo)},
    {"affectedDocuments", std::move(affected)},
  };
}

// mods

ToolResult modsGet(CallContext& context, const Args&)
{
  const auto& map = context.map();
  auto result = modsJson(map);
  result["available"] = findAvailableMods(map.gameInfo());
  result["gamePathValid"] = isGamePathValid(map.gameInfo());
  return result;
}

ToolResult modsSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto mods = args.get<std::vector<std::string>>("mods");

  for (size_t i = 0; i < mods.size(); ++i)
  {
    if (mods[i].empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Mod names must not be empty.",
        "Pass folder names such as 'hipnotic'.");
    }
    for (size_t j = 0; j < i; ++j)
    {
      if (kdl::ci::str_is_equal(mods[i], mods[j]))
      {
        return makeError(
          ErrorCode::InvalidArgument,
          fmt::format("The mod '{}' is listed twice.", mods[i]),
          "List each mod once, in priority order (last wins).");
      }
    }
  }

  const auto available = findAvailableMods(map.gameInfo());
  const auto defaultMod = mdl::defaultMod(map);
  for (const auto& mod : mods)
  {
    if (kdl::ci::str_is_equal(mod, defaultMod))
    {
      context.warn(
        "DEFAULT_MOD",
        fmt::format("'{}' is the game's default folder and is always searched.", mod));
    }
    else if (std::ranges::none_of(available, [&](const auto& availableMod) {
               return kdl::ci::str_is_equal(availableMod, mod);
             }))
    {
      context.warn(
        "UNKNOWN_MOD",
        fmt::format(
          "There is no folder '{}' in the game folder {}.",
          mod,
          gamePath(map.gameInfo())));
    }
  }

  if (mods != mdl::enabledMods(map))
  {
    mdl::setEnabledMods(map, mods);
  }
  warnAboutLoggedProblems(context);

  auto result = modsJson(map);
  result["available"] = available;
  return result;
}

// entity definitions

ToolResult entityDefinitionsGet(CallContext& context, const Args&)
{
  const auto& map = context.map();
  auto result = entityDefinitionsJson(map);

  const auto current = mdl::entityDefinitionFile(map);
  auto builtin = Json::array();
  for (const auto& path : map.gameInfo().gameConfig.entityConfig.defFilePaths)
  {
    builtin.push_back(Json{
      {"path", path.string()},
      {"current",
       current && current->type == mdl::EntityDefinitionFileSpec::Type::Builtin
         && current->path == path},
    });
  }
  result["builtin"] = std::move(builtin);
  return result;
}

ToolResult entityDefinitionsSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto type = args.get<std::string>("type");
  const auto pathStr = args.get<std::string>("path");
  const auto path = std::filesystem::path{pathStr};

  auto spec = mdl::EntityDefinitionFileSpec{};
  if (type == "builtin")
  {
    const auto& builtin = map.gameInfo().gameConfig.entityConfig.defFilePaths;
    const auto it = std::ranges::find_if(builtin, [&](const auto& builtinPath) {
      return kdl::ci::str_is_equal(builtinPath.string(), pathStr);
    });
    if (it == builtin.end())
    {
      auto names = std::vector<std::string>{};
      for (const auto& builtinPath : builtin)
      {
        names.push_back(builtinPath.string());
      }
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format(
          "'{}' is not a builtin entity definition file of {}.",
          pathStr,
          map.gameInfo().gameConfig.name),
        names.empty() ? "This game has no builtin files; use type 'external'."
                      : fmt::format(
                          "Use one of: {}, or type 'external' for your own file.",
                          kdl::str_join(names, ", ")));
    }
    spec = mdl::EntityDefinitionFileSpec::makeBuiltin(*it);
  }
  else
  {
    if (pathStr.empty())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The path of the external entity definition file is empty.",
        "Pass an absolute path, or a path relative to the map or game folder.");
    }
    if (!resolveExternalPath(map, path))
    {
      context.warn(
        "FILE_NOT_FOUND",
        fmt::format(
          "{} was not found (relative paths are searched next to the map, in the game "
          "folder and in the application folder).",
          path));
    }
    spec = mdl::EntityDefinitionFileSpec::makeExternal(path);
  }

  mdl::setEntityDefinitionFile(map, spec);
  warnAboutLoggedProblems(context);
  return entityDefinitionsJson(map);
}

void entityDefinitionsReload(CallContext& context, const Args&, ToolCompletion completion)
{
  if (context.dryRun())
  {
    completion(Json{{"wouldDo", "reload the entity definitions and models"}});
    return;
  }

  context.progress(0, 1, "Reloading entity definitions and models");
  context.defer([&context, completion]() {
    if (context.cancelled())
    {
      completion(makeError(
        ErrorCode::Cancelled, "The call was cancelled before the reload started."));
      return;
    }

    mdl::reloadEntityDefinitions(context.map());
    context.progress(1, 1, "Reloaded entity definitions");

    auto result = entityDefinitionsJson(context.map());
    result["loadMessages"] = toJson(context.loggedProblems());
    completion(std::move(result));
  });
}

// material collections

ToolResult materialsCollectionsGet(CallContext& context, const Args&)
{
  return materialsJson(context.map());
}

ToolResult materialsCollectionsSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& gameInfo = map.gameInfo();
  const auto wads = args.getOptional<std::vector<std::string>>("wads");
  const auto enabled = args.getOptional<std::vector<std::string>>("enabled");

  if (!wads && !enabled)
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Pass 'wads' (the ordered WAD file list) or 'enabled' (the enabled collections).",
      "materials_collections_get shows the current setup and which one applies.");
  }

  if (wads)
  {
    if (!usesWads(gameInfo))
    {
      return makeError(
        ErrorCode::Unsupported,
        fmt::format(
          "{} loads materials from folders, not from WAD files.",
          gameInfo.gameConfig.name),
        "Use 'enabled' to choose the material collections (folders).");
    }

    for (const auto& wad : *wads)
    {
      if (!resolveExternalPath(map, wad))
      {
        context.warn(
          "FILE_NOT_FOUND",
          fmt::format(
            "The WAD file {} was not found (relative paths are searched next to the "
            "map, in the game folder and in the application folder).",
            wad));
      }
    }

    auto entity = map.worldNode().entity();
    const auto value = mdl::joinWadProperty(*wads);
    const auto* current = entity.property(*gameInfo.gameConfig.materialConfig.property);
    if (!current || *current != value)
    {
      entity.addOrUpdateProperty(*gameInfo.gameConfig.materialConfig.property, value);
      if (!mdl::updateNodeContents(
            map,
            "Set WAD Files",
            {{&map.worldNode(), mdl::NodeContents{std::move(entity)}}}))
      {
        return context.operationFailed("Could not set the WAD files.");
      }
    }
  }

  if (enabled)
  {
    const auto& collections = map.materialManager().collections();
    auto paths = std::vector<std::filesystem::path>{};
    for (const auto& str : *enabled)
    {
      const auto path = std::filesystem::path{str};
      if (std::ranges::none_of(collections, [&](const auto& collection) {
            return collection.path() == path;
          }))
      {
        context.warn(
          "UNKNOWN_COLLECTION",
          fmt::format("There is no material collection '{}'.", str));
      }
      paths.push_back(path);
    }
    mdl::setEnabledMaterialCollections(map, paths);
  }

  warnAboutLoggedProblems(context);
  return materialsJson(map);
}

void materialsReload(CallContext& context, const Args&, ToolCompletion completion)
{
  if (context.dryRun())
  {
    completion(Json{{"wouldDo", "reload all material collections"}});
    return;
  }

  context.progress(0, 1, "Reloading material collections");
  context.defer([&context, completion]() {
    if (context.cancelled())
    {
      completion(makeError(
        ErrorCode::Cancelled, "The call was cancelled before the reload started."));
      return;
    }

    mdl::reloadMaterialCollections(context.map());
    context.progress(1, 1, "Reloaded material collections");

    auto result = materialsJson(context.map());
    result["loadMessages"] = toJson(context.loggedProblems());
    completion(std::move(result));
  });
}

// soft bounds

ToolResult softBoundsGet(CallContext& context, const Args&)
{
  return softBoundsJson(context.map());
}

ToolResult softBoundsSet(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto mode = args.get<std::string>("mode");
  const auto bounds = args.getOptional<vm::bbox3d>("bounds");

  auto softBounds = mdl::SoftMapBounds{mdl::SoftMapBoundsType::Game, std::nullopt};
  if (mode == "custom")
  {
    if (!bounds)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "Mode 'custom' needs 'bounds'.",
        "Pass e.g. {\"mode\": \"custom\", \"bounds\": {\"min\": [-4096,-4096,-4096], "
        "\"max\": [4096,4096,4096]}}.");
    }
    if (bounds->is_empty() || !map.worldBounds().contains(*bounds))
    {
      context.warn(
        "BOUNDS_OUTSIDE_WORLD",
        "The soft bounds are empty or extend beyond the world bounds "
          + dumpJson(toJson(map.worldBounds())) + ".");
    }
    softBounds = mdl::SoftMapBounds{mdl::SoftMapBoundsType::Map, *bounds};
  }
  else
  {
    if (bounds)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "'bounds' is only allowed with mode 'custom'.",
        "Remove 'bounds', or use mode 'custom'.");
    }
    if (mode == "unlimited")
    {
      softBounds = mdl::SoftMapBounds{mdl::SoftMapBoundsType::Map, std::nullopt};
    }
  }

  if (mdl::softMapBounds(map) != softBounds)
  {
    mdl::setSoftMapBounds(map, softBounds);
  }
  return softBoundsJson(map);
}

Schema wouldDoField()
{
  return string().describe("Dry run only: what the call would do");
}

Schema modsSchema()
{
  return object({
    field("default", string()).describe("The game's base folder, always searched first"),
    field("enabled", array(string())).describe("Enabled mods; later ones take priority"),
    field("available", array(string()))
      .describe("Folders in the game folder that can be enabled"),
    field("gamePathValid", boolean()),
  });
}

Schema entityDefinitionsSchema()
{
  return object({
    field("type", any()).describe("'builtin', 'external' or null if none"),
    field("path", any()).describe("File name (builtin) or path (external)"),
    field("spec", any()).describe("The worldspawn value, e.g. 'builtin:Quake.fgd'"),
    field("explicitlySet", boolean())
      .describe("false if the game's default file is used"),
    field("definitionCount", integer()).describe("Number of loaded entity classes"),
    field("builtin", array(any())).describe("Builtin files: {path, current}"),
    field("loadMessages", array(any())),
  });
}

Schema materialsSchema()
{
  return object({
    field("mode", enumOf({"wad", "folders"}))
      .describe("'wad': materials come from the WAD list; 'folders': from folders"),
    field("wadProperty", any()).describe("Worldspawn key of the WAD list, or null"),
    field("wads", array(string())).describe("The WAD list in order ('wad' mode)"),
    field("collections", array(any()))
      .describe("Loaded collections: {path, materialCount, enabled}"),
    field("enabled", array(string())).describe("Enabled collection paths"),
    field("collectionCount", integer()),
    field("materialCount", integer()).describe("Materials in enabled collections"),
    field("loadMessages", array(any())),
  });
}

Schema softBoundsSchema()
{
  return object({
    field("mode", enumOf({"game", "unlimited", "custom"})),
    field("bounds", any()).describe("The bounds in effect as {min, max}, or null"),
    field("gameDefault", any()).describe("The game's default bounds, or null"),
  });
}

} // namespace

std::string gameConfigUri(const std::string& gameName)
{
  return "trenchbroom://games/" + percentEncode(gameName) + "/config";
}

Json gameConfigJson(const mdl::GameInfo& gameInfo)
{
  const auto& config = gameInfo.gameConfig;

  auto formats = Json::array();
  for (const auto& format : config.fileFormats)
  {
    const auto initialMap = config.findInitialMap(format.format);
    formats.push_back(Json{
      {"format", format.format},
      {"initialMap",
       !config.forceEmptyNewMap && !format.initialMap.empty() ? pathOrNull(initialMap)
                                                              : Json(nullptr)},
    });
  }

  const auto& materialConfig = config.materialConfig;
  auto compilationTools = Json::array();
  for (const auto& tool : config.compilationTools)
  {
    compilationTools.push_back(Json{
      {"name", tool.name},
      {"description", tool.description ? Json(*tool.description) : Json(nullptr)},
      {"path", pref(tool.pathPreference).string()},
    });
  }

  return Json{
    {"name", config.name},
    {"experimental", config.experimental},
    {"configFile", pathOrNull(config.path)},
    {"formats", std::move(formats)},
    {"gamePath",
     Json{
       {"path", gamePath(gameInfo).string()},
       {"valid", isGamePathValid(gameInfo)},
     }},
    {"fileSystem",
     Json{
       {"searchPath", config.fileSystemConfig.searchPath.string()},
       {"packageFormat", config.fileSystemConfig.packageFormat.format},
       {"packageExtensions", pathsJson(config.fileSystemConfig.packageFormat.extensions)},
     }},
    {"materials",
     Json{
       {"mode", materialConfig.property ? "wad" : "folders"},
       {"root", materialConfig.root.string()},
       {"extensions", pathsJson(materialConfig.extensions)},
       {"palette", pathOrNull(materialConfig.palette)},
       {"wadProperty",
        materialConfig.property ? Json(*materialConfig.property) : Json(nullptr)},
       {"shaderSearchPath", pathOrNull(materialConfig.shaderSearchPath)},
       {"excludes", materialConfig.excludes},
     }},
    {"entityDefinitions",
     Json{
       {"builtin", pathsJson(config.entityConfig.defFilePaths)},
       {"setDefaultProperties", config.entityConfig.setDefaultProperties},
     }},
    {"tags", tagsJson(config.smartTags)},
    {"surfaceFlags", flagsJson(config.faceAttribsConfig.surfaceFlags)},
    {"contentFlags", flagsJson(config.faceAttribsConfig.contentFlags)},
    {"softMapBounds",
     config.softMapBounds ? toJson(*config.softMapBounds) : Json(nullptr)},
    {"compilationTools", std::move(compilationTools)},
    {"maxPropertyLength", config.maxPropertyLength},
  };
}

Json modsJson(const mdl::Map& map)
{
  return Json{
    {"default", mdl::defaultMod(map)},
    {"enabled", mdl::enabledMods(map)},
  };
}

Json entityDefinitionsJson(const mdl::Map& map)
{
  const auto spec = mdl::entityDefinitionFile(map);
  const auto explicitlySet =
    map.worldNode().entity().property(mdl::EntityPropertyKeys::TbEntityDefinitions)
    != nullptr;

  return Json{
    {"type",
     spec ? Json(
              spec->type == mdl::EntityDefinitionFileSpec::Type::Builtin ? "builtin"
                                                                         : "external")
          : Json(nullptr)},
    {"path", spec ? Json(spec->path.string()) : Json(nullptr)},
    {"spec", spec ? Json(spec->asString()) : Json(nullptr)},
    {"explicitlySet", explicitlySet},
    {"definitionCount", map.entityDefinitionManager().definitions().size()},
  };
}

Json materialsJson(const mdl::Map& map)
{
  const auto& gameInfo = map.gameInfo();
  const auto enabled = mdl::enabledMaterialCollections(map);

  auto collections = Json::array();
  auto materialCount = size_t{0};
  for (const auto& collection : map.materialManager().collections())
  {
    const auto isEnabled = std::ranges::find(enabled, collection.path()) != enabled.end();
    if (isEnabled)
    {
      materialCount += collection.materials().size();
    }
    collections.push_back(Json{
      {"path", collection.path().string()},
      {"materialCount", collection.materials().size()},
      {"enabled", isEnabled},
    });
  }

  auto result = Json{
    {"mode", usesWads(gameInfo) ? "wad" : "folders"},
    {"wadProperty",
     usesWads(gameInfo) ? Json(*gameInfo.gameConfig.materialConfig.property)
                        : Json(nullptr)},
  };
  if (usesWads(gameInfo))
  {
    result["wads"] = wadPaths(map);
  }
  result["collectionCount"] = collections.size();
  result["collections"] = std::move(collections);
  result["enabled"] = pathsJson(enabled);
  result["materialCount"] = materialCount;
  return result;
}

Json softBoundsJson(const mdl::Map& map)
{
  const auto bounds = mdl::softMapBounds(map);
  const auto& gameDefault = map.gameInfo().gameConfig.softMapBounds;
  return Json{
    {"mode", softBoundsMode(bounds)},
    {"bounds", bounds.bounds ? toJson(*bounds.bounds) : Json(nullptr)},
    {"gameDefault", gameDefault ? toJson(*gameDefault) : Json(nullptr)},
  };
}

void registerGameTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"game_list"}
      .title("List Games")
      .description(
        "Lists the games TrenchBroom supports with their map formats and whether the "
        "game folder is set up (gamePathValid). Without a valid game folder, materials, "
        "models and mods cannot be loaded; fix it with game_set_path. Example: {}")
      .input(object({}))
      .output(object({
        field(
          "items",
          array(object({
            field("name", string()).required(),
            field("formats", array(string())).describe("Map format names"),
            field("gamePath", string()),
            field("gamePathValid", boolean()),
            field("experimental", boolean()),
            field("activeDocumentGame", boolean())
              .describe("Whether the active document uses this game"),
          })))
          .required(),
        field("total", integer()).required(),
        field("nextCursor", any()),
      }))
      .mutation(Mutation::None)
      .paginated()
      .idempotent()
      .handler(gameList));

  registry.add(
    ToolDef{"game_info"}
      .title("Game Info")
      .description(
        "Describes a game configuration: map formats and their initial map templates, "
        "file system (base folder, package format), game folder, material setup (WAD "
        "list or folders, palette), builtin entity definition files, smart tags, "
        "surface and content flags, default soft map bounds and compile tools. Without "
        "'game' it describes the active document's game. Example: {\"game\": \"Quake "
        "2\"}")
      .input(object({
        field("game", string())
          .describe("Game name; default: the active document's game"),
      }))
      .output(object({
                       field("name", string()).required(),
                       field("formats", array(any())),
                       field("gamePath", any()),
                       field("fileSystem", any()),
                       field("materials", any()),
                       field("entityDefinitions", any()),
                       field("tags", array(any())),
                       field("surfaceFlags", array(any())),
                       field("contentFlags", array(any())),
                       field("softMapBounds", any()),
                       field("compilationTools", array(any())),
                     })
                .allowAdditionalProperties())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(gameInfo));

  registry.add(
    ToolDef{"game_set_path"}
      .title("Set Game Path")
      .description(
        "Sets the installation folder of a game (a preference, not undoable). Open "
        "documents of that game reload their materials, models and mods. Pass '' to "
        "clear it. Example: {\"game\": \"Quake\", \"path\": \"/home/me/quake\"}")
      .input(object({
        field("game", string()).required().describe("Game name from game_list"),
        field("path", string())
          .required()
          .describe("Absolute path of the game folder (containing e.g. 'id1'), or ''"),
      }))
      .output(object({
        field("game", string()).required(),
        field("path", string()).required(),
        field("previousPath", string()),
        field("valid", boolean()),
        field("affectedDocuments", array(string())),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::None)
      .idempotent()
      .handler(gameSetPath));

  registry.add(
    ToolDef{"mods_get"}
      .title("Get Mods")
      .description(
        "Returns the mods (game subfolders) enabled for the document in priority order "
        "(later ones override earlier ones), the game's default folder, and the folders "
        "available in the game folder. Example: {}")
      .input(object({}))
      .output(modsSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(modsGet));

  registry.add(
    ToolDef{"mods_set"}
      .title("Set Mods")
      .description(
        "Sets the enabled mods of the document in priority order (later ones override "
        "earlier ones); [] disables all. Stored in worldspawn (one undo step); entity "
        "definitions, models and materials are reloaded. Unknown folders produce "
        "warnings. Example: {\"mods\": [\"hipnotic\", \"quoth\"]}")
      .input(object({
        field("mods", array(string())).required().describe("Mod folder names, in order"),
      }))
      .output(modsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(modsSet));

  registry.add(
    ToolDef{"entity_definitions_get"}
      .title("Get Entity Definitions")
      .description(
        "Returns the entity definition file the document uses (builtin file of the game "
        "or an external file), the number of loaded entity classes, and the builtin "
        "files to choose from. Example: {}")
      .input(object({}))
      .output(entityDefinitionsSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(entityDefinitionsGet));

  registry.add(
    ToolDef{"entity_definitions_set"}
      .title("Set Entity Definitions")
      .description(
        "Chooses the entity definition file (FGD, DEF or ENT): a builtin file of the "
        "game (see entity_definitions_get) or an external file (absolute, or relative to "
        "the map or game folder). Stored in worldspawn (one undo step); definitions and "
        "models are reloaded and load problems are returned as warnings. Example: "
        "{\"type\": \"builtin\", \"path\": \"Quoth2.fgd\"}")
      .input(object({
        field("type", enumOf({"builtin", "external"})).required(),
        field("path", string())
          .required()
          .describe("Builtin file name, or path of an external file"),
      }))
      .output(entityDefinitionsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityDefinitionsSet));

  registry.add(
    ToolDef{"entity_definitions_reload"}
      .title("Reload Entity Definitions")
      .description(
        "Reloads the entity definition file and entity models from disk, e.g. after "
        "editing the FGD. Not undoable. Reports progress and can be cancelled before it "
        "starts; returns load problems. Example: {}")
      .input(object({}))
      .output(entityDefinitionsSchema())
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .asyncHandler(entityDefinitionsReload));

  registry.add(
    ToolDef{"materials_collections_get"}
      .title("Get Material Collections")
      .description(
        "Returns how the document gets its materials. 'wad' games (Quake, Half-Life) "
        "use an ordered WAD file list in worldspawn; 'folders' games (Quake 2, Quake 3) "
        "use material folders. Lists the loaded collections with material counts and "
        "whether each is enabled. Example: {}")
      .input(object({}))
      .output(materialsSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(materialsCollectionsGet));

  registry.add(
    ToolDef{"materials_collections_set"}
      .title("Set Material Collections")
      .description(
        "Sets the ordered WAD file list ('wads', WAD games only; absolute paths or paths "
        "relative to the map or game folder) and/or the enabled material collections "
        "('enabled', collection paths from materials_collections_get). Stored in "
        "worldspawn (one undo step); materials are reloaded. Example: {\"wads\": "
        "[\"/home/me/quake/id1/wads/base.wad\"]} or {\"enabled\": [\"textures/e1u1\"]}")
      .input(object({
        field("wads", array(string())).describe("WAD files in search order"),
        field("enabled", array(string())).describe("Collection paths to enable"),
      }))
      .output(materialsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(materialsCollectionsSet));

  registry.add(
    ToolDef{"materials_reload"}
      .title("Reload Materials")
      .description(
        "Reloads all material collections (WAD files or folders) from disk, e.g. after "
        "adding textures. Not undoable. Reports progress and can be cancelled before it "
        "starts; returns load problems. Example: {}")
      .input(object({}))
      .output(materialsSchema())
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .asyncHandler(materialsReload));

  registry.add(
    ToolDef{"soft_bounds_get"}
      .title("Get Soft Map Bounds")
      .description(
        "Returns the soft map bounds: the area the map should stay in (objects outside "
        "produce a validation issue). mode 'game' uses the game default, 'unlimited' "
        "disables them, 'custom' is a box stored in the map. Example: {}")
      .input(object({}))
      .output(softBoundsSchema())
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(softBoundsGet));

  registry.add(
    ToolDef{"soft_bounds_set"}
      .title("Set Soft Map Bounds")
      .description(
        "Sets the soft map bounds (one undo step): 'game' (game default), 'unlimited', "
        "or 'custom' with a box. Example: {\"mode\": \"custom\", \"bounds\": {\"min\": "
        "[-4096,-4096,-4096], \"max\": [4096,4096,4096]}}")
      .input(object({
        field("mode", enumOf({"game", "unlimited", "custom"})).required(),
        field("bounds", box()).describe("Required for mode 'custom'"),
      }))
      .output(softBoundsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(softBoundsSet));
}

} // namespace tb::mcp
