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

#include "mcp/tools/HistoryTools.h"

#include "base/NotifierConnection.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "mdl/TransactionScope.h"
#include "ui/MapDocument.h"

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr auto AgentPrefix = std::string_view{"AI: "};

Json historyEntries(const std::vector<std::string>& names, const size_t limit)
{
  auto result = Json::array();
  for (size_t i = 0; i < names.size() && i < limit; ++i)
  {
    result.push_back(
      Json{{"name", names[i]}, {"agent", names[i].starts_with(AgentPrefix)}});
  }
  return result;
}

std::optional<ToolError> checkNoOpenTransaction(CallContext& context)
{
  const auto& state = context.documentState();
  if (state.transaction)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "The agent transaction '" + state.transaction->name + "' is open.",
      "Commit or roll back the transaction first (transaction_commit or "
      "transaction_rollback).");
  }
  if (context.map().commandProcessor().transactionDepth() > 0)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "A transaction is open in the editor.",
      "Try again when the user has finished the current interaction.");
  }
  return std::nullopt;
}

ToolResult historyGet(CallContext& context, const Args& args)
{
  const auto limit = size_t(args.get<int64_t>("limit"));
  const auto& commandProcessor = context.map().commandProcessor();
  const auto& state = context.documentState();

  const auto undoNames = commandProcessor.undoCommandNames();
  const auto redoNames = commandProcessor.redoCommandNames();
  return Json{
    {"undo", historyEntries(undoNames, limit)},
    {"redo", historyEntries(redoNames, limit)},
    {"undoCount", undoNames.size()},
    {"redoCount", redoNames.size()},
    {"transaction",
     state.transaction
       ? Json{{"name", state.transaction->name}, {"owner", state.transaction->clientName}}
       : Json(nullptr)},
  };
}

enum class Direction
{
  Undo,
  Redo,
};

ToolResult undoRedo(CallContext& context, const Args& args, const Direction direction)
{
  if (auto error = checkNoOpenTransaction(context))
  {
    return *error;
  }

  const auto count = size_t(args.get<int64_t>("count"));
  auto& map = context.map();
  const auto names = direction == Direction::Undo
                       ? map.commandProcessor().undoCommandNames()
                       : map.commandProcessor().redoCommandNames();
  const auto verb = std::string{direction == Direction::Undo ? "undo" : "redo"};

  if (names.empty())
  {
    return makeError(
      ErrorCode::OperationFailed,
      "There is nothing to " + verb + ".",
      "Use history_get to inspect the history.");
  }
  if (count > names.size())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Only " + std::to_string(names.size()) + " steps can be " + verb + "ne, not "
        + std::to_string(count) + ".",
      "Pass count <= " + std::to_string(names.size()) + ".");
  }

  auto steps = Json::array();
  for (size_t i = 0; i < count; ++i)
  {
    steps.push_back(names[i]);
  }

  if (context.dryRun())
  {
    return Json{
      {direction == Direction::Undo ? "wouldUndo" : "wouldRedo", std::move(steps)}};
  }

  for (size_t i = 0; i < count; ++i)
  {
    if (direction == Direction::Undo)
    {
      map.undoCommand();
    }
    else
    {
      map.redoCommand();
    }
  }

  return Json{{direction == Direction::Undo ? "undone" : "redone", std::move(steps)}};
}

ToolResult undo(CallContext& context, const Args& args)
{
  return undoRedo(context, args, Direction::Undo);
}

ToolResult redo(CallContext& context, const Args& args)
{
  return undoRedo(context, args, Direction::Redo);
}

ToolResult transactionBegin(CallContext& context, const Args& args)
{
  auto& state = context.documentState();
  if (state.transaction)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "The transaction '" + state.transaction->name + "' is already open.",
      "Commit or roll back the open transaction first; transactions cannot be nested.");
  }

  const auto name = args.get<std::string>("name");
  if (context.dryRun())
  {
    return Json{{"wouldBegin", name}};
  }

  auto& map = context.map();
  map.startTransaction(
    std::string{AgentPrefix} + name, mdl::TransactionScope::LongRunning);
  state.transaction = AgentTransaction{
    context.session().id,
    context.session().clientDisplayName(),
    name,
    map.commandProcessor().transactionDepth(),
    args.get<std::string>("checks") == "defer"};
  context.server().updateOpenTransactions();

  return Json{
    {"transaction", name},
    {"document", context.documentInfo().id},
    {"checks", args.get<std::string>("checks")}};
}

