#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpSocket;

// One client connection of lantalkd, carrying CENTRAL_ lines: a plain TCP
// socket for LANTalk, or a WebSocket for the web client.
class CentralLink : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;

  // Received bytes, with each line ended by '\n'.
  virtual QByteArray readAll() = 0;
  // Sends one line, given without its '\n'.
  virtual void writeLine(const QByteArray &line) = 0;
  virtual qint64 bytesToWrite() const = 0;
  virtual bool isConnected() const = 0;
  virtual bool isUnconnected() const = 0;
  virtual QString peerAddress() const = 0;
  // A browser running the web client.
  virtual bool isWeb() const { return false; }
  // Closes once pending output is written; disconnected() follows.
  virtual void close() = 0;
  virtual void abort() = 0;
  // While off, the link takes in no more data, so TCP slows the sender
  // down; lines already received can still be read.
  virtual void setReading(bool reading) { Q_UNUSED(reading) }

signals:
  void readyRead();
  void disconnected();
  void bytesWritten();
};

class TcpLink final : public CentralLink {
  Q_OBJECT

public:
  // Takes ownership of the connected socket.
  explicit TcpLink(QTcpSocket *socket, QObject *parent = nullptr);

  QByteArray readAll() override;
  void writeLine(const QByteArray &line) override;
  qint64 bytesToWrite() const override;
  bool isConnected() const override;
  bool isUnconnected() const override;
  QString peerAddress() const override;
  void close() override;
  void abort() override;

private:
  QTcpSocket *socket_;
};
