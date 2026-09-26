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

#include "mcp/StreamableHttp.h"

#include "mcp/Endpoint.h"
#include "mcp/HttpResponse.h"
#include "mcp/Json.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>
#include <vector>

namespace tb::mcp
{

HttpConnection::~HttpConnection() = default;

struct StreamableHttpServer::ConnectionState
{
  StreamableHttpServer& server;
  HttpConnection& connection;
  HttpParser parser;

  /** Set once the connection was closed; the connection must not be used afterwards. */
  bool closed = false;
  /** Set while processRequests runs, to prevent reentrant processing. */
  bool processing = false;
  /** Set while a request is being answered, including open SSE streams. */
  bool busy = false;
  /** Whether the connection stays open after the current response. */
  bool keepAlive = true;
  /** Set while an SSE response body is open; such streams receive keep-alives. */
  bool streaming = false;

  std::shared_ptr<RequestStream> requestStream;
  std::shared_ptr<NotificationStream> notificationStream;

  ConnectionState(
    StreamableHttpServer& server_, HttpConnection& connection_, std::size_t maxBodySize)
    : server{server_}
    , connection{connection_}
    , parser{maxBodySize}
  {
  }

  void write(const std::string_view bytes)
  {
    if (!closed && !bytes.empty())
    {
      connection.write(bytes);
    }
  }

  void writeResponse(HttpResponse response)
  {
    if (!keepAlive)
    {
      response.headers.emplace_back("Connection", "close");
    }
    write(serializeHttpResponse(response));
  }

  void writeSseHead(HttpHeaders headers)
  {
    if (!keepAlive)
    {
      headers.emplace_back("Connection", "close");
    }
    write(serializeSseResponseHead(200, headers));
    streaming = true;
  }

  void writeSseEvent(const Json& message, const std::string& sessionId)
  {
    const auto id =
      sessionId.empty() ? std::string{} : std::to_string(server.nextEventId(sessionId));
    write(encodeHttpChunk(encodeSseEvent(dumpJson(message), "message", id)));
  }

  void endSse()
  {
    write(lastHttpChunk());
    streaming = false;
  }

