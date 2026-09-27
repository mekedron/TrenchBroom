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
#include "mcp/tools/CompileUtils.h"
#include "mcp/tools/ComposedDefinitions.h"
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
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <set>
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

  const auto active = context.server().targetDocument(context.session()).document;
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

// entity_definitions_compose

/** The text of a file, or an empty string if it cannot be read. */
std::string readText(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

/** The game's FGD to compose from: a builtin file name or a path. */
Result<std::filesystem::path, ToolError> baseFgd(
  const mdl::Map& map,
  const std::optional<std::string>& argument,
  const std::optional<std::filesystem::path>& previous,
  const std::filesystem::path& output)
{
  const auto& gameConfig = map.gameInfo().gameConfig;
  const auto isFgd = [](const std::filesystem::path& path) {
    return kdl::ci::str_is_equal(path.extension().string(), ".fgd");
  };
  const auto builtin =
    [&](const std::filesystem::path& name) -> std::optional<std::filesystem::path> {
    const auto& paths = gameConfig.entityConfig.defFilePaths;
    const auto it = std::ranges::find_if(paths, [&](const auto& path) {
      return kdl::ci::str_is_equal(path.string(), name.string());
    });
    if (it == paths.end())
    {
      return std::nullopt;
    }
    return gameConfig.findConfigFile(*it);
  };

  auto result = std::optional<std::filesystem::path>{};
  if (argument)
  {
    result = builtin(*argument);
    if (!result)
    {
      result = resolveExternalPath(map, *argument);
    }
    if (!result)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The base FGD '{}' was not found.", *argument),
        "Pass a builtin file of the game (entity_definitions_get) or an absolute path.");
    }
  }
  else if (previous && pathExists(*previous))
  {
    result = previous;
  }
  else if (const auto spec = mdl::entityDefinitionFile(map);
           spec && spec->type == mdl::EntityDefinitionFileSpec::Type::Builtin)
  {
    result = builtin(spec->path);
  }
  else if (
    const auto external = spec ? resolveExternalPath(map, spec->path) : std::nullopt)
  {
    // a composed file (the map's own or another one) is replaced by its game source
    const auto sources = composedFgdSources(readText(*external));
    const auto game = std::ranges::find_if(
      sources, [](const auto& source) { return source.first == "game"; });
    if (game != sources.end())
    {
      result = game->second;
    }
    else if (external->lexically_normal() != output.lexically_normal())
    {
      result = external;
    }
  }
  if (!result)
  {
    for (const auto& path : gameConfig.entityConfig.defFilePaths)
    {
      if (isFgd(path))
      {
        result = gameConfig.findConfigFile(path);
        break;
      }
    }
  }

  if (!result || !isFgd(*result))
  {
    return makeError(
      ErrorCode::Unsupported,
      fmt::format(
        "{} has no FGD to compose from{}.",
        gameConfig.name,
        result ? fmt::format(" ({} is not an FGD)", *result) : std::string{}),
      "Composed entity definitions need FGD files; pass 'base' with an FGD.");
  }
  return *result;
}

