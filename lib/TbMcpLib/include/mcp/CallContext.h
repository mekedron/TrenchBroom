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
#include "mcp/LogCapture.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
  using Deferrer =
    std::function<void(std::function<void()>, std::chrono::milliseconds delay)>;

private:
  ServerState& m_server;
  Session& m_session;
  const ToolDef& m_tool;
  std::optional<DocumentInfo> m_document;
  bool m_dryRun;
  ProgressFn m_progress;
  std::vector<Warning> m_warnings;
  std::vector<Json> m_content;
  const ScopedLogCapture* m_logCapture = nullptr;
  std::optional<std::string> m_undoStep;
  bool m_cancelled = false;
  Deferrer m_deferrer;

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

  /**
   * Adds an image to the CallToolResult's `content`, after the text block that holds the
   * structured result. `data` are the raw bytes of the image, e.g. a PNG file.
   */
  void addImage(std::string_view data, std::string mimeType);
  /**
   * Adds a text block to the CallToolResult's `content`, after the blocks added so far,
   * e.g. a label before an image.
   */
  void addText(std::string text);
  /** The content blocks added with addImage and addText. */
  const std::vector<Json>& content() const;

  /** Emits `notifications/progress` if the client asked for progress. */
  void progress(double progress, std::optional<double> total, const std::string& message);

  /**
   * For tools that manage the history themselves (transaction_commit): the name of the
   * undo step the call created. Transactional tools get this set automatically.
   */
  void setUndoStep(std::string undoStep);
  const std::optional<std::string>& undoStep() const;

  void setLogCapture(const ScopedLogCapture* logCapture);

  /**
   * The warnings and errors that the editor logged for the target document during this
   * call so far, e.g. entity definition or material loading problems.
   */
  std::vector<LogMessage> loggedProblems() const;

  /** Whether the client cancelled the call (asynchronous tools only). */
  bool cancelled() const;
  void cancel();

  /**
   * For asynchronous tools: runs the given step later on the main thread, after pending
   * events (such as a cancellation) were processed. The step is dropped if the call was
   * abandoned, e.g. because the server shut down. With a delay, the step runs after
   * the delay instead, e.g. to wait for resources to load without blocking.
   */
  void defer(
    std::function<void()> step,
    std::chrono::milliseconds delay = std::chrono::milliseconds{0});
  void setDeferrer(Deferrer deferrer);

  /**
   * Returns an OPERATION_FAILED error that includes the warnings and errors the editor
   * logged during this call, e.g. when a Map_* function returned false.
   */
  ToolError operationFailed(std::string message, std::string hint = {}) const;
};

} // namespace mcp
} // namespace tb
