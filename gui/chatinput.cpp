#include "gui/chatinput.h"

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QMimeData>

#include "protocol/quotedmessage.h"

#include <algorithm>

namespace {

constexpr int MaximumVisibleLines = 12;

} // namespace

ChatInput::ChatInput(QWidget *parent) : QPlainTextEdit(parent) {
  setTabChangesFocus(true);
  setLineWrapMode(QPlainTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(this, &QPlainTextEdit::textChanged, this, [this] {
    if (!settingText_) emit textEdited(text());
  });
  // Also emitted when wrapping changes with the width.
  connect(document()->documentLayout(),
          &QAbstractTextDocumentLayout::documentSizeChanged, this,
          [this] { fitHeightToContent(this, 1, MaximumVisibleLines); });
  fitHeightToContent(this, 1, MaximumVisibleLines);
}

QString ChatInput::text() const { return toPlainText(); }

void ChatInput::setText(const QString &text) {
  settingText_ = true;
  setPlainText(text);
  moveCursor(QTextCursor::End);
  settingText_ = false;
}

void ChatInput::clear() { setText({}); }

QSize ChatInput::sizeHint() const {
  return {QPlainTextEdit::sizeHint().width(), height()};
}

QSize ChatInput::minimumSizeHint() const {
  return {QPlainTextEdit::minimumSizeHint().width(),
          plainTextHeightForLines(this, 1)};
}

void ChatInput::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_V &&
      event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
    pastePlainText_ = true;
    paste();
    pastePlainText_ = false;
    return;
  }
  const bool isReturn =
      event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
  if (isReturn && !(event->modifiers() & Qt::ShiftModifier)) {
    emit returnPressed();
    return;
  }
  if (isReturn) {
    // A plain line break rather than QPlainTextEdit's Shift+Enter line
    // separator, so the text keeps ordinary newlines.
    if (!isReadOnly()) insertPlainText(QStringLiteral("\n"));
    return;
  }
  QPlainTextEdit::keyPressEvent(event);
}

bool ChatInput::canInsertFromMimeData(const QMimeData *source) const {
  return source->hasText();
}

void ChatInput::insertFromMimeData(const QMimeData *source) {
  // Windows clipboards use CRLF line endings.
  auto text = source->text();
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  if (!pastePlainText_ && !isReadOnly() && QuotedMessage::shouldQuote(text)) {
    emit quotePasted(text);
    return;
  }
  insertPlainText(text);
}

int plainTextHeightForLines(const QPlainTextEdit *editor, int lines) {
  const auto margins = editor->contentsMargins();
  return lines * editor->fontMetrics().lineSpacing() +
         2 * int(editor->document()->documentMargin()) + margins.top() +
         margins.bottom() + 2 * editor->frameWidth();
}

void fitHeightToContent(QPlainTextEdit *editor, int minLines, int maxLines) {
  // QPlainTextEdit's layout reports the document height in visual lines.
  const int lines =
      int(editor->document()->documentLayout()->documentSize().height());
  const int height = plainTextHeightForLines(
      editor, std::clamp(lines, minLines, maxLines));
  if (height != editor->height()) {
    editor->setFixedHeight(height);
    editor->updateGeometry();
  }
}
