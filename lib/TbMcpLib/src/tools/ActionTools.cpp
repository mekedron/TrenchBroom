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

#include "mcp/tools/ActionTools.h"

#include "ToolUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Host.h"
#include "mcp/Pagination.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/ActionCatalog.h"
#include "mdl/Map.h"

#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{
using namespace schema;

namespace
{

const auto ViewIds = std::vector<std::string>{"3d", "xy", "xz", "yz"};

Json toJsonOrNull(const std::optional<std::string>& value)
{
  return value ? Json(*value) : Json(nullptr);
}

Result<ActionHost*, ToolError> actionHostOf(CallContext& context)
{
  if (auto* actionHost = context.host().actionHost())
  {
    return actionHost;
  }
  return makeError(
    ErrorCode::UnsupportedInHost,
    "This host has no editor actions.",
    "Use the semantic tools, or run the action in the TrenchBroom editor.");
}

Result<std::vector<EditorAction>, ToolError> listActions(
  CallContext& context, ActionHost& actionHost, const std::optional<std::string>& viewId)
{
  auto actions = actionHost.actions(context.document(), viewId);
  if (actions.is_error())
  {
    return makeError(
      ErrorCode::OperationFailed,
      fmt::format("Could not list the editor actions: {}", errorMessage(actions)),
      viewId ? "Omit 'view' to use the window's current view." : "");
  }
  return std::move(actions).value();
}

std::string toolList(const ActionClass& actionClass)
{
  return kdl::str_join(actionClass.tools, ", ");
}

Json actionJson(const EditorAction& action, const Detail detail)
{
  const auto actionClass = classifyAction(action.path, action.label);
  auto item = Json{
    {"path", action.path},
    {"label", action.label},
    {"kind", action.kind},
    {"menu", action.menu},
    {"shortcuts", action.shortcuts},
    {"enabled", action.enabled},
    {"checked", action.checkable ? Json(action.checked) : Json(nullptr)},
    {"opensDialog", actionClass.handling == ActionHandling::Dialog},
    {"invokable", actionClass.handling == ActionHandling::Invoke},
    {"tools", actionClass.tools},
  };
  if (detail == Detail::Full)
  {
    item["context"] = action.context;
    item["checkable"] = action.checkable;
    item["handling"] = toString(actionClass.handling);
    item["dialog"] =
      actionClass.dialog.empty() ? Json(nullptr) : Json(actionClass.dialog);
    item["reason"] =
      actionClass.reason.empty() ? Json(nullptr) : Json(actionClass.reason);
  }
  return item;
}

bool menuMatches(const EditorAction& action, const std::string& menu)
{
  const auto actionMenu = kdl::str_to_lower(kdl::str_join(action.menu, "/"));
  auto prefix = kdl::str_to_lower(menu);
  while (!prefix.empty() && prefix.back() == '/')
  {
    prefix.pop_back();
  }
  return actionMenu == prefix || actionMenu.starts_with(prefix + "/");
}

bool queryMatches(const EditorAction& action, const std::string& query)
{
  const auto lowerQuery = kdl::str_to_lower(query);
  return kdl::str_to_lower(action.label).find(lowerQuery) != std::string::npos
         || kdl::str_to_lower(action.path).find(lowerQuery) != std::string::npos;
}

// actions_list

ToolResult actionsList(CallContext& context, const Args& args)
{
  const auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto actionHost = actionHostOf(context);
  if (actionHost.is_error())
  {
    return errorOf(actionHost);
  }

  const auto actions =
    listActions(context, *actionHost.value(), args.getOptional<std::string>("view"));
  if (actions.is_error())
  {
    return errorOf(actions);
  }

  const auto kind = args.getOptional<std::string>("kind");
  const auto menu = args.getOptional<std::string>("menu");
  const auto query = args.getOptional<std::string>("query");
  const auto handling = args.getOptional<std::string>("handling");
  const auto enabledOnly = args.getOr<bool>("enabledOnly", false);

  auto items = std::vector<Json>{};
  auto kinds = std::map<std::string, size_t>{};
  for (const auto& action : actions.value())
  {
    if (
      (kind && action.kind != *kind) || (menu && !menuMatches(action, *menu))
      || (query && !queryMatches(action, *query)) || (enabledOnly && !action.enabled)
      || (handling && toString(classifyAction(action.path, action.label).handling) != *handling))
    {
      continue;
    }
    ++kinds[action.kind];
    items.push_back(actionJson(action, request.value().detail));
  }

  auto result = makePage(items, request.value(), 0);
  result["kinds"] = kinds;
  return result;
}

// action_invoke

/** Finds an action by its path, or by its label if no path matches and it is unique. */
Result<const EditorAction*, ToolError> findAction(
  const std::vector<EditorAction>& actions, const std::string& pathOrLabel)
{
  if (const auto it = std::ranges::find(actions, pathOrLabel, &EditorAction::path);
      it != actions.end())
  {
    return &*it;
  }

  const auto lowerLabel = kdl::str_to_lower(pathOrLabel);
  auto byLabel = std::vector<const EditorAction*>{};
  for (const auto& action : actions)
  {
    if (kdl::str_to_lower(action.label) == lowerLabel)
    {
      byLabel.push_back(&action);
    }
  }
  if (byLabel.size() == 1)
  {
    return byLabel.front();
  }

  if (byLabel.size() > 1)
  {
    auto paths = std::vector<std::string>{};
    for (const auto* action : byLabel)
    {
      paths.push_back(action->path);
    }
    auto error = makeError(
      ErrorCode::InvalidArgument,
      fmt::format(
        "The label '{}' belongs to {} actions: {}.",
        pathOrLabel,
        byLabel.size(),
        kdl::str_join(paths, ", ")),
      "Pass the action's path instead of its label.");
    error.details["candidates"] = paths;
    return error;
  }

  // suggest actions whose label or path contains the last part of the given text
  const auto lastSlash = pathOrLabel.find_last_of('/');
  const auto term =
    lastSlash == std::string::npos ? pathOrLabel : pathOrLabel.substr(lastSlash + 1);
  auto suggestions = std::vector<std::string>{};
  for (const auto& action : actions)
  {
    if (!term.empty() && queryMatches(action, term) && suggestions.size() < 5)
    {
      suggestions.push_back(action.path);
    }
  }

  auto error = makeError(
    ErrorCode::InvalidArgument,
    fmt::format("There is no editor action '{}'.", pathOrLabel),
    suggestions.empty()
      ? "Use actions_list with 'query' to find the action's path."
      : fmt::format(
          "Did you mean one of: {}? Use actions_list with 'query' to find the path.",
          kdl::str_join(suggestions, ", ")));
  error.details["suggestions"] = suggestions;
  return error;
}

Json invokeResultJson(
  const EditorAction& action,
  const ActionClass& actionClass,
  CallContext& context,
  const bool executed)
{
  return Json{
    {"path", action.path},
    {"label", action.label},
    {"kind", action.kind},
    {"executed", executed},
    {"opened", nullptr},
    {"mapChanged", false},
    {"enabled", action.enabled},
    {"checked", action.checkable ? Json(action.checked) : Json(nullptr)},
    {"currentTool", toJsonOrNull(context.host().currentToolName(context.document()))},
    {"tools", actionClass.tools},
  };
}

ToolResult actionInvoke(CallContext& context, const Args& args)
{
  auto actionHost = actionHostOf(context);
  if (actionHost.is_error())
  {
    return errorOf(actionHost);
  }

  const auto viewId = args.getOptional<std::string>("view");
  const auto actions = listActions(context, *actionHost.value(), viewId);
  if (actions.is_error())
  {
    return errorOf(actions);
  }

  const auto found = findAction(actions.value(), args.get<std::string>("path"));
  if (found.is_error())
  {
    return errorOf(found);
  }
  const auto& action = *found.value();
  const auto actionClass = classifyAction(action.path, action.label);
  const auto openDialog = args.getOr<bool>("openDialog", false);

  if (actionClass.handling == ActionHandling::Refuse)
  {
    auto error = makeError(
      ErrorCode::ActionRefused,
      fmt::format(
        "'{}' ({}) cannot run from a tool call: it {}.",
        action.label,
        action.path,
        actionClass.reason),
      actionClass.tools.empty()
        ? "This action is only available to the user in the editor."
        : fmt::format("Use {} instead.", toolList(actionClass)));
    error.details["tools"] = actionClass.tools;
    return error;
  }

  if (actionClass.handling == ActionHandling::Dialog && !openDialog)
  {
    auto error = makeError(
      ErrorCode::DialogRequired,
      fmt::format(
        "'{}' ({}) {}, which only the user can answer.",
        action.label,
        action.path,
        actionClass.reason),
      actionClass.tools.empty()
        ? "Pass openDialog: true to open it for the user; the call returns immediately."
        : fmt::format(
            "Use {} instead, or pass openDialog: true to open it for the user (the call "
            "returns immediately).",
            toolList(actionClass)));
    error.details["tools"] = actionClass.tools;
    error.details["dialog"] = actionClass.dialog;
    return error;
  }

  if (!action.enabled)
  {
    auto error = makeError(
      ErrorCode::OperationFailed,
      fmt::format(
        "'{}' ({}) is disabled in the current editor state.", action.label, action.path),
      fmt::format(
        "It applies in: {}. Check the selection and the active tool (actions_list shows "
        "which actions are enabled){}.",
        action.context,
        actionClass.tools.empty()
          ? ""
          : fmt::format(
              "; the semantic tools {} may work instead", toolList(actionClass))));
    error.details["context"] = action.context;
    error.details["currentTool"] =
      toJsonOrNull(context.host().currentToolName(context.document()));
    return error;
  }

  const auto deferred = actionClass.handling == ActionHandling::Dialog;
  if (context.dryRun())
  {
    auto result = invokeResultJson(action, actionClass, context, false);
    result["wouldDo"] =
      deferred ? fmt::format("open the dialog of '{}' for the user", action.label)
               : fmt::format("run '{}'", action.label);
    return result;
  }

  auto& map = context.map();
  const auto modificationCount = map.modificationCount();
  const auto invoked =
    actionHost.value()->invokeAction(context.document(), action.path, viewId, deferred);
  if (invoked.is_error())
  {
    return context.operationFailed(
      fmt::format("Could not run '{}': {}", action.label, errorMessage(invoked)),
      "Check the action with actions_list.");
  }

  auto result = invokeResultJson(invoked.value(), actionClass, context, !deferred);
  result["opened"] = deferred ? Json("dialog") : Json(nullptr);
  result["mapChanged"] = map.modificationCount() != modificationCount;
  if (deferred)
  {
    context.warn(
      "DIALOG_OPENED",
      fmt::format(
        "'{}' opens for the user after this call; further modifying calls wait until "
        "the user closes it.",
        action.label));
  }
  return result;
}

Schema viewSchema()
{
  return enumOf(ViewIds);
}

Schema actionItemSchema()
{
  return object({
    field("path", string())
      .required()
      .describe("The action's path; pass it to action_invoke"),
    field("label", string()).required(),
    field("kind", enumOf({"menu", "view", "tag", "entity"})).required(),
    field("menu", array(string()))
      .required()
      .describe("The menu path of a menu action, e.g. [\"Edit\", \"CSG\"]"),
    field("shortcuts", array(string())).required(),
    field("enabled", boolean()).required(),
    field("checked", any()).required().describe("null if the action is not checkable"),
    field("opensDialog", boolean()).required(),
    field("invokable", boolean())
      .required()
      .describe("Whether action_invoke runs it inside the call"),
    field("tools", array(string()))
      .required()
      .describe("Semantic tools that do the same"),
    field("context", string()).describe("full: where the action applies"),
    field("checkable", boolean()).describe("full"),
    field("handling", enumOf({"invoke", "dialog", "refuse"})).describe("full"),
    field("dialog", any())
      .describe("full: modal, file, input, confirmation, menu, window or browser"),
    field("reason", any()).describe("full: why it opens a dialog or is refused"),
  });
}

} // namespace

void registerActionTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"actions_list"}
      .title("List Editor Actions")
      .description(
        "Lists the editor's actions: the main menu (kind \"menu\"), the map view "
        "shortcuts (\"view\"), and the document's smart tag (\"tag\") and entity class "
        "(\"entity\") actions, with label, path, menu, keyboard shortcuts and whether "
        "they are enabled / checked now in the given view (default: the window's "
        "current view). opensDialog marks actions that ask the user (a dialog, file "
        "chooser, text prompt or confirmation); invokable marks the actions that "
        "action_invoke runs inside the call; tools names the semantic tools that do "
        "the same, which are preferable because they take explicit arguments and "
        "object ids. Filters are combined: kind, menu (menu path prefix such as "
        "\"Edit/CSG\"), query (case-insensitive substring of label or path), handling "
        "(invoke, dialog, refuse) and enabledOnly. detail \"full\" adds context, "
        "checkable, handling, dialog and reason. kinds counts the matches by kind. "
        "Example: {\"menu\":\"Edit/CSG\"} -> {\"items\":[{\"path\":"
        "\"Menu/Edit/CSG/Convex Merge\",\"label\":\"Convex Merge\",\"kind\":\"menu\","
        "\"menu\":[\"Edit\",\"CSG\"],\"shortcuts\":[\"Ctrl+J\"],\"enabled\":false,"
        "\"checked\":null,\"opensDialog\":false,\"invokable\":true,\"tools\":"
        "[\"csg_merge\"]}, ...],\"total\":4,\"nextCursor\":null,\"kinds\":{\"menu\":4}}")
      .input(object({
        field("kind", enumOf({"menu", "view", "tag", "entity"}))
          .describe("Only actions of this kind"),
        field("menu", string().nonEmpty())
          .describe("Only menu actions under this menu path, e.g. \"View/Grid\""),
        field("query", string().nonEmpty())
          .describe("Case-insensitive substring of the label or the path"),
        field("handling", enumOf({"invoke", "dialog", "refuse"}))
          .describe("Only actions that action_invoke treats this way"),
        field("enabledOnly", boolean()).describe("Only actions that are enabled now"),
        field("view", viewSchema())
          .describe(
            "The view to evaluate enabled/checked for; default: the current view"),
      }))
      .output(object({
        field("items", array(actionItemSchema())).required(),
        field("total", integer()).required(),
        field("nextCursor", any()).required(),
        field("kinds", object({}).allowAdditionalProperties())
          .required()
          .describe("Matches by kind"),
      }))
      .paginated()
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Required)
      .idempotent()
      .handler(actionsList));

  registry.add(
    ToolDef{"action_invoke"}
      .title("Invoke Action")
      .description(
        "Runs an editor action by its path (from actions_list; a unique label such as "
        "\"Convex Merge\" is accepted too) as if the user chose it from the menu or "
        "pressed its shortcut in the given view (default: the current view), in the "
        "editor's current state: the active tool stays active, so tool actions such as "
        "\"Controls/Map view/Perform clip\" work. Map changes become one undo step "
        "\"AI: Invoke Action\" with a change report; toggling a tool, a view filter, the "
        "grid or the camera changes no map object. Prefer the semantic tools listed in "
        "the action's 'tools': they take explicit arguments and object ids. Disabled "
        "actions fail with the context in which they apply. Actions that ask the user "
        "(opensDialog) fail with DIALOG_REQUIRED and name the semantic tool, unless "
        "openDialog is true: then the dialog opens for the user after the call returned "
        "(opened: \"dialog\"). Actions that must not run from a call (undo/redo inside "
        "the call's transaction, reloading definitions, debug crashes) fail with "
        "ACTION_REFUSED and name the tool to use. dryRun checks the action without "
        "running it. Returns executed, mapChanged, the action's checked state "
        "afterwards and the current tool. Example: {\"path\":\"Menu/Edit/Tools/Clip "
        "Tool\"} -> {\"result\":{\"path\":\"Menu/Edit/Tools/Clip Tool\",\"executed\":"
        "true,\"checked\":true,\"currentTool\":\"Clip Tool\",...}}")
      .input(object({
        field("path", string().nonEmpty())
          .required()
          .describe("The action's path, e.g. \"Menu/View/Grid/Set Grid Size 16\""),
        field("view", viewSchema())
          .describe("The view to run the action in; default: the current view"),
        field("openDialog", boolean())
          .defaultsTo(false)
          .describe("For actions that open a dialog: open it for the user after the call "
                    "returns"),
      }))
      .output(object({
        field("path", string()).required(),
        field("label", string()).required(),
        field("kind", string()).required(),
        field("executed", boolean())
          .required()
          .describe("Whether the action ran inside the call"),
        field("opened", any())
          .required()
          .describe("\"dialog\" if the action opens a dialog for the user, else null"),
        field("mapChanged", boolean()).required(),
        field("enabled", boolean()).required().describe("Whether it is enabled now"),
        field("checked", any())
          .required()
          .describe("The checked state afterwards; null if not checkable"),
        field("currentTool", any()).required().describe("The active tool afterwards"),
        field("tools", array(string())).required(),
        field("wouldDo", string()).describe("dry run: what the call would do"),
      }))
      .mutation(Mutation::Map)
      .keepsActiveTool()
      .handler(actionInvoke));
}

} // namespace tb::mcp
