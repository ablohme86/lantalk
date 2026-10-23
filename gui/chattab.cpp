#include "gui/chattab.h"

#include "appconfig.h"
#include "gui/chatinput.h"
#include "gui/dialogs.h"
#include "protocol/chathistory.h"
#include "protocol/chatsession.h"
#include "protocol/quotedmessage.h"
#include "translationrestorer.h"

#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QColor>
#include <QFontDatabase>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSettings>
#include <QSaveFile>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextImageFormat>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <utility>

namespace {

QUrl attachmentActionUrl(const QString &action, const QString &path,
                         const QString &name = {}) {
  QUrl url;
  url.setScheme(QStringLiteral("lantalk"));
  url.setHost(action);
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("path"), path);
  if (!name.isEmpty()) query.addQueryItem(QStringLiteral("name"), name);
  url.setQuery(query);
  return url;
}

} // namespace

class ChatImagePreviewWidget final : public QWidget {
public:
  explicit ChatImagePreviewWidget(QWidget *parent = nullptr)
      : QWidget(parent), imageLabel_(new QLabel(this)),
        closeButton_(new QPushButton(QStringLiteral("Close"), this)) {
    setStyleSheet(QStringLiteral("background-color: #111; color: white;"));

    auto *layout = new QVBoxLayout(this);
    auto *header = new QHBoxLayout;
    header->addStretch();
    header->addWidget(closeButton_);
    layout->addLayout(header);

    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(imageLabel_, 1);
  }

  void setImage(const QPixmap &image) {
    image_ = image;
    updateImage();
  }

  QPushButton *closeButton() const { return closeButton_; }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    updateImage();
  }

private:
  void updateImage() {
    if (!image_.isNull()) {
      imageLabel_->setPixmap(image_.scaled(imageLabel_->size(),
                                           Qt::KeepAspectRatio,
                                           Qt::FastTransformation));
    }
  }

  QPixmap image_;
  QLabel *imageLabel_;
  QPushButton *closeButton_;
};