ToolResult entityDefinitionsCompose(CallContext& context, const Args& args)
{
  auto& map = context.map();
  const auto& documentGame = map.gameInfo();
  const auto* managedGame =
    context.host().gameManager().gameInfo(documentGame.gameConfig.name);
  const auto& gameConfig = (managedGame ? *managedGame : documentGame).gameConfig;

  auto output = std::filesystem::path{};
  if (const auto path = args.getOptional<std::string>("path"))
  {
    output = *path;
    if (!output.is_absolute())
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("'{}' is not an absolute path.", *path),
        "Pass an absolute path, or omit it to write <map>.mcp.fgd next to the map.");
    }
  }
  else if (map.path().is_absolute())
  {
    output = composedFgdPath(map.path());
  }
  else
  {
    return makeError(
      ErrorCode::UnsavedChanges,
      "The map has never been saved, so there is no folder to write the FGD to.",
      "Save it first with document_save_as, or pass an absolute 'path'.");
  }

  // a composed file names its sources, so that regenerating it keeps them
  auto previousGame = std::optional<std::filesystem::path>{};
  auto previousCompiler = std::vector<std::filesystem::path>{};
  if (pathExists(output))
  {
    const auto text = readText(output);
    if (!text.starts_with(ComposedFgdMarker) && !args.get<bool>("overwrite"))
    {
      return makeError(
        ErrorCode::FileExists,
        fmt::format(
          "{} exists and was not composed by entity_definitions_compose.", output),
        "Pass overwrite: true to replace it, or another 'path'.");
    }
    for (const auto& [role, path] : composedFgdSources(text))
    {
      if (role == "game")
      {
        previousGame = path;
      }
      else if (pathExists(path))
      {
        previousCompiler.push_back(path);
      }
    }
  }

  auto base = baseFgd(map, args.getOptional<std::string>("base"), previousGame, output);
  if (base.is_error())
  {
    return errorOf(base);
  }

  auto compilerFgds = std::vector<std::filesystem::path>{};
  if (const auto path = args.getOptional<std::string>("compilerFgd"))
  {
    const auto resolved = resolveExternalPath(map, *path);
    if (!resolved)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        fmt::format("The compiler FGD '{}' was not found.", *path),
        "Pass an absolute path, e.g. the sdhlt.fgd of sdHLT.");
    }
    compilerFgds.push_back(*resolved);
  }
  else if (args.get<bool>("includeCompilerFgd"))
  {
    compilerFgds =
      !previousCompiler.empty() ? previousCompiler : findCompilerFgds(gameConfig);
  }

  auto parts = std::vector<FgdPart>{};
  auto sources = Json::array();
  const auto addPart = [&](
                         const std::string& role,
                         const std::filesystem::path& path) -> std::optional<ToolError> {
    auto text = readFgdInlined(path);
    if (text.is_error())
    {
      auto error = errorOf(text);
      error.hint = "Check the path and the files it includes.";
      return error;
    }
    sources.push_back(Json{
      {"role", role},
      {"path", path.string()},
      {"classes", fgdClassNames(text.value()).size()},
    });
    parts.push_back(FgdPart{role, path, std::move(text).value()});
    return std::nullopt;
  };
  if (auto error = addPart("game", base.value()))
  {
    return *error;
  }
  for (const auto& path : compilerFgds)
  {
    if (auto error = addPart("compiler", path))
    {
      return *error;
    }
  }

  // MCP additions: model expressions on the last definition, missing classes
  auto additions = Json::array();
  if (args.get<bool>("mcpAdditions"))
  {
    const auto family = compileFamily(gameConfig);
    const auto mcp = mcpAdditions(family ? toString(*family) : std::string_view{});
    for (const auto& addition : mcp.models)
    {
      const auto applied = std::ranges::any_of(
        parts | std::views::reverse,
        [&](auto& part) { return applyModelAddition(part.text, addition); });
      if (applied)
      {
        additions.push_back(Json{
          {"classname", addition.classname},
          {"kind", "model"},
          {"definition", addition.property},
          {"reason", addition.reason},
        });
      }
    }

    auto defined = std::set<std::string>{};
    for (const auto& part : parts)
    {
      for (auto& name : fgdClassNames(part.text))
      {
        defined.insert(kdl::str_to_lower(name));
      }
    }
    auto mcpText = std::string{};
    for (const auto& addition : mcp.classes)
    {
      if (!defined.contains(kdl::str_to_lower(addition.classname)))
      {
        mcpText += addition.definition + "\n";
        additions.push_back(Json{
          {"classname", addition.classname},
          {"kind", "class"},
          {"definition", addition.definition},
          {"reason", addition.reason},
        });
      }
    }
    if (!mcpText.empty())
    {
      parts.push_back(FgdPart{"mcp", {}, std::move(mcpText)});
    }
  }

  const auto text = composeFgd(parts);
  // next to the map, the file is referenced by its name, so the map folder can move
  const auto spec = mdl::EntityDefinitionFileSpec::makeExternal(
    map.path().is_absolute() && output.parent_path() == map.path().parent_path()
      ? output.filename()
      : output);

  auto result = Json{
    {"path", output.string()},
    {"spec", spec.asString()},
    {"sources", std::move(sources)},
    {"additions", std::move(additions)},
    {"bytes", text.size()},
  };
  if (compilerFgds.empty() && args.get<bool>("includeCompilerFgd"))
  {
    context.warn(
      "NO_COMPILER_FGD",
      "No FGD was found next to the configured compile tools (compile_tools_get); pass "
      "'compilerFgd' to include one.");
  }

  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format(
      "write {} ({} parts) and load it as the map's entity definitions",
      output,
      parts.size());
    return result;
  }

  {
    auto error = std::error_code{};
    std::filesystem::create_directories(output.parent_path(), error);
    auto stream = std::ofstream{output, std::ios::binary | std::ios::trunc};
    stream << text;
    if (!stream)
    {
      return makeError(
        ErrorCode::IoError,
        fmt::format("{} cannot be written.", output),
        "Check that the folder is writable, or pass another 'path'.");
    }
  }

  if (mdl::entityDefinitionFile(map) == spec)
  {
    mdl::reloadEntityDefinitions(map);
  }
  else
  {
    mdl::setEntityDefinitionFile(map, spec);
  }
  warnAboutLoggedProblems(context);
  result["entityDefinitions"] = entityDefinitionsJson(map);
  return result;
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

    // compile tools open relative WAD paths relative to their working directory, so
    // found relative paths are stored as absolute paths unless keepRelative is set
    const auto keepRelative = args.get<bool>("keepRelative");
    auto paths = std::vector<std::string>{};
    for (const auto& wad : *wads)
    {
      const auto resolved = resolveExternalPath(map, wad);
      if (!resolved)
      {
        context.warn(
          "FILE_NOT_FOUND",
          fmt::format(
            "The WAD file {} was not found (relative paths are searched next to the "
            "map, in the game folder and in the application folder).",
            wad));
        paths.push_back(wad);
      }
      else if (std::filesystem::path{wad}.is_absolute())
      {
        paths.push_back(wad);
      }
      else if (keepRelative)
      {
        context.warn(
          "RELATIVE_WAD_PATH",
          fmt::format(
            "The WAD path {} is relative; the editor finds it, but compile tools such "
            "as hlcsg may not. The absolute path is {}.",
            wad,
            resolved->lexically_normal()));
        paths.push_back(wad);
      }
      else
      {
        paths.push_back(resolved->lexically_normal().string());
      }
    }

    auto entity = map.worldNode().entity();
    const auto value = mdl::joinWadProperty(paths);
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
    field("gamePathValid", boolean())
      .describe("Whether the game folder exists; without it no mods are available"),
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
    field("loadMessages", array(any()))
      .describe("{level, message} logged while loading the definitions"),
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
    field("collectionCount", integer()).describe("Number of loaded collections"),
    field("materialCount", integer()).describe("Materials in enabled collections"),
    field("loadMessages", array(any()))
      .describe("{level, message} logged while loading the materials"),
  });
}

