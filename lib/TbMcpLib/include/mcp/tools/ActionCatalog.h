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

#include <string>
#include <string_view>
#include <vector>

// The MCP classification of the editor's actions (E14.4, E14.5, E14.8): which actions
// action_invoke may run inside a call, which ones open a dialog for the user, which ones
// it refuses, and which semantic tools do the same.

namespace tb::mcp
{

/** How action_invoke treats an action. */
enum class ActionHandling
{
  /** Runs synchronously inside the call. */
  Invoke,
  /**
   * Opens a modal dialog, a file chooser, a text prompt or a confirmation (or a window
   * for the user), which would block a call. Refused unless the agent asks to open the
   * dialog for the user; then the action runs after the call returned.
   */
  Dialog,
  /**
   * Must not run from a call at all: it would break the call's transaction, destroy the
   * document during the call, or crash the editor. A semantic tool does the same.
   */
  Refuse,
};

/** The MCP classification of one action. */
struct ActionClass
{
  ActionHandling handling = ActionHandling::Invoke;
  /**
   * What a Dialog action opens: "modal" (a modal dialog), "file" (a file chooser),
   * "input" (a text prompt), "confirmation" (a question that may be asked), "menu" (a
   * popup menu that may be shown), "window" (a non-modal window) or "browser" (an
   * external application). Empty for other actions.
   */
  std::string_view dialog;
  /** The semantic MCP tools that do the same (or its main effect). May be empty. */
  std::vector<std::string_view> tools;
  /** Why the action opens a dialog or is refused; empty for Invoke actions. */
  std::string_view reason;
};

/** A catalog entry of an action with a fixed path (main menu and map view actions). */
struct ActionCatalogEntry
{
  std::string_view path;
  ActionClass actionClass;
};

/** The catalog entries of the actions with a fixed path. */
const std::vector<ActionCatalogEntry>& actionCatalog();

/**
 * The patterns of the per-document actions: "Filters/Tags/<tag>/Toggle Visible",
 * "Tags/<tag>/Enable", "Tags/<tag>/Disable", "Entities/<class>/Toggle" and
 * "Entities/<class>/Create", each as {prefix, suffix, class}.
 */
struct ActionCatalogPattern
{
  std::string_view prefix;
  std::string_view suffix;
  ActionClass actionClass;
};

const std::vector<ActionCatalogPattern>& actionCatalogPatterns();

/**
 * The classification of the action with the given path, or nullptr if the catalog does
 * not know it.
 */
const ActionClass* findActionClass(std::string_view path);

/**
 * The classification of the action with the given path and label. An action the catalog
 * does not know (e.g. added by a newer editor) is treated as a Dialog action if its label
 * ends with "..." (the editor's convention for actions that ask for more input), and as
 * an Invoke action otherwise.
 */
ActionClass classifyAction(std::string_view path, std::string_view label);

/** The names of all tools the catalog refers to, sorted, each once. */
std::vector<std::string> actionCatalogToolNames();

/** "invoke", "dialog" or "refuse". */
std::string_view toString(ActionHandling handling);

} // namespace tb::mcp
