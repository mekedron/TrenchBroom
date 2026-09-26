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

#include "mcp/McpServer.h"

#include "base/Uuid.h"
#include "mcp/CallRunner.h"
#include "mcp/JsonRpc.h"
#include "mcp/ProtocolVersion.h"
#include "mcp/ServerState.h"

#include <algorithm>

namespace tb::mcp
{
namespace
{

using namespace jsonrpc;

/**
 * Collects the responses to the requests of one POST and completes the request stream
 * once all of them are available.
 */
class PendingResponses
{
private:
  std::weak_ptr<RequestStream> m_stream;
  bool m_isBatch;
  std::vector<std::optional<Json>> m_responses;
  size_t m_received = 0;

public:
  PendingResponses(
    std::weak_ptr<RequestStream> stream, const bool isBatch, const size_t count)
    : m_stream{std::move(stream)}
    , m_isBatch{isBatch}
    , m_responses(count)
  {
  }

  void set(const size_t index, Json response)
  {
    if (m_responses[index])
    {
      return;
    }
    m_responses[index] = std::move(response);
    ++m_received;

    if (m_received == m_responses.size())
    {
      if (auto stream = m_stream.lock())
      {
        if (m_isBatch)
        {
          auto array = Json::array();
          for (auto& response_ : m_responses)
          {
            array.push_back(std::move(*response_));
          }
          stream->complete(array);
        }
        else
        {
          stream->complete(*m_responses.front());
        }
      }
    }
  }
};

std::optional<std::string> stringParam(const Json& params, const std::string_view key)
{
  const auto* value = findMember(params, key);
  return value && value->is_string() ? std::optional{value->get<std::string>()}
                                     : std::nullopt;
}

std::string newSessionId()
{
  auto id = generateUuid();
  std::erase(id, '-');
  return id;
}

} // namespace

McpServer::McpServer(
  McpHost& host, Scheduler& scheduler, ServerInfo info, ServerOptions options)
  : m_state{std::make_unique<ServerState>(
      *this, host, scheduler, std::move(info), std::move(options))}
{
}

McpServer::~McpServer() = default;

ToolRegistry& McpServer::tools()
{
  return m_state->tools;
}

ResourceRegistry& McpServer::resources()
{
  return m_state->resources;
}

PromptRegistry& McpServer::prompts()
{
  return m_state->prompts;
}

CallLog& McpServer::callLog()
{
  return m_state->callLog;
}

const ServerOptions& McpServer::options() const
{
  return m_state->options;
}

void McpServer::setOptions(ServerOptions options)
{
  m_state->options = std::move(options);
}

std::vector<SessionInfo> McpServer::sessions() const
{
  auto result = std::vector<SessionInfo>{};
  for (const auto& [id, session] : m_state->sessions)
  {
    result.push_back(SessionInfo{
      session->id,
      session->clientName,
      session->clientVersion,
      session->protocolVersion});
  }
  return result;
}

size_t McpServer::sessionCount() const
{
  return m_state->sessions.size();
}

const ServerActivity& McpServer::activity() const
{
  return m_state->activity;
}

void McpServer::stopAgents()
{
  m_state->callRunner->cancelAll();

  auto sessionIds = std::vector<std::string>{};
  for (const auto& [id, session] : m_state->sessions)
  {
    sessionIds.push_back(id);
  }
  for (const auto& id : sessionIds)
  {
    m_state->closeSession(id);
  }

  // also roll back transactions whose session is already gone
  auto documents = std::vector<ui::MapDocument*>{};
  for (const auto& [document, state] : m_state->documentStates)
  {
    if (state->transaction)
    {
      documents.push_back(document);
    }
  }
  for (auto* document : documents)
  {
    m_state->rollbackAgentTransaction(*document);
  }

  m_state->setActivity(ServerActivity::State::Idle);
}

void McpServer::notifyResourceUpdated(const std::string& uri)
{
  m_state->notifyResourceUpdated(uri);
}

void McpServer::broadcast(const Json& notification)
{
  m_state->broadcast(notification);
}

ServerState& McpServer::state()
{
  return *m_state;
}

PostResult McpServer::post(
  const std::optional<std::string>& sessionId,
  const std::string_view body,
  std::shared_ptr<RequestStream> stream)
{
  auto parsed = parsePayload(body);
  if (const auto* error = std::get_if<ErrorResponse>(&parsed))
  {
    return {PostStatus::BadRequest, {}, makeError(nullptr, error->code, error->message)};
  }
  auto& payload = std::get<Payload>(parsed);

  if (!sessionId)
  {
    const auto* message = !payload.isBatch && payload.messages.size() == 1
                            ? std::get_if<Message>(&payload.messages.front())
                            : nullptr;
    const auto* request = message ? std::get_if<Request>(message) : nullptr;
    if (!request || request->method != "initialize")
    {
      return {
        PostStatus::BadRequest,
        {},
        makeError(
          request ? request->id : Json(nullptr),
          ErrorCode::InvalidRequest,
          "Missing Mcp-Session-Id header. Send an initialize request first.")};
    }

    const auto protocolVersion = stringParam(request->params, "protocolVersion");
    if (!protocolVersion)
    {
      return {
        PostStatus::BadRequest,
        {},
        makeError(
          request->id,
          ErrorCode::InvalidParams,
          "initialize requires the parameter protocolVersion")};
    }

    auto session = std::make_unique<Session>();
    session->id = newSessionId();
    session->protocolVersion = negotiateProtocolVersion(*protocolVersion);
    if (const auto* clientInfo = findMember(request->params, "clientInfo"))
    {
      session->clientName = stringParam(*clientInfo, "name").value_or("");
      session->clientVersion = stringParam(*clientInfo, "version").value_or("");
    }
    if (const auto* capabilities = findMember(request->params, "capabilities"))
    {
      session->clientCapabilities = *capabilities;
    }

    auto serverInfo = Json{{"name", m_state->info.name}};
    if (supportsTitles(session->protocolVersion))
    {
      serverInfo["title"] = m_state->info.title;
    }
    serverInfo["version"] = m_state->info.version;

    const auto result = Json{
      {"protocolVersion", session->protocolVersion},
      {"capabilities",
       Json{
         {"tools", Json{{"listChanged", true}}},
         {"resources", Json{{"subscribe", true}, {"listChanged", true}}},
         {"prompts", Json{{"listChanged", false}}},
         {"logging", Json::object()},
       }},
      {"serverInfo", std::move(serverInfo)},
      {"instructions", m_state->info.instructions},
    };

    const auto id = session->id;
    m_state->sessions.emplace(id, std::move(session));
    sessionsDidChangeNotifier();

    stream->complete(makeResponse(request->id, result));
    return {PostStatus::Pending, id};
  }

  auto* session = m_state->findSession(*sessionId);
  if (!session)
  {
    return {PostStatus::SessionNotFound};
  }

  if (payload.isBatch && !supportsBatches(session->protocolVersion))
  {
    return {
      PostStatus::BadRequest,
      {},
      makeError(
        nullptr,
        ErrorCode::InvalidRequest,
        "JSON-RPC batches are not supported in protocol version "
          + session->protocolVersion)};
  }

  auto requestCount = size_t{0};
  for (const auto& message : payload.messages)
  {
    if (
      std::holds_alternative<InvalidMessage>(message)
      || std::holds_alternative<Request>(std::get<Message>(message)))
    {
      ++requestCount;
    }
  }

  auto pending =
    std::make_shared<PendingResponses>(stream, payload.isBatch, requestCount);
  auto index = size_t{0};
  const auto sessionIdCopy = session->id;
  for (auto& message : payload.messages)
  {
    // a previous message may have closed the session
    session = m_state->findSession(sessionIdCopy);

    if (const auto* invalid = std::get_if<InvalidMessage>(&message))
    {
      pending->set(index++, makeError(invalid->id, invalid->code, invalid->message));
      continue;
    }

    auto& msg = std::get<Message>(message);
    if (auto* request = std::get_if<Request>(&msg))
    {
      const auto responseIndex = index++;
      if (!session)
      {
        pending->set(
          responseIndex,
          makeError(request->id, ErrorCode::InvalidRequest, "The session was closed."));
        continue;
      }
      dispatch(
        *session, std::move(*request), stream, [pending, responseIndex](Json response) {
          pending->set(responseIndex, std::move(response));
        });
    }
    else if (const auto* notification = std::get_if<Notification>(&msg))
    {
      if (session)
      {
        handleNotification(*session, *notification);
      }
    }
    // responses from the client are ignored because the server sends no requests
  }

  return {requestCount > 0 ? PostStatus::Pending : PostStatus::Accepted};
}

std::optional<std::string> McpServer::sessionProtocolVersion(
  const std::string_view sessionId) const
{
  const auto it = m_state->sessions.find(sessionId);
  return it != m_state->sessions.end() ? std::optional{it->second->protocolVersion}
                                       : std::nullopt;
}

bool McpServer::openNotificationStream(
  const std::string_view sessionId, std::shared_ptr<NotificationStream> stream)
{
  auto* session = m_state->findSession(sessionId);
  if (!session)
  {
    return false;
  }
  session->notificationStream = stream;
  return true;
}

bool McpServer::deleteSession(const std::string_view sessionId)
{
  if (!m_state->findSession(sessionId))
  {
    return false;
  }
  m_state->closeSession(std::string{sessionId});
  return true;
}

void McpServer::dispatch(
  Session& session,
  jsonrpc::Request request,
  const std::weak_ptr<RequestStream>& stream,
  std::function<void(Json)> respond)
{
  const auto& method = request.method;
  const auto& params = request.params.is_object() ? request.params : Json::object();
  const auto& id = request.id;

  const auto ok = [&](Json result) { respond(makeResponse(id, std::move(result))); };
  const auto fail = [&](const int code, std::string message, Json data = nullptr) {
    respond(makeError(id, code, std::move(message), std::move(data)));
  };

  if (method == "ping")
  {
    return ok(Json::object());
  }

  if (method == "initialize")
  {
    return fail(ErrorCode::InvalidRequest, "The session is already initialized.");
  }

  if (method == "tools/list")
  {
    return ok(
      m_state->tools.list(session.protocolVersion, stringParam(params, "cursor")));
  }

  if (method == "tools/call")
  {
    const auto name = stringParam(params, "name");
    if (!name)
    {
      return fail(ErrorCode::InvalidParams, "tools/call requires the parameter name");
    }
    if (!m_state->tools.find(*name))
    {
      return fail(
        ErrorCode::InvalidParams,
        "Unknown tool: " + *name + ". Use tools/list to see the available tools.");
    }

    const auto* arguments = findMember(params, "arguments");
    if (arguments && !arguments->is_object() && !arguments->is_null())
    {
      return fail(ErrorCode::InvalidParams, "tools/call arguments must be an object");
    }

    auto progress = CallContext::ProgressFn{};
    if (const auto* meta = findMember(params, "_meta"))
    {
      if (const auto* token = findMember(*meta, "progressToken"))
      {
        progress = [stream, token = *token](
                     const double value,
                     const std::optional<double> total,
                     const std::string& message) {
          if (auto s = stream.lock())
          {
            auto progressParams = Json{{"progressToken", token}, {"progress", value}};
            if (total)
            {
              progressParams["total"] = *total;
            }
            if (!message.empty())
            {
              progressParams["message"] = message;
            }
            s->notify(
              makeNotification("notifications/progress", std::move(progressParams)));
          }
        };
      }
    }

    m_state->callRunner->submit(CallRequest{
      session.id,
      id,
      *name,
      arguments ? *arguments : Json::object(),
      std::move(progress),
      [respond, id](Json result) { respond(makeResponse(id, std::move(result))); },
    });
    return;
  }

  if (method == "resources/list")
  {
    return ok(m_state->resources.list(*m_state, session, stringParam(params, "cursor")));
  }

  if (method == "resources/templates/list")
  {
    return ok(m_state->resources.listTemplates());
  }

  if (method == "resources/read")
  {
    const auto uri = stringParam(params, "uri");
    if (!uri)
    {
      return fail(ErrorCode::InvalidParams, "resources/read requires the parameter uri");
    }
    auto contents = m_state->resources.read(*m_state, session, *uri);
    if (contents.is_error())
    {
      const auto error = errorOf(contents);
      const auto notFound = error.code == mcp::ErrorCode::ObjectNotFound
                            || error.code == mcp::ErrorCode::DocumentNotFound
                            || error.code == mcp::ErrorCode::NoDocument;
      return fail(
        notFound ? ErrorCode::ResourceNotFound : ErrorCode::InternalError,
        toText(error),
        Json{{"uri", *uri}});
    }
    return ok(Json{{"contents", std::move(contents.value())}});
  }

  if (method == "resources/subscribe" || method == "resources/unsubscribe")
  {
    const auto uri = stringParam(params, "uri");
    if (!uri)
    {
      return fail(ErrorCode::InvalidParams, method + " requires the parameter uri");
    }
    if (method == "resources/subscribe")
    {
      if (!m_state->resources.exists(*uri))
      {
        return fail(
          ErrorCode::ResourceNotFound,
          "Resource '" + *uri + "' does not exist.",
          Json{{"uri", *uri}});
      }
      session.subscriptions.insert(*uri);
    }
    else
    {
      session.subscriptions.erase(*uri);
    }
    return ok(Json::object());
  }

  if (method == "prompts/list")
  {
    return ok(
      m_state->prompts.list(session.protocolVersion, stringParam(params, "cursor")));
  }

  if (method == "prompts/get")
  {
    const auto name = stringParam(params, "name");
    if (!name)
    {
      return fail(ErrorCode::InvalidParams, "prompts/get requires the parameter name");
    }

    auto arguments = PromptArguments{};
    if (const auto* args = findMember(params, "arguments"); args && args->is_object())
    {
      for (const auto& [key, value] : args->items())
      {
        if (value.is_string())
        {
          arguments[key] = value.get<std::string>();
        }
        else
        {
          arguments[key] = dumpJson(value);
        }
      }
    }

    auto result = m_state->prompts.get(*name, arguments);
    if (result.is_error())
    {
      return fail(ErrorCode::InvalidParams, toText(errorOf(result)));
    }
    return ok(std::move(result.value()));
  }

  if (method == "logging/setLevel")
  {
    if (const auto level = stringParam(params, "level"))
    {
      session.logLevel = *level;
      return ok(Json::object());
    }
    return fail(
      ErrorCode::InvalidParams, "logging/setLevel requires the parameter level");
  }

  fail(ErrorCode::MethodNotFound, "Method not found: " + method);
}

void McpServer::handleNotification(
  Session& session, const jsonrpc::Notification& notification)
{
  if (notification.method == "notifications/initialized")
  {
    session.initialized = true;
  }
  else if (notification.method == "notifications/cancelled")
  {
    if (const auto* requestId = findMember(notification.params, "requestId"))
    {
      m_state->callRunner->cancel(session.id, *requestId);
    }
  }
  // other notifications (e.g. roots/list_changed) are ignored
}

} // namespace tb::mcp
