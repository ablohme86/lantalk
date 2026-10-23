#pragma once

#include "ollamadiscovery.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QMainWindow>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

class QComboBox;
class ChatSession;
class ChatTab;
class QEvent;
class QSystemTrayIcon;
class QTabWidget;
class QTcpSocket;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  MainWindow();
  void activateFromAnotherLaunch();

protected:
  bool event(QEvent *event) override;

private slots:
  void showSettings();
  void showFontSizeDialog();
  void connectToHost();
  void showConnectDialog();
  void acceptConnection();

private:
  void applyDarkMenuStyle();
  void promptForNickname();
  void connectToAddress(const QString &address, quint16 port,
                        const QString &nickname = {});
  void startListening();
  void startDiscovery();
  void setupTrayIcon();
  void updateTrayIcon();
  void showFromTray();
  void clearUnreadMessages();
  bool inForeground() const;
  void noteIncomingMessage();
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
  QComboBox *host_;
  QTabWidget *tabs_;
  QSystemTrayIcon *trayIcon_ = nullptr;
  OllamaDiscovery ollamaDiscovery_;
  QTimer ollamaScanTimer_;
  int unreadMessageCount_ = 0;
  // The conversation the last notification was about; clicking it opens it.
  QPointer<ChatTab> notifiedTab_;
};
