#include "protocol/chattransport.h"

#include "protocol/centralprotocol.h"

#include <QTcpSocket>

namespace {

ChatTransport::Factory &factory() {
  static ChatTransport::Factory current;
  return current;
}

} // namespace

ChatTransport *ChatTransport::create(QObject *parent) {
  if (factory()) return factory()(parent);
  return new TcpTransport(new QTcpSocket, parent);
}

void ChatTransport::setFactory(Factory newFactory) {
  factory() = std::move(newFactory);
}

TcpTransport::TcpTransport(QTcpSocket *socket, QObject *parent)
    : ChatTransport(parent), socket_(socket) {
  socket_->setParent(this);
  connect(socket_, &QTcpSocket::connected, this, &ChatTransport::connected);
  connect(socket_, &QTcpSocket::readyRead, this, &ChatTransport::readyRead);
  connect(socket_, &QTcpSocket::bytesWritten, this,
          &ChatTransport::bytesWritten);
  connect(socket_, &QTcpSocket::disconnected, this,
          &ChatTransport::disconnected);
  connect(socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) { emit errorOccurred(); });
}

void TcpTransport::connectToHost(const QString &host, quint16 port,
                                 const QString &) {
  socket_->connectToHost(host, port);
}

void TcpTransport::disconnectFromHost() { socket_->disconnectFromHost(); }

void TcpTransport::abort() { socket_->abort(); }

ChatTransport::State TcpTransport::state() const {
  switch (socket_->state()) {
  case QAbstractSocket::ConnectedState:
    return State::Connected;
  case QAbstractSocket::UnconnectedState:
    return State::Unconnected;
  default:
    return State::Connecting;
  }
}

qint64 TcpTransport::write(const QByteArray &data) {
  return socket_->write(data);
}

QByteArray TcpTransport::readAll() { return socket_->readAll(); }

qint64 TcpTransport::bytesAvailable() const {
  return socket_->bytesAvailable();
}

qint64 TcpTransport::bytesToWrite() const { return socket_->bytesToWrite(); }

QString TcpTransport::peerAddress() const {
  return CentralProtocol::formatAddress(socket_->peerAddress());
}

quint16 TcpTransport::peerPort() const { return socket_->peerPort(); }

QString TcpTransport::errorString() const { return socket_->errorString(); }

// Both ends of a TCP connection derive the same key for it (initiator
// endpoint first).
QString TcpTransport::connectionKey(bool outgoing) const {
  const auto local =
      QStringLiteral("%1:%2")
          .arg(CentralProtocol::formatAddress(socket_->localAddress()))
          .arg(socket_->localPort());
  const auto remote =
      QStringLiteral("%1:%2")
          .arg(CentralProtocol::formatAddress(socket_->peerAddress()))
          .arg(socket_->peerPort());
  return outgoing ? local + u'>' + remote : remote + u'>' + local;
}
