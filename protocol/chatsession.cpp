#include "protocol/chatsession.h"

#include "appconfig.h"
#include "protocol/connectionregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUuid>

#include <utility>

namespace {

QString formatPeerAddress(const QHostAddress &address) {
  if (address.isNull()) return {};
  if (address.protocol() == QAbstractSocket::IPv4Protocol) {
    return address.toString();
  }
  if (address.protocol() != QAbstractSocket::IPv6Protocol) {
    return address.toString();
  }

  const auto ipv6 = address.toIPv6Address();
  bool isIpv4Mapped = ipv6[10] == 0xff && ipv6[11] == 0xff;
  for (int index = 0; index < 10 && isIpv4Mapped; ++index) {
    isIpv4Mapped = ipv6[index] == 0;
  }
  if (!isIpv4Mapped) return address.toString();

  return QStringLiteral("%1.%2.%3.%4")
      .arg(ipv6[12])
      .arg(ipv6[13])
      .arg(ipv6[14])
      .arg(ipv6[15]);
}

} // namespace

ChatSession::ChatSession(QTcpSocket *socket, QObject *parent,
                         ConnectionRegistry *registry)
    : QObject(parent),
      registry_(registry ? registry : ConnectionRegistry::shared()),
      socket_(socket) {
  peerAddress_ = formatPeerAddress(socket_->peerAddress());
  outgoingFilePump_.setSingleShot(true);
  connect(&outgoingFilePump_, &QTimer::timeout, this,
          &ChatSession::sendNextFileChunk);
  localTypingIdleTimer_.setSingleShot(true);
  localTypingIdleTimer_.setInterval(TypingInactivityTimeoutMs);
  connect(&localTypingIdleTimer_, &QTimer::timeout, this,
          &ChatSession::stopLocalTyping);
  localTypingRefreshTimer_.setInterval(TypingRefreshIntervalMs);
  connect(&localTypingRefreshTimer_, &QTimer::timeout, this, [this] {
    if (localTyping_ && handshakeComplete_ && isConnected()) {
      sendLine(QStringLiteral("TYPING START"));
    }
  });
  peerTypingExpiryTimer_.setSingleShot(true);
  peerTypingExpiryTimer_.setInterval(TypingIndicatorTimeoutMs);
  connect(&peerTypingExpiryTimer_, &QTimer::timeout, this,
          [this] { setPeerTyping(false); });
  setupSocket();
  registry_->add(this);
}

void ChatSession::setupSocket() {
  socket_->setParent(this);
  connect(socket_, &QTcpSocket::connected, this, [this] {
    peerAddress_ = formatPeerAddress(socket_->peerAddress());
    if (!nickname_.isEmpty() && !handshakeComplete_) {
      sendLine(QStringLiteral("NICKNAME %1").arg(nickname_));
    }
  });
  connect(socket_, &QTcpSocket::readyRead, this, &ChatSession::readData);
  connect(socket_, &QTcpSocket::bytesWritten, this, [this](qint64) {
    if (outgoingFile_ && socket_->bytesToWrite() < 128 * 1024 &&
        !outgoingFilePump_.isActive()) {
      outgoingFilePump_.start(0);
    }
  });
  connect(socket_, &QTcpSocket::disconnected, this, [this] {
    notifyDisconnected();
  });
  connect(socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
            emit statusChanged(socket_->errorString());
            if (socket_->state() == QAbstractSocket::UnconnectedState) {
              notifyDisconnected();
            }
          });
}

ChatSession::~ChatSession() {
  registry_->remove(this);
  outgoingFilePump_.stop();
  if (outgoingFile_) {
    outgoingFile_->close();
  }
  for (const auto &path : receivedFilePaths_) {
    QFile::remove(path);
  }
}

void ChatSession::beginHandshake(const QString &nickname) {
  nickname_ = escapeProtocolField(nickname);
  if (socket_->state() == QAbstractSocket::ConnectedState &&
      !handshakeComplete_) {
    sendLine(QStringLiteral("NICKNAME %1").arg(nickname_));
  }
}

