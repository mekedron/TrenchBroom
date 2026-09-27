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

#include "mcp/CallRunner.h"

#include "base/Logger.h"
#include "base/NotifierConnection.h"
#include "mcp/Args.h"
#include "mcp/ChangeCollector.h"
#include "mcp/ConsoleBuffer.h"
#include "mcp/ListDetail.h"
#include "mcp/LogCapture.h"
#include "mcp/ObjectIds.h"
#include "mcp/ProtocolVersion.h"
#include "mcp/Scheduler.h"
#include "mcp/Schema.h"
#include "mcp/ServerState.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/TransactionScope.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"
#include "kd/string_utils.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <unordered_map>

namespace tb::mcp
{
namespace
{

/** The most console messages a result lists. */
constexpr size_t MaxConsoleMessages = 50;

Json errorContent(const ToolError& error, Json console = nullptr)
{
  auto result = Json{{"ok", false}, {"error", toJson(error)}};
  if (!console.is_null())
  {
    result["console"] = std::move(console);
  }
  return result;
}

/** Adds the console report to a result object, if there is one. */
void addConsole(Json& structured, Json console)
{
  if (!console.is_null() && structured.is_object())
  {
    structured["console"] = std::move(console);
  }
}

std::string protocolVersionOf(ServerState& server, const std::string& sessionId)
{
  const auto* session = server.findSession(sessionId);
  return session ? session->protocolVersion : std::string{ProtocolVersion::Latest};
}

/**
 * The net changes of the calls in an open agent transaction (transaction_begin), which
 * transaction_commit reports: the commit itself changes no objects.
 */
struct TransactionChanges
{
  std::string sessionId;
  std::string name;
  size_t depth = 0;
  ChangeReport report;
};

/**
 * The accumulated changes per document state. An entry whose document has no agent
 * transaction any more (rolled back when a session closed) or another one is replaced on
 * the next call; the state of a closed document leaves a small stale entry.
 */
std::unordered_map<const DocumentState*, TransactionChanges>& transactionChanges()
{
  static auto changes = std::unordered_map<const DocumentState*, TransactionChanges>{};
  return changes;
}

bool sameTransaction(
  const TransactionChanges& changes, const AgentTransaction& transaction)
{
  return changes.sessionId == transaction.sessionId && changes.name == transaction.name
         && changes.depth == transaction.depth;
}

void eraseId(std::vector<std::string>& ids, const std::string& id)
{
  std::erase(ids, id);
}

void addId(std::vector<std::string>& ids, const std::string& id)
{
  if (std::ranges::find(ids, id) == ids.end())
  {
    ids.push_back(id);
  }
}

/** Adds the changes of a later call to the net changes of the earlier calls. */
void mergeChanges(ChangeReport& total, const ChangeReport& call)
{
  const auto contains = [](const auto& ids, const auto& id) {
    return std::ranges::find(ids, id) != ids.end();
  };
  for (const auto& id : call.created)
  {
    if (contains(total.removed, id))
    {
      // removed earlier and restored under the same id
      eraseId(total.removed, id);
      addId(total.modified, id);
    }
    else
    {
      addId(total.created, id);
    }
  }
  for (const auto& id : call.modified)
  {
    if (!contains(total.created, id))
    {
      addId(total.modified, id);
    }
  }
  for (const auto& id : call.removed)
  {
    if (contains(total.created, id))
    {
      eraseId(total.created, id);
    }
    else
    {
      eraseId(total.modified, id);
      addId(total.removed, id);
    }
  }
  total.selectionChanged = total.selectionChanged || call.selectionChanged;
  total.contextChanged = total.contextChanged || call.contextChanged;
}

/**
 * Records the changes of a call in the open agent transaction of its document. When the
 * call committed the transaction (transaction_commit), the report becomes the net changes
 * of the whole transaction.
 */
void accumulateTransactionChanges(
  const ToolDef& tool, const DocumentState& state, ChangeReport& report)
{
  auto& allChanges = transactionChanges();
  auto it = allChanges.find(&state);
  if (state.transaction)
  {
    if (
      it == allChanges.end() || tool.name() == "transaction_begin"
      || !sameTransaction(it->second, *state.transaction))
    {
      it = allChanges
             .insert_or_assign(
               &state,
               TransactionChanges{
                 state.transaction->sessionId,
                 state.transaction->name,
                 state.transaction->depth,
                 {}})
             .first;
    }
    mergeChanges(it->second.report, report);
    return;
  }

  if (it == allChanges.end())
  {
    return;
  }
  if (tool.name() == "transaction_commit")
  {
    auto total = std::move(it->second.report);
    mergeChanges(total, report);
    report.created = std::move(total.created);
    report.modified = std::move(total.modified);
    report.removed = std::move(total.removed);
    report.selectionChanged = total.selectionChanged;
    report.contextChanged = total.contextChanged;
  }
  allChanges.erase(it);
}

enum class ChecksMode
{
  Report,
  Defer,
};

/**
 * Whether a call reports its checks or defers them: the `checks` argument, else defer
 * inside a transaction of the session begun with checks: "defer" (except for the calls
 * that close it).
 */
ChecksMode checksMode(
  const ToolDef& tool,
  const Json& arguments,
  const DocumentState& state,
  const std::string& sessionId)
{
  if (const auto* checks = findMember(arguments, "checks"); checks && checks->is_string())
  {
    return checks->get<std::string>() == "defer" ? ChecksMode::Defer : ChecksMode::Report;
  }
  const auto closesTransaction =
    tool.name() == "transaction_commit" || tool.name() == "transaction_rollback";
  return state.transaction && state.transaction->sessionId == sessionId
             && state.transaction->deferChecks && !closesTransaction
           ? ChecksMode::Defer
           : ChecksMode::Report;
}

/** The document a call acts on. */
struct ResolvedDocument
{
  std::optional<DocumentInfo> document;
  /**
   * The call has no `document` argument and the session no active document, so the call
   * acts on the focused document, which becomes the session's active document.
   */
  bool fromFocus = false;
  /**
   * The session's active document was closed and the tool does not need a document: the
   * call runs without one and is warned.
   */
  std::optional<ToolError> activeClosed = std::nullopt;
};

std::string describeOpenDocuments(ServerState& server)
{
  auto descriptions = std::vector<std::string>{};
  for (const auto& document : server.host.documents())
  {
    descriptions.push_back(document.id + " (" + document.windowTitle + ")");
  }
  return descriptions.empty() ? std::string{"none"} : kdl::str_join(descriptions, ", ");
}

ToolError activeDocumentClosedError(ServerState& server, const Session& session)
{
  auto openDocuments = Json::array();
  for (const auto& document : server.host.documents())
  {
    openDocuments.push_back(Json{{"id", document.id}, {"title", document.windowTitle}});
  }

  auto error = makeError(
    ErrorCode::ActiveDocumentClosed,
    "This session's active document " + *session.activeDocumentId
      + " was closed. Open documents: " + describeOpenDocuments(server) + ".",
    "Call document_activate {\"document\": ...} to choose the document to work on "
    "(ask the user if unsure which one is yours), or open or create one with "
    "document_open / document_new.");
  error.details = Json{
    {"closedDocument", *session.activeDocumentId},
    {"openDocuments", std::move(openDocuments)},
  };
  return error;
}

/**
 * Replaces the name addresses (`group:@Bar`, `layer:@Details`, `entity:@door1`) in the
 * object id arguments of the schema by the ids of the named objects, so that names
 * resolve when the call runs and handlers see ids only.
 */
std::optional<ToolError> resolveNameAddresses(
  const schema::Schema& schema, Json& value, const IdRegistry& ids)
{
  using schema::Type;
  switch (schema.type)
  {
  case Type::String:
    if (schema.format == "object-id" && value.is_string())
    {
      if (const auto address = parseNameAddress(value.get<std::string>()))
      {
        auto node = ids.resolve(*address);
        if (node.is_error())
        {
          return errorOf(node);
        }
        value = ids.format(*node.value());
      }
    }
    break;
  case Type::Array:
    if (schema.items && value.is_array())
    {
      for (auto& item : value)
      {
        if (auto error = resolveNameAddresses(*schema.items, item, ids))
        {
          return error;
        }
      }
    }
    break;
  case Type::Object:
    if (value.is_object())
    {
      for (const auto& field : schema.fields)
      {
        if (auto it = value.find(field.name); it != value.end())
        {
          if (auto error = resolveNameAddresses(field.schema, *it, ids))
          {
            return error;
          }
        }
      }
    }
    break;
  case Type::OneOf:
    for (const auto& alternative : schema.alternatives)
    {
      const auto matches = (alternative.type == Type::String && value.is_string())
                           || (alternative.type == Type::Array && value.is_array())
                           || (alternative.type == Type::Object && value.is_object());
      if (matches)
      {
        return resolveNameAddresses(alternative, value, ids);
      }
    }
    break;
  case Type::Any:
  case Type::Boolean:
  case Type::Integer:
  case Type::Number:
    break;
  }
  return std::nullopt;
}

Result<ResolvedDocument, ToolError> resolveDocument(
  ServerState& server, const ToolDef& tool, const Json& arguments, const Session& session)
{
  if (tool.documentUse() == DocumentUse::None)
  {
    return ResolvedDocument{};
  }

  if (const auto* documentId = findMember(arguments, "document");
      documentId && documentId->is_string())
  {
    if (auto document = server.findDocument(documentId->get<std::string>()))
    {
      return ResolvedDocument{std::move(*document)};
    }
    return makeError(
      ErrorCode::DocumentNotFound,
      "Document " + documentId->get<std::string>() + " is not open.",
      "Use document_list to see the open documents.");
  }

  auto target = server.targetDocument(session);
  switch (target.source)
  {
  case DocumentTarget::Source::Active:
    return ResolvedDocument{std::move(target.document)};
  case DocumentTarget::Source::ActiveClosed:
    if (tool.documentUse() == DocumentUse::Optional)
    {
      return ResolvedDocument{
        std::nullopt, false, activeDocumentClosedError(server, session)};
    }
    return activeDocumentClosedError(server, session);
  case DocumentTarget::Source::Focused:
    if (!tool.focusFallback())
    {
      return makeError(
        ErrorCode::NoDocument,
        tool.name()
          + " only acts on the document named by 'document' or on this session's "
            "active document, and this session has no active document. Open documents: "
          + describeOpenDocuments(server) + ".",
        "Pass 'document' explicitly, e.g. {\"document\": \"" + target.document->id
          + "\"}.");
    }
    return ResolvedDocument{std::move(target.document), true};
  case DocumentTarget::Source::None:
    break;
  }

  if (tool.documentUse() == DocumentUse::Required)
  {
    return makeError(
      ErrorCode::NoDocument,
      "No document with a window is open in TrenchBroom and this session has no "
      "active document.",
      "Open or create a map first, or pass 'document' or call document_activate for a "
      "background document (document_list lists them).");
  }
  return ResolvedDocument{};
}

/**
 * A call that fell back to the focused document makes it the session's active document,
 * so that the session keeps working on it when the focus changes, and warns about it. A
 * call that runs without a document because the active document was closed is warned,
 * too.
 */
void applyDocumentChoice(
  ServerState& server,
  Session& session,
  CallContext& context,
  const ResolvedDocument& resolved)
{
  if (resolved.activeClosed)
  {
    context.warn(
      "ACTIVE_DOCUMENT_CLOSED",
      resolved.activeClosed->message
        + " The call ran without a document; call document_activate to choose one.");
  }
  if (!resolved.fromFocus)
  {
    return;
  }

  const auto& document = *resolved.document;
  server.setActiveDocument(session, document.id);
  context.warn(
    "DOCUMENT_FROM_FOCUS",
    "This session had no active document, so the call acts on " + document.id + " ("
      + document.windowTitle
      + "), the focused window; it is now this session's active document. If this is "
        "not the map you are working on, call document_activate or pass 'document'.");
}

bool isBusy(ServerState& server, ui::MapDocument& document)
{
  return server.host.busyState(document) == BusyState::Busy
         || document.map().commandProcessor().transactionDepth()
              > server.agentDepth(document);
}

} // namespace

Json makeCallToolResult(
  const Json& structured,
  const bool isError,
  const std::string_view protocolVersion,
  const std::vector<Json>& additionalContent)
{
  auto text = std::string{};
  if (isError)
  {
    const auto* error = findMember(structured, "error");
    const auto* code = error ? findMember(*error, "code") : nullptr;
    const auto* message = error ? findMember(*error, "message") : nullptr;
    const auto* hint = error ? findMember(*error, "hint") : nullptr;
    if (code && message)
    {
      text = code->get<std::string>() + ": " + message->get<std::string>();
      if (hint)
      {
        text += " Hint: " + hint->get<std::string>();
      }
      text += "\n" + dumpJson(structured);
    }
  }
  if (text.empty())
  {
    text = dumpJson(structured);
  }

  auto content = Json::array({Json{{"type", "text"}, {"text", text}}});
  for (const auto& block : additionalContent)
  {
    content.push_back(block);
  }

  auto result = Json{
    {"content", std::move(content)},
  };
  if (supportsStructuredContent(protocolVersion))
  {
    result["structuredContent"] = structured;
  }
  result["isError"] = isError;
  return result;
}

struct CallRunner::AsyncCall
{
  CallRequest request;
  std::optional<Args> args;
  std::chrono::steady_clock::time_point startTime;
  CallLogEntry logEntry;
  bool dryRun = false;
  ui::MapDocument* document = nullptr;
  bool documentClosed = false;
  /** Read-only calls run outside the queue. */
  bool readOnly = false;
  /** The last console message before the call started. */
  uint64_t consoleSeq = 0;
  std::unique_ptr<CallContext> context;
  std::unique_ptr<ScopedLogCapture> logCapture;
  NotifierConnection connection;
  /** Cleared when the call completes; pending steps and completions are then ignored. */
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  ~AsyncCall() { *alive = false; }
};

CallRunner::CallRunner(ServerState& server)
  : m_server{server}
  , m_alive{std::make_shared<bool>(true)}
{
}

CallRunner::~CallRunner()
{
  *m_alive = false;
}

void CallRunner::submit(CallRequest request)
{
  const auto* tool = m_server.tools.find(request.toolName);
  auto* session = m_server.findSession(request.sessionId);
  if (!tool || !session)
  {
    finish(
      request,
      makeCallToolResult(
        errorContent(makeError(
          ErrorCode::InternalError, "Unknown tool or session: " + request.toolName)),
        true,
        protocolVersionOf(m_server, request.sessionId)));
    return;
  }

  // validate before queueing so that invalid calls fail immediately
  auto errors = std::vector<schema::SchemaError>{};
  const auto arguments =
    request.arguments.is_null() ? Json::object() : std::move(request.arguments);
  auto validated = tool->inputSchema().validate(arguments, errors);
  if (!validated)
  {
    auto errorDetails = Json::array();
    for (const auto& error : errors)
    {
      errorDetails.push_back(Json{{"path", error.path}, {"message", error.message}});
    }
    auto error = makeError(
      ErrorCode::InvalidArgument,
      "Invalid arguments for " + tool->name() + ": " + schema::formatErrors(errors),
      "Fix the listed arguments and call again; the tool's inputSchema lists all "
      "parameters.");
    error.details["errors"] = std::move(errorDetails);

    auto entry = CallLogEntry{};
    entry.sessionId = session->id;
    entry.clientName = session->clientDisplayName();
    entry.tool = tool->name();
    entry.arguments = arguments;
    entry.ok = false;
    entry.errorCode = toString(error.code);
    m_server.callLog.add(std::move(entry));

    finish(
      request, makeCallToolResult(errorContent(error), true, session->protocolVersion));
    return;
  }
  request.arguments = std::move(*validated);

  if (!tool->isModifying())
  {
    if (tool->isAsync())
    {
      startAsync(std::move(request), true);
      return;
    }

    auto result = execute(request);
    finish(request, std::move(result));
    if (!m_queue.empty() && !m_running)
    {
      m_server.setActivity(
        ServerActivity::State::WaitingForUser,
        m_server.tools.find(m_queue.front().request.toolName)->title());
    }
    return;
  }

  m_queue.push_back(PendingCall{std::move(request), m_server.scheduler.now()});
  pump();
}

bool CallRunner::cancel(const std::string& sessionId, const Json& requestId)
{
  const auto it = std::ranges::find_if(m_queue, [&](const auto& call) {
    return call.request.sessionId == sessionId && call.request.requestId == requestId;
  });
  if (it == m_queue.end())
  {
    if (auto* call = findAsyncCall(sessionId, requestId))
    {
      // the call stops at its next step
      call->context->cancel();
      return true;
    }
    return false;
  }

  auto request = std::move(it->request);
  m_queue.erase(it);
  finish(
    request,
    makeCallToolResult(
      errorContent(makeError(ErrorCode::Cancelled, "The call was cancelled.")),
      true,
      protocolVersionOf(m_server, sessionId)));
  pump();
  return true;
}

void CallRunner::cancelAll(const std::optional<std::string>& sessionId)
{
  auto cancelled = std::vector<CallRequest>{};
  std::erase_if(m_queue, [&](auto& call) {
    if (!sessionId || call.request.sessionId == *sessionId)
    {
      cancelled.push_back(std::move(call.request));
      return true;
    }
    return false;
  });

  for (const auto& request : cancelled)
  {
    finish(
      request,
      makeCallToolResult(
        errorContent(makeError(
          ErrorCode::Cancelled, "The call was cancelled because the agent was stopped.")),
        true,
        protocolVersionOf(m_server, request.sessionId)));
  }

  auto abandoned = std::vector<AsyncCall*>{};
  for (auto& call : m_readOnlyCalls)
  {
    if (!sessionId || call->request.sessionId == *sessionId)
    {
      abandoned.push_back(call.get());
    }
  }
  if (m_asyncCall && (!sessionId || m_asyncCall->request.sessionId == *sessionId))
  {
    abandoned.push_back(m_asyncCall.get());
  }
  for (auto* call : abandoned)
  {
    completeAsync(
      *call,
      makeError(
        ErrorCode::Cancelled, "The call was cancelled because the agent was stopped."));
  }

  if (m_queue.empty() && !m_running)
  {
    m_server.setActivity(ServerActivity::State::Idle);
  }
}

size_t CallRunner::queueSize() const
{
  return m_queue.size();
}

void CallRunner::pump()
{
  if (m_running)
  {
    return;
  }

  while (!m_queue.empty())
  {
    auto& head = m_queue.front();
    const auto* tool = m_server.tools.find(head.request.toolName);
    auto* session = m_server.findSession(head.request.sessionId);

    if (tool && session)
    {
      const auto document =
        resolveDocument(m_server, *tool, head.request.arguments, *session);
      if (
        document.is_success() && document.value().document
        && isBusy(m_server, *document.value().document->document))
      {
        if (m_server.scheduler.now() - head.enqueued >= m_server.options.busyWaitTimeout)
        {
          auto request = std::move(head.request);
          m_queue.pop_front();
          auto error = makeError(
            ErrorCode::BusyTimeout,
            "The user kept interacting with the editor (dragging, a dialog or a tool "
            "gesture) and the call timed out waiting.",
            "Try again in a moment, or ask the user to finish their current "
            "interaction.");
          finish(
            request,
            makeCallToolResult(errorContent(error), true, session->protocolVersion));
          continue;
        }

        m_server.setActivity(ServerActivity::State::WaitingForUser, tool->title());
        schedulePoll();
        return;
      }
    }

    auto request = std::move(head.request);
    m_queue.pop_front();

    m_running = true;
    if (tool && tool->isAsync())
    {
      if (startAsync(std::move(request), false))
      {
        // the queue resumes when the call completes
        return;
      }
      continue;
    }

    auto result = execute(request);
    m_running = false;

    finish(request, std::move(result));
  }

  m_server.setActivity(ServerActivity::State::Idle);
}

void CallRunner::schedulePoll()
{
  if (m_pollScheduled)
  {
    return;
  }

  m_pollScheduled = true;
  m_server.scheduler.postDelayed(
    m_server.options.busyPollInterval, [this, alive = m_alive]() {
      if (*alive)
      {
        m_pollScheduled = false;
        pump();
      }
    });
}

Json CallRunner::execute(const CallRequest& request)
{
  const auto startTime = std::chrono::steady_clock::now();
  const auto startSeq = consoleSeq();
  const auto* tool = m_server.tools.find(request.toolName);
  auto* session = m_server.findSession(request.sessionId);
  if (!tool || !session)
  {
    return makeCallToolResult(
      errorContent(makeError(ErrorCode::Cancelled, "The session was closed.")),
      true,
      protocolVersionOf(m_server, request.sessionId));
  }

  const auto dryRun = tool->isModifying() && findMember(request.arguments, "dryRun")
                      && request.arguments["dryRun"].get<bool>();
  const auto* detailArgument =
    tool->mutation() == Mutation::Map ? findMember(request.arguments, "detail") : nullptr;
  const auto detail =
    detailArgument && detailArgument->is_string()
      ? listDetailFromString(detailArgument->get<std::string>()).value_or(ListDetail::Ids)
      : ListDetail::Ids;
  const auto limit = listLimit(detail);

  auto logEntry = CallLogEntry{};
  logEntry.sessionId = session->id;
  logEntry.clientName = session->clientDisplayName();
  logEntry.tool = tool->name();
  logEntry.arguments = request.arguments;
  logEntry.dryRun = dryRun;

  const auto finishLog = [&](const bool ok, const std::string_view errorCode) {
    logEntry.ok = ok;
    logEntry.errorCode = errorCode;
    logEntry.durationMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - startTime)
                            .count();
    m_server.callLog.add(logEntry);
  };

