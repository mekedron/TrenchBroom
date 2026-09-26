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
// newline-delimited JSON-RPC messages from stdin, forwards them to the editor's MCP
// endpoint (http://127.0.0.1:<port>/mcp), and writes the responses and notifications to
// stdout, one message per line. It finds the editor through the discovery file that the
// editor writes while its MCP server listens, and launches the editor if necessary.
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

#include "mcp/Json.h"
#include "mcp/SseParser.h"

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

  std::string method() const { return json.is_object() ? json.value("method", "") : ""; }
};

class Bridge : public QObject
{
private:
  Options m_options;
  QNetworkAccessManager m_network;

  /** The port of the editor, if known. */
  std::optional<quint16> m_port;
  std::string m_sessionId;
  std::string m_protocolVersion;

  /** Messages that wait for the editor or for the initialize response. */
  std::deque<PendingMessage> m_queue;
  bool m_initializing = false;

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

  /**
   * Forwards queued messages as far as possible.
   */
  void pump()
  {
    while (!m_queue.empty() && !m_initializing && !m_shuttingDown)
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

      auto message = std::move(m_queue.front());
      m_queue.pop_front();
      forward(std::move(message));
    }
  }

  void forward(PendingMessage message)
  {
    const auto method = message.method();
    const auto isInitialize = method == "initialize";
    if (isInitialize)
    {
      // A new session starts; the headers of the old one must not be sent
      m_sessionId.clear();
      m_protocolVersion.clear();
      m_initializing = true;
    }

    auto request = makeRequest();
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");

    auto* reply = m_network.post(request, QByteArray::fromStdString(message.text));
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
      [this, reply, parser, message = std::move(message), isInitialize]() mutable {
        reply->deleteLater();
        postFinished(*reply, *parser, std::move(message), isInitialize);
      });
  }

  void postFinished(
    QNetworkReply& reply,
    SseParser& parser,
    PendingMessage message,
    const bool isInitialize)
  {
    if (isInitialize)
    {
      m_initializing = false;
    }

    if (m_shuttingDown)
    {
      return;
    }

    const auto status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0)
    {
      if (
        reply.error() == QNetworkReply::ConnectionRefusedError
        || reply.error() == QNetworkReply::HostNotFoundError)
      {
        // The editor is not running (anymore); retry once it is
        logMessage(
          QString{"Cannot connect to TrenchBroom on port %1"}.arg(m_port.value_or(0)));
        m_port = std::nullopt;
        m_queue.push_front(std::move(message));
        editorUnavailable();
      }
      else
      {
        replyWithError(
          message, "Connection to TrenchBroom failed: " + reply.errorString());
        pump();
      }
      return;
    }

    if (isEventStream(&reply))
    {
      forwardEvents(parser, reply.readAll());
    }
    else if (status >= 200 && status < 300)
    {
      const auto body = reply.readAll();
      if (isInitialize)
      {
        m_sessionId = toStdString(reply.rawHeader(SessionIdHeader));
        const auto json = parseJson(toStdString(body));
        const auto* result = json ? findMember(*json, "result") : nullptr;
        const auto* version = result ? findMember(*result, "protocolVersion") : nullptr;
        if (version && version->is_string())
        {
          m_protocolVersion = version->get<std::string>();
        }
      }
      if (!body.isEmpty())
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

      if (status == 404 && !m_sessionId.empty())
      {
        m_sessionId.clear();
        m_protocolVersion.clear();
        replyWithError(
          message,
          "The MCP session has expired (was TrenchBroom restarted?). Reconnect to start "
          "a new session.",
          error);
      }
      else
      {
        replyWithError(
          message, QString{"TrenchBroom returned HTTP %1"}.arg(status), error);
      }
    }

    if (message.method() == "notifications/initialized" && status >= 200 && status < 300)
    {
      m_streamAttempts = 0;
      openStream();
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
      failQueued("TrenchBroom is not running");
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
    if (!QProcess::startDetached(editor, {"--mcp-server"}))
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

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, parser]() {
      if (isEventStream(reply))
      {
        m_streamAttempts = 0;
        forwardEvents(*parser, reply->readAll());
      }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
      reply->deleteLater();
      m_streamReply = nullptr;
      if (m_shuttingDown)
      {
        return;
      }

      const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (status >= 400)
      {
        // The session is gone or the server does not offer a stream
        logMessage(
          QString{"The notification stream was rejected with HTTP %1"}.arg(status));
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