void ChatSession::connectToHost(const QString &address, quint16 port) {
  closed_ = false;
  outgoing_ = true;
  remoteHost_ = address.trimmed();
  remotePort_ = port;
  const auto requestedAddress = formatPeerAddress(QHostAddress(remoteHost_));
  if (!requestedAddress.isEmpty()) peerAddress_ = requestedAddress;
  socket_->connectToHost(remoteHost_, port);
}

void ChatSession::reconnectToHost(const QString &address, quint16 port) {
  if (nickname_.isEmpty() || address.trimmed().isEmpty() || port == 0) {
    return;
  }

  cancelOutgoingFiles();
  auto *oldSocket = socket_;
  disconnect(oldSocket, nullptr, this, nullptr);
  oldSocket->abort();
  oldSocket->deleteLater();
  resetConnectionState();

  socket_ = new QTcpSocket(this);
  setupSocket();
  connectToHost(address, port);
}

void ChatSession::resetConnectionState() {
  buffer_.clear();
  peerName_.clear();
  handshakeComplete_ = false;
  handshakeClock_.invalidate();
  translationActive_ = false;
  peerTranslationActive_ = false;
  translationRequestId_.clear();
  incomingFiles_.clear();
  disconnectNotified_ = false;
  peerSupportsTyping_ = false;
  resetTypingState();
}

void ChatSession::setRemoteEndpoint(const QString &address, quint16 port) {
  remoteHost_ = address.trimmed();
  remotePort_ = port;
}

void ChatSession::closeConnection() {
  stopLocalTyping();
  closed_ = true;
  cancelOutgoingFiles();
  if (socket_->state() == QAbstractSocket::ConnectedState) {
    socket_->disconnectFromHost();
  } else {
    socket_->abort();
  }
}

void ChatSession::notifyDisconnected() {
  if (disconnectNotified_) {
    return;
  }
  disconnectNotified_ = true;
  cancelOutgoingFiles();
  translationActive_ = false;
  peerTranslationActive_ = false;
  translationRequestId_.clear();
  resetTypingState();
  peerSupportsTyping_ = false;
  emit disconnected();
}

void ChatSession::sendMessage(const QString &message,
                              const QString &originalMessage) {
  if (!handshakeComplete_) {
    emit statusChanged(QStringLiteral("Waiting for the nickname handshake."));
    return;
  }
  const auto payload = message.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
  if (originalMessage.isEmpty()) {
    sendLine(QStringLiteral("MESSAGE %1").arg(QString::fromLatin1(payload)));
    if (localTyping_) sendLine(QStringLiteral("TYPING START"));
    return;
  }
  const auto originalPayload =
      originalMessage.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
  sendLine(QStringLiteral("MESSAGE %1 ORG_LANG %2")
               .arg(QString::fromLatin1(payload),
                    QString::fromLatin1(originalPayload)));
  // The peer hides its indicator when a message arrives; restore it if the
  // user is already composing the next one.
  if (localTyping_) sendLine(QStringLiteral("TYPING START"));
}

bool ChatSession::sendFile(const QString &filePath) {
  if (!handshakeComplete_) {
    emit statusChanged(QStringLiteral("Waiting for the nickname handshake."));
    return false;
  }
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    emit statusChanged(
        QStringLiteral("Cannot read %1.").arg(QFileInfo(filePath).fileName()));
    return false;
  }
  if (file.size() > MaximumFileSize) {
    emit statusChanged(
        QStringLiteral("Files larger than 50 MB cannot be sent."));
    return false;
  }
  file.close();

  outgoingFilePaths_.enqueue(filePath);
  if (!outgoingFileBatchActive_) {
    outgoingFileBatchActive_ = true;
  }
  startNextFile();
  return true;
}