  const auto failWith = [&](const ToolError& error) {
    finishLog(false, toString(error.code));
    return makeCallToolResult(
      errorContent(error, consoleReport(startSeq)), true, session->protocolVersion);
  };

  auto document = resolveDocument(m_server, *tool, request.arguments, *session);
  if (document.is_error())
  {
    return failWith(errorOf(document));
  }
  const auto resolved = std::move(document.value());
  const auto& documentInfo = resolved.document;

  auto* mapDocument = documentInfo ? documentInfo->document : nullptr;
  auto* documentState = mapDocument ? &m_server.documentState(*mapDocument) : nullptr;

  // an agent transaction belongs to one session
  if (
    tool->mutation() == Mutation::Map && documentState && documentState->transaction
    && documentState->transaction->sessionId != session->id)
  {
    return failWith(makeError(
      ErrorCode::TransactionActive,
      "Another client (" + documentState->transaction->clientName
        + ") has the transaction '" + documentState->transaction->name + "' open on "
        + documentInfo->id + ".",
      "Wait until that client commits or rolls back its transaction."));
  }

  auto context =
    CallContext{m_server, *session, *tool, documentInfo, dryRun, request.progress};
  applyDocumentChoice(m_server, *session, context, resolved);

  // name addresses resolve now, when the call runs
  auto arguments = request.arguments;
  if (documentState)
  {
    if (
      auto error =
        resolveNameAddresses(tool->inputSchema(), arguments, documentState->ids))
    {
      return failWith(*error);
    }
  }

