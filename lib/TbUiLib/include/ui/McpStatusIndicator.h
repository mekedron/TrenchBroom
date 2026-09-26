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

#include <QWidget>

class QLabel;
class QPushButton;

namespace tb::ui
{
class McpServerController;

/**
 * Shows the MCP server activity in the status bar, e.g. "AI: 1 client · idle", and offers
 * a button to stop all agents. Hidden while the server is disabled.
 */
class McpStatusIndicator : public QWidget
{
  Q_OBJECT
private:
  McpServerController& m_controller;
  QLabel* m_label = nullptr;
  QPushButton* m_stopButton = nullptr;

public:
  explicit McpStatusIndicator(McpServerController& controller, QWidget* parent = nullptr);

  QString text() const;

private:
  void createGui();
  void updateContents();
};

} // namespace tb::ui
