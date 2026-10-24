#include "testsupport.h"

#include "appconfig.h"

#include <QRegularExpression>
#include <QSignalSpy>

#include <algorithm>

using namespace TestSupport;

class TypingTest final : public QObject {
  Q_OBJECT

private:
  // Connects a RawPeer to `host` and waits for the handshake to complete.
  static ChatSession *handshakeWithRaw(Peer &host, RawPeer &raw,
                                       bool supportsTyping) {
    raw.send(QStringLiteral("NICKNAME raw"));
    if (supportsTyping) raw.send(QStringLiteral("CAPABILITIES TYPING"));
    if (!QTest::qWaitFor([&] {
          return !host.incoming().isEmpty() &&
                 host.incoming().first()->isHandshakeComplete() &&
                 (!supportsTyping ||
                  host.incoming().first()->peerSupportsTyping());
        })) {
      return nullptr;
    }
    return host.incoming().first();
  }

private slots:
  void initTestCase() { isolateSettings(); }

  void capabilitiesAreNegotiatedBetweenCurrentClients() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_VERIFY(session->peerSupportsTyping());
    QTRY_COMPARE(bob.incoming().size(), 1);
    QTRY_VERIFY(bob.incoming().first()->peerSupportsTyping());
  }

  void startAndStopAreSentOnlyOnTransitions() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);

    for (int edit = 0; edit < 20; ++edit) session->noteLocalTyping(true);
    QTRY_COMPARE(raw.count(QStringLiteral("TYPING START")), 1);
    QTest::qWait(100);
    QCOMPARE(raw.count(QStringLiteral("TYPING START")), 1);

    session->noteLocalTyping(false);
    session->stopLocalTyping();
    QTRY_COMPARE(raw.count(QStringLiteral("TYPING STOP")), 1);
    QTest::qWait(100);
    QCOMPARE(raw.count(QStringLiteral("TYPING STOP")), 1);
  }

  void inactivitySendsTypingStop() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    session->setTypingTimeouts(150, 10000, 10000);

    session->noteLocalTyping(true);
    QTRY_COMPARE(raw.count(QStringLiteral("TYPING START")), 1);
    QTRY_COMPARE_WITH_TIMEOUT(raw.count(QStringLiteral("TYPING STOP")), 1,
                              2000);
  }

  void newInputResetsInactivityTimer() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    session->setTypingTimeouts(300, 10000, 10000);

    session->noteLocalTyping(true);
    for (int edit = 0; edit < 5; ++edit) {
      QTest::qWait(120);
      session->noteLocalTyping(true);
    }
    QCOMPARE(raw.count(QStringLiteral("TYPING STOP")), 0);
    QTRY_COMPARE_WITH_TIMEOUT(raw.count(QStringLiteral("TYPING STOP")), 1,
                              2000);
  }

  void typingIsRefreshedWhileComposing() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    session->setTypingTimeouts(10000, 100, 10000);

    session->noteLocalTyping(true);
    QTRY_VERIFY(raw.count(QStringLiteral("TYPING START")) >= 3);
  }

  void peerIndicatorFollowsStartAndStop() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    QSignalSpy changes(session, &ChatSession::peerTypingChanged);

    raw.send(QStringLiteral("TYPING START"));
    raw.send(QStringLiteral("TYPING START"));
    QTRY_VERIFY(session->peerTyping());
    raw.send(QStringLiteral("TYPING STOP"));
    QTRY_VERIFY(!session->peerTyping());
    QCOMPARE(changes.size(), 2);
  }

  void peerIndicatorExpiresWhenStopIsLost() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    session->setTypingTimeouts(10000, 10000, 200);

    raw.send(QStringLiteral("TYPING START"));
    QTRY_VERIFY(session->peerTyping());
    QTRY_VERIFY_WITH_TIMEOUT(!session->peerTyping(), 2000);
  }

  void incomingMessageHidesIndicator() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    QSignalSpy messages(session, &ChatSession::messageReceived);

    raw.send(QStringLiteral("TYPING START"));
    QTRY_VERIFY(session->peerTyping());
    raw.send(QStringLiteral("MESSAGE %1").arg(QString::fromLatin1(
        QByteArray("hi").toBase64(QByteArray::Base64UrlEncoding))));
    QTRY_COMPARE(messages.size(), 1);
    QVERIFY(!session->peerTyping());
  }

  void sendingWhileStillTypingRestoresIndicator() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);

    session->noteLocalTyping(true);
    session->sendMessage(QStringLiteral("hello"));
    QTRY_COMPARE(raw.count(QStringLiteral("TYPING START")), 2);
    const auto lines = raw.lines();
    QVERIFY(lines.indexOf(QRegularExpression(QStringLiteral("^MESSAGE "))) <
            lines.lastIndexOf(QStringLiteral("TYPING START")));
  }

  void disconnectClearsTypingState() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);

    raw.send(QStringLiteral("TYPING START"));
    QTRY_VERIFY(session->peerTyping());
    raw.close();
    QTRY_VERIFY(!session->isConnected());
    QVERIFY(!session->peerTyping());
    QVERIFY(!session->peerSupportsTyping());
  }

  void awayIsSharedWithPeers() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_COMPARE(bob.incoming().size(), 1);
    auto *incoming = bob.incoming().first();
    QTRY_VERIFY(session->isHandshakeComplete() && incoming->isHandshakeComplete());
    QTest::qWait(100);
    QVERIFY(!incoming->peerAway());

    // This process is both peers, so both are away now; each tells the other.
    ChatSession::setLocalAway(true);
    QTRY_VERIFY(incoming->peerAway());
    QTRY_VERIFY(session->peerAway());
    // Someone connecting meanwhile learns it right away.
    Peer carol(QStringLiteral("carol"));
    auto *late = carol.connectTo(bob);
    QTRY_VERIFY(late->isHandshakeComplete());
    QTRY_VERIFY(late->peerAway());
    ChatSession::setLocalAway(false);
    QTRY_VERIFY(!incoming->peerAway());
    QTRY_VERIFY(!late->peerAway());
  }

  void olderPeersOnlyGetFilesTheyAccept() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    // A LANTalk from before large files: TYPING, but no LARGEFILES.
    auto *session = handshakeWithRaw(host, raw, true);
    QVERIFY(session);
    QCOMPARE(session->maximumFileSize(), LegacyMaximumFileSize);
    raw.send(QStringLiteral("CAPABILITIES TYPING LARGEFILES"));
    QTRY_COMPARE(session->maximumFileSize(), MaximumFileSize);
  }

  void legacyPeersNeverReceiveTypingCommands() {
    Peer host(QStringLiteral("host"));
    RawPeer raw(host.port());
    auto *session = handshakeWithRaw(host, raw, false);
    QVERIFY(session);
    QTRY_VERIFY(raw.lines().contains(QStringLiteral("CAPABILITIES TYPING LARGEFILES PRESENCE")));

    session->noteLocalTyping(true);
    session->sendMessage(QStringLiteral("hello"));
    session->stopLocalTyping();
    QTRY_VERIFY(std::any_of(raw.lines().cbegin(), raw.lines().cend(),
                            [](const QString &line) {
                              return line.startsWith(QStringLiteral("MESSAGE "));
                            }));
    for (const auto &line : raw.lines()) {
      QVERIFY2(!line.startsWith(QStringLiteral("TYPING")), qPrintable(line));
    }
  }

  void typingWorksAgainAfterReconnect() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_VERIFY(session->peerSupportsTyping());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    auto *bobSession = bob.liveSessions().first();

    session->noteLocalTyping(true);
    QTRY_VERIFY(bobSession->peerTyping());
    session->reconnectToHost(QStringLiteral("127.0.0.1"), bob.port());
    QTRY_VERIFY(!bobSession->peerTyping());
    QTRY_VERIFY(session->peerSupportsTyping());
    // Bob's existing session adopts the reconnection, so the same object
    // keeps receiving typing notifications.
    QTRY_VERIFY(bobSession->isConnected());
    session->noteLocalTyping(true);
    QTRY_VERIFY(bobSession->peerTyping());
  }
};

QTEST_GUILESS_MAIN(TypingTest)
#include "tst_typing.moc"
