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

#include "mcp/Json.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tb::mcp
{

/** Where the stdio bridge sends a message of its client. */
enum class BridgeRoute
{
  /** The bridge answers the message itself with BridgeSession::answer. */
  Bridge,
  /**
   * The message goes to the editor. The bridge connects to the editor (and launches it)
   * first if it has no session with it yet.
   */
  Editor,
  /**
   * The message goes to the editor if the bridge has a session with it or messages wait
   * for the editor; otherwise it is dropped (notifications such as
   * `notifications/cancelled`, and responses of the client).
   */
  EditorIfConnected,
};

/**
 * The protocol logic of the stdio bridge (`TrenchBroomMcp`) that does not need the
 * editor, so that starting an MCP client does not start TrenchBroom.
 *
 * The bridge answers `initialize`, `ping`, `tools/list`, `resources/list`,
 * `resources/templates/list`, `prompts/list`, `prompts/get` and `logging/setLevel`
 * itself, with an McpServer that has every tool, resource and prompt registered
 * (`registerAll`) but no editor behind it. Its answers are those of the editor's server
 * of the same version: without an editor, `resources/list` has only the static
 * resources. Everything else needs the editor: the bridge then opens an editor session
 * with the client's `initialize` parameters (`handshake`), replays the log level and the
 * subscriptions, and forwards the waiting messages. Once connected, the bridge forwards
 * every message except `initialize`.
 *
 * The offline server needs a PreferenceManager; a process that has none calls
 * createNullPreferenceManager first.
 */
class BridgeSession
{
private:
  struct Offline;
  std::unique_ptr<Offline> m_offline;

  std::optional<std::string> m_offlineSessionId;
  std::optional<Json> m_initializeParams;
  std::optional<std::string> m_logLevel;
  std::set<std::string> m_subscriptions;

  /** The tools/list and resources/list results the client knows, if known. */
  std::optional<Json> m_knownTools;
  std::optional<Json> m_knownResources;
  bool m_resourcesStale = false;

public:
  /** The prefix of the ids of the requests that the bridge sends on its own. */
  static constexpr auto HandshakeIdPrefix = "trenchbroom-bridge:";

  /**
   * @param version the server version reported in `serverInfo`, the editor's version
   */
  explicit BridgeSession(std::string version);
  ~BridgeSession();

  /**
   * Creates a PreferenceManager whose preferences all have their default values and are
   * never saved, for a process that has no preferences of its own (the bridge).
   */
  static void createNullPreferenceManager();

  /**
   * Where the given message (a JSON-RPC message or batch) goes.
   *
   * @param editorConnected whether the bridge has a session with the editor
   */
  BridgeRoute route(const Json& message, bool editorConnected) const;

  /**
   * Answers the given message (routed to BridgeRoute::Bridge) and records the client's
   * state: the `initialize` parameters and the log level. Returns the response to send
   * to the client, or nullopt if there is none (notifications).
   */
  std::optional<Json> answer(const Json& message);

  /**
   * Records the client state that a message forwarded to the editor sets: the log
   * level and the resource subscriptions, which the handshake replays when the bridge
   * reconnects.
   */
  void forwarded(const Json& message);

  /** Whether the client has sent `initialize`. */
  bool clientInitialized() const;

  /**
   * The messages that open an editor session for the client, to be sent in order: the
   * client's `initialize` request, `notifications/initialized`, `logging/setLevel` if
   * the client set a level, a `resources/subscribe` for each subscription, then
   * `tools/list` and `resources/list`. The requests have ids with HandshakeIdPrefix.
   * Precondition: clientInitialized().
   */
  std::vector<Json> handshake() const;

  /** Whether the given id is the id of a request of the handshake. */
  static bool isHandshakeId(const Json& id);

  /**
   * Called when the handshake has completed with the editor's `tools/list` and
   * `resources/list` results (null if unknown). Returns the `list_changed`
   * notifications to send to the client for the lists that differ from what the client
   * knows (the bridge's own lists before the first connection).
   */
  std::vector<Json> editorConnected(const Json& editorTools, const Json& editorResources);

  /**
   * Called when the editor session was lost; the bridge answers the list requests again.
   * Returns the `list_changed` notifications to send to the client: always for the
   * resources (the documents of the lost editor are gone), and for the tools if the
   * editor's differ from the bridge's. The next connection reports the resources as
   * changed.
   */
  std::vector<Json> editorDisconnected();

private:
  Json offlineResult(const std::string& method);
};

} // namespace tb::mcp