  if (tool->mutation() == Mutation::Map && mapDocument && !tool->keepsActiveTool())
  {
    for (auto& note : m_server.host.prepareForAgentEdit(*mapDocument))
    {
      context.warn("EDITOR_STATE_CHANGED", std::move(note));
    }
  }

  auto logCapture = std::optional<ScopedLogCapture>{};
  auto collector = std::optional<ChangeCollector>{};
  if (mapDocument)
  {
    logCapture.emplace(*mapDocument, m_server.host.logTarget(*mapDocument));
    context.setLogCapture(&*logCapture);
  }
  const auto placementCountAtStart =
    documentState ? documentState->placement.changeCount : size_t(0);
  const auto placementOptions = [&](const bool dryRunCall) {
    return PlacementTrackerOptions{
      &documentState->placement,
      m_server.host.knowledgeDirectory(),
      dryRunCall,
      documentState->disabledValidators};
  };
  // Checks deferred by this call or reported for the session's deferred series (a dry
  // run neither extends nor ends a series)
  auto checks = ChecksMode::Report;
  auto* series = static_cast<DeferredChecks*>(nullptr);
  if (tool->mutation() == Mutation::Map && mapDocument)
  {
    checks = checksMode(*tool, request.arguments, *documentState, session->id);
    if (!dryRun)
    {
      if (const auto it = documentState->deferredChecks.find(session->id);
          it != documentState->deferredChecks.end())
      {
        series = &it->second;
      }
      else if (checks == ChecksMode::Defer)
      {
        series = &documentState->deferredChecks[session->id];
        series->collector = std::make_unique<ChangeCollector>(
          *mapDocument, documentState->ids, placementOptions(false));
      }
    }

    // the calls of a series are checked by the series' collector
    const auto ownChecks = checks == ChecksMode::Report && !series;
    collector.emplace(
      *mapDocument,
      documentState->ids,
      ownChecks ? std::optional{placementOptions(dryRun)} : std::nullopt,
      ownChecks);
  }

