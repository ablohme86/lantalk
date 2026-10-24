#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QTimer>

#include <functional>

class CentralLink;
class TranslationService;

// lantalkd: keeps the list of connected LANTalk clients
// and relays each conversation between two of them, line by line, without
// interpreting it (see protocol/centralprotocol.h).
class CentralServer final : public QObject {
  Q_OBJECT

public:
  explicit CentralServer(QObject *parent = nullptr);
  ~CentralServer() override;

  bool listen(const QHostAddress &address, quint16 port);
  quint16 serverPort() const;
  QString errorString() const;
  // Registered clients.
  int clientCount() const;
  int channelCount() const;
  // Clients must prove they know `password` to sign in; empty allows all.
  void setPassword(const QString &password);
  void setLogger(std::function<void(const QString &)> logger);
  void setIdleTimeout(int milliseconds);
  // Serves a client that connected some other way, e.g. the web client's
  // WebSocket; the server takes ownership.
  void addLink(CentralLink *link);
  // Offers Live Translate through lantalkd to clients (see
  // central/translationservice.h).
  void setTranslationService(TranslationService *service);
  // Relays files up to this size; larger ones are refused. 0 allows none.
  void setMaximumFileSize(qint64 bytes);
  // A sender stops being read while its receiver has more than `highWater`
  // bytes waiting, and a receiver that falls `maximumPending` bytes behind
  // loses the conversation. For tests.
  void setRelayLimits(qint64 highWater, qint64 maximumPending);

private:
  struct Client {
    QString id;
    QString nickname;
    QString address;
    quint16 port = 0;
    QByteArray buffer;
    bool registered = false;
    // A failed sign-in waits briefly for its rejection; ignore it until then.
    bool rejected = false;
    QByteArray nonce;
    QSet<QString> channels;
    bool web = false;
    // CENTRAL_STATUS away: the user has not touched their client for a while.
    bool away = false;
    QElapsedTimer lastActivity;
    // Not read while this receiver catches up with what it sent.
    CentralLink *waitingFor = nullptr;
  };
  struct Channel {
    CentralLink *opener;
    CentralLink *target;
    // Transfers over the size limit, whose lines are not relayed.
    QSet<QString> refusedFiles;
  };

  void acceptClients();
  void readClient(CentralLink *socket);
  void processLine(CentralLink *socket, const QString &line);
  void registerClient(CentralLink *socket, const QStringList &parts);
  void openChannel(CentralLink *socket, const QString &channelId,
                   const QString &targetId);
  void relay(CentralLink *socket, const QString &channelId,
             const QByteArray &line);
  void closeChannel(const QString &channelId, CentralLink *closedBy,
                    const QString &reason);
  void dropClient(CentralLink *socket, const QString &why);
  void sweepIdleClients();
  void send(CentralLink *socket, const QByteArray &line);
  void broadcast(const QByteArray &line, CentralLink *except);
  QByteArray peerLine(const Client &client) const;
  QByteArray kindLine(const Client &client) const;
  QByteArray statusLine(const Client &client) const;
  CentralLink *findClient(const QString &id) const;
  QString describe(const Client &client) const;
  void log(const QString &message) const;

  QTcpServer server_;
  QHash<CentralLink *, Client> clients_;
  QHash<QString, Channel> channels_;
  QTimer sweepTimer_;
  int idleTimeoutMs_;
  QString password_;
  std::function<void(const QString &)> logger_;
  TranslationService *translation_ = nullptr;
  qint64 maximumFileSize_;
  qint64 relayHighWater_;
  qint64 maximumPending_;
  void receiverDrained(CentralLink *receiver);
};
