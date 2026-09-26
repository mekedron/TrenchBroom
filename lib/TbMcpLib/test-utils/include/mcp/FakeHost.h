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
#include "mdl/EnvironmentConfig.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb
{
namespace gl
{
class ResourceManager;
}
} // namespace tb

namespace tb::mcp
{

/**
 * A host for tests. Documents are registered explicitly or created through the document
 * host; the busy state and other editor state can be set directly.
 *
 * The game manager knows the games "Test", "Quake" and "Quake 2". Quake and Quake 2 use
 * the real game configurations from the fixture's games folder and the fixture game
 * folders (test/mdl/Game/...) as game paths.
 */
class FakeHost : public McpHost, public DocumentHost
{
public:
  std::vector<DocumentInfo> documentList;
  BusyState busy = BusyState::Idle;
  std::vector<std::string> prepareNotes;
  size_t prepareCount = 0;
  std::optional<std::string> toolName;
  bool compileRunning = false;
  std::string version = "test-version";

  /** Simulates single window mode: new documents replace the focused document. */
  bool singleWindow = false;
  std::vector<std::filesystem::path> recentDocumentList;
  mdl::EnvironmentConfig environmentConfig;

private:
  size_t m_nextDocumentId = 1;
  std::unique_ptr<kdl::task_manager> m_taskManager;
  std::unique_ptr<gl::ResourceManager> m_resourceManager;
  std::unique_ptr<mdl::GameManager> m_gameManager;
  /** Documents created by the document host, including closed ones. */
  std::vector<std::unique_ptr<ui::MapDocument>> m_ownedDocuments;

public:
  FakeHost();
  ~FakeHost() override;

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
  DocumentHost& documentHost() override;
  mdl::GameManager& gameManager() override;

  // DocumentHost
  std::optional<DocumentInfo> documentToReplace() override;
  Result<OpenedDocument> createDocument(
    const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat) override;
  Result<OpenedDocument> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const std::filesystem::path& path) override;
  /** Removes the document like removeDocument; the document object stays alive. */
  void closeDocument(ui::MapDocument& document) override;
  std::vector<std::filesystem::path> recentDocuments() override;

private:
  std::optional<DocumentInfo> findDocumentInfo(const ui::MapDocument& document) const;
  void processResources();
};

} // namespace tb::mcp
