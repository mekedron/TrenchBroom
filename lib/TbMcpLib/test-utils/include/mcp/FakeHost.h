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

#include "mcp/ConsoleBuffer.h"
#include "mcp/FakeActionHost.h"
#include "mcp/FakePreferenceHost.h"
#include "mcp/FakeViewHost.h"
#include "mcp/Host.h"
#include "mcp/Snapshot.h"
#include "mdl/CompilationProfile.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameEngineProfile.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kdl
{
class task_manager;
}

namespace tb
{
namespace fs
{
class TestEnvironment;
}
namespace gl
{
class ResourceManager;
}
} // namespace tb

namespace tb::mcp
{
class FakeCompileHost;

/**
 * A compile job whose output and end are scripted by the test. cancel() behaves like the
 * editor's runner: it appends "\n\n#### Terminated\n" and ends the job.
 */
class FakeCompileJob : public CompileJob
{
private:
  FakeCompileHost* m_host;
  CompileJobCallbacks m_callbacks;
  std::string m_log;
  bool m_running = true;

public:
  FakeCompileJob(FakeCompileHost& host, CompileJobCallbacks callbacks);
  ~FakeCompileJob() override;

  std::string log() const override;
  bool running() const override;
  void cancel() override;

  /** Appends output and calls outputChanged. Precondition: running() */
  void append(std::string_view text);
  /** Ends the job and calls ended. Precondition: running() */
  void finish();

private:
  friend class FakeCompileHost;
};

/**
 * Records the compilations started through it and returns FakeCompileJobs that the test
 * drives with append() and finish().
 */
class FakeCompileHost : public CompileHost
{
public:
  struct StartedCompile
  {
    ui::MapDocument* document = nullptr;
    mdl::CompilationProfile profile;
    bool test = false;
    /** The job, or nullptr once it was destroyed. */
    FakeCompileJob* job = nullptr;
  };

  /** Every compilation started so far, in order. */
  std::vector<StartedCompile> started;
  /** If set, startCompile fails with this message. */
  std::optional<std::string> startError;
  /**
   * Called by startCompile after the job was created and before it is returned, e.g. to
   * append output or to finish the job synchronously like a test run does.
   */
  std::function<void(FakeCompileJob&, const StartedCompile&)> onStart;

  ~FakeCompileHost() override;

  Result<std::unique_ptr<CompileJob>> startCompile(
    ui::MapDocument& document,
    const mdl::CompilationProfile& profile,
    bool test,
    CompileJobCallbacks callbacks) override;

  /** The job of the most recent compilation, or nullptr. */
  FakeCompileJob* lastJob();

private:
  friend class FakeCompileJob;
  void jobDestroyed(FakeCompileJob& job);
};

/**
 * Records the engine launches instead of starting processes. engineParameters replaces
 * ${MAP_BASE_NAME} with the base name of the document's map file and keeps everything
 * else, so tests can see that the parameters were interpolated.
 */
class FakeEngineHost : public EngineHost
{
public:
  struct Launch
  {
    ui::MapDocument* document = nullptr;
    mdl::GameEngineProfile profile;
    /** The parameter spec that was passed, or nullopt for the profile's. */
    std::optional<std::string> parameterSpec;
    /** The interpolated parameters. */
    std::string parameters;
    int64_t processId = 0;
  };

  /** Every launch so far, in order. */
  std::vector<Launch> launches;
  /** If set, launchEngine fails with this message. */
  std::optional<std::string> startError;
  /** If set, engineParameters (and therefore launchEngine) fails with this message. */
  std::optional<std::string> parametersError;
  /** The process id of the next launch; incremented by each launch. */
  int64_t nextProcessId = 4242;

