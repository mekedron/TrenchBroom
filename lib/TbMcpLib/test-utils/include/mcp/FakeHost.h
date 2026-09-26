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

#include "mcp/Host.h"

#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{

/**
 * A host for tests. Documents are registered explicitly; the busy state and other editor
 * state can be set directly.
 */
class FakeHost : public McpHost
{
public:
  std::vector<DocumentInfo> documentList;
  BusyState busy = BusyState::Idle;
  std::vector<std::string> prepareNotes;
  size_t prepareCount = 0;
  std::optional<std::string> toolName;
  bool compileRunning = false;
  std::string version = "test-version";

private:
  size_t m_nextDocumentId = 1;

public:
  /** Registers the document and returns its handle. The new document gets the focus. */
  std::string addDocument(ui::MapDocument& document, std::string title = "unnamed.map");

  /** Fires documentWillClose, removes the document and fires documentsDidChange. */
  void removeDocument(ui::MapDocument& document);

  void setFocused(const std::string& documentId);

  std::string applicationVersion() const override;
  std::vector<DocumentInfo> documents() override;
  BusyState busyState(ui::MapDocument& document) override;
  std::vector<std::string> prepareForAgentEdit(ui::MapDocument& document) override;
  std::optional<std::string> currentToolName(ui::MapDocument& document) override;
  bool isCompileRunning(ui::MapDocument& document) override;
};

} // namespace tb::mcp
