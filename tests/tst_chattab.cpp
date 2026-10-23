#include "testsupport.h"

#include "gui/chattab.h"
#include "translationrestorer.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>

using namespace TestSupport;

class ChatTabTest final : public QObject {
  Q_OBJECT

private slots:
  void init() { isolateSettings(); }

  void typingIndicatorFollowsPeer() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.show();
    auto *indicator = tab.findChild<QLabel *>(QStringLiteral("typingIndicator"));
    QVERIFY(indicator);
    QTRY_VERIFY(session->peerSupportsTyping());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QTRY_VERIFY(bob.liveSessions().first()->peerSupportsTyping());
    auto *bobSession = bob.liveSessions().first();

    QVERIFY(!indicator->isVisible());
    bobSession->noteLocalTyping(true);
    QTRY_VERIFY(indicator->isVisible());
    QVERIFY(indicator->text().startsWith(QStringLiteral("bob is typing")));
    bobSession->stopLocalTyping();
    QTRY_VERIFY(!indicator->isVisible());
  }

  void userEditsSendTypingAndSendingStopsIt() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.show();
    QTRY_VERIFY(session->peerSupportsTyping());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    auto *bobSession = bob.liveSessions().first();
    QSignalSpy received(bobSession, &ChatSession::messageReceived);
    auto *input = tab.findChild<QLineEdit *>();
    QVERIFY(input);

    QTest::keyClicks(input, QStringLiteral("hello"));
    QTRY_VERIFY(bobSession->peerTyping());
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_COMPARE(received.size(), 1);
    QTRY_VERIFY(!bobSession->peerTyping());

    QTest::keyClicks(input, QStringLiteral("x"));
    QTRY_VERIFY(bobSession->peerTyping());
    QTest::keyClick(input, Qt::Key_Backspace);
    QTRY_VERIFY(!bobSession->peerTyping());
  }

  void savedTranslationResumesAndChecksButton() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), QStringLiteral("translator"));
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {true, QStringLiteral("English"),
                               QStringLiteral("Simplified Chinese")});

    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = new ChatSession(new QTcpSocket, nullptr, &alice.registry());
    ChatTab tab(session);
    session->setParent(&tab);
    session->beginHandshake(QStringLiteral("alice"));
    session->connectToHost(QStringLiteral("127.0.0.1"), bob.port());
    QPushButton *button = nullptr;
    for (auto *candidate : tab.findChildren<QPushButton *>()) {
      if (candidate->text() == QStringLiteral("Live Translate")) button = candidate;
    }
    QVERIFY(button);

    QTRY_VERIFY(button->isChecked());
    QVERIFY(session->liveTranslationActiveOrPending());
  }
};

QTEST_MAIN(ChatTabTest)
#include "tst_chattab.moc"
