#pragma once

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QMainWindow>
#include <QQueue>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

class QComboBox;
class QSystemTrayIcon;
class QTabWidget;
class QTcpSocket;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  MainWindow();
  void activateFromAnotherLaunch();

private slots:
  void showSettings();
  void connectToHost();
  void showConnectDialog();
  void acceptConnection();

private:
  void applyDarkMenuStyle();
  void promptForNickname();
  void connectToAddress(const QString &address, quint16 port);
  void startListening();
  void startDiscovery();
  void setupTrayIcon();
  void updateTrayIcon();
  void showFromTray();
  void noteIncomingMessage();
  void announcePresence();
  void readDiscoveryDatagrams();
  void scanPrivateSubnets();
  void sendDiscoveryProbeBatch();
  void refreshHostList();
  void addSession(QTcpSocket *socket, const QString &initialTitle);

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
  QQueue<QHostAddress> pendingDiscoveryProbes_;
  int discoveredHostExpiryMs_ = 7000;
  const QString discoveryId_ =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  QHash<QString, DiscoveredHost> discoveredHosts_;
  QComboBox *host_;
  QTabWidget *tabs_;
  QSystemTrayIcon *trayIcon_ = nullptr;
  int unreadMessageCount_ = 0;
};
