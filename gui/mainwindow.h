#pragma once

#include "gui/appicon.h"
#include "ollamadiscovery.h"
#include "protocol/centralclient.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QMainWindow>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class ChatSession;
class ChatTab;
class QEvent;
class QSoundEffect;
class QSystemTrayIcon;
class QTabWidget;
class QTcpSocket;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  MainWindow();
  ~MainWindow() override;
  void activateFromAnotherLaunch();

protected:
  bool event(QEvent *event) override;
  // Any input to LANTalk counts as activity (see AwayAfterMs).
  bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
  void showSettings();
  void showFontSizeDialog();
  void connectToHostItem(QListWidgetItem *item);
  // The sidebar's text while it lists no hosts.
  void setHostListStatus(const QString &text);
  void selectHostForTab(ChatTab *tab);
  // The green or red dot on a conversation tab.
  void updateTabStatus(ChatTab *tab);
  Presence presenceOf(ChatTab *tab) const;
  // Whether the central host reports this host away.
  bool centralPeerAway(const QString &address, const QString &nickname) const;
  bool lanPeerAway(const QString &address, const QString &nickname) const;
  void setAway(bool away);
  void showConnectDialog();
  void acceptConnection();

private:
  void applyDarkMenuStyle();
  void promptForNickname();
  void connectToAddress(const QString &address, quint16 port,
                        const QString &nickname = {});
  void startListening();
  // Listens and discovers hosts on the LAN, or, with a central host
  // configured, leaves both to the central host.
  void applyNetworkMode();
  void updateCentralTranslation();
  // Reconnects dropped conversations with peers the central host now lists.
  void reconnectArrivedPeers();
  void startDiscovery();
  void stopDiscovery();
  void acceptRelayConnection(RelayTransport *transport);
  void adoptIncomingSession(ChatSession *session, quint16 port);
  void updateCentralStatus();
  // Offers a personal PIN once a central host lets this nickname have one.
  void updateAccountState();
  void askForPin(const QString &reason);
  void showPinDialog(int mode, const QString &message = {});
  void showSyncedEntry(const QString &address, const QString &nickname,
                       const ChatHistory::Entry &entry);
  QString pinOfferKey() const;
  void setupTrayIcon();
  void updateTrayIcon();
  void showFromTray();
  void clearUnreadMessages();
  bool inForeground() const;
  void noteIncomingMessage();
  void playMessageSound(ChatTab *tab);
  void showNotification(ChatTab *tab, const QString &sender,
                        const QString &preview);
  void setTabUnread(ChatTab *tab, bool unread);
  void announcePresence();
  void readDiscoveryDatagrams();
  void scanPrivateSubnets();
  void sendDiscoveryProbeBatch();
  void refreshHostList();
  ChatTab *createTab(ChatSession *session, const QString &reconnectAddress,
                     quint16 reconnectPort);
  void showTab(ChatTab *tab, const QString &title);
  ChatTab *tabForSession(ChatSession *session) const;
  void restoreOpenConnections();
  void persistOpenConnections();
  quint16 discoveredPortForAddress(const QHostAddress &address) const;

  QTcpServer server_;
  CentralClient central_;
  // What the current connections were made for: empty on the LAN, else the
  // central host and the identity registered with it.
  QString networkModeKey_;
  bool networkModeApplied_ = false;
  bool discoverySetUp_ = false;
  struct DiscoveredHost {
    QString nickname;
    QHostAddress address;
    quint16 port;
    QDateTime lastSeen;
  };
  QUdpSocket discoverySocket_;
  QTimer discoveryTimer_;
  QTimer subnetScanTimer_;
  QTimer probeSendTimer_;
  QTimer discoverySettleTimer_;
  QQueue<QHostAddress> pendingDiscoveryProbes_;
  bool initialDiscoveryScanComplete_ = false;
  int discoveredHostExpiryMs_ = 7000;
  const QString discoveryId_ =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  QHash<QString, DiscoveredHost> discoveredHosts_;
  // The sidebar of available hosts, as in the TUI and the web client.
  QSet<QString> centralPeersSeen_;
  QAction *pinAction_ = nullptr;
  bool pinDialogOpen_ = false;
  bool pinOffered_ = false;
  bool pinRequested_ = false;
  QElapsedTimer lastActivity_;
  QTimer awayTimer_;
  bool away_ = false;
  QSplitter *splitter_;
  QListWidget *hosts_;
  QLabel *hostListStatus_;
  QTabWidget *tabs_;
  QSystemTrayIcon *trayIcon_ = nullptr;
  OllamaDiscovery ollamaDiscovery_;
  QTimer ollamaScanTimer_;
  int unreadMessageCount_ = 0;
  // The conversation the last notification was about; clicking it opens it.
  QPointer<ChatTab> notifiedTab_;
  QSoundEffect *messageSound_ = nullptr;
  QElapsedTimer lastMessageSound_;
};
