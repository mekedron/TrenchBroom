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

#include "ui/QtMcpHost.h"

#include <QApplication>
#include <QEvent>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include "base/Logger.h"
#include "base/NotifierConnection.h"
#include "gl/GlManager.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/Map.h"
#include "ui/AppController.h"
#include "ui/CompilationDialog.h"
#include "ui/GetVersion.h"
#include "ui/MapDocument.h"
#include "ui/MapViewToolBox.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"
#include "ui/McpConsoleHook.h"
#include "ui/RecentDocuments.h"
#include "ui/SwitchableMapViewContainer.h"
#include "ui/SystemPaths.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <ranges>

namespace tb::ui
{
namespace
{

/**
 * Whether the active modal tool keeps state that depends on the current selection and
 * would become stale if the agent changed the map, e.g. vertex handles or clip points.
 */
bool modalToolHoldsSelectionState(const MapViewToolBox& toolBox)
{
  return toolBox.selectionOwnedByTool() || toolBox.anyNodeHandleToolActive()
         || toolBox.clipToolActive();
}

std::string windowTitle(const MapDocument& document)
{
  const auto& map = document.map();
  return fmt::format("{}{} - TrenchBroom", map.filename(), map.modified() ? "*" : "");
}

} // namespace

/**
 * A document without a window, with the logger that adds its messages to the console
 * buffer (instead of a window console).
 */
struct QtMcpHost::BackgroundDocument
{
  class ConsoleLogger : public Logger
  {
  private:
    QtMcpHost& m_host;
    const MapDocument& m_document;

  public:
    ConsoleLogger(QtMcpHost& host, const MapDocument& document)
      : m_host{host}
      , m_document{document}
    {
    }

  private:
    void doLog(const LogLevel level, const std::string_view message) override
    {
      if (QThread::currentThread() == m_host.thread())
      {
        m_host.logBackgroundMessage(&m_document, level, message);
      }
      else
      {
        // the console buffer is not thread-safe; the message is dropped if the host is
        // destroyed before it is delivered
        QMetaObject::invokeMethod(
          &m_host,
          [host = &m_host, document = &m_document, level, text = std::string{message}]() {
            host->logBackgroundMessage(document, level, text);
          },
          Qt::QueuedConnection);
      }
    }
  };

  std::unique_ptr<MapDocument> document;
  std::unique_ptr<ConsoleLogger> logger;
  NotifierConnection notifierConnection;

