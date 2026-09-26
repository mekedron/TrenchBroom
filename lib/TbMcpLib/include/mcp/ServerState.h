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

#include "base/NotifierConnection.h"
#include "mcp/CallLog.h"
#include "mcp/Host.h"
#include "mcp/McpServer.h"
#include "mcp/ObjectIds.h"
#include "mcp/PromptRegistry.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/Session.h"
#include "mcp/ToolRegistry.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb::ui
{
class MapDocument;
}

namespace tb::mcp
{
class CallRunner;
class Scheduler;

/** An explicit agent transaction (transaction_begin). */
struct AgentTransaction
{
  std::string sessionId;
  std::string clientName;
  std::string name;
  /** The document's transaction depth right after the transaction was started. */
  size_t depth = 0;
};

/** The server's state for one open document. */
class DocumentState
{
public:
  ui::MapDocument& document;
  IdRegistry ids;
  std::optional<AgentTransaction> transaction;

private:
  NotifierConnection m_notifierConnection;
  std::function<void()> m_infoDidChange;
  bool m_lastModified = false;

public:
  /**
   * The callback is called when the document info (trenchbroom://documents/{doc}/info)
   * may have changed: saved, loaded, modified flag, mods, entity definitions, materials
   * or worldspawn changed.
   */
  explicit DocumentState(
    ui::MapDocument& document, std::function<void()> infoDidChange = {});

private:
  void infoDidChange();
};

/**
 * The shared state of the MCP server. Tool handlers reach it through
 * `CallContext::server()`.
 */
class ServerState
{
public:
  McpServer& server;
  McpHost& host;
  Scheduler& scheduler;
  ServerInfo info;
  ServerOptions options;

  ToolRegistry tools;
  ResourceRegistry resources;
  PromptRegistry prompts;
  CallLog callLog;

  std::map<std::string, std::unique_ptr<Session>, std::less<>> sessions;
  std::unordered_map<ui::MapDocument*, std::unique_ptr<DocumentState>> documentStates;
  std::unique_ptr<CallRunner> callRunner;

  ServerActivity activity;

private:
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  NotifierConnection m_hostConnection;

public:
  ServerState(
    McpServer& server,
    McpHost& host,
    Scheduler& scheduler,
    ServerInfo info,
    ServerOptions options);
  ~ServerState();

  Session* findSession(std::string_view id);

  /** Returns the state of the given document, creating it if necessary. */
  DocumentState& documentState(ui::MapDocument& document);

  /** Finds an open document by its handle. */
  std::optional<DocumentInfo> findDocument(std::string_view documentId) const;

  /**
   * The document that tools of the given session target by default: the one chosen with
   * document_activate if it is still open, otherwise the focused document, otherwise the
   * first one.
   */
  std::optional<DocumentInfo> defaultDocument(const Session& session) const;

  /** The transaction depth that the server itself opened on the given document. */
  size_t agentDepth(ui::MapDocument& document) const;

  /**
   * Rolls back the agent transaction of the given document. If the human has a nested
   * transaction open (e.g. a drag), the rollback is retried later.
   */
  void rollbackAgentTransaction(ui::MapDocument& document);

  /** Rolls back all agent transactions owned by the given session. */
  void rollbackAgentTransactions(const std::string& sessionId);

  /** Closes the given session: drops its queued calls and rolls back its transaction. */
  void closeSession(const std::string& sessionId);

  void setActivity(ServerActivity::State state, std::string toolTitle = {});
  void updateOpenTransactions();

  /** Sends a notification to all initialized sessions. */
  void broadcast(const Json& notification);
  void notifyResourceUpdated(const std::string& uri);

private:
  void documentWillClose(ui::MapDocument& document);
  void documentsDidChange();
  void documentInfoDidChange(ui::MapDocument& document);
};

} // namespace tb::mcp
