#include "gui/mainwindow.h"

#include "appconfig.h"
#include "gui/appicon.h"
#include "gui/chattab.h"
#include "gui/dialogs.h"
#include "protocol/chatsession.h"
#include "protocol/peerdiscovery.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTcpSocket>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

MainWindow::MainWindow() {
  setWindowTitle(QStringLiteral("LANChat"));
  setWindowIcon(createAppIcon());
  resize(900, 600);
  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  host_ = new QComboBox(central);
  host_->setPlaceholderText(QStringLiteral("No LANChat hosts found"));
  tabs_ = new QTabWidget(central);
  tabs_->setTabsClosable(true);
  layout->addWidget(host_);
  layout->addWidget(tabs_);
  setCentralWidget(central);

  auto *chatMenu = menuBar()->addMenu(QStringLiteral("Chat"));
  auto *connectToAction = chatMenu->addAction(QStringLiteral("Connect To…"));
  auto *settingsAction = menuBar()->addAction(QStringLiteral("Settings"));
  applyDarkMenuStyle();
  connect(connectToAction, &QAction::triggered, this,
          &MainWindow::showConnectDialog);
  connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);
  setupTrayIcon();
  connect(host_, QOverload<int>::of(&QComboBox::activated), this,
          [this](int index) {
            if (!host_->itemData(index).toMap().isEmpty()) {
              connectToHost();
            }
          });
  connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
    tabs_->widget(index)->deleteLater();
    tabs_->removeTab(index);
  });
  promptForNickname();
  startListening();
  connect(&server_, &QTcpServer::newConnection, this,
          &MainWindow::acceptConnection);
  startDiscovery();
}

void MainWindow::showSettings() {
  SettingsDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  dialog.save();
  startListening();
  announcePresence();
  for (int index = 0; index < tabs_->count(); ++index) {
    static_cast<ChatTab *>(tabs_->widget(index))
        ->refreshTranslationAvailability();
  }
}

void MainWindow::setupTrayIcon() {
  trayIcon_ = new QSystemTrayIcon(this);
  auto *menu = new QMenu(this);
  auto *openAction = menu->addAction(QStringLiteral("Open LANChat"));
  menu->addSeparator();
  auto *quitAction = menu->addAction(QStringLiteral("Quit"));
  trayIcon_->setContextMenu(menu);
  connect(openAction, &QAction::triggered, this, &MainWindow::showFromTray);
  connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
  connect(trayIcon_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              showFromTray();
            }
          });
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && unreadMessageCount_ > 0) {
              unreadMessageCount_ = 0;
              updateTrayIcon();
            }
          });
  updateTrayIcon();
  trayIcon_->show();
}

