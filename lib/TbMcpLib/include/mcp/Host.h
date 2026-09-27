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

#include "base/Notifier.h"
#include "base/Result.h"
#include "mcp/LogCapture.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb
{
namespace mdl
{
class GameManager;
struct CompilationProfile;
struct GameInfo;
enum class MapFormat;
} // namespace mdl

namespace ui
{
class MapDocument;
}
} // namespace tb

namespace tb::mcp
{

enum class BusyState
{
  /** The human is not interacting with the document; modifying calls may run. */
  Idle,
  /** The human is dragging, has a modal dialog open, or similar; calls must wait. */
  Busy,
};

struct DocumentInfo
{
  /** The document handle, `doc:<n>`. Stable for as long as the document is open. */
  std::string id;
  ui::MapDocument* document = nullptr;
  std::string windowTitle;
  /** Whether the document's window is the focused (or most recently focused) window. */
  bool focused = false;
};

/** A document that the host created or loaded, with the messages it logged meanwhile. */
struct OpenedDocument
{
  DocumentInfo document;
  /** Warnings and errors logged while the document was created or loaded. */
  std::vector<LogMessage> messages;
};

/**
 * Creates, loads and closes documents without showing dialogs. Implemented by the host.
 */
class DocumentHost
{
public:
  virtual ~DocumentHost();

  /**
   * The document that a new or loaded document replaces instead of opening a new window
   * (single window mode), or nullopt if new documents get their own window.
   */
  virtual std::optional<DocumentInfo> documentToReplace() = 0;

  /**
   * Creates a new document for the given game and format and shows it. The new
   * document's window gets the focus.
   */
  virtual Result<OpenedDocument> createDocument(
    const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat) = 0;

  /**
   * Loads the given map file and shows it. MapFormat::Unknown detects the format. The
   * new document's window gets the focus.
   */
  virtual Result<OpenedDocument> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const std::filesystem::path& path) = 0;

  /**
   * Closes the given document without asking the user, discarding unsaved changes. The
   * host fires documentWillCloseNotifier. The document object must stay alive until
   * control returns to the event loop, because the calling tool may still refer to it.
   */
  virtual void closeDocument(ui::MapDocument& document) = 0;

  /** The recently opened map files, most recent first. */
  virtual std::vector<std::filesystem::path> recentDocuments() = 0;
};

/**
 * A compilation started with CompileHost::startCompile. Destroying a running job
 * terminates it without calling its callbacks.
 */
class CompileJob
{
public:
  virtual ~CompileJob();

  /**
   * The output so far as plain text, as the editor's compilation dialog shows it: the
   * runner's "#### ..." lines and the output of the tools, lines separated by '\n'.
   */
  virtual std::string log() const = 0;

  /** Whether the job is still running. */
  virtual bool running() const = 0;

  /**
   * Terminates the running task; the remaining tasks are skipped. The runner logs
   * "#### Terminated" and the job ends: CompileJobCallbacks::ended is called, possibly
   * before this function returns. Does nothing if the job is not running.
   */
  virtual void cancel() = 0;
};

struct CompileJobCallbacks
{
  /** Called when output was appended to the log; may be called very often. */
  std::function<void()> outputChanged;
  /** Called once when the job ended: all tasks finished, a task failed, or cancelled. */
  std::function<void()> ended;
};

/**
 * Runs compilation profiles with the editor's compilation runner. Implemented by the
 * editor (ui::McpCompileHost); tests use FakeCompileHost.
 */
class CompileHost
{
public:
  virtual ~CompileHost();

  /**
   * Runs the enabled tasks of the given profile for the given document in the background,
   * like the editor's compilation dialog: the working directory and the task specs are
   * interpolated with the editor's compilation variables (MAP_DIR_PATH, GAME_DIR_PATH,
   * MODS, the game's compilation tool names, ...). An export task writes the document's
   * current state, including unsaved changes. In test mode the tasks only log what they
   * would do.
   *
   * Precondition: the profile has at least one enabled task.
   *
   * The callbacks may be called before this function returns (a test run usually ends
   * synchronously). The job is cancelled if the document is reloaded, because the runner
   * refers to the document's map; the caller must destroy the job before the document is
   * destroyed. Fails if the working directory cannot be determined.
   */
  virtual Result<std::unique_ptr<CompileJob>> startCompile(
    ui::MapDocument& document,
    const mdl::CompilationProfile& profile,
    bool test,
    CompileJobCallbacks callbacks) = 0;
};

/**
 * The editor as seen by the MCP server. Implemented by ui::QtMcpHost in the editor and by
 * FakeHost in tests. All functions are called on the thread that owns the server.
 *
 * Further sub-interfaces (actions, views, preferences) are added by
 * the epics that need them.
 */
class McpHost
{
public:
  /**
   * The host must fire this notifier before a document is closed and destroyed, so that
   * the server can roll back agent transactions and drop its per-document state.
   */
  Notifier<ui::MapDocument&> documentWillCloseNotifier;

  /**
   * The host fires this notifier after a document was opened or closed, or when the
   * focused document changed.
   */
  Notifier<> documentsDidChangeNotifier;

  /**
   * The host fires this notifier when the active tool of a document's window changes
   * (see currentToolName).
   */
  Notifier<ui::MapDocument&> currentToolDidChangeNotifier;

  virtual ~McpHost();

  /** The editor version, e.g. "2026.1". */
  virtual std::string applicationVersion() const = 0;

  /** All open documents, in window order. */
  virtual std::vector<DocumentInfo> documents() = 0;

  /**
   * Whether the human is currently busy with the given document: dragging with the mouse,
   * a modal dialog is open, etc. The server additionally treats the document as busy
   * while a transaction that it did not open is active.
   */
  virtual BusyState busyState(ui::MapDocument& document) = 0;

  /**
   * Called before a modifying call runs on the given document while the human is idle.
   * Deactivates modal tools that own the selection (vertex tool, clip tool, ...) as
   * Escape would. Returns notes that are passed on to the agent as warnings, e.g.
   * "deactivated tool: Vertex Tool".
   */
  virtual std::vector<std::string> prepareForAgentEdit(ui::MapDocument& document) = 0;

  /** The name of the active tool of the given document's window, if any. */
  virtual std::optional<std::string> currentToolName(ui::MapDocument& document) = 0;

  /** Whether a compilation is running for the given document. */
  virtual bool isCompileRunning(ui::MapDocument& document) = 0;

  /** Creates, loads and closes documents. */
  virtual DocumentHost& documentHost() = 0;

  /** The configured games. */
  virtual mdl::GameManager& gameManager() = 0;

  /**
   * Runs compilations, or nullptr if the host cannot compile (the compile tools then fail
   * with UNSUPPORTED_IN_HOST). The default implementation returns nullptr.
   */
  virtual CompileHost* compileHost();
};

} // namespace tb::mcp
