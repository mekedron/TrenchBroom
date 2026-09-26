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

#include "mcp/FakeHost.h"

#include "TestEnvironment.h"
#include "fs/DiskIO.h"
#include "fs/FileSystem.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/TestUtils.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/TestUtils.h"
#include "ui/MapDocument.h"

#include "kd/task_manager.h"

#include <algorithm>

namespace tb::mcp
{
namespace
{

const auto WorldBounds = vm::bbox3d{8192.0};

/**
 * Loads the real game configuration from the fixture's games folder so that builtin
 * entity definitions, tags and flags are available. Falls back to the given game info.
 */
mdl::GameInfo loadGameInfo(
  const std::string& configFolder,
  const std::filesystem::path& gamePath,
  const mdl::GameInfo& fallback)
{
  const auto configPath = getFixtureRoot() / "games" / configFolder / "GameConfig.cfg";
  return fs::Disk::withInputStream(
           configPath,
           [](auto& stream) {
             return std::string{std::istreambuf_iterator<char>{stream}, {}};
           })
         | kdl::and_then(
           [&](const auto& str) { return mdl::parseGameConfig(str, configPath); })
         | kdl::transform([&](auto gameConfig) {
             return mdl::detail::makeGameInfoFixture(std::move(gameConfig), gamePath);
           })
         | kdl::value_or(fallback);
}

std::vector<mdl::GameInfo> createGameInfos()
{
  const auto gamesRoot = getFixtureRoot() / "test" / "mdl" / "Game";
  return {
    mdl::DefaultGameInfo,
    loadGameInfo("Quake", gamesRoot / "Quake", mdl::QuakeGameInfo),
    loadGameInfo("Quake2", gamesRoot / "Quake2", mdl::Quake2GameInfo),
  };
}

} // namespace

FakeHost::FakeHost()
  : m_taskManager{createTestTaskManager()}
  , m_resourceManager{std::make_unique<gl::ResourceManager>()}
  , m_gameManager{std::make_unique<mdl::GameManager>(nullptr, createGameInfos())}
{
}

FakeHost::~FakeHost()
{
  // documents must be destroyed before the resource manager and the game infos
  m_ownedDocuments.clear();
}

std::string FakeHost::addDocument(ui::MapDocument& document, std::string title)
{
  for (auto& info : documentList)
  {
    info.focused = false;
  }

  auto id = "doc:" + std::to_string(m_nextDocumentId++);
  documentList.push_back(DocumentInfo{id, &document, std::move(title), true});
  documentsDidChangeNotifier();
  return id;
}

void FakeHost::removeDocument(ui::MapDocument& document)
{
  documentWillCloseNotifier(document);
  std::erase_if(
    documentList, [&](const auto& info) { return info.document == &document; });
  documentsDidChangeNotifier();
}

void FakeHost::setFocused(const std::string& documentId)
{
  for (auto& info : documentList)
  {
    info.focused = info.id == documentId;
  }
  documentsDidChangeNotifier();
}

std::string FakeHost::applicationVersion() const
{
  return version;
}

std::vector<DocumentInfo> FakeHost::documents()
{
  return documentList;
}

BusyState FakeHost::busyState(ui::MapDocument&)
{
  return busy;
}

std::vector<std::string> FakeHost::prepareForAgentEdit(ui::MapDocument&)
{
  ++prepareCount;
  return prepareNotes;
}

std::optional<std::string> FakeHost::currentToolName(ui::MapDocument&)
{
  return toolName;
}

bool FakeHost::isCompileRunning(ui::MapDocument&)
{
  return compileRunning;
}

DocumentHost& FakeHost::documentHost()
{
  return *this;
}

mdl::GameManager& FakeHost::gameManager()
{
  return *m_gameManager;
}

std::optional<DocumentInfo> FakeHost::documentToReplace()
{
  if (!singleWindow || documentList.empty())
  {
    return std::nullopt;
  }

  const auto it =
    std::ranges::find_if(documentList, [](const auto& info) { return info.focused; });
  return it != documentList.end() ? *it : documentList.front();
}

Result<OpenedDocument> FakeHost::createDocument(
  const mdl::GameInfo& gameInfo, const mdl::MapFormat mapFormat)
{
  if (auto replaced = documentToReplace())
  {
    auto capture = ScopedLogCapture{*replaced->document};
    return replaced->document->create(environmentConfig, gameInfo, mapFormat, WorldBounds)
           | kdl::transform([&]() {
               processResources();
               documentsDidChangeNotifier();
               return OpenedDocument{
                 *findDocumentInfo(*replaced->document), capture.messages()};
             });
  }

  return ui::MapDocument::createDocument(
           environmentConfig,
           gameInfo,
           mapFormat,
           WorldBounds,
           *m_taskManager,
           *m_resourceManager)
         | kdl::transform([&](auto document) {
             auto messages = collectCachedMessages(*document);
             auto& documentRef = *m_ownedDocuments.emplace_back(std::move(document));
             processResources();
             addDocument(documentRef, "unnamed.map");
             return OpenedDocument{*findDocumentInfo(documentRef), std::move(messages)};
           });
}

Result<OpenedDocument> FakeHost::loadDocument(
  const mdl::GameInfo& gameInfo,
  const mdl::MapFormat mapFormat,
  const std::filesystem::path& path)
{
  const auto addRecent = [&]() {
    std::erase(recentDocumentList, path);
    recentDocumentList.insert(recentDocumentList.begin(), path);
  };

  if (auto replaced = documentToReplace())
  {
    auto capture = ScopedLogCapture{*replaced->document};
    return replaced->document->load(
             environmentConfig, gameInfo, mapFormat, WorldBounds, path)
           | kdl::transform([&]() {
               processResources();
               addRecent();
               documentsDidChangeNotifier();
               return OpenedDocument{
                 *findDocumentInfo(*replaced->document), capture.messages()};
             });
  }

  return ui::MapDocument::loadDocument(
           environmentConfig,
           gameInfo,
           mapFormat,
           WorldBounds,
           path,
           *m_taskManager,
           *m_resourceManager)
         | kdl::transform([&](auto document) {
             auto messages = collectCachedMessages(*document);
             auto& documentRef = *m_ownedDocuments.emplace_back(std::move(document));
             processResources();
             addRecent();
             addDocument(documentRef, path.filename().string());
             return OpenedDocument{*findDocumentInfo(documentRef), std::move(messages)};
           });
}

void FakeHost::closeDocument(ui::MapDocument& document)
{
  removeDocument(document);
}

std::vector<std::filesystem::path> FakeHost::recentDocuments()
{
  return recentDocumentList;
}

std::optional<DocumentInfo> FakeHost::findDocumentInfo(
  const ui::MapDocument& document) const
{
  const auto it = std::ranges::find_if(
    documentList, [&](const auto& info) { return info.document == &document; });
  return it != documentList.end() ? std::optional{*it} : std::nullopt;
}

void FakeHost::processResources()
{
  auto gl = gl::TestGl{};
  auto processContext = gl::ProcessContext{gl, [](auto, auto) {}};
  gl::processResourcesSync(*m_resourceManager, processContext);
}

} // namespace tb::mcp
