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

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb::mcp
{

/**
 * A view host for tests: four views (3d, xy, xz, yz) with plain cameras. setCamera and
 * setMaximizedView change them like the editor would, including linked 2D cameras
 * (the "Link 2D cameras" preference), and record their calls.
 */
class FakeViewHost : public ViewHost
{
public:
  /** Returned by views(); setCamera changes the cameras in it. */
  std::vector<UserView> viewList;
  ViewLayout currentLayout;
  /** If false, the document has no window: views() is empty and the rest fails. */
  bool hasWindow = true;

  /** The calls of setCamera and setMaximizedView, in order. */
  std::vector<std::pair<std::string, AgentCamera>> setCameraCalls;
  std::vector<std::optional<std::string>> setMaximizedViewCalls;
  /** The number of prepareForLayoutChange calls. */
  size_t prepareForLayoutChangeCount = 0;

  FakeViewHost();

  std::vector<UserView> views(ui::MapDocument& document) override;
  Result<void> setCamera(
    ui::MapDocument& document,
    const std::string& viewId,
    const AgentCamera& camera) override;
  Result<ViewLayout> layout(ui::MapDocument& document) override;
  Result<void> setMaximizedView(
    ui::MapDocument& document, const std::optional<std::string>& viewId) override;
  void prepareForLayoutChange() override;

  /** The view with the given id. Precondition: it exists. */
  UserView& view(const std::string& viewId);
};

} // namespace tb::mcp
