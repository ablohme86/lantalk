#include "gui/chatlog.h"

#include "protocol/quotedmessage.h"

#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QScrollBar>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// iMessage-like colours: blue for sent, gray for received.
const auto SentColor = QStringLiteral("#0b84fe");
const auto SentQuoteColor = QStringLiteral("#0a6ed6");
const auto MetaColor = QStringLiteral("#8e8e93");
constexpr int BubblePaddingX = 11;
constexpr int BubblePaddingY = 7;
constexpr int ThumbnailWidth = 360;

QString plainMessage(const QString &message) {
  QStringList parts;
  for (const auto &segment : QuotedMessage::parse(message)) {
    parts << segment.text;
  }
  return parts.join(u'\n');
}

} // namespace

ChatLog::ChatLog(QWidget *parent) : QScrollArea(parent) {
  setWidgetResizable(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFrameShape(QFrame::StyledPanel);
  content_ = new QWidget(this);
  content_->setBackgroundRole(QPalette::Base);
  content_->setAutoFillBackground(true);
  rows_ = new QVBoxLayout(content_);
  rows_->setContentsMargins(0, 6, 0, 6);
  rows_->setSpacing(4);
  rows_->addStretch();
  setWidget(content_);

  // Stay at the newest message unless the user has scrolled up to read.
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
    followBottom_ = value >= verticalScrollBar()->maximum() - 4;
  });
  connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int maximum) {
            if (followBottom_) verticalScrollBar()->setValue(maximum);
          });
}

int ChatLog::appendMessage(bool outgoing, const QString &message,
                           const QDateTime &timestamp, bool historical,
                           const QString &toolTip) {
  // Sending always brings the conversation back to the newest message.
  if (outgoing && !historical) followBottom_ = true;
  auto *bubble = addBubble(outgoing, timestamp, historical);
  bubble->setText(messageHtml(message, outgoing));
  bubble->setToolTip(toolTip);
  bubble->setProperty("plainText", plainMessage(message));
  fitBubble(bubble);
  const int id = nextId_++;
  messages_.insert(id, bubble);
  return id;
}

void ChatLog::setMessage(int id, const QString &message, bool pending,
                         const QString &toolTip) {
  auto *bubble = messages_.value(id);
  if (!bubble) return;
  auto font = bubble->font();
  font.setItalic(pending || bubble->property("historical").toBool());
  bubble->setFont(font);
  bubble->setText(messageHtml(message, bubble->property("outgoing").toBool()));
  bubble->setToolTip(toolTip);
  bubble->setProperty("plainText", plainMessage(message));
  fitBubble(bubble);
}

void ChatLog::appendFile(bool outgoing, const QString &name,
                         const QString &path, const QString &mimeType,
                         const QDateTime &timestamp) {
  QLabel *thumbnail = nullptr;
  if (mimeType.startsWith(QStringLiteral("image/"))) {
    const QPixmap image(path);
    if (!image.isNull()) {
      thumbnail = new QLabel;
      thumbnail->setPixmap(image.width() > ThumbnailWidth
                               ? image.scaledToWidth(ThumbnailWidth,
                                                     Qt::SmoothTransformation)
                               : image);
      thumbnail->setCursor(Qt::PointingHandCursor);
      thumbnail->setToolTip(QStringLiteral("Click to view the image"));
      thumbnail->setProperty("imagePath", path);
      thumbnail->installEventFilter(this);
    }
  }
  if (outgoing) followBottom_ = true;
  auto *bubble = addBubble(outgoing, timestamp, false, thumbnail);
  auto html = QStringLiteral("📎 <i>%1</i>").arg(name.toHtmlEscaped());
  if (!outgoing) {
    html += QStringLiteral(" &nbsp;<a href=\"save\">Save as...</a>");
    connect(bubble, &QLabel::linkActivated, this, [this, path, name] {
      emit saveAttachmentRequested(path, name);
    });
  }
  bubble->setText(html);
  bubble->setProperty("plainText", name);
  fitBubble(bubble);
}

void ChatLog::appendNotice(const QString &text) {
  auto *notice = new QLabel(text);
  notice->setAlignment(Qt::AlignCenter);
  notice->setWordWrap(true);
  notice->setStyleSheet(QStringLiteral("color: %1; padding: 4px 8px;").arg(MetaColor));
  auto font = notice->font();
  font.setItalic(true);
  font.setPointSizeF(font.pointSizeF() * 0.9);
  notice->setFont(font);
  addRow(notice, text);
}

QString ChatLog::toPlainText() const {
  QStringList lines;
  for (int index = 0; index < rows_->count(); ++index) {
    if (auto *row = rows_->itemAt(index)->widget()) {
      lines << row->property("plainText").toString();
    }
  }
  return lines.join(u'\n');
}

void ChatLog::resizeEvent(QResizeEvent *event) {
  QScrollArea::resizeEvent(event);
  for (auto *bubble : std::as_const(bubbles_)) fitBubble(bubble);
}

bool ChatLog::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::MouseButtonRelease &&
      static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
    const auto path = watched->property("imagePath").toString();
    if (!path.isEmpty()) {
      emit imageRequested(path);
      return true;
    }
  }
  return QScrollArea::eventFilter(watched, event);
}

