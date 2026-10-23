#include "tui/terminalui.h"

#include "appconfig.h"
#include "protocol/chathistory.h"
#include "protocol/centralprotocol.h"
#include "protocol/chattransport.h"
#include "protocol/peerdiscovery.h"
#include "protocol/chatsession.h"
#include "protocol/connectionregistry.h"
#include "protocol/quotedmessage.h"
#include "messagetranslator.h"
#include "translationrestorer.h"

#include <QCoreApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTcpSocket>

#ifdef Q_OS_UNIX
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <algorithm>

namespace {
QTextStream terminalOutput(stdout);
QTextStream terminalInput(stdin);

QString nickname()
{
    return escapeProtocolField(
        QSettings().value(QStringLiteral("profile/nickname"), QStringLiteral("Guest")).toString());
}

bool isTerminal()
{
#ifdef Q_OS_UNIX
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#else
    return false;
#endif
}

int displayWidth(const QString &text)
{
    int width = 0;
    for (int index = 0; index < text.size(); ++index) {
        if (text.at(index) == u'\x1b' && index + 1 < text.size() && text.at(index + 1) == u'[') {
            index += 2;
            while (index < text.size()) {
                const QChar character = text.at(index);
                if (character >= u'@' && character <= u'~') break;
                ++index;
            }
        } else if (text.at(index).isPrint()) {
            ++width;
        }
    }
    return width;
}

// Top menu entries; '&' marks the accelerator letter, as in MS-DOS Edit.
struct MenuEntry {
    QString label;
    QString shortcut;
};

const QStringList &menuTitles()
{
    static const QStringList titles{QStringLiteral("&Chat"), QStringLiteral("&Settings")};
    return titles;
}

QList<MenuEntry> menuEntries(int menu)
{
    if (menu == 0) {
        return {{QStringLiteral("&Connect To..."), {}},
                {QStringLiteral("C&lose Chat"), QStringLiteral("Ctrl+W")},
                {QStringLiteral("&Quit"), QStringLiteral("Ctrl+C")}};
    }
    return {{QStringLiteral("&Nickname..."), {}},
            {QStringLiteral("&Ollama Settings..."), {}},
            {QStringLiteral("&Listening Port..."), {}},
            {QStringLiteral("&Central Host..."), {}},
            {QStringLiteral("&Back"), QStringLiteral("Esc")}};
}

QChar accelerator(const QString &label)
{
    const int marker = label.indexOf(u'&');
    return marker >= 0 && marker + 1 < label.size() ? label.at(marker + 1).toLower() : QChar();
}

int labelWidth(const QString &label)
{
    return label.size() - (label.contains(u'&') ? 1 : 0);
}

// Underlines the accelerator without disturbing the surrounding colors.
QString styledLabel(const QString &label)
{
    const int marker = label.indexOf(u'&');
    if (marker < 0 || marker + 1 >= label.size()) return label;
    return label.left(marker) + QStringLiteral("\x1b[4m") + label.at(marker + 1)
           + QStringLiteral("\x1b[24m") + label.mid(marker + 2);
}

const auto PasteStart = QStringLiteral("\x1b[200~");
const auto PasteEnd = QStringLiteral("\x1b[201~");

// Transcript lines for a message: continuation lines are indented and
// quotes are shown as a bar-marked block.
QStringList transcriptLines(const QString &message)
{
    QStringList lines;
    for (const auto &segment : QuotedMessage::parse(message)) {
        for (const auto &part : segment.text.split(u'\n')) {
            if (segment.quote) lines << QStringLiteral("    │ ") + part;
            else lines << (lines.isEmpty() ? part : QStringLiteral("    ") + part);
        }
    }
    if (lines.isEmpty()) lines << QString();
    return lines;
}

const auto ReconnectHint = QStringLiteral("Re-connect to this host to start chatting");

const auto MenuBarStyle = QStringLiteral("\x1b[0;97;44m");
const auto MenuHighlightStyle = QStringLiteral("\x1b[0;97;42m");
const auto MenuBoxStyle = QStringLiteral("\x1b[0;97;100m");
} // namespace

TerminalUi::TerminalUi(QObject *parent) : QObject(parent)
{
    configureNickname();
    ansi_ = isTerminal();
    configureTerminal();
    connect(&server_, &QTcpServer::newConnection, this, &TerminalUi::acceptConnection);
    connect(&central_, &CentralClient::incomingConnection, this, &TerminalUi::acceptRelayConnection);
    connect(&central_, &CentralClient::peersChanged, this, [this] {
        if (ansi_) redraw();
    });
    connect(&central_, &CentralClient::stateChanged, this, &TerminalUi::reportCentralState);
    applyNetworkMode();

#ifdef Q_OS_UNIX
    auto *inputNotifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(inputNotifier, &QSocketNotifier::activated, this, &TerminalUi::handleInput);
#endif
    if (!ansi_) {
        print(QStringLiteral("LANTalk terminal mode\n"
                             "Commands: /help, /hosts, /connect <host|number> [port], /tabs, "
                             "/use <tab>, /close [tab], /nick <name>, /translate <my language> | "
                             "<peer language>, /stoptranslate, /quit"));
        printPrompt();
        return;
    }

    resizeTimer_.setInterval(500);
    connect(&resizeTimer_, &QTimer::timeout, this, &TerminalUi::redrawIfResized);
    resizeTimer_.start();
    escapeTimer_.setSingleShot(true);
    escapeTimer_.setInterval(35);
    connect(&escapeTimer_, &QTimer::timeout, this, &TerminalUi::flushEscape);
    status_ = QStringLiteral("Tab changes focus; arrows navigate; Enter selects or sends; Ctrl-W closes the active chat.");
    redraw();
}

TerminalUi::~TerminalUi()
{
    ChatTransport::setFactory({});
    restoreTerminal();
}

void TerminalUi::applyNetworkMode()
{
    const QSettings settings;
    const auto centralHost = settings.value(QStringLiteral("network/centralHost")).toString().trimmed();
    const auto listenPort = quint16(settings.value(QStringLiteral("network/listenPort"), DefaultPort).toUInt());
    const auto password = settings.value(QStringLiteral("network/centralPassword")).toString();
    QString modeKey;
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
        central_.start(centralHost, nickname(), listenPort, password);
        modeKey = QStringLiteral("%1\n%2\n%3\n%4").arg(centralHost, nickname()).arg(listenPort).arg(password);
    }
    const bool modeChanged = networkModeApplied_ && modeKey != networkModeKey_;
    networkModeKey_ = modeKey;
    networkModeApplied_ = true;
    if (!modeChanged) return;
    // Open conversations move to the new route right away.
    for (const auto &conversation : std::as_const(conversations_)) {
        if (conversation.disconnected) continue;
        auto *session = conversation.session;
        const auto address = session->remoteHost().isEmpty() ? session->peerAddress() : session->remoteHost();
        if (!address.isEmpty()) {
            session->reconnectToHost(address, session->remotePort() ? session->remotePort() : DefaultPort);
        }
    }
}

void TerminalUi::reportCentralState()
{
    if (!central_.isActive()) {
        if (!central_.lastError().isEmpty()) setStatus(central_.lastError());
        return;
    }
    if (central_.isReady()) {
        setStatus(QStringLiteral("Connected to central host %1.").arg(central_.hostSpec()));
    } else if (!central_.lastError().isEmpty()) {
        setStatus(QStringLiteral("Not connected to central host %1: %2")
                      .arg(central_.hostSpec(), central_.lastError()));
    } else {
        setStatus(QStringLiteral("Connecting to central host %1...").arg(central_.hostSpec()));
    }
}

void TerminalUi::activateFromAnotherLaunch()
{
    setStatus(QStringLiteral("LANTalk is already running in another terminal."));
}

void TerminalUi::configureNickname()
{
    QSettings settings;
    if (!settings.value(QStringLiteral("profile/nickname")).toString().trimmed().isEmpty()) {
        return;
    }
#ifdef Q_OS_UNIX
    if (isatty(STDIN_FILENO)) {
        terminalOutput << "Choose a username: " << Qt::flush;
        const QString entered = escapeProtocolField(terminalInput.readLine());
        if (!entered.isEmpty()) {
            settings.setValue(QStringLiteral("profile/nickname"), entered);
            return;
        }
    }
#endif
    settings.setValue(QStringLiteral("profile/nickname"), QStringLiteral("Guest"));
}

void TerminalUi::configureTerminal()
{
#ifdef Q_OS_UNIX
    if (!ansi_ || tcgetattr(STDIN_FILENO, &terminalMode_) != 0) {
        ansi_ = false;
        return;
    }
    termios raw = terminalMode_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        ansi_ = false;
        return;
    }
    rawMode_ = true;
    // Bracketed paste lets a multi-line paste arrive as one quote instead of
    // one message per line.
    terminalOutput << "\x1b[?1049h\x1b[?25l\x1b[?2004h" << Qt::flush;
#endif
}

void TerminalUi::restoreTerminal()
{
#ifdef Q_OS_UNIX
    if (rawMode_) {
        tcsetattr(STDIN_FILENO, TCSANOW, &terminalMode_);
        rawMode_ = false;
    }
    if (ansi_) {
        terminalOutput << "\x1b[?2004l\x1b[?25h\x1b[?1049l" << Qt::flush;
    }
#endif
}

