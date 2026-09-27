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

#include "base/Color.h"
#include "base/KeySequence.h"
#include "base/Notifier.h"
#include "base/Preference.h"
#include "base/Result.h"
#include "mcp/LogCapture.h"
#include "mcp/Snapshot.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tb
{
namespace mdl
{
class GameManager;
struct CompilationProfile;
struct GameEngineProfile;
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
class ConsoleBuffer;
class SnapshotRenderer;

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
  /**
   * Whether the document is a background document: the host owns it without a map
   * window (DocumentHost::createDocument / loadDocument with background = true) until
   * DocumentHost::showDocument gives it one. A background document is never focused.
   */
  bool background = false;
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
   * (single window mode), or nullopt if new documents get their own window. Never a
   * background document.
   */
  virtual std::optional<DocumentInfo> documentToReplace() = 0;

  /**
   * Creates a new document for the given game and format and shows it. The new
   * document's window gets the focus.
   *
   * If background is true, the host keeps the new document without a window instead: it
   * never replaces another document (documentToReplace does not apply), does not take
   * the focus, and is listed with DocumentInfo::background until showDocument gives it a
   * window.
   */
  virtual Result<OpenedDocument> createDocument(
    const mdl::GameInfo& gameInfo, mdl::MapFormat mapFormat, bool background) = 0;

  /**
   * Loads the given map file and shows it. MapFormat::Unknown detects the format. The
   * new document's window gets the focus. With background = true, the document has no
   * window (see createDocument).
   */
  virtual Result<OpenedDocument> loadDocument(
    const mdl::GameInfo& gameInfo,
    mdl::MapFormat mapFormat,
    const std::filesystem::path& path,
    bool background) = 0;

  /**
   * Gives a background document its own map window, which gets the focus. The document
   * keeps its handle, its undo history and its unsaved changes; documentWillCloseNotifier
   * is not fired. The window may fire the document's documentWasLoadedNotifier to update
   * itself, without replacing the document's map. Fails if the document is not a
   * background document or if the host cannot open another window (single window mode
   * while a window is open).
   */
  virtual Result<void> showDocument(ui::MapDocument& document) = 0;

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
 * Launches game engines like the editor's Launch Engine dialog. Implemented by the editor
 * (ui::McpEngineHost); tests use FakeEngineHost.
 */
class EngineHost
{
public:
  virtual ~EngineHost();

  /**
   * Interpolates an engine parameter spec with the editor's launch variables of the
   * given document (MAP_BASE_NAME, GAME_DIR_PATH, MODS, the game's compilation tool
   * names, ...), as the Launch Engine dialog does. Fails if the spec is malformed or
   * refers to an unknown variable.
   */
  virtual Result<std::string> engineParameters(
    ui::MapDocument& document, const std::string& parameterSpec) = 0;

  /**
   * Starts the engine of the given profile detached from the editor, in the folder of
   * the engine executable, with the given parameter spec (default: the profile's)
   * interpolated like engineParameters does. Returns the id of the started process.
   */
  virtual Result<int64_t> launchEngine(
    ui::MapDocument& document,
    const mdl::GameEngineProfile& profile,
    std::optional<std::string> parameterSpec) = 0;
};

/**
 * The layout of the views of a document's map window. The number of panes is the global
 * "Views/Map view layout" preference; the maximized and the current view belong to the
 * window.
 */
struct ViewLayout
{
  /** The id of the maximized view ("3d", "xy", "xz" or "yz"), or nullopt. */
  std::optional<std::string> maximizedView;
  /** The id of the view that has (or last had) the focus, or nullopt. */
  std::optional<std::string> currentView;
};

/**
 * The user's editor views of a document: their cameras and the maximized view. Changing
 * them changes what the user sees. Implemented by ui::McpViewHost in the editor and by
 * FakeViewHost in tests.
 */
class ViewHost
{
public:
  virtual ~ViewHost();

  /**
   * The views of the document's window ("3d", then "xy", "xz", "yz"), each once, with
   * its current camera, size and whether it is shown in the current layout. Empty if the
   * document has no window.
   */
  virtual std::vector<UserView> views(ui::MapDocument& document) = 0;

  /**
   * Sets the camera of the given view without animation. For a perspective view the
   * position, direction and up vector are used, and the field of view if it differs from
   * the view's current (zoomed) one; it then lasts until the "Field of vision"
   * preference changes or the views are recreated. For an orthographic view the
   * position (its component along the view axis is ignored) and the zoom (0.02 to 100)
   * are used; the direction of a 2D view is fixed. With "Link 2D cameras", the other 2D
   * views follow. Fails if the document has no window, the view does not exist or the
   * zoom is out of range.
   */
  virtual Result<void> setCamera(
    ui::MapDocument& document, const std::string& viewId, const AgentCamera& camera) = 0;

  /** The maximized and the current view. Fails if the document has no window. */
  virtual Result<ViewLayout> layout(ui::MapDocument& document) = 0;

  /**
   * Maximizes the given view, or restores all views if nullopt. The view becomes the
   * current view; if it shares a pane with other views (the cycling 2D pane of the two-
   * and three-pane layouts), the pane is cycled to it first, like the Cycle Map View
   * action. Fails if the document has no window, the view does not exist, or the layout
   * has only one pane.
   */
  virtual Result<void> setMaximizedView(
    ui::MapDocument& document, const std::optional<std::string>& viewId) = 0;

  /**
   * Called before the pane count preference changes, which makes every map window
   * recreate its views. Moves the keyboard focus out of the map views: the editor
   * crashes if a view is destroyed while it has the focus (the window's focus change
   * handler reaches the destroyed view).
   */
  virtual void prepareForLayoutChange() = 0;
};

/** An action of the editor's action registry, as actions_list reports it. */
struct EditorAction
{
  /** The action's preference path, which identifies it, e.g. "Menu/Edit/Undo". */
  std::string path;
  std::string label;
  /**
   * "menu" (main menu), "view" (map view shortcut), "tag" (a smart tag of the game) or
   * "entity" (an entity definition of the document).
   */
  std::string kind;
  /** The menu path of a menu action without the label, e.g. {"Edit", "CSG"}. */
  std::vector<std::string> menu;
  /** The keyboard shortcuts in portable text, e.g. "Ctrl+Shift+Z". */
  std::vector<std::string> shortcuts;
  /** The action context in which the action applies, e.g. "any" or "3D view". */
  std::string context;
  /** Whether the action can run now in the window and view it was evaluated for. */
  bool enabled = false;
  bool checkable = false;
  bool checked = false;
};

/**
 * The editor's action registry: the main menu, the map view actions and the tag and
 * entity definition actions of a document. Implemented by ui::McpActionHost in the
 * editor and by FakeActionHost in tests.
 */
class ActionHost
{
public:
  virtual ~ActionHost();

  /**
   * All actions for the document's window, with enabled and checked evaluated for the
   * given view ("3d", "xy", "xz", "yz"; default: the window's current view). Fails if
   * the document has no window or the view does not exist.
   */
  virtual Result<std::vector<EditorAction>> actions(
    ui::MapDocument& document, const std::optional<std::string>& viewId) = 0;

  /**
   * Runs the action with the given path in the context of the document's window and the
   * given view (default: the current view), as the menu or the view's shortcut would. If
   * deferred, the action runs after control returned to the event loop (for actions that
   * open a modal dialog), and is looked up again by its path then; otherwise it runs
   * before this function returns. Returns the action as evaluated afterwards (a deferred
   * action: before it runs). Fails if the document has no window, the action or the view
   * does not exist, or the action is disabled.
   */
  virtual Result<EditorAction> invokeAction(
    ui::MapDocument& document,
    const std::string& path,
    const std::optional<std::string>& viewId,
    bool deferred) = 0;
};

/** A preference of any type that preferences_get / preferences_set can access. */
using AnyPreference = std::variant<
  Preference<bool>*,
  Preference<int>*,
  Preference<float>*,
  Preference<std::string>*,
  Preference<std::filesystem::path>*,
  Preference<Color>*,
  Preference<std::vector<KeySequence>>*>;

/** A preference that only the host knows, with its category. */
struct HostPreference
{
  AnyPreference preference;
  /** "keyboard" for action shortcuts, "mcp" for the MCP server's preferences, ... */
  std::string category;
  std::string description;
  /** The range of a numeric preference, if limited. */
  std::optional<double> minimum = std::nullopt;
  std::optional<double> maximum = std::nullopt;
  /**
   * If not empty, agents cannot change the preference, and preferences_set reports this
   * reason, e.g. because changing it would restart the MCP server during the call.
   */
  std::string lockedReason = {};
  /** Whether the value is a secret that preferences_get does not report. */
  bool secret = false;
};

/**
 * The preferences that only the host knows: the keyboard shortcuts of the editor's
 * actions (including the document's tag and entity definition actions) and the host's
 * own preferences. The core knows the editor's static preferences and the game
 * preferences itself. Implemented by ui::McpPreferenceHost in the editor and by
 * FakePreferenceHost in tests.
 */
class PreferenceHost
{
public:
  virtual ~PreferenceHost();

  /**
   * The host's preferences. The pointers stay valid until the next call of this
   * function. The document, if given, contributes its tag and entity definition actions.
   */
  virtual std::vector<HostPreference> preferences(ui::MapDocument* document) = 0;
};

/**
 * The editor as seen by the MCP server. Implemented by ui::QtMcpHost in the editor and by
 * FakeHost in tests. All functions are called on the thread that owns the server.
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

  /**
   * Launches game engines, or nullptr if the host cannot launch them (engine_launch then
   * fails with UNSUPPORTED_IN_HOST). The default implementation returns nullptr.
   */
  virtual EngineHost* engineHost();

  /**
   * Returns the logger that receives the document's messages outside of a log capture
   * (the console of the document's window; for a background document, a logger that
   * adds them to the console buffer), or nullptr. The default implementation returns
   * nullptr.
   */
  virtual Logger* logTarget(ui::MapDocument& document);

  /**
   * Renders snapshots offscreen, or nullptr if the host cannot render (the snapshot
   * tools then fail with UNSUPPORTED_IN_HOST). The default implementation returns
   * nullptr.
   */
  virtual SnapshotRenderer* snapshotRenderer();

  /**
   * The buffer of the messages the editor logged to its consoles, or nullptr if the host
   * has no console (the console tools then fail with UNSUPPORTED_IN_HOST). The default
   * implementation returns nullptr.
   */
  virtual ConsoleBuffer* consoleBuffer();

  /**
   * Clears the console views of the editor's windows (console_clear). The default
   * implementation does nothing.
   */
  virtual void clearConsoleViews();

  /**
   * The folder in which the server keeps level-design knowledge (material corpus
   * statistics and material notes, one subfolder per game and mod), or nullopt if the
   * host has none (the tools that read or write it then fail with UNSUPPORTED_IN_HOST).
   * The folder may not exist yet. The default implementation returns nullopt.
   */
  virtual std::optional<std::filesystem::path> knowledgeDirectory();

  /**
   * The user's editor views, or nullptr if the host has none (the camera and layout
   * tools then fail with UNSUPPORTED_IN_HOST). The default implementation returns
   * nullptr.
   */
  virtual ViewHost* viewHost();

  /**
   * The editor's action registry, or nullptr if the host has none (actions_list and
   * action_invoke then fail with UNSUPPORTED_IN_HOST). The default implementation
   * returns nullptr.
   */
  virtual ActionHost* actionHost();

  /**
   * The preferences only the host knows, or nullptr if there are none. The default
   * implementation returns nullptr.
   */
  virtual PreferenceHost* preferenceHost();

  /**
   * The user manual as the editor ships it (manual/index.html, generated from the
   * manual's Markdown source), or nullopt if the host has none (the manual tools then
   * fail with UNSUPPORTED_IN_HOST). The default implementation returns nullopt.
   */
  virtual std::optional<std::filesystem::path> manualPath();
};

} // namespace tb::mcp
