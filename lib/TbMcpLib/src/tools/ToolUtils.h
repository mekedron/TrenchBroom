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

#include "base/Error.h"
#include "base/Result.h"
#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/LogCapture.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tb::mdl
{
struct GameInfo;
}

namespace tb::mcp
{
class Args;
class McpHost;
struct DocumentInfo;

// Shared building blocks of the tool implementations. Keep this small; it must not grow
// into a second tool file.

/** Formats a time point as ISO 8601 in UTC, e.g. "2026-09-27T10:15:00Z". */
std::string isoTime(std::chrono::system_clock::time_point time);
std::string isoTime(std::filesystem::file_time_type time);

/** Returns the game with the given name (case-insensitive), or nullptr. */
const mdl::GameInfo* findGame(McpHost& host, std::string_view name);

/** The names of all configured games. */
std::vector<std::string> gameNames(McpHost& host);

/** An INVALID_ARGUMENT error for an unknown game that lists the known games. */
ToolError unknownGameError(McpHost& host, std::string_view name);

/**
 * The problems of the items of a bulk tool (brushes_create, entities_create), each with
 * the index of its item, so that all of them are reported at once.
 */
class ItemErrors
{
private:
  Json m_errors = Json::array();

public:
  void add(size_t index, std::string message);
  /** Adds the error's message and hint (and its code unless INVALID_ARGUMENT). */
  void add(size_t index, const ToolError& error);
  bool empty() const;

  /**
   * INVALID_ARGUMENT whose message names the first problems and how many items are
   * invalid; details.errors lists all problems as [{index, message, code?, hint?}].
   */
  ToolError error(size_t itemCount, std::string hint) const;
};

/**
 * A DOCUMENT_IN_BACKGROUND error if the given document is a background document, which
 * has no editor window; `what` names what needs the window, e.g. "The user views".
 */
std::optional<ToolError> backgroundDocumentError(
  const DocumentInfo& document, std::string_view what);

/** The configured game folder of the given game (empty if not set). */
std::filesystem::path gamePath(const mdl::GameInfo& gameInfo);

/** Whether the game folder of the given game is set and is an existing directory. */
bool isGamePathValid(const mdl::GameInfo& gameInfo);

/**
 * Reads a path argument that must be absolute. Returns the lexically normalized path or
 * an INVALID_ARGUMENT error.
 */
Result<std::filesystem::path, ToolError> absolutePathArgument(
  const Args& args, std::string_view key);

/** Converts log messages to `[{"level": "warning", "message": "..."}]`. */
Json toJson(const std::vector<LogMessage>& messages);

/** Percent-encodes everything but unreserved URI characters (RFC 3986). */
std::string percentEncode(std::string_view str);

/** Decodes percent-encoded characters; returns nullopt for malformed input. */
std::optional<std::string> percentDecode(std::string_view str);

/** The message of a failed editor result. Precondition: result.is_error() */
template <typename T>
std::string errorMessage(const Result<T>& result)
{
  return std::get<Error>(result.error()).msg;
}

/** Whether the given path names an existing file or directory. */
bool pathExists(const std::filesystem::path& path);

} // namespace tb::mcp