  m_server.setActivity(ServerActivity::State::Running, tool->title());

  const auto transactional = tool->transactional() && mapDocument;
  const auto undoStepName = "AI: " + tool->title();
  const auto depthBefore =
    mapDocument ? mapDocument->map().commandProcessor().transactionDepth() : 0;
  if (transactional)
  {
    mapDocument->map().startTransaction(undoStepName, mdl::TransactionScope::Oneshot);
  }

  auto result = [&]() -> ToolResult {
    try
    {
      return tool->handler()(context, Args{arguments});
    }
    catch (const std::exception& e)
    {
      return makeError(
        ErrorCode::InternalError,
        std::string{"The tool failed with an internal error: "} + e.what(),
        "This is a bug in TrenchBroom; the map was left unchanged.");
    }
    catch (...)
    {
      return makeError(
        ErrorCode::InternalError,
        "The tool failed with an unknown internal error.",
        "This is a bug in TrenchBroom; the map was left unchanged.");
    }
  }();

  auto report = std::optional<ChangeReport>{};
  auto selection = Json(nullptr);
  auto undoStep = std::optional<std::string>{};

  if (transactional)
  {
    auto& map = mapDocument->map();

    // close any transaction the handler leaked
    while (map.commandProcessor().transactionDepth() > depthBefore + 1)
    {
      map.cancelTransaction();
    }

    if (result.is_error())
    {
      map.cancelTransaction();
      documentState->placement.rolledBack(placementCountAtStart);
    }
    else if (dryRun)
    {
      // the report must be computed while the changes still exist
      report = collector->finish();
      selection = selectionSummary(map, documentState->ids, limit);
      map.cancelTransaction();
      documentState->placement.rolledBack(placementCountAtStart);
    }
    else
    {
      auto stored = false;
      auto connection = NotifierConnection{};
      connection += mapDocument->transactionDoneNotifier.connect(
        [&](const std::string& name, bool, bool) {
          if (name == undoStepName)
          {
            stored = true;
          }
        });

      // Consecutive calls that change the same objects must not be collated into one
      // undo step (spec X2)
      const auto collationEnabled = map.isCommandCollationEnabled();
      map.setIsCommandCollationEnabled(false);
      const auto committed = map.commitTransaction();
      map.setIsCommandCollationEnabled(collationEnabled);

      if (!committed)
      {
        documentState->placement.rolledBack(placementCountAtStart);
        result = context.operationFailed(
          "The change could not be applied to all linked groups, so it was rolled back.",
          "Check for conflicts between linked groups, e.g. objects that would move "
          "outside the world bounds in a linked copy.");
      }
      else if (stored && depthBefore == 0)
      {
        // Inside an agent transaction (or another enclosing transaction) the call's
        // changes become part of that transaction's undo step, not a step of their own.
        undoStep = undoStepName;
      }
    }
  }

