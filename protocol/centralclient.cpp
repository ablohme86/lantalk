#include "protocol/centralclient.h"

#include "protocol/centralprotocol.h"
#include "protocol/conversationidentity.h"

#include <QHostAddress>
#include <QLocale>
#include <QUuid>

#include <utility>

namespace {

constexpr int DefaultReconnectIntervalMs = 10 * 1000;
// How long a new conversation waits for the central host to be reachable.
constexpr int ReadyTimeoutMs = 10 * 1000;

QString closeReasonText(const QString &reason, const QString &peer) {
  if (reason == QStringLiteral("unavailable")) {
    return QStringLiteral("%1 is not connected to the central host.").arg(peer);
  }
  if (reason == QStringLiteral("overloaded")) {
    return QStringLiteral(
        "The central host dropped the conversation because it fell behind.");
  }
  return QStringLiteral("%1 closed the conversation.").arg(peer);
}

} // namespace

CentralClient::CentralClient(QObject *parent) : QObject(parent) {
  reconnectTimer_.setSingleShot(true);
  reconnectTimer_.setInterval(DefaultReconnectIntervalMs);
  connect(&reconnectTimer_, &QTimer::timeout, this,
          &CentralClient::connectToService);
  pingTimer_.setInterval(CentralProtocol::PingIntervalMs);
  connect(&pingTimer_, &QTimer::timeout, this,
          [this] { sendLine(QStringLiteral("CENTRAL_PING")); });
  idleTimer_.setSingleShot(true);
  idleTimer_.setInterval(CentralProtocol::IdleTimeoutMs);
  connect(&idleTimer_, &QTimer::timeout, this, [this] {
    socket_.abort();
    handleDisconnected(QStringLiteral("The central host stopped responding."));
  });
  connect(&socket_, &QTcpSocket::connected, this,
          &CentralClient::handleConnected);
  connect(&socket_, &QTcpSocket::readyRead, this, &CentralClient::readLines);
  connect(&socket_, &QTcpSocket::bytesWritten, this, [this](qint64 bytes) {
    for (const auto &channel : std::as_const(channels_)) {
      if (channel) emit channel->bytesWritten(bytes);
    }
  });
  connect(&socket_, &QTcpSocket::disconnected, this, [this] {
    handleDisconnected(
        QStringLiteral("Lost the connection to the central host."));
  });
  connect(&socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
            if (socket_.state() == QAbstractSocket::UnconnectedState) {
              handleDisconnected(socket_.errorString());
            }
          });
}

CentralClient::~CentralClient() {
  disconnect(&socket_, nullptr, this, nullptr);
  // The owners of the channels may be half destroyed themselves by now, so
  // the channels are cut off without signals.
  for (const auto &channel : std::as_const(channels_)) {
    if (!channel) continue;
    channel->client_ = nullptr;
    channel->channelId_.clear();
    channel->state_ = ChatTransport::State::Unconnected;
  }
}

void CentralClient::start(const QString &hostSpec, const QString &nickname,
                          quint16 listenPort, const QString &password) {
  const auto spec = hostSpec.trimmed();
  if (spec.isEmpty()) {
    stop();
    return;
  }
  if (spec == hostSpec_ && nickname == nickname_ && listenPort == listenPort_ &&
      password == password_ && !replaced_) {
    return;
  }
  stop();
  hostSpec_ = spec;
  nickname_ = nickname;
  listenPort_ = listenPort;
  password_ = password;
  if (!CentralProtocol::parseHostSpec(spec, &host_, &port_)) {
    lastError_ = QStringLiteral("\"%1\" is not a valid central host.").arg(spec);
    emit stateChanged();
    return;
  }
  connectToService();
  emit stateChanged();
}

void CentralClient::stop() {
  const bool wasActive = isActive();
  hostSpec_.clear();
  host_.clear();
  port_ = 0;
  replaced_ = false;
  lastError_.clear();
  reconnectTimer_.stop();
  pingTimer_.stop();
  idleTimer_.stop();
  socket_.abort();
  buffer_.clear();
  ready_ = false;
  clientId_.clear();
  failAllChannels(QStringLiteral("LANTalk left the central host."));
  if (!peers_.isEmpty()) {
    peers_.clear();
    emit peersChanged();
  }
  if (wasActive) emit stateChanged();
}

