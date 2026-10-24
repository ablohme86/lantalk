#include "gui/mainwindow.h"

#include "appconfig.h"
#include "gui/appicon.h"
#include "gui/chattab.h"
#include "gui/dialogs.h"
#include "messagetranslator.h"
#include "protocol/chatsession.h"
#include "protocol/chattransport.h"
#include "protocol/connectionregistry.h"
#include "protocol/conversationidentity.h"
#include "protocol/peerdiscovery.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#ifdef LANTALK_HAVE_MULTIMEDIA
#include <QSoundEffect>
#endif
#include <QSplitter>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpSocket>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <QPainter>
#include <QStyledItemDelegate>

#include <algorithm>

namespace {

// Where a sidebar host is: its address, or "Web".
constexpr int HostSubtitleRole = Qt::UserRole + 1;
// Whether a sidebar host is away (bool).
constexpr int HostAwayRole = Qt::UserRole + 2;

// A sidebar host: the nickname, and below it in a smaller, muted font
// where the host is.
class HostItemDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem background = option;
    initStyleOption(&background, index);
    background.text.clear();
    option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &background,
                                        painter, option.widget);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    painter->save();
    // A green or yellow dot before the name; listed hosts are online.
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(QColor(0, 0, 0, 60), 1));
    painter->setBrush(presenceColor(index.data(HostAwayRole).toBool()
                                        ? Presence::Away
                                        : Presence::Online));
    const QFontMetrics dotMetrics(option.font);
    painter->drawEllipse(QRectF(option.rect.left() + 10,
                                option.rect.top() + 5 + dotMetrics.height() / 2.0 - 4.5,
                                9, 9));
    const auto area = option.rect.adjusted(27, 5, -8, -5);
    painter->setPen(option.palette.color(
        selected ? QPalette::HighlightedText : QPalette::Text));
    painter->setFont(option.font);
    const QFontMetrics nameMetrics(option.font);
    painter->drawText(area.left(), area.top() + nameMetrics.ascent(),
                      nameMetrics.elidedText(index.data().toString(),
                                             Qt::ElideRight, area.width()));
    auto small = option.font;
    small.setPointSizeF(small.pointSizeF() * 0.85);
    painter->setFont(small);
    auto muted = option.palette.color(
        selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
    painter->setPen(muted);
    const QFontMetrics smallMetrics(small);
    painter->drawText(area.left(),
                      area.top() + nameMetrics.height() + 2 + smallMetrics.ascent(),
                      smallMetrics.elidedText(index.data(HostSubtitleRole).toString(),
                                              Qt::ElideRight, area.width()));
    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &) const override {
    auto small = option.font;
    small.setPointSizeF(small.pointSizeF() * 0.85);
    return {160, QFontMetrics(option.font).height() +
                     QFontMetrics(small).height() + 12};
  }
};

} // namespace

