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

#include "ui/McpTcpTransport.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <string_view>
#include <utility>

namespace tb::ui
{

class McpTcpTransport::SocketConnection : public mcp::HttpConnection
{
private:
  QTcpSocket* m_socket;

public:
  explicit SocketConnection(QTcpSocket* socket)
    : m_socket{socket}
  {
  }

  void write(const std::string_view bytes) override
  {
    m_socket->write(bytes.data(), qint64(bytes.size()));
  }

  void close() override
  {
    // Flushes pending data first. May emit disconnected synchronously.
    m_socket->disconnectFromHost();
  }
};

McpTcpTransport::McpTcpTransport(
  mcp::Endpoint& endpoint, mcp::StreamableHttpServer::Config config, QObject* parent)
  : QObject{parent}
  , m_endpoint{endpoint}
  , m_config{std::move(config)}
{
  m_keepAliveTimer.setInterval(KeepAliveInterval);
  connect(&m_keepAliveTimer, &QTimer::timeout, this, [&]() {
    if (m_httpServer)
    {
      m_httpServer->sendKeepAlives();
    }
  });
}

McpTcpTransport::~McpTcpTransport()
{
  close();
}

bool McpTcpTransport::listen(
  const QString& bindAddress, const quint16 port, const QString& accessToken)
{
  close();
  m_errorString.clear();

  const auto address = bindAddress.compare("localhost", Qt::CaseInsensitive) == 0
                         ? QHostAddress{QHostAddress::LocalHost}
                         : QHostAddress{bindAddress};
  if (address.isNull())
  {
    m_errorString = QString{"Invalid bind address: %1"}.arg(bindAddress);
    return false;
  }

  auto config = m_config;
  config.bindAddress = bindAddress.toStdString();
  if (!accessToken.isEmpty())
  {
    config.accessToken = accessToken.toStdString();
  }
  if (const auto error = mcp::StreamableHttpServer::validateConfig(config))
  {
    m_errorString = QString::fromStdString(*error);
    return false;
  }

  auto* tcpServer = new QTcpServer{this};
  if (!tcpServer->listen(address, port))
  {
    m_errorString = QString{"Could not listen on %1:%2: %3"}
                      .arg(bindAddress)
                      .arg(port)
                      .arg(tcpServer->errorString());
    delete tcpServer;
    return false;
  }

  m_tcpServer = tcpServer;
  m_httpServer =
    std::make_unique<mcp::StreamableHttpServer>(m_endpoint, std::move(config));
  connect(
    m_tcpServer, &QTcpServer::newConnection, this, &McpTcpTransport::acceptConnections);
  m_keepAliveTimer.start();
  return true;
}

bool McpTcpTransport::isListening() const
{
  return m_tcpServer && m_tcpServer->isListening();
}

quint16 McpTcpTransport::serverPort() const
{
  return isListening() ? m_tcpServer->serverPort() : 0;
}

const QString& McpTcpTransport::errorString() const
{
  return m_errorString;
}

void McpTcpTransport::close()
{
  m_keepAliveTimer.stop();
  closeAllConnections();

  if (m_tcpServer)
  {
    m_tcpServer->close();
    m_tcpServer->deleteLater();
    m_tcpServer = nullptr;
  }
  m_httpServer.reset();
}

void McpTcpTransport::closeAllConnections()
{
  if (m_connections.empty())
  {
    return;
  }

  auto connections = std::exchange(m_connections, {});
  if (m_httpServer)
  {
    for (const auto& [socket, connection] : connections)
    {
      m_httpServer->connectionClosed(*connection);
    }
  }

  for (const auto& [socket, connection] : connections)
  {
    socket->disconnect(this);
    socket->disconnectFromHost();
    socket->deleteLater();
  }

  emit connectionCountChanged(0);
}

int McpTcpTransport::connectionCount() const
{
  return int(m_connections.size());
}

void McpTcpTransport::acceptConnections()
{
  while (auto* socket = m_tcpServer->nextPendingConnection())
  {
    auto connection = std::make_unique<SocketConnection>(socket);
    m_httpServer->openConnection(*connection);
    m_connections.emplace(socket, std::move(connection));

    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
      const auto it = m_connections.find(socket);
      if (it != m_connections.end() && m_httpServer)
      {
        const auto bytes = socket->readAll();
        m_httpServer->feed(
          *it->second, std::string_view{bytes.constData(), size_t(bytes.size())});
      }
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
      socketDisconnected(socket);
    });

    emit connectionCountChanged(connectionCount());
  }
}

void McpTcpTransport::socketDisconnected(QTcpSocket* socket)
{
  const auto it = m_connections.find(socket);
  if (it == m_connections.end())
  {
    return;
  }

  // The HTTP server may be calling into the connection right now (close() can emit
  // disconnected synchronously), so the connection object is destroyed later.
  auto connection = std::move(it->second);
  m_connections.erase(it);
  if (m_httpServer)
  {
    m_httpServer->connectionClosed(*connection);
  }

  socket->disconnect(this);
  QObject::connect(
    socket, &QObject::destroyed, [c = connection.release()]() { delete c; });
  socket->deleteLater();

  emit connectionCountChanged(connectionCount());
}

} // namespace tb::ui
