#pragma once

#include <QPlainTextEdit>

// The chat tab's message editor: Enter sends, Shift+Enter starts a new line,
// and pasted text keeps its line breaks. Multi-line or long pastes are handed
// to the chat tab as quotes instead (Ctrl+Shift+V pastes them as plain text).
// It grows with its content up to a few lines and scrolls beyond that.
// Mirrors the parts of QLineEdit's API the chat tab uses.
class ChatInput final : public QPlainTextEdit {
  Q_OBJECT

public:
  explicit ChatInput(QWidget *parent = nullptr);
  QString text() const;
  // Replaces the content without emitting textEdited().
  void setText(const QString &text);
  void clear();
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

signals:
  void returnPressed();
  // Emitted only for edits made by the user.
  void textEdited(const QString &text);
  void quotePasted(const QString &text);

protected:
  void keyPressEvent(QKeyEvent *event) override;
  bool canInsertFromMimeData(const QMimeData *source) const override;
  void insertFromMimeData(const QMimeData *source) override;

private:
  int heightForLines(int lines) const;
  void updateHeight();

  bool settingText_ = false;
  bool pastePlainText_ = false;
};