ChatTab::ChatTab(ChatSession *session, QWidget *parent)
    : QWidget(parent), session_(session) {
  setAcceptDrops(true);
  auto *layout = new QVBoxLayout(this);
  status_ = new QLabel(QStringLiteral("Performing nickname handshake…"), this);
  viewStack_ = new QStackedWidget(this);
  log_ = new QTextBrowser(this);
  log_->setReadOnly(true);
  log_->setOpenLinks(false);
  log_->setOpenExternalLinks(false);
  imageViewer_ = new ChatImagePreviewWidget(this);
  viewStack_->addWidget(log_);
  viewStack_->addWidget(imageViewer_);
  typingIndicator_ = new QLabel(this);
  typingIndicator_->setObjectName(QStringLiteral("typingIndicator"));
  typingIndicator_->setStyleSheet(
      QStringLiteral("color: rgb(150, 160, 168); font-style: italic;"));
  auto typingPolicy = typingIndicator_->sizePolicy();
  typingPolicy.setRetainSizeWhenHidden(true);
  typingIndicator_->setSizePolicy(typingPolicy);
  typingIndicator_->hide();
  typingAnimation_ = new QTimer(this);
  typingAnimation_->setInterval(400);
  auto *controls = new QHBoxLayout;
  input_ = new ChatInput(this);
  input_->setPlaceholderText(
      QStringLiteral("Message (Shift+Enter for a new line)"));
  translateButton_ = new QPushButton(QStringLiteral("Live Translate"), this);
  translateButton_->setCheckable(true);
  reconnectTimer_ = new QTimer(this);
  reconnectTimer_->setInterval(10000);
  auto *separator = new QFrame(this);
  separator->setFrameShape(QFrame::VLine);
  separator->setFrameShadow(QFrame::Sunken);
  auto *attach = new QPushButton(this);
  attach->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  attach->setToolTip(QStringLiteral("Attach file"));
  attach->setAccessibleName(QStringLiteral("Attach file"));
  controls->addWidget(input_);
  controls->addWidget(translateButton_);
  controls->addWidget(separator);
  controls->addWidget(attach);
  layout->addWidget(status_);
  layout->addWidget(viewStack_);
  layout->addWidget(typingIndicator_);
  quoteCards_ = new QWidget(this);
  quoteCardsLayout_ = new QVBoxLayout(quoteCards_);
  quoteCardsLayout_->setContentsMargins(0, 0, 0, 0);
  quoteCardsLayout_->setSpacing(4);
  quoteCards_->hide();
  layout->addWidget(quoteCards_);
  layout->addLayout(controls);
  translationRestorer_ = new TranslationRestorer(session_, this);
  refreshTranslationAvailability();

  connect(attach, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open file"));
    if (!path.isEmpty()) {
      session_->sendFile(path);
    }
  });
  connect(input_, &ChatInput::returnPressed, this, &ChatTab::send);
  // textEdited only fires for user edits, not for the status texts this tab
  // places in the input.
  connect(input_, &ChatInput::textEdited, this, &ChatTab::updateLocalTyping);
  connect(input_, &ChatInput::quotePasted, this, &ChatTab::addQuote);
  connect(session_, &ChatSession::peerTypingChanged, this,
          &ChatTab::updateTypingIndicator);
  connect(typingAnimation_, &QTimer::timeout, this, [this] {
    typingAnimationFrame_ = (typingAnimationFrame_ + 1) % 4;
    updateTypingIndicator();
  });
  connect(translateButton_, &QPushButton::toggled, this, [this](bool enabled) {
    if (enabled) {
      configureTranslation();
    } else {
      liveTranslate_ = false;
      automaticTranslationRequest_ = false;
      updateQuoteTranslation();
      translationRestorer_->disable();
      status_->setText(QStringLiteral("Live Translate is off."));
    }
  });
  connect(translationRestorer_, &TranslationRestorer::resumeRequested, this,
          [this](const QString &myLanguage, const QString &peerLanguage) {
            myLanguage_ = myLanguage;
            peerLanguage_ = peerLanguage;
            automaticTranslationRequest_ = true;
            status_->setText(QStringLiteral("Resuming Live Translate…"));
          });
  connect(translationRestorer_, &TranslationRestorer::statusChanged, status_,
          &QLabel::setText);
  connect(session_, &ChatSession::statusChanged, status_, &QLabel::setText);
  connect(session_, &ChatSession::peerIdentified, this,
          [this](const QString &name) {
            lastPeerName_ = name;
            hasPeerIdentity_ = true;
            if (!historyLoaded_) {
              historyLoaded_ = true;
              loadConversationHistory();
            }
            if (!connected_) {
              appendSystemMessage(QStringLiteral("Connected to %1").arg(name));
            }
            connected_ = true;
            reconnectTimer_->stop();
            input_->clear();
            input_->setReadOnly(false);
            auto inputFont = input_->font();
            inputFont.setItalic(false);
            input_->setFont(inputFont);
            input_->setEnabled(true);
            status_->setText(QStringLiteral("Connected to %1").arg(name));
            emit titleChanged();
          });
  connect(log_, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
    if (url.scheme() != QStringLiteral("lantalk")) {
      return;
    }
    const QString path =
        QUrlQuery(url).queryItemValue(QStringLiteral("path"));
    if (path.isEmpty()) return;
    if (url.host() == QStringLiteral("save-as")) {
      const QString name =
          QUrlQuery(url).queryItemValue(QStringLiteral("name"));
      saveAttachmentAs(path, name);
      return;
    }
    if (url.host() != QStringLiteral("view-image")) return;
    const QPixmap image(path);
    if (image.isNull()) {
      return;
    }
    imageViewer_->setImage(image);
    viewStack_->setCurrentWidget(imageViewer_);
  });
  connect(imageViewer_->closeButton(), &QPushButton::clicked, this,
          [this] { viewStack_->setCurrentWidget(log_); });
  connect(session_, &ChatSession::messageReceived, this, &ChatTab::received);
  connect(session_, &ChatSession::messageReceived, this,
          [this](const QString &, const QString &) {
            emit incomingMessageReceived();
          });
  connect(
      session_, &ChatSession::fileSent, this,
      [this](const QString &name, const QString &path, const QString &mime) {
        appendFile(QStringLiteral("You"), name, path, mime, false);
      });
  connect(
      session_, &ChatSession::fileReceived, this,
      [this](const QString &name, const QString &path, const QString &mime) {
        appendFile(session_->peerName(), name, path, mime, true);
        emit incomingMessageReceived();
      });
  connect(session_, &ChatSession::fileSendProgressChanged, this,
          [this](int percent) {
            if (!fileSendUiActive_) {
              fileSendPreviousInput_ = outgoingPending_ ? QString() : input_->text();
              fileSendUiActive_ = true;
            }
            input_->setText(QStringLiteral("Sending file... Please wait... %1%")
                                .arg(percent));
            input_->setReadOnly(true);
            auto font = input_->font();
            font.setItalic(true);
            input_->setFont(font);
          });
  connect(session_, &ChatSession::fileSendCompleted, this, [this] {
    if (!fileSendUiActive_) return;
    fileSendUiActive_ = false;
    if (outgoingPending_) {
      input_->setText(QStringLiteral("Translating message..."));
      input_->setReadOnly(true);
      auto font = input_->font();
      font.setItalic(true);
      input_->setFont(font);
      fileSendPreviousInput_.clear();
      return;
    }
    input_->setText(fileSendPreviousInput_);
    input_->setReadOnly(false);
    auto font = input_->font();
    font.setItalic(false);
    input_->setFont(font);
    fileSendPreviousInput_.clear();
  });
  connect(session_, &ChatSession::liveTranslationGranted, this, [this] {
    liveTranslate_ = true;
    automaticTranslationRequest_ = false;
    updateQuoteTranslation();
    {
      const QSignalBlocker blocker(translateButton_);
      translateButton_->setChecked(true);
    }
    status_->setText(QStringLiteral("Live Translate: %1 ↔ %2")
                         .arg(myLanguage_, peerLanguage_));
    translatePendingIncomingMessages();
  });
  connect(session_, &ChatSession::liveTranslationDenied, this,
          [this](const QString &message) {
            liveTranslate_ = false;
            updateQuoteTranslation();
            const QSignalBlocker blocker(translateButton_);
            translateButton_->setChecked(false);
            if (std::exchange(automaticTranslationRequest_, false)) {
              status_->setText(
                  QStringLiteral("Live Translate was not resumed: %1")
                      .arg(message));
              return;
            }
            QMessageBox::warning(
                this, QStringLiteral("Live Translate unavailable"), message);
          });
  connect(
      session_, &ChatSession::peerLiveTranslationChanged, this,
      [this](bool active, const QString &sourceLanguage,
             const QString &targetLanguage) {
        status_->setText(
            active ? QStringLiteral("%1 enabled Live Translate (%2 → %3).")
                         .arg(session_->peerName(), sourceLanguage,
                              targetLanguage)
                   : QStringLiteral("%1 disabled Live Translate.")
                         .arg(session_->peerName()));
      });
  connect(session_, &ChatSession::disconnected, this, [this] {
    if (connected_) {
      const auto peer = session_->peerName().isEmpty()
                            ? reconnectAddress_
                            : session_->peerName();
      appendSystemMessage(QStringLiteral("Disconnected from %1").arg(peer));
    }
    connected_ = false;
    if (hasPeerIdentity_) emit titleChanged();
    liveTranslate_ = false;
    automaticTranslationRequest_ = false;
    updateQuoteTranslation();
    {
      const QSignalBlocker blocker(translateButton_);
      translateButton_->setChecked(false);
    }
    status_->setText(reconnectAddress_.isEmpty()
                         ? QStringLiteral("Disconnected")
                         : QStringLiteral("Disconnected; retrying in 10 seconds"));
    input_->setText(
        QStringLiteral("Re-connect to this host to start chatting"));
    input_->setReadOnly(true);
    auto inputFont = input_->font();
    inputFont.setItalic(true);
    input_->setFont(inputFont);
    input_->setEnabled(false);
    if (!reconnectAddress_.isEmpty() && reconnectPort_ != 0 &&
        !reconnectTimer_->isActive()) {
      reconnectTimer_->start();
    }
  });
  connect(reconnectTimer_, &QTimer::timeout, this, &ChatTab::retryConnection);
  connect(&outgoingTranslator_, &MessageTranslator::translated, this,
          &ChatTab::outgoingTranslationFinished);
  connect(&outgoingTranslator_, &MessageTranslator::failed, this,
          [this](const QString &error, const QString &) {
            status_->setText(error);
            clearOutgoingTranslationStatus();
            outgoingPending_ = false;
            pendingOriginal_.clear();
            input_->setFocus(Qt::OtherFocusReason);
          });
  connect(&incomingTranslator_, &MessageTranslator::translated, this,
          &ChatTab::incomingTranslationFinished);
  connect(&incomingTranslator_, &MessageTranslator::failed, this,
          [this](const QString &error, const QString &originalMessage) {
            status_->setText(error);
            replaceTranslationStatus(originalMessage, originalMessage, false);
          });
}

