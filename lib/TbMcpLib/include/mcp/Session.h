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

#include "mcp/AgentCamera.h"
#include "mcp/Image.h"
#include "mcp/Json.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tb::mcp
{
class NotificationStream;

/** A snapshot image kept by an agent (view_snapshot keepAs) for view_snapshot_compare. */
struct KeptSnapshot
{
  std::string name;
  /** The handle of the document it shows. */
  std::string documentId;
  AgentCamera camera;
  size_t width = 0;
  size_t height = 0;
  /** The view arguments of view_snapshot (options, isolate, highlight). */
  Json view = Json::object();
  RgbaImage image;
};

/**
 * The camera of a rendered snapshot image, kept so that view_pick can find what a pixel
 * of the image shows. No pixels are kept.
 */
struct SnapshotRecord
{
  /** "snap:<n>", unique within the session. */
  std::string id;
  /** The handle of the document it shows. */
  std::string documentId;
  AgentCamera camera;
  size_t width = 0;
  size_t height = 0;
  /**
   * The view arguments that decided which objects were drawn (options, isolate,
   * highlight), in the form of view_snapshot.
   */
  Json view = Json::object();
};

/**
 * The state of one connected client. Each `initialize` creates a session.
 */
/** The full lists of a modifying call whose response cut them (result_list_get). */
struct KeptLists
{
  /** "lists:<n>", unique within the session. */
  std::string id;
  /** The tool that made the call. */
  std::string tool;
  /** Path in the response (e.g. "changes.created", "result.ids") -> all items. */
  std::map<std::string, Json> lists;
};

class Session
{
public:
  std::string id;
  std::string protocolVersion;
  std::string clientName;
  std::string clientVersion;
  Json clientCapabilities = Json::object();

  /** Whether the client sent `notifications/initialized`. */
  bool initialized = false;

  /**
   * The handle of the session's active document, which calls without a `document`
   * argument act on: set by document_new, document_open and document_activate, or by
   * the first call that fell back to the focused document. It keeps the handle of a
   * document that was closed by someone else (ServerState::targetDocument).
   */
  std::optional<std::string> activeDocumentId;

  /** Subscribed resource URIs. */
  std::set<std::string> subscriptions;

  /** The standalone notification stream (HTTP GET), if open. */
  std::weak_ptr<NotificationStream> notificationStream;

  std::string logLevel = "info";

  /** The most agent cameras a session can have. */
  static constexpr size_t MaxAgentCameras = 64;
  /** The most snapshots a session keeps; keeping another drops the oldest. */
  static constexpr size_t MaxKeptSnapshots = 8;

  /**
   * The session's named agent cameras (agent_camera_set). They are never shown as the
   * user's camera.
   */
  std::map<std::string, AgentCamera> agentCameras;

  /** The kept snapshots, oldest first. */
  std::vector<KeptSnapshot> keptSnapshots;

  /** The most snapshot cameras a session remembers for view_pick. */
  static constexpr size_t MaxSnapshotRecords = 32;

  /** The cameras of the most recent snapshot images, oldest first. */
  std::vector<SnapshotRecord> snapshotRecords;

  /** The number of the next snapshot id. */
  uint64_t nextSnapshotNumber = 1;

  /** The most calls whose cut lists a session keeps; keeping another drops the oldest. */
  static constexpr size_t MaxKeptLists = 10;

  /** The full lists of the recent calls whose response cut lists, oldest first. */
  std::vector<KeptLists> keptLists;

  /** The number of the next lists id. */
  uint64_t nextListsNumber = 1;

  /**
   * Keeps the full lists of a call, dropping the oldest kept lists if there are
   * MaxKeptLists already. Assigns and returns the id.
   */
  std::string keepLists(KeptLists lists);

  /** The kept lists with the given id, or nullptr if they are unknown or were dropped. */
  const KeptLists* findKeptLists(const std::string& id) const;

  /**
   * Remembers the camera of a rendered image, dropping the oldest record if there are
   * MaxSnapshotRecords already. Assigns and returns the record's id.
   */
  std::string recordSnapshot(SnapshotRecord record);

  /** The record with the given id, or nullptr if it is unknown or was dropped. */
  const SnapshotRecord* findSnapshotRecord(const std::string& id) const;

  /**
   * Sends a notification over the standalone stream if the session is initialized and
   * the stream is open. Returns whether it was sent.
   */
  bool send(const Json& notification) const;

  /** A display name for the client, e.g. "claude-code 1.0". */
  std::string clientDisplayName() const;
};

} // namespace tb::mcp