MainWindow::MainWindow() {
  setWindowTitle(QStringLiteral("LANTalk v%1").arg(
      QCoreApplication::applicationVersion()));
  setWindowIcon(createAppIcon());
  resize(900, 600);
  splitter_ = new QSplitter(Qt::Horizontal, this);
  auto *sidebar = new QWidget(splitter_);
  auto *sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(6, 6, 0, 6);
  sidebarLayout->setSpacing(4);
  auto *hostsTitle = new QLabel(tr("Hosts"), sidebar);
  auto titleFont = hostsTitle->font();
  titleFont.setBold(true);
  hostsTitle->setFont(titleFont);
  hosts_ = new QListWidget(sidebar);
  hosts_->setSelectionMode(QAbstractItemView::SingleSelection);
  hosts_->setUniformItemSizes(true);
  hosts_->setSpacing(1);
  hosts_->setItemDelegate(new HostItemDelegate(hosts_));
  // The open conversation's host stays marked while typing in it.
  auto hostsPalette = hosts_->palette();
  for (const auto role : {QPalette::Highlight, QPalette::HighlightedText}) {
    hostsPalette.setColor(QPalette::Inactive, role,
                          hostsPalette.color(QPalette::Active, role));
  }
  hosts_->setPalette(hostsPalette);
  hostListStatus_ = new QLabel(tr("Scanning for hosts..."), sidebar);
  hostListStatus_->setWordWrap(true);
  hostListStatus_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  hostListStatus_->setStyleSheet(QStringLiteral("color: palette(mid); padding: 6px 2px;"));
  sidebarLayout->addWidget(hostsTitle);
  sidebarLayout->addWidget(hosts_, 1);
  sidebarLayout->addWidget(hostListStatus_, 1);
  hosts_->hide();
  tabs_ = new QTabWidget(splitter_);
  tabs_->setTabsClosable(true);
  splitter_->addWidget(sidebar);
  splitter_->addWidget(tabs_);
  splitter_->setStretchFactor(1, 1);
  splitter_->setChildrenCollapsible(false);
  splitter_->setSizes({220, 680});
  splitter_->restoreState(
      QSettings().value(QStringLiteral("appearance/sidebar")).toByteArray());
  connect(splitter_, &QSplitter::splitterMoved, this, [this] {
    QSettings().setValue(QStringLiteral("appearance/sidebar"),
                         splitter_->saveState());
  });
  setCentralWidget(splitter_);

  auto *fileMenu = menuBar()->addMenu(tr("File"));
  auto *exitAction = fileMenu->addAction(tr("Exit LANTalk"));
  auto *connectToAction =
      menuBar()->addAction(tr("Connect..."));
  auto *settingsMenu = menuBar()->addMenu(tr("Settings"));
  auto *fontSizeAction = settingsMenu->addAction(tr("Font Size"));
  settingsMenu->addSeparator();
  auto *settingsAction =
      settingsMenu->addAction(tr("General..."));
  applyDarkMenuStyle();
  connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);
  connect(connectToAction, &QAction::triggered, this,
          &MainWindow::showConnectDialog);
  connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);
  connect(fontSizeAction, &QAction::triggered, this,
          &MainWindow::showFontSizeDialog);
  setupTrayIcon();
  lastActivity_.start();
  qApp->installEventFilter(this);
  awayTimer_.setInterval(30 * 1000);
  connect(&awayTimer_, &QTimer::timeout, this, [this] {
    if (!away_ && lastActivity_.elapsed() > AwayAfterMs) setAway(true);
  });
  awayTimer_.start();
#ifdef LANTALK_HAVE_MULTIMEDIA
  messageSound_ = new QSoundEffect(this);
  messageSound_->setSource(QUrl(QStringLiteral("qrc:/sounds/message.wav")));
  messageSound_->setVolume(0.6f);
#endif
  // One click opens the conversation, or switches to it if it is open.
  connect(hosts_, &QListWidget::itemClicked, this,
          &MainWindow::connectToHostItem);
  connect(hosts_, &QListWidget::itemActivated, this,
          &MainWindow::connectToHostItem);
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
      selectHostForTab(tab);
    }
  });
  connect(&server_, &QTcpServer::newConnection, this,
          &MainWindow::acceptConnection);
  connect(&central_, &CentralClient::incomingConnection, this,
          &MainWindow::acceptRelayConnection);
  connect(&central_, &CentralClient::peersChanged, this,
          &MainWindow::refreshHostList);
  connect(&central_, &CentralClient::peersChanged, this,
          &MainWindow::reconnectArrivedPeers);
  connect(&central_, &CentralClient::peersChanged, this, [this] {
    for (int index = 0; index < tabs_->count(); ++index) {
      if (auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index))) updateTabStatus(tab);
    }
  });
  connect(&central_, &CentralClient::stateChanged, this,
          &MainWindow::updateCentralStatus);
  connect(&central_, &CentralClient::translationAvailabilityChanged, this,
          &MainWindow::updateCentralTranslation);
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

MainWindow::~MainWindow() {
  ChatTransport::setFactory({});
  MessageTranslator::setRemote({});
}

