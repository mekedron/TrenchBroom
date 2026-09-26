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

#include "mcp/Errors.h"
#include "mcp/Host.h"
#include "mcp/Json.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tb
{
namespace mdl
{
class Map;
}

namespace ui
{
class MapDocument;
}

namespace mcp
{
class DocumentState;
class IdRegistry;
class McpHost;
class ServerState;
class Session;
class ToolDef;

/**
 * Everything a tool handler sees: the server, the calling session, the target document,
 * the dry run flag, and a place for warnings and progress.
 */
class CallContext
{
public:
  using ProgressFn =
    std::function<void(double progress, std::optional<double> total, const std::string&)>;

private:
  ServerState& m_server;
  Session& m_session;
  const ToolDef& m_tool;
  std::optional<DocumentInfo> m_document;
  bool m_dryRun;
  ProgressFn m_progress;
  std::vector<Warning> m_warnings;
  const std::vector<std::string>* m_capturedMessages = nullptr;
  std::optional<std::string> m_undoStep;

public:
  CallContext(
    ServerState& server,
    Session& session,
    const ToolDef& tool,
    std::optional<DocumentInfo> document,
    bool dryRun,
    ProgressFn progress = {});

  ServerState& server();
  McpHost& host();
  Session& session();
  const ToolDef& tool() const;

  bool hasDocument() const;
  /** Precondition: hasDocument() */
  const DocumentInfo& documentInfo() const;
  /** Precondition: hasDocument() */
  ui::MapDocument& document();
  /** Precondition: hasDocument() */
  mdl::Map& map();
  /** Precondition: hasDocument() */
  DocumentState& documentState();
  /** Precondition: hasDocument() */
  IdRegistry& ids();

  bool dryRun() const;

  void warn(
    std::string code, std::string message, std::vector<std::string> objectIds = {});
  const std::vector<Warning>& warnings() const;

  /** Emits `notifications/progress` if the client asked for progress. */
  void progress(double progress, std::optional<double> total, const std::string& message);

  /**
   * For tools that manage the history themselves (transaction_commit): the name of the
   * undo step the call created. Transactional tools get this set automatically.
   */
  void setUndoStep(std::string undoStep);
  const std::optional<std::string>& undoStep() const;

  void setCapturedMessages(const std::vector<std::string>* messages);

  /**
   * Returns an OPERATION_FAILED error that includes the warnings and errors the editor
   * logged during this call, e.g. when a Map_* function returned false.
   */
  ToolError operationFailed(std::string message, std::string hint = {}) const;
};

} // namespace mcp
} // namespace tb
