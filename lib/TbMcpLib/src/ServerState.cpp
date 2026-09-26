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

#include "mcp/ServerState.h"

#include "mcp/CallRunner.h"
#include "mcp/JsonRpc.h"
#include "mcp/Scheduler.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"
#include "ui/MapDocument.h"

#include <algorithm>

namespace tb::mcp
{

DocumentState::DocumentState(
  ui::MapDocument& document_, std::function<void()> infoDidChange_)
  : document{document_}
  , ids{document_}
  , m_infoDidChange{std::move(infoDidChange_)}
  , m_lastModified{document_.map().modified()}
{
  // reloading replaces the map and with it the command processor, so an open agent
  // transaction is gone
  m_notifierConnection += document.documentWasLoadedNotifier.connect([&]() {
    transaction.reset();
    infoDidChange();
  });

  m_notifierConnection +=
    document.documentWasSavedNotifier.connect(this, &DocumentState::infoDidChange);
  m_notifierConnection += document.modificationStateDidChangeNotifier.connect([&]() {
    // only a change of the modified flag changes the info
    if (document.map().modified() != m_lastModified)
    {
      infoDidChange();
    }
  });
  m_notifierConnection +=
    document.modsDidChangeNotifier.connect(this, &DocumentState::infoDidChange);
  m_notifierConnection += document.entityDefinitionsDidChangeNotifier.connect(
    this, &DocumentState::infoDidChange);
  m_notifierConnection += document.materialCollectionsDidChangeNotifier.connect(
    this, &DocumentState::infoDidChange);
  m_notifierConnection +=
    document.nodesDidChangeNotifier.connect([&](const std::vector<mdl::Node*>& nodes) {
      // worldspawn holds the soft bounds, WAD list and other document settings
      if (std::ranges::find(nodes, &document.map().worldNode()) != nodes.end())
      {
        infoDidChange();
      }
    });
}

void DocumentState::infoDidChange()
{
  m_lastModified = document.map().modified();
  if (m_infoDidChange)
  {
    m_infoDidChange();
  }
}

ServerState::ServerState(
  McpServer& server_,
  McpHost& host_,
  Scheduler& scheduler_,
  ServerInfo info_,
  ServerOptions options_)
  : server{server_}
  , host{host_}
  , scheduler{scheduler_}
  , info{std::move(info_)}
  , options{std::move(options_)}
  , callRunner{std::make_unique<CallRunner>(*this)}
{
  m_hostConnection +=
    host.documentWillCloseNotifier.connect(this, &ServerState::documentWillClose);
  m_hostConnection +=
    host.documentsDidChangeNotifier.connect(this, &ServerState::documentsDidChange);
}

ServerState::~ServerState()
{
  *m_alive = false;
  callRunner->cancelAll();
  for (auto& [document, state] : documentStates)
  {
    if (
      state->transaction
      && document->map().transactionDepth() == state->transaction->depth)
    {
      document->map().cancelTransaction();
    }
  }
}

Session* ServerState::findSession(const std::string_view id)
{
  const auto it = sessions.find(id);
  return it != sessions.end() ? it->second.get() : nullptr;
}

DocumentState& ServerState::documentState(ui::MapDocument& document)
{
  auto it = documentStates.find(&document);
  if (it == documentStates.end())
  {
    it = documentStates
           .emplace(
             &document,
             std::make_unique<DocumentState>(
               document, [this, &document]() { documentInfoDidChange(document); }))
           .first;
  }
  return *it->second;
}

std::optional<DocumentInfo> ServerState::findDocument(
  const std::string_view documentId) const
{
  for (auto& info_ : host.documents())
  {
    if (info_.id == documentId)
    {
      return info_;
    }
  }
  return std::nullopt;
}

std::optional<DocumentInfo> ServerState::defaultDocument(const Session& session) const
{
  const auto documents = host.documents();
  if (session.activeDocumentId)
  {
    const auto it = std::ranges::find_if(documents, [&](const auto& info_) {
      return info_.id == *session.activeDocumentId;
    });
    if (it != documents.end())
    {
      return *it;
    }
  }

  const auto focused =
    std::ranges::find_if(documents, [](const auto& info_) { return info_.focused; });
  if (focused != documents.end())
  {
    return *focused;
  }

  return !documents.empty() ? std::optional{documents.front()} : std::nullopt;
}

size_t ServerState::agentDepth(ui::MapDocument& document) const
{
  const auto it = documentStates.find(&document);
  return it != documentStates.end() && it->second->transaction
           ? it->second->transaction->depth
           : 0;
}

void ServerState::rollbackAgentTransaction(ui::MapDocument& document)
{
  const auto it = documentStates.find(&document);
  if (it == documentStates.end() || !it->second->transaction)
  {
    return;
  }

  auto& state = *it->second;
  auto& map = document.map();
  const auto depth = state.transaction->depth;
  if (map.transactionDepth() == depth)
  {
    map.cancelTransaction();
    state.transaction.reset();
  }
  else if (map.transactionDepth() < depth)
  {
    // the transaction is already gone
    state.transaction.reset();
  }
  else
  {
    // the human has a transaction open inside ours (e.g. a drag); try again later, but
    // make sure that no other session can use the transaction meanwhile
    state.transaction->sessionId.clear();
    scheduler.postDelayed(
      options.busyPollInterval, [this, documentPtr = &document, alive = m_alive]() {
        if (*alive && documentStates.contains(documentPtr))
        {
          rollbackAgentTransaction(*documentPtr);
        }
      });
  }
  updateOpenTransactions();
}

void ServerState::rollbackAgentTransactions(const std::string& sessionId)
{
  auto documents = std::vector<ui::MapDocument*>{};
  for (const auto& [document, state] : documentStates)
  {
    if (state->transaction && state->transaction->sessionId == sessionId)
    {
      documents.push_back(document);
    }
  }
  for (auto* document : documents)
  {
    rollbackAgentTransaction(*document);
  }
}

void ServerState::closeSession(const std::string& sessionId)
{
  callRunner->cancelAll(sessionId);
  rollbackAgentTransactions(sessionId);
  if (const auto it = sessions.find(sessionId); it != sessions.end())
  {
    sessions.erase(it);
    server.sessionsDidChangeNotifier();
  }
}

void ServerState::setActivity(const ServerActivity::State state, std::string toolTitle)
{
  auto newActivity = activity;
  newActivity.state = state;
  newActivity.toolTitle = std::move(toolTitle);
  if (newActivity != activity)
  {
    activity = std::move(newActivity);
    server.activityDidChangeNotifier(activity);
  }
}

void ServerState::updateOpenTransactions()
{
  auto names = std::vector<std::string>{};
  for (const auto& [document, state] : documentStates)
  {
    if (state->transaction)
    {
      names.push_back(state->transaction->name);
    }
  }
  std::ranges::sort(names);

  if (names != activity.openTransactions)
  {
    activity.openTransactions = std::move(names);
    server.activityDidChangeNotifier(activity);
  }
}

void ServerState::broadcast(const Json& notification)
{
  for (const auto& [id, session] : sessions)
  {
    session->send(notification);
  }
}

void ServerState::notifyResourceUpdated(const std::string& uri)
{
  const auto notification =
    jsonrpc::makeNotification("notifications/resources/updated", Json{{"uri", uri}});
  for (const auto& [id, session] : sessions)
  {
    if (session->subscriptions.contains(uri))
    {
      session->send(notification);
    }
  }
}

void ServerState::documentWillClose(ui::MapDocument& document)
{
  const auto it = documentStates.find(&document);
  if (it == documentStates.end())
  {
    return;
  }

  auto& state = *it->second;
  if (state.transaction && document.map().transactionDepth() == state.transaction->depth)
  {
    document.map().cancelTransaction();
  }
  documentStates.erase(it);
  updateOpenTransactions();
}

void ServerState::documentsDidChange()
{
  // track every open document so that info subscriptions work before any tool used it
  for (const auto& document : host.documents())
  {
    documentState(*document.document);
  }
  notifyResourceUpdated("trenchbroom://editor/status");
}

void ServerState::documentInfoDidChange(ui::MapDocument& document)
{
  for (const auto& info_ : host.documents())
  {
    if (info_.document == &document)
    {
      notifyResourceUpdated("trenchbroom://documents/" + info_.id + "/info");
    }
  }
}

} // namespace tb::mcp
