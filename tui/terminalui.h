#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <termios.h>
#endif

class ChatSession;
class QTcpSocket;

class TerminalUi final : public QObject {
  Q_OBJECT

public:
  explicit TerminalUi(QObject *parent = nullptr);
  ~TerminalUi() override;

private:
  struct DiscoveredHost {
    QString nickname;
    QHostAddress address;
    quint16 port;
    QDateTime lastSeen;
  };

  struct Conversation {
    ChatSession *session;
    QString title;
    QStringList transcript;
  };

  enum class Focus { Hosts, Chat, Menu };
  enum class Menu { None, Chat, Settings };
  enum class Dialog { None, Connect, Nickname, OllamaUrl, OllamaModel, ListenPort, CloseChat, Quit };

  void configureNickname();
  void configureTerminal();
  void restoreTerminal();
  void startListening();
  void startDiscovery();
  void announcePresence();
  void readDiscoveryDatagrams();
  void handleInput();
  void flushEscape();
  void handleKey(const QByteArray &key);
  void handleCommand(const QString &command);
  void activateMenuItem();
  void openDialog(Dialog dialog, const QString &title, const QString &prompt,
                  const QString &value = {});
  void acceptDialog();
  void closeDialog();
  void setListenPort(quint16 port);
  void connectToAddress(const QString &address, quint16 port);
  void acceptConnection();
  void addSession(QTcpSocket *socket, const QString &title);
  void closeConversation(int index);
  QList<DiscoveredHost> sortedHosts() const;
  void appendTranscript(int index, const QString &line);
  void redraw();
  void redrawIfResized();
  QString sanitize(const QString &text) const;
  QString elide(const QString &text, int width) const;
  void writeLine(const QString &text, int width) const;
  QString boxedLine(const QString &text, int width) const;
  void drawDialog() const;
  void print(const QString &text) const;
  void printPrompt();
  void printHosts() const;
  void printConversations() const;
  QString conversationTitle(const Conversation &conversation) const;

  QTcpServer server_;
  QUdpSocket discoverySocket_;
  QTimer discoveryTimer_;
  const QString discoveryId_ =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  QHash<QString, DiscoveredHost> discoveredHosts_;
  QList<Conversation> conversations_;
  int currentConversation_ = -1;
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
