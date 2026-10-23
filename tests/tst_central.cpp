#include "testsupport.h"

#include "appconfig.h"

#include "central/centralserver.h"
#include "protocol/centralclient.h"
#include "protocol/centralprotocol.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace TestSupport;

namespace {

// One LANTalk client in central mode: its connection to the service, its own
// connection registry, and the sessions it owns.
class CentralPeer {
public:
  CentralPeer(const QString &nickname, quint16 servicePort,
              quint16 listenPort = DefaultPort)
      : nickname_(nickname) {
    client_.setReconnectInterval(100);
    QObject::connect(&client_, &CentralClient::incomingConnection,
                     owner_.get(), [this](RelayTransport *transport) {
                       auto *session =
                           new ChatSession(transport, owner_.get(), &registry_);
                       session->beginHandshake(nickname_);
                       incoming_.append(session);
                     });
    client_.start(QStringLiteral("127.0.0.1:%1").arg(servicePort), nickname,
                  listenPort);
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

  void silentClientsAreDropped() {
    CentralServer server;
    server.setIdleTimeout(200);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    RawPeer raw(server.serverPort());
    raw.send(QStringLiteral("CENTRAL_HELLO 1 %1 45454")
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
