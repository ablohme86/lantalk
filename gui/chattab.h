#pragma once

#include <QDateTime>
#include <QList>
#include <QWidget>

#include "messagetranslator.h"

class ChatSession;
class QLabel;
class ChatInput;
class OllamaDiscovery;
class QCheckBox;
class QPlainTextEdit;
class QVBoxLayout;
class QPushButton;
class QStackedWidget;
class ChatLog;
class ChatImagePreviewWidget;
class QDragEnterEvent;
class QDropEvent;
class QTimer;
class TranslationRestorer;

class ChatTab final : public QWidget {
  Q_OBJECT

public:
  explicit ChatTab(ChatSession *session, QWidget *parent = nullptr);
  QString title() const;
  ChatSession *session() const;
  QString reconnectAddress() const;
  quint16 reconnectPort() const;
  void setReconnectEndpoint(const QString &address, quint16 port);
  void refreshTranslationAvailability();
  // Only one side translates at a time; the other side's button waits.
  void setPeerTranslating(bool active);
  // Ollama servers found on the network also make Live Translate available
  // and can be picked in its dialog.
  void setOllamaDiscovery(OllamaDiscovery *discovery);
  // The peer's nickname from the latest handshake, if there was one.
  QString peerNickname() const;
  // Retries a disconnected conversation now instead of waiting for the timer.
  void reconnectNow();
  // Drops the current connection, if any, and connects again, e.g. after the
  // network mode (LAN or central host) changed.
  void restartConnection();
  void closeConversation();

signals:
  void titleChanged();
  void incomingMessageReceived();
  // What a desktop notification for an incoming message or file should
  // show. With Live Translate on, it follows the translation.
  void notifyIncoming(const QString &sender, const QString &preview);

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
    int messageId;
    bool translated = false;
    bool pending = false;
    // A notification waits for this message's translation.
    bool notify = false;
  };

  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  int appendMessage(
      const QString &sender, const QString &message,
      const QString &hoverText = {},
      const QString &hoverLabel = QStringLiteral("Original"),
      bool historical = false, const QDateTime &timestamp = {});
  void loadConversationHistory();
  void appendHistorySeparator();
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
  void updateTypingIndicator();
  void updateLocalTyping();
  void addQuote(const QString &text);
  void removeQuote(QWidget *card);
  void clearQuotes();
  void updateQuoteTranslation();

  ChatSession *session_;
  QLabel *status_;
  QLabel *typingIndicator_;
  QTimer *typingAnimation_;
  int typingAnimationFrame_ = 0;
  QStackedWidget *viewStack_;
  ChatLog *log_;
  ChatImagePreviewWidget *imageViewer_;
  ChatInput *input_;
  QPushButton *translateButton_;
  QTimer *reconnectTimer_;
  QString reconnectAddress_;
  QString lastPeerName_;
  quint16 reconnectPort_ = 0;
  bool connected_ = false;
  bool hasPeerIdentity_ = false;
  bool historyLoaded_ = false;
  struct PendingQuote {
    QWidget *card;
    QCheckBox *translate;
    QPlainTextEdit *editor;
  };

  QWidget *quoteCards_;
  QVBoxLayout *quoteCardsLayout_;
  QList<PendingQuote> pendingQuotes_;
  MessageTranslator outgoingTranslator_;
  MessageTranslator incomingTranslator_;
  TranslationRestorer *translationRestorer_;
  OllamaDiscovery *ollamaDiscovery_ = nullptr;
  bool liveTranslate_ = false;
  bool automaticTranslationRequest_ = false;
  bool outgoingPending_ = false;
  bool fileSendUiActive_ = false;
  QString fileSendPreviousInput_;
  QString pendingOriginal_;
  QList<IncomingMessage> incomingMessages_;
  QString myLanguage_;
  QString peerLanguage_;
};
