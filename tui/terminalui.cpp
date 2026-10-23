#include "tui/terminalui.h"

#include "appconfig.h"
#include "protocol/chatsession.h"

#include <QCoreApplication>
#include <QPointer>
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
} // namespace

TerminalUi::TerminalUi(QObject *parent) : QObject(parent)
{
    configureNickname();
    ansi_ = isTerminal();
    configureTerminal();
    startListening();
    connect(&server_, &QTcpServer::newConnection, this, &TerminalUi::acceptConnection);
    startDiscovery();

#ifdef Q_OS_UNIX
    auto *inputNotifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(inputNotifier, &QSocketNotifier::activated, this, &TerminalUi::handleInput);
#endif
    if (!ansi_) {
        print(QStringLiteral("LANChat terminal mode\n"
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
    status_ = QStringLiteral("Tab changes focus; arrows navigate; Enter selects or sends.");
    redraw();
}

TerminalUi::~TerminalUi()
{
    restoreTerminal();
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
    terminalOutput << "\x1b[?1049h\x1b[?25l" << Qt::flush;
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
        terminalOutput << "\x1b[?25h\x1b[?1049l" << Qt::flush;
    }
#endif
}

void TerminalUi::startListening()
{
    const quint16 port = QSettings().value(QStringLiteral("network/listenPort"), DefaultPort).toUInt();
    if (!server_.listen(QHostAddress::Any, port)) {
        print(QStringLiteral("Cannot listen on port %1: %2").arg(port).arg(server_.errorString()));
        return;
    }
    print(QStringLiteral("Listening on TCP port %1").arg(server_.serverPort()));
}

void TerminalUi::startDiscovery()
{
    if (!discoverySocket_.bind(QHostAddress::AnyIPv4, DiscoveryPort,
                               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        print(QStringLiteral("LAN discovery unavailable: %1").arg(discoverySocket_.errorString()));
        return;
    }
    connect(&discoverySocket_, &QUdpSocket::readyRead, this, &TerminalUi::readDiscoveryDatagrams);
    discoveryTimer_.setInterval(2000);
    connect(&discoveryTimer_, &QTimer::timeout, this, &TerminalUi::announcePresence);
    discoveryTimer_.start();
    announcePresence();
}

void TerminalUi::announcePresence()
{
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
        host = host->lastSeen.msecsTo(now) > 7000 ? discoveredHosts_.erase(host) : std::next(host);
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
        if (parts.size() != 4 || parts.at(0) != QStringLiteral("LANCHAT_DISCOVER")
            || parts.at(1) == discoveryId_) {
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
            sendChatMessage(text);
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
        openMenu_ = key == QByteArray("\x1b") + 'c' ? Menu::Chat : Menu::Settings;
        selectedAction_ = 0;
        focus_ = Focus::Menu;
        status_ = openMenu_ == Menu::Chat ? QStringLiteral("Chat menu open.") : QStringLiteral("Settings menu open.");
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
    } else if (key == "\x1b") {
        openMenu_ = Menu::None;
        focus_ = Focus::Chat;
        status_ = QStringLiteral("Menu closed.");
    } else if (key == "\t") {
        focus_ = focus_ == Focus::Hosts ? Focus::Chat
                 : focus_ == Focus::Chat  ? Focus::Menu
                                           : Focus::Hosts;
    } else if (key == "\x1b[A" || key == "\x1b[B") {
        const int delta = key == "\x1b[A" ? -1 : 1;
        if (focus_ == Focus::Hosts) {
            const int count = sortedHosts().size();
            if (count > 0) selectedHost_ = std::clamp(selectedHost_ + delta, 0, count - 1);
        } else if (focus_ == Focus::Menu) {
            if (openMenu_ != Menu::None) {
                const int actionCount = openMenu_ == Menu::Chat ? 3 : 4;
                selectedAction_ = (selectedAction_ + delta + actionCount) % actionCount;
            } else {
                selectedMenu_ = std::clamp(selectedMenu_ + delta, 0, 1);
            }
        } else if (!conversations_.isEmpty()) {
            currentConversation_ = (currentConversation_ + delta + conversations_.size())
                                   % conversations_.size();
        }
    } else if (key == "\x1b[C" || key == "\x1b[D") {
        if (focus_ == Focus::Menu) {
            if (openMenu_ != Menu::None) {
                openMenu_ = key == "\x1b[C" ? Menu::Settings : Menu::Chat;
                selectedAction_ = 0;
            } else {
                selectedMenu_ = selectedMenu_ == 0 ? 1 : 0;
            }
        } else if (focus_ == Focus::Chat && !conversations_.isEmpty()) {
            const int delta = key == "\x1b[C" ? 1 : -1;
            currentConversation_ = (currentConversation_ + delta + conversations_.size())
                                   % conversations_.size();
        }
    } else if (key == "\r" || key == "\n") {
        if (focus_ == Focus::Hosts) {
            const auto hosts = sortedHosts();
            if (selectedHost_ >= 0 && selectedHost_ < hosts.size()) {
                connectToAddress(hosts.at(selectedHost_).address.toString(), hosts.at(selectedHost_).port);
            } else {
                status_ = QStringLiteral("No discovered host selected.");
            }
        } else if (focus_ == Focus::Menu) {
            if (openMenu_ != Menu::None) activateMenuItem();
            else status_ = selectedMenu_ == 0 ? QStringLiteral("Press Alt-C for Chat actions.")
                                               : QStringLiteral("Press Alt-S for Settings.");
        } else {
            const QString text = input_.trimmed();
            input_.clear();
            if (text.startsWith(u'/')) {
                handleCommand(text);
            } else if (!text.isEmpty() && currentConversation_ >= 0
                       && currentConversation_ < conversations_.size()) {
                sendChatMessage(text);
            } else if (!text.isEmpty()) {
                status_ = QStringLiteral("Select a host and press Enter to connect.");
            }
        }
    } else if (key == "\x7f" || key == "\b") {
        if (focus_ == Focus::Chat && !input_.isEmpty()) input_.chop(1);
    } else if (key == "\x03") {
        QCoreApplication::quit();
        return;
    } else if (focus_ == Focus::Chat && key.size() == 1 && key.at(0) >= ' ') {
        input_.append(QString::fromUtf8(key));
    }
    redraw();
}

void TerminalUi::handleCommand(const QString &command)
{
    const auto parts = command.split(u' ', Qt::SkipEmptyParts);
    const QString name = parts.value(0).toLower();
    if (name == QStringLiteral("/help")) {
        print(QStringLiteral("/hosts, /connect <host|number> [port], /tabs, /use <tab>, /close [tab], "
                             "/nick <name>, /translate <my language> | <peer language>, "
                             "/stoptranslate, /quit. In the full-screen UI use Tab and arrows."));
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
            connectToAddress(hosts.at(number - 1).address.toString(), hosts.at(number - 1).port);
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
        conversation.session->requestLiveTranslation(myLanguage, peerLanguage);
        setStatus(QStringLiteral("Requesting Live Translate…"));
    } else if (name == QStringLiteral("/stoptranslate")) {
        if (currentConversation_ < 0 || currentConversation_ >= conversations_.size()) {
            setStatus(QStringLiteral("There is no active conversation."));
            return;
        }
        auto &conversation = conversations_[currentConversation_];
        conversation.liveTranslate = false;
        conversation.session->stopLiveTranslation();
        setStatus(QStringLiteral("Live Translate is off."));
    } else if (name == QStringLiteral("/quit") || name == QStringLiteral("/exit")) {
        QCoreApplication::quit();
    } else {
        print(QStringLiteral("Unknown command. Type /help for available commands."));
    }
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
            openDialog(Dialog::Quit, QStringLiteral("Quit LANChat"),
                       QStringLiteral("Quit LANChat? (y/n)"), QString());
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
                       QString::number(server_.serverPort() ? server_.serverPort() : DefaultPort));
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
        status_ = QStringLiteral("Nickname changed to %1.").arg(name);
    } else if (dialog == Dialog::OllamaUrl) {
        QSettings().setValue(QStringLiteral("ollama/url"), value);
        openDialog(Dialog::OllamaModel, QStringLiteral("Ollama Settings"),
                   QStringLiteral("Model name"), QSettings().value(QStringLiteral("ollama/model")).toString());
        return;
    } else if (dialog == Dialog::OllamaModel) {
        QSettings().setValue(QStringLiteral("ollama/model"), value);
        status_ = QStringLiteral("Ollama settings saved.");
    } else if (dialog == Dialog::ListenPort) {
        bool validPort = false;
        const quint16 port = value.toUShort(&validPort);
        if (!validPort || port == 0) {
            status_ = QStringLiteral("Port must be between 1 and 65535.");
            return;
        }
        setListenPort(port);
    }
    dialog_ = Dialog::None;
}

void TerminalUi::setListenPort(quint16 port)
{
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

void TerminalUi::connectToAddress(const QString &address, quint16 port)
{
    auto *socket = new QTcpSocket(this);
    socket->connectToHost(address, port);
    addSession(socket, QStringLiteral("Connecting to %1:%2").arg(address).arg(port));
}

void TerminalUi::acceptConnection()
{
    while (server_.hasPendingConnections()) addSession(server_.nextPendingConnection(), QStringLiteral("Incoming connection"));
}

void TerminalUi::addSession(QTcpSocket *socket, const QString &title)
{
    auto *session = new ChatSession(socket, this);
    conversations_.append({session, title, {QStringLiteral("* %1").arg(title)}});
    const int index = conversations_.size() - 1;
    currentConversation_ = index;
    focus_ = Focus::Chat;
    connect(session, &ChatSession::peerIdentified, this, [this, session] {
        for (int index = 0; index < conversations_.size(); ++index) if (conversations_[index].session == session) {
            conversations_[index].title = conversationTitle(conversations_[index]);
            appendTranscript(index, QStringLiteral("* Connected to %1").arg(conversations_[index].title));
            return;
        }
    });
    connect(session, &ChatSession::messageReceived, this,
            [this, session](const QString &message, const QString &) {
                const int index = conversationIndex(session);
                if (index < 0) return;
                const auto &conversation = conversations_.at(index);
                if (!conversation.liveTranslate) {
                    appendTranscript(index,
                                     QStringLiteral("%1: %2").arg(session->peerName(), message));
                    return;
                }

                const auto myLanguage = conversation.myLanguage;
                const auto peerLanguage = conversation.peerLanguage;
                const QPointer<ChatSession> safeSession(session);
                auto *translator = new OllamaClient(this);
                ++pendingIncomingTranslations_;
                setStatus(QStringLiteral("Translating incoming message... Stand by."));
                connect(translator, &OllamaClient::translated, this,
                        [this, safeSession, translator](const QString &translation,
                                                        const QString &) {
                            if (safeSession) {
                                const int conversation = conversationIndex(safeSession);
                                if (conversation >= 0) {
                                    appendTranscript(conversation,
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
                connect(translator, &OllamaClient::failed, this,
                        [this, safeSession, translator, message](const QString &error,
                                                                 const QString &) {
                            if (safeSession) {
                                const int conversation = conversationIndex(safeSession);
                                if (conversation >= 0) {
                                    appendTranscript(conversation,
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
            [this](bool active, const QString &sourceLanguage,
                   const QString &targetLanguage) {
                setStatus(active
                              ? QStringLiteral("The other user enabled Live Translate (%1 → %2).")
                                    .arg(sourceLanguage, targetLanguage)
                              : QStringLiteral("The other user disabled Live Translate."));
            });
    connect(session, &ChatSession::fileReceived, this, [this, session](const QString &name, const QString &path, const QString &) {
        const int index = std::find_if(conversations_.cbegin(), conversations_.cend(),
                                       [session](const Conversation &item) { return item.session == session; })
                          - conversations_.cbegin();
        if (index < conversations_.size()) appendTranscript(index, QStringLiteral("* %1 sent %2 (%3)").arg(session->peerName(), name, path));
    });
    connect(session, &ChatSession::disconnected, this, [this, session] {
        const int index = conversationIndex(session);
        if (index >= 0) appendTranscript(index, QStringLiteral("* Disconnected."));
        setStatus(QStringLiteral("Disconnected."));
    });
    session->beginHandshake(nickname());
    print(QStringLiteral("Opened tab %1: %2").arg(index + 1).arg(title));
}

void TerminalUi::closeConversation(int index)
{
    if (index < 0 || index >= conversations_.size()) {
        print(QStringLiteral("No such tab."));
        return;
    }
    conversations_.takeAt(index).session->deleteLater();
    if (conversations_.isEmpty()) currentConversation_ = -1;
    else if (currentConversation_ >= conversations_.size()) currentConversation_ = conversations_.size() - 1;
    else if (index < currentConversation_) --currentConversation_;
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
        conversation.session->sendMessage(text);
        appendTranscript(currentConversation_, QStringLiteral("me: %1").arg(text));
        return;
    }

    const QPointer<ChatSession> session(conversation.session);
    auto *translator = new OllamaClient(this);
    ++pendingOutgoingTranslations_;
    setStatus(QStringLiteral("Translating message..."));
    connect(translator, &OllamaClient::translated, this,
            [this, session, translator](const QString &translation,
                                        const QString &originalMessage) {
                if (session) {
                    const int index = conversationIndex(session);
                    if (index >= 0) {
                        session->sendMessage(translation, originalMessage);
                        appendTranscript(index,
                                         QStringLiteral("me: %1").arg(originalMessage));
                    }
                }
                --pendingOutgoingTranslations_;
                updateTranslationStatus(session ? QStringLiteral("Message translated and sent.")
                                                 : QStringLiteral("Translation finished; conversation closed."));
                translator->deleteLater();
            });
    connect(translator, &OllamaClient::failed, this,
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

QList<TerminalUi::DiscoveredHost> TerminalUi::sortedHosts() const
{
    auto hosts = discoveredHosts_.values();
    std::sort(hosts.begin(), hosts.end(), [](const DiscoveredHost &a, const DiscoveredHost &b) {
        return a.nickname.localeAwareCompare(b.nickname) < 0;
    });
    return hosts;
}

void TerminalUi::appendTranscript(int index, const QString &line)
{
    if (index >= 0 && index < conversations_.size()) {
        auto &lines = conversations_[index].transcript;
        lines.append(sanitize(line));
        if (lines.size() > 200) lines.remove(0, lines.size() - 200);
    }
    redraw();
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
    terminalOutput << "\x1b[1;36m" << boxedLine(QStringLiteral("> %1").arg(dialogInput_), width) << "\x1b[0m";
    at(5);
    terminalOutput << "\x1b[2;35m" << boxedLine(QStringLiteral("Enter = accept   Esc = cancel"), width) << "\x1b[0m";
    at(6);
    terminalOutput << "\x1b[1;35m" << bottomBorder << "\x1b[0m";
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
    const int contentRows = std::max(1, terminalRows_ - 6);
    terminalOutput << "\x1b[H\x1b[2J";
    const QString top = QStringLiteral("┌%1┐").arg(QString(std::max(1, terminalColumns_ - 2), u'─'));
    const QString divider = QStringLiteral("├%1┼%2┤").arg(QString(std::max(0, sidebar - 1), u'─'),
                                                          QString(std::max(0, mainWidth - 1), u'─'));
    const QString bottom = QStringLiteral("└%1┴%2┘").arg(QString(std::max(0, sidebar - 1), u'─'),
                                                        QString(std::max(0, mainWidth - 1), u'─'));
    terminalOutput << "\x1b[1;34m" << top << "\x1b[0m\x1b[K\n";
    QString menu = QStringLiteral("│ \x1b[1;36mLANChat\x1b[0m  ")
                       + (openMenu_ == Menu::Chat ? QStringLiteral("\x1b[1;33m[Chat]\x1b[0m")
                                                   : QStringLiteral("[Chat]"))
                       + QStringLiteral("  ")
                       + (openMenu_ == Menu::Settings ? QStringLiteral("\x1b[1;33m[Settings]\x1b[0m")
                                                       : QStringLiteral("[Settings]"))
                       + QStringLiteral("  \x1b[2mAlt-C / Alt-S opens menus\x1b[0m");
    terminalOutput << menu << "\x1b[" << terminalColumns_ << "G\x1b[1;34m│\x1b[0m\x1b[K\n";
    if (openMenu_ != Menu::None) {
        const QStringList actions = openMenu_ == Menu::Chat
                                        ? QStringList{QStringLiteral("Connect To"), QStringLiteral("Close Chat"), QStringLiteral("Quit")}
                                        : QStringList{QStringLiteral("Nickname"), QStringLiteral("Ollama settings"),
                                                      QStringLiteral("Listening port"), QStringLiteral("Back")};
        QString actionLine = QStringLiteral("│ ");
        for (int i = 0; i < actions.size(); ++i)
            actionLine += i == selectedAction_ ? QStringLiteral("\x1b[1;30;43m %1 \x1b[0m ").arg(actions.at(i))
                                               : QStringLiteral("\x1b[36m %1 \x1b[0m ").arg(actions.at(i));
        terminalOutput << actionLine << "\x1b[" << terminalColumns_ << "G\x1b[1;34m│\x1b[0m\x1b[K\n";
    } else {
        terminalOutput << "\x1b[1;34m" << divider << "\x1b[0m\x1b[K\n";
    }
    const auto hosts = sortedHosts();
    for (int row = 0; row < contentRows; ++row) {
        QString left = QStringLiteral("│");
        QString right;
        if (row == 0) left = QStringLiteral("├── HOSTS %1").arg(
            QString(std::max(0, sidebar - displayWidth(QStringLiteral("├── HOSTS "))), u'─'));
        else if (row - 1 < hosts.size()) {
            const auto &host = hosts.at(row - 1);
            left = QStringLiteral("│%1 %2").arg(row - 1 == selectedHost_ ? u'>' : u' ').arg(host.nickname);
        } else if (row == hosts.size() + 1 && hosts.isEmpty()) left = QStringLiteral("│ (discovering...)");
        if (row == 0) {
            right = QStringLiteral("── CHAT %1┤").arg(QString(
                std::max(0, mainWidth - displayWidth(QStringLiteral("── CHAT ┤"))), u'─'));
        } else if (row == 1) {
            if (conversations_.isEmpty()) right = QStringLiteral(" No conversations. Select a host and press Enter.");
            else {
                for (int tab = 0; tab < conversations_.size(); ++tab)
                    right += QStringLiteral(" %1%2 ").arg(tab == currentConversation_ ? u'[' : u' ')
                                 .arg(tab + 1) + (tab == currentConversation_ ? u']' : u' ');
            }
        } else if (currentConversation_ >= 0 && currentConversation_ < conversations_.size()) {
            const auto &lines = conversations_.at(currentConversation_).transcript;
            const int first = std::max(0, int(lines.size()) - (contentRows - 3));
            if (row - 2 + first < lines.size()) right = lines.at(row - 2 + first);
        }
        terminalOutput << (focus_ == Focus::Hosts && row > 0 && row - 1 == selectedHost_ ? "\x1b[30;46m" : "\x1b[36m")
                       << elide(left, sidebar) << "\x1b[0m";
        terminalOutput << QString(std::max(0, sidebar - displayWidth(elide(left, sidebar))), u' ')
                       << "\x1b[1;34m" << (row == 0 ? "┬" : "│") << "\x1b[0m";
        if (row == 0) {
            terminalOutput << "\x1b[36m" << right << "\x1b[0m\x1b[K\n";
        } else {
            terminalOutput << (focus_ == Focus::Chat ? "\x1b[37m" : "") << elide(right, mainWidth - 1)
                           << "\x1b[0m"
                           << QString(std::max(0, mainWidth - 1 - displayWidth(elide(right, mainWidth - 1))), u' ')
                           << "\x1b[1;34m│\x1b[0m\x1b[K\n";
        }
    }
    terminalOutput << "\x1b[1;34m" << bottom << "\x1b[0m\x1b[K\n";
    terminalOutput << "\x1b[30;46m" << elide(QStringLiteral(" %1").arg(status_), terminalColumns_)
                   << "\x1b[0m\x1b[K\n";
    terminalOutput << "\x1b[1;32m> \x1b[0m" << elide(input_, terminalColumns_ - 2) << "\x1b[K";
    drawDialog();
    terminalOutput << "\x1b[?25h" << Qt::flush;
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
        const_cast<TerminalUi *>(this)->status_ = hosts.isEmpty() ? QStringLiteral("No LANChat hosts found.")
                                                                    : QStringLiteral("%1 host(s) found.").arg(hosts.size());
        const_cast<TerminalUi *>(this)->redraw();
        return;
    }
    QStringList lines{QStringLiteral("Discovered hosts:")};
    for (int i = 0; i < hosts.size(); ++i) lines << QStringLiteral("  %1. %2 (%3:%4)").arg(i + 1).arg(hosts[i].nickname, hosts[i].address.toString()).arg(hosts[i].port);
    print(hosts.isEmpty() ? QStringLiteral("No LANChat hosts found.") : lines.join(u'\n'));
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
    return peer.isEmpty() ? (address.isEmpty() ? conversation.title : address)
                          : (address.isEmpty() ? peer : QStringLiteral("%1 (%2)").arg(peer, address));
}
