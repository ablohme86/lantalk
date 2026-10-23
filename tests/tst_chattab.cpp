#include "testsupport.h"

#include "gui/chatinput.h"
#include "gui/chattab.h"
#include "translationrestorer.h"

#include <QApplication>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QTextBrowser>
#include <QLabel>
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
    QVERIFY(indicator->text().startsWith(
        QStringLiteral("bob is typing a message")));
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
    auto *input = tab.findChild<ChatInput *>();
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

  void shiftEnterAddsLinesAndEnterSendsThem() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.resize(500, 400);
    tab.show();
    QTRY_VERIFY(session->isHandshakeComplete());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QSignalSpy received(bob.liveSessions().first(),
                        &ChatSession::messageReceived);
    auto *input = tab.findChild<ChatInput *>();
    QVERIFY(input);
    const int singleLineHeight = input->height();

    QTest::keyClicks(input, QStringLiteral("if (x) {"));
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClicks(input, QStringLiteral("    run();"));
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClicks(input, QStringLiteral("}"));
    QCOMPARE(input->text(), QStringLiteral("if (x) {\n    run();\n}"));
    QCOMPARE(received.size(), 0);
    QTRY_VERIFY(input->height() > singleLineHeight);

    QTest::keyClick(input, Qt::Key_Return);
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.first().first().toString(),
             QStringLiteral("if (x) {\n    run();\n}"));
    QVERIFY(input->text().isEmpty());
    QTRY_COMPARE(input->height(), singleLineHeight);
  }

  void pastedCodeKeepsIndentationAndHeightIsCapped() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.resize(500, 400);
    tab.show();
    QTRY_VERIFY(session->isHandshakeComplete());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QSignalSpy received(bob.liveSessions().first(),
                        &ChatSession::messageReceived);
    auto *input = tab.findChild<ChatInput *>();
    QVERIFY(input);

    QStringList lines;
    for (int line = 0; line < 20; ++line) {
      lines << QStringLiteral("    line %1").arg(line);
    }
    input->insertPlainText(QStringLiteral("\n") + lines.join(u'\n') +
                           QStringLiteral("  \n\n"));
    const int cappedHeight = input->height();
    input->insertPlainText(QStringLiteral("\nmore"));
    QCOMPARE(input->height(), cappedHeight);

    QTest::keyClick(input, Qt::Key_Return);
    QTRY_COMPARE(received.size(), 1);
    const auto message = received.first().first().toString();
    QVERIFY(message.startsWith(QStringLiteral("    line 0\n")));
    QVERIFY(message.endsWith(QStringLiteral("    line 19  \n\n\nmore")));
  }

  void largePastesBecomeQuotesSentAsBlocks() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.resize(500, 400);
    tab.show();
    QTRY_VERIFY(session->isHandshakeComplete());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QSignalSpy received(bob.liveSessions().first(),
                        &ChatSession::messageReceived);
    auto *input = tab.findChild<ChatInput *>();
    QVERIFY(input);

    const auto code = QStringLiteral("int x = 1;\r\n    return x;\r\n");
    QGuiApplication::clipboard()->setText(code);
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    QVERIFY(input->text().isEmpty());
    QCheckBox *translate = nullptr;
    QTRY_VERIFY((translate = tab.findChild<QCheckBox *>()) != nullptr);
    QVERIFY(!translate->isChecked());
    QVERIFY(!translate->isEnabled()); // Live Translate is off

    // The quote is shown in full and can be edited before sending.
    auto *editor = tab.findChild<QPlainTextEdit *>(QStringLiteral("quoteEditor"));
    QVERIFY(editor);
    QCOMPARE(editor->toPlainText(), QStringLiteral("int x = 1;\n    return x;"));
    QVERIFY(editor->height() >= editor->fontMetrics().lineSpacing() * 3);
    editor->moveCursor(QTextCursor::End);
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::keyClicks(editor, QStringLiteral("// edited"));

    QTest::keyClicks(input, QStringLiteral("Look:"));
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.first().first().toString(),
             QStringLiteral("Look:\n[quote]\nint x = 1;\n    return x;\n// edited\n[/quote]"));
    QTRY_VERIFY(tab.findChild<QCheckBox *>() == nullptr);

    const auto log = tab.findChild<QTextBrowser *>()->toPlainText();
    QVERIFY(log.contains(QStringLiteral("    return x;")));
    QVERIFY(!log.contains(QStringLiteral("[quote]")));
  }

  void ctrlShiftVPastesPlainText() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.show();
    auto *input = tab.findChild<ChatInput *>();
    QVERIFY(input);

    QGuiApplication::clipboard()->setText(QStringLiteral("a\nb"));
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(input->text(), QStringLiteral("a\nb"));
    QVERIFY(tab.findChild<QCheckBox *>() == nullptr);
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
