#include "central/centralserver.h"

#include "central/centrallink.h"
#include "central/translationservice.h"

#include "appconfig.h"
#include "protocol/centralprotocol.h"
#include "protocol/conversationidentity.h"

#include <QRandomGenerator>
#include <QUuid>

namespace {

// A relayed file is at most 50 MB (about 67 MB in base64); a receiver this
// far behind is not keeping up and its conversation is dropped.
constexpr qint64 MaximumPendingBytes = 128LL * 1024 * 1024;
// Clients must register this soon after connecting.
constexpr qint64 RegistrationTimeoutMs = 15 * 1000;
constexpr qsizetype MaximumNicknameLength = 256;
// Slows down password guessing: a failed sign-in is answered this late.
constexpr int RejectDelayMs = 1000;

QByteArray createNonce() {
  QByteArray nonce(16, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32 *>(nonce.data()), nonce.size() / 4);
  return nonce.toHex();
}

} // namespace

CentralServer::CentralServer(QObject *parent)
    : QObject(parent), idleTimeoutMs_(CentralProtocol::IdleTimeoutMs),
      maximumFileSize_(MaximumFileSize) {
  connect(&server_, &QTcpServer::newConnection, this,
          &CentralServer::acceptClients);
  sweepTimer_.setInterval(5 * 1000);
  connect(&sweepTimer_, &QTimer::timeout, this,
          &CentralServer::sweepIdleClients);
  sweepTimer_.start();
}

CentralServer::~CentralServer() {
  // Links are children of this server; keep their final signals away from a
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

void CentralServer::setPassword(const QString &password) {
  password_ = password;
}

void CentralServer::setLogger(std::function<void(const QString &)> logger) {
  logger_ = std::move(logger);
}

void CentralServer::setIdleTimeout(int milliseconds) {
  idleTimeoutMs_ = milliseconds;
  sweepTimer_.setInterval(qMax(50, milliseconds / 4));
}

void CentralServer::setMaximumFileSize(qint64 bytes) {
  maximumFileSize_ = bytes;
}

void CentralServer::setTranslationService(TranslationService *service) {
  translation_ = service;
  connect(service, &TranslationService::infoChanged, this,
          [this] { broadcast(translation_->infoLine(), nullptr); });
}

void CentralServer::acceptClients() {
  while (auto *socket = server_.nextPendingConnection()) {
    addLink(new TcpLink(socket));
  }
}

void CentralServer::addLink(CentralLink *link) {
  link->setParent(this);
  Client client;
  client.address = link->peerAddress();
  client.web = link->isWeb();
  client.lastActivity.start();
  client.nonce = createNonce();
  clients_.insert(link, client);
  connect(link, &CentralLink::readyRead, this,
          [this, link] { readClient(link); });
  connect(link, &CentralLink::disconnected, this,
          [this, link] { dropClient(link, QStringLiteral("disconnected")); });
  send(link, "CENTRAL_CHALLENGE " + client.nonce);
  // Lines may have arrived with the connection, e.g. in a WebSocket's
  // first frames.
  readClient(link);
}

void CentralServer::readClient(CentralLink *socket) {
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

void CentralServer::processLine(CentralLink *socket, const QString &line) {
  const auto command = line.section(u' ', 0, 0);
  auto &client = clients_[socket];
  if (client.rejected) return;
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
  } else if (command == QStringLiteral("CENTRAL_TRANSLATE") &&
             parts.size() == 3 && translation_) {
    translation_->translate(socket, parts.at(1), parts.at(2),
                            [this, socket](const QByteArray &reply) {
                              send(socket, reply);
                            });
  } else if (command == QStringLiteral("CENTRAL_OLLAMA_SCAN") && translation_) {
    translation_->scan();
  }
  // Anything else is ignored, so newer clients can add commands.
}

void CentralServer::registerClient(CentralLink *socket,
                                   const QStringList &parts) {
  bool versionIsValid = false;
  bool portIsValid = false;
  const int version = parts.value(1).toInt(&versionIsValid);
  const auto nickname =
      CentralProtocol::decodeField(parts.value(2)).simplified();
  const auto port = parts.value(3).toUShort(&portIsValid);
  if (parts.size() != 5 || !versionIsValid || version < 1 || !portIsValid ||
      nickname.isEmpty() || nickname.size() > MaximumNicknameLength) {
    send(socket, "CENTRAL_REJECT " +
                     CentralProtocol::encodeField(QStringLiteral(
                         "The central host could not read the sign-in.")));
    socket->close();
    return;
  }

  auto &client = clients_[socket];
  if (!password_.isEmpty() &&
      !CentralProtocol::sameProof(
          parts.at(4).toLatin1(),
          CentralProtocol::authProof(password_, client.nonce))) {
    const bool withoutPassword = parts.at(4) == QStringLiteral("-");
    const auto reason =
        withoutPassword
            ? QStringLiteral("The central host requires a password.")
            : QStringLiteral("Wrong password for the central host.");
    client.rejected = true;
    log(QStringLiteral("Rejected sign-in by %1 (%2): %3")
            .arg(nickname, client.address,
                 withoutPassword ? QStringLiteral("no password")
                                 : QStringLiteral("wrong password")));
    QTimer::singleShot(RejectDelayMs, socket, [this, socket, reason] {
      send(socket, "CENTRAL_REJECT " + CentralProtocol::encodeField(reason));
      dropClient(socket, QStringLiteral("was rejected"));
    });
    return;
  }
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
      if (other->web) send(socket, kindLine(*other));
    }
  }
  send(socket, QStringLiteral("CENTRAL_WELCOME %1 %2")
                   .arg(registered.id, registered.address)
                   .toUtf8());
  broadcast(peerLine(registered), socket);
  // Older clients ignore the unknown line and list web users as usual.
  if (registered.web) broadcast(kindLine(registered), socket);
  send(socket, "CENTRAL_FILE_LIMIT " + QByteArray::number(maximumFileSize_));
  if (translation_) send(socket, translation_->infoLine());
  log(QStringLiteral("%1 connected (%2 online)")
          .arg(describe(registered))
          .arg(clientCount()));
}