QString ChatTab::title() const {
  const auto currentPeer = session_->peerName();
  const auto peer = currentPeer.isEmpty() ? lastPeerName_ : currentPeer;
  const auto address = session_->peerAddress();
  if (hasPeerIdentity_ && !connected_) {
    return QStringLiteral("%1 (Disconnected)").arg(peer);
  }
  if (peer.isEmpty()) {
    return address.isEmpty() ? QStringLiteral("Unknown host") : address;
  }
  return address.isEmpty() ? peer
                           : QStringLiteral("%1 (%2)").arg(peer, address);
}

ChatSession *ChatTab::session() const { return session_; }

QString ChatTab::reconnectAddress() const { return reconnectAddress_; }

quint16 ChatTab::reconnectPort() const { return reconnectPort_; }

void ChatTab::setReconnectEndpoint(const QString &address, quint16 port) {
  reconnectAddress_ = address.trimmed();
  reconnectPort_ = port;
}

void ChatTab::reconnectNow() {
  if (connected_ || !reconnectTimer_->isActive()) return;
  reconnectTimer_->stop();
  retryConnection();
}

void ChatTab::closeConversation() {
  reconnectTimer_->stop();
  session_->closeConnection();
}

void ChatTab::updateTypingIndicator() {
  if (!session_->peerTyping()) {
    typingAnimation_->stop();
    typingAnimationFrame_ = 0;
    typingIndicator_->hide();
    return;
  }
  const auto peer = session_->peerName().isEmpty() ? lastPeerName_
                                                   : session_->peerName();
  // Pad the dots so the label width stays constant while animating.
  const auto dots = QString(typingAnimationFrame_, u'.')
                        .leftJustified(3, QChar(0x2007));
  typingIndicator_->setText(
      QStringLiteral("%1 is typing a message%2").arg(peer, dots));
  typingIndicator_->show();
  if (!typingAnimation_->isActive()) typingAnimation_->start();
}