  void dropStreams()
  {
    requestStream.reset();
    notificationStream.reset();
    streaming = false;
  }
};

namespace
{

using ConnectionState = StreamableHttpServer::ConnectionState;

constexpr auto SessionIdHeader = std::string_view{"Mcp-Session-Id"};
constexpr auto ProtocolVersionHeader = std::string_view{"MCP-Protocol-Version"};

HttpResponse makeErrorResponse(
  const int status, const std::string_view message, HttpHeaders headers = {})
{
  const auto code = status == 400 ? -32600 : -32000;
  const auto body = Json{
    {"jsonrpc", "2.0"},
    {"id", nullptr},
    {"error", {{"code", code}, {"message", message}}},
  };
  headers.emplace_back("Content-Type", "application/json");
  return HttpResponse{status, std::move(headers), dumpJson(body)};
}

std::string_view trim(std::string_view str)
{
  while (!str.empty() && (str.front() == ' ' || str.front() == '\t'))
  {
    str.remove_prefix(1);
  }
  while (!str.empty() && (str.back() == ' ' || str.back() == '\t'))
  {
    str.remove_suffix(1);
  }
  return str;
}

std::string toLower(std::string_view str)
{
  auto result = std::string{str};
  std::ranges::transform(result, result.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

/**
 * Returns the host part of a `host[:port]` authority, without brackets for IPv6
 * addresses, in lower case.
 */
std::string hostOfAuthority(std::string_view authority)
{
  if (authority.starts_with('['))
  {
    const auto end = authority.find(']');
    return toLower(authority.substr(1, end == std::string_view::npos ? end : end - 1));
  }

  if (const auto colon = authority.find(':');
      colon != std::string_view::npos
      && authority.find(':', colon + 1) == std::string_view::npos)
  {
    return toLower(authority.substr(0, colon));
  }
  return toLower(authority);
}

bool isIPv4Address(std::string_view address)
{
  auto octets = 0;
  while (true)
  {
    const auto dot = address.find('.');
    const auto octet = address.substr(0, dot);
    if (
      octet.empty() || octet.size() > 3
      || !std::ranges::all_of(octet, [](const char c) { return c >= '0' && c <= '9'; })
      || std::stoi(std::string{octet}) > 255)
    {
      return false;
    }

    ++octets;
    if (dot == std::string_view::npos)
    {
      break;
    }
    address.remove_prefix(dot + 1);
  }
  return octets == 4;
}

bool isWildcardAddress(const std::string_view address)
{
  return address.empty() || address == "0.0.0.0" || address == "::" || address == "[::]";
}

bool isLocalHostName(const std::string_view host)
{
  return host == "localhost" || host == "::1"
         || StreamableHttpServer::isLoopbackAddress(host);
}

bool contains(const std::string_view haystack, const std::string_view needle)
{
  return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

bool isJsonContentType(const std::string_view contentType)
{
  const auto mediaType = trim(contentType.substr(0, contentType.find(';')));
  return equalsIgnoreCase(mediaType, "application/json");
}

bool constantTimeEquals(const std::string_view lhs, const std::string_view rhs)
{
  if (lhs.size() != rhs.size())
  {
    return false;
  }

  auto diff = 0u;
  for (auto i = std::size_t{0}; i < lhs.size(); ++i)
  {
    diff |= static_cast<unsigned>(lhs[i] ^ rhs[i]);
  }
  return diff == 0;
}

/**
 * The stream for one POST exchange. Buffers all output until `postReturned` was called,
 * then answers with JSON if the final response arrives before any notification, or
 * switches to SSE otherwise.
 */
class PostRequestStream : public RequestStream
{
private:
  std::weak_ptr<ConnectionState> m_state;
  std::function<void(const std::shared_ptr<ConnectionState>&)> m_onComplete;

  bool m_postReturned = false;
  bool m_done = false;
  bool m_sse = false;
  std::vector<Json> m_bufferedNotifications;
  std::optional<Json> m_bufferedResponse;

  std::string m_sessionId;
  HttpHeaders m_headers;

public:
  PostRequestStream(
    std::weak_ptr<ConnectionState> state,
    std::function<void(const std::shared_ptr<ConnectionState>&)> onComplete)
    : m_state{std::move(state)}
    , m_onComplete{std::move(onComplete)}
  {
  }

  void notify(const Json& notification) override
  {
    if (m_done)
    {
      return;
    }

    if (!m_postReturned)
    {
      m_bufferedNotifications.push_back(notification);
      return;
    }

    if (auto state = lockState())
    {
      startSse(*state);
      state->writeSseEvent(notification, m_sessionId);
    }
  }

  void complete(const Json& response) override
  {
    if (m_done)
    {
      return;
    }

    if (!m_postReturned)
    {
      m_bufferedResponse = response;
      return;
    }

    if (auto state = lockState())
    {
      finish(state, response);
    }
    else
    {
      m_done = true;
    }
  }

  /**
   * Called when `Endpoint::post` returned `Pending`. Flushes the buffered output.
   *
   * @param sessionId the session of the exchange, used for SSE event IDs
   * @param headers additional response headers, e.g. `Mcp-Session-Id`
   */
  void postReturned(
    const std::shared_ptr<ConnectionState>& state,
    std::string sessionId,
    HttpHeaders headers)
  {
    m_postReturned = true;
    m_sessionId = std::move(sessionId);
    m_headers = std::move(headers);

    if (!m_bufferedNotifications.empty())
    {
      startSse(*state);
      for (const auto& notification : m_bufferedNotifications)
      {
        state->writeSseEvent(notification, m_sessionId);
      }
      m_bufferedNotifications.clear();
    }

    if (m_bufferedResponse)
    {
      auto response = std::move(*m_bufferedResponse);
      m_bufferedResponse = std::nullopt;
      finish(state, response);
    }
  }

private:
  std::shared_ptr<ConnectionState> lockState() const
  {
    auto state = m_state.lock();
    return state && !state->closed && state->requestStream.get() == this ? state
                                                                         : nullptr;
  }

  void startSse(ConnectionState& state)
  {
    if (!m_sse)
    {
      m_sse = true;
      state.writeSseHead(m_headers);
    }
  }

  void finish(const std::shared_ptr<ConnectionState>& state, const Json& response)
  {
    m_done = true;
    if (m_sse)
    {
      state->writeSseEvent(response, m_sessionId);
      state->endSse();
    }
    else
    {
      auto headers = m_headers;
      headers.emplace_back("Content-Type", "application/json");
      state->writeResponse(HttpResponse{200, std::move(headers), dumpJson(response)});
    }

    // must be last, this may destroy this object
    m_onComplete(state);
  }
};

/**
 * The standalone SSE stream of a session (HTTP GET).
 */
class SseNotificationStream : public NotificationStream
{
private:
  std::weak_ptr<ConnectionState> m_state;
  std::string m_sessionId;

public:
  SseNotificationStream(std::weak_ptr<ConnectionState> state, std::string sessionId)
    : m_state{std::move(state)}
    , m_sessionId{std::move(sessionId)}
  {
  }

  void send(const Json& notification) override
  {
    auto state = m_state.lock();
    if (state && !state->closed && state->notificationStream.get() == this)
    {
      state->writeSseEvent(notification, m_sessionId);
    }
  }
};

} // namespace

bool StreamableHttpServer::isLoopbackAddress(std::string_view address)
{
  if (address.starts_with('[') && address.ends_with(']'))
  {
    address = address.substr(1, address.size() - 2);
  }

  if (address.starts_with("::ffff:"))
  {
    address.remove_prefix(7);
  }

  return equalsIgnoreCase(address, "localhost") || address == "::1"
         || (address.starts_with("127.") && isIPv4Address(address));
}

std::optional<std::string> StreamableHttpServer::validateConfig(const Config& config)
{
  if (!isLoopbackAddress(config.bindAddress) && config.accessToken.empty())
  {
    return "An access token is required to listen on the non-loopback address "
           + config.bindAddress;
  }
  return std::nullopt;
}

StreamableHttpServer::StreamableHttpServer(Endpoint& endpoint, Config config)
  : m_endpoint{endpoint}
  , m_config{std::move(config)}
{
}

StreamableHttpServer::~StreamableHttpServer()
{
  for (auto& [connection, state] : m_connections)
  {
    state->closed = true;
    state->dropStreams();
  }
}

const StreamableHttpServer::Config& StreamableHttpServer::config() const
{
  return m_config;
}

void StreamableHttpServer::openConnection(HttpConnection& connection)
{
  m_connections[&connection] =
    std::make_shared<ConnectionState>(*this, connection, m_config.maxBodySize);
}

void StreamableHttpServer::feed(HttpConnection& connection, const std::string_view bytes)
{
  const auto it = m_connections.find(&connection);
  if (it == m_connections.end())
  {
    return;
  }

  auto state = it->second;
  state->parser.feed(bytes);
  processRequests(state);
}

void StreamableHttpServer::connectionClosed(HttpConnection& connection)
{
  const auto it = m_connections.find(&connection);
  if (it == m_connections.end())
  {
    return;
  }

  auto state = std::move(it->second);
  m_connections.erase(it);
  state->closed = true;
  state->dropStreams();
}

void StreamableHttpServer::sendKeepAlives()
{
  auto states = std::vector<std::shared_ptr<ConnectionState>>{};
  for (const auto& [connection, state] : m_connections)
  {
    if (state->streaming)
    {
      states.push_back(state);
    }
  }

  for (const auto& state : states)
  {
    if (state->streaming)
    {
      state->write(encodeHttpChunk(sseKeepAliveComment()));
    }
  }
}

void StreamableHttpServer::closeAllConnections()
{
  auto connections = std::exchange(m_connections, {});
  m_notificationStreams.clear();

  for (auto& [connection, state] : connections)
  {
    state->closed = true;
    state->dropStreams();
    connection->close();
  }
}

std::size_t StreamableHttpServer::connectionCount() const
{
  return m_connections.size();
}

std::uint64_t StreamableHttpServer::nextEventId(const std::string& sessionId)
{
  return ++m_nextEventIds[sessionId];
}

void StreamableHttpServer::processRequests(const std::shared_ptr<ConnectionState>& state)
{
  if (state->processing)
  {
    return;
  }

  state->processing = true;
  while (!state->closed && !state->busy)
  {
    if (auto request = state->parser.nextRequest())
    {
      handleRequest(state, *request);
    }
    else if (const auto& error = state->parser.error())
    {
      state->keepAlive = false;
      state->writeResponse(makeErrorResponse(error->status, error->message));
      closeConnection(state);
    }
    else
    {
      break;
    }
  }
  state->processing = false;
}

void StreamableHttpServer::completeExchange(const std::shared_ptr<ConnectionState>& state)
{
  state->busy = false;
  state->dropStreams();

  if (!state->keepAlive)
  {
    closeConnection(state);
  }
  else
  {
    processRequests(state);
  }
}

void StreamableHttpServer::closeConnection(const std::shared_ptr<ConnectionState>& state)
{
  if (state->closed)
  {
    return;
  }

  state->closed = true;
  state->dropStreams();

  if (const auto it = m_connections.find(&state->connection);
      it != m_connections.end() && it->second == state)
  {
    m_connections.erase(it);
  }

  // may call connectionClosed, which is a no-op now
  state->connection.close();
}

void StreamableHttpServer::handleRequest(
  const std::shared_ptr<ConnectionState>& state, const HttpRequest& request)
{
  state->busy = true;
  state->keepAlive = request.keepAlive();

  if (!checkSecurity(state, request))
  {
    return;
  }

  if (request.path != m_config.path)
  {
    state->writeResponse(makeErrorResponse(404, "Not found"));
    completeExchange(state);
  }
  else if (request.method == "POST")
  {
    handlePost(state, request);
  }
  else if (request.method == "GET")
  {
    handleGet(state, request);
  }
  else if (request.method == "DELETE")
  {
    handleDelete(state, request);
  }
  else
  {
    state->writeResponse(
      makeErrorResponse(405, "Method not allowed", {{"Allow", "GET, POST, DELETE"}}));
    completeExchange(state);
  }
}

void StreamableHttpServer::handlePost(
  const std::shared_ptr<ConnectionState>& state, const HttpRequest& request)
{
  const auto contentType = request.header("Content-Type");
  if (!contentType || !isJsonContentType(*contentType))
  {
    state->writeResponse(makeErrorResponse(415, "Content-Type must be application/json"));
    completeExchange(state);
    return;
  }

  auto sessionId = std::optional<std::string>{};
  if (const auto header = request.header(SessionIdHeader))
  {
    sessionId = std::string{*header};
    if (!checkSession(state, request, *sessionId))
    {
      return;
    }
  }

  auto stream = std::make_shared<PostRequestStream>(
    state, [this](const auto& s) { completeExchange(s); });
  state->requestStream = stream;

  const auto result = m_endpoint.post(sessionId, request.body, stream);

  if (state->closed)
  {
    return;
  }

  auto headers = HttpHeaders{};
  if (!result.newSessionId.empty())
  {
    headers.emplace_back(SessionIdHeader, result.newSessionId);
  }

  switch (result.status)
  {
  case PostStatus::Accepted:
    state->requestStream.reset();
    state->writeResponse(HttpResponse{202, std::move(headers), {}});
    completeExchange(state);
    break;
  case PostStatus::Pending: {
    auto eventSessionId =
      !result.newSessionId.empty() ? result.newSessionId : sessionId.value_or("");
    stream->postReturned(state, std::move(eventSessionId), std::move(headers));
    break;
  }
  case PostStatus::BadRequest:
    state->requestStream.reset();
    if (!result.body.is_null())
    {
      headers.emplace_back("Content-Type", "application/json");
      state->writeResponse(HttpResponse{400, std::move(headers), dumpJson(result.body)});
    }
    else
    {
      state->writeResponse(HttpResponse{400, std::move(headers), {}});
    }
    completeExchange(state);
    break;
  case PostStatus::SessionNotFound:
    state->requestStream.reset();
    state->writeResponse(makeErrorResponse(404, "Session not found"));
    completeExchange(state);
    break;
  }
}

void StreamableHttpServer::handleGet(
  const std::shared_ptr<ConnectionState>& state, const HttpRequest& request)
{
  if (const auto accept = request.header("Accept");
      accept && !contains(*accept, "text/event-stream") && !contains(*accept, "*/*"))
  {
    state->writeResponse(makeErrorResponse(406, "Accept must include text/event-stream"));
    completeExchange(state);
    return;
  }

  const auto header = request.header(SessionIdHeader);
  if (!header)
  {
    state->writeResponse(makeErrorResponse(400, "Missing Mcp-Session-Id header"));
    completeExchange(state);
    return;
  }

  auto sessionId = std::string{*header};
  if (!checkSession(state, request, sessionId))
  {
    return;
  }

  // There is only one standalone stream per session
  endNotificationStream(sessionId);

  state->writeSseHead({});
  auto stream = std::make_shared<SseNotificationStream>(state, sessionId);
  state->notificationStream = stream;
  m_notificationStreams[sessionId] = state;

  if (!m_endpoint.openNotificationStream(sessionId, stream))
  {
    m_notificationStreams.erase(sessionId);
    state->endSse();
    completeExchange(state);
  }
}

void StreamableHttpServer::handleDelete(
  const std::shared_ptr<ConnectionState>& state, const HttpRequest& request)
{
  const auto header = request.header(SessionIdHeader);
  if (!header)
  {
    state->writeResponse(makeErrorResponse(400, "Missing Mcp-Session-Id header"));
    completeExchange(state);
    return;
  }

  const auto sessionId = std::string{*header};
  if (!checkSession(state, request, sessionId))
  {
    return;
  }

  if (!m_endpoint.deleteSession(sessionId))
  {
    state->writeResponse(makeErrorResponse(404, "Session not found"));
    completeExchange(state);
    return;
  }

  endNotificationStream(sessionId);
  m_nextEventIds.erase(sessionId);

  state->writeResponse(HttpResponse{200, {}, {}});
  completeExchange(state);
}

bool StreamableHttpServer::checkSecurity(
  const std::shared_ptr<ConnectionState>& state, const HttpRequest& request)
{
  const auto reject =
    [&](const int status, const std::string_view message, HttpHeaders headers = {}) {
      state->writeResponse(makeErrorResponse(status, message, std::move(headers)));
      completeExchange(state);
      return false;
    };

  const auto bindHost = hostOfAuthority(m_config.bindAddress);

  // DNS rebinding protection
  if (const auto host = request.header("Host"))
  {
    const auto hostName = hostOfAuthority(*host);
    if (
      !isWildcardAddress(m_config.bindAddress) && !isLocalHostName(hostName)
      && hostName != bindHost)
    {
      return reject(403, "Host not allowed");
    }
  }
  else if (request.minorVersion == 1)
  {
    return reject(400, "Missing Host header");
  }

  // Cross-origin requests from browsers
  if (const auto origin = request.header("Origin"))
  {
    auto allowed = false;
    for (const auto scheme : {"http://", "https://"})
    {
      if (origin->starts_with(scheme))
      {
        const auto authority = origin->substr(std::string_view{scheme}.size());
        const auto hostName = hostOfAuthority(authority.substr(0, authority.find('/')));
        allowed = isLocalHostName(hostName)
                  || (!isWildcardAddress(m_config.bindAddress) && hostName == bindHost);
      }
    }
    if (!allowed)
    {
      return reject(403, "Origin not allowed");
    }
  }

  if (!isLoopbackAddress(m_config.bindAddress))
  {
    const auto authorization = request.header("Authorization");
    const auto space = authorization ? authorization->find(' ') : std::string_view::npos;
    if (
      space == std::string_view::npos
      || !equalsIgnoreCase(authorization->substr(0, space), "Bearer")
      || !constantTimeEquals(
        trim(authorization->substr(space + 1)), m_config.accessToken))
    {
      return reject(401, "Unauthorized", {{"WWW-Authenticate", "Bearer"}});
    }
  }

  return true;
}

bool StreamableHttpServer::checkSession(
  const std::shared_ptr<ConnectionState>& state,
  const HttpRequest& request,
  const std::string& sessionId)
{
  const auto version = m_endpoint.sessionProtocolVersion(sessionId);
  if (!version)
  {
    state->writeResponse(makeErrorResponse(404, "Session not found"));
    completeExchange(state);
    return false;
  }

  if (const auto header = request.header(ProtocolVersionHeader);
      header && *header != *version)
  {
    state->writeResponse(makeErrorResponse(
      400, "MCP-Protocol-Version does not match the negotiated version " + *version));
    completeExchange(state);
    return false;
  }

  return true;
}

void StreamableHttpServer::endNotificationStream(const std::string& sessionId)
{
  const auto it = m_notificationStreams.find(sessionId);
  if (it == m_notificationStreams.end())
  {
    return;
  }

  auto state = it->second.lock();
  m_notificationStreams.erase(it);

  if (state && !state->closed && state->notificationStream)
  {
    state->endSse();
    closeConnection(state);
  }
}

} // namespace tb::mcp
