#pragma once

#include <QList>
#include <QString>

// Chat messages may contain quoted blocks, typically pasted text such as code:
//
//   Look at this:
//   [quote]
//   int x = 1;
//   [/quote]
//
// Quotes are shown as blocks and are not translated. Locally, a quote can be
// marked "[quote translate]" to have Live Translate translate it anyway; the
// mark is removed before the message is sent, so peers only see "[quote]".
// Clients without quote support show the tags as text.
namespace QuotedMessage {

struct Segment {
  QString text;
  bool quote = false;
  bool translate = false;
};

QList<Segment> parse(const QString &message);
// Joins segments, putting each quote's tags on lines of their own.
QString compose(const QList<Segment> &segments, bool keepTranslateFlags);
// The message as it is sent and stored: quotes without translate marks.
QString withoutTranslateFlags(const QString &message);
// Whether pasted text is large enough to become a quote rather than inline
// text: it spans several lines or is long.
bool shouldQuote(const QString &pastedText);
// A one-line summary for notifications: the text on one line, each quote as
// "[quote: N lines]", shortened to about maxLength characters.
QString preview(const QString &message, int maxLength = 160);

} // namespace QuotedMessage
