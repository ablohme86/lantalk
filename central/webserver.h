#pragma once

#include "central/centrallink.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class CentralServer;
class QTcpServer;
class QTcpSocket;

// lantalkd's web client: serves the chat page to browsers (phones in
// particular) and turns each WebSocket from it into a CentralServer client.
// The page speaks the CENTRAL and LANCHAT protocols itself, so the service
// still only relays lines.
class WebServer final : public QObject {
  Q_OBJECT

public:
  explicit WebServer(CentralServer *central, QObject *parent = nullptr);
  ~WebServer() override;

  // Serves HTTPS and WSS with this PEM certificate (chain) and key; returns
  // false with errorString() set when they cannot be used.
  bool setCertificate(const QString &certificatePath, const QString &keyPath);
  bool listen(const QHostAddress &address, quint16 port);
  quint16 serverPort() const;
  QString errorString() const;
  bool isSecure() const;

private:
  void accept(QTcpSocket *socket);
  void readRequest(QTcpSocket *socket);
  void respond(QTcpSocket *socket, const QByteArray &status,
               const QByteArray &contentType, const QByteArray &body,
               bool headOnly = false);

  QPointer<CentralServer> central_;
  std::unique_ptr<QTcpServer> server_;
  QString error_;
  bool secure_ = false;
};

// A CentralLink over a WebSocket (RFC 6455): each text message is one line.
class WebSocketLink final : public CentralLink {
  Q_OBJECT

public:
  // Takes ownership of the socket, whose upgrade handshake is done;
  // `pending` holds bytes that followed the handshake.
  WebSocketLink(QTcpSocket *socket, const QByteArray &pending,
                QObject *parent = nullptr);

  QByteArray readAll() override;
  void writeLine(const QByteArray &line) override;
  qint64 bytesToWrite() const override;
  bool isConnected() const override;
  bool isUnconnected() const override;
  QString peerAddress() const override;
  bool isWeb() const override { return true; }
  void close() override;
  void abort() override;
  void setReading(bool reading) override;

private:
  void readFrames();
  void sendFrame(quint8 opcode, const QByteArray &payload);
  void fail();

  QTcpSocket *socket_;
  QByteArray input_;
  QByteArray message_;
  QByteArray lines_;
  bool closing_ = false;
  bool reading_ = true;
};
