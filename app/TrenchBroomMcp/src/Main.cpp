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

// TrenchBroomMcp: a stdio <-> Streamable HTTP bridge for the TrenchBroom MCP server.
//
// MCP clients that only support the stdio transport launch this executable. It reads
// newline-delimited JSON-RPC messages from stdin and writes the responses and
// notifications to stdout, one message per line. It answers initialize and the list
// requests itself (BridgeSession), so that starting a client does not start the editor.
// The first message that needs the editor opens a session with the editor's MCP
// endpoint (http://127.0.0.1:<port>/mcp); from then on the bridge forwards the messages.
// It finds the editor through the discovery file that the editor writes while its MCP
// server listens, and launches the editor if necessary.
//
// stdout is reserved for the protocol; all diagnostics go to stderr.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "mcp/BridgeSession.h"
#include "mcp/Json.h"
#include "mcp/SseParser.h"
#include "version/Version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace tb::mcp
{
namespace
{

using namespace std::chrono_literals;

constexpr auto DiscoveryPollInterval = 250ms;
constexpr auto LaunchTimeout = 30s;
constexpr auto DeleteTimeout = 2s;
constexpr auto ReconnectDelays = std::array{1s, 2s, 5s};

constexpr auto SessionIdHeader = "Mcp-Session-Id";
constexpr auto ProtocolVersionHeader = "MCP-Protocol-Version";

void logMessage(const QString& message)
{
  std::cerr << "TrenchBroomMcp: " << message.toStdString() << std::endl;
}

/**
 * Returns TrenchBroom's user data directory. This must match
 * tb::ui::SystemPaths::userDataDirectory (non-portable mode); it relies on the
 * application and organization names being set exactly like the editor sets them.
 */
QString userDataDirectory()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
  return QDir::homePath() + "/.TrenchBroom";
#else
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
}

QString discoveryFilePath()
{
  return QDir{userDataDirectory()}.filePath("mcp-server.json");
}

/**
 * Reads the port from the discovery file, or returns nullopt if the file does not exist
 * or is invalid.
 */
std::optional<quint16> readDiscoveryFile()
{
  auto file = QFile{discoveryFilePath()};
  if (!file.open(QIODevice::ReadOnly))
  {
    return std::nullopt;
  }

  const auto bytes = file.readAll();
  const auto json = parseJson(std::string_view{bytes.constData(), size_t(bytes.size())});
  const auto* port = json ? findMember(*json, "port") : nullptr;
  if (!port || !port->is_number_integer())
  {
    return std::nullopt;
  }

  const auto value = port->get<int>();
  return value > 0 && value <= 65535 ? std::optional{quint16(value)} : std::nullopt;
}

std::optional<QDateTime> discoveryFileModificationTime()
{
  const auto info = QFileInfo{discoveryFilePath()};
  return info.exists() ? std::optional{info.lastModified()} : std::nullopt;
}

std::string toStdString(const QByteArray& bytes)
{
  return std::string{bytes.constData(), size_t(bytes.size())};
}

/**
 * Returns the given JSON text as a single line. Compacts it if it is valid JSON.
 */
std::string toLine(const std::string& text)
{
  if (const auto json = parseJson(text))
  {
    return dumpJson(*json);
  }

  auto result = text;
  std::ranges::replace(result, '\n', ' ');
  std::ranges::replace(result, '\r', ' ');
  return result;
}

void writeLine(const std::string& text)
{
  const auto line = toLine(text) + "\n";
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fflush(stdout);
}

/**
 * Returns the ids of the requests in the given JSON-RPC message or batch.
 */
std::vector<Json> requestIds(const Json& message)
{
  auto result = std::vector<Json>{};
  const auto collect = [&](const Json& m) {
    if (m.is_object() && m.contains("method") && m.contains("id"))
    {
      result.push_back(m["id"]);
    }
  };

  if (message.is_array())
  {
    for (const auto& m : message)
    {
      collect(m);
    }
  }
  else
  {
    collect(message);
  }
  return result;
}

struct Options
{
  std::optional<quint16> port;
  bool noLaunch = false;
  QString editorPath;
};

struct PendingMessage
{
  std::string text;
  Json json;
  /** Whether the message was queued again because the editor session had expired. */
  bool retried = false;

  std::string method() const { return json.is_object() ? json.value("method", "") : ""; }
};

/** The outcome of a POST of a handshake message. */
struct HandshakeReply
{
  int status = 0;
  QNetworkReply::NetworkError error = QNetworkReply::NoError;
  QString errorString;
  std::string sessionId;
  /** The JSON-RPC response to the posted request, or null. */
  Json response;
};

enum class EditorSession
{
  /** The bridge has no session with the editor. */
  None,
  /** The bridge is sending the handshake (BridgeSession::handshake). */
  Opening,
  Open,
};

const auto NotRunningMessage = QString{
  "TrenchBroom is not running. Start TrenchBroom with its MCP server enabled "
  "(Preferences > AI Agents > Enable MCP server, or trenchbroom --mcp-server) and try "
  "again."};

bool isConnectionRefused(const QNetworkReply::NetworkError error)
{
  return error == QNetworkReply::ConnectionRefusedError
         || error == QNetworkReply::HostNotFoundError;
}

class Bridge : public QObject
{
private:
  Options m_options;
  BridgeSession m_session;
  QNetworkAccessManager m_network;

  /** The port of the editor, if known. */
  std::optional<quint16> m_port;
  EditorSession m_editorSession = EditorSession::None;
  std::string m_sessionId;
  std::string m_protocolVersion;

  /** Messages that wait for the editor session. */
  std::deque<PendingMessage> m_queue;
  /** The handshake messages that remain to be sent, and the list results received. */
  std::deque<Json> m_handshake;
  Json m_editorTools;
  Json m_editorResources;

  bool m_launching = false;
  std::optional<QDateTime> m_staleDiscoveryTime;
  QElapsedTimer m_launchTimer;
  QTimer m_pollTimer;

  QNetworkReply* m_streamReply = nullptr;
  size_t m_streamAttempts = 0;
  QTimer m_reconnectTimer;

  bool m_shuttingDown = false;

public:
  explicit Bridge(Options options)
    : m_options{std::move(options)}
    , m_session{VERSION_STR}
  {
    m_pollTimer.setInterval(DiscoveryPollInterval);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() { pollDiscoveryFile(); });

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() { openStream(); });
  }

  void receive(std::string line)
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    {
      line.pop_back();
    }
    if (m_shuttingDown || line.find_first_not_of(" \t") == std::string::npos)
    {
      return;
    }

    auto json = parseJson(line);
    if (!json)
    {
      // Answer like a JSON-RPC server would; the editor cannot do better
      writeLine(dumpJson(Json{
        {"jsonrpc", "2.0"},
        {"id", nullptr},
        {"error", {{"code", -32700}, {"message", "Parse error"}}}}));
      return;
    }

    const auto route = m_session.route(*json, m_editorSession == EditorSession::Open);
    if (route == BridgeRoute::Bridge)
    {
      if (json->is_object() && json->value("method", "") == "initialize")
      {
        // The client starts a new session; the editor session of the old one is useless
        closeEditorSession();
      }
      if (const auto response = m_session.answer(*json))
      {
        writeLine(dumpJson(*response));
      }
      return;
    }

    if (route == BridgeRoute::EditorIfConnected && m_queue.empty())
    {
      // Only the editor could use the message, and no session with it exists
      return;
    }

    m_session.forwarded(*json);
    m_queue.push_back({std::move(line), std::move(*json)});
    pump();
  }

  void endOfInput()
  {
    m_shuttingDown = true;
    m_pollTimer.stop();
    m_reconnectTimer.stop();
    if (m_streamReply)
    {
      m_streamReply->abort();
    }

    if (m_sessionId.empty() || !m_port)
    {
      QCoreApplication::quit();
      return;
    }

    // Terminate the session, best effort
    auto request = makeRequest();
    request.setTransferTimeout(int(std::chrono::milliseconds{DeleteTimeout}.count()));
    auto* reply = m_network.deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, []() { QCoreApplication::quit(); });
    QTimer::singleShot(DeleteTimeout + 1s, this, []() { QCoreApplication::quit(); });
  }

