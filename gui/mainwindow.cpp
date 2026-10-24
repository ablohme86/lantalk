#include "gui/mainwindow.h"

#include "appconfig.h"
#include "gui/appicon.h"
#include "gui/chattab.h"
#include "gui/dialogs.h"
#include "protocol/chatsession.h"
#include "protocol/chattransport.h"
#include "protocol/connectionregistry.h"
#include "protocol/peerdiscovery.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QColor>
#include <QEvent>
#include <QFont>
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
#include <QTabBar>
#include <QTabWidget>
#include <QTcpSocket>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

MainWindow::MainWindow() {
  setWindowTitle(QStringLiteral("LANTalk v%1").arg(
      QCoreApplication::applicationVersion()));
  setWindowIcon(createAppIcon());
  resize(900, 600);
  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  host_ = new QComboBox(central);
  host_->setPlaceholderText(QStringLiteral("No LANTalk hosts found"));
  tabs_ = new QTabWidget(central);
  tabs_->setTabsClosable(true);
  layout->addWidget(host_);
  layout->addWidget(tabs_);
  setCentralWidget(central);

  auto *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
  auto *exitAction = fileMenu->addAction(QStringLiteral("Exit LANTalk"));
  auto *connectToAction =
      menuBar()->addAction(QStringLiteral("Connect..."));
  auto *settingsMenu = menuBar()->addMenu(QStringLiteral("Settings"));
  auto *fontSizeAction = settingsMenu->addAction(QStringLiteral("Font Size"));
  settingsMenu->addSeparator();
  auto *settingsAction =
      settingsMenu->addAction(QStringLiteral("General..."));
  applyDarkMenuStyle();
  connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);
  connect(connectToAction, &QAction::triggered, this,
          &MainWindow::showConnectDialog);
  connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);
  connect(fontSizeAction, &QAction::triggered, this,
          &MainWindow::showFontSizeDialog);
  setupTrayIcon();
  connect(host_, QOverload<int>::of(&QComboBox::activated), this,
          [this](int index) {
            if (!host_->itemData(index).toMap().isEmpty()) {
              connectToHost();
            }
          });
  connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
    auto *tab = tabs_->widget(index);
    if (auto *chatTab = qobject_cast<ChatTab *>(tab)) {
      chatTab->closeConversation();
    }
    tabs_->removeTab(index);
    tab->deleteLater();
    persistOpenConnections();
  });
  connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
    if (auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index))) {
      setTabUnread(tab, false);
    }
  });
  connect(&server_, &QTcpServer::newConnection, this,
          &MainWindow::acceptConnection);
  connect(&central_, &CentralClient::incomingConnection, this,
          &MainWindow::acceptRelayConnection);
  connect(&central_, &CentralClient::peersChanged, this,
          &MainWindow::refreshHostList);
  connect(&central_, &CentralClient::stateChanged, this,
          &MainWindow::updateCentralStatus);
  // Ollama servers on the network make Live Translate available; look again
  // every few minutes.
  ollamaScanTimer_.setInterval(5 * 60 * 1000);
  connect(&ollamaScanTimer_, &QTimer::timeout, &ollamaDiscovery_,
          &OllamaDiscovery::scan);
  // The window is on screen before the nickname prompt and the network
  // start, so a slow network never looks like LANTalk failed to start.
  QTimer::singleShot(0, this, [this] {
    promptForNickname();
    applyNetworkMode();
    ollamaScanTimer_.start();
    ollamaDiscovery_.scan();
    restoreOpenConnections();
  });
}

void MainWindow::showSettings() {
  SettingsDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  dialog.save();
  applyNetworkMode();
  ollamaDiscovery_.scan();
  for (int index = 0; index < tabs_->count(); ++index) {
    static_cast<ChatTab *>(tabs_->widget(index))
        ->refreshTranslationAvailability();
  }
}

MainWindow::~MainWindow() { ChatTransport::setFactory({}); }

