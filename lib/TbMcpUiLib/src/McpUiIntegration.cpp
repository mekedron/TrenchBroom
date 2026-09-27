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

#include "ui/McpUiIntegration.h"

#include <QStatusBar>

#include "ui/ImageUtils.h"
#include "ui/MapWindow.h"
#include "ui/McpPreferencePane.h"
#include "ui/McpStatusIndicator.h"
#include "ui/PreferenceDialog.h"

namespace tb::ui
{

McpStatusIndicator* addMcpStatusIndicator(
  MapWindow& mapWindow, McpServerController& controller)
{
  auto* statusBar = mapWindow.statusBar();
  if (auto* indicator = statusBar->findChild<McpStatusIndicator*>())
  {
    return indicator;
  }

  auto* indicator = new McpStatusIndicator{controller};
  statusBar->addWidget(indicator);
  return indicator;
}

McpPreferencePane* addMcpPreferencePane(
  PreferenceDialog& dialog, McpServerController& controller)
{
  if (auto* pane = dialog.findChild<McpPreferencePane*>())
  {
    return pane;
  }

  auto* pane = new McpPreferencePane{controller};
  dialog.addPane(loadSVGIcon("McpPreferences.svg"), QObject::tr("AI Agents"), pane);
  return pane;
}

} // namespace tb::ui
