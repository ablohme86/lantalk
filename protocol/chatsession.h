#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>

class QFile;
class QTcpSocket;

class ChatSession final : public QObject {
  Q_OBJECT

public:
  explicit ChatSession(QTcpSocket *socket, QObject *parent = nullptr);
  ~ChatSession() override;
  void beginHandshake(const QString &nickname);
  void reconnectToHost(const QString &address, quint16 port);
  void closeConnection();
  void sendMessage(const QString &message, const QString &originalMessage = {});
  bool sendFile(const QString &filePath);
  void requestLiveTranslation(const QString &sourceLanguage,
                              const QString &targetLanguage);
  void stopLiveTranslation();
  QString peerName() const;
  QString peerAddress() const;
  quint16 peerPort() const;

signals:
  void peerIdentified(const QString &name);
  void messageReceived(const QString &message, const QString &originalMessage);
  void fileSent(const QString &name, const QString &path,
                const QString &mimeType);
  void fileReceived(const QString &name, const QString &path,
                    const QString &mimeType);
  void fileSendProgressChanged(int percent);
  void fileSendCompleted();
  void liveTranslationGranted();
  void liveTranslationDenied(const QString &message);
  void peerLiveTranslationChanged(bool active, const QString &sourceLanguage,
                                  const QString &targetLanguage);
  void statusChanged(const QString &status);
  void disconnected();

private slots:
  void readData();
  void sendNextFileChunk();

private:
  void processLine(const QString &line);
  void sendLine(const QString &line);
  void startNextFile();
  void cancelOutgoingFiles();
  void notifyDisconnected();
  void setupSocket();

  struct IncomingFile {
    QString name;
    QString mimeType;
    qint64 size;
    QByteArray data;
  };

  QTcpSocket *socket_;
  QByteArray buffer_;
  QString nickname_;
  QString peerName_;
  QString peerAddress_;
  bool handshakeComplete_ = false;
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
  bool outgoingFileBatchActive_ = false;
  bool disconnectNotified_ = false;
};