void MainWindow::applyNetworkMode() {
  const QSettings settings;
  const auto centralHost =
      settings.value(QStringLiteral("network/centralHost")).toString().trimmed();
  const auto nickname =
      settings.value(QStringLiteral("profile/nickname"), QStringLiteral("Guest"))
          .toString();
  const auto listenPort =
      quint16(settings.value(QStringLiteral("network/listenPort"), DefaultPort)
                  .toUInt());
  const auto password =
      settings.value(QStringLiteral("network/centralPassword")).toString();
  QString modeKey;
  ollamaDiscovery_.setScanLocalNetwork(centralHost.isEmpty());
  if (centralHost.isEmpty()) {
    central_.stop();
    ChatTransport::setFactory({});
    startListening();
    startDiscovery();
    announcePresence();
  } else {
    server_.close();
    stopDiscovery();
    ChatTransport::setFactory(central_.transportFactory());
    central_.start(centralHost, nickname, listenPort, password);
    modeKey = QStringLiteral("%1\n%2\n%3\n%4")
                  .arg(centralHost, nickname)
                  .arg(listenPort)
                  .arg(password);
    updateCentralStatus();
  }
  refreshHostList();
  const bool modeChanged = networkModeApplied_ && modeKey != networkModeKey_;
  networkModeKey_ = modeKey;
  networkModeApplied_ = true;
  if (!modeChanged) return;
  // Conversations move to the new route right away.
  for (int index = 0; index < tabs_->count(); ++index) {
    if (auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index))) {
      tab->restartConnection();
    }
  }
}

void MainWindow::updateCentralStatus() {
  if (!central_.isActive()) {
    if (!central_.lastError().isEmpty()) {
      statusBar()->showMessage(central_.lastError());
    }
    return;
  }
  if (central_.isReady()) {
    statusBar()->showMessage(
        QStringLiteral("Connected to central host %1").arg(central_.hostSpec()));
  } else if (!central_.lastError().isEmpty()) {
    statusBar()->showMessage(
        QStringLiteral("Not connected to central host %1: %2")
            .arg(central_.hostSpec(), central_.lastError()));
  } else {
    statusBar()->showMessage(
        QStringLiteral("Connecting to central host %1…").arg(central_.hostSpec()));
  }
  refreshHostList();
}

void MainWindow::showFontSizeDialog() {
  const auto currentFont = QApplication::font();
  const int currentSize = currentFont.pointSize() > 0
                              ? currentFont.pointSize()
                              : 11;
  bool accepted = false;
  const int fontSize = QInputDialog::getInt(
      this, QStringLiteral("Font Size"), QStringLiteral("Font size (pt):"),
      currentSize, 8, 32, 1, &accepted);
  if (!accepted) return;

  auto font = currentFont;
  font.setPointSize(fontSize);
  QApplication::setFont(font);
  QSettings().setValue(QStringLiteral("appearance/fontSize"), fontSize);
}

void MainWindow::setupTrayIcon() {
  trayIcon_ = new QSystemTrayIcon(this);
  auto *menu = new QMenu(this);
  auto *openAction = menu->addAction(QStringLiteral("Open LANTalk"));
  menu->addSeparator();
  auto *quitAction = menu->addAction(QStringLiteral("Quit"));
  trayIcon_->setContextMenu(menu);
  connect(openAction, &QAction::triggered, this, &MainWindow::showFromTray);
  connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
  connect(trayIcon_, &QSystemTrayIcon::messageClicked, this, [this] {
    showFromTray();
    if (notifiedTab_ && tabs_->indexOf(notifiedTab_) >= 0) {
      tabs_->setCurrentWidget(notifiedTab_);
    }
  });
  connect(trayIcon_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              showFromTray();
            }
          });
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) {
              clearUnreadMessages();
            }
          });
  updateTrayIcon();
  trayIcon_->show();
}

