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

#include "mcp/McpToolFixture.h"

#include "mcp/JsonRpc.h"
#include "mcp/McpServer.h"
#include "mcp/ObjectIds.h"
#include "mcp/RegisterAll.h"
#include "mcp/ServerState.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include "kd/contracts.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{

void CapturingRequestStream::notify(const Json& notification)
{
  notifications.push_back(notification);
}

void CapturingRequestStream::complete(const Json& response_)
{
  REQUIRE(!response.has_value());
  response = response_;
}

void CapturingNotificationStream::send(const Json& notification)
{
  notifications.push_back(notification);
}

ToolError toolErrorFromJson(const Json& error)
{
  auto result = ToolError{};
  result.code = errorCodeFromString(error.value("code", std::string{}))
                  .value_or(ErrorCode::InternalError);
  result.message = error.value("message", std::string{});
  result.hint = error.value("hint", std::string{});
  if (const auto* objectIds = findMember(error, "objectIds"))
  {
    result.objectIds = objectIds->get<std::vector<std::string>>();
  }
  if (const auto* details = findMember(error, "details"))
  {
    result.details = *details;
  }
  return result;
}

McpToolFixture::McpToolFixture(std::string protocolVersion)
  : m_server{std::make_unique<McpServer>(
      m_host, m_scheduler, ServerInfo{.version = "test-version"})}
{
  registerAll(*m_server);
  m_sessionId = openSession("test-client", protocolVersion);
}

McpToolFixture::~McpToolFixture()
{
  // the server must be destroyed before the documents it observes
  m_server.reset();
}

McpServer& McpToolFixture::server()
{
  return *m_server;
}

FakeHost& McpToolFixture::host()
{
  return m_host;
}

FakeScheduler& McpToolFixture::scheduler()
{
  return m_scheduler;
}

const std::string& McpToolFixture::sessionId() const
{
  return m_sessionId;
}

ui::MapDocument& McpToolFixture::create(mdl::MapFixtureConfig config)
{
  auto& fixture =
    *m_documentFixtures.emplace_back(std::make_unique<ui::MapDocumentFixture>());
  auto& document = fixture.create(std::move(config));
  m_host.addDocument(document, "unnamed" + std::to_string(m_documentFixtures.size()));
  return document;
}

ui::MapDocument& McpToolFixture::load(
  const std::filesystem::path& path, mdl::MapFixtureConfig config)
{
  auto& fixture =
    *m_documentFixtures.emplace_back(std::make_unique<ui::MapDocumentFixture>());
  auto& document = fixture.load(path, std::move(config));
  m_host.addDocument(document, path.filename().string());
  return document;
}

std::string McpToolFixture::documentId(const ui::MapDocument& document) const
{
  for (const auto& info : m_host.documentList)
  {
    if (info.document == &document)
    {
      return info.id;
    }
  }
  contract_assert(false);
  return {};
}

std::string McpToolFixture::openSession(
  const std::string& clientName, const std::string& protocolVersion)
{
  auto stream = std::make_shared<CapturingRequestStream>();
  const auto result = m_server->post(
    std::nullopt,
    dumpJson(jsonrpc::makeRequest(
      m_nextRequestId++,
      "initialize",
      Json{
        {"protocolVersion", protocolVersion},
        {"capabilities", Json::object()},
        {"clientInfo", Json{{"name", clientName}, {"version", "1.0"}}},
      })),
    stream);
  REQUIRE(result.status == PostStatus::Pending);
  REQUIRE(stream->response.has_value());

  const auto accepted = m_server->post(
    result.newSessionId,
    dumpJson(jsonrpc::makeNotification("notifications/initialized")),
    std::make_shared<CapturingRequestStream>());
  REQUIRE(accepted.status == PostStatus::Accepted);

  return result.newSessionId;
}

std::shared_ptr<CapturingRequestStream> McpToolFixture::post(
  const std::string& sessionId, const Json& message)
{
  auto stream = std::make_shared<CapturingRequestStream>();
  m_server->post(sessionId, dumpJson(message), stream);
  return stream;
}

Json McpToolFixture::rpc(const std::string& method, Json params)
{
  auto stream =
    post(m_sessionId, jsonrpc::makeRequest(m_nextRequestId++, method, std::move(params)));
  REQUIRE(stream->response.has_value());
  return *stream->response;
}

Json McpToolFixture::callRawAs(
  const std::string& sessionId, const std::string_view tool, Json arguments)
{
  auto stream = post(
    sessionId,
    jsonrpc::makeRequest(
      m_nextRequestId++,
      "tools/call",
      Json{{"name", tool}, {"arguments", std::move(arguments)}}));

  // asynchronous tools complete in scheduled steps, some of them delayed
  while (!stream->response.has_value()
         && (m_scheduler.runPending() > 0 || m_scheduler.advanceToNextTask()))
  {
  }
  REQUIRE(stream->response.has_value());

  const auto& response = *stream->response;
  INFO(dumpJson(response));
  REQUIRE(response.contains("result"));
  return response["result"];
}

Json McpToolFixture::callRaw(const std::string_view tool, Json arguments)
{
  return callRawAs(m_sessionId, tool, std::move(arguments));
}

Json McpToolFixture::callAs(
  const std::string& sessionId, const std::string_view tool, Json arguments)
{
  const auto result = callRawAs(sessionId, tool, std::move(arguments));
  INFO(dumpJson(result));
  REQUIRE(result["isError"] == false);
  return result["structuredContent"];
}

Json McpToolFixture::call(const std::string_view tool, Json arguments)
{
  return callAs(m_sessionId, tool, std::move(arguments));
}

ToolError McpToolFixture::callExpectingErrorAs(
  const std::string& sessionId, const std::string_view tool, Json arguments)
{
  const auto result = callRawAs(sessionId, tool, std::move(arguments));
  INFO(dumpJson(result));
  REQUIRE(result["isError"] == true);
  REQUIRE(result["structuredContent"]["ok"] == false);
  return toolErrorFromJson(result["structuredContent"]["error"]);
}

ToolError McpToolFixture::callExpectingError(const std::string_view tool, Json arguments)
{
  return callExpectingErrorAs(m_sessionId, tool, std::move(arguments));
}

mdl::Node* McpToolFixture::node(const std::string_view id, ui::MapDocument* document)
{
  auto& doc = document ? *document : focusedDocument();
  auto result = m_server->state().documentState(doc).ids.resolve(id);
  return result.is_success() ? result.value() : nullptr;
}

std::string McpToolFixture::id(const mdl::Node& node, ui::MapDocument* document)
{
  auto& doc = document ? *document : focusedDocument();
  return m_server->state().documentState(doc).ids.format(node);
}

ui::MapDocument& McpToolFixture::focusedDocument()
{
  for (const auto& info : m_host.documentList)
  {
    if (info.focused)
    {
      return *info.document;
    }
  }
  REQUIRE(!m_host.documentList.empty());
  return *m_host.documentList.front().document;
}

} // namespace tb::mcp
