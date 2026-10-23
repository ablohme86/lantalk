#pragma once

#include "ollamaclient.h"
#include "protocol/centralclient.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QQueue>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <termios.h>
#endif

class ChatSession;
class QTcpSocket;
class TranslationRestorer;

class TerminalUi final : public QObject {
  Q_OBJECT

public:
  explicit TerminalUi(QObject *parent = nullptr);
  ~TerminalUi() override;
  void activateFromAnotherLaunch();

private:
  struct DiscoveredHost {
    QString nickname;
    QHostAddress address;
    quint16 port;
    QDateTime lastSeen;
  };

  struct Conversation {
    struct TranscriptLine {
      QString text;
      bool historical = false;
      bool separator = false;
    };

    ChatSession *session;
    TranslationRestorer *translation = nullptr;
    QString title;
    QList<TranscriptLine> transcript;
    bool liveTranslate = false;
    bool historyLoaded = false;
    bool disconnected = false;
    bool unread = false;
    QString myLanguage;
    QString peerLanguage;
  };

  enum class Focus { Hosts, Chat, Conversations, Menu };
  enum class Menu { None, Chat, Settings };
  enum class Dialog { None, Connect, Nickname, OllamaUrl, OllamaModel, ListenPort, CentralHost, CentralPassword, CloseChat, Quit };

  void configureNickname();
  void configureTerminal();
  void restoreTerminal();
  void startListening();
  // Listens and discovers hosts on the LAN, or, with a central host
  // configured, leaves both to the central host.
  void applyNetworkMode();
  void startDiscovery();
  void stopDiscovery();
  void reportCentralState();
  void announcePresence();
  void readDiscoveryDatagrams();
  void scanPrivateSubnets();
  void sendDiscoveryProbeBatch();
  void handleInput();
  void flushEscape();
  void handleKey(const QByteArray &key);
  void handleCommand(const QString &command);
  void openTopMenu(Menu menu);
  void activateMenuItem();
  void openDialog(Dialog dialog, const QString &title, const QString &prompt,
                  const QString &value = {});
  void acceptDialog();
  void closeDialog();
  void setListenPort(quint16 port);
  void connectToAddress(const QString &address, quint16 port, const QString &nickname = {});
  void acceptConnection();
  void acceptRelayConnection(RelayTransport *transport);
  void adoptIncomingSession(ChatSession *session, quint16 port);
  void addSession(ChatSession *session, const QString &title);
  void conversationIdentified(ChatSession *session);
  void mergeConversation(ChatSession *duplicate, ChatSession *existing);
  void syncLocalTyping();
  void handlePaste(QString text);
  QString composeDraft(const QString &text) const;
  void closeConversation(int index);
  void sendChatMessage(const QString &text);
  void setStatus(const QString &message);
  void updateTranslationStatus(const QString &idleMessage);
  bool currentConversationDisconnected() const;
  int conversationIndex(ChatSession *session) const;
  QList<DiscoveredHost> sortedHosts() const;
  void appendTranscript(int index, const QString &line);
  void loadConversationHistory(int index);
  void redraw();
  void redrawIfResized();
  QString sanitize(const QString &text) const;
  QString elide(const QString &text, int width) const;
  void writeLine(const QString &text, int width) const;
  QString boxedLine(const QString &text, int width) const;
  void drawDialog() const;
  void drawMenu(int column) const;
  void print(const QString &text) const;
  void printPrompt();
  void printHosts() const;
  void printConversations() const;
  QString conversationTitle(const Conversation &conversation) const;

  QTcpServer server_;
  CentralClient central_;
  // What the current connections were made for: empty on the LAN, else the
  // central host and the identity registered with it.
  QString networkModeKey_;
  bool networkModeApplied_ = false;
  bool discoverySetUp_ = false;
  QUdpSocket discoverySocket_;
  QTimer discoveryTimer_;
  QTimer subnetScanTimer_;
  QTimer probeSendTimer_;
  QQueue<QHostAddress> pendingDiscoveryProbes_;
  int discoveredHostExpiryMs_ = 7000;
  const QString discoveryId_ =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  QHash<QString, DiscoveredHost> discoveredHosts_;
  QList<Conversation> conversations_;
  int currentConversation_ = -1;
  int selectedConversation_ = -1;
  int selectedHost_ = 0;
  int selectedMenu_ = 0;
  int selectedAction_ = 0;
  Focus focus_ = Focus::Hosts;
  Menu openMenu_ = Menu::None;
  Dialog dialog_ = Dialog::None;
  QString input_;
  QString inputBytes_;
  QString dialogTitle_;
  QString dialogPrompt_;
  QString dialogInput_;
  QString status_;
  // Pasted blocks waiting to be sent with the next message.
  struct PendingQuote {
    QString text;
    bool translate = false;
  };
  QList<PendingQuote> pendingQuotes_;
  QPointer<ChatSession> typingSession_;
  QString typingDraft_;
  int pendingOutgoingTranslations_ = 0;
  int pendingIncomingTranslations_ = 0;
  bool ansi_ = false;
  bool rawMode_ = false;
  int terminalRows_ = 0;
  int terminalColumns_ = 0;
  QTimer resizeTimer_;
  QTimer escapeTimer_;
#ifdef Q_OS_UNIX
  termios terminalMode_ {};
#endif
};
