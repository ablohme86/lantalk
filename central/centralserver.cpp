#include "central/centralserver.h"

#include "protocol/centralprotocol.h"
#include "protocol/conversationidentity.h"

#include <QTcpSocket>
#include <QUuid>

namespace {

// A relayed file is at most 50 MB (about 67 MB in base64); a receiver this
// far behind is not keeping up and its conversation is dropped.
constexpr qint64 MaximumPendingBytes = 128LL * 1024 * 1024;
// Clients must register this soon after connecting.
constexpr qint64 RegistrationTimeoutMs = 15 * 1000;
constexpr qsizetype MaximumNicknameLength = 256;

} // namespace

CentralServer::CentralServer(QObject *parent)
    : QObject(parent), idleTimeoutMs_(CentralProtocol::IdleTimeoutMs) {
  connect(&server_, &QTcpServer::newConnection, this,
          &CentralServer::acceptClients);
  sweepTimer_.setInterval(5 * 1000);
  connect(&sweepTimer_, &QTimer::timeout, this,
          &CentralServer::sweepIdleClients);
  sweepTimer_.start();
}

CentralServer::~CentralServer() {
  // Sockets are children of server_; keep their final signals away from a
  // half-destroyed server.
  for (auto *socket : clients_.keys()) {
    disconnect(socket, nullptr, this, nullptr);
    socket->abort();
  }
}

bool CentralServer::listen(const QHostAddress &address, quint16 port) {
  return server_.listen(address, port);
}

quint16 CentralServer::serverPort() const { return server_.serverPort(); }

QString CentralServer::errorString() const { return server_.errorString(); }

int CentralServer::clientCount() const {
  int count = 0;
  for (const auto &client : clients_) count += client.registered ? 1 : 0;
  return count;
}

int CentralServer::channelCount() const { return int(channels_.size()); }

void CentralServer::setLogger(std::function<void(const QString &)> logger) {
  logger_ = std::move(logger);
}

void CentralServer::setIdleTimeout(int milliseconds) {
  idleTimeoutMs_ = milliseconds;
  sweepTimer_.setInterval(qMax(50, milliseconds / 4));
}

void CentralServer::acceptClients() {
  while (auto *socket = server_.nextPendingConnection()) {
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    Client client;
    client.address = CentralProtocol::formatAddress(socket->peerAddress());
    client.lastActivity.start();
    clients_.insert(socket, client);
    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket] { readClient(socket); });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, socket] { dropClient(socket, QStringLiteral("disconnected")); });
  }
}

void CentralServer::readClient(QTcpSocket *socket) {
  auto client = clients_.find(socket);
  if (client == clients_.end()) return;
  client->buffer.append(socket->readAll());
  client->lastActivity.start();
  while (clients_.contains(socket)) {
    auto &buffer = clients_[socket].buffer;
    const auto newline = buffer.indexOf('\n');
    if (newline < 0) {
      if (buffer.size() > CentralProtocol::MaximumLineLength) {
        dropClient(socket, QStringLiteral("sent an oversized line"));
      }
      return;
    }
    auto line = buffer.left(newline);
    buffer.remove(0, newline + 1);
    if (line.endsWith('\r')) line.chop(1);
    processLine(socket, QString::fromUtf8(line));
  }
}

