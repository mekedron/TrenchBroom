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

#include "mcp/CallContext.h"

#include "mcp/ServerState.h"
#include "ui/MapDocument.h"

#include "kd/contracts.h"

namespace tb::mcp
{

CallContext::CallContext(
  ServerState& server,
  Session& session,
  const ToolDef& tool,
  std::optional<DocumentInfo> document,
  const bool dryRun,
  ProgressFn progress)
  : m_server{server}
  , m_session{session}
  , m_tool{tool}
  , m_document{std::move(document)}
  , m_dryRun{dryRun}
  , m_progress{std::move(progress)}
{
}

ServerState& CallContext::server()
{
  return m_server;
}

McpHost& CallContext::host()
{
  return m_server.host;
}

Session& CallContext::session()
{
  return m_session;
}

const ToolDef& CallContext::tool() const
{
  return m_tool;
}

bool CallContext::hasDocument() const
{
  return m_document.has_value();
}

const DocumentInfo& CallContext::documentInfo() const
{
  contract_pre(hasDocument());
  return *m_document;
}

ui::MapDocument& CallContext::document()
{
  contract_pre(hasDocument());
  return *m_document->document;
}

mdl::Map& CallContext::map()
{
  return document().map();
}

DocumentState& CallContext::documentState()
{
  return m_server.documentState(document());
}

IdRegistry& CallContext::ids()
{
  return documentState().ids;
}

bool CallContext::dryRun() const
{
  return m_dryRun;
}

void CallContext::warn(
  std::string code, std::string message, std::vector<std::string> objectIds)
{
  m_warnings.push_back(
    Warning{std::move(code), std::move(message), std::move(objectIds)});
}

const std::vector<Warning>& CallContext::warnings() const
{
  return m_warnings;
}

void CallContext::progress(
  const double progress_, const std::optional<double> total, const std::string& message)
{
  if (m_progress)
  {
    m_progress(progress_, total, message);
  }
}

void CallContext::setUndoStep(std::string undoStep)
{
  m_undoStep = std::move(undoStep);
}

const std::optional<std::string>& CallContext::undoStep() const
{
  return m_undoStep;
}

void CallContext::setCapturedMessages(const std::vector<std::string>* messages)
{
  m_capturedMessages = messages;
}

ToolError CallContext::operationFailed(std::string message, std::string hint) const
{
  auto error = makeError(ErrorCode::OperationFailed, std::move(message), std::move(hint));
  if (m_capturedMessages && !m_capturedMessages->empty())
  {
    error.details["editorMessages"] = *m_capturedMessages;
    error.message += " Editor said: " + m_capturedMessages->back();
  }
  return error;
}

} // namespace tb::mcp
