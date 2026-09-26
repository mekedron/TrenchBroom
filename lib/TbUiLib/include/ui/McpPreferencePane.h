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

#include "ui/PreferencePane.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace tb::ui
{
class AppController;

/**
 * The "AI Agents (MCP)" preference pane: enables the MCP server and configures its
 * network settings and logging.
 */
class McpPreferencePane : public PreferencePane
{
  Q_OBJECT
private:
  AppController& m_appController;
  QCheckBox* m_enabled = nullptr;
  QSpinBox* m_port = nullptr;
  QLineEdit* m_bindAddress = nullptr;
  QLineEdit* m_accessToken = nullptr;
  QCheckBox* m_logToFile = nullptr;
  QSpinBox* m_busyWaitTimeout = nullptr;
  QLabel* m_status = nullptr;

public:
  explicit McpPreferencePane(AppController& appController, QWidget* parent = nullptr);

private:
  void createGui();
  QWidget* createMcpPreferences();
  void updateStatus();

  bool canResetToDefaults() override;
  void doResetToDefaults() override;
  void updateControls() override;
  bool validate() override;
};

} // namespace tb::ui