void ChatSession::startNextFile() {
  if (outgoingFile_) {
    return;
  }
  while (!outgoingFilePaths_.isEmpty()) {
    const auto path = outgoingFilePaths_.dequeue();
    auto *file = new QFile(path, this);
    if (!file->open(QIODevice::ReadOnly)) {
      emit statusChanged(
          QStringLiteral("Cannot read %1.").arg(QFileInfo(path).fileName()));
      file->deleteLater();
      continue;
    }
    if (file->size() > MaximumFileSize) {
      emit statusChanged(
          QStringLiteral("Files larger than 50 MB cannot be sent."));
      file->deleteLater();
      continue;
    }

    outgoingFile_ = file;
    outgoingFilePath_ = path;
    outgoingFileName_ = QFileInfo(path).fileName();
    outgoingMimeType_ = QMimeDatabase().mimeTypeForFile(path).name();
    outgoingFileSize_ = file->size();
    outgoingBytesSent_ = 0;
    outgoingFileId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto encodedName = outgoingFileName_.toUtf8().toBase64(
        QByteArray::Base64UrlEncoding);
    const auto encodedMime = outgoingMimeType_.toUtf8().toBase64(
        QByteArray::Base64UrlEncoding);
    sendLine(QStringLiteral("FILE_BEGIN %1 %2 %3 %4")
                 .arg(outgoingFileId_, QString::fromLatin1(encodedName),
                      QString::fromLatin1(encodedMime))
                 .arg(outgoingFileSize_));
    emit fileSendProgressChanged(0);
    outgoingFilePump_.start(20);
    return;
  }

  if (outgoingFileBatchActive_) {
    outgoingFileBatchActive_ = false;
    emit fileSendCompleted();
  }
}

void ChatSession::sendNextFileChunk() {
  if (!outgoingFile_) {
    return;
  }
  if (socket_->state() != QAbstractSocket::ConnectedState) {
    cancelOutgoingFiles();
    return;
  }

  if (outgoingFile_->atEnd()) {
    sendLine(QStringLiteral("FILE_END %1").arg(outgoingFileId_));
    const auto name = outgoingFileName_;
    const auto path = outgoingFilePath_;
    const auto mimeType = outgoingMimeType_;
    outgoingFile_->close();
    outgoingFile_->deleteLater();
    outgoingFile_ = nullptr;
    outgoingFileId_.clear();
    outgoingFileName_.clear();
    outgoingFilePath_.clear();
    outgoingMimeType_.clear();
    outgoingFileSize_ = 0;
    outgoingBytesSent_ = 0;
    emit fileSendProgressChanged(100);
    emit fileSent(name, path, mimeType);
    startNextFile();
    return;
  }

  if (socket_->bytesToWrite() >= 256 * 1024) {
    return;
  }
  const auto chunk = outgoingFile_->read(16 * 1024);
  if (chunk.isEmpty()) {
    if (outgoingFile_->error() == QFileDevice::NoError &&
        outgoingFile_->atEnd()) {
      outgoingFilePump_.start(0);
      return;
    }
    emit statusChanged(QStringLiteral("Cannot read %1.").arg(outgoingFileName_));
    sendLine(QStringLiteral("FILE_END %1").arg(outgoingFileId_));
    outgoingFile_->close();
    outgoingFile_->deleteLater();
    outgoingFile_ = nullptr;
    outgoingFileId_.clear();
    outgoingFileName_.clear();
    outgoingFilePath_.clear();
    outgoingMimeType_.clear();
    outgoingFileSize_ = 0;
    outgoingBytesSent_ = 0;
    startNextFile();
    return;
  }

  const auto encodedChunk = chunk.toBase64(QByteArray::Base64UrlEncoding);
  sendLine(QStringLiteral("FILE_CHUNK %1 %2")
               .arg(outgoingFileId_, QString::fromLatin1(encodedChunk)));
  outgoingBytesSent_ += chunk.size();
  const int percent = outgoingFileSize_ == 0
                          ? 100
                          : static_cast<int>(outgoingBytesSent_ * 100 /
                                             outgoingFileSize_);
  emit fileSendProgressChanged(qMin(percent, 100));
  outgoingFilePump_.start(0);
}