  if (result.is_error())
  {
    if (series && series->calls == 0)
    {
      // the series has not started
      documentState->deferredChecks.erase(session->id);
    }
    m_server.setActivity(ServerActivity::State::Idle);
    return failWith(errorOf(result));
  }

  if (collector && !report)
  {
    report = collector->finish();
  }
  auto checksJson = Json(nullptr);
  if (series)
  {
    ++series->calls;
    if (checks == ChecksMode::Defer)
    {
      checksJson = Json{{"deferred", true}, {"calls", series->calls}};
    }
    else
    {
      // this call ends the series and reports the issues of all its calls
      auto seriesReport = series->collector->finish();
      checksJson = Json{
        {"deferredCalls", series->calls - 1},
        {"changes",
         Json{
           {"created", seriesReport.created.size()},
           {"modified", seriesReport.modified.size()},
           {"removed", seriesReport.removed.size()},
         }},
      };
      report->issuesIntroduced = std::move(seriesReport.issuesIntroduced);
      std::ranges::move(seriesReport.warnings, std::back_inserter(report->warnings));
      documentState->deferredChecks.erase(session->id);
      series = nullptr;
    }
  }
  if (report && !dryRun)
  {
    accumulateTransactionChanges(*tool, *documentState, *report);
  }
  if (report)
  {
    // report each problem once: as the tool's warning if it already warned about it
    removeIssuesWarnedAbout(report->issuesIntroduced, context.warnings());
    for (auto& warning : report->warnings)
    {
      context.warn(
        std::move(warning.code),
        std::move(warning.message),
        std::move(warning.objectIds));
    }
    logEntry.created = report->created.size();
    logEntry.modified = report->modified.size();
    logEntry.removed = report->removed.size();
  }
  if (!undoStep && context.undoStep())
  {
    undoStep = context.undoStep();
  }
  logEntry.undoStep = undoStep;

