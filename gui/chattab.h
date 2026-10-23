#pragma once

#include <QWidget>

#include "ollamaclient.h"

class ChatSession;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
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
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void appendMessage(const QString &sender, const QString &message,
                     const QString &hoverText = {},
                     const QString &hoverLabel = QStringLiteral("Original"));
  void appendFile(const QString &sender, const QString &name,
                  const QString &path, const QString &mimeType);

  ChatSession *session_;
  QLabel *status_;
  QTextEdit *log_;
  QLineEdit *input_;
  QPushButton *translateButton_;
  OllamaClient outgoingTranslator_;
  OllamaClient incomingTranslator_;
  bool liveTranslate_ = false;
  bool outgoingPending_ = false;
  QString pendingOriginal_;
  QString myLanguage_;
  QString peerLanguage_;
};