bool CentralClient::isActive() const { return !hostSpec_.isEmpty(); }

bool CentralClient::isReady() const { return ready_; }

QString CentralClient::hostSpec() const { return hostSpec_; }

QString CentralClient::lastError() const { return lastError_; }

qint64 CentralClient::maximumFileSize() const { return maximumFileSize_; }

QList<CentralClient::Peer> CentralClient::peers() const {
  return ready_ ? peers_ : QList<Peer>();
}

std::optional<CentralClient::Peer>
CentralClient::findPeer(const QString &address, quint16 port,
                        const QString &nickname) const {
  if (!ready_) return std::nullopt;
  QList<Peer> candidates;
  // Newest registrations first: a client that just reconnected wins over a
  // stale entry for the same address.
  for (auto peer = peers_.crbegin(); peer != peers_.crend(); ++peer) {
    if (ConversationIdentity::sameAddress(peer->address, address)) {
      candidates.append(*peer);
    }
  }
  if (candidates.isEmpty()) {
    // "Connect To" also accepts a nickname in central mode.
    for (auto peer = peers_.crbegin(); peer != peers_.crend(); ++peer) {
      if (ConversationIdentity::sameNickname(peer->nickname, address)) {
        return *peer;
      }
    }
    return std::nullopt;
  }
  if (!nickname.trimmed().isEmpty()) {
    for (const auto &peer : std::as_const(candidates)) {
      if (ConversationIdentity::sameNickname(peer.nickname, nickname)) {
        return peer;
      }
    }
  }
  if (candidates.size() == 1) return candidates.first();
  for (const auto &peer : std::as_const(candidates)) {
    if (peer.port == port) return peer;
  }
  return candidates.first();
}

void CentralClient::setReconnectInterval(int milliseconds) {
  reconnectTimer_.setInterval(milliseconds);
}

ChatTransport::Factory CentralClient::transportFactory() {
  QPointer<CentralClient> client(this);
  return [client](QObject *parent) -> ChatTransport * {
    return new RelayTransport(client, parent);
  };
}

void CentralClient::connectToService() {
  if (host_.isEmpty()) return;
  socket_.abort();
  buffer_.clear();
  socket_.connectToHost(host_, port_);
}

void CentralClient::handleConnected() {
  // The service speaks first, with CENTRAL_CHALLENGE.
  idleTimer_.start();
}

void CentralClient::handleDisconnected(const QString &reason) {
  // Lines that arrived just before the close, such as CENTRAL_REPLACED.
  if (socket_.bytesAvailable() > 0) readLines();
  pingTimer_.stop();
  idleTimer_.stop();
  const bool wasReady = ready_;
  ready_ = false;
  clientId_.clear();
  buffer_.clear();
  failAllChannels(reason);
  if (!peers_.isEmpty()) {
    peers_.clear();
    emit peersChanged();
  } else if (wasReady) {
    emit peersChanged();
  }
  if (!replaced_) lastError_ = reason;
  if (isActive() && !replaced_) reconnectTimer_.start();
  emit stateChanged();
}

void CentralClient::readLines() {
  buffer_.append(socket_.readAll());
  while (true) {
    const auto newline = buffer_.indexOf('\n');
    if (newline < 0) {
      if (buffer_.size() > CentralProtocol::MaximumLineLength) {
        socket_.abort();
        handleDisconnected(
            QStringLiteral("The central host sent an invalid reply."));
      }
      return;
    }
    const auto line = QString::fromUtf8(buffer_.left(newline));
    buffer_.remove(0, newline + 1);
    idleTimer_.start();
    processLine(line.trimmed());
  }
}

