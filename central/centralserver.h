#pragma once

#include "central/accountstore.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QTimer>

#include <functional>

class CentralLink;
class TranslationService;

// lantalkd: keeps the list of connected LANTalk clients
// and relays each conversation between them, line by line (see
// protocol/centralprotocol.h). A nickname protected with a PIN may be signed
// in on several devices: they appear as one peer, every conversation reaches
// all of them, and lantalkd keeps the account's history.
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
  // Conversations going on.
  int channelCount() const;
  // Clients must prove they know `password` to sign in; empty allows all.
  void setPassword(const QString &password);
  // Keeps accounts (nicknames protected with a PIN) and their history in
  // `directory`. Without one, nobody can protect a nickname.
  bool setDataDirectory(const QString &directory, QString *error = nullptr);
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
  // A sender stops being read while one of its receivers has more than
  // `highWater` bytes waiting, and a receiver that falls `maximumPending`
  // bytes behind leaves the conversation. For tests.
  void setRelayLimits(qint64 highWater, qint64 maximumPending);

private:
  struct Client {
    QString nickname;
    QString address;
    quint16 port = 0;
    QByteArray buffer;
    bool registered = false;
    // A failed sign-in waits briefly for its rejection; ignore it until then.
    bool rejected = false;
    QByteArray nonce;
    int version = 1;
    // Chosen by the device and kept across sign-ins (version 2).
    QString deviceId;
    // The peer this client is, alone or as one device of an account.
    QString partyId;
    // Signed in with the account's PIN.
    QString accountName;
    // An account device gets no conversations until it caught up with the
    // history (CENTRAL_SYNC).
    bool synced = true;
    QSet<QString> channels;
    bool web = false;
    // CENTRAL_STATUS away: the user has not touched their client for a while.
    bool away = false;
    QElapsedTimer lastActivity;
    // Not read while this receiver catches up with what it sent.
    CentralLink *waitingFor = nullptr;
  };
  // What the other clients see as one peer.
  struct Party {
    QString id;
    QString nickname;
    // Of the device that signed in first.
    QString address;
    quint16 port = 0;
    QString accountName;
    QList<CentralLink *> devices;
    // As last announced: every device is away, every device is a browser.
    bool away = false;
    bool web = false;
  };
  struct Conversation {
    QString id;
    QString firstParty;
    QString secondParty;
    // Every device in it, with its own channel id.
    QHash<CentralLink *, QString> channels;
    // NICKNAME and CAPABILITIES of each party, sent on once and replayed to
    // devices that join later.
    QHash<QString, QHash<QByteArray, QByteArray>> handshake;
    // Live Translate: the party that translates, once requested.
    QString translator;
    QString translateId;
    QByteArray translateRequest;
    bool translateGranted = false;
    // Requests from the translator's other devices, answered with the first.
    QList<QPair<CentralLink *, QString>> waitingRequests;
    // Transfers over the size limit, whose lines are not relayed.
    QSet<QString> refusedFiles;
  };

  void acceptClients();
  void readClient(CentralLink *socket);
  void processLine(CentralLink *socket, const QString &line);
  void registerClient(CentralLink *socket, const QStringList &parts);
  void reject(CentralLink *socket, const QString &reason, bool delayed,
              const QByteArray &before = {});
  void setPin(CentralLink *socket, const QString &verifier);
  void synchronize(CentralLink *socket, const QString &after);
  void openChannel(CentralLink *socket, const QString &channelId,
                   const QString &targetId);
  void joinConversation(const QString &conversationId, CentralLink *device);
  void replayConversation(const Conversation &conversation,
                          CentralLink *device);
  void relay(CentralLink *socket, const QString &channelId,
             const QByteArray &line);
  void storeMessage(const Conversation &conversation, const QString &fromParty,
                    const QByteArray &line);
  void leaveConversation(CentralLink *device, const QString &conversationId,
                         const QString &reason, bool notifyDevice);
  void endConversation(const QString &conversationId, const QString &reason);
  void dropClient(CentralLink *socket, const QString &why);
  void setDeviceAway(CentralLink *socket, bool away);
  void updateParty(const QString &partyId);
  void sweepIdleClients();
  void send(CentralLink *socket, const QByteArray &line);
  void broadcast(const QByteArray &line, CentralLink *except);
  // To everyone but the party's own devices.
  void broadcastAbout(const Party &party, const QByteArray &line);
  void announceParty(const Party &party, CentralLink *to);
  QByteArray peerLine(const Party &party) const;
  QByteArray kindLine(const Party &party) const;
  QByteArray statusLine(const Party &party) const;
  QByteArray accountLine(const Party &party) const;
  QString otherParty(const Conversation &conversation,
                     const QString &partyId) const;
  QList<CentralLink *> devicesIn(const Conversation &conversation,
                                 const QString &partyId) const;
  bool supportsPresence(const Conversation &conversation,
                        const QString &partyId) const;
  QString conversationBetween(const QString &first,
                              const QString &second) const;
  QString describe(const Client &client) const;
  void log(const QString &message) const;
  void receiverDrained(CentralLink *receiver);

  QTcpServer server_;
  QHash<CentralLink *, Client> clients_;
  QHash<QString, Party> parties_;
  QHash<QString, Conversation> conversations_;
  // Channel id to conversation id.
  QHash<QString, QString> channelConversations_;
  AccountStore accounts_;
  QTimer sweepTimer_;
  int idleTimeoutMs_;
  QString password_;
  std::function<void(const QString &)> logger_;
  TranslationService *translation_ = nullptr;
  qint64 maximumFileSize_;
  qint64 relayHighWater_;
  qint64 maximumPending_;
};
