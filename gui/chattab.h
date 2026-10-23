#pragma once

#include <QList>
#include <QTextCursor>
#include <QWidget>

#include "ollamaclient.h"

class ChatSession;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QDragEnterEvent;
class QDropEvent;

class ChatTab final : public QWidget {
  Q_OBJECT

public:
  explicit ChatTab(ChatSession *session, QWidget *parent = nullptr);
  QString title() const;
  void refreshTranslationAvailability();

signals:
  void titleChanged();

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
  struct PendingIncomingTranslation {
    QString originalMessage;
    QTextCursor statusCursor;
  };

  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void appendMessage(const QString &sender, const QString &message,
                     const QString &hoverText = {},
                     const QString &hoverLabel = QStringLiteral("Original"));
  QTextCursor appendTranslationStatus(const QString &sender,
                                      const QString &message);
  void replaceTranslationStatus(const QString &originalMessage,
                                const QString &replacement);
  void clearOutgoingTranslationStatus();
  void appendFile(const QString &sender, const QString &name,
                  const QString &path, const QString &mimeType);

  ChatSession *session_;
  QLabel *status_;
  QTextBrowser *log_;
  QLineEdit *input_;
  QPushButton *translateButton_;
  OllamaClient outgoingTranslator_;
  OllamaClient incomingTranslator_;
  bool liveTranslate_ = false;
  bool outgoingPending_ = false;
  QString pendingOriginal_;
  QList<PendingIncomingTranslation> pendingIncomingTranslations_;
  QString myLanguage_;
  QString peerLanguage_;
};
