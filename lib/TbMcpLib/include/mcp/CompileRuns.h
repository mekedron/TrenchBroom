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

#include "base/NotifierConnection.h"
#include "base/Result.h"
#include "mdl/CompilationProfile.h"
#include "mdl/CompilationTask.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::ui
{
class MapDocument;
}

namespace tb::mcp
{
class CompileHost;
class CompileJob;
class McpHost;

/** A compilation started by compile_run. */
struct CompileRun
{
  /** 1-based, per server. */
  size_t number = 0;
  /** "run:<n>". */
  std::string id;
  std::string gameName;
  /** The document's handle when the run started. */
  std::string documentId;
  /** The document, or nullptr once it was closed. */
  ui::MapDocument* document = nullptr;
  /** The document's map file when the run started. */
  std::filesystem::path mapPath;
  std::string sessionId;
  mdl::CompilationProfile profile;
  /** The enabled tasks of the profile, in order. */
  std::vector<mdl::CompilationTask> tasks;
  /** The preset the profile was taken from, if any. */
  std::optional<std::string> preset;
  bool test = false;
  std::chrono::system_clock::time_point startedAt;
  std::optional<std::chrono::system_clock::time_point> endedAt;
  /** The job; nullptr once it was destroyed (the log is then in finalLog). */
  std::unique_ptr<CompileJob> job;
  std::string finalLog;
  bool cancelRequested = false;
  bool documentClosed = false;

  ~CompileRun();

  bool running() const;
  /** The output so far. */
  std::string log() const;
};

/** What a new run compiles. */
struct CompileRunSpec
{
  std::string gameName;
  std::string documentId;
  std::string sessionId;
  mdl::CompilationProfile profile;
  std::optional<std::string> preset;
  bool test = false;
};

/**
 * The compile runs of the server. Keeps every running run and the most recent ended
 * ones. Runs of a document that closes are cancelled and keep their log.
 */
class CompileRuns
{
public:
  /** The maximum number of ended runs that are kept. */
  static constexpr size_t MaxEndedRuns = 20;

  /** Called when a run's output changed or the run ended. */
  using DidChange = std::function<void(const CompileRun& run, bool ended)>;

private:
  std::vector<std::unique_ptr<CompileRun>> m_runs;
  size_t m_nextNumber = 1;
  DidChange m_didChange;
  NotifierConnection m_notifierConnection;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

public:
  CompileRuns(McpHost& host, DidChange didChange);
  ~CompileRuns();

  CompileRuns(const CompileRuns&) = delete;
  CompileRuns& operator=(const CompileRuns&) = delete;

  /**
   * Starts a run of the enabled tasks of the given profile. The run may already have
   * ended when this returns (test runs usually end synchronously). Returns the error
   * message of the compile host on failure.
   */
  Result<CompileRun*> start(
    CompileHost& compileHost, ui::MapDocument& document, CompileRunSpec spec);

  /** Requests cancellation. The run may have ended when this returns. */
  void cancel(CompileRun& run);

  /** The run with the given id ("run:<n>"), or nullptr. */
  CompileRun* find(std::string_view id) const;

  /** The most recent run of the given document, or of any document if nullptr. */
  CompileRun* latest(const ui::MapDocument* document) const;

  /** The running run of the given document, or nullptr. */
  CompileRun* running(const ui::MapDocument& document) const;

  /** All known runs, oldest first. */
  std::vector<const CompileRun*> runs() const;

  /** The URI of the log resource of a run, e.g. trenchbroom://compile/run:3/log. */
  static std::string logUri(std::string_view runId);

private:
  CompileRun* findByNumber(size_t number) const;
  void outputChanged(size_t number);
  void jobEnded(size_t number);
  void documentWillClose(ui::MapDocument& document);
  void dropOldRuns();
};

} // namespace tb::mcp
