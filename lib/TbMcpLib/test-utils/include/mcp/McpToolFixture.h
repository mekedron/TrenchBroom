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

#include "mcp/Endpoint.h"
#include "mcp/Errors.h"
#include "mcp/FakeHost.h"
#include "mcp/FakeScheduler.h"
#include "mcp/Json.h"
#include "mdl/MapFixture.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb
{
namespace mdl
{
class Map;
class Node;
} // namespace mdl

namespace ui
{
class MapDocument;
class MapDocumentFixture;
} // namespace ui

namespace mcp
{
class McpServer;

/** A request stream that records everything it receives. */
class CapturingRequestStream : public RequestStream
{
public:
  std::vector<Json> notifications;
  std::optional<Json> response;

  void notify(const Json& notification) override;
  void complete(const Json& response) override;
};

/** A notification stream that records everything it receives. */
class CapturingNotificationStream : public NotificationStream
{
public:
  std::vector<Json> notifications;

  void send(const Json& notification) override;
};

/**
 * Runs an McpServer with all tools over headless documents (ui::MapDocumentFixture), a
 * FakeHost and a FakeScheduler. The fixture opens one initialized session.
 */
class McpToolFixture
{
private:
  FakeHost m_host;
  FakeScheduler m_scheduler;
  std::unique_ptr<McpServer> m_server;
  std::vector<std::unique_ptr<ui::MapDocumentFixture>> m_documentFixtures;
  std::string m_sessionId;
  int m_nextRequestId = 1;

public:
  explicit McpToolFixture(std::string protocolVersion = "2025-11-25");
  ~McpToolFixture();

  McpServer& server();
  FakeHost& host();
  FakeScheduler& scheduler();
  const std::string& sessionId() const;

  /** Creates a new document and registers it with the host (it gets the focus). */
  ui::MapDocument& create(mdl::MapFixtureConfig config = {});
  ui::MapDocument& load(
    const std::filesystem::path& path, mdl::MapFixtureConfig config = {});

  /** The handle of the given document. */
  std::string documentId(const ui::MapDocument& document) const;

  /** Opens and initializes another session; returns its id. */
  std::string openSession(
    const std::string& clientName = "test-client",
    const std::string& protocolVersion = "2025-11-25");

  /** Posts a raw message on the given session; returns the stream (may be pending). */
  std::shared_ptr<CapturingRequestStream> post(
    const std::string& sessionId, const Json& message);

  /** Sends a JSON-RPC request on the default session and returns the response. */
  Json rpc(const std::string& method, Json params = Json::object());

  /**
   * Calls a tool and returns the full CallToolResult. Runs pending scheduler tasks until
   * an asynchronous tool has completed.
   */
  Json callRaw(std::string_view tool, Json arguments = Json::object());
  Json callRawAs(const std::string& sessionId, std::string_view tool, Json arguments);

  /** Calls a tool, checks that it succeeded, and returns the structured content. */
  Json call(std::string_view tool, Json arguments = Json::object());
  Json callAs(const std::string& sessionId, std::string_view tool, Json arguments);

  /** Calls a tool, checks that it failed, and returns the error. */
  ToolError callExpectingError(std::string_view tool, Json arguments = Json::object());
  ToolError callExpectingErrorAs(
    const std::string& sessionId, std::string_view tool, Json arguments);

  /** Resolves an object id in the given document (or the focused one). */
  mdl::Node* node(std::string_view id, ui::MapDocument* document = nullptr);

  /** The id of the given node. */
  std::string id(const mdl::Node& node, ui::MapDocument* document = nullptr);

private:
  ui::MapDocument& focusedDocument();
};

/** Converts the error object of a failed tool result. */
ToolError toolErrorFromJson(const Json& error);

} // namespace mcp
} // namespace tb