void ChatSession::cancelOutgoingFiles() {
  outgoingFilePump_.stop();
  if (outgoingFile_) {
    outgoingFile_->close();
    outgoingFile_->deleteLater();
    outgoingFile_ = nullptr;
  }
  outgoingFilePaths_.clear();
  outgoingFileId_.clear();
  outgoingFileName_.clear();
  outgoingFilePath_.clear();
  outgoingMimeType_.clear();
  outgoingFileSize_ = 0;
  outgoingBytesSent_ = 0;
  if (outgoingFileBatchActive_) {
    outgoingFileBatchActive_ = false;
    emit fileSendCompleted();
  }
}

void ChatSession::requestLiveTranslation(const QString &sourceLanguage,
                                         const QString &targetLanguage) {
  if (peerTranslationActive_) {
    emit liveTranslationDenied(
        QStringLiteral("Live Translate is already active for the other user."));
    return;
  }
  if (translationActive_ || !translationRequestId_.isEmpty()) {
    return;
  }
  translationRequestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto source =
      sourceLanguage.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
  const auto target =
      targetLanguage.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
  sendLine(QStringLiteral("TRANSLATE_REQUEST %1 %2 %3")
               .arg(translationRequestId_, QString::fromLatin1(source),
                    QString::fromLatin1(target)));
  emit statusChanged(QStringLiteral("Requesting Live Translate…"));
}

void ChatSession::stopLiveTranslation() {
  if (!translationActive_ && translationRequestId_.isEmpty()) {
    return;
  }
  if (!translationRequestId_.isEmpty()) {
    sendLine(QStringLiteral("TRANSLATE_CANCEL %1").arg(translationRequestId_));
  } else {
    sendLine(QStringLiteral("TRANSLATE_STOP"));
  }
  translationActive_ = false;
  translationRequestId_.clear();
}

bool ChatSession::liveTranslationActiveOrPending() const {
  return translationActive_ || !translationRequestId_.isEmpty();
}

void ChatSession::noteLocalTyping(bool hasDraft) {
  if (!hasDraft) {
    stopLocalTyping();
    return;
  }
  if (!handshakeComplete_ || !peerSupportsTyping_ || !isConnected()) {
    return;
  }
  localTypingIdleTimer_.start();
  if (localTyping_) return;
  localTyping_ = true;
  sendLine(QStringLiteral("TYPING START"));
  localTypingRefreshTimer_.start();
}

void ChatSession::stopLocalTyping() {
  localTypingIdleTimer_.stop();
  localTypingRefreshTimer_.stop();
  if (!localTyping_) return;
  localTyping_ = false;
  if (handshakeComplete_ && isConnected()) {
    sendLine(QStringLiteral("TYPING STOP"));
  }
}

bool ChatSession::peerTyping() const { return peerTyping_; }

bool ChatSession::peerSupportsTyping() const { return peerSupportsTyping_; }

void ChatSession::setTypingTimeouts(int inactivityMs, int refreshMs,
                                    int expiryMs) {
  localTypingIdleTimer_.setInterval(inactivityMs);
  localTypingRefreshTimer_.setInterval(refreshMs);
  peerTypingExpiryTimer_.setInterval(expiryMs);
}

void ChatSession::setPeerTyping(bool typing) {
  if (!typing) peerTypingExpiryTimer_.stop();
  if (peerTyping_ == typing) return;
  peerTyping_ = typing;
  emit peerTypingChanged(typing);
}

void ChatSession::resetTypingState() {
  localTypingIdleTimer_.stop();
  localTypingRefreshTimer_.stop();
  localTyping_ = false;
  setPeerTyping(false);
}

QString ChatSession::peerName() const { return peerName_; }
QString ChatSession::identityName() const { return identityName_; }
QString ChatSession::peerAddress() const {
  const auto currentAddress = formatPeerAddress(socket_->peerAddress());
  return currentAddress.isEmpty() ? peerAddress_ : currentAddress;
}
quint16 ChatSession::peerPort() const { return socket_->peerPort(); }
QString ChatSession::remoteHost() const { return remoteHost_; }
quint16 ChatSession::remotePort() const { return remotePort_; }

bool ChatSession::isConnected() const {
  return socket_->state() == QAbstractSocket::ConnectedState;
}

bool ChatSession::isHandshakeComplete() const { return handshakeComplete_; }