  auto structured = Json{};
  if (tool->isModifying())
  {
    // long id lists are cut to the detail level; the full lists are kept for
    // result_list_get
    auto truncated = std::vector<TruncatedList>{};
    auto toolResult = std::move(result.value());
    if (tool->mutation() == Mutation::Map)
    {
      truncateIdLists(toolResult, "result", limit, truncated);
    }
    structured = Json{
      {"ok", true},
      {"dryRun", dryRun},
      {"undoStep", undoStep ? Json(*undoStep) : Json(nullptr)},
      {"result", std::move(toolResult)},
    };
    if (report)
    {
      auto changes = changesToJson(*report, detail, &truncated);
      if (dryRun && !report->created.empty())
      {
        changes["ephemeral"] = true;
      }
      structured["changes"] = std::move(changes);
    }
    if (mapDocument && tool->mutation() == Mutation::Map)
    {
      structured["selection"] =
        !selection.is_null()
          ? std::move(selection)
          : selectionSummary(mapDocument->map(), documentState->ids, limit);
      const auto& issues =
        report ? report->issuesIntroduced : std::vector<IntroducedIssue>{};
      structured["issuesIntroduced"] = issuesToJson(issues, limit);
      if (issues.size() > limit)
      {
        truncated.push_back(TruncatedList{
          "issuesIntroduced", issuesToJson(issues, std::numeric_limits<size_t>::max())});
      }
      if (issues.size() > limit || (detail == ListDetail::Summary && !issues.empty()))
      {
        structured["issuesSummary"] = issuesSummaryJson(issues);
      }
      if (!checksJson.is_null())
      {
        structured["checks"] = std::move(checksJson);
      }
    }
    if (!truncated.empty())
    {
      auto kept = KeptLists{};
      kept.tool = tool->name();
      for (const auto& list : truncated)
      {
        kept.lists[list.path] = list.items;
      }
      const auto listsId = session->keepLists(std::move(kept));
      structured["truncatedLists"] = truncatedListsJson(truncated, listsId, limit);
    }
    auto warnings = Json::array();
    for (const auto& warning : context.warnings())
    {
      warnings.push_back(toJson(warning));
    }
    structured["warnings"] = std::move(warnings);
    if (mapDocument)
    {
      structured["grid"] = mapDocument->map().grid().actualSize();
    }
  }
  else
  {
    structured = std::move(result.value());
    if (!context.warnings().empty() && structured.is_object())
    {
      auto warnings = Json::array();
      for (const auto& warning : context.warnings())
      {
        warnings.push_back(toJson(warning));
      }
      structured["warnings"] = std::move(warnings);
    }
  }

