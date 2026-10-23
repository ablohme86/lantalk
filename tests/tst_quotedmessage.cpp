#include "testsupport.h"

#include "messagetranslator.h"
#include "protocol/quotedmessage.h"

#include <QSignalSpy>

using namespace TestSupport;
using QuotedMessage::Segment;

class QuotedMessageTest final : public QObject {
  Q_OBJECT

private slots:
  void init() { isolateSettings(); }

  void messagesWithoutQuotesAreUnchanged() {
    const auto message = QStringLiteral("hello\n  indented\n");
    const auto segments = QuotedMessage::parse(message);
    QCOMPARE(segments.size(), 1);
    QCOMPARE(segments.first().text, message);
    QVERIFY(!segments.first().quote);
    QCOMPARE(QuotedMessage::withoutTranslateFlags(message), message);
    QVERIFY(QuotedMessage::parse(QString()).isEmpty());
  }

  void quotesAreParsedAndComposedBack() {
    const auto message = QStringLiteral(
        "Look:\n[quote]\nint x = 1;\n  return x;\n[/quote]\nand\n"
        "[quote translate]\nHei\n[/quote]");
    const auto segments = QuotedMessage::parse(message);
    QCOMPARE(segments.size(), 4);
    QCOMPARE(segments.at(0).text, QStringLiteral("Look:"));
    QVERIFY(segments.at(1).quote && !segments.at(1).translate);
    QCOMPARE(segments.at(1).text, QStringLiteral("int x = 1;\n  return x;"));
    QCOMPARE(segments.at(2).text, QStringLiteral("and"));
    QVERIFY(segments.at(3).quote && segments.at(3).translate);
    QCOMPARE(QuotedMessage::compose(segments, true), message);
  }

  void translateMarksAreNeverSent() {
    const auto message = QuotedMessage::compose(
        {Segment{QStringLiteral("hi")},
         Segment{QStringLiteral("code"), true, true}},
        true);
    QVERIFY(message.contains(QStringLiteral("[quote translate]")));
    QCOMPARE(QuotedMessage::withoutTranslateFlags(message),
             QStringLiteral("hi\n[quote]\ncode\n[/quote]"));
  }

  void unterminatedQuotesStayText() {
    const auto message = QStringLiteral("see [quote] here");
    const auto segments = QuotedMessage::parse(message);
    QCOMPARE(segments.size(), 1);
    QCOMPARE(segments.first().text, message);
  }

  void largePastesBecomeQuotes() {
    QVERIFY(!QuotedMessage::shouldQuote(QStringLiteral("one line")));
    QVERIFY(!QuotedMessage::shouldQuote(QStringLiteral("one line\n")));
    QVERIFY(QuotedMessage::shouldQuote(QStringLiteral("two\nlines")));
    QVERIFY(QuotedMessage::shouldQuote(QString(201, u'x')));
  }

  void previewsFitOnOneLine() {
    QCOMPARE(QuotedMessage::preview(QStringLiteral("hi\n  there")),
             QStringLiteral("hi there"));
    QCOMPARE(QuotedMessage::preview(
                 QStringLiteral("Look:\n[quote]\na\nb\n[/quote]")),
             QStringLiteral("Look: [quote: 2 lines]"));
    const auto shortened = QuotedMessage::preview(QString(300, u'x'), 50);
    QCOMPARE(shortened.size(), 50);
    QVERIFY(shortened.endsWith(QStringLiteral("…")));
  }

  void translatorLeavesQuotesAlone() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url(), QStringLiteral("translator"));
    MessageTranslator translator;
    QSignalSpy translated(&translator, &MessageTranslator::translated);

    const auto message = QuotedMessage::compose(
        {Segment{QStringLiteral("Hello")},
         Segment{QStringLiteral("int x;"), true, false},
         Segment{QStringLiteral("Good luck"), true, true}},
        true);
    translator.translate(message, QStringLiteral("English"),
                         QStringLiteral("Norwegian Bokmål"));

    QTRY_COMPARE(translated.size(), 1);
    QCOMPARE(translated.first().at(0).toString(),
             QStringLiteral("<<Hello>>\n[quote]\nint x;\n[/quote]\n"
                            "[quote]\n<<Good luck>>\n[/quote]"));
    QCOMPARE(translated.first().at(1).toString(),
             QuotedMessage::withoutTranslateFlags(message));
    QCOMPARE(ollama.translatedTexts(),
             (QStringList{QStringLiteral("Hello"), QStringLiteral("Good luck")}));
  }

  void quoteOnlyMessagesNeedNoTranslation() {
    MessageTranslator translator;
    QSignalSpy translated(&translator, &MessageTranslator::translated);
    const auto message = QStringLiteral("[quote]\nint x;\n[/quote]");
    translator.translate(message, QStringLiteral("English"),
                         QStringLiteral("Norwegian Bokmål"));
    QCOMPARE(translated.size(), 0); // still asynchronous
    QTRY_COMPARE(translated.size(), 1);
    QCOMPARE(translated.first().at(0).toString(), message);
  }
};

QTEST_GUILESS_MAIN(QuotedMessageTest)
#include "tst_quotedmessage.moc"