void ChatTab::updateLocalTyping() {
  session_->noteLocalTyping(!input_->text().trimmed().isEmpty() ||
                            !pendingQuotes_.isEmpty());
}

namespace {

// Surrounding blank lines and trailing spaces go; indentation stays.
QString trimmedQuote(QString text) {
  text.remove(QRegularExpression(QStringLiteral("^(\\s*\\n)+")));
  text.remove(QRegularExpression(QStringLiteral("\\s+$")));
  return text;
}

QString quoteSummary(const QString &text) {
  const int lines = int(text.count(u'\n')) + 1;
  return QStringLiteral("Quote · %1 line%2")
      .arg(lines)
      .arg(lines == 1 ? QString() : QStringLiteral("s"));
}

} // namespace

void ChatTab::addQuote(const QString &pastedText) {
  const auto text = trimmedQuote(pastedText);
  if (text.isEmpty()) return;

  // The quote stays editable until it is sent.
  auto *card = new QFrame(quoteCards_);
  card->setFrameShape(QFrame::StyledPanel);
  auto *cardLayout = new QVBoxLayout(card);
  cardLayout->setContentsMargins(6, 2, 2, 4);
  cardLayout->setSpacing(2);
  auto *header = new QHBoxLayout;
  auto *summary = new QLabel(quoteSummary(text), card);
  auto *translate = new QCheckBox(QStringLiteral("Translate this"), card);
  auto *remove = new QToolButton(card);
  remove->setText(QStringLiteral("✕"));
  remove->setAutoRaise(true);
  remove->setToolTip(QStringLiteral("Remove this quote"));
  header->addWidget(summary, 1);
  header->addWidget(translate);
  header->addWidget(remove);
  auto *editor = new QPlainTextEdit(card);
  editor->setObjectName(QStringLiteral("quoteEditor"));
  editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  editor->setTabChangesFocus(true);
  editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  editor->setPlainText(text);
  cardLayout->addLayout(header);
  cardLayout->addWidget(editor);
  quoteCardsLayout_->addWidget(card);
  quoteCards_->show();
  pendingQuotes_.append({card, translate, editor});

  const auto fit = [editor] { fitHeightToContent(editor, 3, 12); };
  connect(editor->document()->documentLayout(),
          &QAbstractTextDocumentLayout::documentSizeChanged, editor, fit);
  fit();
  connect(editor, &QPlainTextEdit::textChanged, this, [this, editor, summary] {
    summary->setText(quoteSummary(editor->toPlainText()));
    updateLocalTyping();
  });
  connect(remove, &QToolButton::clicked, this,
          [this, card] { removeQuote(card); });
  updateQuoteTranslation();
  updateLocalTyping();
  input_->setFocus(Qt::OtherFocusReason);
}