void CentralClient::processLine(const QString &line) {
  const auto command = line.section(u' ', 0, 0);
  if (command == QStringLiteral("CENTRAL_DATA")) {
    const auto channelId = line.section(u' ', 1, 1);
    const auto channel = channels_.value(channelId);
    if (channel) channel->receivedLine(line.section(u' ', 2));
    return;
  }
  const auto parts = line.split(u' ', Qt::SkipEmptyParts);
  if (command == QStringLiteral("CENTRAL_CHALLENGE") && parts.size() == 2 &&
      clientId_.isEmpty()) {
    sendLine(QStringLiteral("CENTRAL_HELLO %1 %2 %3 %4")
                 .arg(CentralProtocol::Version)
                 .arg(QString::fromLatin1(CentralProtocol::encodeField(nickname_)))
                 .arg(listenPort_)
                 .arg(QString::fromLatin1(CentralProtocol::authProof(
                     password_, parts.at(1).toLatin1()))));
    pingTimer_.start();
    return;
  }
  if (command == QStringLiteral("CENTRAL_PEER") && parts.size() == 5) {
    bool portIsValid = false;
    Peer peer{parts.at(1), CentralProtocol::decodeField(parts.at(2)),
              parts.at(3), parts.at(4).toUShort(&portIsValid)};
    if (peer.nickname.isEmpty() || peer.id == clientId_) return;
    for (int index = 0; index < peers_.size(); ++index) {
      if (peers_.at(index).id == peer.id) {
        peers_.removeAt(index);
        break;
      }
    }
    peers_.append(peer);
    if (ready_) emit peersChanged();
    return;
  }
  if (command == QStringLiteral("CENTRAL_FILE_LIMIT") && parts.size() == 2) {
    bool valid = false;
    const auto limit = parts.at(1).toLongLong(&valid);
    maximumFileSize_ = valid && limit >= 0 ? limit : -1;
    return;
  }
  if (command == QStringLiteral("CENTRAL_FILE_REJECTED") && parts.size() == 4) {
    if (auto channel = channels_.value(parts.at(1))) {
      const auto limit = parts.at(3).toLongLong();
      emit channel->fileRefused(
          parts.at(2),
          limit == 0
              ? QStringLiteral("The central host does not relay files.")
              : QStringLiteral("The central host only relays files up to %1.")
                    .arg(QLocale::c().formattedDataSize(
                        limit, 0, QLocale::DataSizeTraditionalFormat)));
    }
    return;
  }
  if (command == QStringLiteral("CENTRAL_PEER_KIND") && parts.size() == 3) {
    for (auto &peer : peers_) {
      if (peer.id == parts.at(1)) {
        peer.web = parts.at(2) == QStringLiteral("web");
        if (ready_) emit peersChanged();
        break;
      }
    }
    return;
  }
  if (command == QStringLiteral("CENTRAL_PEER_GONE") && parts.size() == 2) {
    for (int index = 0; index < peers_.size(); ++index) {
      if (peers_.at(index).id == parts.at(1)) {
        peers_.removeAt(index);
        if (ready_) emit peersChanged();
        break;
      }
    }
    return;
  }
  if (command == QStringLiteral("CENTRAL_WELCOME") && parts.size() >= 2) {
    // The peer list precedes the welcome, so it is complete now.
    clientId_ = parts.at(1);
    ready_ = true;
    lastError_.clear();
    emit peersChanged();
    emit stateChanged();
    return;
  }
  if (command == QStringLiteral("CENTRAL_INCOMING") && parts.size() == 3) {
    const auto &channelId = parts.at(1);
    std::optional<Peer> source;
    for (const auto &peer : std::as_const(peers_)) {
      if (peer.id == parts.at(2)) source = peer;
    }
    if (!source || channels_.contains(channelId) ||
        !CentralProtocol::isValidChannelId(channelId)) {
      sendLine(QStringLiteral("CENTRAL_CLOSE %1 unavailable").arg(channelId));
      return;
    }
    auto *transport = new RelayTransport(this, this);
    transport->channelId_ = channelId;
    transport->peer_ = *source;
    transport->state_ = ChatTransport::State::Connected;
    channels_.insert(channelId, transport);
    emit incomingConnection(transport);
    if (transport->parent() == this) {
      transport->abort();
      transport->deleteLater();
    }
    return;
  }
  if (command == QStringLiteral("CENTRAL_OPENED") && parts.size() == 2) {
    if (const auto channel = channels_.value(parts.at(1))) channel->opened();
    return;
  }
  if (command == QStringLiteral("CENTRAL_CLOSE") && parts.size() >= 2) {
    const auto channel = channels_.take(parts.at(1));
    if (channel) {
      channel->closedByService(closeReasonText(
          parts.value(2), channel->peer_.nickname.isEmpty()
                              ? QStringLiteral("The other user")
                              : channel->peer_.nickname));
    }
    return;
  }
  if (command == QStringLiteral("CENTRAL_REPLACED")) {
    replaced_ = true;
    lastError_ = QStringLiteral(
        "Another LANTalk with the same nickname and address signed in to the "
        "central host.");
    return;
  }
  if (command == QStringLiteral("CENTRAL_REJECT")) {
    replaced_ = true;
    lastError_ = parts.size() >= 2
                     ? CentralProtocol::decodeField(parts.at(1))
                     : QStringLiteral("The central host refused the connection.");
    return;
  }
  // CENTRAL_PONG and unknown commands only keep the connection alive.
}

