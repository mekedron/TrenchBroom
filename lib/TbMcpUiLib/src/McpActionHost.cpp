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

#include "ui/McpActionHost.h"

#include <QPointer>
#include <QTimer>

#include "base/PreferenceManager.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/Map.h"
#include "mdl/Selection.h"
#include "mdl/TagManager.h"
#include "ui/Action.h"
#include "ui/ActionContext.h"
#include "ui/ActionExecutionContext.h"
#include "ui/ActionManager.h"
#include "ui/ActionMenu.h"
#include "ui/AppController.h"
#include "ui/MapDocument.h"
#include "ui/MapViewBase.h"
#include "ui/MapWindow.h"
#include "ui/McpSnapshotRenderer.h"
#include "ui/SwitchableMapViewContainer.h"

#include "kd/overload.h"

#include <fmt/format.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

namespace tb::ui
{
namespace
{

Result<MapViewBase*> findMapView(
  MapWindow& mapWindow, const std::optional<std::string>& viewId)
{
  if (!viewId)
  {
    return mapWindow.currentMapViewBase();
  }

  auto mapViews = mapWindow.mapView().findChildren<MapViewBase*>();
  std::ranges::stable_partition(
    mapViews, [](const auto* mapView) { return mapView->isVisible(); });
  for (auto* mapView : mapViews)
  {
    if (mapViewId(*mapView) == *viewId)
    {
      return mapView;
    }
  }
  return Error{fmt::format("The window has no view '{}'", *viewId)};
}

struct ActionRef
{
  const Action* action = nullptr;
  std::string kind;
  std::vector<std::string> menu;
};

/**
 * The actions of a window: the menu and view actions are owned by the action manager, the
 * tag and entity definition actions by this object.
 */
struct ActionSet
{
  std::vector<Action> tagActions;
  std::vector<Action> entityActions;
  std::vector<ActionRef> refs;
};

void collectMenuActions(
  const Menu& menu, std::vector<std::string>& menuPath, std::vector<ActionRef>& refs)
{
  menuPath.push_back(menu.name);
  for (const auto& entry : menu.entries)
  {
    std::visit(
      kdl::overload(
        [](const MenuSeparator&) {},
        [&](const MenuAction& item) { refs.push_back({&item.action, "menu", menuPath}); },
        [&](const Menu& subMenu) { collectMenuActions(subMenu, menuPath, refs); }),
      entry);
  }
  menuPath.pop_back();
}

ActionSet collectActions(const ActionManager& actionManager, MapDocument& document)
{
  auto result = ActionSet{};

  auto menuPath = std::vector<std::string>{};
  actionManager.visitMainMenu(
    [&](auto&, const Menu& menu) { collectMenuActions(menu, menuPath, result.refs); });

  auto viewRefs = std::vector<ActionRef>{};
  actionManager.visitMapViewActions(
    [&](const Action& action) { viewRefs.push_back({&action, "view", {}}); });
  std::ranges::sort(viewRefs, [](const auto& lhs, const auto& rhs) {
    return lhs.action->preference().path < rhs.action->preference().path;
  });
  result.refs.insert(result.refs.end(), viewRefs.begin(), viewRefs.end());

  auto& map = document.map();
  result.tagActions = actionManager.createTagActions(map.tagManager().smartTags());
  result.entityActions = actionManager.createEntityDefinitionActions(
    map.entityDefinitionManager().definitions());
  for (const auto& action : result.tagActions)
  {
    result.refs.push_back({&action, "tag", {}});
  }
  for (const auto& action : result.entityActions)
  {
    result.refs.push_back({&action, "entity", {}});
  }

  return result;
}

const ActionRef* findActionRef(const ActionSet& actions, const std::string& path)
{
  const auto it = std::ranges::find_if(actions.refs, [&](const auto& ref) {
    return ref.action->preference().path.generic_string() == path;
  });
  return it != actions.refs.end() ? &*it : nullptr;
}

/**
 * Whether the action can run. Adds a guard to the action's own check: the Create action
 * of a brush entity class requires selected brushes or patches (mdl::createBrushEntity
 * asserts it, but the action is always enabled).
 */
bool isEnabled(
  const ActionRef& ref, const ActionExecutionContext& context, const mdl::Map& map)
{
  if (!ref.action->enabled(context))
  {
    return false;
  }

  if (ref.kind == "entity")
  {
    const auto path = ref.action->preference().path.generic_string();
    constexpr auto prefix = std::string_view{"Entities/"};
    constexpr auto suffix = std::string_view{"/Create"};
    if (path.starts_with(prefix) && path.ends_with(suffix))
    {
      const auto classname =
        path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
      const auto* definition = map.entityDefinitionManager().definition(classname);
      if (
        definition && mdl::getType(*definition) == mdl::EntityDefinitionType::Brush
        && !map.selection().hasOnlyGeometryNodes())
      {
        return false;
      }
    }
  }

  return true;
}

mcp::EditorAction editorAction(
  const ActionRef& ref, const ActionExecutionContext& context, const mdl::Map& map)
{
  const auto& action = *ref.action;

  auto shortcuts = std::vector<std::string>{};
  for (const auto& keySequence : pref(action.preference()))
  {
    if (!keySequence.value.empty())
    {
      shortcuts.push_back(keySequence.value);
    }
  }

  return mcp::EditorAction{
    .path = action.preference().path.generic_string(),
    .label = action.label(),
    .kind = ref.kind,
    .menu = ref.menu,
    .shortcuts = std::move(shortcuts),
    .context = actionContextName(action.actionContext()),
    .enabled = isEnabled(ref, context, map),
    .checkable = action.checkable(),
    .checked = action.checkable() && action.checked(context),
  };
}

/** Looks up the action and the view again and runs the action if it is enabled. */
void runDeferred(
  AppController& appController,
  MapWindow& mapWindow,
  const std::string& path,
  const std::optional<std::string>& viewId)
{
  auto& document = mapWindow.document();
  const auto actions = collectActions(appController.actionManager(), document);
  const auto* ref = findActionRef(actions, path);
  auto mapView = findMapView(mapWindow, viewId);
  if (!ref || mapView.is_error())
  {
    return;
  }

  auto context = ActionExecutionContext{appController, &mapWindow, mapView.value()};
  if (isEnabled(*ref, context, document.map()))
  {
    ref->action->execute(context);
  }
}

} // namespace

McpActionHost::McpActionHost(AppController& appController, FindMapWindow findMapWindow)
  : m_appController{appController}
  , m_findMapWindow{std::move(findMapWindow)}
{
}

McpActionHost::~McpActionHost() = default;

Result<std::vector<mcp::EditorAction>> McpActionHost::actions(
  MapDocument& document, const std::optional<std::string>& viewId)
{
  auto* mapWindow = m_findMapWindow(document);
  if (!mapWindow)
  {
    return Error{"The document has no window"};
  }

  return findMapView(*mapWindow, viewId) | kdl::transform([&](auto* mapView) {
           const auto actionSet =
             collectActions(m_appController.actionManager(), document);
           const auto context =
             ActionExecutionContext{m_appController, mapWindow, mapView};

           auto result = std::vector<mcp::EditorAction>{};
           result.reserve(actionSet.refs.size());
           for (const auto& ref : actionSet.refs)
           {
             result.push_back(editorAction(ref, context, document.map()));
           }
           return result;
         });
}

Result<mcp::EditorAction> McpActionHost::invokeAction(
  MapDocument& document,
  const std::string& path,
  const std::optional<std::string>& viewId,
  const bool deferred)
{
  auto* mapWindow = m_findMapWindow(document);
  if (!mapWindow)
  {
    return Error{"The document has no window"};
  }

  auto mapView = findMapView(*mapWindow, viewId);
  if (mapView.is_error())
  {
    return Error{fmt::format("The window has no view '{}'", viewId.value_or(""))};
  }

  const auto actionSet = collectActions(m_appController.actionManager(), document);
  const auto* ref = findActionRef(actionSet, path);
  if (!ref)
  {
    return Error{fmt::format("There is no action '{}'", path)};
  }

  auto context = ActionExecutionContext{m_appController, mapWindow, mapView.value()};
  if (!isEnabled(*ref, context, document.map()))
  {
    return Error{fmt::format("The action '{}' is disabled", path)};
  }

  if (deferred)
  {
    auto result = editorAction(*ref, context, document.map());
    QTimer::singleShot(
      0,
      mapWindow,
      [&appController = m_appController,
       window = QPointer<MapWindow>{mapWindow},
       path,
       viewId]() {
        if (window)
        {
          runDeferred(appController, *window, path, viewId);
        }
      });
    return result;
  }

  ref->action->execute(context);

  // the action may have changed the view's action context (e.g. activated a tool)
  const auto contextAfter =
    ActionExecutionContext{m_appController, mapWindow, mapView.value()};
  return editorAction(*ref, contextAfter, document.map());
}

} // namespace tb::ui
