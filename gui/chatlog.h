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
  // With `replacing`, the attachment takes that message's place, e.g. its
  // transfer progress.
  void appendFile(bool outgoing, const QString &name, const QString &path,
                  const QString &mimeType, const QDateTime &timestamp,
                  int replacing = -1);
  void appendNotice(const QString &text);
  // A "Show earlier messages" link at the top; clicking it emits
  // earlierMessagesRequested().
  void setEarlierMessagesAvailable(bool available);
  struct HistoryMessage {
    bool outgoing;
    QString message;
    QDateTime timestamp;
    QString toolTip;
  };
  // Older history above everything shown, oldest first, keeping the view
  // where it is.
  void prependHistory(const QList<HistoryMessage> &messages);
  // The visible text, one entry per line; used by tests.
  QString toPlainText() const;

signals:
  void saveAttachmentRequested(const QString &path, const QString &name);
  void imageRequested(const QString &path);
  void earlierMessagesRequested();

protected:
  void resizeEvent(QResizeEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  int addMessage(bool outgoing, const QString &message,
                 const QDateTime &timestamp, bool historical,
                 const QString &toolTip, int position);
  QLabel *addBubble(bool outgoing, const QDateTime &timestamp,
                    bool historical, QWidget *below = nullptr,
                    int position = -1);
  void addRow(QWidget *row, const QString &plainText, int position = -1);
  // The row holding a message from appendMessage().
  QWidget *messageRow(int id) const;
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
  // While older messages go in above: the distance to keep from the bottom.
  int keepFromBottom_ = -1;
  QLabel *earlierLink_ = nullptr;
};
