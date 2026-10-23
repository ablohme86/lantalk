#include "testsupport.h"

#include "protocol/chathistory.h"
#include "protocol/conversationidentity.h"

#include <QCryptographicHash>
#include <QSignalSpy>

using namespace TestSupport;

class ConnectionRegistryTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() { isolateSettings(); }

  void identityComparisonsAreNormalized() {
    using namespace ConversationIdentity;
    QVERIFY(sameNickname(QStringLiteral("  Bob "), QStringLiteral("bob")));
    QVERIFY(sameNickname(QStringLiteral("Ｂｏｂ"), QStringLiteral("bob")));
    QVERIFY(!sameNickname(QStringLiteral("bob"), QStringLiteral("bobby")));
    QVERIFY(!sameNickname(QString(), QString()));
    QVERIFY(sameAddress(QStringLiteral("::ffff:192.168.1.5"),
                        QStringLiteral(" 192.168.1.5")));
    QVERIFY(sameAddress(QStringLiteral("MyHost.local"),
                        QStringLiteral("myhost.local")));
    QVERIFY(!sameAddress(QStringLiteral("192.168.1.5"),
                         QStringLiteral("192.168.1.6")));
    QCOMPARE(digest(QStringLiteral("10.0.0.1"), QStringLiteral(" Bob")),
             digest(QStringLiteral("10.0.0.1"), QStringLiteral("bob")));
  }

  void historyKeysStayCompatible() {
    // The digest must match the one chat history used before it moved to
    // ConversationIdentity, or existing histories would disappear.
    const QByteArray legacyIdentity = QByteArray("10.0.0.7") + '\0' + "bob";
    const auto legacyDigest = QString::fromLatin1(
        QCryptographicHash::hash(legacyIdentity, QCryptographicHash::Sha256)
            .toHex());
    QCOMPARE(ConversationIdentity::digest(QStringLiteral("10.0.0.7"),
                                          QStringLiteral("Bob")),
             legacyDigest);
    QSettings().setValue(QStringLiteral("chatHistory/%1").arg(legacyDigest),
                         QVariantList{QVariantMap{
                             {QStringLiteral("sender"), QStringLiteral("Bob")},
                             {QStringLiteral("message"), QStringLiteral("hi")},
                         }});
    QCOMPARE(ChatHistory::load(QStringLiteral("10.0.0.7"),
                               QStringLiteral("bob"))
                 .size(),
             1);
  }

  void pendingConnectionIsFoundByEndpoint() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    auto &registry = alice.registry();
    QCOMPARE(registry.findSession(QStringLiteral("127.0.0.1"), bob.port()),
             session);
    QCOMPARE(registry.findSession(QStringLiteral("127.0.0.1"), bob.port(),
                                  QStringLiteral("bob")),
             session);
    QCOMPARE(registry.findSession(QStringLiteral("127.0.0.1"),
                                  quint16(bob.port() + 1)),
             nullptr);
    QCOMPARE(registry.findSession(QStringLiteral("127.0.0.2"), bob.port()),
             nullptr);
  }

  void identifiedConnectionIsFoundByNickname() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_VERIFY(session->isHandshakeComplete());
    auto &registry = alice.registry();
    QCOMPARE(registry.findSession(QStringLiteral("127.0.0.1"), 1,
                                  QStringLiteral(" BOB ")),
             session);
    QCOMPARE(registry.findSession(QStringLiteral("127.0.0.1"), bob.port(),
                                  QStringLiteral("carol")),
             nullptr);

    // Incoming sessions are matched by address and nickname as well.
    QTRY_COMPARE(bob.incoming().size(), 1);
    QTRY_VERIFY(bob.incoming().first()->isHandshakeComplete());
    QCOMPARE(bob.registry().findSession(QStringLiteral("127.0.0.1"),
                                        alice.port(), QStringLiteral("alice")),
             bob.incoming().first());
  }

  void closedConversationsAreNotReused() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_VERIFY(session->isHandshakeComplete());
    session->closeConnection();
    QCOMPARE(alice.registry().findSession(QStringLiteral("127.0.0.1"),
                                          bob.port(), QStringLiteral("bob")),
             nullptr);
  }

  void duplicateOutgoingConnectionIsMerged() {
    // Bypasses the pre-connect lookup, as an internal code path could.
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *first = alice.connectTo(bob);
    QTRY_VERIFY(first->isHandshakeComplete());
    auto *second = alice.connectTo(bob);
    QSignalSpy merged(second, &ChatSession::mergedInto);

    QTRY_COMPARE(merged.size(), 1);
    QCOMPARE(merged.first().first().value<ChatSession *>(), first);
    QVERIFY(second->isRetired());
    QTRY_COMPARE(alice.liveSessions(), QList<ChatSession *>{first});
    QTRY_COMPARE(bob.liveSessions().size(), 1);

    QSignalSpy received(bob.liveSessions().first(),
                        &ChatSession::messageReceived);
    first->sendMessage(QStringLiteral("still here"));
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.first().first().toString(), QStringLiteral("still here"));
  }

  void restartedPeerResumesExistingConversation() {
    Peer alice(QStringLiteral("alice"));
    auto bob = std::make_unique<Peer>(QStringLiteral("bob"));
    auto *conversation = alice.connectTo(*bob);
    QTRY_VERIFY(conversation->isHandshakeComplete());

    // Bob's application exits...
    bob.reset();
    QTRY_VERIFY(!conversation->isConnected());

    // ...and after restarting connects back to Alice.
    Peer restartedBob(QStringLiteral("bob"));
    QSignalSpy identified(conversation, &ChatSession::peerIdentified);
    QSignalSpy received(conversation, &ChatSession::messageReceived);
    auto *bobSession = restartedBob.connectTo(alice);
    QTRY_COMPARE(identified.size(), 1);
    QVERIFY(conversation->isConnected());
    QTRY_COMPARE(alice.liveSessions(), QList<ChatSession *>{conversation});
    QCOMPARE(alice.incoming().size(), 1);
    QVERIFY(alice.incoming().first()->isRetired());

    bobSession->sendMessage(QStringLiteral("I'm back"));
    QTRY_COMPARE(received.size(), 1);
  }

  void simultaneousConnectsConvergeOnOneConnection() {
    for (int attempt = 0; attempt < 10; ++attempt) {
      Peer alice(QStringLiteral("alice"));
      Peer bob(QStringLiteral("bob"));
      alice.connectTo(bob);
      bob.connectTo(alice);

      QTRY_VERIFY(alice.liveSessions().size() == 1 &&
                  bob.liveSessions().size() == 1);
      QTest::qWait(50);
      QCOMPARE(alice.liveSessions().size(), 1);
      QCOMPARE(bob.liveSessions().size(), 1);
      auto *aliceSide = alice.liveSessions().first();
      auto *bobSide = bob.liveSessions().first();
      // Both ends kept the same TCP connection, so it carries traffic both
      // ways.
      QSignalSpy bobReceived(bobSide, &ChatSession::messageReceived);
      QSignalSpy aliceReceived(aliceSide, &ChatSession::messageReceived);
      aliceSide->sendMessage(QStringLiteral("ping %1").arg(attempt));
      bobSide->sendMessage(QStringLiteral("pong %1").arg(attempt));
      QTRY_COMPARE(bobReceived.size(), 1);
      QTRY_COMPARE(aliceReceived.size(), 1);
      QVERIFY(aliceSide->isConnected() && bobSide->isConnected());
    }
  }

  void staleEstablishedConnectionIsReplaced() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    alice.registry().setRaceWindow(0);
    bob.registry().setRaceWindow(0);
    auto *conversation = alice.connectTo(bob);
    QTRY_VERIFY(conversation->isHandshakeComplete());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    const auto originalPort = conversation->peerPort();
    QTest::qWait(20);

    // Bob believes the old connection is gone (e.g. a half-open socket after
    // a crash) and connects again; the new connection wins on both sides.
    bob.connectTo(alice);
    QTRY_VERIFY(conversation->peerPort() != originalPort);
    QTRY_COMPARE(alice.liveSessions(), QList<ChatSession *>{conversation});
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QSignalSpy received(bob.liveSessions().first(),
                        &ChatSession::messageReceived);
    conversation->sendMessage(QStringLiteral("hello again"));
    QTRY_COMPARE(received.size(), 1);
  }
};

QTEST_GUILESS_MAIN(ConnectionRegistryTest)
#include "tst_connectionregistry.moc"
