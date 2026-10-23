#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpSocket;

class ChatSession final : public QObject {
  Q_OBJECT

public:
  explicit ChatSession(QTcpSocket *socket, QObject *parent = nullptr);
  void beginHandshake(const QString &nickname);
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
  void liveTranslationGranted();
  void liveTranslationDenied(const QString &message);
  void peerLiveTranslationChanged(bool active, const QString &sourceLanguage,
                                  const QString &targetLanguage);
  void statusChanged(const QString &status);
  void disconnected();

private slots:
  void readData();

private:
  void processLine(const QString &line);
  void sendLine(const QString &line);

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
  bool handshakeComplete_ = false;
  bool translationActive_ = false;
  bool peerTranslationActive_ = false;
  QString translationRequestId_;
  QHash<QString, IncomingFile> incomingFiles_;
};
