#include "central/centrallink.h"

#include "protocol/centralprotocol.h"

#include <QTcpSocket>

TcpLink::TcpLink(QTcpSocket *socket, QObject *parent)
    : CentralLink(parent), socket_(socket) {
  socket_->setParent(this);
  socket_->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
  // Unread data stays in the kernel, so not reading slows the sender down.
  socket_->setReadBufferSize(256 * 1024);
  connect(socket_, &QTcpSocket::readyRead, this, &CentralLink::readyRead);
  connect(socket_, &QTcpSocket::disconnected, this, &CentralLink::disconnected);
  connect(socket_, &QTcpSocket::bytesWritten, this, &CentralLink::bytesWritten);
}

QByteArray TcpLink::readAll() { return socket_->readAll(); }

void TcpLink::writeLine(const QByteArray &line) {
  if (isConnected()) socket_->write(line + '\n');
}

qint64 TcpLink::bytesToWrite() const { return socket_->bytesToWrite(); }

bool TcpLink::isConnected() const {
  return socket_->state() == QAbstractSocket::ConnectedState;
}

bool TcpLink::isUnconnected() const {
  return socket_->state() == QAbstractSocket::UnconnectedState;
}

QString TcpLink::peerAddress() const {
  return CentralProtocol::formatAddress(socket_->peerAddress());
}

void TcpLink::close() {
  if (isConnected()) socket_->disconnectFromHost();
}

void TcpLink::abort() { socket_->abort(); }