  Result<std::string> engineParameters(
    ui::MapDocument& document, const std::string& parameterSpec) override;
  Result<int64_t> launchEngine(
    ui::MapDocument& document,
    const mdl::GameEngineProfile& profile,
    std::optional<std::string> parameterSpec) override;
};

/**
 * A snapshot renderer for tests. It records every request and returns a deterministic
 * image that depends on the request: a background whose color depends on the camera and
 * the options, plus one 4 x 4 pixel block per drawn node (brushes, patches, point
 * entities) at a position derived from the node's bounds, in a color that depends on
 * the node's kind, whether it is highlighted and how many of its faces pass the face
 * filter. Toggling objects therefore changes the image.
 */
class FakeSnapshotRenderer : public SnapshotRenderer
{
public:
  /** What a render request contained; the node pointers must not be dereferenced. */
  struct RecordedRequest
  {
    AgentCamera camera;
    size_t width = 0;
    size_t height = 0;
    SnapshotOptions options;
    std::vector<const mdl::Node*> nodes;
    std::vector<const mdl::Node*> highlighted;
    Color highlightColor;
    std::vector<SnapshotMarker> markers;
    /** For each listed brush: the indices of the faces that pass the face filter. */
    std::vector<std::pair<const mdl::Node*, std::vector<size_t>>> visibleFaces;
    bool hasFaceFilter = false;

    bool contains(const mdl::Node& node) const;
    bool isHighlighted(const mdl::Node& node) const;
    /** The visible faces of the brush, or nullopt if it was not drawn. */
    std::optional<std::vector<size_t>> facesOf(const mdl::Node& node) const;
  };

  /** Every render request so far, in order. */
  std::vector<RecordedRequest> requests;
  /** resourcesPending() returns true this many times, then false. */
  size_t pendingChecks = 0;
  /** The number of resourcesPending() calls. */
  size_t resourceChecks = 0;
  /** If set, render() fails with this message. */
  std::optional<std::string> renderError;
  /** Returned by userViews(). */
  std::vector<UserView> userViewList;
  /** Returned by captureUserView() for a view id; other ids fail. */
  std::vector<std::pair<std::string, RgbaImage>> captures;
  /** The view ids passed to captureUserView(). */
  std::vector<std::string> capturedViews;
  /** If true, encodeJpeg() returns "JPEG" followed by the quality. */
  bool supportsJpeg = false;
  /** Called by render() after the request was recorded, e.g. to cancel the call. */
  std::function<void(const RecordedRequest&)> onRender;

  bool resourcesPending(ui::MapDocument& document) override;
  Result<RgbaImage> render(
    ui::MapDocument& document, const SnapshotRequest& request) override;
  std::vector<UserView> userViews(ui::MapDocument& document) override;
  Result<RgbaImage> captureUserView(
    ui::MapDocument& document, const std::string& viewId) override;
  std::optional<std::string> encodeJpeg(const RgbaImage& image, int quality) override;

  /** The most recent request. Precondition: !requests.empty() */
  const RecordedRequest& last() const;
};

/**
 * A host for tests. Documents are registered explicitly or created through the document
 * host; the busy state and other editor state can be set directly.
 *
 * The game manager knows the games "Test", "Quake", "Quake 2", "Half-Life" and "Quake 3".
 * All but "Test" use the real game configurations from the fixture's games folder. Quake
 * and Quake 2 use the fixture game folders (test/mdl/Game/...) as game paths, Half-Life
 * and Quake 3 use empty folders in the host's temporary directory (see configDir()). The
 * game manager writes the user's configuration files (compilation profiles, engine
 * profiles) to the host's temporary directory, which is removed when the host is
 * destroyed.
 *
 * Like the editor's map windows, the host makes a logger that adds to `console` the
 * target logger of every document it registers (logTarget()), so document messages reach
 * the console buffer. Registered documents must be destroyed before the host.
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

  /** The compile host returned by compileHost(). */
  FakeCompileHost compile;
  /**
   * If set, compileHost() returns this host instead of `compile`, e.g. the editor's real
   * compile host in TbUiLibTest.
   */
  CompileHost* compileHostOverride = nullptr;
  /** If false, compileHost() returns nullptr (a host that cannot compile). */
  bool supportsCompile = true;

