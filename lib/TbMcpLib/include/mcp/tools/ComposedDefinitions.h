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

#include "base/Result.h"
#include "mcp/Errors.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mdl
{
struct GameConfig;
} // namespace tb::mdl

namespace tb::mcp
{

// Composed entity definitions (E17.7): one FGD next to the map made of the game's FGD,
// the compile tools' FGD (e.g. sdHLT's func_detail and info_texlights) and MCP additions
// (model expressions for classes whose model comes from the "model" key). Later parts
// override earlier classes of the same name, as the FGD parser keeps the last one.

/** The first line of a composed FGD; files without it are not overwritten silently. */
inline constexpr auto ComposedFgdMarker = std::string_view{
  "// Composed by the TrenchBroom MCP server (entity_definitions_compose)"};

/** The composed FGD of a map: `/x/foo.map` -> `/x/foo.mcp.fgd`. */
std::filesystem::path composedFgdPath(const std::filesystem::path& mapPath);

/**
 * The FGD files of the configured compile tools: *.fgd files in the folders of the tools
 * and their parent folders (sdHLT ships tools/sdhlt.fgd next to tools/Linux/sdHLCSG).
 */
std::vector<std::filesystem::path> findCompilerFgds(const mdl::GameConfig& gameConfig);

/**
 * Reads an FGD file and replaces its @include directives by the included files
 * (relative to the including file), so that the text can be written elsewhere.
 */
Result<std::string, ToolError> readFgdInlined(const std::filesystem::path& path);

/** The names of the classes an FGD text defines (@PointClass, @SolidClass, ...). */
std::vector<std::string> fgdClassNames(std::string_view text);

/** A model expression the MCP adds to a class of the game's FGD. */
struct ModelAddition
{
  std::string classname;
  /** The class header property, e.g. `model({"path": model, "frame": sequence})`. */
  std::string property;
  std::string reason;
};

/** A class the MCP defines when no part defines it (e.g. func_detail for Half-Life). */
struct ClassAddition
{
  std::string classname;
  std::string definition;
  std::string reason;
};

struct McpAdditions
{
  std::vector<ModelAddition> models;
  std::vector<ClassAddition> classes;
};

/** The MCP additions for a game family ("halflife", ...); empty for other families. */
McpAdditions mcpAdditions(std::string_view family);

/**
 * Replaces the model property (model, studio, studioprop) of the last definition of the
 * class in the text by the addition's property, or adds it. Returns false if the text
 * does not define the class as a point class.
 */
bool applyModelAddition(std::string& text, const ModelAddition& addition);

/** One part of a composed FGD. */
struct FgdPart
{
  /** "game", "compiler" or "mcp". */
  std::string role;
  /** The source file; empty for the MCP part. */
  std::filesystem::path path;
  std::string text;
};

/**
 * The text of a composed FGD: the marker, the source lines (`// game: <path>`,
 * `// compiler: <path>`), then the parts in order.
 */
std::string composeFgd(const std::vector<FgdPart>& parts);

/** The sources named in a composed FGD's header: {role, path}. */
std::vector<std::pair<std::string, std::filesystem::path>> composedFgdSources(
  std::string_view text);

} // namespace tb::mcp
