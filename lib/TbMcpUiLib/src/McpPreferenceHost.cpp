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

#include "ui/McpPreferenceHost.h"

#include "mdl/EntityDefinitionManager.h"
#include "mdl/Map.h"
#include "mdl/TagManager.h"
#include "ui/Action.h"
#include "ui/ActionManager.h"
#include "ui/ActionMenu.h"
#include "ui/AppController.h"
#include "ui/MapDocument.h"
#include "ui/McpPreferences.h"

#include "kd/overload.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <string>
#include <unordered_set>

namespace tb::ui
{
namespace
{

constexpr auto ConnectionLocked =
  "changing it stops or restarts the MCP server while the call runs, which ends the "
  "agent's connection; ask the user to change it in the preferences (MCP)";

std::vector<mcp::HostPreference> mcpPreferences()
{
  using namespace McpPreferences;
  return {
    mcp::HostPreference{
      .preference = &McpServerEnabled,
      .category = "mcp",
      .description = "Run the MCP server for AI agents.",
      .lockedReason = ConnectionLocked,
    },
    mcp::HostPreference{
      .preference = &McpServerPort,
      .category = "mcp",
      .description = "TCP port of the MCP server (0 picks a free port).",
      .minimum = 0.0,
      .maximum = 65535.0,
      .lockedReason = ConnectionLocked,
    },
    mcp::HostPreference{
      .preference = &McpServerBindAddress,
      .category = "mcp",
      .description = "Address the MCP server listens on, e.g. 127.0.0.1.",
      .lockedReason = ConnectionLocked,
    },
    mcp::HostPreference{
      .preference = &McpServerAccessToken,
      .category = "mcp",
      .description = "Access token that clients must send (empty: none).",
      .lockedReason = ConnectionLocked,
      .secret = true,
    },
    mcp::HostPreference{
      .preference = &McpLogToFile,
      .category = "mcp",
      .description =
        "Write the MCP call log to JSONL files in the user data folder (mcp-logs).",
    },
    mcp::HostPreference{
      .preference = &McpBusyWaitTimeoutMs,
      .category = "mcp",
      .description = "How long modifying calls wait while the user is busy, in "
                     "milliseconds, before they fail with BUSY_TIMEOUT.",
      .minimum = 0.0,
    },
  };
}

mcp::HostPreference shortcutPreference(Action& action, std::string description)
{
  return mcp::HostPreference{
    .preference = &action.preference(),
    .category = "keyboard",
    .description = std::move(description),
  };
}

} // namespace

McpPreferenceHost::McpPreferenceHost(AppController& appController)
  : m_appController{appController}
{
}

McpPreferenceHost::~McpPreferenceHost() = default;

std::vector<mcp::HostPreference> McpPreferenceHost::preferences(MapDocument* document)
{
  auto result = mcpPreferences();
  auto paths = std::unordered_set<std::filesystem::path>{};

  const auto add = [&](Action& action, std::string description) {
    if (paths.insert(action.preference().path).second)
    {
      result.push_back(shortcutPreference(action, std::move(description)));
    }
  };

  auto& actionManager = m_appController.actionManager();

  auto menuPath = std::vector<std::string>{};
  actionManager.visitMainMenu(kdl::overload(
    [](MenuSeparator&) {},
    [&](MenuAction& item) {
      auto labels = menuPath;
      labels.push_back(item.action.label());
      add(item.action, fmt::format("Menu: {}", kdl::str_join(labels, " > ")));
    },
    [&](auto& thisLambda, Menu& menu) {
      menuPath.push_back(menu.name);
      menu.visitEntries(thisLambda);
      menuPath.pop_back();
    }));

  actionManager.visitMapViewActions(
    [&](Action& action) { add(action, fmt::format("Map view: {}", action.label())); });

  m_documentActions.clear();
  if (document)
  {
    auto& map = document->map();
    m_documentActions = actionManager.createTagActions(map.tagManager().smartTags());
    auto entityActions = actionManager.createEntityDefinitionActions(
      map.entityDefinitionManager().definitions());
    const auto tagActionCount = m_documentActions.size();
    for (auto& action : entityActions)
    {
      m_documentActions.push_back(std::move(action));
    }

    for (size_t i = 0; i < m_documentActions.size(); ++i)
    {
      auto& action = m_documentActions[i];
      add(
        action,
        fmt::format(
          "{}: {}",
          i < tagActionCount ? "Smart tag" : "Entity definition",
          action.label()));
    }
  }

  return result;
}

} // namespace tb::ui
