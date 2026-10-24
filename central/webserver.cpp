#include "central/webserver.h"

#include "central/centralserver.h"
#include "protocol/centralprotocol.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

#if QT_CONFIG(ssl)
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#endif

namespace {

// Requests are a few hundred bytes; anything this large is not a browser.
constexpr qsizetype MaximumRequestSize = 16 * 1024;
constexpr int RequestTimeoutMs = 10 * 1000;
const QByteArray WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct Asset {
  QString resource;
  QByteArray contentType;
};

const QHash<QByteArray, Asset> &assets() {
  static const QHash<QByteArray, Asset> table{
      {"/", {QStringLiteral(":/web/index.html"), "text/html; charset=utf-8"}},
      {"/app.js",
       {QStringLiteral(":/web/app.js"), "text/javascript; charset=utf-8"}},
      {"/app.css", {QStringLiteral(":/web/app.css"), "text/css; charset=utf-8"}},
      {"/manifest.webmanifest",
       {QStringLiteral(":/web/manifest.webmanifest"),
        "application/manifest+json"}},
      {"/icon.svg", {QStringLiteral(":/web/icon.svg"), "image/svg+xml"}},
  };
  return table;
}

QByteArray header(const QList<QByteArray> &lines, const QByteArray &name) {
  for (const auto &line : lines) {
    const auto colon = line.indexOf(':');
    if (colon > 0 && line.left(colon).trimmed().toLower() == name) {
      return line.mid(colon + 1).trimmed();
    }
  }
  return {};
}

} // namespace

WebServer::WebServer(CentralServer *central, QObject *parent)
    : QObject(parent), central_(central),
      server_(std::make_unique<QTcpServer>()) {}

WebServer::~WebServer() = default;

bool WebServer::setCertificate(const QString &certificatePath,
                               const QString &keyPath) {
#if QT_CONFIG(ssl)
  QFile certificateFile(certificatePath);
  QFile keyFile(keyPath);
  if (!certificateFile.open(QIODevice::ReadOnly)) {
    error_ = QStringLiteral("Cannot read %1: %2")
                 .arg(certificatePath, certificateFile.errorString());
    return false;
  }
  if (!keyFile.open(QIODevice::ReadOnly)) {
    error_ = QStringLiteral("Cannot read %1: %2")
                 .arg(keyPath, keyFile.errorString());
    return false;
  }
  const auto chain = QSslCertificate::fromData(certificateFile.readAll());
  const auto keyData = keyFile.readAll();
  QSslKey key(keyData, QSsl::Ec);
  if (key.isNull()) key = QSslKey(keyData, QSsl::Rsa);
  if (chain.isEmpty() || key.isNull()) {
    error_ = chain.isEmpty()
                 ? QStringLiteral("%1 holds no PEM certificate.").arg(certificatePath)
                 : QStringLiteral("%1 holds no unencrypted PEM RSA or EC key.")
                       .arg(keyPath);
    return false;
  }
  auto configuration = QSslConfiguration::defaultConfiguration();
  configuration.setLocalCertificateChain(chain);
  configuration.setPrivateKey(key);
  configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
  auto server = std::make_unique<QSslServer>();
  server->setSslConfiguration(configuration);
  server_ = std::move(server);
  secure_ = true;
  return true;
#else
  Q_UNUSED(certificatePath)
  Q_UNUSED(keyPath)
  error_ = QStringLiteral("This lantalkd was built without TLS support.");
  return false;
#endif
}

bool WebServer::listen(const QHostAddress &address, quint16 port) {
  // Over TLS a connection is pending only once its handshake is done.
  connect(server_.get(), &QTcpServer::pendingConnectionAvailable, this, [this] {
    while (auto *socket = server_->nextPendingConnection()) accept(socket);
  });
  if (!server_->listen(address, port)) {
    error_ = server_->errorString();
    return false;
  }
  return true;
}

quint16 WebServer::serverPort() const { return server_->serverPort(); }

QString WebServer::errorString() const { return error_; }

bool WebServer::isSecure() const { return secure_; }

void WebServer::accept(QTcpSocket *socket) {
  socket->setParent(this);
  connect(socket, &QTcpSocket::readyRead, this,
          [this, socket] { readRequest(socket); });
  connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
  QTimer::singleShot(RequestTimeoutMs, socket, [socket] {
    // Not yet handed to a WebSocketLink: the request never completed.
    if (!qobject_cast<CentralLink *>(socket->parent())) {
      socket->abort();
      socket->deleteLater();
    }
  });
  if (socket->bytesAvailable() > 0) readRequest(socket);
}