private:
  QNetworkRequest makeRequest() const
  {
    auto url = QUrl{};
    url.setScheme("http");
    url.setHost("127.0.0.1");
    url.setPort(m_port.value_or(0));
    url.setPath("/mcp");

    auto request = QNetworkRequest{url};
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    if (!m_sessionId.empty())
    {
      request.setRawHeader(SessionIdHeader, QByteArray::fromStdString(m_sessionId));
    }
    if (!m_protocolVersion.empty())
    {
      request.setRawHeader(
        ProtocolVersionHeader, QByteArray::fromStdString(m_protocolVersion));
    }
    return request;
  }

  QNetworkRequest makePostRequest() const
  {
    auto request = makeRequest();
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");
    return request;
  }

  /**
   * Forwards queued messages as far as possible, connecting to the editor first.
   */
  void pump()
  {
    while (!m_queue.empty() && !m_shuttingDown
           && m_editorSession != EditorSession::Opening)
    {
      if (!m_port)
      {
        m_port = m_options.port ? m_options.port : readDiscoveryFile();
        if (!m_port)
        {
          editorUnavailable();
          return;
        }
      }

      if (m_editorSession == EditorSession::None)
      {
        openEditorSession();
        return;
      }

      auto message = std::move(m_queue.front());
      m_queue.pop_front();
      forward(std::move(message));
    }
  }

  /**
   * Opens a session with the editor for the client: sends the client's initialize
   * parameters and replays its state, then forwards the queued messages.
   */
  void openEditorSession()
  {
    m_editorSession = EditorSession::Opening;
    m_sessionId.clear();
    m_protocolVersion.clear();
    m_editorTools = nullptr;
    m_editorResources = nullptr;

    const auto messages = m_session.handshake();
    m_handshake = std::deque<Json>{messages.begin(), messages.end()};
    sendNextHandshakeMessage();
  }

  void sendNextHandshakeMessage()
  {
    if (m_shuttingDown)
    {
      return;
    }
    if (m_handshake.empty())
    {
      editorSessionOpened();
      return;
    }

    auto message = std::move(m_handshake.front());
    m_handshake.pop_front();

    const auto id = message.value("id", Json{});
    auto* reply =
      m_network.post(makePostRequest(), QByteArray::fromStdString(dumpJson(message)));
    connect(
      reply,
      &QNetworkReply::finished,
      this,
      [this, reply, id, method = message.value("method", "")]() {
        reply->deleteLater();
        handshakeReplied(method, readHandshakeReply(*reply, id));
      });
  }

  static HandshakeReply readHandshakeReply(QNetworkReply& reply, const Json& id)
  {
    auto result = HandshakeReply{
      reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
      reply.error(),
      reply.errorString(),
      toStdString(reply.rawHeader(SessionIdHeader)),
      nullptr,
    };

    auto messages = std::vector<Json>{};
    const auto body = reply.readAll();
    if (isEventStream(&reply))
    {
      auto parser = SseParser{};
      for (const auto& event :
           parser.feed(std::string_view{body.constData(), size_t(body.size())}))
      {
        if (auto json = event.event == "message" ? parseJson(event.data) : std::nullopt)
        {
          messages.push_back(std::move(*json));
        }
      }
    }
    else if (auto json = parseJson(toStdString(body)))
    {
      messages.push_back(std::move(*json));
    }

    for (auto& message : messages)
    {
      if (
        message.is_object()
        && (message.value("id", Json{}) == id || messages.size() == 1))
      {
        result.response = std::move(message);
      }
    }
    return result;
  }

  void handshakeReplied(const std::string& method, const HandshakeReply& reply)
  {
    if (m_shuttingDown)
    {
      return;
    }

    const auto fail = [&](const QString& text) {
      m_handshake.clear();
      m_editorSession = EditorSession::None;
      m_sessionId.clear();
      m_protocolVersion.clear();
      failQueued(text);
    };

    if (reply.status == 0)
    {
      if (isConnectionRefused(reply.error))
      {
        // The editor is not running (anymore); launch it or wait for it
        logMessage(
          QString{"Cannot connect to TrenchBroom on port %1"}.arg(m_port.value_or(0)));
        m_handshake.clear();
        m_editorSession = EditorSession::None;
        m_sessionId.clear();
        m_protocolVersion.clear();
        m_port = std::nullopt;
        editorUnavailable();
      }
      else
      {
        fail("Connection to TrenchBroom failed: " + reply.errorString);
      }
      return;
    }

    const auto succeeded = reply.status >= 200 && reply.status < 300;
    const auto* result = findMember(reply.response, "result");
    const auto* error = findMember(reply.response, "error");
    if (method == "initialize")
    {
      const auto* version = result ? findMember(*result, "protocolVersion") : nullptr;
      if (!succeeded || reply.sessionId.empty() || !version || !version->is_string())
      {
        fail(QString{"TrenchBroom did not accept the MCP session (HTTP %1)%2"}
               .arg(reply.status)
               .arg(error ? QString::fromStdString(": " + dumpJson(*error)) : QString{}));
        return;
      }
      m_sessionId = reply.sessionId;
      m_protocolVersion = version->get<std::string>();
    }
    else if (!succeeded)
    {
      fail(QString{"TrenchBroom returned HTTP %1 for %2 while connecting"}
             .arg(reply.status)
             .arg(QString::fromStdString(method)));
      return;
    }
    else if (method == "tools/list")
    {
      m_editorTools = result ? *result : Json{};
    }
    else if (method == "resources/list")
    {
      m_editorResources = result ? *result : Json{};
    }
    else if (error)
    {
      // e.g. a subscription of a document that is not open anymore
      logMessage(QString{"Could not replay %1: %2"}
                   .arg(QString::fromStdString(method))
                   .arg(QString::fromStdString(dumpJson(*error))));
    }

    sendNextHandshakeMessage();
  }

  void editorSessionOpened()
  {
    logMessage(QString{"Connected to TrenchBroom on port %1"}.arg(m_port.value_or(0)));
    m_editorSession = EditorSession::Open;
    for (const auto& notification :
         m_session.editorConnected(m_editorTools, m_editorResources))
    {
      writeLine(dumpJson(notification));
    }

    m_streamAttempts = 0;
    openStream();
    pump();
  }

  /**
   * Forgets the editor session after the editor has quit or dropped it. The next
   * message that needs the editor opens a new one.
   */
  void editorSessionLost()
  {
    if (m_editorSession != EditorSession::Open)
    {
      return;
    }

    m_editorSession = EditorSession::None;
    m_sessionId.clear();
    m_protocolVersion.clear();
    if (m_streamReply)
    {
      m_streamReply->abort();
    }

    // The bridge answers the list requests again
    for (const auto& notification : m_session.editorDisconnected())
    {
      writeLine(dumpJson(notification));
    }
  }

  /**
   * Closes the editor session because the client starts a new session.
   */
  void closeEditorSession()
  {
    if (m_editorSession != EditorSession::Open)
    {
      return;
    }

    auto* reply = m_network.deleteResource(makeRequest());
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);

    m_editorSession = EditorSession::None;
    m_sessionId.clear();
    m_protocolVersion.clear();
    if (m_streamReply)
    {
      m_streamReply->abort();
    }
    m_session.editorDisconnected();
  }

  /**
   * Queues a message again that the editor could not handle because it has quit or
   * dropped the session. The bridge answers it itself if it can.
   */
  void requeue(PendingMessage message)
  {
    if (m_session.route(message.json, false) == BridgeRoute::Bridge)
    {
      if (const auto response = m_session.answer(message.json))
      {
        writeLine(dumpJson(*response));
      }
      return;
    }
    m_queue.push_front(std::move(message));
  }

  void forward(PendingMessage message)
  {
    auto* reply =
      m_network.post(makePostRequest(), QByteArray::fromStdString(message.text));
    auto parser = std::make_shared<SseParser>();

    connect(reply, &QNetworkReply::readyRead, this, [reply, parser]() {
      if (isEventStream(reply))
      {
        forwardEvents(*parser, reply->readAll());
      }
    });
    connect(
      reply,
      &QNetworkReply::finished,
      this,
      [this,
       reply,
       parser,
       message = std::move(message),
       sessionId = m_sessionId]() mutable {
        reply->deleteLater();
        postFinished(*reply, *parser, std::move(message), sessionId);
      });
  }

  void postFinished(
    QNetworkReply& reply,
    SseParser& parser,
    PendingMessage message,
    const std::string& sessionId)
  {
    if (m_shuttingDown)
    {
      return;
    }

    const auto status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0)
    {
      if (isConnectionRefused(reply.error()))
      {
        // The editor is not running anymore; launch it or wait for it
        logMessage(
          QString{"Cannot connect to TrenchBroom on port %1"}.arg(m_port.value_or(0)));
        if (sessionId == m_sessionId)
        {
          editorSessionLost();
          m_port = std::nullopt;
        }
        requeue(std::move(message));
      }
      else
      {
        replyWithError(
          message, "Connection to TrenchBroom failed: " + reply.errorString());
      }
      pump();
      return;
    }

    if (isEventStream(&reply))
    {
      forwardEvents(parser, reply.readAll());
    }
    else if (status >= 200 && status < 300)
    {
      if (const auto body = reply.readAll(); !body.isEmpty())
      {
        writeLine(toStdString(body));
      }
    }
    else
    {
      const auto body = toStdString(reply.readAll());
      const auto json = parseJson(body);
      const auto* error = json ? findMember(*json, "error") : nullptr;
      auto text = error ? dumpJson(*error) : body;
      logMessage(QString{"HTTP %1 for %2: %3"}
                   .arg(status)
                   .arg(QString::fromStdString(message.method()))
                   .arg(QString::fromStdString(text)));

      if (status == 404 && !sessionId.empty())
      {
        // The editor was restarted or has dropped the session: open a new one
        if (sessionId == m_sessionId)
        {
          editorSessionLost();
        }
        if (!message.retried)
        {
          message.retried = true;
          requeue(std::move(message));
        }
        else
        {
          replyWithError(
            message,
            "The MCP session has expired again (was TrenchBroom restarted?).",
            error);
        }
      }
      else
      {
        replyWithError(
          message, QString{"TrenchBroom returned HTTP %1"}.arg(status), error);
      }
    }

    pump();
  }

  static bool isEventStream(const QNetworkReply* reply)
  {
    return reply->header(QNetworkRequest::ContentTypeHeader)
      .toString()
      .startsWith("text/event-stream", Qt::CaseInsensitive);
  }

  static void forwardEvents(SseParser& parser, const QByteArray& bytes)
  {
    for (const auto& event :
         parser.feed(std::string_view{bytes.constData(), size_t(bytes.size())}))
    {
      if (event.event == "message" && !event.data.empty())
      {
        writeLine(event.data);
      }
    }
  }

  /**
   * Answers every request in the given message with a JSON-RPC error.
   */
  static void replyWithError(
    const PendingMessage& message, const QString& text, const Json* error = nullptr)
  {
    const auto ids = requestIds(message.json);
    if (ids.empty())
    {
      logMessage("Dropping message: " + text);
      return;
    }

    auto responses = Json::array();
    for (const auto& id : ids)
    {
      auto errorObject = error && error->is_object()
                           ? *error
                           : Json{{"code", -32000}, {"message", text.toStdString()}};
      responses.push_back(
        Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(errorObject)}});
    }

    if (message.json.is_array())
    {
      writeLine(dumpJson(responses));
    }
    else
    {
      writeLine(dumpJson(responses.front()));
    }
  }

  void failQueued(const QString& text)
  {
    logMessage(text);
    for (const auto& message : std::exchange(m_queue, {}))
    {
      replyWithError(message, text);
    }
  }

  void editorUnavailable()
  {
    if (m_launching)
    {
      return;
    }

    if (m_options.noLaunch)
    {
      failQueued(NotRunningMessage);
      return;
    }

    if (m_launchTimer.isValid() && m_launchTimer.elapsed() < 30000)
    {
      failQueued("TrenchBroom does not accept connections");
      m_launchTimer.invalidate();
      return;
    }

    launchEditor();
  }

  QString findEditor() const
  {
    if (!m_options.editorPath.isEmpty())
    {
      return m_options.editorPath;
    }

    const auto dir = QDir{QCoreApplication::applicationDirPath()};
    const auto candidates = QStringList{
#if defined(Q_OS_WIN)
      dir.filePath("TrenchBroom.exe"),
#elif defined(Q_OS_MACOS)
      dir.filePath("TrenchBroom"),
      dir.filePath("../MacOS/TrenchBroom"),
      dir.filePath("TrenchBroom.app/Contents/MacOS/TrenchBroom"),
#else
      dir.filePath("trenchbroom"),
      dir.filePath("TrenchBroom"),
      dir.filePath("../TrenchBroom/trenchbroom"),
#endif
    };

    for (const auto& candidate : candidates)
    {
      if (const auto info = QFileInfo{candidate}; info.isFile() && info.isExecutable())
      {
        return info.absoluteFilePath();
      }
    }
    return {};
  }

  void launchEditor()
  {
    const auto editor = findEditor();
    if (editor.isEmpty())
    {
      failQueued(
        "TrenchBroom is not running and its executable was not found; use --editor");
      return;
    }

    logMessage("Launching " + editor);
    // The editor must not inherit the bridge's stdout: it is reserved for the protocol.
    auto process = QProcess{};
    process.setProgram(editor);
    process.setArguments({"--mcp-server"});
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    if (!process.startDetached())
    {
      failQueued("Could not launch " + editor);
      return;
    }

    m_launching = true;
    m_staleDiscoveryTime = discoveryFileModificationTime();
    m_launchTimer.start();
    m_pollTimer.start();
  }

  void pollDiscoveryFile()
  {
    const auto modificationTime = discoveryFileModificationTime();
    if (modificationTime && modificationTime != m_staleDiscoveryTime)
    {
      if (const auto port = readDiscoveryFile())
      {
        logMessage(QString{"TrenchBroom is listening on port %1"}.arg(*port));
        m_launching = false;
        m_pollTimer.stop();
        m_port = m_options.port ? m_options.port : port;
        pump();
        return;
      }
    }

    if (m_launchTimer.elapsed() > std::chrono::milliseconds{LaunchTimeout}.count())
    {
      m_launching = false;
      m_pollTimer.stop();
      m_launchTimer.invalidate();
      failQueued("TrenchBroom did not start");
    }
  }

  /**
   * Opens the standalone SSE stream of the session (HTTP GET).
   */
  void openStream()
  {
    if (m_shuttingDown || m_streamReply || m_sessionId.empty() || !m_port)
    {
      return;
    }

    auto request = makeRequest();
    request.setRawHeader("Accept", "text/event-stream");

    auto* reply = m_network.get(request);
    auto parser = std::make_shared<SseParser>();
    m_streamReply = reply;
    const auto sessionId = m_sessionId;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, parser]() {
      if (isEventStream(reply))
      {
        m_streamAttempts = 0;
        forwardEvents(*parser, reply->readAll());
      }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, sessionId]() {
      reply->deleteLater();
      m_streamReply = nullptr;
      if (m_shuttingDown || sessionId != m_sessionId)
      {
        // The session was closed or replaced
        return;
      }

      const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (status >= 400)
      {
        // The session is gone or the server does not offer a stream
        logMessage(
          QString{"The notification stream was rejected with HTTP %1"}.arg(status));
        if (status == 404)
        {
          editorSessionLost();
        }
        return;
      }

      if (status == 0 && isConnectionRefused(reply->error()))
      {
        // TrenchBroom has quit; the next message that needs it connects again
        logMessage("TrenchBroom closed the connection");
        editorSessionLost();
        return;
      }

      const auto delay =
        ReconnectDelays[std::min(m_streamAttempts, ReconnectDelays.size() - 1)];
      ++m_streamAttempts;
      m_reconnectTimer.start(delay);
    });
  }
};

} // namespace
} // namespace tb::mcp

