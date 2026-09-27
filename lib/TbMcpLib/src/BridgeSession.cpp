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

#include "mcp/BridgeSession.h"

#include "base/PreferenceManager.h"
#include "base/PreferenceStore.h"
#include "fs/FileSystem.h"
#include "mcp/Host.h"
#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/RegisterAll.h"
#include "mcp/Scheduler.h"
#include "mdl/GameManager.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace tb::mcp
{
namespace
{

constexpr auto NoEditor = "The stdio bridge has no editor.";

/**
 * The host of the bridge's offline server: no editor, no documents, no games. The
 * offline server never runs tools, so the functions that only tools use fail or do
 * nothing.
 */
class OfflineHost : public McpHost, public DocumentHost
{
private:
  mdl::GameManager m_gameManager{nullptr, {}};
  std::string m_version;

public:
  explicit OfflineHost(std::string version)
    : m_version{std::move(version)}
  {
  }

  std::string applicationVersion() const override { return m_version; }

  std::vector<DocumentInfo> documents() override { return {}; }

  BusyState busyState(ui::MapDocument&) override { return BusyState::Idle; }

  std::vector<std::string> prepareForAgentEdit(ui::MapDocument&) override { return {}; }

  std::optional<std::string> currentToolName(ui::MapDocument&) override
  {
    return std::nullopt;
  }

  bool isCompileRunning(ui::MapDocument&) override { return false; }

  DocumentHost& documentHost() override { return *this; }

  mdl::GameManager& gameManager() override { return m_gameManager; }

  std::optional<DocumentInfo> documentToReplace() override { return std::nullopt; }

  Result<OpenedDocument> createDocument(
    const mdl::GameInfo&, mdl::MapFormat, bool) override
  {
    return Error{NoEditor};
  }

  Result<OpenedDocument> loadDocument(
    const mdl::GameInfo&, mdl::MapFormat, const std::filesystem::path&, bool) override
  {
    return Error{NoEditor};
  }

  Result<void> showDocument(ui::MapDocument&) override { return Error{NoEditor}; }

  void closeDocument(ui::MapDocument&) override {}

  std::vector<std::filesystem::path> recentDocuments() override { return {}; }
};

/** The scheduler of the offline server, which never schedules anything it must run. */
class OfflineScheduler : public Scheduler
{
public:
  void post(std::function<void()>) override {}

  void postDelayed(std::chrono::milliseconds, std::function<void()>) override {}

  std::chrono::steady_clock::time_point now() const override
  {
    return std::chrono::steady_clock::now();
  }
};

class ResponseStream : public RequestStream
{
public:
  std::optional<Json> response;

  void notify(const Json&) override {}

  void complete(const Json& response_) override { response = response_; }
};

/** Loads no preferences and saves none. */
class NullPreferenceStore : public PreferenceStore
{
public:
  bool load(const std::filesystem::path&, bool&) override { return false; }
  bool load(const std::filesystem::path&, int&) override { return false; }
  bool load(const std::filesystem::path&, float&) override { return false; }
  bool load(const std::filesystem::path&, std::string&) override { return false; }
  bool load(const std::filesystem::path&, std::filesystem::path&) override
  {
    return false;
  }
  bool load(const std::filesystem::path&, Color&) override { return false; }
  bool load(const std::filesystem::path&, std::vector<KeySequence>&) override
  {
    return false;
  }

  void save(const std::filesystem::path&, bool) override {}
  void save(const std::filesystem::path&, int) override {}
  void save(const std::filesystem::path&, float) override {}
  void save(const std::filesystem::path&, const std::string&) override {}
  void save(const std::filesystem::path&, const std::filesystem::path&) override {}
  void save(const std::filesystem::path&, const Color&) override {}
  void save(const std::filesystem::path&, const std::vector<KeySequence>&) override {}
};

/** The requests that the bridge answers itself while it has no editor session. */
constexpr auto BridgeMethods = std::array<std::string_view, 7>{
  "ping",
  "tools/list",
  "resources/list",
  "resources/templates/list",
  "prompts/list",
  "prompts/get",
  "logging/setLevel",
};

std::string methodOf(const Json& message)
{
  const auto* method = findMember(message, "method");
  return method && method->is_string() ? method->get<std::string>() : std::string{};
}

bool isRequest(const Json& message)
{
  return message.is_object() && message.contains("method") && message.contains("id");
}

bool isNotification(const Json& message)
{
  return message.is_object() && message.contains("method") && !message.contains("id");
}

bool isClientResponse(const Json& message)
{
  return message.is_object() && !message.contains("method") && message.contains("id")
         && (message.contains("result") || message.contains("error"));
}

/** Whether the message or batch is answered without a response. */
bool expectsNoResponse(const Json& message)
{
  const auto noResponse = [](const Json& m) {
    return isNotification(m) || isClientResponse(m);
  };
  if (message.is_array())
  {
    return !message.empty() && std::ranges::all_of(message, noResponse);
  }
  return noResponse(message);
}

template <typename F>
void forEachMessage(const Json& message, const F& f)
{
  if (message.is_array())
  {
    for (const auto& m : message)
    {
      f(m);
    }
  }
  else
  {
    f(message);
  }
}

std::optional<std::string> stringParam(const Json& message, const std::string_view key)
{
  const auto* params = findMember(message, "params");
  const auto* value = params ? findMember(*params, key) : nullptr;
  return value && value->is_string() ? std::optional{value->get<std::string>()}
                                     : std::nullopt;
}

BridgeRoute routeOfMessage(
  const Json& message, const bool clientInitialized, const bool editorConnected)
{
  const auto method = methodOf(message);
  if (method == "initialize")
  {
    return BridgeRoute::Bridge;
  }
  if (editorConnected)
  {
    return BridgeRoute::Editor;
  }
  if (!clientInitialized)
  {
    // the offline server answers as the editor would: initialize first
    return BridgeRoute::Bridge;
  }
  if (isRequest(message))
  {
    return std::ranges::find(BridgeMethods, method) != BridgeMethods.end()
             ? BridgeRoute::Bridge
             : BridgeRoute::Editor;
  }
  if (isNotification(message))
  {
    return method == "notifications/initialized" ? BridgeRoute::Bridge
                                                 : BridgeRoute::EditorIfConnected;
  }
  if (isClientResponse(message))
  {
    return BridgeRoute::EditorIfConnected;
  }
  // invalid messages get their error response from the offline server
  return BridgeRoute::Bridge;
}

Json handshakeId(const std::string_view name)
{
  return std::string{BridgeSession::HandshakeIdPrefix} + std::string{name};
}

} // namespace

struct BridgeSession::Offline
{
  OfflineHost host;
  OfflineScheduler scheduler;
  McpServer server;

  explicit Offline(std::string version)
    : host{version}
    , server{host, scheduler, ServerInfo{.version = std::move(version)}}
  {
    registerAll(server);
  }
};

BridgeSession::BridgeSession(std::string version)
  : m_offline{std::make_unique<Offline>(std::move(version))}
{
}

BridgeSession::~BridgeSession() = default;

void BridgeSession::createNullPreferenceManager()
{
  PreferenceManager::createInstance(std::make_unique<NullPreferenceStore>(), true);
}

BridgeRoute BridgeSession::route(const Json& message, const bool editorConnected) const
{
  if (!message.is_array())
  {
    return routeOfMessage(message, clientInitialized(), editorConnected);
  }

  if (editorConnected)
  {
    return BridgeRoute::Editor;
  }

  // a batch goes to the editor if any of its messages needs the editor
  const auto needsEditor = std::ranges::any_of(message, [&](const auto& m) {
    return routeOfMessage(m, clientInitialized(), false) == BridgeRoute::Editor;
  });
  return needsEditor ? BridgeRoute::Editor : BridgeRoute::Bridge;
}

std::optional<Json> BridgeSession::answer(const Json& message)
{
  auto& server = m_offline->server;
  const auto isInitialize = isRequest(message) && methodOf(message) == "initialize";
  if (isInitialize && m_offlineSessionId)
  {
    // the client starts a new session
    server.deleteSession(*m_offlineSessionId);
    m_offlineSessionId = std::nullopt;
  }

  auto stream = std::make_shared<ResponseStream>();
  const auto result = server.post(
    isInitialize ? std::nullopt : m_offlineSessionId, dumpJson(message), stream);

  if (isInitialize && !result.newSessionId.empty())
  {
    m_offlineSessionId = result.newSessionId;
    const auto* params = findMember(message, "params");
    m_initializeParams = params ? *params : Json::object();
  }

  if (result.status == PostStatus::Pending && stream->response)
  {
    // record the log level that the client set successfully
    const auto& response = *stream->response;
    forEachMessage(message, [&](const Json& m) {
      if (isRequest(m) && methodOf(m) == "logging/setLevel")
      {
        const auto succeeded = [&](const Json& r) {
          return r.is_object() && r.contains("result")
                 && r.value("id", Json{}) == m["id"];
        };
        const auto ok = response.is_array() ? std::ranges::any_of(response, succeeded)
                                            : succeeded(response);
        if (ok)
        {
          m_logLevel = stringParam(m, "level");
        }
      }
    });
  }

  if (expectsNoResponse(message))
  {
    return std::nullopt;
  }

  switch (result.status)
  {
  case PostStatus::Pending:
    return stream->response;
  case PostStatus::BadRequest:
    return result.body.is_null() ? std::nullopt : std::optional{result.body};
  case PostStatus::SessionNotFound:
    return jsonrpc::makeError(
      isRequest(message) ? message["id"] : Json(nullptr),
      jsonrpc::ErrorCode::InvalidRequest,
      "Send an initialize request first.");
  case PostStatus::Accepted:
    break;
  }
  return std::nullopt;
}

void BridgeSession::forwarded(const Json& message)
{
  forEachMessage(message, [&](const Json& m) {
    if (!isRequest(m))
    {
      return;
    }

    const auto method = methodOf(m);
    if (method == "logging/setLevel")
    {
      if (const auto level = stringParam(m, "level"))
      {
        m_logLevel = level;
      }
    }
    else if (method == "resources/subscribe")
    {
      if (const auto uri = stringParam(m, "uri"))
      {
        m_subscriptions.insert(*uri);
      }
    }
    else if (method == "resources/unsubscribe")
    {
      if (const auto uri = stringParam(m, "uri"))
      {
        m_subscriptions.erase(*uri);
      }
    }
  });
}

bool BridgeSession::clientInitialized() const
{
  return m_offlineSessionId.has_value();
}

std::vector<Json> BridgeSession::handshake() const
{
  auto result = std::vector<Json>{
    jsonrpc::makeRequest(
      handshakeId("initialize"),
      "initialize",
      m_initializeParams.value_or(Json::object())),
    jsonrpc::makeNotification("notifications/initialized"),
  };

  if (m_logLevel)
  {
    result.push_back(jsonrpc::makeRequest(
      handshakeId("logging"), "logging/setLevel", Json{{"level", *m_logLevel}}));
  }

  auto index = size_t{0};
  for (const auto& uri : m_subscriptions)
  {
    result.push_back(jsonrpc::makeRequest(
      handshakeId("subscribe-" + std::to_string(index++)),
      "resources/subscribe",
      Json{{"uri", uri}}));
  }

  result.push_back(
    jsonrpc::makeRequest(handshakeId("tools"), "tools/list", Json::object()));
  result.push_back(
    jsonrpc::makeRequest(handshakeId("resources"), "resources/list", Json::object()));
  return result;
}

bool BridgeSession::isHandshakeId(const Json& id)
{
  return id.is_string() && id.get<std::string>().starts_with(HandshakeIdPrefix);
}

std::vector<Json> BridgeSession::editorConnected(
  const Json& editorTools, const Json& editorResources)
{
  if (!m_knownTools)
  {
    m_knownTools = offlineResult("tools/list");
  }
  if (!m_knownResources)
  {
    m_knownResources = offlineResult("resources/list");
  }

  auto result = std::vector<Json>{};
  if (editorTools.is_null() || editorTools != *m_knownTools)
  {
    result.push_back(jsonrpc::makeNotification("notifications/tools/list_changed"));
  }
  if (
    m_resourcesStale || editorResources.is_null() || editorResources != *m_knownResources)
  {
    result.push_back(jsonrpc::makeNotification("notifications/resources/list_changed"));
  }

  m_knownTools = editorTools;
  m_knownResources = editorResources;
  m_resourcesStale = false;
  return result;
}

std::vector<Json> BridgeSession::editorDisconnected()
{
  auto result = std::vector<Json>{};
  const auto tools = offlineResult("tools/list");
  if (m_knownTools && *m_knownTools != tools)
  {
    result.push_back(jsonrpc::makeNotification("notifications/tools/list_changed"));
  }
  result.push_back(jsonrpc::makeNotification("notifications/resources/list_changed"));

  m_knownTools = tools;
  m_resourcesStale = true;
  return result;
}

Json BridgeSession::offlineResult(const std::string& method)
{
  if (!m_offlineSessionId)
  {
    return nullptr;
  }

  auto stream = std::make_shared<ResponseStream>();
  m_offline->server.post(
    m_offlineSessionId,
    dumpJson(jsonrpc::makeRequest(handshakeId(method), method, Json::object())),
    stream);

  const auto* result =
    stream->response ? findMember(*stream->response, "result") : nullptr;
  return result ? *result : Json(nullptr);
}

} // namespace tb::mcp
