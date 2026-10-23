#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QScrollArea>

class QLabel;
class QVBoxLayout;

// The conversation view: messages as chat bubbles, received ones on the left
// in gray and sent ones on the right in blue, with the time below each.
// Quotes render as monospace blocks inside the bubble. Connection notices
// and the history divider are centred lines between the bubbles.
class ChatLog final : public QScrollArea {
  Q_OBJECT

public:
  explicit ChatLog(QWidget *parent = nullptr);
  // Returns an id for replacing the text later (Live Translate).
  int appendMessage(bool outgoing, const QString &message,
                    const QDateTime &timestamp, bool historical = false,
                    const QString &toolTip = {});
  // pending shows the text as a status (italic), e.g. while translating.
  void setMessage(int id, const QString &message, bool pending,
                  const QString &toolTip);
  void appendFile(bool outgoing, const QString &name, const QString &path,
                  const QString &mimeType, const QDateTime &timestamp);
  void appendNotice(const QString &text);
  // The visible text, one entry per line; used by tests.
  QString toPlainText() const;

signals:
  void saveAttachmentRequested(const QString &path, const QString &name);
  void imageRequested(const QString &path);

protected:
  void resizeEvent(QResizeEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  QLabel *addBubble(bool outgoing, const QDateTime &timestamp,
                    bool historical, QWidget *below = nullptr);
  void addRow(QWidget *row, const QString &plainText);
  void fitBubble(QLabel *bubble) const;
  int bubbleMaximumWidth() const;
  QString messageHtml(const QString &message, bool outgoing) const;
  bool darkTheme() const;

  QWidget *content_;
  QVBoxLayout *rows_;
  QHash<int, QLabel *> messages_;
  QList<QLabel *> bubbles_;
  int nextId_ = 0;
  bool followBottom_ = true;
};