void CentralClient::sendLine(const QString &line) {
  if (socket_.state() != QAbstractSocket::ConnectedState) return;
  socket_.write(line.toUtf8() + '\n');
}

void CentralClient::openChannel(RelayTransport *transport,
                                const QString &targetId) {
  channels_.insert(transport->channelId_, transport);
  sendLine(QStringLiteral("CENTRAL_OPEN %1 %2")
               .arg(transport->channelId_, targetId));
}

void CentralClient::sendChannelLine(const QString &channelId,
                                    const QByteArray &line) {
  if (socket_.state() != QAbstractSocket::ConnectedState) return;
  socket_.write("CENTRAL_DATA " + channelId.toLatin1() + ' ' + line + '\n');
}

void CentralClient::closeChannel(const QString &channelId) {
  if (!channels_.remove(channelId)) return;
  sendLine(QStringLiteral("CENTRAL_CLOSE %1").arg(channelId));
}

void CentralClient::forgetChannel(const QString &channelId) {
  channels_.remove(channelId);
}

void CentralClient::failAllChannels(const QString &reason) {
  const auto channels = std::exchange(channels_, {});
  for (const auto &channel : channels) {
    if (channel) channel->closedByService(reason);
  }
}

RelayTransport::RelayTransport(CentralClient *client, QObject *parent)
    : ChatTransport(parent), client_(client) {
  readyTimeout_.setSingleShot(true);
  readyTimeout_.setInterval(ReadyTimeoutMs);
  connect(&readyTimeout_, &QTimer::timeout, this, [this] {
    const auto reason = client_ ? client_->lastError() : QString();
    fail(reason.isEmpty()
             ? QStringLiteral("Not connected to the central host.")
             : QStringLiteral("Not connected to the central host: %1")
                   .arg(reason));
  });
  if (client_) {
    connect(client_, &CentralClient::stateChanged, this, [this] {
      if (state_ == State::Connecting && channelId_.isEmpty() &&
          readyTimeout_.isActive() && client_->isReady()) {
        readyTimeout_.stop();
        resolveTarget();
      }
    });
  }
}

RelayTransport::~RelayTransport() { abort(); }

void RelayTransport::connectToHost(const QString &host, quint16 port,
                                   const QString &peerNameHint) {
  abort();
  state_ = State::Connecting;
  error_.clear();
  readBuffer_.clear();
  writeBuffer_.clear();
  requestedHost_ = host.trimmed();
  requestedPort_ = port;
  requestedName_ = peerNameHint;
  if (!client_ || !client_->isActive()) {
    QTimer::singleShot(0, this, [this] {
      if (state_ == State::Connecting && channelId_.isEmpty()) {
        fail(QStringLiteral("No central host is configured."));
      }
    });
    return;
  }
  if (!client_->isReady()) {
    readyTimeout_.start();
    return;
  }
  // Like a TCP connect, the outcome is always reported asynchronously.
  QTimer::singleShot(0, this, [this] {
    if (state_ == State::Connecting && channelId_.isEmpty()) resolveTarget();
  });
}