void CentralServer::processLine(QTcpSocket *socket, const QString &line) {
  const auto command = line.section(u' ', 0, 0);
  auto &client = clients_[socket];
  if (!client.registered) {
    if (command == QStringLiteral("CENTRAL_HELLO")) {
      registerClient(socket, line.split(u' ', Qt::SkipEmptyParts));
    } else {
      dropClient(socket, QStringLiteral("did not register"));
    }
    return;
  }
  if (command == QStringLiteral("CENTRAL_DATA")) {
    const auto channelId = line.section(u' ', 1, 1);
    // Relay the rest of the line byte for byte.
    const auto prefix = QByteArrayLiteral("CENTRAL_DATA ").size() +
                        channelId.toUtf8().size() + 1;
    relay(socket, channelId, line.toUtf8().mid(prefix));
    return;
  }
  const auto parts = line.split(u' ', Qt::SkipEmptyParts);
  if (command == QStringLiteral("CENTRAL_OPEN") && parts.size() == 3) {
    openChannel(socket, parts.at(1), parts.at(2));
  } else if (command == QStringLiteral("CENTRAL_CLOSE") && parts.size() >= 2) {
    const auto channel = channels_.value(parts.at(1));
    if (channel.opener == socket || channel.target == socket) {
      closeChannel(parts.at(1), socket, QStringLiteral("closed"));
    }
  } else if (command == QStringLiteral("CENTRAL_PING")) {
    send(socket, "CENTRAL_PONG");
  }
  // Anything else is ignored, so newer clients can add commands.
}

void CentralServer::registerClient(QTcpSocket *socket,
                                   const QStringList &parts) {
  bool versionIsValid = false;
  bool portIsValid = false;
  const int version = parts.value(1).toInt(&versionIsValid);
  const auto nickname =
      CentralProtocol::decodeField(parts.value(2)).simplified();
  const auto port = parts.value(3).toUShort(&portIsValid);
  if (parts.size() != 4 || !versionIsValid || version < 1 || !portIsValid ||
      nickname.isEmpty() || nickname.size() > MaximumNicknameLength) {
    send(socket, "CENTRAL_REJECT " +
                     CentralProtocol::encodeField(QStringLiteral(
                         "The central host could not read the sign-in.")));
    socket->disconnectFromHost();
    return;
  }

  auto &client = clients_[socket];
  client.nickname = nickname;
  client.port = port;
  // The same address and nickname is the same conversation partner for
  // every LANTalk, so an older registration is a stale or restarted client.
  for (auto other = clients_.begin(); other != clients_.end(); ++other) {
    if (other.key() == socket || !other->registered ||
        !ConversationIdentity::sameAddress(other->address, client.address) ||
        !ConversationIdentity::sameNickname(other->nickname, nickname)) {
      continue;
    }
    auto *stale = other.key();
    send(stale, "CENTRAL_REPLACED");
    dropClient(stale, QStringLiteral("was replaced by a new sign-in"));
    break;
  }

  auto &registered = clients_[socket];
  registered.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  registered.registered = true;
  for (auto other = clients_.cbegin(); other != clients_.cend(); ++other) {
    if (other.key() != socket && other->registered) {
      send(socket, peerLine(*other));
    }
  }
  send(socket, QStringLiteral("CENTRAL_WELCOME %1 %2")
                   .arg(registered.id, registered.address)
                   .toUtf8());
  broadcast(peerLine(registered), socket);
  log(QStringLiteral("%1 connected (%2 online)")
          .arg(describe(registered))
          .arg(clientCount()));
}

void CentralServer::openChannel(QTcpSocket *socket, const QString &channelId,
                                const QString &targetId) {
  if (!CentralProtocol::isValidChannelId(channelId) ||
      channels_.contains(channelId)) {
    send(socket, "CENTRAL_CLOSE " + channelId.toUtf8() + " unavailable");
    return;
  }
  auto *target = findClient(targetId);
  if (!target || target == socket) {
    send(socket, "CENTRAL_CLOSE " + channelId.toUtf8() + " unavailable");
    return;
  }
  channels_.insert(channelId, {socket, target});
  clients_[socket].channels.insert(channelId);
  clients_[target].channels.insert(channelId);
  send(target, QStringLiteral("CENTRAL_INCOMING %1 %2")
                   .arg(channelId, clients_.value(socket).id)
                   .toUtf8());
  send(socket, "CENTRAL_OPENED " + channelId.toUtf8());
}

