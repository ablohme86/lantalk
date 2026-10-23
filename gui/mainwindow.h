#pragma once

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QMainWindow>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

class QComboBox;
class QTabWidget;
class QTcpSocket;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  MainWindow();

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
  void announcePresence();
  void readDiscoveryDatagrams();
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
  const QString discoveryId_ =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  QHash<QString, DiscoveredHost> discoveredHosts_;
  QComboBox *host_;
  QTabWidget *tabs_;
};
