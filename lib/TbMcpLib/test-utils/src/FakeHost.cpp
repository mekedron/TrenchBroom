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
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameManager.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/PatchNode.h"
#include "mdl/TestUtils.h"
#include "ui/MapDocument.h"

#include "kd/task_manager.h"

#include <algorithm>
#include <cassert>
#include <cmath>
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

Result<std::string> FakeEngineHost::engineParameters(
  ui::MapDocument& document, const std::string& parameterSpec)
{
  if (parametersError)
  {
    return Error{*parametersError};
  }

  const auto variable = std::string{"${MAP_BASE_NAME}"};
  const auto baseName = document.map().path().stem().string();
  auto result = parameterSpec;
  for (auto pos = result.find(variable); pos != std::string::npos;
       pos = result.find(variable, pos + baseName.size()))
  {
    result.replace(pos, variable.size(), baseName);
  }
  return result;
}

Result<int64_t> FakeEngineHost::launchEngine(
  ui::MapDocument& document,
  const mdl::GameEngineProfile& profile,
  std::optional<std::string> parameterSpec)
{
  if (startError)
  {
    return Error{*startError};
  }

  return engineParameters(document, parameterSpec.value_or(profile.parameterSpec))
         | kdl::transform([&](auto parameters) {
             const auto processId = nextProcessId++;
             launches.push_back(Launch{
               &document,
               profile,
               std::move(parameterSpec),
               std::move(parameters),
               processId,
             });
             return processId;
           });
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

class FakeHost::ConsoleLogger : public Logger
{
private:
  FakeHost& m_host;
  const ui::MapDocument& m_document;

public:
  ConsoleLogger(FakeHost& host, const ui::MapDocument& document)
    : m_host{host}
    , m_document{document}
  {
  }

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    m_host.logToConsole(m_document, level, message);
  }
};

FakeHost::FakeHost()
  : m_configEnvironment{createConfigEnvironment()}
  , m_taskManager{createTestTaskManager()}
  , m_resourceManager{std::make_unique<gl::ResourceManager>()}
  , m_gameManager{std::make_unique<mdl::GameManager>(
      std::make_unique<fs::WritableDiskFileSystem>(m_configEnvironment->dir()),
      createGameInfos(*m_configEnvironment))}
{
  knowledgeDir = m_configEnvironment->dir() / "mcp-knowledge";
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

std::string FakeHost::addDocument(
  ui::MapDocument& document, std::string title, const bool background)
{
  if (!background)
  {
    for (auto& info : documentList)
    {
      info.focused = false;
    }
  }

  auto id = "doc:" + std::to_string(m_nextDocumentId++);
  documentList.push_back(
    DocumentInfo{id, &document, std::move(title), !background, background});
  // like a map window, which makes its console the document's logger
  document.setTargetLogger(logTarget(document));
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

SnapshotRenderer* FakeHost::snapshotRenderer()
{
  if (snapshotRendererOverride)
  {
    return snapshotRendererOverride;
  }
  return supportsSnapshots ? &snapshot : nullptr;
}

CompileHost* FakeHost::compileHost()
{
  if (compileHostOverride)
  {
    return compileHostOverride;
  }
  return supportsCompile ? &compile : nullptr;
}

EngineHost* FakeHost::engineHost()
{
  return supportsEngine ? &engine : nullptr;
}

Logger* FakeHost::logTarget(ui::MapDocument& document)
{
  auto& logger = m_consoleLoggers[&document];
  if (!logger)
  {
    logger = std::make_unique<ConsoleLogger>(*this, document);
  }
  return logger.get();
}

ConsoleBuffer* FakeHost::consoleBuffer()
{
  return supportsConsole ? &console : nullptr;
}

void FakeHost::clearConsoleViews()
{
  ++clearConsoleViewsCount;
}

std::optional<std::filesystem::path> FakeHost::knowledgeDirectory()
{
  return knowledgeDir;
}

ViewHost* FakeHost::viewHost()
{
  return supportsViews ? &view : nullptr;
}

ActionHost* FakeHost::actionHost()
{
  if (!supportsActions)
  {
    return nullptr;
  }
  return actionHostOverride ? actionHostOverride : &action;
}

PreferenceHost* FakeHost::preferenceHost()
{
  return supportsPreferences ? &preference : nullptr;
}

std::optional<std::filesystem::path> FakeHost::manualPath()
{
  return manualFile;
}

std::optional<DocumentInfo> FakeHost::documentToReplace()
{
  if (!singleWindow)
  {
    return std::nullopt;
  }

  const auto it =
    std::ranges::find_if(documentList, [](const auto& info) { return info.focused; });
  if (it != documentList.end())
  {
    return *it;
  }

  const auto shown =
    std::ranges::find_if(documentList, [](const auto& info) { return !info.background; });
  return shown != documentList.end() ? std::optional{*shown} : std::nullopt;
}

Result<OpenedDocument> FakeHost::createDocument(
  const mdl::GameInfo& gameInfo, const mdl::MapFormat mapFormat, const bool background)
{
  if (auto replaced = background ? std::nullopt : documentToReplace())
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
             addDocument(documentRef, "unnamed.map", background);
             return OpenedDocument{*findDocumentInfo(documentRef), std::move(messages)};
           });
}

