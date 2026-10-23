#include "gui/chatinput.h"

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>

#include <algorithm>

namespace {

constexpr int MaximumVisibleLines = 6;

} // namespace

ChatInput::ChatInput(QWidget *parent) : QPlainTextEdit(parent) {
  setTabChangesFocus(true);
  setLineWrapMode(QPlainTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(this, &QPlainTextEdit::textChanged, this, [this] {
    if (!settingText_) emit textEdited(text());
  });
  // QPlainTextEdit's layout reports the document height in visual lines, and
  // updates it when wrapping changes too.
  connect(document()->documentLayout(),
          &QAbstractTextDocumentLayout::documentSizeChanged, this,
          &ChatInput::updateHeight);
  updateHeight();
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
  return {QPlainTextEdit::minimumSizeHint().width(), heightForLines(1)};
}

void ChatInput::keyPressEvent(QKeyEvent *event) {
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

int ChatInput::heightForLines(int lines) const {
  const auto margins = contentsMargins();
  return lines * fontMetrics().lineSpacing() +
         2 * int(document()->documentMargin()) + margins.top() +
         margins.bottom() + 2 * frameWidth();
}

void ChatInput::updateHeight() {
  const int lines =
      int(document()->documentLayout()->documentSize().height());
  const int height =
      heightForLines(std::clamp(lines, 1, MaximumVisibleLines));
  if (height != this->height()) {
    setFixedHeight(height);
    updateGeometry();
  }
}