void MainWindow::updateTrayIcon() {
  if (!trayIcon_) return;
  trayIcon_->setIcon(createAppIcon(unreadMessageCount_));
  if (unreadMessageCount_ == 0) {
    trayIcon_->setToolTip(QStringLiteral("LANTalk"));
    return;
  }
  trayIcon_->setToolTip(
      QStringLiteral("LANTalk — %1 unread message%2")
          .arg(unreadMessageCount_)
          .arg(unreadMessageCount_ == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::showFromTray() {
  activateFromAnotherLaunch();
}

bool MainWindow::event(QEvent *event) {
  if (event->type() == QEvent::WindowActivate) {
    clearUnreadMessages();
  }
  return QMainWindow::event(event);
}

void MainWindow::activateFromAnotherLaunch() {
  if (isMinimized()) showNormal();
  else show();
  raise();
  activateWindow();
}

void MainWindow::clearUnreadMessages() {
  if (unreadMessageCount_ == 0) return;
  unreadMessageCount_ = 0;
  updateTrayIcon();
}

bool MainWindow::inForeground() const {
  return isActiveWindow() ||
         (QGuiApplication::applicationState() == Qt::ApplicationActive &&
          isVisible() && !isMinimized());
}

void MainWindow::noteIncomingMessage() {
  if (inForeground()) {
    clearUnreadMessages();
    return;
  }
  ++unreadMessageCount_;
  updateTrayIcon();
}

void MainWindow::showNotification(ChatTab *tab, const QString &sender,
                                  const QString &preview) {
  // A desktop notification (a toast on Windows, the notification service on
  // Linux) through the tray icon, unless LANTalk is in front by now.
  if (inForeground() || !trayIcon_ || !QSystemTrayIcon::supportsMessages() ||
      !QSettings()
           .value(QStringLiteral("notifications/enabled"), true)
           .toBool()) {
    return;
  }
  notifiedTab_ = tab;
  trayIcon_->showMessage(sender, preview, createAppIcon(), 8000);
}

void MainWindow::setTabUnread(ChatTab *tab, bool unread) {
  const int index = tabs_->indexOf(tab);
  if (index < 0) return;
  tabs_->tabBar()->setTabTextColor(
      index, unread ? QColor(QStringLiteral("#ffbf47")) : QColor());
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
  connectToAddress(address, peerPort,
                   endpoint.value(QStringLiteral("nickname")).toString());
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
    auto *socket = server_.nextPendingConnection();
    const auto port = discoveredPortForAddress(socket->peerAddress());
    adoptIncomingSession(new ChatSession(socket, this), port);
  }
}

void MainWindow::acceptRelayConnection(RelayTransport *transport) {
  adoptIncomingSession(new ChatSession(transport, this), transport->peerPort());
}

void MainWindow::adoptIncomingSession(ChatSession *session, quint16 port) {
  const auto address = session->peerAddress();
  session->setRemoteEndpoint(address, port);
  // The tab stays hidden until the handshake shows that this is a new
  // conversation; a duplicate of an open one is merged into its tab.
  auto *tab = createTab(session, address, port);
  tab->hide();
  connect(session, &ChatSession::peerIdentified, this, [this, tab] {
    if (tabs_->indexOf(tab) < 0) showTab(tab, tab->title());
  });
  connect(session, &ChatSession::disconnected, tab, [this, tab] {
    if (tabs_->indexOf(tab) < 0) tab->deleteLater();
  });
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
      this, QStringLiteral("Welcome to LANTalk"),
      QStringLiteral("Choose a username:"), QLineEdit::Normal, {}, &accepted));
  if (accepted && !nickname.isEmpty()) {
    settings.setValue(QStringLiteral("profile/nickname"), nickname);
  }
}

