#pragma once

#include "protocol/chathistory.h"
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
    // Signed in from lantalkd's web client (CENTRAL_PEER_KIND <id> web).
    bool web = false;
    // Away from their client (CENTRAL_PEER_STATUS <id> away).
    bool away = false;
    // A nickname protected with a PIN, possibly signed in on several devices
    // (CENTRAL_PEER_ACCOUNT <id> <devices>). Its address is then the central
    // host's, so the conversation stays the same whichever device answers;
    // deviceAddress is the one lantalkd listed.
    bool account = false;
    int devices = 1;
    QString deviceAddress;
  };

  // Whether this nickname is protected with a PIN on the central host.
  enum class AccountState {
    // Not signed in yet.
    Unknown,
    // The central host keeps no accounts, or is too old for them.
    Unavailable,
    // Anyone may use the nickname; setPin() protects it.
    Unprotected,
    // Signed in with the nickname's PIN.
    Protected,
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
  // What the central host relays (CENTRAL_FILE_LIMIT), or -1 if it did not
  // say.
  qint64 maximumFileSize() const;
  // The central host has an Ollama server that answers with its model
  // (CENTRAL_OLLAMA), so it can translate for Live Translate.
  bool canTranslate() const;
  // Tells the central host, and so everyone, that this user is away.
  void setAway(bool away);
  AccountState accountState() const;
  // Protects the nickname with `pin`, or changes its PIN; accountStateChanged()
  // or pinChanged() follows once the central host saved it.
  bool setPin(const QString &pin);
  // Signs in again with `pin` after pinRequired().
  void signInWithPin(const QString &pin);
  // A PIN is saved for this central host and nickname.
  bool hasSavedPin() const;
  // Identifies this installation to the central host, so it can tell this
  // client's devices apart; made up and saved on first use.
  QString deviceId() const;
  void setDeviceId(const QString &id);
  // Translates a message on the central host (CENTRAL_TRANSLATE); `done`
  // gets the translation, or the error with ok false, always later.
  void translate(const QString &message, const QString &sourceLanguage,
                 const QString &targetLanguage,
                 std::function<void(bool ok, const QString &result)> done);
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
  void translationAvailabilityChanged();
  void accountStateChanged();
  // The nickname is protected and the PIN is "missing", "wrong", or "locked"
  // after too many wrong ones. LANTalk stopped trying until signInWithPin().
  void pinRequired(const QString &reason);
  void pinChanged();
  // A message of a conversation this client did not take part in, e.g. one
  // another device of this account had, now in ChatHistory.
  void historyEntryAdded(const QString &address, const QString &nickname,
                         const ChatHistory::Entry &entry);
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
  void applyAccount(Peer &peer);
  void addHistoryEntry(const QByteArray &encoded);
  void noteSequence(qint64 sequence);
  QString accountAddress() const;
  QString pinKey() const;

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
  qint64 maximumFileSize_ = -1;
  bool canTranslate_ = false;
  bool away_ = false;
  struct PendingTranslation {
    std::function<void(bool, const QString &)> done;
    QTimer *timeout;
  };
  QHash<QString, PendingTranslation> translations_;
  void setCanTranslate(bool canTranslate);
  void finishTranslation(const QString &id, bool ok, const QString &result);
  QHash<QString, QPointer<RelayTransport>> channels_;
  QString deviceId_;
  AccountState accountState_ = AccountState::Unknown;
  QString accountId_;
  // The account's last history entry this client has.
  qint64 syncedThrough_ = 0;
  // Sent with CENTRAL_PIN_SET, saved once the central host accepts it.
  QByteArray pendingVerifier_;
  // Peers that are accounts, with their number of devices.
  QHash<QString, int> accountPeers_;
  // CENTRAL_PIN_REQUIRED's reason, reported with the rejection after it.
  QString pinRequired_;
  // The central host predates PINs and only reads CENTRAL_HELLO 1.
  bool legacySignIn_ = false;
  bool reconnectSoon_ = false;
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
  bool peerIsWebClient() const override;
  bool peerHasAccount() const override;
  qint64 maximumFileSize() const override;

private:
  friend class CentralClient;

  void resolveTarget();
  void opened();
  void receivedLine(const QString &line);
  void receivedMirror(const QString &line);
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
