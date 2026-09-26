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

#include "mcp/CallContext.h"
#include "mcp/Json.h"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tb::mcp
{
class ServerState;
class ToolDef;

/** A `tools/call` request as the call runner sees it. */
struct CallRequest
{
  std::string sessionId;
  /** The JSON-RPC request id, used for cancellation. */
  Json requestId;
  std::string toolName;
  Json arguments;
  CallContext::ProgressFn progress;
  /** Receives the CallToolResult. May be called before `submit` returns. */
  std::function<void(Json)> completion;
};

/** Builds a CallToolResult from the structured content. */
Json makeCallToolResult(
  const Json& structured, bool isError, std::string_view protocolVersion);

/**
 * Runs tool calls. Read-only calls run immediately. Modifying calls go through one FIFO
 * queue shared by all sessions, wait while the human is busy (spec X13), and run in a
 * transaction (Mutation::Map) that is rolled back on failure or dry run.
 */
class CallRunner
{
private:
  struct PendingCall
  {
    CallRequest request;
    std::chrono::steady_clock::time_point enqueued;
  };

  /** An asynchronous call that has started and not yet completed. */
  struct AsyncCall;

  ServerState& m_server;
  std::deque<PendingCall> m_queue;
  bool m_running = false;
  std::unique_ptr<AsyncCall> m_asyncCall;
  bool m_startingAsync = false;
  bool m_pollScheduled = false;
  /** Invalidates scheduled polls when the runner is destroyed. */
  std::shared_ptr<bool> m_alive;

public:
  explicit CallRunner(ServerState& server);
  ~CallRunner();

  void submit(CallRequest request);

  /**
   * Removes a queued call and completes it with CANCELLED, or asks a running asynchronous
   * call to stop at its next step. Returns false if the call is neither queued nor
   * running asynchronously.
   */
  bool cancel(const std::string& sessionId, const Json& requestId);

  /**
   * Cancels all queued calls, or those of one session. A running asynchronous call of
   * the affected sessions is abandoned and completed with CANCELLED.
   */
  void cancelAll(const std::optional<std::string>& sessionId = std::nullopt);

  size_t queueSize() const;

private:
  void pump();
  void schedulePoll();

  /** Executes the call; returns the CallToolResult. */
  Json execute(const CallRequest& request);

  /**
   * Starts an asynchronous call. Returns true if the call is still running; it then
   * resumes the queue when it completes.
   */
  bool startAsync(CallRequest request);
  /** Completes the running asynchronous call and resumes the queue. */
  void completeAsync(ToolResult result);
  void finish(const CallRequest& request, Json result);
};

} // namespace tb::mcp