void TerminalUi::startListening()
{
    if (server_.isListening()) return;
    const quint16 port = QSettings().value(QStringLiteral("network/listenPort"), DefaultPort).toUInt();
    if (!server_.listen(QHostAddress::Any, port)) {
        print(QStringLiteral("Cannot listen on port %1: %2").arg(port).arg(server_.errorString()));
        return;
    }
    print(QStringLiteral("Listening on TCP port %1").arg(server_.serverPort()));
}

void TerminalUi::startDiscovery()
{
    if (discoverySocket_.state() == QAbstractSocket::BoundState) return;
    if (!discoverySocket_.bind(QHostAddress::AnyIPv4, DiscoveryPort,
                               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        print(QStringLiteral("LAN discovery unavailable: %1").arg(discoverySocket_.errorString()));
        return;
    }
    if (discoverySetUp_) {
        discoveryTimer_.start();
        subnetScanTimer_.start();
        scanPrivateSubnets();
        return;
    }
    discoverySetUp_ = true;
    connect(&discoverySocket_, &QUdpSocket::readyRead, this, &TerminalUi::readDiscoveryDatagrams);
    discoveryTimer_.setInterval(2000);
    connect(&discoveryTimer_, &QTimer::timeout, this, &TerminalUi::announcePresence);
    discoveryTimer_.start();

    probeSendTimer_.setInterval(10);
    connect(&probeSendTimer_, &QTimer::timeout, this,
            &TerminalUi::sendDiscoveryProbeBatch);
    subnetScanTimer_.setInterval(5000);
    connect(&subnetScanTimer_, &QTimer::timeout, this,
            &TerminalUi::scanPrivateSubnets);
    subnetScanTimer_.start();

    announcePresence();
    scanPrivateSubnets();
}

void TerminalUi::stopDiscovery()
{
    discoveryTimer_.stop();
    subnetScanTimer_.stop();
    probeSendTimer_.stop();
    pendingDiscoveryProbes_.clear();
    discoverySocket_.close();
    discoveredHosts_.clear();
}

void TerminalUi::announcePresence()
{
    if (discoverySocket_.state() != QAbstractSocket::BoundState) return;
    const auto encodedNickname = nickname().toUtf8().toBase64(QByteArray::Base64UrlEncoding);
    const quint16 port = QSettings().value(QStringLiteral("network/listenPort"), DefaultPort).toUInt();
    const QByteArray announcement = QStringLiteral("LANCHAT_DISCOVER %1 %2 %3")
                                        .arg(discoveryId_, QString::fromLatin1(encodedNickname))
                                        .arg(port)
                                        .toUtf8();
    discoverySocket_.writeDatagram(announcement, QHostAddress::Broadcast, DiscoveryPort);
    discoverySocket_.writeDatagram(announcement, QHostAddress::LocalHost, DiscoveryPort);

    const auto now = QDateTime::currentDateTimeUtc();
    for (auto host = discoveredHosts_.begin(); host != discoveredHosts_.end();) {
        host = host->lastSeen.msecsTo(now) > discoveredHostExpiryMs_
                   ? discoveredHosts_.erase(host)
                   : std::next(host);
    }
    if (ansi_) {
        redraw();
    }
}

void TerminalUi::readDiscoveryDatagrams()
{
    while (discoverySocket_.hasPendingDatagrams()) {
        QByteArray data(int(discoverySocket_.pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress address;
        quint16 senderPort = 0;
        discoverySocket_.readDatagram(data.data(), data.size(), &address, &senderPort);
        const auto parts = QString::fromUtf8(data).trimmed().split(u' ', Qt::KeepEmptyParts);
        if (parts.size() == 2 && parts.at(0) == QStringLiteral("LANCHAT_QUERY")
            && !parts.at(1).isEmpty() && parts.at(1).size() <= 64
            && senderPort == DiscoveryPort
            && PeerDiscovery::isPrivateIPv4Address(address)
            && !PeerDiscovery::isLocalIPv4Address(address)) {
            const auto encodedNickname = nickname().toUtf8().toBase64(QByteArray::Base64UrlEncoding);
            const quint16 port = QSettings().value(QStringLiteral("network/listenPort"), DefaultPort).toUInt();
            const auto response = QStringLiteral("LANCHAT_DISCOVER %1 %2 %3")
                                      .arg(discoveryId_, QString::fromLatin1(encodedNickname))
                                      .arg(port)
                                      .toUtf8();
            discoverySocket_.writeDatagram(response, address, senderPort);
            continue;
        }
        if (parts.size() != 4 || parts.at(0) != QStringLiteral("LANCHAT_DISCOVER")
            || parts.at(1) == discoveryId_ || PeerDiscovery::isLocalIPv4Address(address)) {
            continue;
        }
        bool validPort = false;
        const quint16 port = parts.at(3).toUShort(&validPort);
        const auto decodedName = QByteArray::fromBase64(parts.at(2).toLatin1(),
                                                        QByteArray::Base64UrlEncoding);
        if (!validPort || port == 0 || decodedName.isEmpty()) {
            continue;
        }
        discoveredHosts_.insert(QStringLiteral("%1:%2").arg(address.toString()).arg(port),
                                {QString::fromUtf8(decodedName), address, port,
                                 QDateTime::currentDateTimeUtc()});
    }
    if (ansi_) {
        redraw();
    }
}

void TerminalUi::scanPrivateSubnets()
{
    if (!pendingDiscoveryProbes_.isEmpty()) return;

    const auto targets = PeerDiscovery::privateSubnetHosts();
    discoveredHostExpiryMs_ = PeerDiscovery::peerExpiryMilliseconds(targets.size());
    if (targets.isEmpty()) return;

    for (const auto &target : targets) pendingDiscoveryProbes_.enqueue(target);
    if (!probeSendTimer_.isActive()) probeSendTimer_.start();
}

void TerminalUi::sendDiscoveryProbeBatch()
{
    const auto query = QStringLiteral("LANCHAT_QUERY %1").arg(discoveryId_).toUtf8();
    for (int sent = 0; sent < 20 && !pendingDiscoveryProbes_.isEmpty(); ++sent) {
        discoverySocket_.writeDatagram(query, pendingDiscoveryProbes_.dequeue(), DiscoveryPort);
    }
    if (pendingDiscoveryProbes_.isEmpty()) probeSendTimer_.stop();
}

void TerminalUi::handleInput()
{
#ifdef Q_OS_UNIX
    if (!ansi_) {
        const QString line = terminalInput.readLine();
        if (line.isNull()) {
            QCoreApplication::quit();
            return;
        }
        const QString text = line.trimmed();
        if (text.startsWith(u'/')) {
            handleCommand(text);
        } else if (!text.isEmpty()) {
            if (currentConversationDisconnected()) {
                setStatus(ReconnectHint);
            } else {
                sendChatMessage(text);
            }
        }
        printPrompt();
        return;
    }

    char bytes[128];
    const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
    if (count <= 0) {
        return;
    }
    inputBytes_.append(QString::fromLatin1(bytes, int(count)));
    while (!inputBytes_.isEmpty()) {
        if (inputBytes_ == QStringLiteral("\x1b")) {
            escapeTimer_.start();
            break;
        }
        escapeTimer_.stop();
        if (inputBytes_.startsWith(PasteStart)) {
            const int end = inputBytes_.indexOf(PasteEnd);
            if (end < 0) break; // the rest of the paste is still coming
            handlePaste(QString::fromUtf8(inputBytes_.mid(PasteStart.size(), end - PasteStart.size()).toLatin1()));
            inputBytes_.remove(0, end + PasteEnd.size());
            continue;
        }
        if (inputBytes_.size() < PasteStart.size() && PasteStart.startsWith(inputBytes_)) {
            break; // may be the start of a paste
        }
        if (inputBytes_.startsWith(QStringLiteral("\x1b["))) {
            if (inputBytes_.size() < 3) {
                break;
            }
            const QChar code = inputBytes_.at(2);
            if (code == u'A' || code == u'B' || code == u'C' || code == u'D') {
                handleKey(QByteArray("\x1b[") + code.toLatin1());
                inputBytes_.remove(0, 3);
                continue;
            }
        }
        if (inputBytes_.startsWith(u'\x1b') && inputBytes_.size() >= 2) {
            const QChar altKey = inputBytes_.at(1).toLower();
            if (altKey == u'c' || altKey == u's') {
                handleKey(QByteArray("\x1b") + altKey.toLatin1());
                inputBytes_.remove(0, 2);
                continue;
            }
        }
        const QChar key = inputBytes_.at(0);
        inputBytes_.remove(0, 1);
        handleKey(QString(key).toLatin1());
    }
#endif
}

void TerminalUi::flushEscape()
{
    if (inputBytes_ == QStringLiteral("\x1b")) {
        inputBytes_.clear();
        handleKey(QByteArray("\x1b"));
    }
}

void TerminalUi::handleKey(const QByteArray &key)
{
    if (key == QByteArray("\x1b") + 'c' || key == QByteArray("\x1b") + 's') {
        openTopMenu(key == QByteArray("\x1b") + 'c' ? Menu::Chat : Menu::Settings);
    } else if (dialog_ != Dialog::None) {
        if (key == "\x1b") {
            closeDialog();
        } else if (key == "\r" || key == "\n") {
            acceptDialog();
        } else if (key == "\x7f" || key == "\b") {
            dialogInput_.chop(1);
        } else if (key.size() == 1 && key.at(0) >= ' ') {
            dialogInput_.append(QString::fromUtf8(key));
        }
    } else if (focus_ == Focus::Menu && key.size() == 1 && QChar(key.at(0)).isLetter()) {
        // Accelerator letters open a menu, or pick an item in the open one.
        const QChar letter = QChar(key.at(0)).toLower();
        if (openMenu_ == Menu::None) {
            for (int menu = 0; menu < menuTitles().size(); ++menu) {
                if (accelerator(menuTitles().at(menu)) == letter)
                    openTopMenu(menu == 0 ? Menu::Chat : Menu::Settings);
            }
        } else {
            const auto entries = menuEntries(openMenu_ == Menu::Chat ? 0 : 1);
            for (int item = 0; item < entries.size(); ++item) {
                if (accelerator(entries.at(item).label) == letter) {
                    selectedAction_ = item;
                    activateMenuItem();
                    break;
                }
            }
        }
    } else if (key == "\x1b") {
        openMenu_ = Menu::None;
        focus_ = Focus::Chat;
        status_ = QStringLiteral("Menu closed.");
    } else if (key == "\t") {
        if (focus_ == Focus::Hosts) {
            focus_ = Focus::Chat;
        } else if (focus_ == Focus::Chat) {
            focus_ = Focus::Conversations;
            selectedConversation_ = currentConversation_ >= 0
                                        ? currentConversation_
                                        : 0;
        } else if (focus_ == Focus::Conversations) {
            focus_ = Focus::Menu;
        } else {
            focus_ = Focus::Hosts;
            openMenu_ = Menu::None;
        }
    } else if (key == "\x1b[A" || key == "\x1b[B") {
        const int delta = key == "\x1b[A" ? -1 : 1;
        if (focus_ == Focus::Hosts) {
            const int count = sortedHosts().size();
            if (count > 0) selectedHost_ = std::clamp(selectedHost_ + delta, 0, count - 1);
        } else if (focus_ == Focus::Conversations) {
            if (!conversations_.isEmpty()) {
                if (selectedConversation_ < 0) {
                    selectedConversation_ = currentConversation_ >= 0
                                                ? currentConversation_
                                                : 0;
                }
                selectedConversation_ =
                    (selectedConversation_ + delta + conversations_.size()) %
                    conversations_.size();
            }
        } else if (focus_ == Focus::Menu) {
            if (openMenu_ != Menu::None) {
                const int actionCount = menuEntries(openMenu_ == Menu::Chat ? 0 : 1).size();
                selectedAction_ = (selectedAction_ + delta + actionCount) % actionCount;
            } else if (delta > 0) {
                openTopMenu(selectedMenu_ == 0 ? Menu::Chat : Menu::Settings);
            }
        } else if (!conversations_.isEmpty()) {
            currentConversation_ = (currentConversation_ + delta + conversations_.size())
                                   % conversations_.size();
            selectedConversation_ = currentConversation_;
            conversations_[currentConversation_].unread = false;
            if (currentConversationDisconnected()) status_ = ReconnectHint;
        }
    } else if (key == "\x1b[C" || key == "\x1b[D") {
        if (focus_ == Focus::Menu) {
            if (openMenu_ != Menu::None) {
                openTopMenu(openMenu_ == Menu::Chat ? Menu::Settings : Menu::Chat);
            } else {
                selectedMenu_ = selectedMenu_ == 0 ? 1 : 0;
            }
        } else if (focus_ == Focus::Conversations && !conversations_.isEmpty()) {
            if (selectedConversation_ < 0) {
                selectedConversation_ = currentConversation_ >= 0
                                            ? currentConversation_
                                            : 0;
            }
            const int delta = key == "\x1b[C" ? 1 : -1;
            selectedConversation_ =
                (selectedConversation_ + delta + conversations_.size()) %
                conversations_.size();
        } else if (focus_ == Focus::Chat && !conversations_.isEmpty()) {
            const int delta = key == "\x1b[C" ? 1 : -1;
            currentConversation_ = (currentConversation_ + delta + conversations_.size())
                                   % conversations_.size();
            selectedConversation_ = currentConversation_;
            conversations_[currentConversation_].unread = false;
            if (currentConversationDisconnected()) status_ = ReconnectHint;
        }
    } else if (key == "\r" || key == "\n") {
        if (focus_ == Focus::Hosts) {
            const auto hosts = sortedHosts();
            if (selectedHost_ >= 0 && selectedHost_ < hosts.size()) {
                const auto &host = hosts.at(selectedHost_);
                connectToAddress(host.address.toString(), host.port, host.nickname);
            } else {
                status_ = QStringLiteral("No discovered host selected.");
            }
        } else if (focus_ == Focus::Conversations) {
            if (selectedConversation_ >= 0 &&
                selectedConversation_ < conversations_.size()) {
                currentConversation_ = selectedConversation_;
                conversations_[currentConversation_].unread = false;
                if (currentConversationDisconnected()) status_ = ReconnectHint;
                focus_ = Focus::Chat;
                status_ = QStringLiteral("Active tab: %1")
                              .arg(conversationTitle(
                                  conversations_.at(currentConversation_)));
            } else {
                status_ = QStringLiteral("No conversations to select.");
            }
        } else if (focus_ == Focus::Menu) {
            if (openMenu_ != Menu::None) activateMenuItem();
            else openTopMenu(selectedMenu_ == 0 ? Menu::Chat : Menu::Settings);
        } else {
            const QString text = input_.trimmed();
            const bool hasMessage = !text.isEmpty() || !pendingQuotes_.isEmpty();
            if (text.startsWith(u'/')) {
                input_.clear();
                handleCommand(text);
            } else if (hasMessage && currentConversationDisconnected()) {
                // Keep the draft; commands such as /connect still work.
                status_ = ReconnectHint;
            } else if (hasMessage && currentConversation_ >= 0
                       && currentConversation_ < conversations_.size()) {
                const auto message = composeDraft(text);
                input_.clear();
                pendingQuotes_.clear();
                sendChatMessage(message);
            } else if (!text.isEmpty()) {
                input_.clear();
                status_ = QStringLiteral("Select a host and press Enter to connect.");
            }
        }
    } else if (key == "\x7f" || key == "\b") {
        if (focus_ == Focus::Chat && !input_.isEmpty()) {
            input_.chop(1);
        } else if (focus_ == Focus::Chat && !pendingQuotes_.isEmpty()) {
            pendingQuotes_.removeLast();
            status_ = QStringLiteral("Quote removed.");
        }
    } else if (key == "\x17") {
        if (currentConversation_ >= 0 &&
            currentConversation_ < conversations_.size()) {
            closeConversation(currentConversation_);
        } else {
            status_ = QStringLiteral("No chat tab to close.");
        }
    } else if (key == "\x03") {
        QCoreApplication::quit();
        return;
    } else if (focus_ == Focus::Chat && key.size() == 1 && key.at(0) >= ' ') {
        input_.append(QString::fromUtf8(key));
    }
    syncLocalTyping();
    redraw();
}

void TerminalUi::handleCommand(const QString &command)
{
    const auto parts = command.split(u' ', Qt::SkipEmptyParts);
    const QString name = parts.value(0).toLower();
    if (name == QStringLiteral("/help")) {
        print(QStringLiteral("/hosts, /connect <host|number> [port], /tabs, /use <tab>, /close [tab], "
                             "/nick <name>, /translate <my language> | <peer language>, "
                             "/stoptranslate, /quote-translate [n|all], /unquote [n|all], /quit. "
                             "In the full-screen UI use Tab and arrows; Ctrl-W closes the active chat. "
                             "Multi-line pastes become quotes, sent with the next message and not translated."));
    } else if (name == QStringLiteral("/hosts")) {
        focus_ = Focus::Hosts;
        printHosts();
    } else if (name == QStringLiteral("/connect")) {
        if (parts.size() < 2 || parts.size() > 3) {
            print(QStringLiteral("Usage: /connect <host> [port]"));
            return;
        }
        bool validPort = true;
        const quint16 port = parts.size() == 3 ? parts.at(2).toUShort(&validPort) : DefaultPort;
        if (!validPort || port == 0) {
            print(QStringLiteral("Port must be between 1 and 65535."));
            return;
        }
        bool hostNumber = false;
        const int number = parts.at(1).toInt(&hostNumber);
        if (hostNumber && parts.size() == 2) {
            const auto hosts = sortedHosts();
            if (number < 1 || number > hosts.size()) {
                print(QStringLiteral("No discovered host numbered %1.").arg(number));
                return;
            }
            const auto &host = hosts.at(number - 1);
            connectToAddress(host.address.toString(), host.port, host.nickname);
        } else {
            connectToAddress(parts.at(1), port);
        }
    } else if (name == QStringLiteral("/tabs")) {
        printConversations();
    } else if (name == QStringLiteral("/use")) {
        bool validIndex = false;
        const int index = parts.value(1).toInt(&validIndex) - 1;
        if (!validIndex || index < 0 || index >= conversations_.size()) {
            print(QStringLiteral("Usage: /use <tab number>"));
            return;
        }
        currentConversation_ = index;
        selectedConversation_ = index;
        conversations_[index].unread = false;
        if (currentConversationDisconnected()) status_ = ReconnectHint;
        focus_ = Focus::Chat;
        print(QStringLiteral("Active tab: %1").arg(conversationTitle(conversations_.at(index))));
    } else if (name == QStringLiteral("/close")) {
        int index = currentConversation_;
        if (parts.size() == 2) {
            bool validIndex = false;
            index = parts.at(1).toInt(&validIndex) - 1;
            if (!validIndex) index = -1;
        }
        closeConversation(index);
    } else if (name == QStringLiteral("/nick")) {
        const QString newNickname = escapeProtocolField(command.mid(name.size()).trimmed());
        if (newNickname.isEmpty()) {
            print(QStringLiteral("Usage: /nick <name>"));
            return;
        }
        QSettings().setValue(QStringLiteral("profile/nickname"), newNickname);
        announcePresence();
        applyNetworkMode();
        print(QStringLiteral("Nickname changed to %1. New connections will use it.").arg(newNickname));
    } else if (name == QStringLiteral("/translate")) {
        if (currentConversation_ < 0 || currentConversation_ >= conversations_.size()) {
            setStatus(QStringLiteral("Open a conversation before enabling Live Translate."));
            return;
        }
        if (!isOllamaConfigured()) {
            setStatus(QStringLiteral("Configure an Ollama URL and model in Settings first."));
            return;
        }
        const auto languagePair = command.mid(name.size()).trimmed().split(u'|');
        if (languagePair.size() != 2) {
            setStatus(QStringLiteral("Usage: /translate <my language> | <peer language>"));
            return;
        }
        const auto myLanguage = languagePair.at(0).trimmed();
        const auto peerLanguage = languagePair.at(1).trimmed();
        const auto availableLanguages = languages();
        if (!availableLanguages.contains(myLanguage) || !availableLanguages.contains(peerLanguage)) {
            setStatus(QStringLiteral("Languages: %1").arg(availableLanguages.join(QStringLiteral(", "))));
            return;
        }
        auto &conversation = conversations_[currentConversation_];
        conversation.myLanguage = myLanguage;
        conversation.peerLanguage = peerLanguage;
        conversation.translation->request(myLanguage, peerLanguage);
        setStatus(QStringLiteral("Requesting Live Translate…"));
    } else if (name == QStringLiteral("/quote-translate") || name == QStringLiteral("/unquote")) {
        if (pendingQuotes_.isEmpty()) {
            setStatus(QStringLiteral("There are no pending quotes; paste text to add one."));
            return;
        }
        QList<int> targets;
        const auto which = parts.value(1, QStringLiteral("all"));
        if (which.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0) {
            for (int quote = 0; quote < pendingQuotes_.size(); ++quote) targets << quote;
        } else {
            bool valid = false;
            const int number = which.toInt(&valid);
            if (!valid || number < 1 || number > pendingQuotes_.size()) {
                setStatus(QStringLiteral("Usage: %1 [1-%2|all]").arg(name).arg(pendingQuotes_.size()));
                return;
            }
            targets << number - 1;
        }
        if (name == QStringLiteral("/unquote")) {
            for (auto quote = targets.crbegin(); quote != targets.crend(); ++quote) pendingQuotes_.removeAt(*quote);
            syncLocalTyping();
            setStatus(QStringLiteral("Removed %1 quote(s).").arg(targets.size()));
            return;
        }
        for (const int quote : targets) pendingQuotes_[quote].translate = !pendingQuotes_.at(quote).translate;
        const bool translate = pendingQuotes_.at(targets.first()).translate;
        setStatus(translate ? QStringLiteral("Live Translate will translate the quote(s) too.")
                            : QStringLiteral("The quote(s) will be sent as they are."));
    } else if (name == QStringLiteral("/stoptranslate")) {
        if (currentConversation_ < 0 || currentConversation_ >= conversations_.size()) {
            setStatus(QStringLiteral("There is no active conversation."));
            return;
        }
        auto &conversation = conversations_[currentConversation_];
        conversation.liveTranslate = false;
        conversation.translation->disable();
        setStatus(QStringLiteral("Live Translate is off."));
    } else if (name == QStringLiteral("/quit") || name == QStringLiteral("/exit")) {
        QCoreApplication::quit();
    } else {
        print(QStringLiteral("Unknown command. Type /help for available commands."));
    }
}

void TerminalUi::openTopMenu(Menu menu)
{
    openMenu_ = menu;
    selectedMenu_ = menu == Menu::Chat ? 0 : 1;
    selectedAction_ = 0;
    focus_ = Focus::Menu;
    status_ = menu == Menu::Chat ? QStringLiteral("Chat menu open.") : QStringLiteral("Settings menu open.");
}

void TerminalUi::activateMenuItem()
{
    if (openMenu_ == Menu::Chat) {
        if (selectedAction_ == 0) {
            openDialog(Dialog::Connect, QStringLiteral("Connect To"),
                       QStringLiteral("Host[:port]"), QString());
        } else if (selectedAction_ == 1) {
            if (currentConversation_ < 0) {
                status_ = QStringLiteral("There is no chat to close.");
            } else {
                openDialog(Dialog::CloseChat, QStringLiteral("Close Chat"),
                           QStringLiteral("Close the active chat? (y/n)"), QString());
            }
        } else {
            openDialog(Dialog::Quit, QStringLiteral("Quit LANTalk"),
                       QStringLiteral("Quit LANTalk? (y/n)"), QString());
        }
    } else if (openMenu_ == Menu::Settings) {
        if (selectedAction_ == 0) {
            openDialog(Dialog::Nickname, QStringLiteral("Nickname"),
                       QStringLiteral("Display name"), nickname());
        } else if (selectedAction_ == 1) {
            openDialog(Dialog::OllamaUrl, QStringLiteral("Ollama Settings"),
                       QStringLiteral("Server URL"), QSettings().value(QStringLiteral("ollama/url")).toString());
        } else if (selectedAction_ == 2) {
            openDialog(Dialog::ListenPort, QStringLiteral("Listening Port"),
                       QStringLiteral("TCP port (1-65535)"),
                       QString::number(server_.serverPort() ? server_.serverPort()
                                                            : QSettings().value(QStringLiteral("network/listenPort"), DefaultPort).toUInt()));
        } else if (selectedAction_ == 3) {
            openDialog(Dialog::CentralHost, QStringLiteral("Central Host"),
                       QStringLiteral("host[:port], empty for LAN"),
                       QSettings().value(QStringLiteral("network/centralHost")).toString());
        } else {
            openMenu_ = Menu::None;
            focus_ = Focus::Chat;
            status_ = QStringLiteral("Settings menu closed.");
        }
    }
}

void TerminalUi::openDialog(Dialog dialog, const QString &title, const QString &prompt,
                            const QString &value)
{
    dialog_ = dialog;
    dialogTitle_ = title;
    dialogPrompt_ = prompt;
    dialogInput_ = value;
    openMenu_ = Menu::None;
}

void TerminalUi::closeDialog()
{
    dialog_ = Dialog::None;
    dialogInput_.clear();
    status_ = QStringLiteral("Cancelled.");
}

void TerminalUi::acceptDialog()
{
    const QString value = dialogInput_.trimmed();
    const Dialog dialog = dialog_;
    if (dialog == Dialog::CloseChat || dialog == Dialog::Quit) {
        if (value.compare(QStringLiteral("y"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0) {
            dialog_ = Dialog::None;
            if (dialog == Dialog::CloseChat) closeConversation(currentConversation_);
            else QCoreApplication::quit();
        } else {
            closeDialog();
        }
        return;
    }
    if (dialog == Dialog::Connect) {
        QString address = value;
        quint16 port = DefaultPort;
        const int separator = value.lastIndexOf(u':');
        if (separator > 0) {
            bool validPort = false;
            port = value.mid(separator + 1).toUShort(&validPort);
            address = value.left(separator);
            if (!validPort || port == 0) {
                status_ = QStringLiteral("Enter a valid host and port.");
                return;
            }
        }
        if (address.isEmpty()) {
            status_ = QStringLiteral("Enter a host name or address.");
            return;
        }
        dialog_ = Dialog::None;
        connectToAddress(address, port);
        return;
    }
    if (dialog == Dialog::Nickname) {
        const QString name = escapeProtocolField(value);
        if (name.isEmpty()) {
            status_ = QStringLiteral("Nickname cannot be empty.");
            return;
        }
        QSettings().setValue(QStringLiteral("profile/nickname"), name);
        announcePresence();
        applyNetworkMode();
        status_ = QStringLiteral("Nickname changed to %1.").arg(name);
    } else if (dialog == Dialog::OllamaUrl) {
        QSettings().setValue(QStringLiteral("ollama/url"), value);
        openDialog(Dialog::OllamaModel, QStringLiteral("Ollama Settings"),
                   QStringLiteral("Model name"), QSettings().value(QStringLiteral("ollama/model")).toString());
        return;
    } else if (dialog == Dialog::OllamaModel) {
        QSettings().setValue(QStringLiteral("ollama/model"), value);
        status_ = QStringLiteral("Ollama settings saved.");
        for (const auto &conversation : std::as_const(conversations_)) conversation.translation->retry();
    } else if (dialog == Dialog::ListenPort) {
        bool validPort = false;
        const quint16 port = value.toUShort(&validPort);
        if (!validPort || port == 0) {
            status_ = QStringLiteral("Port must be between 1 and 65535.");
            return;
        }
        setListenPort(port);
    } else if (dialog == Dialog::CentralHost) {
        if (!value.isEmpty() && !CentralProtocol::parseHostSpec(value, nullptr, nullptr)) {
            status_ = QStringLiteral("Enter a host name or address, optionally with :port.");
            return;
        }
        QSettings().setValue(QStringLiteral("network/centralHost"), value);
        if (value.isEmpty()) {
            dialog_ = Dialog::None;
            applyNetworkMode();
            status_ = QStringLiteral("Central host removed; chatting directly on the LAN.");
            return;
        }
        // The password is never shown, so the field starts empty; leaving it
        // empty keeps the saved one.
        openDialog(Dialog::CentralPassword, QStringLiteral("Central Host"),
                   QStringLiteral("Password (Enter keeps it, - for none)"), QString());
        return;
    } else if (dialog == Dialog::CentralPassword) {
        // Passwords may start or end with spaces, so use the raw input.
        const QString password = dialogInput_;
        if (password == QStringLiteral("-")) {
            QSettings().remove(QStringLiteral("network/centralPassword"));
        } else if (!password.isEmpty()) {
            QSettings().setValue(QStringLiteral("network/centralPassword"), password);
        }
        dialog_ = Dialog::None;
        dialogInput_.clear();
        applyNetworkMode();
        return;
    }
    dialog_ = Dialog::None;
}

void TerminalUi::setListenPort(quint16 port)
{
    if (central_.isActive()) {
        // Nothing listens while a central host relays the conversations.
        QSettings().setValue(QStringLiteral("network/listenPort"), port);
        applyNetworkMode();
        status_ = QStringLiteral("Listening port set to %1.").arg(port);
        return;
    }
    if (server_.serverPort() != port) {
        const quint16 previousPort = server_.serverPort();
        QTcpServer probe;
        if (!probe.listen(QHostAddress::Any, port)) {
            status_ = QStringLiteral("Cannot listen on port %1: %2").arg(port).arg(probe.errorString());
            return;
        }
        probe.close();
        server_.close();
        if (!server_.listen(QHostAddress::Any, port)) {
            if (previousPort != 0) server_.listen(QHostAddress::Any, previousPort);
            status_ = QStringLiteral("Cannot listen on port %1: %2").arg(port).arg(server_.errorString());
            return;
        }
    }
    QSettings().setValue(QStringLiteral("network/listenPort"), port);
    announcePresence();
    status_ = QStringLiteral("Listening on TCP port %1.").arg(port);
}

void TerminalUi::connectToAddress(const QString &address, quint16 port, const QString &nickname)
{
    if (auto *existing = ConnectionRegistry::shared()->findSession(address, port, nickname)) {
        const int index = conversationIndex(existing);
        if (index < 0) {
            setStatus(QStringLiteral("A connection to %1 is already being established.").arg(address));
            return;
        }
        currentConversation_ = index;
        selectedConversation_ = index;
        conversations_[index].unread = false;
        focus_ = Focus::Chat;
        if (conversations_.at(index).disconnected) {
            existing->reconnectToHost(address, port);
            setStatus(QStringLiteral("Reconnecting to %1 in tab %2.").arg(address).arg(index + 1));
        } else {
            setStatus(QStringLiteral("Already connected to %1; switched to tab %2.")
                          .arg(conversationTitle(conversations_.at(index)))
                          .arg(index + 1));
        }
        syncLocalTyping();
        return;
    }
    auto *session = new ChatSession(ChatTransport::create(this), this);
    addSession(session, QStringLiteral("Connecting to %1:%2").arg(address).arg(port));
    session->connectToHost(address, port, nickname);
}

void TerminalUi::acceptConnection()
{
    while (server_.hasPendingConnections()) {
        adoptIncomingSession(new ChatSession(server_.nextPendingConnection(), this), 0);
    }
}

void TerminalUi::acceptRelayConnection(RelayTransport *transport)
{
    adoptIncomingSession(new ChatSession(transport, this), transport->peerPort());
}

void TerminalUi::adoptIncomingSession(ChatSession *session, quint16 port)
{
    // Incoming conversations get a tab once the handshake shows they do not
    // duplicate an open one; duplicates are merged into that tab.
    session->setRemoteEndpoint(session->peerAddress(), port);
    connect(session, &ChatSession::peerIdentified, this, [this, session] {
        if (conversationIndex(session) >= 0) return;
        addSession(session, QStringLiteral("Incoming connection"));
        conversationIdentified(session);
    });
    connect(session, &ChatSession::mergedInto, this, [this, session] {
        if (conversationIndex(session) < 0) session->deleteLater();
    });
    connect(session, &ChatSession::disconnected, this, [this, session] {
        if (conversationIndex(session) < 0) session->deleteLater();
    });
    session->beginHandshake(nickname());
}

void TerminalUi::addSession(ChatSession *session, const QString &title)
{
    Conversation conversation;
    conversation.session = session;
    conversation.translation = new TranslationRestorer(session, session);
    conversation.title = title;
    conversation.transcript.append(
        {QStringLiteral("* %1").arg(title), false, false});
    conversations_.append(conversation);
    const int index = conversations_.size() - 1;
    currentConversation_ = index;
    selectedConversation_ = index;
    focus_ = Focus::Chat;
    syncLocalTyping();
    connect(session, &ChatSession::peerIdentified, this,
            [this, session] { conversationIdentified(session); });
    connect(session, &ChatSession::mergedInto, this,
            [this, session](ChatSession *existing) { mergeConversation(session, existing); });
    connect(session, &ChatSession::peerTypingChanged, this, [this] { redraw(); });
    connect(conversation.translation, &TranslationRestorer::resumeRequested, this,
            [this, session](const QString &myLanguage, const QString &peerLanguage) {
                const int index = conversationIndex(session);
                if (index < 0) return;
                conversations_[index].myLanguage = myLanguage;
                conversations_[index].peerLanguage = peerLanguage;
                setStatus(QStringLiteral("Resuming Live Translate…"));
            });
    connect(conversation.translation, &TranslationRestorer::statusChanged, this,
            &TerminalUi::setStatus);
    connect(session, &ChatSession::messageReceived, this,
            [this, session](const QString &message, const QString &) {
                const int index = conversationIndex(session);
                if (index < 0) return;
                const auto timestamp = QDateTime::currentDateTime();
                if (index != currentConversation_) {
                    conversations_[index].unread = true;
                }
                ChatHistory::append(session->peerAddress(), session->peerName(),
                                    session->peerName(), message, timestamp);
                const auto &conversation = conversations_.at(index);
                if (!conversation.liveTranslate) {
                    appendTranscript(index,
                                     ChatHistory::timestampPrefix(timestamp) +
                                         QStringLiteral("%1: %2")
                                             .arg(session->peerName(), message));
                    return;
                }

                const auto myLanguage = conversation.myLanguage;
                const auto peerLanguage = conversation.peerLanguage;
                const QPointer<ChatSession> safeSession(session);
                auto *translator = new MessageTranslator(this);
                ++pendingIncomingTranslations_;
                setStatus(QStringLiteral("Translating incoming message... Stand by."));
                connect(translator, &MessageTranslator::translated, this,
                        [this, safeSession, translator, timestamp](const QString &translation,
                                                                   const QString &) {
                            if (safeSession) {
                                const int conversation = conversationIndex(safeSession);
                                if (conversation >= 0) {
                                    appendTranscript(conversation,
                                                     ChatHistory::timestampPrefix(timestamp) +
                                                         QStringLiteral("%1: %2")
                                                             .arg(safeSession->peerName(), translation));
                                }
                            }
                            --pendingIncomingTranslations_;
                            updateTranslationStatus(safeSession
                                                        ? QStringLiteral("Incoming message translated.")
                                                        : QStringLiteral("Incoming translation finished; conversation closed."));
                            translator->deleteLater();
                        });
                connect(translator, &MessageTranslator::failed, this,
                        [this, safeSession, translator, message, timestamp](const QString &error,
                                                                            const QString &) {
                            if (safeSession) {
                                const int conversation = conversationIndex(safeSession);
                                if (conversation >= 0) {
                                    appendTranscript(conversation,
                                                     ChatHistory::timestampPrefix(timestamp) +
                                                         QStringLiteral("%1: %2")
                                                             .arg(safeSession->peerName(), message));
                                }
                            }
                            --pendingIncomingTranslations_;
                            updateTranslationStatus(error);
                            translator->deleteLater();
                        });
                translator->translate(message, peerLanguage, myLanguage);
            });
    connect(session, &ChatSession::statusChanged, this, [this, session](const QString &message) {
        const int index = conversationIndex(session);
        if (index >= 0) appendTranscript(index, QStringLiteral("* %1").arg(message));
        setStatus(message);
    });
    connect(session, &ChatSession::liveTranslationGranted, this, [this, session] {
        const int index = conversationIndex(session);
        if (index < 0) return;
        auto &conversation = conversations_[index];
        conversation.liveTranslate = true;
        setStatus(QStringLiteral("Live Translate: %1 ↔ %2")
                      .arg(conversation.myLanguage, conversation.peerLanguage));
    });
    connect(session, &ChatSession::liveTranslationDenied, this,
            [this, session](const QString &message) {
                const int index = conversationIndex(session);
                if (index >= 0) conversations_[index].liveTranslate = false;
                setStatus(message);
            });
    connect(session, &ChatSession::peerLiveTranslationChanged, this,
            [this, session](bool active, const QString &sourceLanguage,
                   const QString &targetLanguage) {
                // Read from our side: our language (the peer's target) first.
                setStatus(active
                              ? QStringLiteral("%1 enabled Live Translate (%2 → %3).")
                                    .arg(session->peerName(), targetLanguage,
                                         sourceLanguage)
                              : QStringLiteral("%1 disabled Live Translate.")
                                    .arg(session->peerName()));
            });
    connect(session, &ChatSession::fileReceived, this, [this, session](const QString &name, const QString &path, const QString &) {
        const int index = std::find_if(conversations_.cbegin(), conversations_.cend(),
                                       [session](const Conversation &item) { return item.session == session; })
                          - conversations_.cbegin();
        if (index < conversations_.size()) {
            if (index != currentConversation_) conversations_[index].unread = true;
            appendTranscript(index, QStringLiteral("* %1 sent %2 (%3)").arg(session->peerName(), name, path));
        }
    });
    connect(session, &ChatSession::disconnected, this, [this, session] {
        const int index = conversationIndex(session);
        if (index >= 0) {
            conversations_[index].disconnected = true;
            conversations_[index].liveTranslate = false;
            appendTranscript(index, QStringLiteral("* Disconnected."));
        }
        setStatus(index >= 0 && index == currentConversation_
                      ? QStringLiteral("Disconnected. %1.").arg(ReconnectHint)
                      : QStringLiteral("Disconnected."));
    });
    session->beginHandshake(nickname());
    print(QStringLiteral("Opened tab %1: %2").arg(index + 1).arg(title));
}

void TerminalUi::conversationIdentified(ChatSession *session)
{
    const int index = conversationIndex(session);
    if (index < 0) return;
    conversations_[index].disconnected = false;
    conversations_[index].title = conversationTitle(conversations_[index]);
    loadConversationHistory(index);
    redraw();
}

void TerminalUi::mergeConversation(ChatSession *duplicate, ChatSession *existing)
{
    const int index = conversationIndex(duplicate);
    if (index < 0) return;
    const bool wasCurrent = index == currentConversation_;
    conversations_.removeAt(index);
    duplicate->deleteLater();
    const int existingIndex = conversationIndex(existing);
    if (wasCurrent && existingIndex >= 0) currentConversation_ = existingIndex;
    else if (index < currentConversation_) --currentConversation_;
    currentConversation_ = std::min(currentConversation_, int(conversations_.size()) - 1);
    selectedConversation_ = currentConversation_;
    syncLocalTyping();
    if (wasCurrent && existingIndex >= 0) {
        setStatus(QStringLiteral("Already connected to %1; switched to tab %2.")
                      .arg(conversationTitle(conversations_.at(existingIndex)))
                      .arg(existingIndex + 1));
    } else {
        redraw();
    }
}

void TerminalUi::syncLocalTyping()
{
    // Only drafts of chat messages in a connected conversation count as
    // typing; slash commands do not.
    ChatSession *session = nullptr;
    const auto draft = input_.trimmed();
    const bool composing = (!draft.isEmpty() && !draft.startsWith(u'/')) || !pendingQuotes_.isEmpty();
    if (currentConversation_ >= 0 && currentConversation_ < conversations_.size()
        && !conversations_.at(currentConversation_).disconnected && composing) {
        session = conversations_.at(currentConversation_).session;
    }
    const auto state = input_ + u'\0' + QString::number(pendingQuotes_.size());
    if (typingSession_ && typingSession_ != session) typingSession_->stopLocalTyping();
    if (session && (typingSession_ != session || typingDraft_ != state)) {
        session->noteLocalTyping(true);
    }
    typingSession_ = session;
    typingDraft_ = session ? state : QString();
}

void TerminalUi::handlePaste(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n');
    if (dialog_ != Dialog::None) {
        dialogInput_ += text.simplified();
        redraw();
        return;
    }
    focus_ = Focus::Chat;
    openMenu_ = Menu::None;
    if (!QuotedMessage::shouldQuote(text)) {
        input_ += QString(text).replace(u'\n', u' ');
    } else {
        text.remove(QRegularExpression(QStringLiteral("^(\\s*\\n)+")));
        text.remove(QRegularExpression(QStringLiteral("\\s+$")));
        if (!text.isEmpty()) {
            pendingQuotes_.append({text});
            status_ = QStringLiteral("Quote %1 added (%2 lines). Enter sends it with your message; "
                                     "/quote-translate %1 translates it, /unquote %1 removes it.")
                          .arg(pendingQuotes_.size())
                          .arg(text.count(u'\n') + 1);
        }
    }
    syncLocalTyping();
    redraw();
}

QString TerminalUi::composeDraft(const QString &text) const
{
    QList<QuotedMessage::Segment> segments;
    if (!text.isEmpty()) segments.append({text});
    for (const auto &quote : pendingQuotes_) segments.append({quote.text, true, quote.translate});
    return QuotedMessage::compose(segments, true);
}

void TerminalUi::closeConversation(int index)
{
    if (index < 0 || index >= conversations_.size()) {
        print(QStringLiteral("No such tab."));
        return;
    }
    const auto conversation = conversations_.takeAt(index);
    conversation.session->closeConnection();
    conversation.session->deleteLater();
    if (conversations_.isEmpty()) currentConversation_ = -1;
    else if (currentConversation_ >= conversations_.size()) currentConversation_ = conversations_.size() - 1;
    else if (index < currentConversation_) --currentConversation_;
    selectedConversation_ = currentConversation_;
    if (currentConversation_ >= 0)
        conversations_[currentConversation_].unread = false;
    if (currentConversationDisconnected()) status_ = ReconnectHint;
    syncLocalTyping();
    print(QStringLiteral("Closed tab %1.").arg(index + 1));
}

void TerminalUi::sendChatMessage(const QString &text)
{
    if (currentConversation_ < 0 || currentConversation_ >= conversations_.size()) {
        setStatus(QStringLiteral("No active conversation. Use /hosts then /connect <host>."));
        return;
    }
    auto &conversation = conversations_[currentConversation_];
    if (!conversation.liveTranslate) {
        // Quote translate marks only matter to Live Translate.
        const auto message = QuotedMessage::withoutTranslateFlags(text);
        const auto timestamp = QDateTime::currentDateTime();
        conversation.session->sendMessage(message);
        appendTranscript(currentConversation_,
                         ChatHistory::timestampPrefix(timestamp) +
                             QStringLiteral("You: %1").arg(message));
        ChatHistory::append(conversation.session->peerAddress(),
                            conversation.session->peerName(),
                            QStringLiteral("You"), message, timestamp);
        return;
    }

    const QPointer<ChatSession> session(conversation.session);
    auto *translator = new MessageTranslator(this);
    ++pendingOutgoingTranslations_;
    setStatus(QStringLiteral("Translating message..."));
    connect(translator, &MessageTranslator::translated, this,
            [this, session, translator](const QString &translation,
                                        const QString &originalMessage) {
                if (session) {
                    const int index = conversationIndex(session);
                    if (index >= 0) {
                        const auto timestamp = QDateTime::currentDateTime();
                        session->sendMessage(translation, originalMessage);
                        appendTranscript(index,
                                         ChatHistory::timestampPrefix(timestamp) +
                                             QStringLiteral("You: %1")
                                                 .arg(originalMessage));
                        ChatHistory::append(
                            session->peerAddress(), session->peerName(),
                            QStringLiteral("You"), originalMessage, timestamp);
                    }
                }
                --pendingOutgoingTranslations_;
                updateTranslationStatus(session ? QStringLiteral("Message translated and sent.")
                                                 : QStringLiteral("Translation finished; conversation closed."));
                translator->deleteLater();
            });
    connect(translator, &MessageTranslator::failed, this,
            [this, translator](const QString &error, const QString &) {
                --pendingOutgoingTranslations_;
                updateTranslationStatus(error);
                translator->deleteLater();
            });
    translator->translate(text, conversation.myLanguage, conversation.peerLanguage);
}

void TerminalUi::setStatus(const QString &message)
{
    status_ = sanitize(message).replace(u'\n', QStringLiteral(" | "));
    if (ansi_) {
        redraw();
    } else {
        terminalOutput << "\r\n" << status_ << "\n" << Qt::flush;
    }
}

void TerminalUi::updateTranslationStatus(const QString &idleMessage)
{
    if (pendingIncomingTranslations_ > 0) {
        setStatus(QStringLiteral("Translating incoming message... Stand by."));
    } else if (pendingOutgoingTranslations_ > 0) {
        setStatus(QStringLiteral("Translating message..."));
    } else {
        setStatus(idleMessage);
    }
}

int TerminalUi::conversationIndex(ChatSession *session) const
{
    const auto found = std::find_if(conversations_.cbegin(), conversations_.cend(),
                                    [session](const Conversation &conversation) {
                                        return conversation.session == session;
                                    });
    return found == conversations_.cend() ? -1 : int(found - conversations_.cbegin());
}

bool TerminalUi::currentConversationDisconnected() const
{
    return currentConversation_ >= 0 &&
           currentConversation_ < conversations_.size() &&
           conversations_.at(currentConversation_).disconnected;
}

QList<TerminalUi::DiscoveredHost> TerminalUi::sortedHosts() const
{
    auto hosts = discoveredHosts_.values();
    if (central_.isActive()) {
        // The central host lists the clients connected to it instead.
        hosts.clear();
        for (const auto &peer : central_.peers()) {
            hosts.append({peer.nickname, QHostAddress(peer.address), peer.port, QDateTime::currentDateTimeUtc()});
        }
    }
    std::sort(hosts.begin(), hosts.end(), [](const DiscoveredHost &a, const DiscoveredHost &b) {
        return a.nickname.localeAwareCompare(b.nickname) < 0;
    });
    return hosts;
}

void TerminalUi::appendTranscript(int index, const QString &line)
{
    if (index >= 0 && index < conversations_.size()) {
        auto &lines = conversations_[index].transcript;
        for (const auto &part : transcriptLines(line)) lines.append({sanitize(part), false, false});
        if (lines.size() > 200) lines.remove(0, lines.size() - 200);
    }
    redraw();
}

void TerminalUi::loadConversationHistory(int index)
{
    if (index < 0 || index >= conversations_.size()) return;
    auto &conversation = conversations_[index];
    if (conversation.historyLoaded) return;
    conversation.historyLoaded = true;
    conversation.transcript.clear();

    const auto entries = ChatHistory::load(conversation.session->peerAddress(),
                                           conversation.session->peerName(), 20);
    for (const auto &entry : entries) {
        for (const auto &part : transcriptLines(ChatHistory::timestampPrefix(entry.timestamp)
                                                + QStringLiteral("%1: %2").arg(entry.sender, entry.message))) {
            conversation.transcript.append({sanitize(part), true, false});
        }
    }
    if (!entries.isEmpty()) {
        conversation.transcript.append(
            {QStringLiteral("──────── Earlier messages ────────"), false, true});
    }
}

QString TerminalUi::sanitize(const QString &text) const
{
    QString result;
    for (int index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == u'\x1b') {
            if (index + 1 < text.size() && text.at(index + 1) == u'[') {
                index += 2;
                while (index < text.size()) {
                    const QChar escapeCharacter = text.at(index);
                    if (escapeCharacter >= u'@' && escapeCharacter <= u'~') break;
                    ++index;
                }
            }
            continue;
        }
        result += character.isPrint() ? character : u' ';
    }
    return result;
}

QString TerminalUi::elide(const QString &text, int width) const
{
    if (width <= 0) return {};
    const QString safe = sanitize(text);
    return displayWidth(safe) > width ? safe.left(std::max(0, width - 3)) + QStringLiteral("...") : safe;
}

void TerminalUi::writeLine(const QString &text, int width) const
{
    terminalOutput << elide(text, width) << "\x1b[K\n";
}

QString TerminalUi::boxedLine(const QString &text, int width) const
{
    return QStringLiteral("│ %1 │").arg(elide(text, std::max(0, width - 4)).leftJustified(
        std::max(0, width - 4), u' '));
}

void TerminalUi::drawDialog() const
{
    if (dialog_ == Dialog::None) return;
    const int width = std::min(std::max(32, terminalColumns_ * 2 / 3), terminalColumns_ - 4);
    const int height = 7;
    const int row = std::max(1, (terminalRows_ - height) / 2);
    const int column = std::max(1, (terminalColumns_ - width) / 2);
    const QString topBorder = QStringLiteral("┌%1┐").arg(QString(std::max(1, width - 2), u'─'));
    const QString bottomBorder = QStringLiteral("└%1┘").arg(QString(std::max(1, width - 2), u'─'));
    const auto at = [row, column](int line) {
        terminalOutput << QStringLiteral("\x1b[%1;%2H").arg(row + line).arg(column);
    };
    at(0);
    terminalOutput << "\x1b[1;35m" << topBorder << "\x1b[0m";
    at(1);
    terminalOutput << "\x1b[1;35m" << boxedLine(dialogTitle_, width) << "\x1b[0m";
    at(2);
    terminalOutput << "\x1b[35m" << QStringLiteral("│%1│").arg(QString(width - 2, u' ')) << "\x1b[0m";
    at(3);
    terminalOutput << "\x1b[35m" << boxedLine(dialogPrompt_, width) << "\x1b[0m";
    at(4);
    const auto shownInput = dialog_ == Dialog::CentralPassword
                                ? QString(dialogInput_.size(), u'*')
                                : dialogInput_;
    terminalOutput << "\x1b[1;36m" << boxedLine(QStringLiteral("> %1").arg(shownInput), width) << "\x1b[0m";
    at(5);
    terminalOutput << "\x1b[2;35m" << boxedLine(QStringLiteral("Enter = accept   Esc = cancel"), width) << "\x1b[0m";
    at(6);
    terminalOutput << "\x1b[1;35m" << bottomBorder << "\x1b[0m";
}

void TerminalUi::drawMenu(int column) const
{
    const auto entries = menuEntries(openMenu_ == Menu::Chat ? 0 : 1);
    int labelColumns = 0;
    int shortcutColumns = 0;
    for (const auto &entry : entries) {
        labelColumns = std::max(labelColumns, labelWidth(entry.label));
        shortcutColumns = std::max(shortcutColumns, int(entry.shortcut.size()));
    }
    const int inner = 2 + labelColumns + (shortcutColumns > 0 ? 4 + shortcutColumns : 0);
    const int width = inner + 2;
    const int left = std::max(1, std::min(column, terminalColumns_ - width + 1));
    const auto at = [left](int row) { terminalOutput << QStringLiteral("\x1b[%1;%2H").arg(row).arg(left); };
    // Drawn right under the menu bar, over the conversation area.
    at(2);
    terminalOutput << MenuBoxStyle << QStringLiteral("┌") << QString(inner, u'─') << QStringLiteral("┐");
    for (int item = 0; item < entries.size(); ++item) {
        const auto &entry = entries.at(item);
        const int gap = inner - 2 - labelWidth(entry.label) - int(entry.shortcut.size());
        at(3 + item);
        terminalOutput << MenuBoxStyle << QStringLiteral("│")
                       << (item == selectedAction_ ? MenuHighlightStyle : MenuBoxStyle) << QStringLiteral(" ")
                       << styledLabel(entry.label) << QString(std::max(0, gap), u' ') << entry.shortcut
                       << QStringLiteral(" ") << MenuBoxStyle << QStringLiteral("│");
    }
    at(3 + entries.size());
    terminalOutput << MenuBoxStyle << QStringLiteral("└") << QString(inner, u'─') << QStringLiteral("┘") << "\x1b[0m";
}

void TerminalUi::redraw()
{
    if (!ansi_) return;
#ifdef Q_OS_UNIX
    winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        terminalRows_ = std::max(4, int(size.ws_row));
        terminalColumns_ = std::max(30, int(size.ws_col));
    }
#endif
    if (terminalRows_ == 0) terminalRows_ = 24;
    if (terminalColumns_ == 0) terminalColumns_ = 80;
    const int sidebar = std::min(30, std::max(18, terminalColumns_ / 3));
    const int mainWidth = terminalColumns_ - sidebar - 1;
    const int contentRows = std::max(1, terminalRows_ - 5);
    terminalOutput << "\x1b[H\x1b[2J";
    const QString top = QStringLiteral("┌%1┬%2┐").arg(QString(std::max(0, sidebar - 1), u'─'),
                                                     QString(std::max(0, mainWidth - 1), u'─'));
    const QString bottom = QStringLiteral("└%1┴%2┘").arg(QString(std::max(0, sidebar - 1), u'─'),
                                                        QString(std::max(0, mainWidth - 1), u'─'));
    // MS-DOS Edit style menu bar: blue across the full width, accelerators
    // underlined, and the open or focused menu highlighted.
    QString menuBar = MenuBarStyle + u' ';
    int menuBarWidth = 1;
    int openMenuColumn = 1;
    for (int menu = 0; menu < menuTitles().size(); ++menu) {
        const bool open = openMenu_ != Menu::None && selectedMenu_ == menu;
        const bool focused = openMenu_ == Menu::None && focus_ == Focus::Menu && selectedMenu_ == menu;
        if (open) openMenuColumn = menuBarWidth + 1;
        menuBar += (open || focused ? MenuHighlightStyle : MenuBarStyle) + u' '
                   + styledLabel(menuTitles().at(menu)) + u' ' + MenuBarStyle + u' ';
        menuBarWidth += labelWidth(menuTitles().at(menu)) + 3;
    }
    const QString appTitle = QStringLiteral("LANTalk v%1 ").arg(QCoreApplication::applicationVersion());
    QString menuHint = QStringLiteral("Alt-C / Alt-S   ") + appTitle;
    if (menuBarWidth + displayWidth(menuHint) > terminalColumns_) menuHint = appTitle;
    if (menuBarWidth + displayWidth(menuHint) > terminalColumns_) menuHint.clear();
    menuBar += QString(std::max(0, terminalColumns_ - menuBarWidth - displayWidth(menuHint)), u' ') + menuHint;
    terminalOutput << menuBar << "\x1b[0m\x1b[K\n";
    terminalOutput << "\x1b[1;34m" << top << "\x1b[0m\x1b[K\n";
    const auto hosts = sortedHosts();
    for (int row = 0; row < contentRows; ++row) {
        QString left = QStringLiteral("│");
        QString right;
        QList<QPair<int, int>> activeTabRanges;
        QList<QPair<int, int>> selectedTabRanges;
        QList<QPair<int, int>> unreadTabRanges;
        if (row == 0) left = QStringLiteral("├── HOSTS ON NETWORK %1").arg(
            QString(std::max(0, sidebar - displayWidth(QStringLiteral("├── HOSTS ON NETWORK "))), u'─'));
        else if (row - 1 < hosts.size()) {
            const auto &host = hosts.at(row - 1);
            left = QStringLiteral("│%1 %2").arg(row - 1 == selectedHost_ ? u'>' : u' ').arg(host.nickname);
        } else if (row == hosts.size() + 1 && hosts.isEmpty()) {
            left = central_.isActive() && !central_.isReady() ? QStringLiteral("│ (connecting...)")
                                                              : QStringLiteral("│ (discovering...)");
        }
        if (row == 0) {
            if (conversations_.isEmpty()) {
                right = QStringLiteral("── CHAT");
            } else {
                right = QStringLiteral("── ");
                for (int tab = 0; tab < conversations_.size(); ++tab) {
                    if (tab > 0) right += u' ';
                    const bool active = tab == currentConversation_;
                    const bool selected = focus_ == Focus::Conversations &&
                                          tab == selectedConversation_;
                    const bool unread = conversations_.at(tab).unread;
                    if (unread) {
                        const int unreadStart = right.size();
                        right += u'!';
                        unreadTabRanges.append({unreadStart, right.size()});
                    } else {
                        right += u' ';
                    }
                    right += u' ';
                    if (active) {
                        const int markerStart = right.size();
                        right += u'*';
                        activeTabRanges.append({markerStart, right.size()});
                    } else {
                        right += u' ';
                    }
                    right += u' ';
                    if (selected) {
                        const int markerStart = right.size();
                        right += u'>';
                        selectedTabRanges.append({markerStart, right.size()});
                    } else {
                        right += u' ';
                    }
                    right += u' ';
                    const int titleStart = right.size();
                    right += QStringLiteral("[%1]").arg(
                        conversationTitle(conversations_.at(tab)));
                    const auto titleRange = qMakePair(titleStart, right.size());
                    if (active) activeTabRanges.append(titleRange);
                    if (selected) selectedTabRanges.append(titleRange);
                    if (unread && !selected && !active)
                        unreadTabRanges.append(titleRange);
                }
            }
            right = elide(right, mainWidth - 1);
            right += QString(std::max(0, mainWidth - 1 - displayWidth(right)), u'─');
        } else if (row == 1) {
            if (conversations_.isEmpty()) right = QStringLiteral(" No conversations. Select a host and press Enter.");
            else if (focus_ == Focus::Conversations) {
                right = QStringLiteral(" Use arrow keys to choose a tab, then press Enter.");
            }
        } else if (currentConversation_ >= 0 && currentConversation_ < conversations_.size()) {
            const auto &lines = conversations_.at(currentConversation_).transcript;
            const int first = std::max(0, int(lines.size()) - (contentRows - 3));
            if (row - 2 + first < lines.size())
                right = lines.at(row - 2 + first).text;
        }
        terminalOutput << (focus_ == Focus::Hosts && row > 0 && row - 1 == selectedHost_ ? "\x1b[30;46m" : "\x1b[36m")
                       << elide(left, sidebar) << "\x1b[0m";
        terminalOutput << QString(std::max(0, sidebar - displayWidth(elide(left, sidebar))), u' ')
                       << "\x1b[1;34m" << (row == 0 ? "┬" : "│") << "\x1b[0m";
        if (row == 0) {
            const QString header = elide(right, mainWidth - 1);
            const auto rangeContains = [](const QList<QPair<int, int>> &ranges,
                                          int position) {
                return std::any_of(ranges.cbegin(), ranges.cend(),
                                   [position](const auto &range) {
                                       return position >= range.first &&
                                              position < range.second;
                                   });
            };
            QString styledHeader;
            int previousStyle = -1;
            for (int position = 0; position < header.size(); ++position) {
                const int style = rangeContains(selectedTabRanges, position)
                                      ? 2
                                  : rangeContains(activeTabRanges, position)
                                      ? 1
                                  : rangeContains(unreadTabRanges, position)
                                      ? 3
                                      : 0;
                if (style != previousStyle) {
                    styledHeader += QStringLiteral("\x1b[0m");
                    styledHeader += style == 2   ? QStringLiteral("\x1b[30;43m")
                                    : style == 1 ? QStringLiteral("\x1b[1;96m")
                                    : style == 3 ? QStringLiteral("\x1b[1;91m")
                                                 : QStringLiteral("\x1b[36m");
                    previousStyle = style;
                }
                styledHeader += header.at(position);
            }
            terminalOutput << "\x1b[36m" << styledHeader << "\x1b[0m"
                           << QString(std::max(0, mainWidth - 1 - displayWidth(header)), u' ')
                           << "\x1b[1;34m│\x1b[0m\x1b[K\n";
        } else {
            const Conversation::TranscriptLine *line = nullptr;
            if (row >= 2 && currentConversation_ >= 0 &&
                currentConversation_ < conversations_.size()) {
                const auto &lines =
                    conversations_.at(currentConversation_).transcript;
                const int first = std::max(0, int(lines.size()) - (contentRows - 3));
                const int lineIndex = row - 2 + first;
                if (lineIndex >= 0 && lineIndex < lines.size())
                    line = &lines.at(lineIndex);
            }
            const QString rendered = elide(right, mainWidth - 1);
            terminalOutput << (focus_ == Focus::Chat ? "\x1b[37m" : "");
            if (line && line->historical) terminalOutput << "\x1b[3m";
            if (line && line->separator) terminalOutput << "\x1b[2;4m";
            terminalOutput << rendered << "\x1b[0m"
                           << QString(std::max(0, mainWidth - 1 - displayWidth(rendered)), u' ')
                           << "\x1b[1;34m│\x1b[0m\x1b[K\n";
        }
    }
    const bool peerTyping = currentConversation_ >= 0 && currentConversation_ < conversations_.size()
                            && !conversations_.at(currentConversation_).disconnected
                            && conversations_.at(currentConversation_).session->peerTyping();
    terminalOutput << "\x1b[1;34m" << bottom << "\x1b[0m\x1b[K\n";
    // The typing notice takes the status line above the input while it lasts;
    // the regular status returns when the peer stops typing.
    const QString statusText =
        peerTyping ? QStringLiteral("%1 is typing a message...")
                         .arg(conversations_.at(currentConversation_).session->peerName())
                   : status_;
    terminalOutput << "\x1b[30;46m" << (peerTyping ? "\x1b[3m" : "")
                   << elide(QStringLiteral(" %1").arg(statusText), terminalColumns_)
                   << "\x1b[0m\x1b[K\n";
    QString quotes;
    for (int quote = 0; quote < pendingQuotes_.size(); ++quote) {
        quotes += QStringLiteral("[quote %1: %2 lines%3] ")
                      .arg(quote + 1)
                      .arg(pendingQuotes_.at(quote).text.count(u'\n') + 1)
                      .arg(pendingQuotes_.at(quote).translate ? QStringLiteral(", translate") : QString());
    }
    terminalOutput << "\x1b[1;32m> \x1b[0m";
    if (!quotes.isEmpty()) terminalOutput << "\x1b[30;47m" << elide(quotes, terminalColumns_ - 2) << "\x1b[0m";
    terminalOutput << elide(input_, std::max(0, terminalColumns_ - 2 - displayWidth(quotes))) << "\x1b[K";
    if (openMenu_ != Menu::None) drawMenu(openMenuColumn);
    drawDialog();
    // The cursor would otherwise blink inside the open drop-down.
    terminalOutput << (openMenu_ != Menu::None && dialog_ == Dialog::None ? "\x1b[?25l" : "\x1b[?25h")
                   << Qt::flush;
}

void TerminalUi::redrawIfResized()
{
    if (!ansi_) return;
#ifdef Q_OS_UNIX
    winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0
        && (size.ws_row != terminalRows_ || size.ws_col != terminalColumns_)) redraw();
#endif
}

void TerminalUi::print(const QString &text) const
{
    if (ansi_) {
        const_cast<TerminalUi *>(this)->status_ = sanitize(text).replace(u'\n', QStringLiteral(" | "));
        const_cast<TerminalUi *>(this)->redraw();
    } else {
        terminalOutput << "\r\n" << text << "\n" << Qt::flush;
    }
}

void TerminalUi::printPrompt()
{
    if (!ansi_) terminalOutput << "> " << Qt::flush;
}

void TerminalUi::printHosts() const
{
    const auto hosts = sortedHosts();
    if (ansi_) {
        const_cast<TerminalUi *>(this)->status_ = hosts.isEmpty() ? QStringLiteral("No LANTalk hosts found.")
                                                                    : QStringLiteral("%1 host(s) found.").arg(hosts.size());
        const_cast<TerminalUi *>(this)->redraw();
        return;
    }
    QStringList lines{QStringLiteral("Discovered hosts:")};
    for (int i = 0; i < hosts.size(); ++i) lines << QStringLiteral("  %1. %2 (%3:%4)").arg(i + 1).arg(hosts[i].nickname, hosts[i].address.toString()).arg(hosts[i].port);
    print(hosts.isEmpty() ? QStringLiteral("No LANTalk hosts found.") : lines.join(u'\n'));
}

void TerminalUi::printConversations() const
{
    if (ansi_) {
        const_cast<TerminalUi *>(this)->status_ = QStringLiteral("%1 conversation(s).").arg(conversations_.size());
        const_cast<TerminalUi *>(this)->redraw();
        return;
    }
    QStringList lines{QStringLiteral("Conversations:")};
    for (int i = 0; i < conversations_.size(); ++i) lines << QStringLiteral(" %1%2 %3").arg(i == currentConversation_ ? QStringLiteral("*") : QStringLiteral(" ")).arg(i + 1).arg(conversationTitle(conversations_[i]));
    print(conversations_.isEmpty() ? QStringLiteral("No conversations.") : lines.join(u'\n'));
}

QString TerminalUi::conversationTitle(const Conversation &conversation) const
{
    const QString peer = conversation.session->peerName();
    const QString address = conversation.session->peerAddress();
    const QString title = peer.isEmpty()
                              ? (address.isEmpty() ? conversation.title : address)
                              : (address.isEmpty()
                                     ? peer
                                     : QStringLiteral("%1 (%2)").arg(peer, address));
    return conversation.disconnected && !peer.isEmpty()
               ? QStringLiteral("%1 (Disconnected)").arg(peer)
               : title;
}