bool ChatSession::isRetired() const { return closed_ || merged_; }

qint64 ChatSession::handshakeAgeMs() const {
  return handshakeClock_.isValid() ? handshakeClock_.elapsed() : -1;
}

// Both ends of a TCP connection derive the same key for it (initiator
// endpoint first), so peers resolving a simultaneous-connect race agree on
// which connection survives.
QString ChatSession::connectionKey() const {
  const auto local = QStringLiteral("%1:%2")
                         .arg(formatPeerAddress(socket_->localAddress()))
                         .arg(socket_->localPort());
  const auto remote = QStringLiteral("%1:%2")
                          .arg(formatPeerAddress(socket_->peerAddress()))
                          .arg(socket_->peerPort());
  return outgoing_ ? local + u'>' + remote : remote + u'>' + local;
}

void ChatSession::adoptConnection(ChatSession *duplicate) {
  if (handshakeComplete_ && !disconnectNotified_) notifyDisconnected();
  cancelOutgoingFiles();
  auto *oldSocket = socket_;
  disconnect(oldSocket, nullptr, this, nullptr);
  oldSocket->abort();
  oldSocket->deleteLater();
  resetConnectionState();

  socket_ = duplicate->socket_;
  disconnect(socket_, nullptr, duplicate, nullptr);
  duplicate->socket_ = new QTcpSocket(duplicate);
  buffer_ = std::exchange(duplicate->buffer_, {});
  outgoing_ = duplicate->outgoing_;
  if (remoteHost_.isEmpty() || remotePort_ == 0) {
    remoteHost_ = duplicate->remoteHost_;
    remotePort_ = duplicate->remotePort_;
  }
  peerAddress_ = duplicate->peerAddress_;
  peerName_ = duplicate->peerName_;
  identityName_ = peerName_;
  peerSupportsTyping_ = duplicate->peerSupportsTyping_;
  handshakeComplete_ = true;
  handshakeClock_ = duplicate->handshakeClock_;
  closed_ = false;
  setupSocket();

  duplicate->merged_ = true;
  duplicate->resetConnectionState();
  emit peerIdentified(peerName_);
  emit statusChanged(QStringLiteral("Connected to %1").arg(peerName_));
  emit duplicate->mergedInto(this);
  if (!buffer_.isEmpty() || socket_->bytesAvailable() > 0) {
    QMetaObject::invokeMethod(this, &ChatSession::readData,
                              Qt::QueuedConnection);
  }
}

void ChatSession::retireAsDuplicate(ChatSession *existing) {
  merged_ = true;
  cancelOutgoingFiles();
  disconnect(socket_, nullptr, this, nullptr);
  socket_->disconnectFromHost();
  resetConnectionState();
  emit mergedInto(existing);
}

void ChatSession::readData() {
  buffer_.append(socket_->readAll());
  while (!merged_) {
    const auto newline = buffer_.indexOf('\n');
    if (newline < 0) {
      if (buffer_.size() > 1024 * 1024) {
        socket_->disconnectFromHost();
      }
      return;
    }
    const auto line = QString::fromUtf8(buffer_.left(newline)).trimmed();
    buffer_.remove(0, newline + 1);
    processLine(line);
  }
}

