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

#include "mcp/tools/ComposedDefinitions.h"

#include "mcp/tools/CompileUtils.h"
#include "mdl/GameConfig.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace tb::mcp
{
namespace
{

constexpr auto MaxIncludeDepth = 8;

Result<std::string, ToolError> readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return makeError(ErrorCode::IoError, fmt::format("{} cannot be read.", path));
  }
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

Result<std::string, ToolError> inlineIncludes(
  const std::filesystem::path& path, std::set<std::filesystem::path>& visiting, int depth)
{
  if (depth > MaxIncludeDepth)
  {
    return makeError(
      ErrorCode::IoError, fmt::format("{}: @include is nested too deeply.", path));
  }
  const auto normalized = path.lexically_normal();
  if (visiting.contains(normalized))
  {
    return makeError(ErrorCode::IoError, fmt::format("{} includes itself.", path));
  }

  auto text = readFile(path);
  if (text.is_error())
  {
    return text;
  }

  static const auto includePattern =
    std::regex{R"re(@include\s+"([^"]+)")re", std::regex::icase};
  auto result = std::string{};
  const auto& source = text.value();
  auto last = source.cbegin();
  visiting.insert(normalized);
  for (auto it = std::sregex_iterator{source.begin(), source.end(), includePattern};
       it != std::sregex_iterator{};
       ++it)
  {
    const auto& match = *it;
    // an @include in a comment stays as it is
    const auto lineStart = source.rfind('\n', size_t(match.position(0)));
    const auto linePrefix = source.substr(
      lineStart == std::string::npos ? 0 : lineStart + 1,
      size_t(match.position(0)) - (lineStart == std::string::npos ? 0 : lineStart + 1));
    if (linePrefix.find("//") != std::string::npos)
    {
      continue;
    }
    result.append(last, match[0].first);
    auto included =
      inlineIncludes(path.parent_path() / match[1].str(), visiting, depth + 1);
    if (included.is_error())
    {
      return included;
    }
    result += fmt::format("// included: \"{}\"\n", match[1].str());
    result += included.value();
    last = match[0].second;
  }
  visiting.erase(normalized);
  result.append(last, source.cend());
  return result;
}

bool isWordChar(const char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/** Removes `name(...)` properties (balanced parentheses) from a class header. */
void removeProperty(std::string& header, const std::string_view name)
{
  auto pos = size_t(0);
  while (pos < header.size())
  {
    const auto lower = kdl::str_to_lower(header);
    pos = lower.find(name, pos);
    if (pos == std::string::npos)
    {
      return;
    }
    const auto before = pos == 0 ? ' ' : header[pos - 1];
    auto open = pos + name.size();
    while (open < header.size() && std::isspace(static_cast<unsigned char>(header[open])))
    {
      ++open;
    }
    if (isWordChar(before) || open >= header.size() || header[open] != '(')
    {
      pos += name.size();
      continue;
    }

    auto depth = 0;
    auto end = open;
    for (; end < header.size(); ++end)
    {
      if (header[end] == '(')
      {
        ++depth;
      }
      else if (header[end] == ')' && --depth == 0)
      {
        break;
      }
    }
    if (end >= header.size())
    {
      return;
    }
    header.erase(pos, end + 1 - pos);
  }
}

const auto HalfLifeModel =
  std::string{R"(model({"path": model, "skin": skin, "frame": sequence}))"};

} // namespace

std::filesystem::path composedFgdPath(const std::filesystem::path& mapPath)
{
  auto result = mapPath;
  result.replace_extension(".mcp.fgd");
  return result;
}

std::vector<std::filesystem::path> findCompilerFgds(const mdl::GameConfig& gameConfig)
{
  auto result = std::vector<std::filesystem::path>{};
  auto seen = std::set<std::filesystem::path>{};
  for (const auto& tool : gameConfig.compilationTools)
  {
    const auto status = compileToolStatus(tool);
    if (status.path.empty() || status.status != "ok")
    {
      continue;
    }
    for (const auto& dir :
         {status.path.parent_path(), status.path.parent_path().parent_path()})
    {
      auto error = std::error_code{};
      auto files = std::vector<std::filesystem::path>{};
      for (const auto& entry : std::filesystem::directory_iterator{dir, error})
      {
        if (
          entry.is_regular_file(error)
          && kdl::ci::str_is_equal(entry.path().extension().string(), ".fgd"))
        {
          files.push_back(entry.path());
        }
      }
      std::ranges::sort(files);
      for (auto& file : files)
      {
        if (seen.insert(file.lexically_normal()).second)
        {
          result.push_back(std::move(file));
        }
      }
    }
  }
  return result;
}

Result<std::string, ToolError> readFgdInlined(const std::filesystem::path& path)
{
  auto visiting = std::set<std::filesystem::path>{};
  return inlineIncludes(path, visiting, 0);
}

std::vector<std::string> fgdClassNames(const std::string_view text)
{
  static const auto classPattern = std::regex{
    R"(@(PointClass|SolidClass|BaseClass|NPCClass|KeyFrameClass|MoveClass|FilterClass)\b[^=@]*=\s*([A-Za-z0-9_]+))",
    std::regex::icase};
  auto result = std::vector<std::string>{};
  const auto str = std::string{text};
  for (auto it = std::sregex_iterator{str.begin(), str.end(), classPattern};
       it != std::sregex_iterator{};
       ++it)
  {
    result.push_back((*it)[2].str());
  }
  return result;
}

McpAdditions mcpAdditions(const std::string_view family)
{
  if (family != "halflife")
  {
    return {};
  }
  const auto reason =
    std::string{"the model comes from the entity's model key (the FGD's empty studio())"};
  return McpAdditions{
    {
      {"monster_generic", HalfLifeModel, reason},
      {"monster_furniture", HalfLifeModel, reason},
      {"cycler", HalfLifeModel, reason},
      {"cycler_weapon", HalfLifeModel, reason},
    },
    {
      {"func_detail",
       "@SolidClass = func_detail : \"Detail brushes: merged into the world by sdHLT "
       "and VHLT, they neither seal the map nor split visibility\" []",
       "sdHLT and VHLT merge func_detail into the world; no FGD defines it"},
    },
  };
}

bool applyModelAddition(std::string& text, const ModelAddition& addition)
{
  const auto pattern = std::regex{
    "@PointClass\\b([^=@]*)=\\s*" + addition.classname + "(?![A-Za-z0-9_])",
    std::regex::icase};
  auto lastPosition = std::string::npos;
  auto lastLength = size_t(0);
  for (auto it = std::sregex_iterator{text.begin(), text.end(), pattern};
       it != std::sregex_iterator{};
       ++it)
  {
    lastPosition = size_t(it->position(1));
    lastLength = size_t(it->length(1));
  }
  if (lastPosition == std::string::npos)
  {
    return false;
  }

  auto header = text.substr(lastPosition, lastLength);
  for (const auto* name : {"studioprop", "studio", "model"})
  {
    removeProperty(header, name);
  }
  header = kdl::str_trim(header);
  header =
    " " + (header.empty() ? addition.property : header + " " + addition.property) + " ";
  text.replace(lastPosition, lastLength, header);
  return true;
}

std::string composeFgd(const std::vector<FgdPart>& parts)
{
  auto result = std::string{ComposedFgdMarker} + "\n";
  result += "// Regenerate it with entity_definitions_compose instead of editing it.\n";
  for (const auto& part : parts)
  {
    if (!part.path.empty())
    {
      result += fmt::format("// {}: {}\n", part.role, part.path.string());
    }
  }
  for (const auto& part : parts)
  {
    result += fmt::format(
      "\n// ---- {}{} ----\n",
      part.role,
      part.path.empty() ? std::string{} : ": " + part.path.string());
    result += part.text;
    if (!part.text.empty() && part.text.back() != '\n')
    {
      result += "\n";
    }
  }
  return result;
}

std::vector<std::pair<std::string, std::filesystem::path>> composedFgdSources(
  const std::string_view text)
{
  auto result = std::vector<std::pair<std::string, std::filesystem::path>>{};
  if (!text.starts_with(ComposedFgdMarker))
  {
    return result;
  }
  auto stream = std::istringstream{std::string{text}};
  auto line = std::string{};
  std::getline(stream, line);
  while (std::getline(stream, line) && line.starts_with("//"))
  {
    for (const auto* role : {"game", "compiler"})
    {
      const auto prefix = fmt::format("// {}: ", role);
      if (line.starts_with(prefix))
      {
        result.emplace_back(role, kdl::str_trim(line.substr(prefix.size())));
      }
    }
  }
  return result;
}

} // namespace tb::mcp