  /** The engine host returned by engineHost(). */
  FakeEngineHost engine;
  /** If false, engineHost() returns nullptr (a host that cannot launch engines). */
  bool supportsEngine = true;

  /** The snapshot renderer returned by snapshotRenderer(). */
  FakeSnapshotRenderer snapshot;
  /** If set, snapshotRenderer() returns this renderer instead of `snapshot`. */
  SnapshotRenderer* snapshotRendererOverride = nullptr;
  /** If false, snapshotRenderer() returns nullptr (a host that cannot render). */
  bool supportsSnapshots = true;

  /** The console buffer; the documents' messages are added to it (see logTarget()). */
  ConsoleBuffer console;
  /** If false, consoleBuffer() returns nullptr (a host without a console). */
  bool supportsConsole = true;
  /** The number of clearConsoleViews() calls. */
  size_t clearConsoleViewsCount = 0;

  /**
   * The folder returned by knowledgeDirectory(); by default "mcp-knowledge" in the host's
   * temporary directory. nullopt simulates a host without one.
   */
  std::optional<std::filesystem::path> knowledgeDir;
  /** The view host returned by viewHost(). */
  FakeViewHost view;
  /** If false, viewHost() returns nullptr (a host without views). */
  bool supportsViews = true;
  /** The action host returned by actionHost(). */
  FakeActionHost action;
  /**
   * If set, actionHost() returns this host instead of `action`, e.g. the editor's real
   * action host in TbMcpUiLibTest.
   */
  ActionHost* actionHostOverride = nullptr;
  /** If false, actionHost() returns nullptr (a host without actions). */
  bool supportsActions = true;
  /** The preference host returned by preferenceHost(). */
  FakePreferenceHost preference;
  /** If false, preferenceHost() returns nullptr. */
  bool supportsPreferences = true;
  /** Returned by manualPath(); nullopt simulates a host without a manual. */
  std::optional<std::filesystem::path> manualFile;

private:
  size_t m_nextDocumentId = 1;
  std::unique_ptr<fs::TestEnvironment> m_configEnvironment;
  std::unique_ptr<kdl::task_manager> m_taskManager;
  std::unique_ptr<gl::ResourceManager> m_resourceManager;
  std::unique_ptr<mdl::GameManager> m_gameManager;
  /** Documents created by the document host, including closed ones. */
  std::vector<std::unique_ptr<ui::MapDocument>> m_ownedDocuments;

  class ConsoleLogger;
  /** The loggers that add the documents' messages to the console buffer. */
  std::unordered_map<const ui::MapDocument*, std::unique_ptr<ConsoleLogger>>
    m_consoleLoggers;
  /** Serializes messages that documents log on worker threads. */
  std::mutex m_consoleMutex;

public:
  FakeHost();
  ~FakeHost() override;

  /**
   * The temporary directory that holds the user's game configuration files, one folder
   * per game config folder (e.g. "Quake/CompilationProfiles.cfg"; the files of the
   * "Test" game are at the top level), and the game folders of Half-Life and Quake 3
   * ("games/Halflife", "games/Quake3").
   */
  const std::filesystem::path& configDir() const;

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
  CompileHost* compileHost() override;
  EngineHost* engineHost() override;
  SnapshotRenderer* snapshotRenderer() override;
  /** A logger that adds the messages to `console` with the document and its title. */
  Logger* logTarget(ui::MapDocument& document) override;
  ConsoleBuffer* consoleBuffer() override;
  void clearConsoleViews() override;
  std::optional<std::filesystem::path> knowledgeDirectory() override;
  ViewHost* viewHost() override;
  ActionHost* actionHost() override;
  PreferenceHost* preferenceHost() override;
  std::optional<std::filesystem::path> manualPath() override;

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
  void logToConsole(
    const ui::MapDocument& document, LogLevel level, std::string_view message);
  void processResources();
};

} // namespace tb::mcp
