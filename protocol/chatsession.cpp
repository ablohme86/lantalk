#include "protocol/chatsession.h"

#include "appconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUuid>

ChatSession::ChatSession(QTcpSocket *socket, QObject *parent)
    : QObject(parent), socket_(socket) {
  socket_->setParent(this);
  connect(socket_, &QTcpSocket::readyRead, this, &ChatSession::readData);
  outgoingFilePump_.setSingleShot(true);
  connect(&outgoingFilePump_, &QTimer::timeout, this,
          &ChatSession::sendNextFileChunk);
  connect(socket_, &QTcpSocket::bytesWritten, this, [this](qint64) {
    if (outgoingFile_ && socket_->bytesToWrite() < 128 * 1024 &&
        !outgoingFilePump_.isActive()) {
      outgoingFilePump_.start(0);
    }
  });
  connect(socket_, &QTcpSocket::disconnected, this, [this] {
    cancelOutgoingFiles();
    emit disconnected();
  });
  connect(socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
            emit statusChanged(socket_->errorString());
          });
}

ChatSession::~ChatSession() {
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
  sendLine(QStringLiteral("NICKNAME %1").arg(nickname_));
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
    return;
  }
  const auto originalPayload =
      originalMessage.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
  sendLine(QStringLiteral("MESSAGE %1 ORG_LANG %2")
               .arg(QString::fromLatin1(payload),
                    QString::fromLatin1(originalPayload)));
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

QString ChatSession::peerName() const { return peerName_; }
QString ChatSession::peerAddress() const {
  const auto address = socket_->peerAddress();
  return address.protocol() == QAbstractSocket::IPv4Protocol ? address.toString()
                                                               : QString();
}
quint16 ChatSession::peerPort() const { return socket_->peerPort(); }

void ChatSession::readData() {
  buffer_.append(socket_->readAll());
  while (true) {
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
    peerName_ = line.mid(9).trimmed();
    if (peerName_.isEmpty()) {
      socket_->disconnectFromHost();
      return;
    }
    handshakeComplete_ = true;
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
    emit messageReceived(QString::fromUtf8(message),
                         QString::fromUtf8(originalMessage));
    return;
  }
  const auto parts = line.split(u' ', Qt::KeepEmptyParts);
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
      emit statusChanged(QStringLiteral("Cannot locate the LANChat cache folder."));
      return;
    }
    const auto directory = QDir(cacheDirectory).filePath(QStringLiteral("received"));
    if (!QDir().mkpath(directory)) {
      emit statusChanged(
          QStringLiteral("Cannot create the LANChat cache folder."));
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