void MainWindow::updateTrayIcon() {
  if (!trayIcon_) return;
  trayIcon_->setIcon(createAppIcon(unreadMessageCount_));
  if (unreadMessageCount_ == 0) {
    trayIcon_->setToolTip(QStringLiteral("LANChat"));
    return;
  }
  trayIcon_->setToolTip(
      QStringLiteral("LANChat — %1 unread message%2")
          .arg(unreadMessageCount_)
          .arg(unreadMessageCount_ == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::showFromTray() {
  activateFromAnotherLaunch();
}

void MainWindow::activateFromAnotherLaunch() {
  if (isMinimized()) showNormal();
  else show();
  raise();
  activateWindow();
}

void MainWindow::noteIncomingMessage() {
  if (QGuiApplication::applicationState() == Qt::ApplicationActive &&
      isVisible() && !isMinimized()) {
    return;
  }
  ++unreadMessageCount_;
  updateTrayIcon();
}

void MainWindow::connectToHost() {
  const auto endpoint = host_->currentData().toMap();
  const auto address =
      endpoint.isEmpty() ? host_->currentText().trimmed()
                         : endpoint.value(QStringLiteral("address")).toString();
  if (address.isEmpty()) {
    return;
  }
  const auto peerPort = endpoint.isEmpty()
                            ? DefaultPort
                            : endpoint.value(QStringLiteral("port")).toUInt();
  connectToAddress(address, peerPort);
}

void MainWindow::showConnectDialog() {
  bool accepted = false;
  const auto address = QInputDialog::getText(this, QStringLiteral("Connect To"),
                                             QStringLiteral("Host IP or name:"),
                                             QLineEdit::Normal, {}, &accepted)
                           .trimmed();
  if (accepted && !address.isEmpty()) {
    connectToAddress(address, DefaultPort);
  }
}

void MainWindow::acceptConnection() {
  while (server_.hasPendingConnections()) {
    addSession(server_.nextPendingConnection(),
               QStringLiteral("Incoming connection"));
  }
}

void MainWindow::applyDarkMenuStyle() {
  if (palette().color(QPalette::Window).lightness() >= 128) {
    return;
  }
  menuBar()->setStyleSheet(QStringLiteral(
      "QMenuBar { background-color: #202426; color: #f4f4f4; }"
      "QMenuBar::item { background: transparent; color: #f4f4f4; padding: 5px "
      "9px; }"
      "QMenuBar::item:selected { background-color: #3d454c; }"
      "QMenu { background-color: #252a2e; color: #f4f4f4; border: 1px solid "
      "#555b60; }"
      "QMenu::item { color: #f4f4f4; padding: 5px 24px 5px 18px; }"
      "QMenu::item:selected { background-color: #3d6f98; }"));
}

void MainWindow::promptForNickname() {
  QSettings settings;
  if (settings.contains(QStringLiteral("profile/nickname"))) {
    return;
  }

  bool accepted = false;
  const auto nickname = escapeProtocolField(QInputDialog::getText(
      this, QStringLiteral("Welcome to LANChat"),
      QStringLiteral("Choose a username:"), QLineEdit::Normal, {}, &accepted));
  if (accepted && !nickname.isEmpty()) {
    settings.setValue(QStringLiteral("profile/nickname"), nickname);
  }
}

void MainWindow::connectToAddress(const QString &address, quint16 port) {
  auto *socket = new QTcpSocket(this);
  socket->connectToHost(address, port);
  addSession(socket, QStringLiteral("Connecting to %1…").arg(address));
}

void MainWindow::startListening() {
  server_.close();
  const auto listenPort =
      QSettings()
          .value(QStringLiteral("network/listenPort"), DefaultPort)
          .toUInt();
  if (!server_.listen(QHostAddress::Any, listenPort)) {
    statusBar()->showMessage(QStringLiteral("Cannot listen on port %1: %2")
                                 .arg(listenPort)
                                 .arg(server_.errorString()));
    return;
  }
  statusBar()->showMessage(
      QStringLiteral("Listening on TCP port %1").arg(listenPort));
}

void MainWindow::startDiscovery() {
  if (!discoverySocket_.bind(QHostAddress::AnyIPv4, DiscoveryPort,
                             QUdpSocket::ShareAddress |
                                 QUdpSocket::ReuseAddressHint)) {
    statusBar()->showMessage(QStringLiteral("LAN discovery unavailable: %1")
                                 .arg(discoverySocket_.errorString()));
    return;
  }
  connect(&discoverySocket_, &QUdpSocket::readyRead, this,
          &MainWindow::readDiscoveryDatagrams);
  discoveryTimer_.setInterval(2000);
  connect(&discoveryTimer_, &QTimer::timeout, this,
          &MainWindow::announcePresence);
  discoveryTimer_.start();

  probeSendTimer_.setInterval(10);
  connect(&probeSendTimer_, &QTimer::timeout, this,
          &MainWindow::sendDiscoveryProbeBatch);
  subnetScanTimer_.setInterval(5000);
  connect(&subnetScanTimer_, &QTimer::timeout, this,
          &MainWindow::scanPrivateSubnets);
  subnetScanTimer_.start();

  announcePresence();
  scanPrivateSubnets();
}

void MainWindow::announcePresence() {
  const auto nickname = escapeProtocolField(
      QSettings()
          .value(QStringLiteral("profile/nickname"), QStringLiteral("Guest"))
          .toString());
  const auto encodedNickname =
      nickname.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
  const auto listenPort =
      QSettings()
          .value(QStringLiteral("network/listenPort"), DefaultPort)
          .toUInt();
  const auto announcement =
      QStringLiteral("LANCHAT_DISCOVER %1 %2 %3")
          .arg(discoveryId_, QString::fromLatin1(encodedNickname))
          .arg(listenPort)
          .toUtf8();
  discoverySocket_.writeDatagram(announcement, QHostAddress::Broadcast,
                                 DiscoveryPort);
  discoverySocket_.writeDatagram(announcement, QHostAddress::LocalHost,
                                 DiscoveryPort);

  const auto now = QDateTime::currentDateTimeUtc();
  for (auto host = discoveredHosts_.begin(); host != discoveredHosts_.end();) {
    if (host->lastSeen.msecsTo(now) > discoveredHostExpiryMs_) {
      host = discoveredHosts_.erase(host);
    } else {
      ++host;
    }
  }
  refreshHostList();
}

void MainWindow::readDiscoveryDatagrams() {
  while (discoverySocket_.hasPendingDatagrams()) {
    QByteArray data;
    data.resize(int(discoverySocket_.pendingDatagramSize()));
    QHostAddress senderAddress;
    quint16 senderPort = 0;
    discoverySocket_.readDatagram(data.data(), data.size(), &senderAddress,
                                  &senderPort);
    const auto parts =
        QString::fromUtf8(data).trimmed().split(u' ', Qt::KeepEmptyParts);
    if (parts.size() == 2 &&
        parts.at(0) == QStringLiteral("LANCHAT_QUERY") &&
        !parts.at(1).isEmpty() && parts.at(1).size() <= 64 &&
        senderPort == DiscoveryPort &&
        PeerDiscovery::isPrivateIPv4Address(senderAddress) &&
        !PeerDiscovery::isLocalIPv4Address(senderAddress)) {
      const auto nickname = escapeProtocolField(
          QSettings()
              .value(QStringLiteral("profile/nickname"), QStringLiteral("Guest"))
              .toString());
      const auto encodedNickname =
          nickname.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
      const auto listenPort =
          QSettings()
              .value(QStringLiteral("network/listenPort"), DefaultPort)
              .toUInt();
      const auto response =
          QStringLiteral("LANCHAT_DISCOVER %1 %2 %3")
              .arg(discoveryId_, QString::fromLatin1(encodedNickname))
              .arg(listenPort)
              .toUtf8();
      discoverySocket_.writeDatagram(response, senderAddress, senderPort);
      continue;
    }
    if (parts.size() != 4 ||
        parts.at(0) != QStringLiteral("LANCHAT_DISCOVER") ||
        parts.at(1) == discoveryId_ ||
        PeerDiscovery::isLocalIPv4Address(senderAddress)) {
      continue;
    }
    bool portIsValid = false;
    const auto peerPort = parts.at(3).toUShort(&portIsValid);
    const auto nickname = QByteArray::fromBase64(parts.at(2).toLatin1(),
                                                 QByteArray::Base64UrlEncoding);
    if (!portIsValid || peerPort == 0 || nickname.isEmpty()) {
      continue;
    }
    const auto key =
        QStringLiteral("%1:%2").arg(senderAddress.toString()).arg(peerPort);
    discoveredHosts_.insert(key, {QString::fromUtf8(nickname), senderAddress,
                                  peerPort, QDateTime::currentDateTimeUtc()});
  }
  refreshHostList();
}

void MainWindow::scanPrivateSubnets() {
  if (!pendingDiscoveryProbes_.isEmpty()) {
    return;
  }

  const auto targets = PeerDiscovery::privateSubnetHosts();
  discoveredHostExpiryMs_ =
      PeerDiscovery::peerExpiryMilliseconds(targets.size());
  if (targets.isEmpty()) {
    return;
  }

  for (const auto &target : targets) {
    pendingDiscoveryProbes_.enqueue(target);
  }
  if (!probeSendTimer_.isActive()) {
    probeSendTimer_.start();
  }
}

void MainWindow::sendDiscoveryProbeBatch() {
  const auto query =
      QStringLiteral("LANCHAT_QUERY %1").arg(discoveryId_).toUtf8();
  for (int sent = 0; sent < 20 && !pendingDiscoveryProbes_.isEmpty(); ++sent) {
    discoverySocket_.writeDatagram(query, pendingDiscoveryProbes_.dequeue(),
                                   DiscoveryPort);
  }
  if (pendingDiscoveryProbes_.isEmpty()) {
    probeSendTimer_.stop();
  }
}

void MainWindow::refreshHostList() {
  const auto currentText = host_->currentText();
  auto hosts = discoveredHosts_.values();
  std::sort(hosts.begin(), hosts.end(),
            [](const DiscoveredHost &left, const DiscoveredHost &right) {
              return left.nickname.localeAwareCompare(right.nickname) < 0;
            });
  const QSignalBlocker blocker(host_);
  host_->clear();
  if (hosts.isEmpty()) {
    host_->setPlaceholderText(QStringLiteral("No LANChat hosts found"));
    return;
  }
  host_->addItem(QStringLiteral("Select a host to chat with"));
  for (const auto &host : hosts) {
    QVariantMap endpoint;
    endpoint.insert(QStringLiteral("address"), host.address.toString());
    endpoint.insert(QStringLiteral("port"), host.port);
    host_->addItem(
        QStringLiteral("%1 (%2)").arg(host.nickname, host.address.toString()),
        endpoint);
  }
  const auto selectedIndex = host_->findText(currentText);
  if (selectedIndex > 0) {
    host_->setCurrentIndex(selectedIndex);
  } else {
    host_->setCurrentIndex(0);
  }
}

void MainWindow::addSession(QTcpSocket *socket, const QString &initialTitle) {
  auto *session = new ChatSession(socket, this);
  auto *tab = new ChatTab(session, tabs_);
  session->setParent(tab);
  const auto index = tabs_->addTab(tab, initialTitle);
  tabs_->setCurrentIndex(index);
  connect(session, &ChatSession::messageReceived, this,
          [this](const QString &, const QString &) { noteIncomingMessage(); });
  connect(session, &ChatSession::peerIdentified, tab,
          [this, tab](const QString &) {
            tabs_->setTabText(tabs_->indexOf(tab), tab->title());
          });
  session->beginHandshake(
      QSettings()
          .value(QStringLiteral("profile/nickname"), QStringLiteral("Guest"))
          .toString());
}