void MainWindow::connectToAddress(const QString &address, quint16 port,
                                  const QString &nickname) {
  if (auto *existing =
          ConnectionRegistry::shared()->findSession(address, port, nickname)) {
    if (auto *tab = tabForSession(existing)) {
      tabs_->setCurrentWidget(tab);
      tab->reconnectNow();
      statusBar()->showMessage(
          QStringLiteral("Already connected to %1; switched to its tab.")
              .arg(tab->title()),
          5000);
    } else {
      statusBar()->showMessage(
          QStringLiteral("A connection to %1 is already being established.")
              .arg(address),
          5000);
    }
    return;
  }
  auto *session = new ChatSession(ChatTransport::create(this), this);
  auto *tab = createTab(session, address, port);
  showTab(tab, QStringLiteral("Connecting to %1…").arg(address));
  session->connectToHost(address, port, nickname);
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
  if (discoverySocket_.state() == QAbstractSocket::BoundState) return;
  if (!discoverySocket_.bind(QHostAddress::AnyIPv4, DiscoveryPort,
                             QUdpSocket::ShareAddress |
                                 QUdpSocket::ReuseAddressHint)) {
    statusBar()->showMessage(QStringLiteral("LAN discovery unavailable: %1")
                                 .arg(discoverySocket_.errorString()));
    return;
  }
  if (discoverySetUp_) {
    discoveryTimer_.start();
    subnetScanTimer_.start();
    scanPrivateSubnets();
    return;
  }
  discoverySetUp_ = true;
  connect(&discoverySocket_, &QUdpSocket::readyRead, this,
          &MainWindow::readDiscoveryDatagrams);
  discoveryTimer_.setInterval(2000);
  connect(&discoveryTimer_, &QTimer::timeout, this,
          &MainWindow::announcePresence);
  discoveryTimer_.start();

  probeSendTimer_.setInterval(10);
  connect(&probeSendTimer_, &QTimer::timeout, this,
          &MainWindow::sendDiscoveryProbeBatch);
  discoverySettleTimer_.setSingleShot(true);
  connect(&discoverySettleTimer_, &QTimer::timeout, this, [this] {
    initialDiscoveryScanComplete_ = true;
    if (discoveredHosts_.isEmpty()) {
      host_->setPlaceholderText(QStringLiteral("No LANTalk hosts found"));
    }
  });
  subnetScanTimer_.setInterval(5000);
  connect(&subnetScanTimer_, &QTimer::timeout, this,
          &MainWindow::scanPrivateSubnets);
  subnetScanTimer_.start();

  announcePresence();
  scanPrivateSubnets();
}

void MainWindow::stopDiscovery() {
  discoveryTimer_.stop();
  subnetScanTimer_.stop();
  probeSendTimer_.stop();
  discoverySettleTimer_.stop();
  pendingDiscoveryProbes_.clear();
  discoverySocket_.close();
  discoveredHosts_.clear();
}

void MainWindow::announcePresence() {
  if (discoverySocket_.state() != QAbstractSocket::BoundState) return;
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
    if (!initialDiscoveryScanComplete_) {
      initialDiscoveryScanComplete_ = true;
      host_->setPlaceholderText(QStringLiteral("No LANTalk hosts found"));
    }
    return;
  }

  if (!initialDiscoveryScanComplete_) {
    host_->setPlaceholderText(QStringLiteral("Scanning for hosts..."));
    discoverySettleTimer_.stop();
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
    if (!initialDiscoveryScanComplete_) {
      discoverySettleTimer_.start(1000);
    }
  }
}