void CentralServer::relay(QTcpSocket *socket, const QString &channelId,
                          const QByteArray &line) {
  const auto channel = channels_.constFind(channelId);
  if (channel == channels_.cend()) return;
  QTcpSocket *destination = nullptr;
  if (channel->opener == socket) destination = channel->target;
  else if (channel->target == socket) destination = channel->opener;
  if (!destination) return;
  if (destination->bytesToWrite() > MaximumPendingBytes) {
    closeChannel(channelId, nullptr, QStringLiteral("overloaded"));
    return;
  }
  send(destination, "CENTRAL_DATA " + channelId.toUtf8() + ' ' + line);
}

void CentralServer::closeChannel(const QString &channelId,
                                 QTcpSocket *closedBy, const QString &reason) {
  const auto channel = channels_.take(channelId);
  for (auto *end : {channel.opener, channel.target}) {
    if (!end || !clients_.contains(end)) continue;
    clients_[end].channels.remove(channelId);
    if (end != closedBy) {
      send(end, "CENTRAL_CLOSE " + channelId.toUtf8() + ' ' + reason.toUtf8());
    }
  }
}

void CentralServer::dropClient(QTcpSocket *socket, const QString &why) {
  const auto found = clients_.find(socket);
  if (found == clients_.end()) return;
  const auto client = *found;
  clients_.erase(found);
  for (const auto &channelId : client.channels) {
    closeChannel(channelId, socket, QStringLiteral("closed"));
  }
  disconnect(socket, nullptr, this, nullptr);
  // Let queued lines (e.g. CENTRAL_REPLACED) reach the client before the
  // socket goes away, but never wait for a stuck one for long.
  connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
  QTimer::singleShot(5000, socket, &QObject::deleteLater);
  if (socket->state() == QAbstractSocket::ConnectedState) {
    socket->disconnectFromHost();
  }
  if (socket->state() == QAbstractSocket::UnconnectedState) {
    socket->deleteLater();
  }
  if (!client.registered) return;
  broadcast("CENTRAL_PEER_GONE " + client.id.toUtf8(), nullptr);
  log(QStringLiteral("%1 %2 (%3 online)")
          .arg(describe(client), why)
          .arg(clientCount()));
}

void CentralServer::sweepIdleClients() {
  QList<QTcpSocket *> idle;
  for (auto client = clients_.cbegin(); client != clients_.cend(); ++client) {
    const auto limit = client->registered
                           ? qint64(idleTimeoutMs_)
                           : qMin<qint64>(idleTimeoutMs_, RegistrationTimeoutMs);
    if (client->lastActivity.elapsed() > limit) idle.append(client.key());
  }
  for (auto *socket : idle) {
    dropClient(socket, QStringLiteral("stopped responding"));
  }
}

void CentralServer::send(QTcpSocket *socket, const QByteArray &line) {
  if (socket->state() != QAbstractSocket::ConnectedState) return;
  socket->write(line + '\n');
}

void CentralServer::broadcast(const QByteArray &line, QTcpSocket *except) {
  for (auto client = clients_.cbegin(); client != clients_.cend(); ++client) {
    if (client.key() != except && client->registered) {
      send(client.key(), line);
    }
  }
}

QByteArray CentralServer::peerLine(const Client &client) const {
  return QStringLiteral("CENTRAL_PEER %1 %2 %3 %4")
      .arg(client.id,
           QString::fromLatin1(CentralProtocol::encodeField(client.nickname)),
           client.address)
      .arg(client.port)
      .toUtf8();
}

QTcpSocket *CentralServer::findClient(const QString &id) const {
  for (auto client = clients_.cbegin(); client != clients_.cend(); ++client) {
    if (client->registered && client->id == id) return client.key();
  }
  return nullptr;
}

QString CentralServer::describe(const Client &client) const {
  return QStringLiteral("%1 (%2)").arg(client.nickname, client.address);
}

void CentralServer::log(const QString &message) const {
  if (logger_) logger_(message);
}