void ChatTab::removeQuote(QWidget *card) {
  for (int index = 0; index < pendingQuotes_.size(); ++index) {
    if (pendingQuotes_.at(index).card != card) continue;
    pendingQuotes_.removeAt(index);
    card->deleteLater();
    break;
  }
  quoteCards_->setVisible(!pendingQuotes_.isEmpty());
  updateLocalTyping();
}

void ChatTab::clearQuotes() {
  for (const auto &quote : std::as_const(pendingQuotes_)) {
    quote.card->deleteLater();
  }
  pendingQuotes_.clear();
  quoteCards_->hide();
}

void ChatTab::updateQuoteTranslation() {
  // Quotes are sent as they are unless Live Translate translates them.
  for (const auto &quote : std::as_const(pendingQuotes_)) {
    quote.translate->setEnabled(liveTranslate_);
    quote.translate->setToolTip(
        liveTranslate_
            ? QStringLiteral("Translate this quote too; otherwise it is "
                             "sent as it is.")
            : QStringLiteral("Turn on Live Translate to translate quotes."));
  }
}

void ChatTab::insertMessageBody(QTextCursor &cursor, const QString &message,
                                const QTextCharFormat &format) {
  QTextBlockFormat quoteBlock;
  quoteBlock.setLeftMargin(12);
  quoteBlock.setTopMargin(2);
  quoteBlock.setBottomMargin(2);
  quoteBlock.setBackground(QColor(127, 127, 127, 40));
  auto quoteFormat = format;
  quoteFormat.setFontFamilies(
      {QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
  bool first = true;
  for (const auto &segment : QuotedMessage::parse(message)) {
    if (segment.quote) {
      cursor.insertBlock(quoteBlock, quoteFormat);
      // Line breaks become blocks that keep the quote's block format.
      cursor.insertText(segment.text, quoteFormat);
    } else {
      if (!first) cursor.insertBlock(QTextBlockFormat(), format);
      cursor.insertText(segment.text, format);
    }
    first = false;
  }
}

void ChatTab::retryConnection() {
  if (connected_ || reconnectAddress_.isEmpty() || reconnectPort_ == 0) {
    return;
  }
  status_->setText(QStringLiteral("Reconnecting to %1…")
                       .arg(reconnectAddress_));
  session_->reconnectToHost(reconnectAddress_, reconnectPort_);
}

void ChatTab::appendSystemMessage(const QString &message) {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat format;
  format.setFontItalic(true);
  format.setFontUnderline(true);
  format.setForeground(QColor(150, 160, 168));
  cursor.insertText(QStringLiteral("* %1").arg(message), format);
  cursor.insertBlock();
  log_->setTextCursor(cursor);
}

void ChatTab::refreshTranslationAvailability() {
  const auto available = isOllamaConfigured();
  if (!available && translateButton_->isChecked()) {
    const QSignalBlocker blocker(translateButton_);
    translateButton_->setChecked(false);
    liveTranslate_ = false;
    session_->stopLiveTranslation();
  }
  translateButton_->setVisible(available);
  if (available) translationRestorer_->retry();
}

void ChatTab::configureTranslation() {
  TranslationDialog dialog(this);
  const auto saved = translationRestorer_->savedPreference();
  if (!saved.myLanguage.isEmpty()) {
    dialog.setLanguages(saved.myLanguage, saved.peerLanguage);
  } else if (!myLanguage_.isEmpty()) {
    dialog.setLanguages(myLanguage_, peerLanguage_);
  }
  if (dialog.exec() == QDialog::Accepted) {
    myLanguage_ = dialog.myLanguage();
    peerLanguage_ = dialog.peerLanguage();
    automaticTranslationRequest_ = false;
    translationRestorer_->request(myLanguage_, peerLanguage_);
  } else {
    const QSignalBlocker blocker(translateButton_);
    translateButton_->setChecked(false);
  }
}

void ChatTab::send() {
  if (fileSendUiActive_) {
    return;
  }
  // Surrounding blank lines and trailing spaces go; the indentation of
  // pasted code stays.
  auto text = input_->text();
  text.remove(QRegularExpression(QStringLiteral("^(\\s*\\n)+")));
  text.remove(QRegularExpression(QStringLiteral("\\s+$")));
  if ((text.trimmed().isEmpty() && pendingQuotes_.isEmpty()) ||
      outgoingPending_) {
    return;
  }
  // The typed text first, then the quotes; quotes ticked "Translate this"
  // keep their mark until the translator has seen it.
  QList<QuotedMessage::Segment> segments;
  if (!text.trimmed().isEmpty()) segments.append({text});
  for (const auto &quote : std::as_const(pendingQuotes_)) {
    // A quote emptied while editing is left out.
    const auto quoted = trimmedQuote(quote.editor->toPlainText());
    if (quoted.isEmpty()) continue;
    segments.append({quoted, true,
                     quote.translate->isEnabled() &&
                         quote.translate->isChecked()});
  }
  if (segments.isEmpty()) return;
  const auto draft = QuotedMessage::compose(segments, true);
  input_->clear();
  clearQuotes();
  session_->stopLocalTyping();
  if (!liveTranslate_) {
    sendText(QuotedMessage::withoutTranslateFlags(draft));
    return;
  }
  text = draft;
  outgoingPending_ = true;
  pendingOriginal_ = QuotedMessage::withoutTranslateFlags(draft);
  input_->setText(QStringLiteral("Translating message..."));
  input_->setReadOnly(true);
  auto inputFont = input_->font();
  inputFont.setItalic(true);
  input_->setFont(inputFont);
  outgoingTranslator_.translate(text, myLanguage_, peerLanguage_);
}

void ChatTab::received(const QString &message, const QString &originalMessage) {
  const auto timestamp = QDateTime::currentDateTime();
  const auto cursor = appendMessage(session_->peerName(), message,
                                    originalMessage,
                                    QStringLiteral("Original"), false,
                                    timestamp);
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      session_->peerName(), message, timestamp);
  const int index = incomingMessages_.size();
  incomingMessages_.append(
      {message, cursor, !originalMessage.isEmpty(), false});
  if (liveTranslate_ && originalMessage.isEmpty()) {
    translateIncomingMessage(index);
  }
}

void ChatTab::translatePendingIncomingMessages() {
  for (int index = 0; index < incomingMessages_.size(); ++index) {
    translateIncomingMessage(index);
  }
}

void ChatTab::translateIncomingMessage(int index) {
  if (!liveTranslate_ || index < 0 || index >= incomingMessages_.size()) {
    return;
  }
  auto &incoming = incomingMessages_[index];
  if (incoming.translated || incoming.pending) return;

  incoming.pending = true;
  setIncomingMessageText(incoming,
                         QStringLiteral("Translating incoming message..."),
                         false, true);
  incomingTranslator_.translate(incoming.originalMessage, peerLanguage_,
                                myLanguage_);
}

void ChatTab::outgoingTranslationFinished(const QString &translation,
                                          const QString &) {
  outgoingPending_ = false;
  clearOutgoingTranslationStatus();
  const auto timestamp = QDateTime::currentDateTime();
  session_->sendMessage(translation, pendingOriginal_);
  appendMessage(QStringLiteral("You"), pendingOriginal_, translation,
                QStringLiteral("Sent translation"), false, timestamp);
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      QStringLiteral("You"), pendingOriginal_, timestamp);
  pendingOriginal_.clear();
  input_->setFocus(Qt::OtherFocusReason);
}

