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

#include "mcp/Errors.h"

#include <array>
#include <utility>

namespace tb::mcp
{
namespace
{

constexpr auto ErrorCodeNames = std::array<std::pair<ErrorCode, std::string_view>, 26>{{
  {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
  {ErrorCode::ObjectNotFound, "OBJECT_NOT_FOUND"},
  {ErrorCode::WrongObjectKind, "WRONG_OBJECT_KIND"},
  {ErrorCode::ObjectNotEditable, "OBJECT_NOT_EDITABLE"},
  {ErrorCode::NoSelection, "NO_SELECTION"},
  {ErrorCode::NoDocument, "NO_DOCUMENT"},
  {ErrorCode::DocumentNotFound, "DOCUMENT_NOT_FOUND"},
  {ErrorCode::ActiveDocumentClosed, "ACTIVE_DOCUMENT_CLOSED"},
  {ErrorCode::DocumentInUse, "DOCUMENT_IN_USE"},
  {ErrorCode::InvalidGeometry, "INVALID_GEOMETRY"},
  {ErrorCode::OutOfWorldBounds, "OUT_OF_WORLD_BOUNDS"},
  {ErrorCode::OperationFailed, "OPERATION_FAILED"},
  {ErrorCode::TransactionActive, "TRANSACTION_ACTIVE"},
  {ErrorCode::NoTransaction, "NO_TRANSACTION"},
  {ErrorCode::BusyTimeout, "BUSY_TIMEOUT"},
  {ErrorCode::Cancelled, "CANCELLED"},
  {ErrorCode::UnsavedChanges, "UNSAVED_CHANGES"},
  {ErrorCode::FileExists, "FILE_EXISTS"},
  {ErrorCode::IoError, "IO_ERROR"},
  {ErrorCode::Unsupported, "UNSUPPORTED"},
  {ErrorCode::UnsupportedInHost, "UNSUPPORTED_IN_HOST"},
  {ErrorCode::DryRunUnsupported, "DRY_RUN_UNSUPPORTED"},
  {ErrorCode::CompileRunning, "COMPILE_RUNNING"},
  {ErrorCode::DialogRequired, "DIALOG_REQUIRED"},
  {ErrorCode::ActionRefused, "ACTION_REFUSED"},
  {ErrorCode::InternalError, "INTERNAL_ERROR"},
}};

} // namespace

std::string_view toString(const ErrorCode code)
{
  for (const auto& [c, name] : ErrorCodeNames)
  {
    if (c == code)
    {
      return name;
    }
  }
  return "INTERNAL_ERROR";
}

std::optional<ErrorCode> errorCodeFromString(const std::string_view str)
{
  for (const auto& [c, name] : ErrorCodeNames)
  {
    if (name == str)
    {
      return c;
    }
  }
  return std::nullopt;
}

Json toJson(const ToolError& error)
{
  auto result = Json{
    {"code", toString(error.code)},
    {"message", error.message},
    {"objectIds", error.objectIds},
  };
  if (!error.hint.empty())
  {
    result["hint"] = error.hint;
  }
  if (!error.details.empty())
  {
    result["details"] = error.details;
  }
  return result;
}

std::string toText(const ToolError& error)
{
  auto result = std::string{toString(error.code)} + ": " + error.message;
  if (!error.hint.empty())
  {
    result += " Hint: " + error.hint;
  }
  return result;
}

ToolError makeError(
  const ErrorCode code,
  std::string message,
  std::string hint,
  std::vector<std::string> objectIds)
{
  return ToolError{code, std::move(message), std::move(objectIds), std::move(hint)};
}

Json toJson(const Warning& warning)
{
  return Json{
    {"code", warning.code},
    {"message", warning.message},
    {"objectIds", warning.objectIds},
  };
}

} // namespace tb::mcp
