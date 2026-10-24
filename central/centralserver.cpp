#include "central/centralserver.h"

#include "central/centrallink.h"
#include "central/translationservice.h"

#include "appconfig.h"
#include "protocol/centralprotocol.h"
#include "protocol/conversationidentity.h"

#include <QJsonDocument>
#include <QRandomGenerator>
#include <QUuid>

namespace {

// While a receiver has this much waiting, its senders are not read, so TCP
// slows them down to the receiver's pace instead of lantalkd buffering a
// whole file.
constexpr qint64 RelayHighWater = 4LL * 1024 * 1024;
// A receiver this far behind despite that is dropped from the conversation.
constexpr qint64 MaximumPendingBytes = 64LL * 1024 * 1024;
// Clients must register this soon after connecting.
constexpr qint64 RegistrationTimeoutMs = 15 * 1000;
constexpr qsizetype MaximumNicknameLength = 256;
// Slows down password and PIN guessing: a failed sign-in is answered this
// late.
constexpr int RejectDelayMs = 1000;

QByteArray createNonce() {
  QByteArray nonce(16, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32 *>(nonce.data()), nonce.size() / 4);
  return nonce.toHex();
}

QString createId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QByteArray command(const QByteArray &line) {
  const auto space = line.indexOf(' ');
  return space < 0 ? line : line.left(space);
}

QString decodeChatField(const QByteArray &field) {
  return QString::fromUtf8(
      QByteArray::fromBase64(field, QByteArray::Base64UrlEncoding));
}

bool isVerifier(const QString &value) {
  if (value.size() != 64) return false;
  for (const auto character : value) {
    if (!((character >= u'0' && character <= u'9') ||
          (character >= u'a' && character <= u'f'))) {
      return false;
    }
  }
  return true;
}

} // namespace