void RelayTransport::resolveTarget() {
  if (!client_ || !client_->isReady()) {
    fail(QStringLiteral("Not connected to the central host."));
    return;
  }
  const auto peer =
      client_->findPeer(requestedHost_, requestedPort_, requestedName_);
  if (!peer) {
    fail(QStringLiteral("%1 is not connected to the central host.")
             .arg(requestedName_.isEmpty() ? requestedHost_ : requestedName_));
    return;
  }
  peer_ = *peer;
  channelId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  client_->openChannel(this, peer_.id);
}

void RelayTransport::opened() {
  if (state_ != State::Connecting) return;
  state_ = State::Connected;
  emit connected();
  // Lines written while connecting, as a TCP socket would have buffered them.
  write({});
}

void RelayTransport::receivedLine(const QString &line) {
  if (state_ == State::Connecting) opened();
  if (state_ != State::Connected) return;
  readBuffer_.append(line.toUtf8() + '\n');
  emit readyRead();
}

void RelayTransport::closedByService(const QString &reason) {
  const auto previous = state_;
  channelId_.clear();
  readyTimeout_.stop();
  state_ = State::Unconnected;
  writeBuffer_.clear();
  error_ = reason;
  if (previous == State::Connected) emit disconnected();
  else if (previous == State::Connecting) emit errorOccurred();
}

void RelayTransport::fail(const QString &error) {
  if (client_ && !channelId_.isEmpty()) client_->closeChannel(channelId_);
  channelId_.clear();
  readyTimeout_.stop();
  state_ = State::Unconnected;
  writeBuffer_.clear();
  error_ = error;
  emit errorOccurred();
}

void RelayTransport::disconnectFromHost() {
  if (state_ == State::Unconnected) return;
  const bool wasConnected = state_ == State::Connected;
  if (wasConnected) write({});
  abort();
  if (wasConnected) {
    QTimer::singleShot(0, this, [this] { emit disconnected(); });
  }
}

void RelayTransport::abort() {
  readyTimeout_.stop();
  if (client_ && !channelId_.isEmpty()) client_->closeChannel(channelId_);
  channelId_.clear();
  state_ = State::Unconnected;
  writeBuffer_.clear();
}

ChatTransport::State RelayTransport::state() const { return state_; }

qint64 RelayTransport::write(const QByteArray &data) {
  if (state_ == State::Unconnected) return -1;
  writeBuffer_.append(data);
  if (state_ != State::Connected || !client_) return data.size();
  qsizetype start = 0;
  while (true) {
    const auto newline = writeBuffer_.indexOf('\n', start);
    if (newline < 0) break;
    client_->sendChannelLine(channelId_,
                             writeBuffer_.mid(start, newline - start));
    start = newline + 1;
  }
  writeBuffer_.remove(0, start);
  return data.size();
}

QByteArray RelayTransport::readAll() { return std::exchange(readBuffer_, {}); }

qint64 RelayTransport::bytesAvailable() const { return readBuffer_.size(); }

qint64 RelayTransport::bytesToWrite() const {
  return writeBuffer_.size() + (client_ ? client_->socket_.bytesToWrite() : 0);
}

QString RelayTransport::peerAddress() const { return peer_.address; }

quint16 RelayTransport::peerPort() const { return peer_.port; }

QString RelayTransport::errorString() const { return error_; }

// Both ends see the same channel id, and it is unique per connection.
QString RelayTransport::connectionKey(bool) const { return channelId_; }

QString RelayTransport::peerNickname() const { return peer_.nickname; }

bool RelayTransport::peerIsWebClient() const { return peer_.web; }

qint64 RelayTransport::maximumFileSize() const {
  return client_ ? client_->maximumFileSize() : -1;
}
