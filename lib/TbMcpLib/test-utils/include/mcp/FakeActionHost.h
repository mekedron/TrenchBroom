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

#include "mcp/Host.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tb::mcp
{

/** An action host for tests. */
class FakeActionHost : public ActionHost
{
public:
  struct Invocation
  {
    ui::MapDocument* document = nullptr;
    std::string path;
    std::optional<std::string> viewId;
    bool deferred = false;
  };

  /** Returned by actions(). */
  std::vector<EditorAction> actionList;
  /** If set, actions() and invokeAction() fail with this message (e.g. no window). */
  std::optional<std::string> error;
  /** The views that exist; other view ids fail. */
  std::vector<std::string> viewIds = {"3d", "xy", "xz", "yz"};
  /** The invocations so far, including deferred ones. */
  std::vector<Invocation> invocations;
  /**
   * Called when an action runs (not when a deferred action is scheduled), e.g. to change
   * the map. A checkable action is toggled before the callback runs.
   */
  std::function<void(ui::MapDocument&, EditorAction&)> onInvoke;

  Result<std::vector<EditorAction>> actions(
    ui::MapDocument& document, const std::optional<std::string>& viewId) override;
  Result<EditorAction> invokeAction(
    ui::MapDocument& document,
    const std::string& path,
    const std::optional<std::string>& viewId,
    bool deferred) override;
};

} // namespace tb::mcp