void WebServer::readRequest(QTcpSocket *socket) {
  auto buffer = socket->property("request").toByteArray() + socket->readAll();
  const auto end = buffer.indexOf("\r\n\r\n");
  if (end < 0) {
    if (buffer.size() > MaximumRequestSize) socket->abort();
    else socket->setProperty("request", buffer);
    return;
  }
  disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
  const auto lines = buffer.left(end).split('\n');
  const auto pending = buffer.mid(end + 4);
  const auto request = lines.value(0).trimmed().split(' ');
  if (request.size() != 3 || !request.at(2).startsWith("HTTP/1.")) {
    respond(socket, "400 Bad Request", "text/plain", "Bad request\n");
    return;
  }
  const auto &method = request.at(0);
  auto path = request.at(1);
  if (const auto query = path.indexOf('?'); query >= 0) path.truncate(query);

  if (path == "/ws") {
    const auto key = header(lines, "sec-websocket-key");
    if (method != "GET" || key.isEmpty() ||
        header(lines, "upgrade").toLower() != "websocket" ||
        !header(lines, "connection").toLower().contains("upgrade") ||
        header(lines, "sec-websocket-version") != "13") {
      respond(socket, "426 Upgrade Required", "text/plain",
              "This address takes WebSocket connections only.\n");
      return;
    }
    const auto accept =
        QCryptographicHash::hash(key + WebSocketGuid, QCryptographicHash::Sha1)
            .toBase64();
    socket->write("HTTP/1.1 101 Switching Protocols\r\n"
                  "Upgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: " +
                  accept + "\r\n\r\n");
    disconnect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    if (!central_) {
      socket->abort();
      socket->deleteLater();
      return;
    }
    central_->addLink(new WebSocketLink(socket, pending));
    return;
  }

  const bool headOnly = method == "HEAD";
  if (method != "GET" && !headOnly) {
    respond(socket, "405 Method Not Allowed", "text/plain",
            "Method not allowed\n");
    return;
  }
  if (path == "/index.html") path = "/";
  const auto asset = assets().constFind(path);
  if (asset == assets().cend()) {
    respond(socket, "404 Not Found", "text/plain", "Not found\n", headOnly);
    return;
  }
  QFile file(asset->resource);
  if (!file.open(QIODevice::ReadOnly)) {
    respond(socket, "500 Internal Server Error", "text/plain",
            "The web client is missing from this lantalkd.\n", headOnly);
    return;
  }
  respond(socket, "200 OK", asset->contentType, file.readAll(), headOnly);
}

void WebServer::respond(QTcpSocket *socket, const QByteArray &status,
                        const QByteArray &contentType, const QByteArray &body,
                        bool headOnly) {
  QByteArray response = "HTTP/1.1 " + status + "\r\n";
  response += "Content-Type: " + contentType + "\r\n";
  response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
  // The page is versioned with lantalkd; let browsers check for a new one.
  response += "Cache-Control: no-cache\r\n";
  response += "X-Content-Type-Options: nosniff\r\n";
  response += "Referrer-Policy: no-referrer\r\n";
  // http: and https: let the page reach an Ollama server the user enters.
  response += "Content-Security-Policy: default-src 'self'; "
              "connect-src 'self' ws: wss: http: https:; img-src 'self' blob: data:; "
              "media-src 'self' blob:; frame-ancestors 'none'\r\n";
  response += "Connection: close\r\n\r\n";
  if (!headOnly) response += body;
  socket->write(response);
  socket->disconnectFromHost();
}

WebSocketLink::WebSocketLink(QTcpSocket *socket, const QByteArray &pending,
                             QObject *parent)
    : CentralLink(parent), socket_(socket), input_(pending) {
  socket_->setParent(this);
  socket_->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
  socket_->setReadBufferSize(256 * 1024);
  connect(socket_, &QTcpSocket::readyRead, this, [this] {
    if (!reading_) return;
    input_.append(socket_->readAll());
    readFrames();
  });
  connect(socket_, &QTcpSocket::disconnected, this, &CentralLink::disconnected);
  connect(socket_, &QTcpSocket::bytesWritten, this, &CentralLink::bytesWritten);
  if (!input_.isEmpty()) {
    QTimer::singleShot(0, this, &WebSocketLink::readFrames);
  }
}

