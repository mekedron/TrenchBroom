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
#include <optional>
#include <string>
#include <vector>

namespace tb
{
namespace mdl
{
class GameManager;
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
 * The editor as seen by the MCP server. Implemented by ui::QtMcpHost in the editor and by
 * FakeHost in tests. All functions are called on the thread that owns the server.
 *
 * Further sub-interfaces (documents, actions, views, compile, preferences) are added by
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
};

} // namespace tb::mcp