std::optional<ToolError> checkOwnTransaction(CallContext& context)
{
  const auto& state = context.documentState();
  if (!state.transaction || state.transaction->sessionId != context.session().id)
  {
    return makeError(
      ErrorCode::NoTransaction,
      "This session has no open transaction on " + context.documentInfo().id + ".",
      "Start one with transaction_begin.");
  }
  if (context.map().commandProcessor().transactionDepth() != state.transaction->depth)
  {
    return makeError(
      ErrorCode::TransactionActive,
      "A nested transaction is open in the editor.",
      "Try again when the user has finished the current interaction.");
  }
  return std::nullopt;
}

ToolResult transactionCommit(CallContext& context, const Args&)
{
  if (auto error = checkOwnTransaction(context))
  {
    return *error;
  }

  auto& state = context.documentState();
  const auto name = state.transaction->name;
  if (context.dryRun())
  {
    return Json{{"wouldCommit", name}};
  }

  const auto undoStepName = std::string{AgentPrefix} + name;
  auto stored = false;
  auto connection = NotifierConnection{};
  connection += context.document().transactionDoneNotifier.connect(
    [&](const std::string& doneName, bool, bool) {
      if (doneName == undoStepName)
      {
        stored = true;
      }
    });

  const auto committed = context.map().commitTransaction();
  state.transaction.reset();
  context.server().updateOpenTransactions();

  if (!committed)
  {
    return context.operationFailed(
      "The transaction could not be applied to all linked groups, so it was rolled back.",
      "Change only one copy of each linked group; the editor updates the other "
      "copies.");
  }
  if (stored)
  {
    context.setUndoStep(undoStepName);
  }
  return Json{{"committed", name}, {"empty", !stored}};
}

ToolResult transactionRollback(CallContext& context, const Args&)
{
  if (auto error = checkOwnTransaction(context))
  {
    return *error;
  }

  auto& state = context.documentState();
  const auto name = state.transaction->name;
  if (context.dryRun())
  {
    return Json{{"wouldRollBack", name}};
  }

  context.map().cancelTransaction();
  state.transaction.reset();
  context.server().updateOpenTransactions();

  return Json{{"rolledBack", name}};
}

Schema historyEntrySchema()
{
  return object({
    field("name", string())
      .required()
      .describe("Undo step name, e.g. 'AI: Move Objects'"),
    field("agent", boolean()).required().describe("Whether an agent created the step"),
  });
}

} // namespace

void registerHistoryTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"history_get"}
      .title("Get History")
      .description(
        "Lists the document's undo and redo steps (read-only), most recent first, with "
        "their total counts. Steps created by agents are named 'AI: <tool title>' and "
        "marked agent: true. Also reports the open agent transaction. Use undo or redo "
        "with a count to step through them. Example: {\"limit\": 10}")
      .input(object({
        field("limit", integer().min(1).max(1000).defaultsTo(20))
          .describe("Maximum number of steps per list"),
      }))
      .output(object({
        field("undo", array(historyEntrySchema())).required(),
        field("redo", array(historyEntrySchema())).required(),
        field("undoCount", integer()).required().describe("Total number of undo steps"),
        field("redoCount", integer()).required().describe("Total number of redo steps"),
        field("transaction", any())
          .describe("{name, owner} of the open agent transaction, or null"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(historyGet));

  registry.add(
    ToolDef{"undo"}
      .title("Undo")
      .description(
        "Undoes the most recent undo steps (agent or human), like Edit > Undo; redo "
        "restores them. Returns the names of the undone steps. Object ids of restored "
        "objects become valid again. Fails with TRANSACTION_ACTIVE while a transaction "
        "is open (transaction_rollback discards an agent transaction instead). "
        "history_get shows what would be undone. Example: {\"count\": 2}")
      .input(object({
        field("count", integer().min(1).max(1000).defaultsTo(1))
          .describe("Number of steps to undo"),
      }))
      .output(object({
        field("undone", array(string())).describe("Names of the undone steps"),
        field("wouldUndo", array(string()))
          .describe("Dry run: steps that would be undone"),
      }))
      .mutation(Mutation::Map)
      .transactional(false)
      .handler(undo));

  registry.add(
    ToolDef{"redo"}
      .title("Redo")
      .description(
        "Redoes the most recently undone steps, like Edit > Redo, and returns their "
        "names. Any new change clears the redo steps. Fails with TRANSACTION_ACTIVE "
        "while a transaction is open. Example: {\"count\": 1}")
      .input(object({
        field("count", integer().min(1).max(1000).defaultsTo(1))
          .describe("Number of steps to redo"),
      }))
      .output(object({
        field("redone", array(string())).describe("Names of the redone steps"),
        field("wouldRedo", array(string()))
          .describe("Dry run: steps that would be redone"),
      }))
      .mutation(Mutation::Map)
      .transactional(false)
      .handler(redo));

  registry.add(
    ToolDef{"transaction_begin"}
      .title("Begin Transaction")
      .description(
        "Opens a transaction on the document: all following changes of this session "
        "become one undo step 'AI: <name>' on transaction_commit, or leave no trace on "
        "transaction_rollback. Other clients cannot modify the document meanwhile, and "
        "undo/redo fail until it is closed. Calls inside the transaction report undoStep "
        "null (their changes belong to the transaction's step). With checks: "
        "\"defer\", the calls inside skip their issue checks (unless they pass checks: "
        "\"report\") and transaction_commit reports the issues of the whole "
        "transaction, with issuesSummary counting them per code: use it for bulk work "
        "such as importing a shell and its entities in several calls. Transactions "
        "cannot be nested. Closing the session rolls it back. Example: {\"name\": "
        "\"Build east wing\", \"checks\": \"defer\"}")
      .input(object({
        field("name", string().nonEmpty()).required().describe("Name of the undo step"),
        field("checks", enumOf({"report", "defer"}).defaultsTo("report"))
          .describe(
            "defer: calls inside the transaction defer their issue checks by default, "
            "and transaction_commit reports them for the whole transaction"),
      }))
      .output(object({
        field("transaction", string()).describe("Name of the opened transaction"),
        field("checks", string()).describe("The checks mode of the calls inside"),
        field("document", string()).describe("Handle of the document"),
        field("wouldBegin", string()).describe("Dry run: name it would open"),
      }))
      .mutation(Mutation::Map)
      .transactional(false)
      .handler(transactionBegin));

  registry.add(
    ToolDef{"transaction_commit"}
      .title("Commit Transaction")
      .description(
        "Closes this session's open transaction (transaction_begin) and stores all its "
        "changes as one undo step 'AI: <name>' (undoStep); empty: true if nothing "
        "changed. 'changes' lists the net created, modified and removed objects of all "
        "calls in the transaction; if the calls deferred their checks (checks: "
        "\"defer\"), issuesIntroduced and issuesSummary report the issues of the whole "
        "transaction. Fails with NO_TRANSACTION if this session has none open. "
        "Example: {\"detail\": \"summary\"}")
      .input(object({}))
      .output(object({
        field("committed", string()).describe("Name of the committed transaction"),
        field("empty", boolean())
          .describe("True if the transaction contained no changes"),
        field("wouldCommit", string()).describe("Dry run: name it would commit"),
      }))
      .mutation(Mutation::Map)
      .transactional(false)
      .handler(transactionCommit));

  registry.add(
    ToolDef{"transaction_rollback"}
      .title("Roll Back Transaction")
      .description("Discards all changes made in this session's open transaction "
                   "(transaction_begin) and closes it, leaving no undo step. Fails with "
                   "NO_TRANSACTION if this session has none open. Example: {}")
      .input(object({}))
      .output(object({
        field("rolledBack", string()).describe("Name of the discarded transaction"),
        field("wouldRollBack", string()).describe("Dry run: name it would discard"),
      }))
      .mutation(Mutation::Map)
      .transactional(false)
      .destructive()
      .handler(transactionRollback));
}

} // namespace tb::mcp