void ChatSession::processLine(const QString &line) {
  if (line.startsWith(QStringLiteral("NICKNAME "))) {
    const auto name = line.mid(9).trimmed();
    if (name.isEmpty()) {
      socket_->disconnectFromHost();
      return;
    }
    const bool firstHandshake = !handshakeComplete_;
    peerName_ = name;
    identityName_ = name;
    handshakeComplete_ = true;
    if (firstHandshake) {
      handshakeClock_.start();
      // Sent only after the handshake so the NICKNAME exchange stays
      // compatible; peers without this line never receive TYPING commands.
      sendLine(QStringLiteral("CAPABILITIES TYPING"));
      registry_->resolveDuplicate(this);
      if (merged_) return;
    }
    emit peerIdentified(peerName_);
    emit statusChanged(QStringLiteral("Connected to %1").arg(peerName_));
    return;
  }
  if (line.startsWith(QStringLiteral("MESSAGE ")) && handshakeComplete_) {
    const auto parts = line.split(u' ', Qt::KeepEmptyParts);
    if ((parts.size() != 2 &&
         (parts.size() != 4 || parts.at(2) != QStringLiteral("ORG_LANG")))) {
      emit statusChanged(QStringLiteral("Received malformed message."));
      return;
    }
    const auto message = QByteArray::fromBase64(parts.at(1).toLatin1(),
                                                QByteArray::Base64UrlEncoding);
    const auto originalMessage =
        parts.size() == 4
            ? QByteArray::fromBase64(parts.at(3).toLatin1(),
                                     QByteArray::Base64UrlEncoding)
            : QByteArray();
    if (message.isEmpty() || (parts.size() == 4 && originalMessage.isEmpty())) {
      emit statusChanged(QStringLiteral("Received malformed message."));
      return;
    }
    setPeerTyping(false);
    emit messageReceived(QString::fromUtf8(message),
                         QString::fromUtf8(originalMessage));
    return;
  }
  const auto parts = line.split(u' ', Qt::KeepEmptyParts);
  if (parts.value(0) == QStringLiteral("CAPABILITIES") && handshakeComplete_) {
    peerSupportsTyping_ = parts.contains(QStringLiteral("TYPING"));
    return;
  }
  if (parts.value(0) == QStringLiteral("TYPING") && handshakeComplete_) {
    // Unknown TYPING variants are ignored for forward compatibility.
    if (parts.size() == 2 && parts.at(1) == QStringLiteral("START")) {
      peerSupportsTyping_ = true;
      setPeerTyping(true);
      peerTypingExpiryTimer_.start();
    } else if (parts.size() == 2 && parts.at(1) == QStringLiteral("STOP")) {
      setPeerTyping(false);
    }
    return;
  }
  if (parts.value(0) == QStringLiteral("TRANSLATE_REQUEST") &&
      parts.size() == 4 && handshakeComplete_) {
    const auto &requestId = parts.at(1);
    const auto source = QByteArray::fromBase64(parts.at(2).toLatin1(),
                                               QByteArray::Base64UrlEncoding);
    const auto target = QByteArray::fromBase64(parts.at(3).toLatin1(),
                                               QByteArray::Base64UrlEncoding);
    if (source.isEmpty() || target.isEmpty()) {
      sendLine(QStringLiteral("TRANSLATE_REJECT %1").arg(requestId));
      return;
    }
    if (translationActive_ || peerTranslationActive_) {
      sendLine(QStringLiteral("TRANSLATE_REJECT %1").arg(requestId));
      return;
    }
    if (!translationRequestId_.isEmpty() && translationRequestId_ < requestId) {
      sendLine(QStringLiteral("TRANSLATE_REJECT %1").arg(requestId));
      return;
    }
    if (!translationRequestId_.isEmpty()) {
      translationRequestId_.clear();
      emit liveTranslationDenied(
          QStringLiteral("The other user requested Live Translate first."));
    }
    peerTranslationActive_ = true;
    sendLine(QStringLiteral("TRANSLATE_GRANTED %1").arg(requestId));
    emit peerLiveTranslationChanged(true, QString::fromUtf8(source),
                                    QString::fromUtf8(target));
    return;
  }
  if (parts.value(0) == QStringLiteral("TRANSLATE_GRANTED") &&
      parts.size() == 2 && parts.at(1) == translationRequestId_) {
    translationRequestId_.clear();
    translationActive_ = true;
    emit liveTranslationGranted();
    return;
  }
  if (parts.value(0) == QStringLiteral("TRANSLATE_REJECT") &&
      parts.size() == 2 && parts.at(1) == translationRequestId_) {
    translationRequestId_.clear();
    emit liveTranslationDenied(
        QStringLiteral("Live Translate is already active for the other user."));
    return;
  }
  if (parts.value(0) == QStringLiteral("TRANSLATE_STOP") && parts.size() == 1 &&
      handshakeComplete_) {
    peerTranslationActive_ = false;
    emit peerLiveTranslationChanged(false, {}, {});
    return;
  }
  if (parts.value(0) == QStringLiteral("TRANSLATE_CANCEL") &&
      parts.size() == 2 && handshakeComplete_) {
    if (peerTranslationActive_) {
      peerTranslationActive_ = false;
      emit peerLiveTranslationChanged(false, {}, {});
    }
    return;
  }
  if (parts.value(0) == QStringLiteral("FILE_BEGIN") && parts.size() == 5 &&
      handshakeComplete_) {
    bool sizeIsValid = false;
    const auto size = parts.at(4).toLongLong(&sizeIsValid);
    const auto name = QByteArray::fromBase64(parts.at(2).toLatin1(),
                                             QByteArray::Base64UrlEncoding);
    const auto mime = QByteArray::fromBase64(parts.at(3).toLatin1(),
                                             QByteArray::Base64UrlEncoding);
    const auto safeName = QFileInfo(QString::fromUtf8(name)).fileName();
    if (!sizeIsValid || size < 0 || size > MaximumFileSize ||
        safeName.isEmpty() || safeName == QStringLiteral(".") ||
        safeName == QStringLiteral("..") || mime.isEmpty()) {
      emit statusChanged(QStringLiteral("Rejected invalid file transfer."));
      return;
    }
    incomingFiles_.insert(
        parts.at(1),
        {safeName, QString::fromUtf8(mime), size, {}});
    return;
  }
  if (parts.value(0) == QStringLiteral("FILE_CHUNK") && parts.size() == 3 &&
      handshakeComplete_) {
    auto transfer = incomingFiles_.find(parts.at(1));
    if (transfer == incomingFiles_.end()) {
      return;
    }
    const auto chunk = QByteArray::fromBase64(parts.at(2).toLatin1(),
                                              QByteArray::Base64UrlEncoding);
    if (chunk.isEmpty() ||
        transfer->data.size() + chunk.size() > transfer->size) {
      incomingFiles_.erase(transfer);
      emit statusChanged(QStringLiteral("Rejected invalid file data."));
      return;
    }
    transfer->data.append(chunk);
    return;
  }
  if (parts.value(0) == QStringLiteral("FILE_END") && parts.size() == 2 &&
      handshakeComplete_) {
    auto transfer = incomingFiles_.find(parts.at(1));
    if (transfer == incomingFiles_.end()) {
      return;
    }
    const auto completed = *transfer;
    incomingFiles_.erase(transfer);
    if (completed.data.size() != completed.size) {
      emit statusChanged(QStringLiteral("File transfer was incomplete."));
      return;
    }
    const auto cacheDirectory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDirectory.isEmpty()) {
      emit statusChanged(QStringLiteral("Cannot locate the LANTalk cache folder."));
      return;
    }
    const auto directory = QDir(cacheDirectory).filePath(QStringLiteral("received"));
    if (!QDir().mkpath(directory)) {
      emit statusChanged(
          QStringLiteral("Cannot create the LANTalk cache folder."));
      return;
    }
    const auto safeName = QFileInfo(completed.name).fileName();
    if (safeName.isEmpty() || safeName == QStringLiteral(".") ||
        safeName == QStringLiteral("..")) {
      emit statusChanged(QStringLiteral("Received file has an invalid name."));
      return;
    }
    const auto destination =
        QDir(directory).filePath(QStringLiteral("%1-%2").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            safeName));
    QFile destinationFile(destination);
    if (!destinationFile.open(QIODevice::WriteOnly) ||
        destinationFile.write(completed.data) != completed.data.size()) {
      destinationFile.close();
      QFile::remove(destination);
      emit statusChanged(QStringLiteral("Cannot save received file."));
      return;
    }
    receivedFilePaths_.append(destination);
    emit fileReceived(completed.name, destination, completed.mimeType);
    return;
  }
  emit statusChanged(
      QStringLiteral("Received unsupported LANCHAT PROTOCOL command."));
}

void ChatSession::sendLine(const QString &line) {
  socket_->write(line.toUtf8() + '\n');
}
