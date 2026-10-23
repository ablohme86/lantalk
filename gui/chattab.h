#pragma once

#include <QList>
#include <QTextCursor>
#include <QWidget>

#include "ollamaclient.h"

class ChatSession;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class ChatImagePreviewWidget;
class QDragEnterEvent;
class QDropEvent;
class QTimer;

class ChatTab final : public QWidget {
  Q_OBJECT

public:
  explicit ChatTab(ChatSession *session, QWidget *parent = nullptr);
  QString title() const;
  QString reconnectAddress() const;
  quint16 reconnectPort() const;
  void setReconnectEndpoint(const QString &address, quint16 port);
  void refreshTranslationAvailability();

signals:
  void titleChanged();
  void incomingMessageReceived();

private slots:
  void configureTranslation();
  void send();
  void received(const QString &message, const QString &originalMessage);
  void outgoingTranslationFinished(const QString &translation,
                                   const QString &originalMessage);
  void incomingTranslationFinished(const QString &translation,
                                   const QString &originalMessage);
  void sendText(const QString &text);

private:
  struct IncomingMessage {
    QString originalMessage;
    QTextCursor messageCursor;
    bool translated = false;
    bool pending = false;
  };

  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  QTextCursor appendMessage(
      const QString &sender, const QString &message,
      const QString &hoverText = {},
      const QString &hoverLabel = QStringLiteral("Original"));
  void translatePendingIncomingMessages();
  void translateIncomingMessage(int index);
  void setIncomingMessageText(IncomingMessage &incoming,
                             const QString &text, bool translated,
                             bool pending);
  void replaceTranslationStatus(const QString &originalMessage,
                                const QString &replacement, bool succeeded);
  void clearOutgoingTranslationStatus();
  void saveAttachmentAs(const QString &path, const QString &suggestedName);
  void appendFile(const QString &sender, const QString &name,
                  const QString &path, const QString &mimeType,
                  bool received);
  void appendSystemMessage(const QString &message);
  void retryConnection();

  ChatSession *session_;
  QLabel *status_;
  QStackedWidget *viewStack_;
  QTextBrowser *log_;
  ChatImagePreviewWidget *imageViewer_;
  QLineEdit *input_;
  QPushButton *translateButton_;
  QTimer *reconnectTimer_;
  QString reconnectAddress_;
  quint16 reconnectPort_ = 0;
  bool connected_ = false;
  OllamaClient outgoingTranslator_;
  OllamaClient incomingTranslator_;
  bool liveTranslate_ = false;
  bool outgoingPending_ = false;
  bool fileSendUiActive_ = false;
  QString fileSendPreviousInput_;
  QString pendingOriginal_;
  QList<IncomingMessage> incomingMessages_;
  QString myLanguage_;
  QString peerLanguage_;
};
