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
#include "mcp/Json.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tb::mcp
{

/** Tool error codes. Serialized as UPPER_SNAKE strings. */
enum class ErrorCode
{
  InvalidArgument,
  ObjectNotFound,
  WrongObjectKind,
  ObjectNotEditable,
  NoSelection,
  NoDocument,
  DocumentNotFound,
  /**
   * The session's active document was closed (by the user or another session); calls
   * without a `document` argument fail until the session chooses another one.
   */
  ActiveDocumentClosed,
  /** The document is the active document of another session. */
  DocumentInUse,
  InvalidGeometry,
  OutOfWorldBounds,
  OperationFailed,
  TransactionActive,
  NoTransaction,
  BusyTimeout,
  Cancelled,
  UnsavedChanges,
  FileExists,
  IoError,
  Unsupported,
  UnsupportedInHost,
  DryRunUnsupported,
  /** A compilation of the document is already running. */
  CompileRunning,
  /**
   * The action opens a dialog, a file chooser or a confirmation that only the user can
   * answer (action_invoke without openDialog).
   */
  DialogRequired,
  /** The action must not run from a tool call; a semantic tool does the same. */
  ActionRefused,
  InternalError,
};

std::string_view toString(ErrorCode code);
std::optional<ErrorCode> errorCodeFromString(std::string_view str);

/**
 * A tool failure. It is reported to the agent as a CallToolResult with isError: true,
 * never as a JSON-RPC error.
 */
struct ToolError
{
  ErrorCode code = ErrorCode::InternalError;
  /** What went wrong, in one sentence. */
  std::string message;
  /** The objects involved. */
  std::vector<std::string> objectIds = {};
  /** A concrete next step, e.g. a tool name and argument. */
  std::string hint = {};
  /** Additional structured information, e.g. schema error paths. */
  Json details = Json::object();

  bool operator==(const ToolError&) const = default;
};

Json toJson(const ToolError& error);

/** Formats the error as a single line, e.g. "NO_SELECTION: ... Hint: ...". */
std::string toText(const ToolError& error);

ToolError makeError(
  ErrorCode code,
  std::string message,
  std::string hint = {},
  std::vector<std::string> objectIds = {});

/** A non-fatal problem, reported under `warnings` (X14). */
struct Warning
{
  std::string code;
  std::string message;
  std::vector<std::string> objectIds = {};
};

Json toJson(const Warning& warning);

/** The result of a tool handler. */
using ToolResult = Result<Json, ToolError>;

/** Returns the error of a failed result. Precondition: result.is_error() */
template <typename T>
ToolError errorOf(const Result<T, ToolError>& result)
{
  return std::get<ToolError>(result.error());
}

} // namespace tb::mcp
