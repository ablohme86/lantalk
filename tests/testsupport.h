#pragma once

#include "protocol/chatsession.h"
#include "protocol/connectionregistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace TestSupport {

// Points QSettings() at a fresh temporary INI location so tests never touch
// the user's real LANTalk settings.
inline void isolateSettings() {
  static QTemporaryDir directory;
  QCoreApplication::setOrganizationName(QStringLiteral("LANTalkTests"));
  QCoreApplication::setApplicationName(QStringLiteral("LANTalkTests"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     directory.path());
  QSettings().clear();
  // Chat histories are files in the app data folder; keep them apart too.
  QStandardPaths::setTestModeEnabled(true);
  QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
      .removeRecursively();
}

// One LANTalk instance: a listener plus the sessions it owns, with its own
// connection registry so several instances can live in one test process.
class Peer {
public:
  explicit Peer(const QString &nickname) : nickname_(nickname) {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, owner_.get(),
                     [this] {
                       while (server_.hasPendingConnections()) {
                         auto *session = new ChatSession(
                             server_.nextPendingConnection(), owner_.get(),
                             &registry_);
                         session->beginHandshake(nickname_);
                         incoming_.append(session);
                       }
                     });
  }

  ChatSession *connectTo(const Peer &other) {
    auto *session = new ChatSession(new QTcpSocket, owner_.get(), &registry_);
    session->beginHandshake(nickname_);
    session->connectToHost(QStringLiteral("127.0.0.1"), other.port());
    return session;
  }

  quint16 port() const { return server_.serverPort(); }
  ConnectionRegistry &registry() { return registry_; }
  QList<ChatSession *> incoming() const { return incoming_; }

  QList<ChatSession *> liveSessions() const {
    QList<ChatSession *> live;
    for (auto *session : registry_.sessions()) {
      if (session->isConnected() && session->isHandshakeComplete() &&
          !session->isRetired()) {
        live.append(session);
      }
    }
    return live;
  }

private:
  QString nickname_;
  ConnectionRegistry registry_;
  QTcpServer server_;
  QList<ChatSession *> incoming_;
  // Destroyed first, so sessions leave the registry before it goes away.
  std::unique_ptr<QObject> owner_ = std::make_unique<QObject>();
};

// A raw protocol peer used to observe exact wire traffic and to imitate
// clients that predate the typing extension.
class RawPeer {
public:
  explicit RawPeer(quint16 port) {
    QObject::connect(&socket_, &QTcpSocket::readyRead, &socket_, [this] {
      buffer_.append(socket_.readAll());
      int newline = -1;
      while ((newline = buffer_.indexOf('\n')) >= 0) {
        lines_.append(QString::fromUtf8(buffer_.left(newline)).trimmed());
        buffer_.remove(0, newline + 1);
      }
    });
    socket_.connectToHost(QHostAddress::LocalHost, port);
    socket_.waitForConnected(3000);
  }

  void send(const QString &line) {
    socket_.write(line.toUtf8() + '\n');
    socket_.flush();
  }
  void close() { socket_.disconnectFromHost(); }
  QStringList lines() const { return lines_; }
  int count(const QString &line) const { return lines_.count(line); }

private:
  QTcpSocket socket_;
  QByteArray buffer_;
  QStringList lines_;
};

// Minimal Ollama stand-in: GET /api/tags lists the given models, and POST
// /api/generate "translates" the prompt's message by wrapping it in <<...>>.
class FakeOllama {
public:
  explicit FakeOllama(QStringList models) : models_(std::move(models)) {
    QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
      while (auto *socket = server_.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [this, socket] { respond(socket); });
      }
    });
  }

  bool listen(quint16 port = 0) {
    return server_.listen(QHostAddress::LocalHost, port);
  }
  void close() { server_.close(); }
  quint16 port() const { return server_.serverPort(); }
  QString url() const {
    return QStringLiteral("http://127.0.0.1:%1").arg(port());
  }
  // The messages sent to /api/generate, in order.
  QStringList translatedTexts() const { return translatedTexts_; }

private:
  void respond(QTcpSocket *socket) {
    auto &request = pending_[socket];
    request.append(socket->readAll());
    const int headerEnd = request.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    const auto headers = request.left(headerEnd).toLower();
    const int lengthAt = headers.indexOf("content-length:");
    const int length =
        lengthAt < 0 ? 0
                     : headers.mid(lengthAt + 15, headers.indexOf('\r', lengthAt) - lengthAt - 15)
                           .trimmed()
                           .toInt();
    if (request.size() < headerEnd + 4 + length) return;
    const auto body = request.mid(headerEnd + 4, length);

    QByteArray reply;
    if (request.startsWith("POST /api/generate")) {
      const auto prompt =
          QJsonDocument::fromJson(body).object().value(QStringLiteral("prompt")).toString();
      const auto marker = QStringLiteral("Translate this message:\n\n");
      const auto text = prompt.mid(prompt.lastIndexOf(marker) + marker.size());
      translatedTexts_.append(text);
      reply = QJsonDocument(QJsonObject{{QStringLiteral("response"),
                                         QStringLiteral("<<%1>>").arg(text)}})
                  .toJson(QJsonDocument::Compact);
    } else {
      QStringList entries;
      for (const auto &model : std::as_const(models_)) {
        entries.append(QStringLiteral("{\"name\":\"%1\",\"model\":\"%1\"}")
                           .arg(model));
      }
      reply = QStringLiteral("{\"models\":[%1]}").arg(entries.join(u',')).toUtf8();
    }
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                  "Connection: close\r\nContent-Length: " +
                  QByteArray::number(reply.size()) + "\r\n\r\n" + reply);
    socket->disconnectFromHost();
    pending_.remove(socket);
  }

  QStringList models_;
  QStringList translatedTexts_;
  QTcpServer server_;
  QHash<QTcpSocket *, QByteArray> pending_;
};

inline void configureOllama(const QString &url, const QString &model) {
  QSettings settings;
  settings.setValue(QStringLiteral("ollama/url"), url);
  settings.setValue(QStringLiteral("ollama/model"), model);
}

} // namespace TestSupport
