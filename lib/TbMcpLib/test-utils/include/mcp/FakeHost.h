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
#include "mdl/CompilationProfile.h"
#include "mdl/EnvironmentConfig.h"

#include <filesystem>
#include <functional>
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

private:
  size_t m_nextDocumentId = 1;
  std::unique_ptr<fs::TestEnvironment> m_configEnvironment;
  std::unique_ptr<kdl::task_manager> m_taskManager;
  std::unique_ptr<gl::ResourceManager> m_resourceManager;
  std::unique_ptr<mdl::GameManager> m_gameManager;
  /** Documents created by the document host, including closed ones. */
  std::vector<std::unique_ptr<ui::MapDocument>> m_ownedDocuments;

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