void MainWindow::reconnectArrivedPeers() {
  // Peers that just appeared, e.g. after they or the central host came back.
  QSet<QString> present;
  QList<CentralClient::Peer> arrived;
  for (const auto &peer : central_.peers()) {
    const auto key = ConversationIdentity::normalizedAddress(peer.address) +
                     u'\n' + ConversationIdentity::normalizedNickname(peer.nickname);
    present.insert(key);
    if (!centralPeersSeen_.contains(key)) arrived.append(peer);
  }
  centralPeersSeen_ = present;
  for (const auto &peer : std::as_const(arrived)) {
    for (int index = 0; index < tabs_->count(); ++index) {
      auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index));
      if (!tab || tab->isConnected() ||
          !ConversationIdentity::sameAddress(peer.address, tab->reconnectAddress()) ||
          (!tab->peerNickname().isEmpty() &&
           !ConversationIdentity::sameNickname(peer.nickname, tab->peerNickname()))) {
        continue;
      }
      tab->reconnectNow();
    }
  }
}

void MainWindow::updateCentralTranslation() {
  // A central host with a verified Ollama server translates for every
  // conversation.
  if (central_.canTranslate()) {
    QPointer<CentralClient> central(&central_);
    MessageTranslator::setRemote(
        [central](const QString &message, const QString &source,
                  const QString &target,
                  std::function<void(bool, const QString &)> done) {
          if (central) central->translate(message, source, target, std::move(done));
        });
  } else {
    MessageTranslator::setRemote({});
  }
  for (int index = 0; index < tabs_->count(); ++index) {
    static_cast<ChatTab *>(tabs_->widget(index))
        ->refreshTranslationAvailability();
  }
}

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
  for (int index = 0; index < tabs_->count(); ++index) {
    if (auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index))) {
      tab->setReconnectOnPresence(central_.isActive());
    }
  }
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
        tr("Connected to central host %1").arg(central_.hostSpec()));
  } else if (!central_.lastError().isEmpty()) {
    statusBar()->showMessage(
        tr("Not connected to central host %1: %2")
            .arg(central_.hostSpec(), central_.lastError()));
  } else {
    statusBar()->showMessage(
        tr("Connecting to central host %1…").arg(central_.hostSpec()));
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
      this, tr("Font Size"), tr("Font size (pt):"),
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
  auto *openAction = menu->addAction(tr("Open LANTalk"));
  menu->addSeparator();
  auto *quitAction = menu->addAction(tr("Quit"));
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
      unreadMessageCount_ == 1
          ? tr("LANTalk — 1 unread message")
          : tr("LANTalk — %1 unread messages").arg(unreadMessageCount_));
}

void MainWindow::showFromTray() {
  activateFromAnotherLaunch();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
  switch (event->type()) {
  case QEvent::KeyPress:
  case QEvent::MouseButtonPress:
  case QEvent::MouseMove:
  case QEvent::Wheel:
  case QEvent::TouchBegin:
    lastActivity_.restart();
    if (away_) setAway(false);
    break;
  default:
    break;
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setAway(bool away) {
  away_ = away;
  ChatSession::setLocalAway(away);
  central_.setAway(away);
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

void MainWindow::playMessageSound(ChatTab *tab) {
  // Only when the message would otherwise go unseen, and at most once per
  // burst so a flurry of messages does not turn into a drum roll.
  if ((inForeground() && tabs_->currentWidget() == tab) ||
      !QSettings().value(QStringLiteral("notifications/sound"), true).toBool()) {
    return;
  }
  if (lastMessageSound_.isValid() && lastMessageSound_.elapsed() < 1500) return;
  lastMessageSound_.start();
#ifdef LANTALK_HAVE_MULTIMEDIA
  messageSound_->play();
#else
  QApplication::beep();
#endif
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

void MainWindow::connectToHostItem(QListWidgetItem *item) {
  const auto endpoint = item ? item->data(Qt::UserRole).toMap() : QVariantMap();
  const auto address = endpoint.value(QStringLiteral("address")).toString();
  if (address.isEmpty()) {
    return;
  }
  connectToAddress(address, endpoint.value(QStringLiteral("port")).toUInt(),
                   endpoint.value(QStringLiteral("nickname")).toString());
}

void MainWindow::showConnectDialog() {
  bool accepted = false;
  const auto address = QInputDialog::getText(this, tr("Connect To"),
                                             tr("Host IP or name:"),
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
      this, tr("Welcome to LANTalk"),
      tr("Choose a username:"), QLineEdit::Normal, {}, &accepted));
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
          tr("Already connected to %1; switched to its tab.")
              .arg(tab->title()),
          5000);
    } else {
      statusBar()->showMessage(
          tr("A connection to %1 is already being established.")
              .arg(address),
          5000);
    }
    return;
  }
  auto *session = new ChatSession(ChatTransport::create(this), this);
  auto *tab = createTab(session, address, port);
  showTab(tab, tr("Connecting to %1…").arg(address));
  session->connectToHost(address, port, nickname);
}