  addConsole(structured, consoleReport(startSeq));

  finishLog(true, {});
  m_server.setActivity(ServerActivity::State::Idle);
  return makeCallToolResult(
    structured, false, session->protocolVersion, context.content());
}

bool CallRunner::startAsync(CallRequest request, const bool readOnly)
{
  const auto* tool = m_server.tools.find(request.toolName);
  auto* session = m_server.findSession(request.sessionId);
  if (!tool || !session)
  {
    if (!readOnly)
    {
      m_running = false;
    }
    finish(
      request,
      makeCallToolResult(
        errorContent(makeError(ErrorCode::Cancelled, "The session was closed.")),
        true,
        protocolVersionOf(m_server, request.sessionId)));
    return false;
  }

  auto call = std::make_unique<AsyncCall>();
  call->request = std::move(request);
  call->readOnly = readOnly;
  call->consoleSeq = consoleSeq();
  call->args.emplace(call->request.arguments);
  call->startTime = std::chrono::steady_clock::now();
  call->dryRun = !readOnly && findMember(call->request.arguments, "dryRun")
                 && call->request.arguments["dryRun"].get<bool>();
  call->logEntry.sessionId = session->id;
  call->logEntry.clientName = session->clientDisplayName();
  call->logEntry.tool = tool->name();
  call->logEntry.arguments = call->request.arguments;
  call->logEntry.dryRun = call->dryRun;

  auto document = resolveDocument(m_server, *tool, call->request.arguments, *session);
  if (document.is_error())
  {
    const auto error = errorOf(document);
    call->logEntry.ok = false;
    call->logEntry.errorCode = toString(error.code);
    m_server.callLog.add(call->logEntry);
    if (!readOnly)
    {
      m_running = false;
    }
    finish(
      call->request,
      makeCallToolResult(errorContent(error), true, session->protocolVersion));
    return false;
  }

  const auto& resolved = document.value();
  const auto& documentInfo = resolved.document;
  call->document = documentInfo ? documentInfo->document : nullptr;

  // name addresses resolve now, when the call starts
  if (call->document)
  {
    auto arguments = call->request.arguments;
    if (
      auto error = resolveNameAddresses(
        tool->inputSchema(), arguments, m_server.documentState(*call->document).ids))
    {
      call->logEntry.ok = false;
      call->logEntry.errorCode = toString(error->code);
      m_server.callLog.add(call->logEntry);
      if (!readOnly)
      {
        m_running = false;
      }
      finish(
        call->request,
        makeCallToolResult(errorContent(*error), true, session->protocolVersion));
      return false;
    }
    call->args.emplace(std::move(arguments));
  }

  call->context = std::make_unique<CallContext>(
    m_server, *session, *tool, documentInfo, call->dryRun, call->request.progress);
  applyDocumentChoice(m_server, *session, *call->context, resolved);

  if (call->document)
  {
    // Read-only calls may run while other calls run; a log capture spanning their steps
    // would interfere with the captures of those calls. Their console messages are
    // reported through the console buffer.
    if (!readOnly)
    {
      call->logCapture = std::make_unique<ScopedLogCapture>(
        *call->document, m_server.host.logTarget(*call->document));
      call->context->setLogCapture(call->logCapture.get());
    }

    // the document may be closed between two steps
    call->connection += m_server.host.documentWillCloseNotifier.connect(
      [callPtr = call.get()](ui::MapDocument& closing) {
        if (&closing == callPtr->document)
        {
          callPtr->logCapture.reset();
          callPtr->context->setLogCapture(nullptr);
          callPtr->documentClosed = true;
        }
      });
  }

  auto* callPtr = call.get();
  call->context->setDeferrer(
    [this, callPtr, alive = call->alive](
      std::function<void()> step, const std::chrono::milliseconds delay) {
      auto run = [this, callPtr, alive, step = std::move(step)]() {
        if (!*alive)
        {
          return;
        }
        if (callPtr->documentClosed)
        {
          completeAsync(
            *callPtr,
            makeError(
              ErrorCode::DocumentNotFound,
              "The document was closed while the call was running.",
              "Use document_list to see the open documents."));
          return;
        }

        try
        {
          step();
        }
        catch (const std::exception& e)
        {
          if (*alive)
          {
            completeAsync(
              *callPtr,
              makeError(
                ErrorCode::InternalError,
                std::string{"The tool failed with an internal error: "} + e.what()));
          }
        }
      };

      if (delay.count() > 0)
      {
        m_server.scheduler.postDelayed(delay, std::move(run));
      }
      else
      {
        m_server.scheduler.post(std::move(run));
      }
    });

  if (!readOnly || (!m_running && m_queue.empty()))
  {
    m_server.setActivity(ServerActivity::State::Running, tool->title());
  }

  const auto alive = call->alive;
  if (readOnly)
  {
    m_readOnlyCalls.push_back(std::move(call));
  }
  else
  {
    m_asyncCall = std::move(call);
    m_startingAsync = true;
  }

  try
  {
    tool->asyncHandler()(
      *callPtr->context, *callPtr->args, [this, callPtr, alive](ToolResult result) {
        if (*alive)
        {
          completeAsync(*callPtr, std::move(result));
        }
      });
  }
  catch (const std::exception& e)
  {
    if (*alive)
    {
      completeAsync(
        *callPtr,
        makeError(
          ErrorCode::InternalError,
          std::string{"The tool failed with an internal error: "} + e.what()));
    }
  }
  if (!readOnly)
  {
    m_startingAsync = false;
  }

  return *alive;
}

