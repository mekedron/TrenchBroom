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
#include "fs/DiskFileSystem.h"
#include "fs/DiskIO.h"
#include "fs/FileSystem.h"
#include "fs/TestEnvironment.h"
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
#include <cassert>
#include <stdexcept>

namespace tb::mcp
{
namespace
{

const auto WorldBounds = vm::bbox3d{8192.0};

/**
 * Loads the real game configuration from the fixture's games folder so that builtin
 * entity definitions, tags and flags are available.
 */
Result<mdl::GameInfo> loadGameInfo(
  const std::string& configFolder, const std::filesystem::path& gamePath)
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
           });
}

/** Loads the real game configuration, or throws if it cannot be loaded. */
mdl::GameInfo loadRequiredGameInfo(
  const std::string& configFolder, const std::filesystem::path& gamePath)
{
  return loadGameInfo(configFolder, gamePath)
         | kdl::if_error([](const auto& e) { throw std::runtime_error{e.msg}; })
         | kdl::value();
}

std::vector<mdl::GameInfo> createGameInfos(const fs::TestEnvironment& configEnvironment)
{
  const auto gamesRoot = getFixtureRoot() / "test" / "mdl" / "Game";
  const auto tempGamesRoot = configEnvironment.dir() / "games";
  return {
    mdl::DefaultGameInfo,
    loadGameInfo("Quake", gamesRoot / "Quake") | kdl::value_or(mdl::QuakeGameInfo),
    loadGameInfo("Quake2", gamesRoot / "Quake2") | kdl::value_or(mdl::Quake2GameInfo),
    loadRequiredGameInfo("Halflife", tempGamesRoot / "Halflife"),
    loadRequiredGameInfo("Quake3", tempGamesRoot / "Quake3"),
  };
}

std::unique_ptr<fs::TestEnvironment> createConfigEnvironment()
{
  return std::make_unique<fs::TestEnvironment>([](auto& env) {
    env.createDirectory("games/Halflife");
    env.createDirectory("games/Quake3");
  });
}

} // namespace

FakeCompileJob::FakeCompileJob(FakeCompileHost& host, CompileJobCallbacks callbacks)
  : m_host{&host}
  , m_callbacks{std::move(callbacks)}
{
}

FakeCompileJob::~FakeCompileJob()
{
  if (m_host)
  {
    m_host->jobDestroyed(*this);
  }
}

std::string FakeCompileJob::log() const
{
  return m_log;
}

bool FakeCompileJob::running() const
{
  return m_running;
}

void FakeCompileJob::cancel()
{
  if (m_running)
  {
    append("\n\n#### Terminated\n");
    finish();
  }
}

void FakeCompileJob::append(const std::string_view text)
{
  assert(m_running);
  m_log += text;
  if (m_callbacks.outputChanged)
  {
    m_callbacks.outputChanged();
  }
}

void FakeCompileJob::finish()
{
  assert(m_running);
  m_running = false;
  if (m_callbacks.ended)
  {
    m_callbacks.ended();
  }
}

FakeCompileHost::~FakeCompileHost()
{
  for (auto& started : this->started)
  {
    if (started.job)
    {
      started.job->m_host = nullptr;
    }
  }
}

Result<std::unique_ptr<CompileJob>> FakeCompileHost::startCompile(
  ui::MapDocument& document,
  const mdl::CompilationProfile& profile,
  const bool test,
  CompileJobCallbacks callbacks)
{
  if (startError)
  {
    return Error{*startError};
  }

  auto job = std::make_unique<FakeCompileJob>(*this, std::move(callbacks));
  started.push_back(StartedCompile{&document, profile, test, job.get()});
  if (onStart)
  {
    onStart(*job, started.back());
  }
  return std::unique_ptr<CompileJob>{std::move(job)};
}

FakeCompileJob* FakeCompileHost::lastJob()
{
  return started.empty() ? nullptr : started.back().job;
}

void FakeCompileHost::jobDestroyed(FakeCompileJob& job)
{
  for (auto& started : this->started)
  {
    if (started.job == &job)
    {
      started.job = nullptr;
    }
  }
}

FakeHost::FakeHost()
  : m_configEnvironment{createConfigEnvironment()}
  , m_taskManager{createTestTaskManager()}
  , m_resourceManager{std::make_unique<gl::ResourceManager>()}
  , m_gameManager{std::make_unique<mdl::GameManager>(
      std::make_unique<fs::WritableDiskFileSystem>(m_configEnvironment->dir()),
      createGameInfos(*m_configEnvironment))}
{
}

FakeHost::~FakeHost()
{
  // documents must be destroyed before the resource manager and the game infos
  m_ownedDocuments.clear();
}

const std::filesystem::path& FakeHost::configDir() const
{
  return m_configEnvironment->dir();
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

CompileHost* FakeHost::compileHost()
{
  if (compileHostOverride)
  {
    return compileHostOverride;
  }
  return supportsCompile ? &compile : nullptr;
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
    auto capture = ScopedLogCapture{*replaced->document, logTarget(*replaced->document)};
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
    auto capture = ScopedLogCapture{*replaced->document, logTarget(*replaced->document)};
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
