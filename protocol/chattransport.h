#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

class QTcpSocket;

// The byte stream a ChatSession speaks LANCHAT PROTOCOL over: a direct TCP
// connection on the LAN, or a channel relayed by a central host.
class ChatTransport : public QObject {
  Q_OBJECT

public:
  enum class State { Unconnected, Connecting, Connected };
  using Factory = std::function<ChatTransport *(QObject *parent)>;

  using QObject::QObject;

  // `peerNameHint` is the nickname expected at address:port, if known; a
  // relay uses it to pick between clients that share an address.
  virtual void connectToHost(const QString &host, quint16 port,
                             const QString &peerNameHint) = 0;
  virtual void disconnectFromHost() = 0;
  // Drops the connection immediately, discarding unsent data.
  virtual void abort() = 0;
  virtual State state() const = 0;
  virtual qint64 write(const QByteArray &data) = 0;
  virtual QByteArray readAll() = 0;
  virtual qint64 bytesAvailable() const = 0;
  virtual qint64 bytesToWrite() const = 0;
  virtual QString peerAddress() const = 0;
  virtual quint16 peerPort() const = 0;
  virtual QString errorString() const = 0;
  // Identical at both ends of one connection and different for every other
  // connection; used to resolve simultaneous connects deterministically.
  virtual QString connectionKey(bool outgoing) const = 0;

  // A new, unconnected transport for outgoing connections in the current
  // network mode (direct TCP unless a central host is configured).
  static ChatTransport *create(QObject *parent);
  // Replaces the transport create() makes; an empty factory restores TCP.
  static void setFactory(Factory factory);

signals:
  void connected();
  void readyRead();
  void bytesWritten(qint64 bytes);
  void disconnected();
  void errorOccurred();
};

class TcpTransport final : public ChatTransport {
  Q_OBJECT

public:
  // Takes ownership of `socket`, connected (incoming) or not (outgoing).
  explicit TcpTransport(QTcpSocket *socket, QObject *parent = nullptr);

  void connectToHost(const QString &host, quint16 port,
                     const QString &peerNameHint) override;
  void disconnectFromHost() override;
  void abort() override;
  State state() const override;
  qint64 write(const QByteArray &data) override;
  QByteArray readAll() override;
  qint64 bytesAvailable() const override;
  qint64 bytesToWrite() const override;
  QString peerAddress() const override;
  quint16 peerPort() const override;
  QString errorString() const override;
  QString connectionKey(bool outgoing) const override;

private:
  QTcpSocket *socket_;
};