void MainWindow::startListening() {
  server_.close();
  const auto listenPort =
      QSettings()
          .value(QStringLiteral("network/listenPort"), DefaultPort)
          .toUInt();
  if (!server_.listen(QHostAddress::Any, listenPort)) {
    statusBar()->showMessage(tr("Cannot listen on port %1: %2")
                                 .arg(listenPort)
                                 .arg(server_.errorString()));
    return;
  }
  statusBar()->showMessage(
      tr("Listening on TCP port %1").arg(listenPort));
}

void MainWindow::startDiscovery() {
  if (discoverySocket_.state() == QAbstractSocket::BoundState) return;
  if (!discoverySocket_.bind(QHostAddress::AnyIPv4, DiscoveryPort,
                             QUdpSocket::ShareAddress |
                                 QUdpSocket::ReuseAddressHint)) {
    statusBar()->showMessage(tr("LAN discovery unavailable: %1")
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
      setHostListStatus(tr("No LANTalk hosts found"));
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
      setHostListStatus(tr("No LANTalk hosts found"));
    }
    return;
  }

  if (!initialDiscoveryScanComplete_) {
    setHostListStatus(tr("Scanning for hosts..."));
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
  auto hosts = discoveredHosts_.values();
  QSet<QString> webClients;
  if (central_.isActive()) {
    // The central host lists the clients connected to it instead.
    hosts.clear();
    for (const auto &peer : central_.peers()) {
      hosts.append({peer.nickname, QHostAddress(peer.address), peer.port,
                    QDateTime::currentDateTimeUtc()});
      if (peer.web) webClients.insert(peer.address + u'\n' + peer.nickname);
    }
  }
  std::sort(hosts.begin(), hosts.end(),
            [](const DiscoveredHost &left, const DiscoveredHost &right) {
              return left.nickname.localeAwareCompare(right.nickname) < 0;
            });
  const QSignalBlocker blocker(hosts_);
  hosts_->clear();
  if (hosts.isEmpty()) {
    const bool searching = central_.isActive() ? !central_.isReady()
                                               : !initialDiscoveryScanComplete_;
    setHostListStatus(!searching             ? tr("No LANTalk hosts found")
                      : central_.isActive() ? tr("Connecting to the central host...")
                                            : tr("Scanning for hosts..."));
    return;
  }
  for (const auto &host : hosts) {
    const auto address = host.address.toString();
    const bool web = webClients.contains(address + u'\n' + host.nickname);
    QVariantMap endpoint;
    endpoint.insert(QStringLiteral("address"), address);
    endpoint.insert(QStringLiteral("port"), host.port);
    endpoint.insert(QStringLiteral("nickname"), host.nickname);
    auto *item = new QListWidgetItem(host.nickname, hosts_);
    const bool away = central_.isActive()
                          ? centralPeerAway(address, host.nickname)
                          : lanPeerAway(address, host.nickname);
    item->setData(HostSubtitleRole,
                  away ? QStringLiteral("%1 · %2").arg(web ? tr("Web") : address, tr("Away"))
                       : (web ? tr("Web") : address));
    item->setData(HostAwayRole, away);
    item->setData(Qt::UserRole, endpoint);
    item->setToolTip(web ? tr("%1 (Web)").arg(host.nickname)
                         : QStringLiteral("%1 (%2)").arg(host.nickname, address));
  }
  setHostListStatus({});
  if (auto *tab = qobject_cast<ChatTab *>(tabs_->currentWidget())) {
    selectHostForTab(tab);
  }
}

void MainWindow::setHostListStatus(const QString &text) {
  hostListStatus_->setText(text);
  hostListStatus_->setVisible(!text.isEmpty());
  hosts_->setVisible(text.isEmpty());
}

void MainWindow::selectHostForTab(ChatTab *tab) {
  // The host whose conversation is in front is the selected one; until the
  // handshake names the peer, the selection stays as it is.
  if (tab->peerNickname().isEmpty()) return;
  const QSignalBlocker blocker(hosts_);
  for (int row = 0; row < hosts_->count(); ++row) {
    const auto endpoint = hosts_->item(row)->data(Qt::UserRole).toMap();
    if (ConversationIdentity::sameAddress(
            endpoint.value(QStringLiteral("address")).toString(),
            tab->reconnectAddress()) &&
        ConversationIdentity::sameNickname(
            endpoint.value(QStringLiteral("nickname")).toString(),
            tab->peerNickname())) {
      hosts_->setCurrentRow(row);
      return;
    }
  }
  hosts_->clearSelection();
}

ChatTab *MainWindow::createTab(ChatSession *session,
                               const QString &reconnectAddress,
                               quint16 reconnectPort) {
  auto *tab = new ChatTab(session, this);
  session->setParent(tab);
  tab->setReconnectEndpoint(reconnectAddress, reconnectPort);
  tab->setOllamaDiscovery(&ollamaDiscovery_);
  tab->setReconnectOnPresence(central_.isActive());
  connect(session, &ChatSession::peerAwayChanged, this, [this, tab] {
    updateTabStatus(tab);
    refreshHostList();
  });
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
            playMessageSound(tab);
            noteIncomingMessage();
          });
  connect(tab, &ChatTab::notifyIncoming, this,
          [this, tab](const QString &sender, const QString &preview) {
            showNotification(tab, sender, preview);
          });
  connect(tab, &ChatTab::titleChanged, this, [this, tab] {
    const auto tabIndex = tabs_->indexOf(tab);
    if (tabIndex >= 0) {
      if (tabs_->currentWidget() == tab) selectHostForTab(tab);
      tabs_->setTabText(tabIndex, tab->title());
      updateTabStatus(tab);
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
  updateTabStatus(tab);
  persistOpenConnections();
}

void MainWindow::updateTabStatus(ChatTab *tab) {
  const int index = tabs_->indexOf(tab);
  if (index < 0) return;
  const auto presence = presenceOf(tab);
  tabs_->setTabIcon(index, createStatusIcon(presence));
  tabs_->setTabToolTip(index, presence == Presence::Online ? tr("Online")
                              : presence == Presence::Away ? tr("Away")
                                                           : tr("Offline"));
}

Presence MainWindow::presenceOf(ChatTab *tab) const {
  if (!tab->isConnected()) return Presence::Offline;
  // The peer says so itself, or the central host does for peers whose
  // LANTalk predates PRESENCE.
  return tab->session()->peerAway() ||
                 centralPeerAway(tab->reconnectAddress(), tab->peerNickname())
             ? Presence::Away
             : Presence::Online;
}

bool MainWindow::lanPeerAway(const QString &address,
                             const QString &nickname) const {
  // On the LAN only an open conversation tells.
  for (int index = 0; index < tabs_->count(); ++index) {
    auto *tab = qobject_cast<ChatTab *>(tabs_->widget(index));
    if (tab && tab->isConnected() && tab->session()->peerAway() &&
        ConversationIdentity::sameAddress(tab->reconnectAddress(), address) &&
        ConversationIdentity::sameNickname(tab->peerNickname(), nickname)) {
      return true;
    }
  }
  return false;
}

bool MainWindow::centralPeerAway(const QString &address,
                                 const QString &nickname) const {
  if (!central_.isActive()) return false;
  for (const auto &peer : central_.peers()) {
    if (ConversationIdentity::sameAddress(peer.address, address) &&
        ConversationIdentity::sameNickname(peer.nickname, nickname)) {
      return peer.away;
    }
  }
  return false;
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
