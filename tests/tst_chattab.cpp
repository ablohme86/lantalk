#include "testsupport.h"

#include "gui/chatinput.h"
#include "gui/chatlog.h"
#include "gui/chattab.h"
#include "gui/dialogs.h"
#include "protocol/chathistory.h"
#include "messagetranslator.h"
#include "ollamadiscovery.h"
#include "translationrestorer.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>

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
    QCOMPARE(translate->text(), QStringLiteral("Translate quote"));
    QVERIFY(translate->isHidden()); // only shown while Live Translate is on

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

    const auto log = tab.findChild<ChatLog *>()->toPlainText();
    QVERIFY(log.contains(QStringLiteral("    return x;")));
    QVERIFY(!log.contains(QStringLiteral("[quote]")));
  }

  void emptyingAQuoteRemovesIt() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.show();
    auto *input = tab.findChild<ChatInput *>();
    QVERIFY(input);

    QGuiApplication::clipboard()->setText(QStringLiteral("one\ntwo"));
    QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
    QPlainTextEdit *editor = nullptr;
    QTRY_VERIFY((editor = tab.findChild<QPlainTextEdit *>(
                     QStringLiteral("quoteEditor"))) != nullptr);
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Delete);
    QTRY_VERIFY(tab.findChild<QPlainTextEdit *>(QStringLiteral("quoteEditor")) ==
                nullptr);
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

  void notificationsShowTheMessage() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    QSignalSpy notified(&tab, &ChatTab::notifyIncoming);
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QTRY_VERIFY(session->isHandshakeComplete());

    bob.liveSessions().first()->sendMessage(QStringLiteral("hello\nthere"));
    QTRY_COMPARE(notified.size(), 1);
    QCOMPARE(notified.first().at(0).toString(), QStringLiteral("bob"));
    QCOMPARE(notified.first().at(1).toString(), QStringLiteral("hello there"));
  }

  void fileTransfersShowProgressBubbles() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *aliceSession = alice.connectTo(bob);
    ChatTab aliceTab(aliceSession);
    aliceSession->setParent(&aliceTab);
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    auto *bobSession = bob.liveSessions().first();
    ChatTab bobTab(bobSession);
    QTRY_VERIFY(aliceSession->isHandshakeComplete());
    QTRY_VERIFY(bobSession->isHandshakeComplete());

    QTemporaryDir directory;
    QFile file(directory.filePath(QStringLiteral("notes.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(100 * 1024, 'x'));
    file.close();

    // Each side shows the transfer as it happens, then the attachment in
    // the same place.
    QStringList uploading;
    QStringList receiving;
    connect(aliceSession, &ChatSession::fileTransferProgress, &aliceTab, [&] {
      uploading << aliceTab.findChild<ChatLog *>()->toPlainText();
    });
    connect(bobSession, &ChatSession::fileTransferProgress, &bobTab, [&] {
      receiving << bobTab.findChild<ChatLog *>()->toPlainText();
    });
    QSignalSpy received(bobSession, &ChatSession::fileReceived);
    QVERIFY(aliceSession->sendFile(file.fileName()));
    QTRY_COMPARE(received.size(), 1);

    QVERIFY(uploading.first().endsWith(QStringLiteral("Uploading notes.txt to bob. 0%…")));
    QVERIFY(uploading.last().endsWith(QStringLiteral("Uploading notes.txt to bob. 100%…")));
    QVERIFY(receiving.first().endsWith(QStringLiteral("Receiving notes.txt from alice. 0%…")));
    QVERIFY(receiving.size() > 2);
    QTRY_VERIFY(aliceTab.findChild<ChatLog *>()->toPlainText().endsWith(QStringLiteral("notes.txt")));
    const auto bobLog = bobTab.findChild<ChatLog *>()->toPlainText();
    QVERIFY(bobLog.endsWith(QStringLiteral("notes.txt")));
    QVERIFY(!bobLog.contains(QStringLiteral("Receiving")));
  }

  void notificationsWaitForLiveTranslate() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), QStringLiteral("translator"));
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {true, QStringLiteral("Norwegian Bokmål"),
                               QStringLiteral("English")});

    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = new ChatSession(new QTcpSocket, nullptr, &alice.registry());
    ChatTab tab(session);
    session->setParent(&tab);
    QSignalSpy notified(&tab, &ChatTab::notifyIncoming);
    QSignalSpy unread(&tab, &ChatTab::incomingMessageReceived);
    session->beginHandshake(QStringLiteral("alice"));
    session->connectToHost(QStringLiteral("127.0.0.1"), bob.port());
    QTRY_VERIFY(session->liveTranslationActiveOrPending());
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QTest::qWait(100);

    bob.liveSessions().first()->sendMessage(QStringLiteral("hello"));
    QTRY_COMPARE(unread.size(), 1);
    QTRY_COMPARE(notified.size(), 1);
    QCOMPARE(notified.first().at(1).toString(), QStringLiteral("<<hello>>"));
    QTest::qWait(100);
    QCOMPARE(notified.size(), 1);
  }

  void centralHostTranslatesWithoutALocalOllama() {
    // What MainWindow sets up while the central host has a verified Ollama.
    MessageTranslator::setRemote([](const QString &message, const QString &,
                                    const QString &,
                                    std::function<void(bool, const QString &)> done) {
      QTimer::singleShot(0, [message, done] { done(true, QStringLiteral("[[%1]]").arg(message)); });
    });
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    OllamaDiscovery discovery;
    discovery.setCandidates({});
    tab.setOllamaDiscovery(&discovery);
    QPushButton *button = nullptr;
    for (auto *candidate : tab.findChildren<QPushButton *>()) {
      if (candidate->text() == QStringLiteral("Live Translate")) button = candidate;
    }
    QVERIFY(button);
    QVERIFY(!button->isHidden());

    // Only the languages are asked for.
    {
      TranslationDialog dialog(&discovery);
      QVERIFY(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
      const auto combos = dialog.findChildren<QComboBox *>();
      QCOMPARE(std::count_if(combos.cbegin(), combos.cend(),
                             [](const QComboBox *combo) { return !combo->isHidden(); }),
               2);
    }

    QTRY_COMPARE(bob.liveSessions().size(), 1);
    QTRY_VERIFY(session->isHandshakeComplete());
    session->requestLiveTranslation(QStringLiteral("Norwegian Bokmål"),
                                    QStringLiteral("English"));
    QTRY_VERIFY(button->isChecked());
    bob.liveSessions().first()->sendMessage(QStringLiteral("hello"));
    QTRY_VERIFY(tab.findChild<ChatLog *>()->toPlainText().endsWith(QStringLiteral("[[hello]]")));
    MessageTranslator::setRemote({});
  }

  void translationsAreRememberedInHistory() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), QStringLiteral("translator"));
    TranslationSettings::save(QStringLiteral("127.0.0.1"),
                              QStringLiteral("bob"),
                              {true, QStringLiteral("Norwegian Bokmål"),
                               QStringLiteral("English")});
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    {
      auto *session = new ChatSession(new QTcpSocket, nullptr, &alice.registry());
      ChatTab tab(session);
      session->setParent(&tab);
      QSignalSpy notified(&tab, &ChatTab::notifyIncoming);
      session->beginHandshake(QStringLiteral("alice"));
      session->connectToHost(QStringLiteral("127.0.0.1"), bob.port());
      QTRY_VERIFY(session->liveTranslationActiveOrPending());
      QTRY_COMPARE(bob.liveSessions().size(), 1);
      QTest::qWait(100);
      bob.liveSessions().first()->sendMessage(QStringLiteral("hello"));
      QTRY_COMPARE(notified.size(), 1);
    }

    // After a restart the translation is in the history, with the original
    // as the other version.
    const auto entries =
        ChatHistory::load(QStringLiteral("127.0.0.1"), QStringLiteral("bob"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().message, QStringLiteral("<<hello>>"));
    QCOMPARE(entries.first().alternate, QStringLiteral("hello"));
  }

  void wholeHistoryIsKeptAndShownOnRequest() {
    const auto address = QStringLiteral("10.0.0.9");
    const auto when = QDateTime::currentDateTime();
    for (int index = 0; index < 650; ++index) {
      ChatHistory::append(address, QStringLiteral("carol"),
                          index % 2 ? QStringLiteral("You") : QStringLiteral("carol"),
                          QStringLiteral("message %1").arg(index), when.addSecs(index));
    }
    QCOMPARE(ChatHistory::count(address, QStringLiteral("carol")), 650);
    const auto page = ChatHistory::load(address, QStringLiteral("carol"), 200, 200);
    QCOMPARE(page.size(), 200);
    QCOMPARE(page.first().message, QStringLiteral("message 250"));
    QCOMPARE(page.last().message, QStringLiteral("message 449"));

    ChatLog log;
    QSignalSpy requested(&log, &ChatLog::earlierMessagesRequested);
    log.setEarlierMessagesAvailable(true);
    log.appendMessage(false, QStringLiteral("newest"), when);
    log.prependHistory({{true, QStringLiteral("older"), when, {}}});
    const auto lines = log.toPlainText().split(u'\n');
    QCOMPARE(lines.value(0), QStringLiteral("Show earlier messages"));
    QCOMPARE(lines.value(1), QStringLiteral("older"));
    QCOMPARE(lines.value(2), QStringLiteral("newest"));
    log.setEarlierMessagesAvailable(false);
    QTRY_COMPARE(log.toPlainText().split(u'\n').value(0), QStringLiteral("older"));

    // Reading at the bottom, the view stays put while older ones go in.
    log.resize(400, 300);
    log.show();
    for (int index = 0; index < 40; ++index) {
      log.appendMessage(index % 2, QStringLiteral("line %1").arg(index), when);
    }
    auto *bar = log.verticalScrollBar();
    QTRY_VERIFY(bar->maximum() > 0 && bar->value() == bar->maximum());
    const int maximumBefore = bar->maximum();
    QList<ChatLog::HistoryMessage> older;
    for (int index = 0; index < 20; ++index) {
      older.append({false, QStringLiteral("old %1").arg(index), when, {}});
    }
    log.prependHistory(older);
    QTRY_VERIFY(bar->maximum() > maximumBefore);
    QCOMPARE(bar->maximum() - bar->value(), 0);
  }

  void peerTranslationStatusShowsOurLanguageFirst() {
    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *aliceSession = alice.connectTo(bob);
    QTRY_COMPARE(bob.liveSessions().size(), 1);
    auto *bobSession = bob.liveSessions().first();
    ChatTab bobTab(bobSession);
    QTRY_VERIFY(aliceSession->isHandshakeComplete());

    // Alice reads Norwegian and translates to and from Bob's English.
    aliceSession->requestLiveTranslation(QStringLiteral("Norwegian Bokmål"),
                                         QStringLiteral("English"));
    const auto showsStatus = [&bobTab] {
      const auto labels = bobTab.findChildren<QLabel *>();
      return std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
        return label->text() ==
               QStringLiteral(
                   "alice enabled Live Translate (English → Norwegian Bokmål).");
      });
    };
    QTRY_VERIFY(showsStatus());

    // Bob cannot translate as well while Alice does, until she stops.
    QPushButton *bobButton = nullptr;
    for (auto *candidate : bobTab.findChildren<QPushButton *>()) {
      if (candidate->text() == QStringLiteral("Live Translate")) bobButton = candidate;
    }
    QVERIFY(bobButton);
    QTRY_VERIFY(!bobButton->isEnabled());
    QVERIFY(bobButton->toolTip().contains(QStringLiteral("alice")));
    aliceSession->stopLiveTranslation();
    QTRY_VERIFY(bobButton->isEnabled());
  }

  void bubblesSitOnTheirSides() {
    ChatLog log;
    log.resize(520, 520);
    log.show();
    const auto now = QDateTime::currentDateTime();
    log.appendNotice(QStringLiteral("──────── Earlier messages ────────"));
    log.appendNotice(QStringLiteral("Connected to bob"));
    log.appendMessage(false, QStringLiteral("Hei! Hvordan går det?"), now);
    const int sent = log.appendMessage(true, QStringLiteral("Bra, takk!"), now);
    log.appendMessage(
        true,
        QStringLiteral("Look at this:\n[quote]\nstatic constexpr int MinPackRadius = 3;\n"
                       "    return radius;\n[/quote]"),
        now);
    const int pending = log.appendMessage(false, QStringLiteral("x"), now);
    log.setMessage(pending, QStringLiteral("Translating incoming message..."),
                   true, {});
    log.setMessage(pending,
                   QStringLiteral("A much longer received message that has to wrap "
                                  "over several lines because it is wider than the "
                                  "bubble is allowed to be."),
                   false, QStringLiteral("Original: ..."));
    QTest::qWait(50);
    // LANTALK_SCREENSHOT=<file.png> saves the rendered log for a visual check.
    const auto screenshot = qEnvironmentVariable("LANTALK_SCREENSHOT");
    if (!screenshot.isEmpty()) log.grab().save(screenshot);

    const auto bubbles = log.findChildren<QLabel *>();
    const auto bubbleWith = [&bubbles](const QString &text) -> QLabel * {
      for (auto *label : bubbles) {
        if (label->property("plainText").toString().startsWith(text)) return label;
      }
      return nullptr;
    };
    auto *received = bubbleWith(QStringLiteral("Hei!"));
    auto *mine = bubbleWith(QStringLiteral("Bra, takk!"));
    auto *wrapped = bubbleWith(QStringLiteral("A much longer"));
    QVERIFY(received && mine && wrapped);
    const auto left = received->mapTo(&log, QPoint(0, 0)).x();
    const auto right = mine->mapTo(&log, QPoint(mine->width(), 0)).x();
    QVERIFY2(left < 40, qPrintable(QString::number(left)));
    QVERIFY2(right > log.viewport()->width() - 40, qPrintable(QString::number(right)));
    QVERIFY(wrapped->width() <= log.viewport()->width() * 3 / 4);
    // Wrapped onto more lines than a one-line bubble.
    QVERIFY(wrapped->height() >= mine->height() + wrapped->fontMetrics().lineSpacing());
    QCOMPARE(wrapped->toolTip(), QStringLiteral("Original: ..."));
    QVERIFY(!wrapped->font().italic());
    Q_UNUSED(sent);

  }

  void liveTranslateAppearsWhenOllamaIsFound() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    OllamaDiscovery discovery;
    discovery.setPort(ollama.port());
    discovery.setCandidates({QHostAddress(QStringLiteral("127.0.0.1"))});

    Peer alice(QStringLiteral("alice"));
    Peer bob(QStringLiteral("bob"));
    auto *session = alice.connectTo(bob);
    ChatTab tab(session);
    session->setParent(&tab);
    tab.setOllamaDiscovery(&discovery);
    QPushButton *button = nullptr;
    for (auto *candidate : tab.findChildren<QPushButton *>()) {
      if (candidate->text() == QStringLiteral("Live Translate")) button = candidate;
    }
    QVERIFY(button);
    QVERIFY(button->isHidden()); // nothing configured, nothing found yet

    QSignalSpy finished(&discovery, &OllamaDiscovery::scanFinished);
    discovery.scan();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!button->isHidden());
  }

  void translationDialogOffersFoundServers() {
    FakeOllama ollama({QStringLiteral("translator:7b"), QStringLiteral("llama3")});
    QVERIFY(ollama.listen());
    OllamaDiscovery discovery;
    discovery.setPort(ollama.port());
    discovery.setCandidates({QHostAddress(QStringLiteral("127.0.0.1"))});
    QSignalSpy finished(&discovery, &OllamaDiscovery::scanFinished);
    discovery.scan();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);

    TranslationDialog dialog(&discovery);
    // LANTALK_SCREENSHOT_DIALOG=<file.png> saves the dialog for a visual check.
    const auto screenshot = qEnvironmentVariable("LANTALK_SCREENSHOT_DIALOG");
    if (!screenshot.isEmpty()) {
      dialog.show();
      QTest::qWait(50);
      dialog.grab().save(screenshot);
    }
    QCOMPARE(dialog.ollamaUrl(), QStringLiteral("http://127.0.0.1:%1").arg(ollama.port()));
    QCOMPARE(dialog.ollamaModel(), QStringLiteral("translator:7b"));
    const auto combos = dialog.findChildren<QComboBox *>();
    QComboBox *models = nullptr;
    for (auto *combo : combos) {
      if (combo->findText(QStringLiteral("llama3")) >= 0) models = combo;
    }
    QVERIFY(models);
    QCOMPARE(models->count(), 2);

    auto *ok = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    QVERIFY(ok->isEnabled());
    models->setCurrentText(QString());
    QVERIFY(!ok->isEnabled());
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
