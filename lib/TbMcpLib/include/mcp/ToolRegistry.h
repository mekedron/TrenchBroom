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

#include "mcp/Errors.h"
#include "mcp/Json.h"
#include "mcp/Schema.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tb::mcp
{
class Args;
class CallContext;

/** How a tool affects the editor. */
enum class Mutation
{
  /** Read-only. Runs immediately, even while the human is busy. */
  None,
  /**
   * Changes the map. Runs in one transaction named "AI: <title>" (one undo step), is
   * rolled back on failure or dry run, and waits while the human is busy.
   */
  Map,
  /** Changes something that is not undoable (files, preferences, ...). No transaction. */
  External,
};

/** Whether a tool operates on a document. */
enum class DocumentUse
{
  /** The tool ignores documents and has no `document` parameter. */
  None,
  /** The tool uses the target document if there is one. */
  Optional,
  /** The tool fails with NO_DOCUMENT if there is no target document. */
  Required,
};

using ToolHandler = std::function<ToolResult(CallContext&, const Args&)>;

/** Delivers the result of an asynchronous tool call. Must be called exactly once. */
using ToolCompletion = std::function<void(ToolResult)>;

/**
 * An asynchronous handler starts the work and returns. It continues in steps scheduled
 * with CallContext::defer and finally calls the completion. The context stays valid until
 * then. Between steps, the handler should check CallContext::cancelled().
 */
using AsyncToolHandler = std::function<void(CallContext&, const Args&, ToolCompletion)>;

/**
 * Declares a tool. Built fluently:
 *
 *   registry.add(ToolDef{"undo"}
 *     .title("Undo")
 *     .description("Undoes the last steps. Example: {\"count\": 2}")
 *     .input(schema::object({schema::field("count", schema::integer().min(1))}))
 *     .output(schema::object({...}))
 *     .mutation(Mutation::Map)
 *     .transactional(false)
 *     .handler(undo));
 */
class ToolDef
{
private:
  std::string m_name;
  std::string m_title;
  std::string m_description;
  schema::Schema m_input = schema::object({});
  std::optional<schema::Schema> m_output;
  Mutation m_mutation = Mutation::None;
  std::optional<DocumentUse> m_documentUse;
  bool m_transactional = true;
  bool m_paginated = false;
  bool m_destructive = false;
  bool m_idempotent = false;
  bool m_openWorld = false;
  ToolHandler m_handler;
  AsyncToolHandler m_asyncHandler;

public:
  explicit ToolDef(std::string name);

  ToolDef& title(std::string title);
  ToolDef& description(std::string description);
  ToolDef& input(schema::Schema input);
  ToolDef& output(schema::Schema output);
  ToolDef& mutation(Mutation mutation);
  /** Defaults to Required for Mutation::Map and None otherwise. */
  ToolDef& documentUse(DocumentUse documentUse);
  /**
   * Whether a Map tool runs in its own transaction (default). Tools that manage the
   * history themselves (undo, redo, transaction_*) set this to false; they must honor
   * dry run themselves.
   */
  ToolDef& transactional(bool transactional);
  /** Adds the standard list parameters `cursor`, `limit`, `fields`, `detail`. */
  ToolDef& paginated(bool paginated = true);
  ToolDef& destructive(bool destructive = true);
  ToolDef& idempotent(bool idempotent = true);
  ToolDef& openWorld(bool openWorld = true);
  ToolDef& handler(ToolHandler handler);
  /**
   * Sets an asynchronous handler for long operations that report progress and can be
   * cancelled (spec E2.16). Only for Mutation::External tools: they wait in the call
   * queue, and the queue waits until they complete.
   */
  ToolDef& asyncHandler(AsyncToolHandler handler);

  const std::string& name() const;
  const std::string& title() const;
  const std::string& description() const;
  Mutation mutation() const;
  DocumentUse documentUse() const;
  bool transactional() const;
  bool paginated() const;
  bool destructive() const;
  bool idempotent() const;
  bool openWorld() const;
  const ToolHandler& handler() const;
  const AsyncToolHandler& asyncHandler() const;
  bool isAsync() const;

  /** Whether calls wait while the human is busy (Map and External tools). */
  bool isModifying() const;

  /** The declared input plus the injected standard parameters. */
  schema::Schema inputSchema() const;

  /**
   * The schema of `structuredContent`: the declared output, wrapped in the result
   * envelope for modifying tools.
   */
  std::optional<schema::Schema> outputSchema() const;

  /** The `tools/list` entry for the given protocol version. */
  Json toJson(std::string_view protocolVersion) const;
};

class ToolRegistry
{
private:
  std::vector<ToolDef> m_tools;

public:
  /**
   * Precondition: the name is valid and not yet registered, and there is exactly one
   * handler. Asynchronous tools are Mutation::External.
   */
  void add(ToolDef tool);

  const ToolDef* find(std::string_view name) const;
  const std::vector<ToolDef>& tools() const;

  /**
   * Returns a `tools/list` result. All tools fit on one page unless a smaller page size
   * is given.
   */
  Json list(
    std::string_view protocolVersion,
    const std::optional<std::string>& cursor,
    size_t pageSize = 1000) const;
};

/** Whether the name matches ^[a-z][a-z0-9_]{0,63}$. */
bool isValidToolName(std::string_view name);

} // namespace tb::mcp