void ChatTab::incomingTranslationFinished(const QString &translation,
                                          const QString &originalMessage) {
  replaceTranslationStatus(originalMessage, translation, true);
}

void ChatTab::sendText(const QString &text) {
  const auto timestamp = QDateTime::currentDateTime();
  session_->sendMessage(text);
  appendMessage(QStringLiteral("You"), text, {}, QStringLiteral("Original"),
                false, timestamp);
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      QStringLiteral("You"), text, timestamp);
  input_->setFocus(Qt::OtherFocusReason);
}

void ChatTab::dragEnterEvent(QDragEnterEvent *event) {
  if (event->mimeData()->hasUrls()) {
    event->acceptProposedAction();
  }
}

void ChatTab::dropEvent(QDropEvent *event) {
  for (const auto &url : event->mimeData()->urls()) {
    if (url.isLocalFile()) {
      session_->sendFile(url.toLocalFile());
    }
  }
  event->acceptProposedAction();
}

QTextCursor ChatTab::appendMessage(const QString &sender,
                                   const QString &message,
                                   const QString &hoverText,
                                   const QString &hoverLabel,
                                   bool historical,
                                   const QDateTime &timestamp) {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
  const auto timestampText = ChatHistory::timestampPrefix(timestamp);
  if (!timestampText.isEmpty()) {
    QTextCharFormat timestampFormat;
    timestampFormat.setFontItalic(historical);
    timestampFormat.setForeground(QColor(150, 160, 168));
    cursor.insertText(timestampText, timestampFormat);
  }
  QTextCharFormat senderFormat;
  senderFormat.setFontWeight(QFont::Bold);
  senderFormat.setFontItalic(historical);
  cursor.insertText(sender + QStringLiteral(": "), senderFormat);
  QTextCharFormat messageFormat;
  messageFormat.setFontItalic(historical);
  if (!hoverText.isEmpty()) {
    messageFormat.setToolTip(
        QStringLiteral("%1: %2").arg(hoverLabel, hoverText));
  }
  const int messageStart = cursor.position();
  insertMessageBody(cursor, message, messageFormat);
  const int messageEnd = cursor.position();
  cursor.insertBlock(QTextBlockFormat());
  log_->setTextCursor(cursor);

  QTextCursor messageCursor(log_->document());
  messageCursor.setPosition(messageStart);
  messageCursor.setPosition(messageEnd, QTextCursor::KeepAnchor);
  return messageCursor;
}