QLabel *ChatLog::addBubble(bool outgoing, const QDateTime &timestamp,
                           bool historical, QWidget *below) {
  auto *bubble = new QLabel;
  bubble->setTextFormat(Qt::RichText);
  bubble->setWordWrap(true);
  bubble->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                  Qt::LinksAccessibleByMouse);
  bubble->setOpenExternalLinks(false);
  bubble->setProperty("outgoing", outgoing);
  bubble->setProperty("historical", historical);
  const bool dark = darkTheme();
  const auto background =
      outgoing ? SentColor : QString::fromLatin1(dark ? "#3a3a3c" : "#e9e9eb");
  const auto foreground =
      outgoing ? QStringLiteral("#ffffff")
               : QString::fromLatin1(dark ? "#ffffff" : "#000000");
  bubble->setStyleSheet(
      QStringLiteral("QLabel { background-color: %1; color: %2; "
                     "border-radius: 14px; padding: %3px %4px; }")
          .arg(background, foreground)
          .arg(BubblePaddingY)
          .arg(BubblePaddingX));
  if (historical) {
    auto font = bubble->font();
    font.setItalic(true);
    bubble->setFont(font);
  }
  bubbles_.append(bubble);

  auto *meta = new QLabel(timestamp.isValid()
                              ? timestamp.toLocalTime().toString(
                                    QStringLiteral("HH:mm"))
                              : QString());
  auto metaFont = meta->font();
  metaFont.setPointSizeF(metaFont.pointSizeF() * 0.8);
  meta->setFont(metaFont);
  meta->setStyleSheet(QStringLiteral("color: %1; padding: 0px 6px;").arg(MetaColor));

  const auto side = outgoing ? Qt::AlignRight : Qt::AlignLeft;
  auto *column = new QWidget;
  auto *columnLayout = new QVBoxLayout(column);
  columnLayout->setContentsMargins(0, 0, 0, 0);
  columnLayout->setSpacing(2);
  columnLayout->addWidget(bubble, 0, side);
  if (below) columnLayout->addWidget(below, 0, side);
  columnLayout->addWidget(meta, 0, side);

  auto *row = new QWidget;
  auto *rowLayout = new QHBoxLayout(row);
  rowLayout->setContentsMargins(10, 0, 10, 0);
  if (outgoing) rowLayout->addStretch();
  rowLayout->addWidget(column);
  if (!outgoing) rowLayout->addStretch();
  // fitBubble() keeps the row's text for toPlainText() up to date.
  addRow(row, {});
  return bubble;
}

void ChatLog::addRow(QWidget *row, const QString &plainText) {
  if (!plainText.isEmpty()) row->setProperty("plainText", plainText);
  // Before the trailing stretch, which keeps the rows at the top.
  rows_->insertWidget(rows_->count() - 1, row);
}

void ChatLog::fitBubble(QLabel *bubble) const {
  // A word-wrapped QLabel guesses its width; size it to the text's natural
  // width instead, wrapping only beyond the maximum.
  QTextDocument document;
  document.setDefaultFont(bubble->font());
  document.setDocumentMargin(0);
  document.setHtml(bubble->text());
  const int natural = int(std::ceil(document.idealWidth())) + 2 * BubblePaddingX + 4;
  const int width = std::min(natural, bubbleMaximumWidth());
  // Aligned layout items get their preferred height, which a QLabel computes
  // for a width of its own choosing; set the height for this width instead.
  document.setTextWidth(width - 2 * BubblePaddingX - 4);
  const int height = int(std::ceil(document.size().height())) + 2 * BubblePaddingY + 2;
  bubble->setFixedSize(width, height);
  if (auto *row = bubble->parentWidget() ? bubble->parentWidget()->parentWidget() : nullptr) {
    row->setProperty("plainText", bubble->property("plainText"));
  }
}

int ChatLog::bubbleMaximumWidth() const {
  return std::max(160, viewport()->width() * 3 / 4);
}

QString ChatLog::messageHtml(const QString &message, bool outgoing) const {
  const auto quoteColor =
      outgoing ? SentQuoteColor : QString::fromLatin1(darkTheme() ? "#4a4a4e" : "#d5d5d9");
  const auto monospace = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  QString html;
  for (const auto &segment : QuotedMessage::parse(message)) {
    const auto text = segment.text.toHtmlEscaped();
    if (segment.quote) {
      html += QStringLiteral("<table width=\"100%\" cellpadding=\"5\" cellspacing=\"0\" "
                             "bgcolor=\"%1\" style=\"margin-top: 3px; margin-bottom: 3px;\">"
                             "<tr><td style=\"font-family: '%2'; white-space: pre-wrap;\">%3"
                             "</td></tr></table>")
                  .arg(quoteColor, monospace, text);
    } else {
      html += QStringLiteral("<div style=\"white-space: pre-wrap;\">%1</div>").arg(text);
    }
  }
  return html;
}

bool ChatLog::darkTheme() const {
  return palette().color(QPalette::Base).lightness() < 128;
}
