#include "testsupport.h"

#include "translationrestorer.h"

#include <QSignalSpy>

using namespace TestSupport;

namespace {

const auto Norwegian = QStringLiteral("Norwegian Bokmål");
const auto English = QStringLiteral("English");
const auto Model = QStringLiteral("translator:7b");

} // namespace

class TranslationRestoreTest final : public QObject {
  Q_OBJECT

private slots:
  void init() { isolateSettings(); }

  void preferencesAreKeptPerConversation() {
    TranslationSettings::save(QStringLiteral("10.0.0.2"), QStringLiteral("bob"),
                              {true, Norwegian, English});
    TranslationSettings::save(QStringLiteral("10.0.0.2"),
                              QStringLiteral("carol"),
                              {false, English, Norwegian});

    const auto bob = TranslationSettings::load(QStringLiteral("10.0.0.2"),
                                               QStringLiteral(" BOB "));
    QVERIFY(bob.enabled);
    QCOMPARE(bob.myLanguage, Norwegian);
    QCOMPARE(bob.peerLanguage, English);
    const auto carol = TranslationSettings::load(QStringLiteral("10.0.0.2"),
                                                 QStringLiteral("carol"));
    QVERIFY(!carol.enabled);
    QCOMPARE(carol.myLanguage, English);
    QVERIFY(!TranslationSettings::load(QStringLiteral("10.0.0.3"),
                                       QStringLiteral("bob"))
                 .enabled);
  }

  void unknownLanguagesAreIgnored() {
    TranslationSettings::save(QStringLiteral("10.0.0.2"), QStringLiteral("bob"),
                              {true, QStringLiteral("Klingon"), English});
    QVERIFY(!TranslationSettings::load(QStringLiteral("10.0.0.2"),
                                       QStringLiteral("bob"))
                 .enabled);
  }

  void grantedRequestIsPersistedAndDisablingIsRemembered() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_VERIFY(session->isHandshakeComplete());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    TranslationRestorer restorer(session);
    QSignalSpy granted(session, &ChatSession::liveTranslationGranted);

    restorer.request(Norwegian, English);
    QTRY_COMPARE(granted.size(), 1);
    auto saved = restorer.savedPreference();
    QVERIFY(saved.enabled);
    QCOMPARE(saved.myLanguage, Norwegian);
    QCOMPARE(saved.peerLanguage, English);

    restorer.disable();
    saved = restorer.savedPreference();
    QVERIFY(!saved.enabled);
    QCOMPARE(saved.myLanguage, Norwegian);
    QVERIFY(!session->liveTranslationActiveOrPending());
  }

  void resumesAfterApplicationRestart() {
    FakeOllama ollama({Model});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), Model);
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {true, Norwegian, English});

    // A fresh session and restorer, as created when the app starts again.
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = new ChatSession(new QTcpSocket, nullptr,
                                    &alice.registry());
    TranslationRestorer restorer(session);
    QSignalSpy resumed(&restorer, &TranslationRestorer::resumeRequested);
    QSignalSpy granted(session, &ChatSession::liveTranslationGranted);
    session->beginHandshake(QStringLiteral("alice"));
    session->connectToHost(QStringLiteral("127.0.0.1"), bob.port());

    QTRY_COMPARE(granted.size(), 1);
    QCOMPARE(resumed.size(), 1);
    QCOMPARE(resumed.first().at(0).toString(), Norwegian);
    QCOMPARE(resumed.first().at(1).toString(), English);
    delete session;
  }

  void resumesAfterReconnect() {
    FakeOllama ollama({Model + QStringLiteral(":latest")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), Model);
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    QTRY_VERIFY(session->isHandshakeComplete());
    TranslationRestorer restorer(session);
    QSignalSpy granted(session, &ChatSession::liveTranslationGranted);
    restorer.request(Norwegian, English);
    QTRY_COMPARE(granted.size(), 1);

    QTRY_COMPARE(bob.liveSessions().size(), 1);
    bob.liveSessions().first()->closeConnection();
    QTRY_VERIFY(!session->isConnected());
    QVERIFY(!session->liveTranslationActiveOrPending());

    session->reconnectToHost(QStringLiteral("127.0.0.1"), bob.port());
    QTRY_COMPARE(granted.size(), 2);
    QVERIFY(session->liveTranslationActiveOrPending());
  }

  void waitsForUnavailableOllamaAndResumesWhenItReturns() {
    FakeOllama ollama({Model});
    QVERIFY(ollama.listen());
    const auto port = ollama.port();
    configureOllama(ollama.url(), Model);
    ollama.close();
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {true, Norwegian, English});

    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = new ChatSession(new QTcpSocket, nullptr,
                                    &alice.registry());
    TranslationRestorer restorer(session);
    restorer.setRetryInterval(100);
    QSignalSpy status(&restorer, &TranslationRestorer::statusChanged);
    QSignalSpy granted(session, &ChatSession::liveTranslationGranted);
    session->beginHandshake(QStringLiteral("alice"));
    session->connectToHost(QStringLiteral("127.0.0.1"), bob.port());

    QTRY_VERIFY(status.size() >= 1);
    QVERIFY(status.last().first().toString().contains(
        QStringLiteral("resume automatically")));
    QTest::qWait(250);
    QCOMPARE(granted.size(), 0);

    QVERIFY(ollama.listen(port));
    QTRY_COMPARE_WITH_TIMEOUT(granted.size(), 1, 5000);
    delete session;
  }

  void missingModelDoesNotResume() {
    FakeOllama ollama({QStringLiteral("other-model")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), Model);
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {true, Norwegian, English});

    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = new ChatSession(new QTcpSocket, nullptr,
                                    &alice.registry());
    TranslationRestorer restorer(session);
    QSignalSpy status(&restorer, &TranslationRestorer::statusChanged);
    session->beginHandshake(QStringLiteral("alice"));
    session->connectToHost(QStringLiteral("127.0.0.1"), bob.port());

    QTRY_VERIFY(status.size() >= 1);
    QVERIFY(status.last().first().toString().contains(Model));
    QVERIFY(!session->liveTranslationActiveOrPending());
    delete session;
  }

  void disabledPreferenceDoesNotResume() {
    FakeOllama ollama({Model});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), Model);
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {false, Norwegian, English});

    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    TranslationRestorer restorer(session);
    QSignalSpy resumed(&restorer, &TranslationRestorer::resumeRequested);
    QTRY_VERIFY(session->isHandshakeComplete());
    QTest::qWait(200);
    QCOMPARE(resumed.size(), 0);
    QVERIFY(!session->liveTranslationActiveOrPending());
  }
};

QTEST_GUILESS_MAIN(TranslationRestoreTest)
#include "tst_translationrestore.moc"
