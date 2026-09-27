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

#include "base/PreferenceManager.h"
#include "gl/MaterialManager.h"
#include "gl/ResourceId.h"
#include "mcp/CallRunner.h"
#include "mcp/ConsoleBuffer.h"
#include "mcp/JsonRpc.h"
#include "mcp/Scheduler.h"
#include "mcp/tools/ConsoleTools.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <chrono>

namespace tb::mcp
{
namespace
{
constexpr auto EditorStatusUri = "trenchbroom://editor/status";

/**
 * Console messages that arrive within this time after the first one are reported with a
 * single notification, also when they are logged by other threads.
 */
constexpr auto ConsoleUpdateDelay = std::chrono::milliseconds{250};

/**
 * The prefix of the call log lines that McpServerController writes to the console
 * (formatCallLogEntryForConsole). They do not notify console subscribers, because a
 * client that reacts to console notifications with a call would cause another one.
 */
constexpr auto CallLogConsolePrefix = std::string_view{"[AI] "};
} // namespace

DocumentState::DocumentState(ui::MapDocument& document_, DidChange didChange_)
  : document{document_}
  , ids{document_}
  , manifest{document_.map().path()}
  , m_didChange{std::move(didChange_)}
  , m_lastModified{document_.map().modified()}
{
  // reloading replaces the map and with it the command processor, so an open agent
  // transaction is gone
  m_notifierConnection += document.documentWasLoadedNotifier.connect([&]() {
    transaction.reset();
    placement = PlacementCache{placement.changeCount + 1};
    manifest.mapWasLoaded(document.map().path());
    infoDidChange();
    didChange(DocumentAspect::Summary);
    didChange(DocumentAspect::Selection);
    didChange(DocumentAspect::EntityDefinitions);
    didChange(DocumentAspect::Materials);
  });

  m_notifierConnection += document.documentWasSavedNotifier.connect([&]() {
    // the user's saves and document_save(_as): write or carry the manifest
    manifest.mapWasSaved(document.map().path());
    infoDidChange();
  });
  m_notifierConnection += document.modificationStateDidChangeNotifier.connect([&]() {
    // only a change of the modified flag changes the info
    if (document.map().modified() != m_lastModified)
    {
      infoDidChange();
    }
  });
  m_notifierConnection +=
    document.modsDidChangeNotifier.connect(this, &DocumentState::infoDidChange);
  m_notifierConnection += document.entityDefinitionsDidChangeNotifier.connect([&]() {
    infoDidChange();
    // issues depend on the entity definitions
    didChange(DocumentAspect::Summary);
    didChange(DocumentAspect::EntityDefinitions);
  });
  m_notifierConnection += document.materialCollectionsDidChangeNotifier.connect([&]() {
    infoDidChange();
    didChange(DocumentAspect::Materials);
  });
  // the materials resource lists the image sizes, which are known once loaded
  m_notifierConnection += document.resourcesWereProcessedNotifier.connect(
    [&](const std::vector<gl::ResourceId>& resourceIds) {
      if (!document.map()
             .materialManager()
             .findMaterialsByTextureResourceId(resourceIds)
             .empty())
      {
        didChange(DocumentAspect::Materials);
      }
    });
  m_notifierConnection +=
    document.nodesDidChangeNotifier.connect(this, &DocumentState::nodesDidChange);

  const auto summaryDidChange = [&]() { didChange(DocumentAspect::Summary); };
  m_notifierConnection += document.nodesWereAddedNotifier.connect([=, this](const auto&) {
    placement.nodesAddedOrRemoved();
    summaryDidChange();
  });
  m_notifierConnection +=
    document.nodesWereRemovedNotifier.connect([=, this](const auto&) {
      placement.nodesAddedOrRemoved();
      summaryDidChange();
    });
  m_notifierConnection += document.nodeVisibilityDidChangeNotifier.connect(
    [=](const auto&) { summaryDidChange(); });
  m_notifierConnection += document.nodeLockingDidChangeNotifier.connect(
    [=](const auto&) { summaryDidChange(); });
  m_notifierConnection +=
    document.currentLayerDidChangeNotifier.connect(summaryDidChange);
  m_notifierConnection += document.gridDidChangeNotifier.connect([=, this]() {
    summaryDidChange();
    didChange(DocumentAspect::Status);
  });

  m_notifierConnection += document.selectionDidChangeNotifier.connect(
    [&](const auto&) { didChange(DocumentAspect::Selection); });
}

void DocumentState::didChange(const DocumentAspect aspect)
{
  if (m_didChange)
  {
    m_didChange(aspect);
  }
}

void DocumentState::infoDidChange()
{
  m_lastModified = document.map().modified();
  didChange(DocumentAspect::Info);
}

void DocumentState::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  placement.nodesChanged(nodes);

  // worldspawn holds the soft bounds, WAD list and other document settings
  if (std::ranges::find(nodes, &document.map().worldNode()) != nodes.end())
  {
    infoDidChange();
  }

  didChange(DocumentAspect::Summary);

