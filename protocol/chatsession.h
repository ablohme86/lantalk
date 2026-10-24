#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

class ChatTransport;
class ConnectionRegistry;
class QFile;
class QTcpSocket;

class ChatSession final : public QObject {
  Q_OBJECT

public:
  // A connected socket makes an incoming session; pass an unconnected socket
  // and call connectToHost() for an outgoing one. Sessions register with
  // `registry`, or with ConnectionRegistry::shared() when it is null.
  explicit ChatSession(QTcpSocket *socket, QObject *parent = nullptr,
                       ConnectionRegistry *registry = nullptr);
  // The same for any transport, e.g. a channel relayed by a central host.
  explicit ChatSession(ChatTransport *transport, QObject *parent = nullptr,
                       ConnectionRegistry *registry = nullptr);
  ~ChatSession() override;
  void beginHandshake(const QString &nickname);
  // `peerNameHint` is the nickname expected at that address, if known.
  void connectToHost(const QString &address, quint16 port,
                     const QString &peerNameHint = {});
  // Replaces the current connection, if any, with a new one made by
  // ChatTransport::create().
  void reconnectToHost(const QString &address, quint16 port);
  void closeConnection();
  // Records the peer's listening endpoint for an incoming session so that a
  // later connection attempt to it is recognized as a duplicate.
  void setRemoteEndpoint(const QString &address, quint16 port);
  void sendMessage(const QString &message, const QString &originalMessage = {});
  bool sendFile(const QString &filePath);
  // LANTalk's own limit, or the central host's when it is lower.
  qint64 maximumFileSize() const;
  void requestLiveTranslation(const QString &sourceLanguage,
                              const QString &targetLanguage);
  void stopLiveTranslation();
  bool liveTranslationActiveOrPending() const;
  // Reports local draft edits; sends TYPING START/STOP to peers that support
  // typing notifications. Typing stops automatically after a period without
  // edits.
  void noteLocalTyping(bool hasDraft);
  void stopLocalTyping();
  bool peerTyping() const;
  // Whether this user is away, for every session (PRESENCE AWAY/ACTIVE to
  // peers that advertise CAPABILITIES PRESENCE).
  static void setLocalAway(bool away);
  static bool localAway();
  bool peerAway() const;
  bool peerSupportsTyping() const;
  void setTypingTimeouts(int inactivityMs, int refreshMs, int expiryMs);
  QString peerName() const;
  bool peerIsWebClient() const;
  // The peer nickname from the most recent handshake; unlike peerName(), it
  // survives disconnects and reconnect attempts.
  QString identityName() const;
  QString peerAddress() const;
  quint16 peerPort() const;
  QString remoteHost() const;
  quint16 remotePort() const;
  bool isConnected() const;
  bool isHandshakeComplete() const;
  // Closed by its owner or merged into another session for the same peer.
  bool isRetired() const;

signals:
  void peerIdentified(const QString &name);
  void messageReceived(const QString &message, const QString &originalMessage);
  // `id` names the transfer in fileTransferProgress().
  void fileSent(const QString &name, const QString &path,
                const QString &mimeType, const QString &id);
  void fileReceived(const QString &name, const QString &path,
                    const QString &mimeType, const QString &id);
  // A transfer started (0) or moved on, in whole percents; fileSent() or
  // fileReceived() ends it, or fileTransferFailed().
  void fileTransferProgress(const QString &id, const QString &name,
                            bool outgoing, int percent);
  void fileTransferFailed(const QString &id, bool outgoing,
                          const QString &reason);
  void liveTranslationGranted();
  void liveTranslationDenied(const QString &message);
  void peerLiveTranslationChanged(bool active, const QString &sourceLanguage,
                                  const QString &targetLanguage);
  void peerTypingChanged(bool typing);
  void peerAwayChanged(bool away);
  void statusChanged(const QString &status);
  void disconnected();
  // This session duplicated `existing` and has been retired: its connection
  // was either handed over to `existing` or closed. Owners should discard it
  // and show `existing` instead.
  void mergedInto(ChatSession *existing);

private slots:
  void readData();
  void sendNextFileChunk();

private:
  friend class ConnectionRegistry;

  void processLine(const QString &line);
  QString fileTooLargeMessage() const;
  void sendLine(const QString &line);
  void startNextFile();
  void cancelOutgoingFiles();
  void notifyDisconnected();
  void setupSocket();
  void resetConnectionState();
  void setPeerTyping(bool typing);
  void setPeerAway(bool away);
  void sendPresence();
  void resetTypingState();
  QString connectionKey() const;
  qint64 handshakeAgeMs() const;
  void adoptConnection(ChatSession *duplicate);
  void retireAsDuplicate(ChatSession *existing);

  // Written straight to its file in the cache folder, so a large file
  // never sits in memory.
  struct IncomingFile {
    QString name;
    QString mimeType;
    qint64 size;
    std::shared_ptr<QFile> file;
    qint64 received = 0;
    int percent = 0;
  };
  void discardIncomingFile(const QString &id, const QString &reason);

  ConnectionRegistry *registry_;
  ChatTransport *transport_;
  QByteArray buffer_;
  QString nickname_;
  QString peerName_;
  QString identityName_;
  QString peerAddress_;
  QString remoteHost_;
  quint16 remotePort_ = 0;
  QString peerNameHint_;
  bool outgoing_ = false;
  bool closed_ = false;
  bool merged_ = false;
  bool handshakeComplete_ = false;
  QElapsedTimer handshakeClock_;
  bool translationActive_ = false;
  bool peerTranslationActive_ = false;
  QString translationRequestId_;
  QHash<QString, IncomingFile> incomingFiles_;
  QStringList receivedFilePaths_;
  QQueue<QString> outgoingFilePaths_;
  QFile *outgoingFile_ = nullptr;
  QTimer outgoingFilePump_;
  QString outgoingFileId_;
  QString outgoingFileName_;
  QString outgoingFilePath_;
  QString outgoingMimeType_;
  qint64 outgoingFileSize_ = 0;
  qint64 outgoingBytesSent_ = 0;
  int outgoingFilePercent_ = -1;
  bool disconnectNotified_ = false;
  bool peerSupportsTyping_ = false;
  // The peer takes files up to MaximumFileSize, not just
  // LegacyMaximumFileSize (CAPABILITIES LARGEFILES).
  bool peerSupportsLargeFiles_ = false;
  bool peerSupportsPresence_ = false;
  bool peerAway_ = false;
  bool localTyping_ = false;
  bool peerTyping_ = false;
  QTimer localTypingIdleTimer_;
  QTimer localTypingRefreshTimer_;
  QTimer peerTypingExpiryTimer_;
};