Schema softBoundsSchema()
{
  return object({
    field("mode", enumOf({"game", "unlimited", "custom"}))
      .describe(
        "'game': the game default, 'unlimited': none, 'custom': stored in the map"),
    field("bounds", any())
      .describe("The bounds in effect as {min, max} in map units, or null"),
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

std::vector<RelativeWadPath> relativeWadPaths(const mdl::Map& map)
{
  auto result = std::vector<RelativeWadPath>{};
  for (const auto& wad : wadPaths(map))
  {
    if (!wad.empty() && !std::filesystem::path{wad}.is_absolute())
    {
      auto absolutePath = resolveExternalPath(map, wad);
      result.push_back(RelativeWadPath{
        wad,
        absolutePath ? std::optional{absolutePath->lexically_normal()} : std::nullopt,
      });
    }
  }
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
        "Lists the games TrenchBroom supports (read-only) with their map formats, game "
        "folder and whether it is set up (gamePathValid). Without a valid game folder, "
        "materials, models and mods cannot be loaded; fix it with game_set_path. "
        "game_info describes one game in detail. Example: {}")
      .input(object({}))
      .output(object({
        field(
          "items",
          array(object({
            field("name", string()).required().describe("Game name, e.g. 'Quake'"),
            field("formats", array(string()))
              .describe("Map format names, the default first"),
            field("gamePath", string()).describe("Configured game folder ('' if unset)"),
            field("gamePathValid", boolean()).describe("Whether the game folder exists"),
            field("experimental", boolean())
              .describe("Whether TrenchBroom marks the game support as experimental"),
            field("activeDocumentGame", boolean())
              .describe("Whether the active document uses this game"),
          })))
          .required(),
        field("total", integer()).required().describe("Number of games"),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
      }))
      .mutation(Mutation::None)
      .paginated()
      .idempotent()
      .handler(gameList));

  registry.add(
    ToolDef{"game_info"}
      .title("Game Info")
      .description(
        "Describes a game configuration (read-only): map formats and their initial map "
        "templates, file system (base folder, package format), game folder, material "
        "setup (WAD list or folders, palette), builtin entity definition files, smart "
        "tags, surface and content flags, default soft map bounds and compile tools. "
        "Without 'game' it describes the active document's game. Example: "
        "{\"game\": \"Quake 2\"}")
      .input(object({
        field("game", string())
          .describe("Game name from game_list; default: the active document's game"),
      }))
      .output(object({
                       field("name", string()).required(),
                       field("formats", array(any()))
                         .describe("{format, initialMap}: formats and their templates"),
                       field("gamePath", any()).describe("{path, valid}"),
                       field("fileSystem", any())
                         .describe("{searchPath, packageFormat, packageExtensions}"),
                       field("materials", any())
                         .describe("{mode ('wad' or 'folders'), root, extensions, "
                                   "palette, wadProperty, shaderSearchPath, excludes}"),
                       field("entityDefinitions", any())
                         .describe("{builtin (definition files), setDefaultProperties}"),
                       field("tags", array(any())).describe("Smart tags"),
                       field("surfaceFlags", array(any())),
                       field("contentFlags", array(any())),
                       field("softMapBounds", any())
                         .describe("Default soft bounds {min, max}, or null"),
                       field("compilationTools", array(any()))
                         .describe("{name, description, path} of the compile tools"),
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
        "clear it. Fails with IO_ERROR if the folder does not exist. Example: "
        "{\"game\": \"Quake\", \"path\": \"/home/me/quake\"}")
      .input(object({
        field("game", string()).required().describe("Game name from game_list"),
        field("path", string())
          .required()
          .describe("Absolute path of the game folder (containing e.g. 'id1'), or ''"),
      }))
      .output(object({
        field("game", string()).required(),
        field("path", string()).required(),
        field("previousPath", string()).describe("The former folder ('' if unset)"),
        field("valid", boolean()).describe("Whether the game folder is valid now"),
        field("affectedDocuments", array(string()))
          .describe("Handles of the open documents that were reloaded"),
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
        "Returns the mods (game subfolders) enabled for the document (read-only) in "
        "priority order (later ones override earlier ones), the game's default folder, "
        "and the folders available in the game folder. Change them with mods_set. "
        "Example: {}")
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
        "definitions, models and materials are reloaded. Folders that do not exist give "
        "warnings; mods_get lists the available ones. Example: "
        "{\"mods\": [\"hipnotic\", \"quoth\"]}")
      .input(object({
        field("mods", array(string()))
          .required()
          .describe("Mod folder names in priority order, each once; [] for none"),
      }))
      .output(modsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(modsSet));

  registry.add(
    ToolDef{"entity_definitions_get"}
      .title("Get Entity Definitions")
      .description(
        "Returns the entity definition file the document uses (read-only; a builtin "
        "file of the game or an external file), the number of loaded entity classes, "
        "and the builtin files to choose from. Change it with entity_definitions_set; "
        "entity_classes_list lists the classes. Example: {}")
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
        "game or an external file. Stored in worldspawn (one undo step); definitions "
        "and models are reloaded and load problems are returned as warnings (a missing "
        "external file gives FILE_NOT_FOUND). Examples: {\"type\": \"builtin\", "
        "\"path\": \"Quoth2.fgd\"}; {\"type\": \"external\", \"path\": "
        "\"/home/me/quake/mymod/mymod.fgd\"}")
      .input(object({
        field("type", enumOf({"builtin", "external"}))
          .required()
          .describe("'builtin': a file of the game; 'external': your own file"),
        field("path", string())
          .required()
          .describe("Builtin file name (entity_definitions_get), or path of an external "
                    "file (absolute, or relative to the map or game folder)"),
      }))
      .output(entityDefinitionsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityDefinitionsSet));

  registry.add(
    ToolDef{"entity_definitions_compose"}
      .title("Compose Entity Definitions")
      .description(
        "Writes one FGD for the map, <map>.mcp.fgd next to it (or 'path'), and loads it "
        "as the map's entity definitions (stored in worldspawn, one undo step; the "
        "file itself stays). It joins, in order: the game's FGD ('base'; default: the "
        "one the map uses, or the game's first builtin FGD), the compile tools' FGD "
        "(default: *.fgd files next to the configured compile tools and in their parent "
        "folder, e.g. sdHLT's sdhlt.fgd with func_detail, info_texlights and "
        "light_surface; or 'compilerFgd'), and MCP additions (Half-Life: a model "
        "expression from the model key for monster_generic, monster_furniture, cycler "
        "and cycler_weapon, so their models render; func_detail when no FGD defines it). "
        "Later parts override classes of the same name; @include files are inlined. The "
        "file names its sources, so calling the tool again regenerates it with the same "
        "ones (e.g. after the compile tools changed). A file that was not composed is "
        "not overwritten without overwrite: true. dryRun reports the parts without "
        "writing. Returns the path, the spec stored in the map, the sources with their "
        "class counts, the additions and the loaded definitions. Examples: {}; "
        "{\"compilerFgd\": \"/opt/sdhlt/tools/sdhlt.fgd\"}; {\"base\": "
        "\"HalfLife.fgd\", \"includeCompilerFgd\": false}")
      .input(object({
        field("base", string().nonEmpty())
          .describe(
            "The game's FGD: a builtin file (entity_definitions_get) or a path. Default: "
            "the FGD the map uses (or the composed file's game source), else the game's "
            "first builtin FGD"),
        field("compilerFgd", string().nonEmpty())
          .describe("The compile tools' FGD (absolute path). Default: found next to the "
                    "configured compile tools"),
        field("includeCompilerFgd", boolean().defaultsTo(true))
          .describe("Include the compile tools' FGD found next to the tools"),
        field("mcpAdditions", boolean().defaultsTo(true))
          .describe("Add the MCP model expressions and missing compiler classes"),
        field("path", string().nonEmpty())
          .describe("Absolute path of the FGD to write. Default: <map>.mcp.fgd"),
        field("overwrite", boolean().defaultsTo(false))
          .describe("Replace a file at path that was not composed by this tool"),
      }))
      .output(object({
        field("path", string()).required().describe("The composed FGD"),
        field("spec", string())
          .required()
          .describe("The entity definition file stored in the map"),
        field("sources", array(any()))
          .required()
          .describe("[{role: game | compiler, path, classes}]"),
        field("additions", array(any()))
          .required()
          .describe("[{classname, kind: model | class, definition, reason}]"),
        field("bytes", integer()).required(),
        field("entityDefinitions", any())
          .describe("The loaded definitions, as entity_definitions_get"),
        field("wouldDo", wouldDoField()),
      }))
      .mutation(Mutation::Map)
      .idempotent()
      .handler(entityDefinitionsCompose));

  registry.add(
    ToolDef{"entity_definitions_reload"}
      .title("Reload Entity Definitions")
      .description(
        "Reloads the entity definition file and entity models from disk, e.g. after "
        "editing the FGD (not undoable). Reports progress and can be cancelled before it "
        "starts; returns the definition setup like entity_definitions_get, with load "
        "problems. Example: {}")
      .input(object({}))
      .output(entityDefinitionsSchema())
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .asyncHandler(entityDefinitionsReload));

  registry.add(
    ToolDef{"materials_collections_get"}
      .title("Get Material Collections")
      .description(
        "Returns how the document gets its materials (read-only). 'wad' games (Quake, "
        "Half-Life) use an ordered WAD file list in worldspawn; 'folders' games (Quake "
        "2, Quake 3) use material folders. Lists the loaded collections with material "
        "counts and whether each is enabled. Change them with materials_collections_set; "
        "materials_list lists the materials. Example: {}")
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
        "Sets the ordered WAD file list ('wads', WAD games only) and/or the enabled "
        "material collections ('enabled'); pass at least one. Stored in worldspawn (one "
        "undo step); materials are reloaded. Relative WAD paths that are found are "
        "stored as absolute paths (like the editor's default), because compile tools "
        "such as hlcsg cannot open paths relative to the game folder; keepRelative "
        "stores them as passed (warning RELATIVE_WAD_PATH). Examples: {\"wads\": "
        "[\"/home/me/quake/id1/wads/base.wad\"]}; {\"wads\": "
        "[\"valve/halflife.wad\"]} (stored as <game folder>/valve/halflife.wad); "
        "{\"enabled\": [\"textures/e1u1\"]}")
      .input(object({
        field("wads", array(string()))
          .describe("WAD files in search order, replacing the list: absolute paths or "
                    "paths relative to the map, game or application folder"),
        field("keepRelative", boolean().defaultsTo(false))
          .describe("Store relative WAD paths as passed instead of as absolute paths "
                    "(for maps shared between machines; compile tools may not find "
                    "them)"),
        field("enabled", array(string()))
          .describe("Collection paths to enable (materials_collections_get); the "
                    "others are disabled"),
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
        "adding textures (not undoable). Reports progress and can be cancelled before "
        "it starts; returns the material setup like materials_collections_get, with "
        "load problems. Example: {}")
      .input(object({}))
      .output(materialsSchema())
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Required)
      .asyncHandler(materialsReload));

  registry.add(
    ToolDef{"soft_bounds_get"}
      .title("Get Soft Map Bounds")
      .description(
        "Returns the soft map bounds (read-only): the area in map units the map should "
        "stay in (objects outside produce a validation issue). mode 'game' uses the "
        "game default, 'unlimited' disables them, 'custom' is a box stored in the map. "
        "Change them with soft_bounds_set. Example: {}")
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
        "Sets the soft map bounds (one undo step, stored in worldspawn): 'game' (game "
        "default), 'unlimited', or 'custom' with a box in map units; 'bounds' is only "
        "allowed with 'custom'. Examples: {\"mode\": \"custom\", \"bounds\": "
        "{\"min\": [-4096,-4096,-4096], \"max\": [4096,4096,4096]}}; "
        "{\"mode\": \"game\"}")
      .input(object({
        field("mode", enumOf({"game", "unlimited", "custom"}))
          .required()
          .describe("'game': the game default, 'unlimited': no bounds, 'custom': "
                    "'bounds'"),
        field("bounds", box()).describe("Box in map units; required for mode 'custom'"),
      }))
      .output(softBoundsSchema())
      .mutation(Mutation::Map)
      .idempotent()
      .handler(softBoundsSet));
}

} // namespace tb::mcp
