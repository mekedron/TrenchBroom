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

#include "mcp/FakeActionHost.h"

#include <algorithm>

namespace tb::mcp
{

Result<std::vector<EditorAction>> FakeActionHost::actions(
  ui::MapDocument&, const std::optional<std::string>& viewId)
{
  if (error)
  {
    return Error{*error};
  }
  if (viewId && std::ranges::find(viewIds, *viewId) == viewIds.end())
  {
    return Error{"View '" + *viewId + "' does not exist"};
  }
  return actionList;
}

Result<EditorAction> FakeActionHost::invokeAction(
  ui::MapDocument& document,
  const std::string& path,
  const std::optional<std::string>& viewId,
  const bool deferred)
{
  if (error)
  {
    return Error{*error};
  }
  if (viewId && std::ranges::find(viewIds, *viewId) == viewIds.end())
  {
    return Error{"View '" + *viewId + "' does not exist"};
  }

  const auto it = std::ranges::find(actionList, path, &EditorAction::path);
  if (it == actionList.end())
  {
    return Error{"Unknown action " + path};
  }
  if (!it->enabled)
  {
    return Error{"Action " + path + " is disabled"};
  }

  invocations.push_back({&document, path, viewId, deferred});
  if (!deferred)
  {
    if (it->checkable)
    {
      it->checked = !it->checked;
    }
    if (onInvoke)
    {
      onInvoke(document, *it);
    }
  }
  return *it;
}

} // namespace tb::mcp