int main(int argc, char** argv)
{
  // Must match the editor so that QStandardPaths yields the same user data directory
  QCoreApplication::setApplicationName("TrenchBroom");
  QCoreApplication::setOrganizationName("");
  QCoreApplication::setOrganizationDomain("io.github.trenchbroom");

  auto app = QCoreApplication{argc, argv};

#ifdef Q_OS_WIN
  // Prevent CRLF translation on stdout
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  auto parser = QCommandLineParser{};
  parser.setApplicationDescription(
    "Connects an MCP client that uses the stdio transport to TrenchBroom.");
  parser.addHelpOption();

  const auto portOption = QCommandLineOption{
    "port",
    "The port of TrenchBroom's MCP server (default: from the discovery file).",
    "N"};
  const auto noLaunchOption =
    QCommandLineOption{"no-launch", "Do not launch TrenchBroom if it is not running."};
  const auto editorOption =
    QCommandLineOption{"editor", "The TrenchBroom executable to launch.", "PATH"};
  parser.addOption(portOption);
  parser.addOption(noLaunchOption);
  parser.addOption(editorOption);
  parser.process(app);

  auto options = tb::mcp::Options{};
  if (parser.isSet(portOption))
  {
    auto ok = false;
    const auto port = parser.value(portOption).toUShort(&ok);
    if (!ok || port == 0)
    {
      tb::mcp::logMessage("Invalid port: " + parser.value(portOption));
      return 1;
    }
    options.port = port;
  }
  options.noLaunch = parser.isSet(noLaunchOption);
  options.editorPath = parser.value(editorOption);

  // The bridge answers the list requests with its own MCP server, which needs preferences
  tb::mcp::BridgeSession::createNullPreferenceManager();

  auto bridge = tb::mcp::Bridge{std::move(options)};
  auto* bridgePtr = &bridge;

  // Read stdin on a separate thread because stdin cannot be watched portably with the
  // Qt event loop (QSocketNotifier does not work for stdin on Windows).
  std::thread{[bridgePtr]() {
    auto line = std::string{};
    while (std::getline(std::cin, line))
    {
      QMetaObject::invokeMethod(
        bridgePtr,
        [bridgePtr, line]() { bridgePtr->receive(line); },
        Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(
      bridgePtr, [bridgePtr]() { bridgePtr->endOfInput(); }, Qt::QueuedConnection);
  }}.detach();

  return app.exec();
}