void MainWindow::refreshHostList() {
  const auto currentText = host_->currentText();
  auto hosts = discoveredHosts_.values();
  if (central_.isActive()) {
    // The central host lists the clients connected to it instead.
    hosts.clear();
    for (const auto &peer : central_.peers()) {
      hosts.append({peer.nickname, QHostAddress(peer.address), peer.port,
                    QDateTime::currentDateTimeUtc()});
    }
  }
  std::sort(hosts.begin(), hosts.end(),
            [](const DiscoveredHost &left, const DiscoveredHost &right) {
              return left.nickname.localeAwareCompare(right.nickname) < 0;
            });
  const QSignalBlocker blocker(host_);
  host_->clear();
  if (hosts.isEmpty()) {
    const bool searching = central_.isActive() ? !central_.isReady()
                                               : !initialDiscoveryScanComplete_;
    host_->setPlaceholderText(
        !searching             ? QStringLiteral("No LANTalk hosts found")
        : central_.isActive() ? QStringLiteral("Connecting to the central host...")
                              : QStringLiteral("Scanning for hosts..."));
    return;
  }
  host_->addItem(QStringLiteral("Select a host to chat with"));
  for (const auto &host : hosts) {
    QVariantMap endpoint;
    endpoint.insert(QStringLiteral("address"), host.address.toString());
    endpoint.insert(QStringLiteral("port"), host.port);
    endpoint.insert(QStringLiteral("nickname"), host.nickname);
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

ChatTab *MainWindow::createTab(ChatSession *session,
                               const QString &reconnectAddress,
                               quint16 reconnectPort) {
  auto *tab = new ChatTab(session, this);
  session->setParent(tab);
  tab->setReconnectEndpoint(reconnectAddress, reconnectPort);
  tab->setOllamaDiscovery(&ollamaDiscovery_);
  connect(session, &ChatSession::mergedInto, this,
          [this, tab](ChatSession *existing) {
            const bool wasCurrent = tabs_->currentWidget() == tab;
            const int index = tabs_->indexOf(tab);
            if (index >= 0) tabs_->removeTab(index);
            tab->deleteLater();
            auto *existingTab = tabForSession(existing);
            if (existingTab && wasCurrent) tabs_->setCurrentWidget(existingTab);
            persistOpenConnections();
          });
  connect(tab, &ChatTab::incomingMessageReceived, this,
          [this, tab] {
            if (tabs_->currentWidget() != tab) setTabUnread(tab, true);
            noteIncomingMessage();
          });
  connect(tab, &ChatTab::notifyIncoming, this,
          [this, tab](const QString &sender, const QString &preview) {
            showNotification(tab, sender, preview);
          });
  connect(tab, &ChatTab::titleChanged, this, [this, tab] {
    const auto tabIndex = tabs_->indexOf(tab);
    if (tabIndex >= 0) {
      tabs_->setTabText(tabIndex, tab->title());
      persistOpenConnections();
    }
  });
  session->beginHandshake(
      QSettings()
          .value(QStringLiteral("profile/nickname"), QStringLiteral("Guest"))
          .toString());
  return tab;
}

void MainWindow::showTab(ChatTab *tab, const QString &title) {
  tabs_->setCurrentIndex(tabs_->addTab(tab, title));
  persistOpenConnections();
}

ChatTab *MainWindow::tabForSession(ChatSession *session) const {
  for (int index = 0; index < tabs_->count(); ++index) {
    auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index));
    if (tab && tab->session() == session) return tab;
  }
  return nullptr;
}

void MainWindow::restoreOpenConnections() {
  const auto savedConnections =
      QSettings()
          .value(QStringLiteral("connections/openTabs"))
          .toList();
  for (const auto &saved : savedConnections) {
    const auto connection = saved.toMap();
    const auto address = connection.value(QStringLiteral("address")).toString();
    bool portIsValid = false;
    const auto port =
        connection.value(QStringLiteral("port")).toUInt(&portIsValid);
    if (!address.trimmed().isEmpty() && portIsValid && port > 0 &&
        port <= 65535) {
      connectToAddress(address, quint16(port),
                       connection.value(QStringLiteral("nickname")).toString());
    }
  }
  persistOpenConnections();
}

void MainWindow::persistOpenConnections() {
  QVariantList openConnections;
  for (int index = 0; index < tabs_->count(); ++index) {
    auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index));
    if (!tab || tab->reconnectAddress().isEmpty() ||
        tab->reconnectPort() == 0) {
      continue;
    }
    QVariantMap connection;
    connection.insert(QStringLiteral("address"), tab->reconnectAddress());
    connection.insert(QStringLiteral("port"), tab->reconnectPort());
    // Tells clients behind the same address apart on a central host.
    if (!tab->peerNickname().isEmpty()) {
      connection.insert(QStringLiteral("nickname"), tab->peerNickname());
    }
    openConnections.append(connection);
  }
  QSettings().setValue(QStringLiteral("connections/openTabs"),
                       openConnections);
}

quint16 MainWindow::discoveredPortForAddress(
    const QHostAddress &address) const {
  const DiscoveredHost *mostRecent = nullptr;
  for (auto current = discoveredHosts_.cbegin();
       current != discoveredHosts_.cend(); ++current) {
    if (current->address != address ||
        (mostRecent && current->lastSeen <= mostRecent->lastSeen)) {
      continue;
    }
    mostRecent = &current.value();
  }
  return mostRecent ? mostRecent->port : DefaultPort;
}