void ChatTab::loadConversationHistory() {
  const auto entries = ChatHistory::load(session_->peerAddress(),
                                         session_->peerName(), 20);
  if (entries.isEmpty()) return;

  for (const auto &entry : entries) {
    appendMessage(entry.sender, entry.message, {},
                  QStringLiteral("Original"), true, entry.timestamp);
  }
  appendHistorySeparator();
}

void ChatTab::appendHistorySeparator() {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat format;
  format.setFontItalic(true);
  format.setForeground(QColor(150, 160, 168));
  cursor.insertText(QStringLiteral("──────── Earlier messages ────────"),
                    format);
  cursor.insertBlock();
  log_->setTextCursor(cursor);
}

void ChatTab::setIncomingMessageText(IncomingMessage &incoming,
                                     const QString &text, bool translated,
                                     bool pending) {
  QTextCharFormat format;
  format.setFontItalic(pending);
  if (translated) {
    format.setToolTip(QStringLiteral("Original: %1")
                          .arg(incoming.originalMessage));
  }
  auto &cursor = incoming.messageCursor;
  const int start = cursor.selectionStart();
  cursor.removeSelectedText();
  insertMessageBody(cursor, text, format);
  const int end = cursor.position();
  cursor.setPosition(start);
  cursor.setPosition(end, QTextCursor::KeepAnchor);
  incoming.translated = translated;
  incoming.pending = pending;
}

