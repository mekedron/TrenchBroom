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

#include <QObject>
#include <QString>
#include <QTimer>

#include "mcp/StreamableHttp.h"

#include <chrono>
#include <memory>
#include <unordered_map>

class QTcpServer;
class QTcpSocket;

namespace tb
{
namespace mcp
{
class Endpoint;
} // namespace mcp

namespace ui
{

/**
 * Serves the MCP Streamable HTTP transport over TCP. Adapts a QTcpServer and one
 * QTcpSocket per connection to the Qt-free mcp::StreamableHttpServer, which handles all
 * HTTP and MCP logic. Everything runs on the thread that owns this object (the main
 * thread); there are no extra threads.
 */
class McpTcpTransport : public QObject
{
  Q_OBJECT
public:
  static constexpr auto KeepAliveInterval = std::chrono::seconds{15};

private:
  class SocketConnection;

  mcp::Endpoint& m_endpoint;
  mcp::StreamableHttpServer::Config m_config;
  QTcpServer* m_tcpServer = nullptr;
  std::unique_ptr<mcp::StreamableHttpServer> m_httpServer;
  std::unordered_map<QTcpSocket*, std::unique_ptr<SocketConnection>> m_connections;
  QTimer m_keepAliveTimer;
  QString m_errorString;

public:
  /**
   * Creates a transport for the given endpoint. The bind address of the given config is
   * replaced by the address passed to `listen`.
   */
  McpTcpTransport(
    mcp::Endpoint& endpoint,
    mcp::StreamableHttpServer::Config config = {},
    QObject* parent = nullptr);
  ~McpTcpTransport() override;

  /**
   * Starts listening on the given address and port (0 = any free port). A non-loopback
   * address requires a non-empty access token, which clients must send as
   * `Authorization: Bearer <token>`. The token is `accessToken` if it is not empty, or
   * the token of the config passed to the constructor otherwise. Stops listening first
   * if already listening.
   *
   * Returns false and sets errorString() on failure, e.g. if the port is in use.
   */
  bool listen(
    const QString& bindAddress, quint16 port, const QString& accessToken = QString{});

  bool isListening() const;

  /**
   * Returns the port the server listens on, or 0 if it is not listening.
   */
  quint16 serverPort() const;

  /**
   * Returns a description of the last error.
   */
  const QString& errorString() const;

  /**
   * Closes all connections and stops listening.
   */
  void close();

  /**
   * Closes all connections, but keeps listening.
   */
  void closeAllConnections();

  int connectionCount() const;

signals:
  void connectionCountChanged(int count);

private:
  void acceptConnections();
  void socketDisconnected(QTcpSocket* socket);
};

} // namespace ui
} // namespace tb
