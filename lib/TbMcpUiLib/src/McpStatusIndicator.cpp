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

#include "ui/McpStatusIndicator.h"

#include <QBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "ui/McpServerController.h"
#include "ui/ViewConstants.h"

namespace tb::ui
{

McpStatusIndicator::McpStatusIndicator(McpServerController& controller, QWidget* parent)
  : QWidget{parent}
  , m_controller{controller}
{
  createGui();

  connect(
    &m_controller,
    &McpServerController::clientsChanged,
    this,
    &McpStatusIndicator::updateContents);
  connect(
    &m_controller,
    &McpServerController::activityChanged,
    this,
    &McpStatusIndicator::updateContents);
  connect(
    &m_controller,
    &McpServerController::statusChanged,
    this,
    &McpStatusIndicator::updateContents);

  updateContents();
}

QString McpStatusIndicator::text() const
{
  return m_label->text();
}

void McpStatusIndicator::createGui()
{
  m_label = new QLabel{};
  m_label->setObjectName("McpStatusIndicator_Label");

  m_stopButton = new QPushButton{tr("Stop agent")};
  m_stopButton->setObjectName("McpStatusIndicator_StopButton");
  m_stopButton->setToolTip(
    tr("Cancel all pending AI agent calls, roll back open agent transactions and "
       "disconnect all agents"));
  m_stopButton->setFlat(true);
  connect(
    m_stopButton, &QPushButton::clicked, this, [&]() { m_controller.stopAgents(); });

  auto* layout = new QHBoxLayout{};
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(LayoutConstants::NarrowHMargin);
  layout->addWidget(m_label);
  layout->addWidget(m_stopButton);
  setLayout(layout);
}

void McpStatusIndicator::updateContents()
{
  if (!m_controller.enabled())
  {
    hide();
    return;
  }

  if (m_controller.listening())
  {
    m_label->setText(m_controller.activityText());
    m_stopButton->setVisible(m_controller.clientCount() > 0);
  }
  else
  {
    m_label->setText(m_controller.statusText());
    m_stopButton->setVisible(false);
  }

  show();
}

} // namespace tb::ui