  ~BackgroundDocument()
  {
    notifierConnection.disconnect();
    if (document)
    {
      // like a map window, which triggers a final autosave before it releases its
      // document
      document->triggerAutosave();
      document->setTargetLogger(nullptr);
      document.reset();
    }
  }
};

std::optional<std::string> activeModalToolName(const MapViewToolBox& toolBox)
{
  if (toolBox.clipToolActive())
  {
    return "Clip Tool";
  }
  if (toolBox.assembleBrushToolActive())
  {
    return "Assemble Brush Tool";
  }
  if (toolBox.rotateToolActive())
  {
    return "Rotate Tool";
  }
  if (toolBox.sweepToolActive())
  {
    return "Sweep Tool";
  }
  if (toolBox.scaleToolActive())
  {
    return "Scale Tool";
  }
  if (toolBox.shearToolActive())
  {
    return "Shear Tool";
  }
  if (toolBox.vertexToolActive())
  {
    return "Vertex Tool";
  }
  if (toolBox.edgeToolActive())
  {
    return "Edge Tool";
  }
  if (toolBox.faceToolActive())
  {
    return "Face Tool";
  }
  if (toolBox.controlPointToolActive())
  {
    return "Control Point Tool";
  }
  return std::nullopt;
}

QtMcpHost::QtMcpHost(AppController& appController, QObject* parent)
  : QObject{parent}
  , m_appController{appController}
  , m_compileHost{[this](const MapDocument& document) {
    const auto* mapWindow = findMapWindow(document);
    return mapWindow ? copyPerspectiveCamera(mapWindow->mapView().perspectiveCamera())
                     : nullptr;
  }}
  , m_viewHost{[this](const MapDocument& document) { return findMapWindow(document); }}
  , m_actionHost{
      appController,
      [this](const MapDocument& document) { return findMapWindow(document); }}
  , m_preferenceHost{appController}
{
  qApp->installEventFilter(this);

  // The map window manager connected to this signal before, so it has already updated
  // the window order when this host is notified
  connect(qApp, &QApplication::focusChanged, this, &QtMcpHost::focusDidChange);

  assignDocumentIds();
  connectMapWindows();
  m_topMapWindow = m_appController.mapWindowManager().topMapWindow();

  // like MapWindow's autosave timer
  m_autosaveTimer = new QTimer{this};
  connect(
    m_autosaveTimer, &QTimer::timeout, this, &QtMcpHost::autosaveBackgroundDocuments);
  m_autosaveTimer->start(std::chrono::milliseconds{1000});
}

QtMcpHost::~QtMcpHost()
{
  // Background documents are discarded; they must be destroyed while the application
  // controller and its GL and resource managers still exist
  for (const auto& backgroundDocument : m_backgroundDocuments)
  {
    if (const auto& map = backgroundDocument->document->map(); map.modified())
    {
      if (auto* topWindow = m_appController.mapWindowManager().topMapWindow())
      {
        topWindow->logger().warn() << fmt::format(
          "Discarded the unsaved changes of the agent's background document {}",
          map.filename());
      }
    }
  }
  m_backgroundDocuments.clear();
  m_closedBackgroundDocuments.clear();
}

MapWindow* QtMcpHost::findMapWindow(const MapDocument& document) const
{
  const auto mapWindows = m_appController.mapWindowManager().mapWindows();
  const auto it = std::ranges::find_if(mapWindows, [&](const auto* mapWindow) {
    return &mapWindow->document() == &document;
  });
  return it != mapWindows.end() ? *it : nullptr;
}

void QtMcpHost::setConsoleHook(McpConsoleHook* consoleHook)
{
  m_consoleHook = consoleHook;
}

void QtMcpHost::setCreateMapWindow(CreateMapWindow createMapWindow)
{
  m_createMapWindow = std::move(createMapWindow);
}

std::string QtMcpHost::applicationVersion() const
{
  return getBuildVersion().toStdString();
}

std::vector<mcp::DocumentInfo> QtMcpHost::documents()
{
  assignDocumentIds();

  const auto mapWindows = m_appController.mapWindowManager().mapWindows();
  auto* focusedWindow = mapWindows.empty() ? nullptr : mapWindows.front();

  auto result = std::vector<std::pair<size_t, mcp::DocumentInfo>>{};
  for (auto* mapWindow : mapWindows)
  {
    auto& document = mapWindow->document();
    const auto id = documentId(document);
    result.emplace_back(
      id,
      mcp::DocumentInfo{
        .id = fmt::format("doc:{}", id),
        .document = &document,
        .windowTitle = windowTitle(document),
        .focused = mapWindow == focusedWindow,
      });
  }

  const auto addBackgroundDocument = [&](MapDocument& document) {
    const auto id = documentId(document);
    result.emplace_back(
      id,
      mcp::DocumentInfo{
        .id = fmt::format("doc:{}", id),
        .document = &document,
        .windowTitle = windowTitle(document),
        .focused = false,
        .background = true,
      });
  };
  for (const auto& backgroundDocument : m_backgroundDocuments)
  {
    addBackgroundDocument(*backgroundDocument->document);
  }
  // while its new window is being created, the document is in neither list
  if (
    m_documentBeingShown && std::ranges::none_of(mapWindows, [&](const auto* mapWindow) {
      return &mapWindow->document() == m_documentBeingShown;
    }))
  {
    addBackgroundDocument(*m_documentBeingShown);
  }

  // List the documents in the order in which they were opened
  std::ranges::sort(result, {}, [](const auto& pair) { return pair.first; });

  return result | std::views::values | kdl::ranges::to<std::vector>();
}

mcp::BusyState QtMcpHost::busyState(MapDocument& document)
{
  // the human cannot interact with a background document
  if (findBackgroundDocument(document))
  {
    return mcp::BusyState::Idle;
  }

  if (QApplication::activeModalWidget() != nullptr)
  {
    return mcp::BusyState::Busy;
  }

  if (const auto* mapWindow = findMapWindow(document);
      mapWindow && mapWindow->toolBox().dragging())
  {
    return mcp::BusyState::Busy;
  }

  return mcp::BusyState::Idle;
}

std::vector<std::string> QtMcpHost::prepareForAgentEdit(MapDocument& document)
{
  auto notes = std::vector<std::string>{};

  if (auto* mapWindow = findMapWindow(document))
  {
    auto& toolBox = mapWindow->toolBox();
    while (modalToolHoldsSelectionState(toolBox))
    {
      const auto toolName = activeModalToolName(toolBox).value_or("modal tool");
      if (!toolBox.deactivateCurrentTool())
      {
        break;
      }
      notes.push_back(fmt::format("deactivated tool: {}", toolName));
    }
  }

  return notes;
}

std::optional<std::string> QtMcpHost::currentToolName(MapDocument& document)
{
  if (const auto* mapWindow = findMapWindow(document))
  {
    return activeModalToolName(mapWindow->toolBox());
  }
  return std::nullopt;
}

bool QtMcpHost::isCompileRunning(MapDocument& document)
{
  const auto* mapWindow = findMapWindow(document);
  const auto* dialog = mapWindow ? mapWindow->compilationDialog() : nullptr;
  return dialog && dialog->running();
}

mcp::DocumentHost& QtMcpHost::documentHost()
{
  return *this;
}

mdl::GameManager& QtMcpHost::gameManager()
{
  return m_appController.gameManager();
}

mcp::CompileHost* QtMcpHost::compileHost()
{
  return &m_compileHost;
}

mcp::EngineHost* QtMcpHost::engineHost()
{
  return &m_engineHost;
}

Logger* QtMcpHost::logTarget(MapDocument& document)
{
  if (const auto* backgroundDocument = findBackgroundDocument(document))
  {
    return backgroundDocument->logger.get();
  }

  const auto* mapWindow = findMapWindow(document);
  return mapWindow ? &mapWindow->logger() : nullptr;
}

mcp::SnapshotRenderer* QtMcpHost::snapshotRenderer()
{
  if (!m_snapshotRenderer)
  {
    m_snapshotRenderer = std::make_unique<McpSnapshotRenderer>(
      m_appController.glManager(),
      [this](const MapDocument& document) { return findMapWindow(document); });
  }
  return m_snapshotRenderer.get();
}

mcp::ConsoleBuffer* QtMcpHost::consoleBuffer()
{
  return m_consoleHook ? &m_consoleHook->buffer() : nullptr;
}

void QtMcpHost::clearConsoleViews()
{
  if (m_consoleHook)
  {
    m_consoleHook->clearConsoleViews();
  }
}

std::optional<std::filesystem::path> QtMcpHost::knowledgeDirectory()
{
  return m_appController.environmentConfig().userDataFolderPath / "mcp-knowledge";
}

mcp::ViewHost* QtMcpHost::viewHost()
{
  return &m_viewHost;
}

mcp::ActionHost* QtMcpHost::actionHost()
{
  return &m_actionHost;
}

mcp::PreferenceHost* QtMcpHost::preferenceHost()
{
  return &m_preferenceHost;
}

std::optional<std::filesystem::path> QtMcpHost::manualPath()
{
  if (auto path = SystemPaths::findResourceFile("manual/index.html");
      !path.empty() && std::filesystem::exists(path))
  {
    return path;
  }
  return std::nullopt;
}

std::optional<mcp::DocumentInfo> QtMcpHost::documentToReplace()
{
  const auto& mapWindowManager = m_appController.mapWindowManager();
  if (mapWindowManager.shouldCreateWindowForDocument())
  {
    return std::nullopt;
  }

  const auto* mapWindow = mapWindowManager.topMapWindow();
  return mapWindow ? std::optional{documentInfo(mapWindow->document())} : std::nullopt;
}

Result<mcp::OpenedDocument> QtMcpHost::createDocument(
  const mdl::GameInfo& gameInfo, const mdl::MapFormat mapFormat, const bool background)
{
  auto& mapWindowManager = m_appController.mapWindowManager();
  if (const auto replaced = background ? std::nullopt : documentToReplace())
  {
    // single window mode: the document is recreated in place, logging to its console
    const auto capture =
      mcp::ScopedLogCapture{*replaced->document, logTarget(*replaced->document)};
    return mapWindowManager.createDocument(
             gameInfo, mapFormat, MapDocument::DefaultWorldBounds)
           | kdl::transform([&]() {
               return mcp::OpenedDocument{
                 documentInfo(*replaced->document), capture.messages()};
             });
  }

  return MapDocument::createDocument(
           m_appController.environmentConfig(),
           gameInfo,
           mapFormat,
           MapDocument::DefaultWorldBounds,
           m_appController.taskManager(),
           m_appController.glManager().resourceManager())
         | kdl::transform([&](auto document) {
             // the new document caches its messages until its window's console shows
             // them
             auto messages = mcp::collectCachedMessages(*document);
             if (background)
             {
               return addBackgroundDocument(std::move(document), std::move(messages));
             }
             auto* mapWindow = mapWindowManager.createMapWindow(std::move(document));
             return mcp::OpenedDocument{
               documentInfo(mapWindow->document()), std::move(messages)};
           });
}

Result<mcp::OpenedDocument> QtMcpHost::loadDocument(
  const mdl::GameInfo& gameInfo,
  const mdl::MapFormat mapFormat,
  const std::filesystem::path& path,
  const bool background)
{
  auto& mapWindowManager = m_appController.mapWindowManager();
  if (const auto replaced = background ? std::nullopt : documentToReplace())
  {
    const auto capture =
      mcp::ScopedLogCapture{*replaced->document, logTarget(*replaced->document)};
    return mapWindowManager.loadDocument(
             gameInfo, mapFormat, MapDocument::DefaultWorldBounds, path)
           | kdl::transform([&]() {
               return mcp::OpenedDocument{
                 documentInfo(*replaced->document), capture.messages()};
             });
  }

  return MapDocument::loadDocument(
           m_appController.environmentConfig(),
           gameInfo,
           mapFormat,
           MapDocument::DefaultWorldBounds,
           path,
           m_appController.taskManager(),
           m_appController.glManager().resourceManager())
         | kdl::transform([&](auto document) {
             auto messages = mcp::collectCachedMessages(*document);
             if (background)
             {
               return addBackgroundDocument(std::move(document), std::move(messages));
             }
             auto* mapWindow = mapWindowManager.createMapWindow(std::move(document));
             return mcp::OpenedDocument{
               documentInfo(mapWindow->document()), std::move(messages)};
           });
}

Result<void> QtMcpHost::showDocument(MapDocument& document)
{
  const auto it = std::ranges::find_if(m_backgroundDocuments, [&](const auto& entry) {
    return entry->document.get() == &document;
  });
  if (it == m_backgroundDocuments.end())
  {
    return Error{"The document is not a background document"};
  }

  auto& mapWindowManager = m_appController.mapWindowManager();
  if (!mapWindowManager.shouldCreateWindowForDocument())
  {
    return Error{"The editor shows one document at a time (single-window mode)"};
  }

  auto backgroundDocument = std::move(*it);
  m_backgroundDocuments.erase(it);
  backgroundDocument->notifierConnection.disconnect();

  // the window's console becomes the document's logger
  auto ownedDocument = std::move(backgroundDocument->document);
  ownedDocument->setTargetLogger(nullptr);
  backgroundDocument.reset();

  m_documentBeingShown = &document;
  auto* mapWindow = m_createMapWindow
                      ? m_createMapWindow(std::move(ownedDocument))
                      : mapWindowManager.createMapWindow(std::move(ownedDocument));
  m_documentBeingShown = nullptr;

  if (!m_mapWindowConnections.contains(mapWindow))
  {
    // the window's show event was not seen
    mapWindowsDidChange();
  }
  return kdl::void_success;
}

void QtMcpHost::closeDocument(MapDocument& document)
{
  if (auto* mapWindow = findMapWindow(document))
  {
    // the window is deleted later, when control returns to the event loop
    mapWindow->closeDiscardingChanges();
    return;
  }

  const auto it = std::ranges::find_if(m_backgroundDocuments, [&](const auto& entry) {
    return entry->document.get() == &document;
  });
  if (it != m_backgroundDocuments.end())
  {
    // like a closed map window: no longer listed now, destroyed later
    m_closedBackgroundDocuments.push_back(std::move(*it));
    m_backgroundDocuments.erase(it);
    QTimer::singleShot(0, this, &QtMcpHost::destroyClosedBackgroundDocuments);
  }
}

std::vector<std::filesystem::path> QtMcpHost::recentDocuments()
{
  return m_appController.recentDocuments().recentDocuments();
}

QtMcpHost::BackgroundDocument* QtMcpHost::findBackgroundDocument(
  const MapDocument& document) const
{
  const auto it = std::ranges::find_if(m_backgroundDocuments, [&](const auto& entry) {
    return entry->document.get() == &document;
  });
  return it != m_backgroundDocuments.end() ? it->get() : nullptr;
}

mcp::OpenedDocument QtMcpHost::addBackgroundDocument(
  std::unique_ptr<MapDocument> document, std::vector<mcp::LogMessage> messages)
{
  auto& documentRef = *document;
  auto backgroundDocument = std::make_unique<BackgroundDocument>();
  backgroundDocument->logger =
    std::make_unique<BackgroundDocument::ConsoleLogger>(*this, documentRef);
  backgroundDocument->document = std::move(document);

  // a document created or loaded in place (document_revert) changes the title
  backgroundDocument->notifierConnection += documentRef.documentWasLoadedNotifier.connect(
    [this]() { documentsDidChangeNotifier(); });

  m_backgroundDocuments.push_back(std::move(backgroundDocument));
  documentId(documentRef);

  // the cached messages go to the console buffer like a window console's
  documentRef.setTargetLogger(m_backgroundDocuments.back()->logger.get());

  documentsDidChangeNotifier();
  return mcp::OpenedDocument{documentInfo(documentRef), std::move(messages)};
}

void QtMcpHost::destroyClosedBackgroundDocuments()
{
  auto closed = std::move(m_closedBackgroundDocuments);
  m_closedBackgroundDocuments.clear();
  if (closed.empty())
  {
    return;
  }

  for (const auto& backgroundDocument : closed)
  {
    documentWillCloseNotifier(*backgroundDocument->document);
    m_documentIds.erase(backgroundDocument->document.get());
  }
  closed.clear();
  documentsDidChangeNotifier();
}

void QtMcpHost::logBackgroundMessage(
  const MapDocument* document, const LogLevel level, const std::string_view message)
{
  if (!m_consoleHook)
  {
    return;
  }

  // the document may have been closed if the message was logged on another thread
  const auto known =
    std::ranges::any_of(
      m_backgroundDocuments,
      [&](const auto& entry) { return entry->document.get() == document; })
    || std::ranges::any_of(m_closedBackgroundDocuments, [&](const auto& entry) {
         return entry->document.get() == document;
       });
  auto documentName = known ? document->map().path().filename().string() : std::string{};
  m_consoleHook->buffer().add(level, message, document, std::move(documentName));
}

void QtMcpHost::autosaveBackgroundDocuments()
{
  for (const auto& backgroundDocument : m_backgroundDocuments)
  {
    backgroundDocument->document->triggerAutosave();
  }
}

mcp::DocumentInfo QtMcpHost::documentInfo(const MapDocument& document)
{
  for (auto& info : documents())
  {
    if (info.document == &document)
    {
      return info;
    }
  }
  contract_assert(false);
  return {};
}

void QtMcpHost::assignDocumentIds()
{
  const auto mapWindows = m_appController.mapWindowManager().mapWindows();

  // Forget documents that are no longer open. Their ids are never reused.
  std::erase_if(m_documentIds, [&](const auto& entry) {
    return entry.first != m_documentBeingShown && !findBackgroundDocument(*entry.first)
           && std::ranges::none_of(mapWindows, [&](const auto* mapWindow) {
                return &mapWindow->document() == entry.first;
              });
  });

  // The most recently focused window comes first, so older windows receive their ids
  // first. New windows are always focused when they open.
  for (const auto* mapWindow : mapWindows | std::views::reverse)
  {
    documentId(mapWindow->document());
  }
}

size_t QtMcpHost::documentId(const MapDocument& document)
{
  if (const auto it = m_documentIds.find(&document); it != m_documentIds.end())
  {
    return it->second;
  }
  return m_documentIds.emplace(&document, m_nextDocumentId++).first->second;
}

bool QtMcpHost::eventFilter(QObject* watched, QEvent* event)
{
  switch (event->type())
  {
  case QEvent::Show:
    // MapWindowManager::createMapWindow registers a new window before showing it
    if (auto* mapWindow = qobject_cast<MapWindow*>(watched);
        mapWindow && !m_mapWindowConnections.contains(mapWindow))
    {
      mapWindowsDidChange();
    }
    break;
  case QEvent::DeferredDelete:
    // A closed map window is deleted later (Qt::WA_DeleteOnClose); its document is
    // destroyed with it
    if (auto* mapWindow = qobject_cast<MapWindow*>(watched))
    {
      mapWindowWillBeDeleted(*mapWindow);
    }
    break;
  default:
    break;
  }
  return QObject::eventFilter(watched, event);
}

void QtMcpHost::mapWindowWillBeDeleted(MapWindow& mapWindow)
{
  auto& document = mapWindow.document();
  documentWillCloseNotifier(document);
  m_documentIds.erase(&document);
  m_mapWindowConnections.erase(&mapWindow);
  mapWindowsDidChange();
}

void QtMcpHost::focusDidChange()
{
  if (m_appController.mapWindowManager().topMapWindow() != m_topMapWindow)
  {
    mapWindowsDidChange();
  }
}

void QtMcpHost::mapWindowsDidChange()
{
  assignDocumentIds();
  connectMapWindows();
  m_topMapWindow = m_appController.mapWindowManager().topMapWindow();
  documentsDidChangeNotifier();
}

void QtMcpHost::connectMapWindows()
{
  for (auto* mapWindow : m_appController.mapWindowManager().mapWindows())
  {
    if (!m_mapWindowConnections.contains(mapWindow))
    {
      // the window's document can be replaced (single window mode), so look it up
      // when the tool changes
      const auto toolDidChange = [this, mapWindow](auto&) {
        currentToolDidChangeNotifier(mapWindow->document());
      };

      auto& toolBox = mapWindow->toolBox();
      auto connection = NotifierConnection{};
      connection += toolBox.toolActivatedNotifier.connect(toolDidChange);
      connection += toolBox.toolDeactivatedNotifier.connect(toolDidChange);

      // a document created or loaded in place (single window mode) changes the title
      connection += mapWindow->document().documentWasLoadedNotifier.connect(
        [this]() { documentsDidChangeNotifier(); });

      m_mapWindowConnections.emplace(mapWindow, std::move(connection));
    }
  }
}

} // namespace tb::ui
