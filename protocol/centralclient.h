#pragma once

#include "protocol/chattransport.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include <optional>

class RelayTransport;

// The connection to lantalkd, the central host. While a central host is
// configured LANTalk neither listens for chats nor scans the LAN: the service
// lists the other connected clients and relays every conversation, each over
// its own channel (see protocol/centralprotocol.h).
class CentralClient final : public QObject {
  Q_OBJECT

public:
  struct Peer {
    QString id;
    QString nickname;
    QString address;
    quint16 port = 0;
  };

  explicit CentralClient(QObject *parent = nullptr);
  ~CentralClient() override;

  // Connects to `hostSpec` ("host" or "host:port") and registers as
  // `nickname`, with `password` if the service requires one, reconnecting
  // whenever the connection is lost. Calling it again with other values
  // starts over; an empty spec is the same as stop().
  void start(const QString &hostSpec, const QString &nickname,
             quint16 listenPort, const QString &password = {});
  void stop();
  bool isActive() const;
  // Registered with the service, so the peer list is current.
  bool isReady() const;
  QString hostSpec() const;
  QString lastError() const;
  QList<Peer> peers() const;
  // The connected client a conversation with address:port (and nickname, if
  // known) should go to. `address` may also be a nickname.
  std::optional<Peer> findPeer(const QString &address, quint16 port,
                               const QString &nickname = {}) const;
  void setReconnectInterval(int milliseconds);

  // A ChatTransport::Factory that makes relay transports for this client.
  ChatTransport::Factory transportFactory();

signals:
  void stateChanged();
  void peersChanged();
  // Another client opened a conversation. Take ownership of `transport`
  // (e.g. with a ChatSession); otherwise it is closed.
  void incomingConnection(RelayTransport *transport);

private:
  friend class RelayTransport;

  void connectToService();
  void handleConnected();
  void handleDisconnected(const QString &reason);
  void readLines();
  void processLine(const QString &line);
  void sendLine(const QString &line);
  void openChannel(RelayTransport *transport, const QString &targetId);
  void sendChannelLine(const QString &channelId, const QByteArray &line);
  void closeChannel(const QString &channelId);
  void forgetChannel(const QString &channelId);
  void failAllChannels(const QString &reason);

  QTcpSocket socket_;
  QByteArray buffer_;
  QTimer reconnectTimer_;
  QTimer pingTimer_;
  QTimer idleTimer_;
  QString hostSpec_;
  QString host_;
  quint16 port_ = 0;
  QString nickname_;
  quint16 listenPort_ = 0;
  QString password_;
  QString clientId_;
  QString lastError_;
  bool ready_ = false;
  // Stops reconnecting after the service refused this client (e.g. a wrong
  // password) or replaced it with another instance using the same address
  // and nickname, until start() is called again.
  bool replaced_ = false;
  QList<Peer> peers_;
  QHash<QString, QPointer<RelayTransport>> channels_;
};

// One relayed conversation. Opened with connectToHost(), which picks the
// connected client by address, port and nickname, or created by
// CentralClient::incomingConnection() for one another client opened.
class RelayTransport final : public ChatTransport {
  Q_OBJECT

public:
  explicit RelayTransport(CentralClient *client, QObject *parent = nullptr);
  ~RelayTransport() override;

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
  QString peerNickname() const;

private:
  friend class CentralClient;

  void resolveTarget();
  void opened();
  void receivedLine(const QString &line);
  void closedByService(const QString &reason);
  void fail(const QString &error);

  QPointer<CentralClient> client_;
  State state_ = State::Unconnected;
  QString channelId_;
  CentralClient::Peer peer_;
  QString requestedHost_;
  quint16 requestedPort_ = 0;
  QString requestedName_;
  QByteArray readBuffer_;
  QByteArray writeBuffer_;
  QString error_;
  QTimer readyTimeout_;
};