void CallRunner::completeAsync(AsyncCall& call, ToolResult result)
{
  // take ownership of the call
  auto owned = std::unique_ptr<AsyncCall>{};
  if (m_asyncCall.get() == &call)
  {
    owned = std::move(m_asyncCall);
  }
  else
  {
    const auto it = std::ranges::find_if(m_readOnlyCalls, [&](const auto& readOnlyCall) {
      return readOnlyCall.get() == &call;
    });
    contract_assert(it != m_readOnlyCalls.end());
    owned = std::move(*it);
    m_readOnlyCalls.erase(it);
  }

  *owned->alive = false;
  owned->logCapture.reset();
  owned->connection.disconnect();

  const auto* session = m_server.findSession(owned->request.sessionId);
  const auto protocolVersion =
    session ? session->protocolVersion : std::string{ProtocolVersion::Latest};

  auto& logEntry = owned->logEntry;
  logEntry.durationMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - owned->startTime)
                          .count();

  auto callToolResult = Json{};
  if (result.is_error())
  {
    const auto error = errorOf(result);
    logEntry.ok = false;
    logEntry.errorCode = toString(error.code);
    callToolResult = makeCallToolResult(
      errorContent(error, consoleReport(owned->consoleSeq)), true, protocolVersion);
  }
  else
  {
    auto warnings = Json::array();
    for (const auto& warning : owned->context->warnings())
    {
      warnings.push_back(toJson(warning));
    }

    auto structured = Json{};
    if (owned->readOnly)
    {
      structured = std::move(result.value());
      if (!warnings.empty() && structured.is_object())
      {
        structured["warnings"] = std::move(warnings);
      }
    }
    else
    {
      structured = Json{
        {"ok", true},
        {"dryRun", owned->dryRun},
        {"undoStep", nullptr},
        {"result", std::move(result.value())},
        {"warnings", std::move(warnings)},
      };
      if (owned->document && !owned->documentClosed)
      {
        structured["grid"] = owned->document->map().grid().actualSize();
      }
    }
    addConsole(structured, consoleReport(owned->consoleSeq));
    callToolResult =
      makeCallToolResult(structured, false, protocolVersion, owned->context->content());
  }
  m_server.callLog.add(logEntry);

  finish(owned->request, std::move(callToolResult));
  const auto readOnly = owned->readOnly;
  owned.reset();

  if (readOnly)
  {
    if (!m_running && m_queue.empty() && m_readOnlyCalls.empty())
    {
      m_server.setActivity(ServerActivity::State::Idle);
    }
    return;
  }

  m_running = false;
  m_server.setActivity(ServerActivity::State::Idle);
  if (!m_startingAsync)
  {
    pump();
  }
}

CallRunner::AsyncCall* CallRunner::findAsyncCall(
  const std::string& sessionId, const Json& requestId)
{
  const auto matches = [&](const AsyncCall& call) {
    return call.request.sessionId == sessionId && call.request.requestId == requestId;
  };
  if (m_asyncCall && matches(*m_asyncCall))
  {
    return m_asyncCall.get();
  }
  const auto it = std::ranges::find_if(
    m_readOnlyCalls, [&](const auto& call) { return matches(*call); });
  return it != m_readOnlyCalls.end() ? it->get() : nullptr;
}

uint64_t CallRunner::consoleSeq() const
{
  const auto* console = m_server.host.consoleBuffer();
  return console ? console->lastSeq() : 0;
}

Json CallRunner::consoleReport(const uint64_t seq) const
{
  const auto* console = m_server.host.consoleBuffer();
  if (!console)
  {
    return nullptr;
  }

  auto documentIds = std::unordered_map<const ui::MapDocument*, std::string>{};
  for (const auto& document : m_server.host.documents())
  {
    documentIds[document.document] = document.id;
  }

  auto messages = Json::array();
  auto omitted = size_t{0};
  for (const auto& message : console->messagesAfter(seq))
  {
    if (message.level != LogLevel::Warn && message.level != LogLevel::Error)
    {
      continue;
    }
    if (messages.size() == MaxConsoleMessages)
    {
      ++omitted;
      continue;
    }

    auto json = Json{
      {"seq", message.seq},
      {"level", toString(message.level)},
      {"text", message.text},
    };
    if (const auto it = documentIds.find(message.document); it != documentIds.end())
    {
      json["document"] = it->second;
    }
    else if (!message.documentName.empty())
    {
      json["documentName"] = message.documentName;
    }
    messages.push_back(std::move(json));
  }

  if (messages.empty())
  {
    return nullptr;
  }
  if (omitted > 0)
  {
    messages.push_back(Json{
      {"level", "info"},
      {"text", std::to_string(omitted) + " more messages; use console_read"},
    });
  }
  return messages;
}

void CallRunner::finish(const CallRequest& request, Json result)
{
  if (request.completion)
  {
    request.completion(std::move(result));
  }
}

} // namespace tb::mcp
