#include "testsupport.h"

#include "appconfig.h"

#include "central/accountstore.h"
#include "central/centralserver.h"
#include "central/translationservice.h"
#include "messagetranslator.h"
#include "protocol/centralclient.h"
#include "protocol/centralprotocol.h"
#include "protocol/chathistory.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>

using namespace TestSupport;

namespace {

// One LANTalk client in central mode: its connection to the service, its own
// connection registry, and the sessions it owns.
class CentralPeer {
public:
  CentralPeer(const QString &nickname, quint16 servicePort,
              quint16 listenPort = DefaultPort, const QString &password = {})
      : nickname_(nickname) {
    client_.setReconnectInterval(100);
    // Each one is a device of its own.
    client_.setDeviceId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QObject::connect(&client_, &CentralClient::incomingConnection,
                     owner_.get(), [this](RelayTransport *transport) {
                       auto *session =
                           new ChatSession(transport, owner_.get(), &registry_);
                       session->beginHandshake(nickname_);
                       incoming_.append(session);
                     });
    client_.start(QStringLiteral("127.0.0.1:%1").arg(servicePort), nickname,
                  listenPort, password);
  }
  // Sessions go first, so they close their channels while the client lives.
  ~CentralPeer() { owner_.reset(); }

  ChatSession *connectTo(const QString &nickname) {
    auto *session = new ChatSession(new RelayTransport(&client_), owner_.get(),
                                    &registry_);
    session->beginHandshake(nickname_);
    session->connectToHost(QStringLiteral("127.0.0.1"), DefaultPort, nickname);
    return session;
  }

  CentralClient &client() { return client_; }
  QList<ChatSession *> incoming() const { return incoming_; }
  ConnectionRegistry &registry() { return registry_; }

private:
  QString nickname_;
  ConnectionRegistry registry_;
  QList<ChatSession *> incoming_;
  std::unique_ptr<QObject> owner_ = std::make_unique<QObject>();
  CentralClient client_;
};

// As on a device that has never signed in with a PIN; the test's clients
// share their settings.
void forgetPins() { QSettings().remove(QStringLiteral("central/pins")); }

std::optional<CentralClient::Peer> peerNamed(CentralClient &client,
                                             const QString &nickname) {
  for (const auto &peer : client.peers()) {
    if (peer.nickname == nickname) return peer;
  }
  return std::nullopt;
}

int devicesOf(CentralClient &client, const QString &nickname) {
  const auto peer = peerNamed(client, nickname);
  return peer ? peer->devices : 0;
}

bool hasPeer(CentralClient &client, const QString &nickname) {
  for (const auto &peer : client.peers()) {
    if (peer.nickname == nickname) return true;
  }
  return false;
}

} // namespace

class CentralTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() { isolateSettings(); }

  void hostSpecsAreParsed() {
    QString host;
    quint16 port = 0;
    QVERIFY(CentralProtocol::parseHostSpec(QStringLiteral("chat.example.com"),
                                           &host, &port));
    QCOMPARE(host, QStringLiteral("chat.example.com"));
    QCOMPARE(port, CentralProtocol::DefaultCentralPort);
    QVERIFY(CentralProtocol::parseHostSpec(QStringLiteral(" 10.0.0.5:5000 "),
                                           &host, &port));
    QCOMPARE(host, QStringLiteral("10.0.0.5"));
    QCOMPARE(port, quint16(5000));
    QVERIFY(CentralProtocol::parseHostSpec(QStringLiteral("[::1]:7000"), &host,
                                           &port));
    QCOMPARE(host, QStringLiteral("::1"));
    QCOMPARE(port, quint16(7000));
    QVERIFY(!CentralProtocol::parseHostSpec(QStringLiteral(""), &host, &port));
    QVERIFY(!CentralProtocol::parseHostSpec(QStringLiteral("host:0"), &host,
                                            &port));
    QVERIFY(!CentralProtocol::parseHostSpec(QStringLiteral("host:x"), &host,
                                            &port));
  }

  void clientsSeeEachOtherAndLeave() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    auto bob = std::make_unique<CentralPeer>(QStringLiteral("bob"),
                                             server.serverPort(), 5000);
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    QTRY_VERIFY(hasPeer(bob->client(), QStringLiteral("alice")));
    QCOMPARE(alice.client().peers().size(), 1);
    const auto peer = alice.client().peers().first();
    QCOMPARE(peer.address, QStringLiteral("127.0.0.1"));
    QCOMPARE(peer.port, quint16(5000));

    bob.reset();
    QTRY_VERIFY(alice.client().peers().isEmpty());
    QTRY_COMPARE(server.clientCount(), 1);
  }

  void conversationIsRelayedEndToEnd() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    CentralPeer bob(QStringLiteral("bob"), server.serverPort());
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));

    auto *outgoing = alice.connectTo(QStringLiteral("bob"));
    QTRY_VERIFY(outgoing->isHandshakeComplete());
    QCOMPARE(outgoing->peerName(), QStringLiteral("bob"));
    QCOMPARE(outgoing->peerAddress(), QStringLiteral("127.0.0.1"));
    QTRY_COMPARE(bob.incoming().size(), 1);
    auto *incoming = bob.incoming().first();
    QTRY_VERIFY(incoming->isHandshakeComplete());
    QCOMPARE(incoming->peerName(), QStringLiteral("alice"));
    QTRY_VERIFY(outgoing->peerSupportsTyping());

    QSignalSpy received(incoming, &ChatSession::messageReceived);
    outgoing->sendMessage(QStringLiteral("Hei fra alice 👋"));
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.first().first().toString(),
             QStringLiteral("Hei fra alice 👋"));

    QSignalSpy typing(outgoing, &ChatSession::peerTypingChanged);
    incoming->noteLocalTyping(true);
    QTRY_VERIFY(outgoing->peerTyping());

    QSignalSpy disconnected(incoming, &ChatSession::disconnected);
    outgoing->closeConnection();
    QTRY_COMPARE(disconnected.size(), 1);
    QTRY_COMPARE(server.channelCount(), 0);
  }

  void filesAreRelayed() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    CentralPeer bob(QStringLiteral("bob"), server.serverPort());
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    auto *outgoing = alice.connectTo(QStringLiteral("bob"));
    QTRY_VERIFY(outgoing->isHandshakeComplete());
    QTRY_COMPARE(bob.incoming().size(), 1);
    auto *incoming = bob.incoming().first();
    QTRY_VERIFY(incoming->isHandshakeComplete());

    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("data.bin"));
    QByteArray contents;
    for (int index = 0; index < 300 * 1024; ++index) {
      contents.append(char(index * 31));
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
    file.close();

    QSignalSpy received(incoming, &ChatSession::fileReceived);
    QVERIFY(outgoing->sendFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), 1, 10000);
    QFile copy(received.first().at(1).toString());
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), contents);
  }

  void fileSizeLimitIsAnnouncedAndApplied() {
    CentralServer server;
    server.setMaximumFileSize(10 * 1024);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    CentralPeer bob(QStringLiteral("bob"), server.serverPort());
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    QCOMPARE(alice.client().maximumFileSize(), qint64(10 * 1024));
    auto *outgoing = alice.connectTo(QStringLiteral("bob"));
    QTRY_VERIFY(outgoing->isHandshakeComplete());
    QCOMPARE(outgoing->maximumFileSize(), qint64(10 * 1024));

    QTemporaryDir directory;
    QFile file(directory.filePath(QStringLiteral("big.bin")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(20 * 1024, 'x'));
    file.close();
    QSignalSpy status(outgoing, &ChatSession::statusChanged);
    QVERIFY(!outgoing->sendFile(file.fileName()));
    QCOMPARE(status.last().first().toString(),
             QStringLiteral("Files larger than 10 kB cannot be sent."));
  }

  void lanTalkTranslatesThroughTheCentralHost() {
    FakeOllama ollama({QStringLiteral("gemma")});
    QVERIFY(ollama.listen());
    CentralServer server;
    TranslationService translation;
    translation.setServer(ollama.url(), QStringLiteral("gemma"));
    server.setTranslationService(&translation);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    QTRY_VERIFY(alice.client().canTranslate());

    // What MainWindow and TerminalUi set up while the host can translate.
    auto *client = &alice.client();
    MessageTranslator::setRemote([client](const QString &message, const QString &source,
                                          const QString &target,
                                          std::function<void(bool, const QString &)> done) {
      client->translate(message, source, target, std::move(done));
    });
    QVERIFY(MessageTranslator::isAvailable());
    MessageTranslator translator;
    QSignalSpy translated(&translator, &MessageTranslator::translated);
    translator.translate(QStringLiteral("Hei\n[quote]\nsitat\n[/quote]"),
                         QStringLiteral("Norwegian Bokmål"), QStringLiteral("English"));
    QTRY_COMPARE(translated.size(), 1);
    MessageTranslator::setRemote({});
    // The central host translated it, leaving the quote alone.
    QCOMPARE(translated.first().at(0).toString(),
             QStringLiteral("<<Hei>>\n[quote]\nsitat\n[/quote]"));
    QCOMPARE(ollama.translatedTexts(), QStringList{QStringLiteral("Hei")});
  }

  void unreachableCentralOllamaIsNotOffered() {
    FakeOllama ollama({QStringLiteral("gemma")});
    QVERIFY(ollama.listen());
    const auto url = ollama.url();
    ollama.close();
    CentralServer server;
    TranslationService translation;
    QSignalSpy checked(&translation, &TranslationService::availabilityChanged);
    translation.setServer(url, QStringLiteral("gemma"));
    server.setTranslationService(&translation);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    QTRY_VERIFY(alice.client().isReady());
    QTRY_COMPARE(checked.size(), 1);
    QCOMPARE(checked.first().first().toBool(), false);
    QTest::qWait(100);
    QVERIFY(!alice.client().canTranslate());

    bool answered = false;
    QString error;
    alice.client().translate(QStringLiteral("Hei"), QStringLiteral("Norwegian Bokmål"),
                             QStringLiteral("English"), [&](bool ok, const QString &result) {
                               answered = !ok;
                               error = result;
                             });
    QTRY_VERIFY(answered);
    QVERIFY(error.contains(QStringLiteral("cannot translate")));
  }

  void slowReceiversSlowSendersDown() {
    CentralServer server;
    // Without flow control, 1 MB waiting would cost the conversation.
    server.setRelayLimits(64 * 1024, 1024 * 1024);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    RawPeer sender(server.serverPort());
    QTcpSocket receiver;
    receiver.setReadBufferSize(64 * 1024);
    receiver.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(receiver.waitForConnected(3000));
    QByteArray received;
    const auto readReceiver = [&] {
      received += receiver.readAll();
    };
    const auto signIn = [&](const QString &name, const std::function<void(const QByteArray &)> &send) {
      send("CENTRAL_HELLO 1 " + CentralProtocol::encodeField(name) + " 45454 -");
    };
    QTRY_VERIFY(!sender.lines().isEmpty());
    signIn(QStringLiteral("sender"), [&](const QByteArray &line) { sender.send(QString::fromUtf8(line)); });
    QTRY_VERIFY(receiver.waitForReadyRead(100) || receiver.bytesAvailable() > 0);
    readReceiver();
    receiver.write("CENTRAL_HELLO 1 " + CentralProtocol::encodeField(QStringLiteral("receiver")) +
                   " 45454 -\n");
    QTRY_VERIFY([&] { readReceiver(); return received.contains("CENTRAL_WELCOME"); }());
    QTRY_VERIFY(sender.lines().join(u'\n').contains(QStringLiteral("CENTRAL_PEER ")));
    QString receiverId;
    for (const auto &line : sender.lines()) {
      if (line.startsWith(QStringLiteral("CENTRAL_PEER "))) receiverId = line.section(u' ', 1, 1);
    }
    sender.send(QStringLiteral("CENTRAL_OPEN flow-1 %1").arg(receiverId));
    QTRY_VERIFY(sender.lines().contains(QStringLiteral("CENTRAL_OPENED flow-1")));

    // 24 MB while the receiver reads nothing: far more than the kernel and
    // 1 MB of pending output hold, so lantalkd must hold the sender back.
    const QString payload(16 * 1024, u'x');
    for (int index = 0; index < 1536; ++index) {
      sender.send(QStringLiteral("CENTRAL_DATA flow-1 L%1 %2").arg(index).arg(payload));
    }
    QTest::qWait(1500);
    QCOMPARE(server.channelCount(), 1);

    // Reading now brings every line, in order, on the receiver's channel.
    QByteArray channel;
    int lines = 0;
    QElapsedTimer timer;
    timer.start();
    while (lines < 1536 && timer.elapsed() < 30000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      readReceiver();
      int newline;
      while ((newline = received.indexOf('\n')) >= 0) {
        const auto line = received.left(newline);
        received.remove(0, newline + 1);
        if (line.startsWith("CENTRAL_INCOMING ")) channel = line.split(' ').value(1);
        if (!channel.isEmpty() && line.startsWith("CENTRAL_DATA " + channel + " L")) {
          QCOMPARE(line.split(' ').value(2), QByteArray("L") + QByteArray::number(lines));
          ++lines;
        }
      }
    }
    QCOMPARE(lines, 1536);
    QCOMPARE(server.channelCount(), 1);
  }

  void awayStatusReachesEveryone() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    CentralPeer bob(QStringLiteral("bob"), server.serverPort());
    QTRY_VERIFY(hasPeer(bob.client(), QStringLiteral("alice")));
    const auto aliceAway = [](CentralClient &client) {
      for (const auto &peer : client.peers()) {
        if (peer.nickname == QStringLiteral("alice")) return peer.away;
      }
      return false;
    };
    alice.client().setAway(true);
    QTRY_VERIFY(aliceAway(bob.client()));
    // Someone signing in later is told too.
    CentralPeer carol(QStringLiteral("carol"), server.serverPort());
    QTRY_VERIFY(hasPeer(carol.client(), QStringLiteral("alice")));
    QTRY_VERIFY(aliceAway(carol.client()));
    alice.client().setAway(false);
    QTRY_VERIFY(!aliceAway(bob.client()));
    QTRY_VERIFY(!aliceAway(carol.client()));
  }

  void peerReturningIsAnnounced() {
    // What LANTalk reconnects on instead of retrying every 10 seconds.
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    QSignalSpy changes(&alice.client(), &CentralClient::peersChanged);
    {
      CentralPeer bob(QStringLiteral("bob"), server.serverPort());
      QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    }
    QTRY_VERIFY(!hasPeer(alice.client(), QStringLiteral("bob")));
    changes.clear();
    CentralPeer bobAgain(QStringLiteral("bob"), server.serverPort());
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    QVERIFY(!changes.isEmpty());
  }

  void conversationEndsWhenThePeerLeaves() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    auto bob = std::make_unique<CentralPeer>(QStringLiteral("bob"),
                                             server.serverPort());
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    auto *outgoing = alice.connectTo(QStringLiteral("bob"));
    QTRY_VERIFY(outgoing->isHandshakeComplete());

    QSignalSpy disconnected(outgoing, &ChatSession::disconnected);
    bob->client().stop();
    QTRY_COMPARE(disconnected.size(), 1);
    QVERIFY(!outgoing->isConnected());
  }

  void unknownPeerFailsTheConnection() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    QTRY_VERIFY(alice.client().isReady());
    auto *outgoing = alice.connectTo(QStringLiteral("nobody"));
    QSignalSpy disconnected(outgoing, &ChatSession::disconnected);
    QSignalSpy status(outgoing, &ChatSession::statusChanged);
    QTRY_COMPARE(disconnected.size(), 1);
    QVERIFY(status.first().first().toString().contains(
        QStringLiteral("not connected to the central host")));
  }

  void connectionWaitsForTheServiceAndRetries() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer bob(QStringLiteral("bob"), server.serverPort());
    QTRY_VERIFY(bob.client().isReady());
    // Alice starts a conversation before her client has signed in.
    CentralPeer alice(QStringLiteral("alice"), server.serverPort());
    QVERIFY(!alice.client().isReady());
    auto *outgoing = alice.connectTo(QStringLiteral("bob"));
    QTRY_VERIFY(outgoing->isHandshakeComplete());

    // A reconnect through the factory opens a new channel.
    ChatTransport::setFactory(alice.client().transportFactory());
    outgoing->reconnectToHost(QStringLiteral("127.0.0.1"), DefaultPort);
    ChatTransport::setFactory({});
    QTRY_VERIFY(outgoing->isHandshakeComplete());
    QCOMPARE(outgoing->peerName(), QStringLiteral("bob"));
    QTRY_COMPARE(server.channelCount(), 1);
  }

  void clientReconnectsAfterTheServiceRestarts() {
    auto server = std::make_unique<CentralServer>();
    QVERIFY(server->listen(QHostAddress::LocalHost, 0));
    const auto port = server->serverPort();
    CentralPeer alice(QStringLiteral("alice"), port);
    QTRY_VERIFY(alice.client().isReady());
    server.reset();
    QTRY_VERIFY(!alice.client().isReady());
    server = std::make_unique<CentralServer>();
    QVERIFY(server->listen(QHostAddress::LocalHost, port));
    QTRY_VERIFY(alice.client().isReady());
  }

  void newSignInReplacesTheSameNicknameAndAddress() {
    CentralServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer first(QStringLiteral("alice"), server.serverPort());
    QTRY_VERIFY(first.client().isReady());
    CentralPeer second(QStringLiteral("Alice"), server.serverPort());
    QTRY_VERIFY(second.client().isReady());
    QTRY_VERIFY(!first.client().isReady());
    QVERIFY2(first.client().lastError().contains(QStringLiteral("same nickname")),
             qPrintable(first.client().lastError()));
    QTest::qWait(300);
    QVERIFY(!first.client().isReady());
    QCOMPARE(server.clientCount(), 1);
  }

  void passwordIsRequiredWhenConfigured() {
    CentralServer server;
    server.setPassword(QStringLiteral("hemmelig passord"));
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CentralPeer alice(QStringLiteral("alice"), server.serverPort(), DefaultPort,
                      QStringLiteral("hemmelig passord"));
    CentralPeer bob(QStringLiteral("bob"), server.serverPort(), DefaultPort,
                    QStringLiteral("hemmelig passord"));
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("bob")));
    auto *outgoing = alice.connectTo(QStringLiteral("bob"));
    QTRY_VERIFY(outgoing->isHandshakeComplete());

    CentralPeer wrong(QStringLiteral("mallory"), server.serverPort(),
                      DefaultPort, QStringLiteral("gjetning"));
    CentralPeer none(QStringLiteral("eve"), server.serverPort());
    QTRY_VERIFY(wrong.client().lastError().contains(QStringLiteral("Wrong password")));
    QTRY_VERIFY(none.client().lastError().contains(QStringLiteral("requires a password")));
    QVERIFY(!wrong.client().isReady());
    QVERIFY(!none.client().isReady());
    QVERIFY(!hasPeer(alice.client(), QStringLiteral("mallory")));
    QCOMPARE(server.clientCount(), 2);
    // A rejected client waits for new settings instead of retrying.
    QTest::qWait(400);
    QVERIFY(!wrong.client().isReady());
    QVERIFY(wrong.client().lastError().contains(QStringLiteral("Wrong password")));

    // New settings with the right password sign in.
    wrong.client().start(QStringLiteral("127.0.0.1:%1").arg(server.serverPort()),
                         QStringLiteral("mallory"), DefaultPort,
                         QStringLiteral("hemmelig passord"));
    QTRY_VERIFY(wrong.client().isReady());
    QTRY_VERIFY(hasPeer(alice.client(), QStringLiteral("mallory")));
  }

  void proofsDependOnPasswordAndChallenge() {
    const auto proof = CentralProtocol::authProof(QStringLiteral("a"), "n1");
    QCOMPARE(proof.size(), 64);
    QVERIFY(CentralProtocol::sameProof(
        proof, CentralProtocol::authProof(QStringLiteral("a"), "n1")));
    QVERIFY(!CentralProtocol::sameProof(
        proof, CentralProtocol::authProof(QStringLiteral("b"), "n1")));
    QVERIFY(!CentralProtocol::sameProof(
        proof, CentralProtocol::authProof(QStringLiteral("a"), "n2")));
    QCOMPARE(CentralProtocol::authProof({}, "n1"), QByteArray("-"));
  }

  void pinProtectsTheNickname() {
    QTemporaryDir data;
    CentralServer server;
    QVERIFY(server.setDataDirectory(data.path()));
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const auto port = server.serverPort();
    CentralPeer laptop(QStringLiteral("alice"), port);
    QTRY_COMPARE(laptop.client().accountState(),
                 CentralClient::AccountState::Unprotected);
    QVERIFY(!laptop.client().setPin(QStringLiteral("12a4")));
    QVERIFY(laptop.client().setPin(QStringLiteral("1234")));
    QTRY_COMPARE(laptop.client().accountState(),
                 CentralClient::AccountState::Protected);
    QVERIFY(laptop.client().hasSavedPin());

    // Another device has to know the PIN.
    forgetPins();
    CentralPeer phone(QStringLiteral("Alice"), port);
    QSignalSpy asked(&phone.client(), &CentralClient::pinRequired);
    QTRY_COMPARE(asked.size(), 1);
    QCOMPARE(asked.first().first().toString(), QStringLiteral("missing"));
    QVERIFY(!phone.client().isReady());
    phone.client().signInWithPin(QStringLiteral("4321"));
    QTRY_COMPARE(asked.size(), 2);
    QCOMPARE(asked.last().first().toString(), QStringLiteral("wrong"));
    QVERIFY(!phone.client().hasSavedPin());
    phone.client().signInWithPin(QStringLiteral("1234"));
    QTRY_VERIFY(phone.client().isReady());
    QCOMPARE(phone.client().accountState(), CentralClient::AccountState::Protected);
    QVERIFY(laptop.client().isReady());

    // Everyone else sees one alice on two devices.
    CentralPeer bob(QStringLiteral("bob"), port);
    QTRY_VERIFY(hasPeer(bob.client(), QStringLiteral("alice")));
    QTRY_COMPARE(devicesOf(bob.client(), QStringLiteral("alice")), 2);
    QCOMPARE(bob.client().peers().size(), 1);
    QVERIFY(peerNamed(bob.client(), QStringLiteral("alice"))->account);
    QVERIFY(!hasPeer(laptop.client(), QStringLiteral("alice")));

    // An older LANTalk cannot use the nickname.
    RawPeer old(port);
    QTRY_VERIFY(!old.lines().isEmpty());
    old.send(QStringLiteral("CENTRAL_HELLO 1 %1 45454 -")
                 .arg(QString::fromLatin1(
                     CentralProtocol::encodeField(QStringLiteral("alice")))));
    QTRY_VERIFY(old.lines().join(u'\n').contains(QStringLiteral("CENTRAL_REJECT")));
    QVERIFY(CentralProtocol::decodeField(old.lines().last().section(u' ', 1))
                .contains(QStringLiteral("protected with a PIN")));
    QTRY_COMPARE(server.clientCount(), 3);

    // Five wrong PINs lock the nickname for a while.
    forgetPins();
    CentralPeer guesser(QStringLiteral("alice"), port);
    QSignalSpy guesses(&guesser.client(), &CentralClient::pinRequired);
    QTRY_COMPARE(guesses.size(), 1);
    for (int attempt = 0; attempt < 5; ++attempt) {
      guesser.client().signInWithPin(QStringLiteral("000%1").arg(attempt));
      QTRY_COMPARE(guesses.size(), attempt + 2);
    }
    guesser.client().signInWithPin(QStringLiteral("1234"));
    QTRY_COMPARE(guesses.size(), 7);
    QCOMPARE(guesses.last().first().toString(), QStringLiteral("locked"));
  }

  void everyDeviceTakesPartInTheConversation() {
    QTemporaryDir data;
    CentralServer server;
    QVERIFY(server.setDataDirectory(data.path()));
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const auto port = server.serverPort();
    CentralPeer laptop(QStringLiteral("alice"), port);
    QTRY_COMPARE(laptop.client().accountState(),
                 CentralClient::AccountState::Unprotected);
    QVERIFY(laptop.client().setPin(QStringLiteral("2468")));
    QTRY_COMPARE(laptop.client().accountState(),
                 CentralClient::AccountState::Protected);
    // The PIN is saved, as the phone's own sign-in would have saved it.
    CentralPeer phone(QStringLiteral("alice"), port);
    QTRY_COMPARE(phone.client().accountState(),
                 CentralClient::AccountState::Protected);
    CentralPeer bob(QStringLiteral("bob"), port);
    QTRY_COMPARE(devicesOf(bob.client(), QStringLiteral("alice")), 2);

    auto *toAlice = bob.connectTo(QStringLiteral("alice"));
    QTRY_VERIFY(toAlice->isHandshakeComplete());
    QVERIFY(toAlice->peerHasAccount());
    QTRY_COMPARE(laptop.incoming().size(), 1);
    QTRY_COMPARE(phone.incoming().size(), 1);
    auto *onLaptop = laptop.incoming().first();
    auto *onPhone = phone.incoming().first();
    QTRY_VERIFY(onLaptop->isHandshakeComplete());
    QTRY_VERIFY(onPhone->isHandshakeComplete());
    QCOMPARE(onPhone->peerName(), QStringLiteral("bob"));

    QSignalSpy laptopReceived(onLaptop, &ChatSession::messageReceived);
    QSignalSpy phoneReceived(onPhone, &ChatSession::messageReceived);
    toAlice->sendMessage(QStringLiteral("Hei Alice"));
    QTRY_COMPARE(laptopReceived.size(), 1);
    QTRY_COMPARE(phoneReceived.size(), 1);

    // What one device sends reaches bob once and the other device as sent.
    QSignalSpy bobReceived(toAlice, &ChatSession::messageReceived);
    QSignalSpy mirrored(onPhone, &ChatSession::messageMirrored);
    onLaptop->sendMessage(QStringLiteral("Hello Bob"), QStringLiteral("Hei Bob"));
    QTRY_COMPARE(bobReceived.size(), 1);
    QTRY_COMPARE(mirrored.size(), 1);
    QCOMPARE(mirrored.first().at(0).toString(), QStringLiteral("Hello Bob"));
    QCOMPARE(mirrored.first().at(1).toString(), QStringLiteral("Hei Bob"));
    QTest::qWait(200);
    QCOMPARE(bobReceived.size(), 1);

    // Live Translate turned on from one device is on for the other.
    QSignalSpy adopted(onPhone, &ChatSession::liveTranslationAdopted);
    QSignalSpy granted(onLaptop, &ChatSession::liveTranslationGranted);
    QSignalSpy bobSees(toAlice, &ChatSession::peerLiveTranslationChanged);
    onLaptop->requestLiveTranslation(QStringLiteral("Norwegian Bokmål"),
                                     QStringLiteral("English"));
    QTRY_COMPARE(granted.size(), 1);
    QTRY_COMPARE(adopted.size(), 1);
    QCOMPARE(adopted.first().at(0).toString(), QStringLiteral("Norwegian Bokmål"));
    QVERIFY(onPhone->liveTranslationActiveOrPending());
    QTRY_COMPARE(bobSees.size(), 1);
    QSignalSpy stopped(onLaptop, &ChatSession::liveTranslationStopped);
    onPhone->stopLiveTranslation();
    QTRY_COMPARE(stopped.size(), 1);
    QVERIFY(!onLaptop->liveTranslationActiveOrPending());
    QTRY_COMPARE(bobSees.size(), 2);
    QCOMPARE(bobSees.last().first().toBool(), false);

    // A new device catches up on the history before it joins in.
    QSettings().remove(QStringLiteral("central/synced"));
    CentralPeer tablet(QStringLiteral("alice"), port);
    QSignalSpy history(&tablet.client(), &CentralClient::historyEntryAdded);
    QTRY_COMPARE(history.size(), 2);
    const auto first = history.first().at(2).value<ChatHistory::Entry>();
    const auto second = history.last().at(2).value<ChatHistory::Entry>();
    QCOMPARE(first.sender, QStringLiteral("bob"));
    QCOMPARE(first.message, QStringLiteral("Hei Alice"));
    QCOMPARE(second.sender, QStringLiteral("You"));
    QCOMPARE(second.message, QStringLiteral("Hei Bob"));
    QCOMPARE(second.alternate, QStringLiteral("Hello Bob"));
    QTRY_COMPARE(tablet.incoming().size(), 1);
    QTRY_VERIFY(tablet.incoming().first()->isHandshakeComplete());
    QCOMPARE(tablet.incoming().first()->peerName(), QStringLiteral("bob"));
    QCOMPARE(ChatHistory::load(tablet.incoming().first()->peerAddress(),
                               QStringLiteral("bob"), -1)
                 .size(),
             2);

    // Devices come and go without bob noticing.
    QSignalSpy bobDisconnected(toAlice, &ChatSession::disconnected);
    laptop.client().stop();
    QTRY_COMPARE(devicesOf(bob.client(), QStringLiteral("alice")), 2);
    QSignalSpy tabletReceived(tablet.incoming().first(), &ChatSession::messageReceived);
    toAlice->sendMessage(QStringLiteral("Fortsatt her?"));
    QTRY_COMPARE(tabletReceived.size(), 1);
    QCOMPARE(bobDisconnected.size(), 0);
    // Bob leaving ends it everywhere.
    QSignalSpy phoneDisconnected(onPhone, &ChatSession::disconnected);
    toAlice->closeConnection();
    QTRY_COMPARE(phoneDisconnected.size(), 1);
    QTRY_COMPARE(server.channelCount(), 0);
  }

  void pinsAndHistorySurviveARestart() {
    QTemporaryDir data;
    quint16 port = 0;
    {
      CentralServer server;
      QVERIFY(server.setDataDirectory(data.path()));
      QVERIFY(server.listen(QHostAddress::LocalHost, 0));
      port = server.serverPort();
      CentralPeer alice(QStringLiteral("alice"), port);
      QTRY_COMPARE(alice.client().accountState(),
                   CentralClient::AccountState::Unprotected);
      QVERIFY(alice.client().setPin(QStringLiteral("1357")));
      QTRY_COMPARE(alice.client().accountState(),
                   CentralClient::AccountState::Protected);
      CentralPeer bob(QStringLiteral("bob"), port);
      QTRY_VERIFY(hasPeer(bob.client(), QStringLiteral("alice")));
      auto *outgoing = bob.connectTo(QStringLiteral("alice"));
      QTRY_VERIFY(outgoing->isHandshakeComplete());
      QTRY_COMPARE(alice.incoming().size(), 1);
      QSignalSpy received(alice.incoming().first(), &ChatSession::messageReceived);
      outgoing->sendMessage(QStringLiteral("Lagret?"));
      QTRY_COMPARE(received.size(), 1);
    }
    CentralServer server;
    QVERIFY(server.setDataDirectory(data.path()));
    QVERIFY(server.listen(QHostAddress::LocalHost, port));
    forgetPins();
    CentralPeer alice(QStringLiteral("alice"), port);
    QSignalSpy asked(&alice.client(), &CentralClient::pinRequired);
    QTRY_COMPARE(asked.size(), 1);

    AccountStore store;
    QVERIFY(store.open(data.path()));
    const auto account = store.find(QStringLiteral("ALICE"));
    QVERIFY(account);
    const auto entries = store.entriesAfter(*account, 0);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().value(QStringLiteral("message")).toString(),
             QStringLiteral("Lagret?"));
    QCOMPARE(entries.first().value(QStringLiteral("peer")).toString(),
             QStringLiteral("bob"));
    QVERIFY(store.remove(QStringLiteral("alice")));
    QVERIFY(!store.find(QStringLiteral("alice")));
    QVERIFY(store.entriesAfter(*account, 0).isEmpty());
  }

  void signsInToALantalkdFromBeforePins() {
    // lantalkd 1.13 and older only read CENTRAL_HELLO 1.
    QTcpServer old;
    QVERIFY(old.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&old, &QTcpServer::newConnection, &old, [&old] {
      auto *socket = old.nextPendingConnection();
      socket->write("CENTRAL_CHALLENGE 00ff\n");
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
        const auto parts = socket->readLine().trimmed().split(' ');
        if (parts.size() != 5) {
          socket->write("CENTRAL_REJECT " +
                        CentralProtocol::encodeField(QStringLiteral(
                            "The central host could not read the sign-in.")) +
                        '\n');
          socket->disconnectFromHost();
          return;
        }
        socket->write("CENTRAL_WELCOME old-1 127.0.0.1\n");
      });
    });
    CentralPeer alice(QStringLiteral("alice"), old.serverPort());
    QTRY_VERIFY(alice.client().isReady());
    QCOMPARE(alice.client().accountState(),
             CentralClient::AccountState::Unavailable);
  }

  void silentClientsAreDropped() {
    CentralServer server;
    server.setIdleTimeout(200);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    RawPeer raw(server.serverPort());
    QTRY_VERIFY(!raw.lines().isEmpty());
    QVERIFY(raw.lines().first().startsWith(QStringLiteral("CENTRAL_CHALLENGE ")));
    raw.send(QStringLiteral("CENTRAL_HELLO 1 %1 45454 -")
                 .arg(QString::fromLatin1(
                     CentralProtocol::encodeField(QStringLiteral("raw")))));
    QTRY_VERIFY(raw.lines().join(u'\n').contains(
        QStringLiteral("CENTRAL_WELCOME")));
    QTRY_COMPARE(server.clientCount(), 1);
    QTRY_COMPARE(server.clientCount(), 0);
  }
};

QTEST_MAIN(CentralTest)
#include "tst_central.moc"