  // the selection summary contains the bounds of the selected objects
  if (std::ranges::any_of(nodes, [](const auto* node) {
        return node->selected() || node->descendantSelected();
      }))
  {
    didChange(DocumentAspect::Selection);
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
  , compileRuns{std::make_unique<CompileRuns>(
      host_, [this](const CompileRun& run, const bool ended) {
        scheduleResourceUpdate(CompileRuns::logUri(run.id));
        if (ended)
        {
          // the editor status reports running compilations
          scheduleResourceUpdate(EditorStatusUri);
        }
      })}
{
  m_hostConnection +=
    host.documentWillCloseNotifier.connect(this, &ServerState::documentWillClose);
  m_hostConnection +=
    host.documentsDidChangeNotifier.connect(this, &ServerState::documentsDidChange);
  m_hostConnection += host.currentToolDidChangeNotifier.connect(
    [&](ui::MapDocument&) { scheduleResourceUpdate(EditorStatusUri); });
  m_hostConnection += PreferenceManager::instance().preferenceDidChangeNotifier.connect(
    [&](const std::filesystem::path& path) {
      // the editor status reports the locks
      if (path == Preferences::AlignmentLock.path || path == Preferences::UvLock.path)
      {
        scheduleResourceUpdate(EditorStatusUri);
      }
    });
  if (auto* console = host.consoleBuffer())
  {
    m_hostConnection +=
      console->messagesAddedNotifier.connect([this]() { consoleDidChange(false); });
    m_hostConnection +=
      console->clearedNotifier.connect([this]() { consoleDidChange(true); });
  }
}

ServerState::~ServerState()
{
  // the compile jobs refer to the documents
  compileRuns.reset();
  *m_alive = false;
  callRunner->cancelAll();
  for (auto& [document, state] : documentStates)
  {
    if (
      state->transaction
      && document->map().commandProcessor().transactionDepth()
           == state->transaction->depth)
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
               document,
               [this, &document](const DocumentAspect aspect) {
                 documentAspectDidChange(document, aspect);
               }))
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
  if (map.commandProcessor().transactionDepth() == depth)
  {
    map.cancelTransaction();
    state.transaction.reset();
  }
  else if (map.commandProcessor().transactionDepth() < depth)
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
    // the editor status reports the open transaction
    scheduleResourceUpdate(EditorStatusUri);
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
  if (
    state.transaction
    && document.map().commandProcessor().transactionDepth() == state.transaction->depth)
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
  notifyResourceUpdated(EditorStatusUri);
}

void ServerState::documentAspectDidChange(
  ui::MapDocument& document, const DocumentAspect aspect)
{
  if (aspect == DocumentAspect::Info)
  {
    for (const auto& info_ : host.documents())
    {
      if (info_.document == &document)
      {
        notifyResourceUpdated(documentResourceUri(info_.id, DocumentAspect::Info));
      }
    }
  }
  else if (aspect != DocumentAspect::Status)
  {
    scheduleDocumentUpdate(document, aspect);
  }

  if (
    aspect != DocumentAspect::Summary && aspect != DocumentAspect::EntityDefinitions
    && aspect != DocumentAspect::Materials)
  {
    // the editor status lists the modified flag and the selection of the documents
    scheduleResourceUpdate(EditorStatusUri);
  }
}

void ServerState::consoleDidChange(const bool cleared)
{
  if (m_consoleUpdateScheduled || !hasSubscriptions())
  {
    return;
  }

  if (!cleared)
  {
    const auto& messages = host.consoleBuffer()->messages();
    if (!messages.empty() && messages.back().text.starts_with(CallLogConsolePrefix))
    {
      return;
    }
  }

  m_consoleUpdateScheduled = true;
  scheduler.postDelayed(ConsoleUpdateDelay, [this, alive = m_alive]() {
    if (*alive)
    {
      m_consoleUpdateScheduled = false;
      scheduleResourceUpdate(ConsoleResourceUri);
    }
  });
}

std::string ServerState::documentResourceUri(
  const std::string& documentId, const DocumentAspect aspect)
{
  const auto suffix = [&]() {
    switch (aspect)
    {
    case DocumentAspect::Info:
      return "info";
    case DocumentAspect::Summary:
      return "summary";
    case DocumentAspect::Selection:
      return "selection";
    case DocumentAspect::EntityDefinitions:
      return "entity-definitions";
    case DocumentAspect::Materials:
      return "materials";
    case DocumentAspect::Status:
      break;
    }
    return "info";
  }();
  return "trenchbroom://documents/" + documentId + "/" + suffix;
}

bool ServerState::isCompileRunning(ui::MapDocument& document) const
{
  return (compileRuns && compileRuns->running(document))
         || host.isCompileRunning(document);
}

bool ServerState::hasSubscriptions() const
{
  return std::ranges::any_of(
    sessions, [](const auto& entry) { return !entry.second->subscriptions.empty(); });
}

void ServerState::scheduleResourceUpdate(std::string uri)
{
  if (hasSubscriptions())
  {
    m_pendingUpdates.insert(std::move(uri));
    scheduleFlush();
  }
}

void ServerState::scheduleDocumentUpdate(
  ui::MapDocument& document, const DocumentAspect aspect)
{
  // this is called for every change of the map, so keep it cheap when nobody listens
  if (hasSubscriptions())
  {
    m_pendingDocumentUpdates.emplace(&document, aspect);
    scheduleFlush();
  }
}

void ServerState::scheduleFlush()
{
  if (!m_flushScheduled)
  {
    m_flushScheduled = true;
    scheduler.post([this, alive = m_alive]() {
      if (*alive)
      {
        flushResourceUpdates();
      }
    });
  }
}

void ServerState::flushResourceUpdates()
{
  m_flushScheduled = false;

  auto uris = std::move(m_pendingUpdates);
  m_pendingUpdates.clear();

  const auto documentUpdates = std::move(m_pendingDocumentUpdates);
  m_pendingDocumentUpdates.clear();
  if (!documentUpdates.empty())
  {
    for (const auto& info_ : host.documents())
    {
      for (const auto& [document, aspect] : documentUpdates)
      {
        if (document == info_.document)
        {
          uris.insert(documentResourceUri(info_.id, aspect));
        }
      }
    }
  }

  for (const auto& uri : uris)
  {
    notifyResourceUpdated(uri);
  }
}

} // namespace tb::mcp