void CentralServer::openChannel(CentralLink *socket, const QString &channelId,
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

void CentralServer::relay(CentralLink *socket, const QString &channelId,
                          const QByteArray &line) {
  const auto channel = channels_.find(channelId);
  if (channel == channels_.end()) return;
  CentralLink *destination = nullptr;
  if (channel->opener == socket) destination = channel->target;
  else if (channel->target == socket) destination = channel->opener;
  if (!destination) return;
  // The one place lantalkd reads the conversation: file transfers over the
  // size limit stop here. FILE_BEGIN <id> <name> <type> <size>, then
  // FILE_CHUNK <id> <data> and FILE_END <id>.
  if (line.startsWith("FILE_")) {
    const auto parts = line.split(' ');
    const auto id = QString::fromUtf8(parts.value(1));
    if (parts.first() == "FILE_BEGIN") {
      bool sizeIsValid = false;
      const auto size = parts.value(4).toLongLong(&sizeIsValid);
      if (sizeIsValid && size > maximumFileSize_) {
        channel->refusedFiles.insert(id);
        send(socket, "CENTRAL_FILE_REJECTED " + channelId.toUtf8() + ' ' +
                         id.toUtf8() + ' ' + QByteArray::number(maximumFileSize_));
        log(QStringLiteral("Refused a file of %1 bytes from %2 (limit %3)")
                .arg(size)
                .arg(describe(clients_.value(socket)))
                .arg(maximumFileSize_));
        return;
      }
    } else if (channel->refusedFiles.contains(id)) {
      if (parts.first() == "FILE_END") channel->refusedFiles.remove(id);
      return;
    }
  }
  if (destination->bytesToWrite() > MaximumPendingBytes) {
    closeChannel(channelId, nullptr, QStringLiteral("overloaded"));
    return;
  }
  send(destination, "CENTRAL_DATA " + channelId.toUtf8() + ' ' + line);
}

void CentralServer::closeChannel(const QString &channelId,
                                 CentralLink *closedBy, const QString &reason) {
  const auto channel = channels_.take(channelId);
  for (auto *end : {channel.opener, channel.target}) {
    if (!end || !clients_.contains(end)) continue;
    clients_[end].channels.remove(channelId);
    if (end != closedBy) {
      send(end, "CENTRAL_CLOSE " + channelId.toUtf8() + ' ' + reason.toUtf8());
    }
  }
}

void CentralServer::dropClient(CentralLink *socket, const QString &why) {
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
  connect(socket, &CentralLink::disconnected, socket, &QObject::deleteLater);
  QTimer::singleShot(5000, socket, &QObject::deleteLater);
  socket->close();
  if (socket->isUnconnected()) socket->deleteLater();
  if (!client.registered) return;
  broadcast("CENTRAL_PEER_GONE " + client.id.toUtf8(), nullptr);
  log(QStringLiteral("%1 %2 (%3 online)")
          .arg(describe(client), why)
          .arg(clientCount()));
}

void CentralServer::sweepIdleClients() {
  QList<CentralLink *> idle;
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

void CentralServer::send(CentralLink *socket, const QByteArray &line) {
  socket->writeLine(line);
}

void CentralServer::broadcast(const QByteArray &line, CentralLink *except) {
  for (auto client = clients_.cbegin(); client != clients_.cend(); ++client) {
    if (client.key() != except && client->registered) {
      send(client.key(), line);
    }
  }
}

QByteArray CentralServer::kindLine(const Client &client) const {
  return "CENTRAL_PEER_KIND " + client.id.toUtf8() + " web";
}

QByteArray CentralServer::peerLine(const Client &client) const {
  return QStringLiteral("CENTRAL_PEER %1 %2 %3 %4")
      .arg(client.id,
           QString::fromLatin1(CentralProtocol::encodeField(client.nickname)),
           client.address)
      .arg(client.port)
      .toUtf8();
}

CentralLink *CentralServer::findClient(const QString &id) const {
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
