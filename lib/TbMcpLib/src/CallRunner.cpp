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
#include "mcp/ProtocolVersion.h"
#include "mcp/Scheduler.h"
#include "mcp/ServerState.h"
#include "mdl/Grid.h"
#include "mdl/Map.h"
#include "mdl/TransactionScope.h"
#include "ui/MapDocument.h"

#include <algorithm>
#include <exception>

namespace tb::mcp
{
namespace
{

/**
 * Forwards everything to the original target logger and records warnings and errors, so
 * that failures of Map_* functions can be explained to the agent.
 */
class CapturingLogger : public Logger
{
private:
  Logger* m_target;
  std::vector<std::string> m_messages;

public:
  explicit CapturingLogger(Logger* target)
    : m_target{target}
  {
  }

  const std::vector<std::string>& messages() const { return m_messages; }

  void clear() { m_messages.clear(); }

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    if (level == LogLevel::Warn || level == LogLevel::Error)
    {
      m_messages.emplace_back(message);
    }
    if (m_target)
    {
      m_target->log(level, message);
    }
  }
};

class ScopedLogCapture
{
private:
  ui::MapDocument& m_document;
  Logger* m_previousTarget;
  CapturingLogger m_logger;

public:
  explicit ScopedLogCapture(ui::MapDocument& document)
    : m_document{document}
    , m_previousTarget{document.targetLogger()}
    , m_logger{m_previousTarget}
  {
    m_document.setTargetLogger(&m_logger);
    // setting a target logger flushes cached messages that predate this call
    m_logger.clear();
  }

  ~ScopedLogCapture() { m_document.setTargetLogger(m_previousTarget); }

  const std::vector<std::string>& messages() const { return m_logger.messages(); }
};

Json errorContent(const ToolError& error)
{
  return Json{{"ok", false}, {"error", toJson(error)}};
}

std::string protocolVersionOf(ServerState& server, const std::string& sessionId)
{
  const auto* session = server.findSession(sessionId);
  return session ? session->protocolVersion : std::string{ProtocolVersion::Latest};
}

Result<std::optional<DocumentInfo>, ToolError> resolveDocument(
  ServerState& server, const ToolDef& tool, const Json& arguments, const Session& session)
{
  if (tool.documentUse() == DocumentUse::None)
  {
    return std::optional<DocumentInfo>{};
  }

  if (const auto* documentId = findMember(arguments, "document");
      documentId && documentId->is_string())
  {
    if (auto document = server.findDocument(documentId->get<std::string>()))
    {
      return std::optional{std::move(*document)};
    }
    return makeError(
      ErrorCode::DocumentNotFound,
      "Document " + documentId->get<std::string>() + " is not open.",
      "Use document_list to see the open documents.");
  }

  if (auto document = server.defaultDocument(session))
  {
    return std::optional{std::move(*document)};
  }

  if (tool.documentUse() == DocumentUse::Required)
  {
    return makeError(
      ErrorCode::NoDocument,
      "No document is open in TrenchBroom.",
      "Open or create a map first.");
  }
  return std::optional<DocumentInfo>{};
}

bool isBusy(ServerState& server, ui::MapDocument& document)
{
  return server.host.busyState(document) == BusyState::Busy
         || document.map().transactionDepth() > server.agentDepth(document);
}

} // namespace

Json makeCallToolResult(
  const Json& structured, const bool isError, const std::string_view protocolVersion)
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

  auto result = Json{
    {"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
  };
  if (supportsStructuredContent(protocolVersion))
  {
    result["structuredContent"] = structured;
  }
  result["isError"] = isError;
  return result;
}

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
        document.is_success() && document.value()
        && isBusy(m_server, *document.value()->document))
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
    return makeCallToolResult(errorContent(error), true, session->protocolVersion);
  };

  auto document = resolveDocument(m_server, *tool, request.arguments, *session);
  if (document.is_error())
  {
    return failWith(errorOf(document));
  }
  auto documentInfo = std::move(document.value());

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

  if (tool->mutation() == Mutation::Map && mapDocument)
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
    logCapture.emplace(*mapDocument);
    context.setCapturedMessages(&logCapture->messages());
  }
  if (tool->mutation() == Mutation::Map && mapDocument)
  {
    collector.emplace(*mapDocument, documentState->ids);
  }

  m_server.setActivity(ServerActivity::State::Running, tool->title());

  const auto transactional = tool->transactional() && mapDocument;
  const auto undoStepName = "AI: " + tool->title();
  const auto depthBefore = mapDocument ? mapDocument->map().transactionDepth() : 0;
  if (transactional)
  {
    mapDocument->map().startTransaction(undoStepName, mdl::TransactionScope::Oneshot);
  }

  auto result = [&]() -> ToolResult {
    try
    {
      return tool->handler()(context, Args{request.arguments});
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
    while (map.transactionDepth() > depthBefore + 1)
    {
      map.cancelTransaction();
    }

    if (result.is_error())
    {
      map.cancelTransaction();
    }
    else if (dryRun)
    {
      // the report must be computed while the changes still exist
      report = collector->finish();
      selection = selectionSummary(map, documentState->ids);
      map.cancelTransaction();
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

      if (!map.commitTransaction())
      {
        result = context.operationFailed(
          "The change could not be applied to all linked groups, so it was rolled back.",
          "Check for conflicts between linked groups, e.g. objects that would move "
          "outside the world bounds in a linked copy.");
      }
      else if (stored)
      {
        undoStep = undoStepName;
      }
    }
  }

  if (result.is_error())
  {
    m_server.setActivity(ServerActivity::State::Idle);
    return failWith(errorOf(result));
  }

  if (collector && !report)
  {
    report = collector->finish();
  }
  if (report)
  {
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
    structured = Json{
      {"ok", true},
      {"dryRun", dryRun},
      {"undoStep", undoStep ? Json(*undoStep) : Json(nullptr)},
      {"result", std::move(result.value())},
    };
    if (report)
    {
      auto changes = changesToJson(*report);
      if (dryRun && !report->created.empty())
      {
        changes["ephemeral"] = true;
      }
      structured["changes"] = std::move(changes);
    }
    if (mapDocument && tool->mutation() == Mutation::Map)
    {
      structured["selection"] =
        !selection.is_null() ? std::move(selection)
                             : selectionSummary(mapDocument->map(), documentState->ids);
      structured["issuesIntroduced"] =
        report ? issuesToJson(report->issuesIntroduced) : Json::array();
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

  finishLog(true, {});
  m_server.setActivity(ServerActivity::State::Idle);
  return makeCallToolResult(structured, false, session->protocolVersion);
}

void CallRunner::finish(const CallRequest& request, Json result)
{
  if (request.completion)
  {
    request.completion(std::move(result));
  }
}

} // namespace tb::mcp