void ChatTab::replaceTranslationStatus(const QString &originalMessage,
                                      const QString &replacement,
                                      bool succeeded) {
  for (auto &incoming : incomingMessages_) {
    if (!incoming.pending || incoming.originalMessage != originalMessage) {
      continue;
    }
    setIncomingMessageText(incoming, replacement, succeeded, false);
    auto endCursor = log_->textCursor();
    endCursor.movePosition(QTextCursor::End);
    log_->setTextCursor(endCursor);
    return;
  }
}

void ChatTab::clearOutgoingTranslationStatus() {
  if (fileSendUiActive_) {
    return;
  }
  input_->clear();
  input_->setReadOnly(false);
  auto inputFont = input_->font();
  inputFont.setItalic(false);
  input_->setFont(inputFont);
}

void ChatTab::saveAttachmentAs(const QString &path,
                               const QString &suggestedName) {
  const auto destination = QFileDialog::getSaveFileName(
      this, QStringLiteral("Save as..."),
      suggestedName.isEmpty() ? QFileInfo(path).fileName() : suggestedName);
  if (destination.isEmpty() ||
      QFileInfo(destination).absoluteFilePath() ==
          QFileInfo(path).absoluteFilePath()) {
    return;
  }
  if (QFileInfo::exists(destination)) {
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Overwrite file?"),
        QStringLiteral("The selected file already exists. Overwrite it?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
  }
  QFile source(path);
  QSaveFile target(destination);
  if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
    target.cancelWriting();
    QMessageBox::warning(this, QStringLiteral("Save failed"),
                         QStringLiteral("Cannot save the selected file."));
    return;
  }
  while (!source.atEnd()) {
    const auto bytes = source.read(64 * 1024);
    if ((bytes.isEmpty() && source.error() != QFileDevice::NoError) ||
        target.write(bytes) != bytes.size()) {
      target.cancelWriting();
      QMessageBox::warning(this, QStringLiteral("Save failed"),
                           QStringLiteral("Cannot save the selected file."));
      return;
    }
  }
  if (!target.commit()) {
    QMessageBox::warning(this, QStringLiteral("Save failed"),
                         QStringLiteral("Cannot save the selected file."));
  }
}

void ChatTab::appendFile(const QString &sender, const QString &name,
                         const QString &path, const QString &mimeType,
                         bool received) {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
  if (!cursor.block().text().isEmpty()) {
    cursor.insertBlock();
  }
  QTextCharFormat senderFormat;
  senderFormat.setFontWeight(QFont::Bold);
  cursor.insertText(sender + QStringLiteral(" sent: "), senderFormat);
  QTextCharFormat nameFormat;
  nameFormat.setFontItalic(true);
  cursor.insertText(name, nameFormat);
  if (received) {
    QTextCharFormat saveFormat;
    saveFormat.setAnchor(true);
    saveFormat.setAnchorHref(
        attachmentActionUrl(QStringLiteral("save-as"), path, name).toString());
    saveFormat.setFontUnderline(true);
    cursor.insertText(QStringLiteral(" (Save as...)"), saveFormat);
  }
  cursor.insertBlock();
  if (mimeType.startsWith(QStringLiteral("image/"))) {
    QTextImageFormat thumbnail;
    thumbnail.setName(QUrl::fromLocalFile(path).toString());
    thumbnail.setWidth(360);
    thumbnail.setAnchor(true);
    thumbnail.setAnchorHref(
        attachmentActionUrl(QStringLiteral("view-image"), path).toString());
    thumbnail.setToolTip(QStringLiteral("Click to view image in chat"));
    cursor.insertImage(thumbnail);
    cursor.insertBlock();
  }
  log_->setTextCursor(cursor);
}