void WebSocketLink::readFrames() {
  while (!closing_) {
    if (input_.size() < 2) return;
    const auto first = quint8(input_.at(0));
    const auto second = quint8(input_.at(1));
    const bool final = first & 0x80;
    const quint8 opcode = first & 0x0f;
    const bool masked = second & 0x80;
    quint64 length = second & 0x7f;
    qsizetype offset = 2;
    if (length == 126) {
      if (input_.size() < 4) return;
      length = (quint64(quint8(input_.at(2))) << 8) | quint8(input_.at(3));
      offset = 4;
    } else if (length == 127) {
      if (input_.size() < 10) return;
      length = 0;
      for (int index = 2; index < 10; ++index) {
        length = (length << 8) | quint8(input_.at(index));
      }
      offset = 10;
    }
    // Browsers always mask; control frames are short and never fragmented.
    if (!masked || (first & 0x70) ||
        length + quint64(message_.size()) >
            quint64(CentralProtocol::MaximumLineLength) ||
        (opcode >= 8 && (length > 125 || !final))) {
      fail();
      return;
    }
    if (quint64(input_.size()) < quint64(offset) + 4 + length) return;
    const auto mask = input_.mid(offset, 4);
    auto payload = input_.mid(offset + 4, qsizetype(length));
    input_.remove(0, offset + 4 + qsizetype(length));
    for (qsizetype index = 0; index < payload.size(); ++index) {
      payload[index] = char(payload.at(index) ^ mask.at(index % 4));
    }

    switch (opcode) {
    case 0x0: // continuation
    case 0x1: // text
    case 0x2: // binary
      message_.append(payload);
      if (!final) break;
      lines_.append(message_);
      lines_.append('\n');
      message_.clear();
      emit readyRead();
      break;
    case 0x8: // close
      sendFrame(0x8, payload.left(2));
      closing_ = true;
      socket_->disconnectFromHost();
      return;
    case 0x9: // ping
      sendFrame(0xA, payload);
      break;
    case 0xA: // pong
      break;
    default:
      fail();
      return;
    }
  }
}

void WebSocketLink::sendFrame(quint8 opcode, const QByteArray &payload) {
  if (socket_->state() != QAbstractSocket::ConnectedState) return;
  QByteArray frame;
  frame.append(char(0x80 | opcode));
  const auto size = quint64(payload.size());
  if (size < 126) {
    frame.append(char(size));
  } else if (size <= 0xffff) {
    frame.append(char(126));
    frame.append(char(size >> 8));
    frame.append(char(size & 0xff));
  } else {
    frame.append(char(127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.append(char((size >> shift) & 0xff));
    }
  }
  socket_->write(frame + payload);
}

void WebSocketLink::fail() {
  // 1002: protocol error.
  sendFrame(0x8, QByteArray("\x03\xea", 2));
  closing_ = true;
  socket_->disconnectFromHost();
}

QByteArray WebSocketLink::readAll() { return std::exchange(lines_, {}); }

void WebSocketLink::writeLine(const QByteArray &line) {
  if (!closing_) sendFrame(0x1, line);
}

qint64 WebSocketLink::bytesToWrite() const { return socket_->bytesToWrite(); }

bool WebSocketLink::isConnected() const {
  return !closing_ && socket_->state() == QAbstractSocket::ConnectedState;
}

bool WebSocketLink::isUnconnected() const {
  return socket_->state() == QAbstractSocket::UnconnectedState;
}

QString WebSocketLink::peerAddress() const {
  return CentralProtocol::formatAddress(socket_->peerAddress());
}

void WebSocketLink::close() {
  if (closing_) return;
  // 1000: normal closure.
  sendFrame(0x8, QByteArray("\x03\xe8", 2));
  closing_ = true;
  socket_->disconnectFromHost();
}

void WebSocketLink::abort() { socket_->abort(); }

void WebSocketLink::setReading(bool reading) {
  if (reading_ == reading) return;
  reading_ = reading;
  if (reading && socket_->bytesAvailable() > 0) {
    QTimer::singleShot(0, this, [this] {
      if (!reading_) return;
      input_.append(socket_->readAll());
      readFrames();
    });
  }
}
