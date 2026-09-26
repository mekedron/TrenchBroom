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

#include "ui/McpPreferencePane.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSpinBox>

#include "base/PreferenceManager.h"
#include "mcp/StreamableHttp.h"
#include "prefs/Preferences.h"
#include "ui/AppController.h"
#include "ui/FormWithSectionsLayout.h"
#include "ui/McpServerController.h"
#include "ui/ViewConstants.h"

namespace tb::ui
{
namespace
{

/**
 * Stores the line edit's text in the given preference. When preferences are saved
 * instantly, the text is only stored when editing finishes, so that the server isn't
 * restarted on every key press.
 */
void connectLineEdit(QLineEdit* lineEdit, Preference<std::string>& preference)
{
  auto& prefs = PreferenceManager::instance();
  const auto setPreference = [lineEdit, &preference]() {
    PreferenceManager::instance().set(
      preference, lineEdit->text().trimmed().toStdString());
  };

  if (prefs.saveInstantly())
  {
    QObject::connect(lineEdit, &QLineEdit::editingFinished, lineEdit, setPreference);
  }
  else
  {
    QObject::connect(lineEdit, &QLineEdit::textChanged, lineEdit, setPreference);
  }
}

bool isLoopbackAddress(const std::string& address)
{
  return mcp::StreamableHttpServer::isLoopbackAddress(address);
}

} // namespace

McpPreferencePane::McpPreferencePane(AppController& appController, QWidget* parent)
  : PreferencePane{parent}
  , m_appController{appController}
{
  createGui();

  auto& controller = m_appController.mcpServerController();
  connect(
    &controller,
    &McpServerController::statusChanged,
    this,
    &McpPreferencePane::updateStatus);
  connect(
    &controller,
    &McpServerController::clientsChanged,
    this,
    &McpPreferencePane::updateStatus);
}

void McpPreferencePane::createGui()
{
  auto* mcpPreferences = createMcpPreferences();

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(QMargins{});
  layout->setSpacing(0);
  layout->addSpacing(LayoutConstants::NarrowVMargin);
  layout->addWidget(mcpPreferences, 1);
  layout->addSpacing(LayoutConstants::MediumVMargin);

  createScrollableContent(layout);
}

QWidget* McpPreferencePane::createMcpPreferences()
{
  auto* info = new QLabel{tr(
    R"(TrenchBroom can run a Model Context Protocol (MCP) server that lets AI agents such as Claude inspect and edit the open maps.
Every change an agent makes is a regular undo step. The status bar shows what the agents are doing and lets you stop them.)")};
  info->setWordWrap(true);

  m_enabled = new QCheckBox{};
  connect(m_enabled, &QCheckBox::checkStateChanged, this, [](const auto state) {
    PreferenceManager::instance().set(
      Preferences::McpServerEnabled, state == Qt::Checked);
  });

  m_status = new QLabel{};
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

  m_port = new QSpinBox{};
  m_port->setRange(1, 65535);
  m_port->setKeyboardTracking(false);
  connect(m_port, &QSpinBox::valueChanged, this, [](const int value) {
    PreferenceManager::instance().set(Preferences::McpServerPort, value);
  });

  m_bindAddress = new QLineEdit{};
  m_bindAddress->setPlaceholderText("127.0.0.1");
  connectLineEdit(m_bindAddress, Preferences::McpServerBindAddress);

  m_accessToken = new QLineEdit{};
  m_accessToken->setEchoMode(QLineEdit::PasswordEchoOnEdit);
  connectLineEdit(m_accessToken, Preferences::McpServerAccessToken);

  auto* networkInfo = new QLabel{tr(
    R"(By default, the server only accepts connections from this computer (127.0.0.1).
If you bind it to another address, agents must send the access token as a bearer token.)")};
  networkInfo->setWordWrap(true);

  m_logToFile = new QCheckBox{};
  connect(m_logToFile, &QCheckBox::checkStateChanged, this, [](const auto state) {
    PreferenceManager::instance().set(Preferences::McpLogToFile, state == Qt::Checked);
  });

  m_busyWaitTimeout = new QSpinBox{};
  m_busyWaitTimeout->setRange(1, 3600);
  m_busyWaitTimeout->setSuffix(tr(" s"));
  m_busyWaitTimeout->setKeyboardTracking(false);
  connect(m_busyWaitTimeout, &QSpinBox::valueChanged, this, [](const int value) {
    PreferenceManager::instance().set(Preferences::McpBusyWaitTimeoutMs, value * 1000);
  });

  auto* busyInfo = new QLabel{tr(
    R"(While you are dragging or a dialog is open, agent edits wait. After this time, they fail.)")};
  busyInfo->setWordWrap(true);

  auto* layout = new FormWithSectionsLayout{};
  layout->setContentsMargins(
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin,
    LayoutConstants::DialogOuterMargin);
  layout->setVerticalSpacing(LayoutConstants::WideVMargin);

  layout->addSection(tr("AI Agents (MCP)"));
  layout->addRow(info);
  layout->addRow(tr("Enable MCP server"), m_enabled);
  layout->addRow(tr("Status"), m_status);

  layout->addSection(tr("Network"));
  layout->addRow(tr("Port"), m_port);
  layout->addRow(tr("Bind address"), m_bindAddress);
  layout->addRow(tr("Access token"), m_accessToken);
  layout->addRow(networkInfo);

  layout->addSection(tr("Behavior"));
  layout->addRow(tr("Busy wait timeout"), m_busyWaitTimeout);
  layout->addRow(busyInfo);
  layout->addRow(tr("Log agent calls to file"), m_logToFile);

  auto* widget = new QWidget{};
  widget->setLayout(layout);
  return widget;
}

void McpPreferencePane::updateStatus()
{
  const auto& controller = m_appController.mcpServerController();
  if (controller.listening())
  {
    m_status->setText(tr("Listening on port %1 (%2)")
                        .arg(*controller.port())
                        .arg(controller.activityText()));
  }
  else if (!controller.statusText().isEmpty())
  {
    m_status->setText(controller.statusText());
  }
  else
  {
    m_status->setText(controller.enabled() ? tr("Not running") : tr("Disabled"));
  }
}

bool McpPreferencePane::canResetToDefaults()
{
  return true;
}

void McpPreferencePane::doResetToDefaults()
{
  auto& prefs = PreferenceManager::instance();
  prefs.resetToDefault(Preferences::McpServerEnabled);
  prefs.resetToDefault(Preferences::McpServerPort);
  prefs.resetToDefault(Preferences::McpServerBindAddress);
  prefs.resetToDefault(Preferences::McpServerAccessToken);
  prefs.resetToDefault(Preferences::McpLogToFile);
  prefs.resetToDefault(Preferences::McpBusyWaitTimeoutMs);
}

void McpPreferencePane::updateControls()
{
  const auto enabledBlocker = QSignalBlocker{m_enabled};
  const auto portBlocker = QSignalBlocker{m_port};
  const auto bindAddressBlocker = QSignalBlocker{m_bindAddress};
  const auto accessTokenBlocker = QSignalBlocker{m_accessToken};
  const auto logToFileBlocker = QSignalBlocker{m_logToFile};
  const auto busyWaitTimeoutBlocker = QSignalBlocker{m_busyWaitTimeout};

  auto& prefs = PreferenceManager::instance();

  m_enabled->setChecked(prefs.getPendingValue(Preferences::McpServerEnabled));
  m_port->setValue(prefs.getPendingValue(Preferences::McpServerPort));

  const auto bindAddress =
    QString::fromStdString(prefs.getPendingValue(Preferences::McpServerBindAddress));
  if (m_bindAddress->text().trimmed() != bindAddress)
  {
    m_bindAddress->setText(bindAddress);
  }

  const auto accessToken =
    QString::fromStdString(prefs.getPendingValue(Preferences::McpServerAccessToken));
  if (m_accessToken->text().trimmed() != accessToken)
  {
    m_accessToken->setText(accessToken);
  }

  m_logToFile->setChecked(prefs.getPendingValue(Preferences::McpLogToFile));
  m_busyWaitTimeout->setValue(
    prefs.getPendingValue(Preferences::McpBusyWaitTimeoutMs) / 1000);

  updateStatus();
}

bool McpPreferencePane::validate()
{
  auto& prefs = PreferenceManager::instance();
  const auto& bindAddress = prefs.getPendingValue(Preferences::McpServerBindAddress);
  const auto& accessToken = prefs.getPendingValue(Preferences::McpServerAccessToken);

  if (
    prefs.getPendingValue(Preferences::McpServerEnabled)
    && !isLoopbackAddress(bindAddress) && accessToken.empty())
  {
    QMessageBox::warning(
      this,
      tr("AI Agents (MCP)"),
      tr("The MCP server is bound to %1, which is reachable from other computers. Please "
         "enter an access token or bind the server to 127.0.0.1.")
        .arg(QString::fromStdString(bindAddress)));
    return false;
  }

  return true;
}

} // namespace tb::ui
