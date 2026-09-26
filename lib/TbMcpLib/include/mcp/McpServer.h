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

#include "base/Notifier.h"
#include "mcp/Endpoint.h"
#include "mcp/Json.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
namespace jsonrpc
{
struct Notification;
struct Request;
} // namespace jsonrpc

class CallLog;
class CallRunner;
class McpHost;
class PromptRegistry;
class ResourceRegistry;
class Scheduler;
class ServerState;
class Session;
class ToolRegistry;

struct ServerInfo
{
  std::string name = "trenchbroom";
  std::string title = "TrenchBroom";
  std::string version;
  std::string instructions =
    "TrenchBroom level editor. Read the resource trenchbroom://guide for conventions "
    "(map units, Z up, degrees) and workflow tips. Every modifying tool is one undo step "
    "and accepts dryRun.";
};

struct ServerOptions
{
  /** How long a modifying call waits while the human is busy before it fails. */
  std::chrono::milliseconds busyWaitTimeout = std::chrono::milliseconds{30000};
  /** How often the busy state is re-checked while a call waits. */
  std::chrono::milliseconds busyPollInterval = std::chrono::milliseconds{50};
};

/** What the server is doing right now, for the status bar. */
struct ServerActivity
{
  enum class State
  {
    Idle,
    /** A tool call is executing. */
    Running,
    /** A modifying call waits because the human is busy. */
    WaitingForUser,
  };

  State state = State::Idle;
  /** The title of the running or waiting tool. */
  std::string toolTitle;
  /** The names of all open agent transactions. */
  std::vector<std::string> openTransactions;

  bool operator==(const ServerActivity&) const = default;
};

struct SessionInfo
{
  std::string id;
  std::string clientName;
  std::string clientVersion;
  std::string protocolVersion;
};

/**
 * The transport-neutral MCP protocol engine: sessions, lifecycle, method dispatch, the
 * registries and the tool call runner. Transports drive it through the Endpoint
 * interface. It must only be used on the thread that owns it.
 */
class McpServer : public Endpoint
{
public:
  /** Fired when a session is created or closed. */
  Notifier<> sessionsDidChangeNotifier;
  /** Fired when the activity changes. */
  Notifier<const ServerActivity&> activityDidChangeNotifier;

private:
  std::unique_ptr<ServerState> m_state;

public:
  McpServer(
    McpHost& host, Scheduler& scheduler, ServerInfo info, ServerOptions options = {});
  ~McpServer() override;

  ToolRegistry& tools();
  ResourceRegistry& resources();
  PromptRegistry& prompts();
  CallLog& callLog();

  const ServerOptions& options() const;
  void setOptions(ServerOptions options);

  std::vector<SessionInfo> sessions() const;
  size_t sessionCount() const;

  const ServerActivity& activity() const;

  /**
   * Stops all agents ("Stop agent" in the status bar): drops queued calls, rolls back
   * all agent transactions and closes all sessions. Clients must re-initialize.
   */
  void stopAgents();

  /**
   * Sends `notifications/resources/updated` to every session subscribed to the given
   * URI.
   */
  void notifyResourceUpdated(const std::string& uri);

  /** Sends a notification to every initialized session that has a notification stream. */
  void broadcast(const Json& notification);

  /** The shared server state, for tools and tests. */
  ServerState& state();

public: // Endpoint
  PostResult post(
    const std::optional<std::string>& sessionId,
    std::string_view body,
    std::shared_ptr<RequestStream> stream) override;

  std::optional<std::string> sessionProtocolVersion(
    std::string_view sessionId) const override;

  bool openNotificationStream(
    std::string_view sessionId, std::shared_ptr<NotificationStream> stream) override;

  bool deleteSession(std::string_view sessionId) override;

private:
  void dispatch(
    Session& session,
    jsonrpc::Request request,
    const std::weak_ptr<RequestStream>& stream,
    std::function<void(Json)> respond);
  void handleNotification(Session& session, const jsonrpc::Notification& notification);
};

} // namespace tb::mcp
