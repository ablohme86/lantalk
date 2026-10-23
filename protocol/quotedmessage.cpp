#include "protocol/quotedmessage.h"

#include <QRegularExpression>

namespace {

const auto QuoteOpen = QStringLiteral("[quote]");
const auto TranslatedQuoteOpen = QStringLiteral("[quote translate]");
const auto QuoteClose = QStringLiteral("[/quote]");
constexpr int LongPasteLength = 200;

QString withoutLeadingNewline(QString text) {
  if (text.startsWith(u'\n')) text.remove(0, 1);
  return text;
}

QString withoutTrailingNewline(QString text) {
  if (text.endsWith(u'\n')) text.chop(1);
  return text;
}

} // namespace

QList<QuotedMessage::Segment> QuotedMessage::parse(const QString &message) {
  static const QRegularExpression open(
      QStringLiteral("\\[quote( translate)?\\]"));
  QList<Segment> segments;
  int position = 0;
  while (position < message.size()) {
    const auto match = open.match(message, position);
    const int close = match.hasMatch()
                          ? message.indexOf(QuoteClose, match.capturedEnd())
                          : -1;
    if (close < 0) {
      // No (complete) quote left: the rest is plain text.
      const auto rest = message.mid(position);
      segments.append({segments.isEmpty() ? rest : withoutLeadingNewline(rest)});
      break;
    }
    const auto before = message.mid(position, match.capturedStart() - position);
    const auto text = withoutTrailingNewline(
        segments.isEmpty() ? before : withoutLeadingNewline(before));
    if (!text.isEmpty()) segments.append({text});
    const auto quoted = message.mid(match.capturedEnd(),
                                    close - match.capturedEnd());
    segments.append({withoutTrailingNewline(withoutLeadingNewline(quoted)), true,
                     match.capturedLength(1) > 0});
    position = close + QuoteClose.size();
  }
  // Text after the last quote lost its separating newline above; an empty
  // remainder is dropped.
  if (!segments.isEmpty() && !segments.last().quote &&
      segments.last().text.isEmpty()) {
    segments.removeLast();
  }
  return segments;
}

QString QuotedMessage::compose(const QList<Segment> &segments,
                               bool keepTranslateFlags) {
  QString message;
  for (const auto &segment : segments) {
    if (!message.isEmpty()) message += u'\n';
    if (!segment.quote) {
      message += segment.text;
      continue;
    }
    message += segment.translate && keepTranslateFlags ? TranslatedQuoteOpen
                                                       : QuoteOpen;
    message += u'\n' + segment.text + u'\n' + QuoteClose;
  }
  return message;
}

QString QuotedMessage::withoutTranslateFlags(const QString &message) {
  if (!message.contains(TranslatedQuoteOpen)) return message;
  return compose(parse(message), false);
}

bool QuotedMessage::shouldQuote(const QString &pastedText) {
  const auto text = withoutTrailingNewline(pastedText);
  return text.contains(u'\n') || text.size() > LongPasteLength;
}