CentralServer::CentralServer(QObject *parent)
    : QObject(parent), idleTimeoutMs_(CentralProtocol::IdleTimeoutMs),
      maximumFileSize_(MaximumFileSize), relayHighWater_(RelayHighWater),
      maximumPending_(MaximumPendingBytes) {
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

int CentralServer::channelCount() const { return int(conversations_.size()); }

void CentralServer::setPassword(const QString &password) {
  password_ = password;
}

bool CentralServer::setDataDirectory(const QString &directory, QString *error) {
  return accounts_.open(directory, error);
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

void CentralServer::setRelayLimits(qint64 highWater, qint64 maximumPending) {
  relayHighWater_ = highWater;
  maximumPending_ = maximumPending;
}

void CentralServer::receiverDrained(CentralLink *receiver) {
  if (receiver->bytesToWrite() > relayHighWater_ / 4) return;
  for (auto client = clients_.begin(); client != clients_.end(); ++client) {
    if (client->waitingFor != receiver) continue;
    client->waitingFor = nullptr;
    auto *sender = client.key();
    sender->setReading(true);
    // Lines it sent meanwhile are still waiting to be read.
    QTimer::singleShot(0, sender, [this, sender] { readClient(sender); });
  }
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
  connect(link, &CentralLink::bytesWritten, this,
          [this, link] { receiverDrained(link); });
  send(link, "CENTRAL_CHALLENGE " + client.nonce);
  // Lines may have arrived with the connection, e.g. in a WebSocket's
  // first frames.
  readClient(link);
}

void CentralServer::readClient(CentralLink *socket) {
  auto client = clients_.find(socket);
  if (client == clients_.end() || client->waitingFor) return;
  client->buffer.append(socket->readAll());
  client->lastActivity.start();
  while (clients_.contains(socket) && !clients_[socket].waitingFor) {
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
    if (client.channels.contains(parts.at(1))) {
      leaveConversation(socket, channelConversations_.value(parts.at(1)),
                        QStringLiteral("closed"), false);
    }
  } else if (command == QStringLiteral("CENTRAL_PING")) {
    send(socket, "CENTRAL_PONG");
  } else if (command == QStringLiteral("CENTRAL_STATUS") && parts.size() == 2) {
    setDeviceAway(socket, parts.at(1) == QStringLiteral("away"));
  } else if (command == QStringLiteral("CENTRAL_PIN_SET") && parts.size() == 2) {
    setPin(socket, parts.at(1));
  } else if (command == QStringLiteral("CENTRAL_SYNC") && parts.size() == 2) {
    synchronize(socket, parts.at(1));
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

void CentralServer::reject(CentralLink *socket, const QString &reason,
                           bool delayed, const QByteArray &before) {
  clients_[socket].rejected = true;
  const auto finish = [this, socket, reason, before] {
    if (!before.isEmpty()) send(socket, before);
    send(socket, "CENTRAL_REJECT " + CentralProtocol::encodeField(reason));
    dropClient(socket, QStringLiteral("was rejected"));
  };
  if (delayed) {
    QTimer::singleShot(RejectDelayMs, socket, finish);
  } else {
    finish();
  }
}

void CentralServer::registerClient(CentralLink *socket,
                                   const QStringList &parts) {
  bool versionIsValid = false;
  bool portIsValid = false;
  const int version = parts.value(1).toInt(&versionIsValid);
  const auto nickname =
      CentralProtocol::decodeField(parts.value(2)).simplified();
  const auto port = parts.value(3).toUShort(&portIsValid);
  // Version 1: CENTRAL_HELLO 1 <nickname> <port> <proof>; version 2 adds
  // <pinProof> <deviceId>.
  const bool withDevice = parts.size() == 7 && version >= 2 &&
                          CentralProtocol::isValidChannelId(parts.at(6));
  if ((parts.size() != 5 && !withDevice) || !versionIsValid || version < 1 ||
      !portIsValid || nickname.isEmpty() ||
      nickname.size() > MaximumNicknameLength) {
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
    log(QStringLiteral("Rejected sign-in by %1 (%2): %3")
            .arg(nickname, client.address,
                 withoutPassword ? QStringLiteral("no password")
                                 : QStringLiteral("wrong password")));
    reject(socket,
           withoutPassword
               ? QStringLiteral("The central host requires a password.")
               : QStringLiteral("Wrong password for the central host."),
           true);
    return;
  }

  const auto account = accounts_.isOpen() ? accounts_.find(nickname)
                                          : std::optional<AccountStore::Account>();
  if (account) {
    if (!withDevice) {
      log(QStringLiteral("Rejected sign-in by %1 (%2): the nickname has a PIN "
                         "and the client is too old for it")
              .arg(nickname, client.address));
      reject(socket,
             QStringLiteral("%1 is protected with a PIN. Update LANTalk to "
                            "sign in with it.")
                 .arg(account->name),
             false);
      return;
    }
    const auto proof = parts.at(5).toLatin1();
    qint64 lockedMs = 0;
    if (accounts_.isLocked(nickname, &lockedMs)) {
      reject(socket,
             QStringLiteral("Too many wrong PINs for %1. Try again in %2 "
                            "minutes.")
                 .arg(account->name)
                 .arg((lockedMs + 59999) / 60000),
             true, "CENTRAL_PIN_REQUIRED locked");
      return;
    }
    if (proof == "-") {
      reject(socket,
             QStringLiteral("%1 is protected with a PIN.").arg(account->name),
             false, "CENTRAL_PIN_REQUIRED missing");
      return;
    }
    if (!CentralProtocol::sameProof(
            proof, CentralProtocol::pinProof(account->verifier, client.nonce))) {
      accounts_.noteWrongPin(nickname);
      log(QStringLiteral("Rejected sign-in by %1 (%2): wrong PIN")
              .arg(nickname, client.address));
      reject(socket, QStringLiteral("Wrong PIN for %1.").arg(account->name),
             true, "CENTRAL_PIN_REQUIRED wrong");
      return;
    }
    accounts_.noteRightPin(nickname);
  }

  client.nickname = account ? account->name : nickname;
  client.port = port;
  client.version = version;
  if (withDevice) client.deviceId = parts.at(6);
  // An older registration of the same device, or of the same address and
  // nickname without a PIN, is a stale or restarted client.
  for (auto other = clients_.begin(); other != clients_.end(); ++other) {
    if (other.key() == socket || !other->registered) continue;
    const bool sameDevice =
        account && !other->accountName.isEmpty() &&
        ConversationIdentity::sameNickname(other->accountName, account->name) &&
        other->deviceId == client.deviceId;
    const bool sameClient =
        !account && other->accountName.isEmpty() &&
        ConversationIdentity::sameAddress(other->address, client.address) &&
        ConversationIdentity::sameNickname(other->nickname, nickname);
    if (!sameDevice && !sameClient) continue;
    auto *stale = other.key();
    send(stale, "CENTRAL_REPLACED");
    dropClient(stale, QStringLiteral("was replaced by a new sign-in"));
    break;
  }

  auto &registered = clients_[socket];
  registered.registered = true;
  bool newParty = true;
  if (account) {
    registered.accountName = account->name;
    registered.synced = false;
    for (auto &party : parties_) {
      if (ConversationIdentity::sameNickname(party.accountName, account->name)) {
        registered.partyId = party.id;
        party.devices.append(socket);
        newParty = false;
        break;
      }
    }
  }
  if (newParty) {
    Party party{createId(), registered.nickname, registered.address,
                registered.port, registered.accountName, {socket}};
    party.web = registered.web;
    registered.partyId = party.id;
    parties_.insert(party.id, party);
  }

  for (const auto &party : std::as_const(parties_)) {
    if (party.id != registered.partyId) announceParty(party, socket);
  }
  // Before the welcome, so the client asks for the history it missed before
  // it opens conversations.
  if (account) {
    send(socket, "CENTRAL_ACCOUNT ok " + account->id.toUtf8());
  } else if (withDevice) {
    send(socket, accounts_.isOpen() ? "CENTRAL_ACCOUNT none"
                                    : "CENTRAL_ACCOUNT unavailable");
  }
  send(socket, QStringLiteral("CENTRAL_WELCOME %1 %2")
                   .arg(registered.partyId, registered.address)
                   .toUtf8());
  const auto &party = parties_[registered.partyId];
  if (newParty) {
    broadcastAbout(party, peerLine(party));
    // Older clients ignore the unknown lines and list the peer as usual.
    if (party.web) broadcastAbout(party, kindLine(party));
    if (!party.accountName.isEmpty()) broadcastAbout(party, accountLine(party));
  } else {
    updateParty(party.id);
  }
  send(socket, "CENTRAL_FILE_LIMIT " + QByteArray::number(maximumFileSize_));
  if (translation_) send(socket, translation_->infoLine());
  log(QStringLiteral("%1 connected%2 (%3 online)")
          .arg(describe(registered),
               newParty ? QString()
                        : QStringLiteral(" as device %1 of the account")
                              .arg(parties_[registered.partyId].devices.size()))
          .arg(clientCount()));
}

void CentralServer::setPin(CentralLink *socket, const QString &verifier) {
  auto &client = clients_[socket];
  if (client.version < 2 || !isVerifier(verifier)) return;
  if (!accounts_.isOpen()) {
    send(socket, "CENTRAL_ACCOUNT unavailable");
    return;
  }
  if (!client.accountName.isEmpty()) {
    if (accounts_.setVerifier(client.accountName, verifier.toLatin1())) {
      send(socket, "CENTRAL_PIN_CHANGED");
      log(QStringLiteral("%1 changed the PIN").arg(describe(client)));
    }
    return;
  }
  if (accounts_.find(client.nickname)) {
    send(socket, "CENTRAL_ACCOUNT taken");
    return;
  }
  const auto account = accounts_.create(client.nickname, verifier.toLatin1());
  if (!account) {
    send(socket, "CENTRAL_ACCOUNT unavailable");
    log(QStringLiteral("Cannot save the account for %1 in %2")
            .arg(client.nickname, accounts_.directory()));
    return;
  }
  client.accountName = account->name;
  client.synced = false;
  auto &party = parties_[client.partyId];
  party.accountName = account->name;
  log(QStringLiteral("%1 protected the nickname with a PIN")
          .arg(describe(client)));
  // Whoever else uses the nickname must now sign in with the PIN.
  QList<CentralLink *> others;
  for (auto other = clients_.cbegin(); other != clients_.cend(); ++other) {
    if (other.key() != socket && other->registered &&
        other->accountName.isEmpty() &&
        ConversationIdentity::sameNickname(other->nickname, account->name)) {
      others.append(other.key());
    }
  }
  for (auto *other : std::as_const(others)) {
    reject(other,
           QStringLiteral("%1 is now protected with a PIN.").arg(account->name),
           false, "CENTRAL_PIN_REQUIRED missing");
  }
  broadcastAbout(parties_[client.partyId], accountLine(parties_[client.partyId]));
  send(socket, "CENTRAL_ACCOUNT ok " + account->id.toUtf8());
}

void CentralServer::synchronize(CentralLink *socket, const QString &after) {
  auto &client = clients_[socket];
  if (client.accountName.isEmpty() || client.synced) return;
  const auto account = accounts_.find(client.accountName);
  if (!account) return;
  bool valid = false;
  const auto sequence = after.toLongLong(&valid);
  for (const auto &entry :
       accounts_.entriesAfter(*account, valid ? qMax<qint64>(0, sequence) : 0)) {
    send(socket, "CENTRAL_HISTORY " +
                     QJsonDocument(entry).toJson(QJsonDocument::Compact).toBase64(
                         QByteArray::Base64UrlEncoding));
  }
  send(socket, "CENTRAL_SYNC_DONE " +
                   QByteArray::number(accounts_.lastSequence(*account)));
  client.synced = true;
  const auto partyId = client.partyId;
  QStringList joining;
  for (const auto &conversation : std::as_const(conversations_)) {
    if ((conversation.firstParty == partyId ||
         conversation.secondParty == partyId) &&
        !conversation.channels.contains(socket)) {
      joining.append(conversation.id);
    }
  }
  for (const auto &id : std::as_const(joining)) joinConversation(id, socket);
}

QString CentralServer::conversationBetween(const QString &first,
                                           const QString &second) const {
  for (const auto &conversation : conversations_) {
    if ((conversation.firstParty == first && conversation.secondParty == second) ||
        (conversation.firstParty == second && conversation.secondParty == first)) {
      return conversation.id;
    }
  }
  return {};
}

void CentralServer::openChannel(CentralLink *socket, const QString &channelId,
                                const QString &targetId) {
  const auto partyId = clients_.value(socket).partyId;
  if (!CentralProtocol::isValidChannelId(channelId) ||
      channelConversations_.contains(channelId) || targetId == partyId ||
      !parties_.contains(targetId)) {
    send(socket, "CENTRAL_CLOSE " + channelId.toUtf8() + " unavailable");
    return;
  }
  auto conversationId = conversationBetween(partyId, targetId);
  const bool existing = !conversationId.isEmpty();
  if (!existing) {
    conversationId = createId();
    conversations_.insert(conversationId, {conversationId, partyId, targetId});
  } else if (const auto old = conversations_[conversationId].channels.value(socket);
             !old.isEmpty()) {
    // The device starts over, e.g. after its conversation stalled.
    conversations_[conversationId].channels.remove(socket);
    channelConversations_.remove(old);
    clients_[socket].channels.remove(old);
    send(socket, "CENTRAL_CLOSE " + old.toUtf8() + " closed");
  }
  conversations_[conversationId].channels.insert(socket, channelId);
  channelConversations_.insert(channelId, conversationId);
  clients_[socket].channels.insert(channelId);
  send(socket, "CENTRAL_OPENED " + channelId.toUtf8());
  if (existing) {
    replayConversation(conversations_[conversationId], socket);
    return;
  }
  // The other party's devices, and this one's others, all take part.
  QList<CentralLink *> joining = parties_.value(targetId).devices;
  for (auto *device : parties_.value(partyId).devices) {
    if (device != socket) joining.append(device);
  }
  for (auto *device : std::as_const(joining)) {
    if (clients_.value(device).synced) joinConversation(conversationId, device);
  }
}

void CentralServer::joinConversation(const QString &conversationId,
                                     CentralLink *device) {
  auto conversation = conversations_.find(conversationId);
  if (conversation == conversations_.end() ||
      conversation->channels.contains(device)) {
    return;
  }
  const auto channelId = createId();
  conversation->channels.insert(device, channelId);
  channelConversations_.insert(channelId, conversationId);
  clients_[device].channels.insert(channelId);
  send(device, QStringLiteral("CENTRAL_INCOMING %1 %2")
                   .arg(channelId, otherParty(*conversation,
                                              clients_.value(device).partyId))
                   .toUtf8());
  replayConversation(*conversation, device);
}

void CentralServer::replayConversation(const Conversation &conversation,
                                       CentralLink *device) {
  // What the device would have seen had it been there from the start.
  const auto channel = conversation.channels.value(device).toUtf8();
  const auto own = clients_.value(device).partyId;
  const auto other = otherParty(conversation, own);
  const auto data = [&](const QByteArray &line) {
    send(device, "CENTRAL_DATA " + channel + ' ' + line);
  };
  const auto handshake = conversation.handshake.value(other);
  for (const auto *name : {"NICKNAME", "CAPABILITIES"}) {
    const auto line = handshake.value(name);
    if (!line.isEmpty()) data(line);
  }
  if (handshake.contains("NICKNAME") && parties_.value(other).away &&
      supportsPresence(conversation, own)) {
    data("PRESENCE AWAY");
  }
  if (!conversation.translateGranted) return;
  if (conversation.translator == other) {
    // The device grants it; that answer goes no further.
    data(conversation.translateRequest);
  } else {
    send(device, "CENTRAL_MIRROR " + channel + ' ' + conversation.translateRequest);
    data("TRANSLATE_GRANTED " + conversation.translateId.toUtf8());
  }
}

void CentralServer::relay(CentralLink *socket, const QString &channelId,
                          const QByteArray &line) {
  const auto conversationId = channelConversations_.value(channelId);
  auto conversation = conversations_.find(conversationId);
  if (conversation == conversations_.end() ||
      conversation->channels.value(socket) != channelId) {
    return;
  }
  const auto side = clients_.value(socket).partyId;
  const auto other = otherParty(*conversation, side);
  const auto name = command(line);
  const auto parts = line.split(' ');

  // Lines to send, worked out first: delivering one can end the conversation.
  QList<QPair<CentralLink *, QByteArray>> outgoing;
  const auto toParty = [&](const QString &party, const QByteArray &text) {
    for (auto *device : devicesIn(*conversation, party)) {
      outgoing.append({device, "CENTRAL_DATA " +
                                   conversation->channels.value(device).toUtf8() +
                                   ' ' + text});
    }
  };
  const auto toDevice = [&](CentralLink *device, const QByteArray &text) {
    outgoing.append({device, "CENTRAL_DATA " +
                                 conversation->channels.value(device).toUtf8() +
                                 ' ' + text});
  };
  // What one device sends, its account's other devices see as their own.
  const auto mirror = [&] {
    for (auto *device : devicesIn(*conversation, side)) {
      if (device == socket) continue;
      outgoing.append({device, "CENTRAL_MIRROR " +
                                   conversation->channels.value(device).toUtf8() +
                                   ' ' + line});
    }
  };
  const auto clearTranslation = [&] {
    conversation->translator.clear();
    conversation->translateId.clear();
    conversation->translateRequest.clear();
    conversation->translateGranted = false;
    conversation->waitingRequests.clear();
  };

  if (name == "NICKNAME" || name == "CAPABILITIES") {
    // Each party introduces itself once, however many devices it has.
    auto &handshake = conversation->handshake[side];
    if (handshake.contains(name)) return;
    handshake.insert(name, line);
    toParty(other, line);
    if (name == "CAPABILITIES" && parts.contains("PRESENCE") &&
        conversation->handshake.value(other).contains("NICKNAME") &&
        parties_.value(other).away) {
      toParty(side, "PRESENCE AWAY");
    }
  } else if (name == "PRESENCE") {
    // A party is away when all its devices are.
    if (parts.size() == 2 && (parts.at(1) == "AWAY" || parts.at(1) == "ACTIVE")) {
      setDeviceAway(socket, parts.at(1) == "AWAY");
    }
    return;
  } else if (name == "TRANSLATE_REQUEST" && parts.size() == 4) {
    const auto id = QString::fromUtf8(parts.at(1));
    if (!conversation->translator.isEmpty()) {
      if (conversation->translator != side) {
        toDevice(socket, "TRANSLATE_REJECT " + parts.at(1));
      } else if (conversation->translateGranted) {
        toDevice(socket, "TRANSLATE_GRANTED " + parts.at(1));
      } else {
        conversation->waitingRequests.append({socket, id});
      }
    } else {
      conversation->translator = side;
      conversation->translateId = id;
      conversation->translateRequest = line;
      toParty(other, line);
      mirror();
    }
  } else if (name == "TRANSLATE_GRANTED" || name == "TRANSLATE_REJECT") {
    // The first answer counts; the other devices' are dropped.
    if (parts.size() != 2 || conversation->translator != other ||
        conversation->translateGranted ||
        QString::fromUtf8(parts.at(1)) != conversation->translateId) {
      return;
    }
    toParty(other, line);
    for (const auto &[device, id] : std::as_const(conversation->waitingRequests)) {
      toDevice(device, name + ' ' + id.toUtf8());
    }
    conversation->waitingRequests.clear();
    if (name == "TRANSLATE_GRANTED") {
      conversation->translateGranted = true;
    } else {
      // The other devices may have granted it already.
      for (auto *device : devicesIn(*conversation, side)) {
        if (device != socket) toDevice(device, "TRANSLATE_CANCEL " + parts.at(1));
      }
      clearTranslation();
    }
  } else if (name == "TRANSLATE_STOP" || name == "TRANSLATE_CANCEL") {
    if (conversation->translator != side) return;
    if (name == "TRANSLATE_CANCEL" &&
        QString::fromUtf8(parts.value(1)) != conversation->translateId) {
      // Only one device's own request, still waiting for the first.
      for (int index = 0; index < conversation->waitingRequests.size(); ++index) {
        if (conversation->waitingRequests.at(index).first == socket) {
          conversation->waitingRequests.removeAt(index);
          break;
        }
      }
      return;
    }
    toParty(other, line);
    mirror();
    clearTranslation();
  } else if (name == "TYPING") {
    toParty(other, line);
  } else if (name == "MESSAGE") {
    toParty(other, line);
    mirror();
  } else if (name.startsWith("FILE_")) {
    // The one place lantalkd reads a transfer: files over the size limit
    // stop here. FILE_BEGIN <id> <name> <type> <size>, then FILE_CHUNK <id>
    // <data> and FILE_END <id>.
    const auto id = QString::fromUtf8(parts.value(1));
    if (name == "FILE_BEGIN") {
      bool sizeIsValid = false;
      const auto size = parts.value(4).toLongLong(&sizeIsValid);
      if (sizeIsValid && size > maximumFileSize_) {
        conversation->refusedFiles.insert(id);
        send(socket, "CENTRAL_FILE_REJECTED " + channelId.toUtf8() + ' ' +
                         id.toUtf8() + ' ' + QByteArray::number(maximumFileSize_));
        log(QStringLiteral("Refused a file of %1 bytes from %2 (limit %3)")
                .arg(size)
                .arg(describe(clients_.value(socket)))
                .arg(maximumFileSize_));
        return;
      }
    } else if (conversation->refusedFiles.contains(id)) {
      if (name == "FILE_END") conversation->refusedFiles.remove(id);
      return;
    }
    toParty(other, line);
    mirror();
  } else {
    toParty(other, line);
  }

  QList<CentralLink *> overloaded;
  for (const auto &[device, text] : std::as_const(outgoing)) {
    if (device->bytesToWrite() > maximumPending_) {
      if (!overloaded.contains(device)) overloaded.append(device);
      continue;
    }
    send(device, text);
    if (device->bytesToWrite() > relayHighWater_ && !clients_[socket].waitingFor) {
      clients_[socket].waitingFor = device;
      socket->setReading(false);
    }
  }
  if (name == "MESSAGE") storeMessage(*conversation, side, line);
  for (auto *device : std::as_const(overloaded)) {
    leaveConversation(device, conversationId, QStringLiteral("overloaded"), true);
  }
}

void CentralServer::storeMessage(const Conversation &conversation,
                                 const QString &fromParty,
                                 const QByteArray &line) {
  if (!accounts_.isOpen()) return;
  // MESSAGE <text> [ORG_LANG <original>]
  const auto parts = line.split(' ');
  if (parts.size() != 2 && parts.size() != 4) return;
  const auto text = decodeChatField(parts.at(1));
  const auto original = parts.size() == 4 ? decodeChatField(parts.at(3)) : QString();
  if (text.isEmpty()) return;
  const auto time =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  const auto toParty = otherParty(conversation, fromParty);
  for (const auto &partyId : {fromParty, toParty}) {
    const auto party = parties_.value(partyId);
    const auto account =
        party.accountName.isEmpty() ? std::nullopt : accounts_.find(party.accountName);
    if (!account) continue;
    const bool outgoing = partyId == fromParty;
    const auto peer = parties_.value(outgoing ? toParty : fromParty);
    // Like LANTalk's own history: what the user wrote or read, and the other
    // language beside it.
    QJsonObject entry{
        {QStringLiteral("peer"), peer.nickname},
        {QStringLiteral("address"), peer.address},
        {QStringLiteral("account"), !peer.accountName.isEmpty()},
        {QStringLiteral("out"), outgoing},
        {QStringLiteral("time"), time},
        {QStringLiteral("message"), outgoing && !original.isEmpty() ? original : text}};
    const auto alternate = outgoing ? (original.isEmpty() ? QString() : text) : original;
    if (!alternate.isEmpty()) entry.insert(QStringLiteral("alternate"), alternate);
    const auto sequence = accounts_.append(*account, entry);
    if (sequence == 0) {
      log(QStringLiteral("Cannot store the history of %1 in %2")
              .arg(account->name, accounts_.directory()));
      continue;
    }
    entry.insert(QStringLiteral("seq"), sequence);
    const auto encoded = QJsonDocument(entry).toJson(QJsonDocument::Compact).toBase64(
        QByteArray::Base64UrlEncoding);
    // Devices in the conversation saw the message; the others learn of it.
    for (auto *device : party.devices) {
      if (!clients_.value(device).synced) continue;
      send(device, conversation.channels.contains(device)
                       ? "CENTRAL_SEQ " + QByteArray::number(sequence)
                       : "CENTRAL_HISTORY " + encoded);
    }
  }
}

void CentralServer::leaveConversation(CentralLink *device,
                                      const QString &conversationId,
                                      const QString &reason, bool notifyDevice) {
  auto conversation = conversations_.find(conversationId);
  if (conversation == conversations_.end()) return;
  const auto channelId = conversation->channels.take(device);
  if (channelId.isEmpty()) return;
  channelConversations_.remove(channelId);
  if (clients_.contains(device)) {
    clients_[device].channels.remove(channelId);
    if (notifyDevice) {
      send(device, "CENTRAL_CLOSE " + channelId.toUtf8() + ' ' + reason.toUtf8());
    }
  }
  for (int index = 0; index < conversation->waitingRequests.size(); ++index) {
    if (conversation->waitingRequests.at(index).first == device) {
      conversation->waitingRequests.removeAt(index--);
    }
  }
  // A party without devices in the conversation has left it.
  const auto partyId = clients_.contains(device) ? clients_.value(device).partyId
                                                 : QString();
  for (const auto &party : {conversation->firstParty, conversation->secondParty}) {
    if (devicesIn(*conversation, party).isEmpty() &&
        (party == partyId || partyId.isEmpty() || !parties_.contains(party))) {
      endConversation(conversationId, reason);
      return;
    }
  }
}

void CentralServer::endConversation(const QString &conversationId,
                                    const QString &reason) {
  const auto conversation = conversations_.take(conversationId);
  for (auto channel = conversation.channels.cbegin();
       channel != conversation.channels.cend(); ++channel) {
    channelConversations_.remove(channel.value());
    if (!clients_.contains(channel.key())) continue;
    clients_[channel.key()].channels.remove(channel.value());
    send(channel.key(), "CENTRAL_CLOSE " + channel.value().toUtf8() + ' ' +
                            reason.toUtf8());
  }
}

void CentralServer::dropClient(CentralLink *socket, const QString &why) {
  const auto found = clients_.find(socket);
  if (found == clients_.end()) return;
  const auto client = *found;
  // Its senders need not wait for it any longer.
  for (auto other = clients_.begin(); other != clients_.end(); ++other) {
    if (other->waitingFor != socket) continue;
    other->waitingFor = nullptr;
    auto *sender = other.key();
    sender->setReading(true);
    QTimer::singleShot(0, sender, [this, sender] { readClient(sender); });
  }
  for (const auto &channelId : client.channels) {
    leaveConversation(socket, channelConversations_.value(channelId),
                      QStringLiteral("closed"), false);
  }
  clients_.remove(socket);
  disconnect(socket, nullptr, this, nullptr);
  // Let queued lines (e.g. CENTRAL_REPLACED) reach the client before the
  // socket goes away, but never wait for a stuck one for long.
  connect(socket, &CentralLink::disconnected, socket, &QObject::deleteLater);
  QTimer::singleShot(5000, socket, &QObject::deleteLater);
  socket->close();
  if (socket->isUnconnected()) socket->deleteLater();
  if (!client.registered) return;

  auto party = parties_.find(client.partyId);
  if (party != parties_.end()) {
    party->devices.removeAll(socket);
    if (party->devices.isEmpty()) {
      const auto partyId = party->id;
      parties_.erase(party);
      QStringList ended;
      for (const auto &conversation : std::as_const(conversations_)) {
        if (conversation.firstParty == partyId || conversation.secondParty == partyId) {
          ended.append(conversation.id);
        }
      }
      for (const auto &id : std::as_const(ended)) {
        endConversation(id, QStringLiteral("closed"));
      }
      broadcast("CENTRAL_PEER_GONE " + partyId.toUtf8(), nullptr);
    } else {
      updateParty(party->id);
    }
  }
  log(QStringLiteral("%1 %2 (%3 online)")
          .arg(describe(client), why)
          .arg(clientCount()));
}

void CentralServer::setDeviceAway(CentralLink *socket, bool away) {
  auto &client = clients_[socket];
  if (client.away == away) return;
  client.away = away;
  updateParty(client.partyId);
}

void CentralServer::updateParty(const QString &partyId) {
  auto party = parties_.find(partyId);
  if (party == parties_.end() || party->devices.isEmpty()) return;
  bool away = true;
  bool web = true;
  for (auto *device : party->devices) {
    away = away && clients_.value(device).away;
    web = web && clients_.value(device).web;
  }
  if (party->web != web) {
    party->web = web;
    broadcastAbout(*party, kindLine(*party));
  }
  if (!party->accountName.isEmpty()) broadcastAbout(*party, accountLine(*party));
  if (party->away == away) return;
  party->away = away;
  broadcastAbout(*party, statusLine(*party));
  // Conversations hear it too, from peers that understand PRESENCE.
  for (const auto &conversation : std::as_const(conversations_)) {
    if (conversation.firstParty != partyId && conversation.secondParty != partyId) {
      continue;
    }
    const auto other = otherParty(conversation, partyId);
    if (!supportsPresence(conversation, other) ||
        !conversation.handshake.value(partyId).contains("NICKNAME")) {
      continue;
    }
    for (auto *device : devicesIn(conversation, other)) {
      send(device, "CENTRAL_DATA " + conversation.channels.value(device).toUtf8() +
                       (away ? " PRESENCE AWAY" : " PRESENCE ACTIVE"));
    }
  }
}

void CentralServer::sweepIdleClients() {
  QList<CentralLink *> idle;
  for (auto client = clients_.cbegin(); client != clients_.cend(); ++client) {
    const auto limit = client->registered
                           ? qint64(idleTimeoutMs_)
                           : qMin<qint64>(idleTimeoutMs_, RegistrationTimeoutMs);
    // A sender held back for a slow receiver is not silent.
    if (client->waitingFor) continue;
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

void CentralServer::broadcastAbout(const Party &party, const QByteArray &line) {
  for (auto client = clients_.cbegin(); client != clients_.cend(); ++client) {
    if (client->registered && client->partyId != party.id) {
      send(client.key(), line);
    }
  }
}

void CentralServer::announceParty(const Party &party, CentralLink *to) {
  send(to, peerLine(party));
  if (party.web) send(to, kindLine(party));
  if (party.away) send(to, statusLine(party));
  if (!party.accountName.isEmpty()) send(to, accountLine(party));
}

QByteArray CentralServer::statusLine(const Party &party) const {
  return "CENTRAL_PEER_STATUS " + party.id.toUtf8() +
         (party.away ? " away" : " active");
}

QByteArray CentralServer::kindLine(const Party &party) const {
  return "CENTRAL_PEER_KIND " + party.id.toUtf8() + (party.web ? " web" : " app");
}

QByteArray CentralServer::accountLine(const Party &party) const {
  return "CENTRAL_PEER_ACCOUNT " + party.id.toUtf8() + ' ' +
         QByteArray::number(party.devices.size());
}

QByteArray CentralServer::peerLine(const Party &party) const {
  return QStringLiteral("CENTRAL_PEER %1 %2 %3 %4")
      .arg(party.id,
           QString::fromLatin1(CentralProtocol::encodeField(party.nickname)),
           party.address)
      .arg(party.port)
      .toUtf8();
}

QString CentralServer::otherParty(const Conversation &conversation,
                                  const QString &partyId) const {
  return conversation.firstParty == partyId ? conversation.secondParty
                                            : conversation.firstParty;
}

QList<CentralLink *> CentralServer::devicesIn(const Conversation &conversation,
                                              const QString &partyId) const {
  QList<CentralLink *> devices;
  for (auto channel = conversation.channels.cbegin();
       channel != conversation.channels.cend(); ++channel) {
    if (clients_.value(channel.key()).partyId == partyId) {
      devices.append(channel.key());
    }
  }
  return devices;
}

bool CentralServer::supportsPresence(const Conversation &conversation,
                                     const QString &partyId) const {
  return conversation.handshake.value(partyId)
      .value("CAPABILITIES")
      .split(' ')
      .contains("PRESENCE");
}

QString CentralServer::describe(const Client &client) const {
  return QStringLiteral("%1 (%2)").arg(client.nickname, client.address);
}

void CentralServer::log(const QString &message) const {
  if (logger_) logger_(message);
}