Result<OpenedDocument> FakeHost::loadDocument(
  const mdl::GameInfo& gameInfo,
  const mdl::MapFormat mapFormat,
  const std::filesystem::path& path,
  const bool background)
{
  if (auto replaced = background ? std::nullopt : documentToReplace())
  {
    auto capture = ScopedLogCapture{*replaced->document, logTarget(*replaced->document)};
    return replaced->document->load(
             environmentConfig, gameInfo, mapFormat, WorldBounds, path)
           | kdl::transform([&]() {
               processResources();
               addRecentDocument(path);
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
             if (!background)
             {
               addRecentDocument(path);
             }
             addDocument(documentRef, path.filename().string(), background);
             return OpenedDocument{*findDocumentInfo(documentRef), std::move(messages)};
           });
}

Result<void> FakeHost::showDocument(ui::MapDocument& document)
{
  const auto it = std::ranges::find_if(
    documentList, [&](const auto& info) { return info.document == &document; });
  if (it == documentList.end() || !it->background)
  {
    return Error{"The document is not a background document"};
  }
  if (singleWindow && std::ranges::any_of(documentList, [](const auto& info) {
        return !info.background;
      }))
  {
    return Error{"Single window mode: another document has a window"};
  }

  for (auto& info : documentList)
  {
    info.focused = false;
  }
  it->background = false;
  it->focused = true;

  if (const auto& map = document.map(); map.persistent())
  {
    addRecentDocument(map.path());
  }

  // like a new map window, which acts as if the document was loaded to update itself
  document.documentWasLoadedNotifier();
  documentsDidChangeNotifier();
  return kdl::void_success;
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

void FakeHost::addRecentDocument(const std::filesystem::path& path)
{
  std::erase(recentDocumentList, path);
  recentDocumentList.insert(recentDocumentList.begin(), path);
}

void FakeHost::logToConsole(
  const ui::MapDocument& document, const LogLevel level, const std::string_view message)
{
  const auto lock = std::lock_guard{m_consoleMutex};
  const auto info = findDocumentInfo(document);
  console.add(level, message, &document, info ? info->windowTitle : std::string{});
}

void FakeHost::processResources()
{
  auto gl = gl::TestGl{};
  auto processContext = gl::ProcessContext{gl, [](auto, auto) {}};
  gl::processResourcesSync(*m_resourceManager, processContext);
}

// FakeSnapshotRenderer

bool FakeSnapshotRenderer::RecordedRequest::contains(const mdl::Node& node) const
{
  return std::ranges::find(nodes, &node) != nodes.end();
}

bool FakeSnapshotRenderer::RecordedRequest::isHighlighted(const mdl::Node& node) const
{
  return std::ranges::find(highlighted, &node) != highlighted.end();
}

std::optional<std::vector<size_t>> FakeSnapshotRenderer::RecordedRequest::facesOf(
  const mdl::Node& node) const
{
  const auto it = std::ranges::find_if(
    visibleFaces, [&](const auto& entry) { return entry.first == &node; });
  return it != visibleFaces.end() ? std::optional{it->second} : std::nullopt;
}

bool FakeSnapshotRenderer::resourcesPending(ui::MapDocument&)
{
  ++resourceChecks;
  if (pendingChecks > 0)
  {
    --pendingChecks;
    return true;
  }
  return false;
}

Result<RgbaImage> FakeSnapshotRenderer::render(
  ui::MapDocument&, const SnapshotRequest& request)
{
  auto recorded = RecordedRequest{};
  recorded.camera = request.camera;
  recorded.width = request.width;
  recorded.height = request.height;
  recorded.options = request.options;
  recorded.highlightColor = request.scene.highlightColor;
  recorded.markers = request.scene.markers;
  recorded.hasFaceFilter = bool(request.scene.faceFilter);
  for (const auto* node : request.scene.nodes)
  {
    recorded.nodes.push_back(node);
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node))
    {
      auto faces = std::vector<size_t>{};
      const auto& brushFaces = brushNode->brush().faces();
      for (size_t i = 0; i < brushFaces.size(); ++i)
      {
        if (
          !request.scene.faceFilter
          || request.scene.faceFilter(*brushNode, brushFaces[i]))
        {
          faces.push_back(i);
        }
      }
      recorded.visibleFaces.emplace_back(node, std::move(faces));
    }
  }
  for (const auto* node : request.scene.highlighted)
  {
    recorded.highlighted.push_back(node);
  }
  requests.push_back(recorded);
  if (onRender)
  {
    onRender(recorded);
  }

  if (renderError)
  {
    return Error{*renderError};
  }

  // a background that depends on the camera and the options
  const auto& camera = request.camera;
  const auto seed =
    camera.position.x() * 3.0 + camera.position.y() * 5.0 + camera.position.z() * 7.0
    + camera.direction.x() * 11.0 + camera.direction.y() * 13.0
    + camera.direction.z() * 17.0 + camera.zoom * 19.0 + camera.fov * 23.0
    + double(request.options.faceMode) * 29.0 + (request.options.grid ? 31.0 : 0.0);
  const auto shade =
    static_cast<unsigned char>(16 + (static_cast<long long>(std::abs(seed)) % 32));
  auto image = makeImage(request.width, request.height, Rgba8{shade, shade, shade, 255});

  const auto block = [&](const vm::vec3d& center, const Rgba8& color) {
    if (request.width < 4 || request.height < 4)
    {
      return;
    }
    const auto x = size_t(std::abs(static_cast<long long>(center.x() + center.z() * 3.0)))
                   % (request.width - 3);
    const auto y =
      size_t(std::abs(static_cast<long long>(center.y()))) % (request.height - 3);
    for (size_t dy = 0; dy < 4; ++dy)
    {
      for (size_t dx = 0; dx < 4; ++dx)
      {
        auto* p = image.pixels.data() + ((y + dy) * request.width + x + dx) * 4;
        std::copy(color.begin(), color.end(), p);
      }
    }
  };

  for (const auto* node : request.scene.nodes)
  {
    const auto highlighted = recorded.isHighlighted(*node);
    auto color = Rgba8{200, 200, 200, 255};
    if (const auto faces = recorded.facesOf(*node))
    {
      color = Rgba8{200, static_cast<unsigned char>(100 + faces->size() * 10), 60, 255};
    }
    else if (dynamic_cast<const mdl::PatchNode*>(node))
    {
      color = Rgba8{60, 200, 200, 255};
    }
    else if (const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node);
             entityNode && !entityNode->hasChildren())
    {
      color = Rgba8{60, 60, 220, 255};
    }
    else
    {
      // groups and brush entities are drawn through their members
      continue;
    }
    if (highlighted)
    {
      color = Rgba8{255, 128, 0, 255};
    }
    block(node->logicalBounds().center(), color);
  }
  for (const auto& marker : request.scene.markers)
  {
    block(marker.position, Rgba8{255, 255, 0, 255});
  }
  return image;
}

std::vector<UserView> FakeSnapshotRenderer::userViews(ui::MapDocument&)
{
  return userViewList;
}

Result<RgbaImage> FakeSnapshotRenderer::captureUserView(
  ui::MapDocument&, const std::string& viewId)
{
  capturedViews.push_back(viewId);
  const auto it = std::ranges::find_if(
    captures, [&](const auto& capture) { return capture.first == viewId; });
  if (it == captures.end())
  {
    return Error{"no such view: " + viewId};
  }
  return it->second;
}

std::optional<std::string> FakeSnapshotRenderer::encodeJpeg(
  const RgbaImage&, const int quality)
{
  if (!supportsJpeg)
  {
    return std::nullopt;
  }
  return "JPEG" + std::to_string(quality);
}

const FakeSnapshotRenderer::RecordedRequest& FakeSnapshotRenderer::last() const
{
  assert(!requests.empty());
  return requests.back();
}

} // namespace tb::mcp
