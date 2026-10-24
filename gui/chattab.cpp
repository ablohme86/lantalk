#include "gui/chattab.h"

#include "appconfig.h"
#include "gui/chatinput.h"
#include "gui/chatlog.h"
#include "gui/dialogs.h"
#include "ollamadiscovery.h"
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
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

class ChatImagePreviewWidget final : public QWidget {
public:
  explicit ChatImagePreviewWidget(QWidget *parent = nullptr)
      : QWidget(parent), imageLabel_(new QLabel(this)),
        closeButton_(new QPushButton(tr("Close"), this)) {
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
  status_ = new QLabel(tr("Performing nickname handshake…"), this);
  viewStack_ = new QStackedWidget(this);
  log_ = new ChatLog(this);
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
      tr("Message (Shift+Enter for a new line)"));
  translateButton_ = new QPushButton(tr("Live Translate"), this);
  translateButton_->setCheckable(true);
  reconnectTimer_ = new QTimer(this);
  reconnectTimer_->setInterval(10000);
  auto *separator = new QFrame(this);
  separator->setFrameShape(QFrame::VLine);
  separator->setFrameShadow(QFrame::Sunken);
  auto *attach = new QPushButton(this);
  attach->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  attach->setToolTip(tr("Attach file"));
  attach->setAccessibleName(tr("Attach file"));
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
        this, tr("Open file"));
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
      status_->setText(tr("Live Translate is off."));
    }
  });
  connect(translationRestorer_, &TranslationRestorer::resumeRequested, this,
          [this](const QString &myLanguage, const QString &peerLanguage) {
            myLanguage_ = myLanguage;
            peerLanguage_ = peerLanguage;
            automaticTranslationRequest_ = true;
            status_->setText(tr("Resuming Live Translate…"));
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
              appendSystemMessage(tr("Connected to %1").arg(name));
            }
            connected_ = true;
            reconnectTimer_->stop();
            input_->clear();
            input_->setReadOnly(false);
            auto inputFont = input_->font();
            inputFont.setItalic(false);
            input_->setFont(inputFont);
            input_->setEnabled(true);
            status_->setText(tr("Connected to %1").arg(name));
            emit titleChanged();
          });
  connect(log_, &ChatLog::earlierMessagesRequested, this,
          &ChatTab::showEarlierHistory);
  connect(log_, &ChatLog::saveAttachmentRequested, this,
          &ChatTab::saveAttachmentAs);
  connect(log_, &ChatLog::imageRequested, this, [this](const QString &path) {
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
  connect(session_, &ChatSession::fileSent, this,
          [this](const QString &name, const QString &path,
                 const QString &mime, const QString &id) {
            appendFile(id, name, path, mime, false);
          });
  connect(session_, &ChatSession::fileReceived, this,
          [this](const QString &name, const QString &path,
                 const QString &mime, const QString &id) {
            appendFile(id, name, path, mime, true);
            emit incomingMessageReceived();
            emit notifyIncoming(session_->peerName(),
                                tr("Sent a file: %1").arg(name));
          });
  connect(session_, &ChatSession::fileTransferProgress, this,
          &ChatTab::showTransferProgress);
  connect(session_, &ChatSession::fileTransferFailed, this,
          &ChatTab::showTransferFailure);
  connect(session_, &ChatSession::liveTranslationGranted, this, [this] {
    liveTranslate_ = true;
    automaticTranslationRequest_ = false;
    updateQuoteTranslation();
    {
      const QSignalBlocker blocker(translateButton_);
      translateButton_->setChecked(true);
    }
    status_->setText(tr("Live Translate: %1 ↔ %2")
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
                  tr("Live Translate was not resumed: %1")
                      .arg(message));
              return;
            }
            QMessageBox::warning(
                this, tr("Live Translate unavailable"), message);
          });
  connect(
      session_, &ChatSession::peerLiveTranslationChanged, this,
      [this](bool active, const QString &sourceLanguage,
             const QString &targetLanguage) {
        // The peer translates from its language (source) into ours
        // (target); we read the pair from our side: ours first.
        status_->setText(
            active ? tr("%1 enabled Live Translate (%2 → %3).")
                         .arg(session_->peerName(), targetLanguage,
                              sourceLanguage)
                   : tr("%1 disabled Live Translate.")
                         .arg(session_->peerName()));
        setPeerTranslating(active);
      });
  connect(session_, &ChatSession::disconnected, this, [this] {
    if (connected_) {
      const auto peer = session_->peerName().isEmpty()
                            ? reconnectAddress_
                            : session_->peerName();
      appendSystemMessage(tr("Disconnected from %1").arg(peer));
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
    setPeerTranslating(false);
    const auto peer = lastPeerName_.isEmpty() ? reconnectAddress_ : lastPeerName_;
    status_->setText(reconnectAddress_.isEmpty() ? tr("Disconnected")
                     : reconnectOnPresence_
                         ? tr("Disconnected; reconnects when %1 comes online").arg(peer)
                         : tr("Disconnected; retrying in 10 seconds"));
    input_->setText(
        tr("Re-connect to this host to start chatting"));
    input_->setReadOnly(true);
    auto inputFont = input_->font();
    inputFont.setItalic(true);
    input_->setFont(inputFont);
    input_->setEnabled(false);
    if (!reconnectOnPresence_ && !reconnectAddress_.isEmpty() &&
        reconnectPort_ != 0 && !reconnectTimer_->isActive()) {
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
    return tr("%1 (Disconnected)").arg(peer);
  }
  if (peer.isEmpty()) {
    return address.isEmpty() ? tr("Unknown host") : address;
  }
  // A phone's address says little; that it is the web client says more.
  if (session_->peerIsWebClient()) return tr("%1 (Web)").arg(peer);
  return address.isEmpty() ? peer
                           : QStringLiteral("%1 (%2)").arg(peer, address);
}

ChatSession *ChatTab::session() const { return session_; }

bool ChatTab::isConnected() const { return connected_; }

QString ChatTab::reconnectAddress() const { return reconnectAddress_; }

quint16 ChatTab::reconnectPort() const { return reconnectPort_; }

void ChatTab::setReconnectEndpoint(const QString &address, quint16 port) {
  reconnectAddress_ = address.trimmed();
  reconnectPort_ = port;
}

QString ChatTab::peerNickname() const { return lastPeerName_; }

void ChatTab::restartConnection() {
  if (reconnectAddress_.isEmpty() || reconnectPort_ == 0) return;
  reconnectTimer_->stop();
  session_->reconnectToHost(reconnectAddress_, reconnectPort_);
  status_->setText(tr("Reconnecting to %1…")
                       .arg(reconnectAddress_));
}

void ChatTab::reconnectNow() {
  if (connected_ || (!reconnectTimer_->isActive() && !reconnectOnPresence_)) {
    return;
  }
  reconnectTimer_->stop();
  retryConnection();
}

void ChatTab::setReconnectOnPresence(bool enabled) {
  if (reconnectOnPresence_ == enabled) return;
  reconnectOnPresence_ = enabled;
  if (enabled) {
    reconnectTimer_->stop();
  } else if (!connected_ && hasPeerIdentity_ && !reconnectAddress_.isEmpty() &&
             reconnectPort_ != 0) {
    reconnectTimer_->start();
  }
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
      tr("%1 is typing a message%2").arg(peer, dots));
  typingIndicator_->show();
  if (!typingAnimation_->isActive()) typingAnimation_->start();
}

void ChatTab::updateLocalTyping() {
  session_->noteLocalTyping(!input_->text().trimmed().isEmpty() ||
                            !pendingQuotes_.isEmpty());
}

namespace {

// History entries shown at a time; "Show earlier messages" adds more.
constexpr int HistoryPageSize = 200;

// Surrounding blank lines and trailing spaces go; indentation stays.
QString trimmedQuote(QString text) {
  text.remove(QRegularExpression(QStringLiteral("^(\\s*\\n)+")));
  text.remove(QRegularExpression(QStringLiteral("\\s+$")));
  return text;
}

QString quoteSummary(const QString &text) {
  const int lines = int(text.count(u'\n')) + 1;
  return lines == 1 ? ChatTab::tr("Quote · 1 line")
                    : ChatTab::tr("Quote · %1 lines").arg(lines);
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
  auto *translate = new QCheckBox(tr("Translate quote"), card);
  translate->setToolTip(
      tr("Translate this quote too; otherwise it is sent as it is."));
  auto *remove = new QToolButton(card);
  remove->setText(QStringLiteral("✕"));
  remove->setAutoRaise(true);
  remove->setToolTip(tr("Remove this quote"));
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
  connect(editor, &QPlainTextEdit::textChanged, this,
          [this, card, editor, summary] {
            // Deleting all of a quote's text removes the quote.
            if (editor->toPlainText().trimmed().isEmpty()) {
              removeQuote(card);
              input_->setFocus(Qt::OtherFocusReason);
              return;
            }
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
  // The choice only exists while Live Translate is on; otherwise quotes are
  // always sent as they are.
  for (const auto &quote : std::as_const(pendingQuotes_)) {
    quote.translate->setVisible(liveTranslate_);
  }
}

void ChatTab::retryConnection() {
  if (connected_ || reconnectAddress_.isEmpty() || reconnectPort_ == 0) {
    return;
  }
  status_->setText(tr("Reconnecting to %1…")
                       .arg(reconnectAddress_));
  session_->reconnectToHost(reconnectAddress_, reconnectPort_);
}

void ChatTab::appendSystemMessage(const QString &message) {
  log_->appendNotice(message);
}

void ChatTab::setOllamaDiscovery(OllamaDiscovery *discovery) {
  ollamaDiscovery_ = discovery;
  connect(discovery, &OllamaDiscovery::serversChanged, this,
          &ChatTab::refreshTranslationAvailability);
  refreshTranslationAvailability();
}

void ChatTab::refreshTranslationAvailability() {
  const auto available =
      MessageTranslator::isAvailable() ||
      (ollamaDiscovery_ && !ollamaDiscovery_->servers().isEmpty());
  if (!available && translateButton_->isChecked()) {
    const QSignalBlocker blocker(translateButton_);
    translateButton_->setChecked(false);
    liveTranslate_ = false;
    session_->stopLiveTranslation();
  }
  translateButton_->setVisible(available);
  if (available) translationRestorer_->retry();
}

void ChatTab::setPeerTranslating(bool active) {
  translateButton_->setEnabled(!active);
  translateButton_->setToolTip(
      active ? tr("%1 is using Live Translate. Only one of you can translate "
                  "at a time.")
                   .arg(session_->peerName())
             : QString());
}

void ChatTab::configureTranslation() {
  TranslationDialog dialog(ollamaDiscovery_, this);
  const auto saved = translationRestorer_->savedPreference();
  if (!saved.myLanguage.isEmpty()) {
    dialog.setLanguages(saved.myLanguage, saved.peerLanguage);
  } else if (!myLanguage_.isEmpty()) {
    dialog.setLanguages(myLanguage_, peerLanguage_);
  }
  if (dialog.exec() == QDialog::Accepted) {
    // The chosen server and model become the Ollama settings, as in the
    // Settings dialog, unless the central host translates.
    if (!MessageTranslator::hasRemote()) {
      QSettings settings;
      settings.setValue(QStringLiteral("ollama/url"), dialog.ollamaUrl());
      settings.setValue(QStringLiteral("ollama/model"), dialog.ollamaModel());
    }
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
  // Surrounding blank lines and trailing spaces go; the indentation of
  // pasted code stays.
  auto text = input_->text();
  text.remove(QRegularExpression(QStringLiteral("^(\\s*\\n)+")));
  text.remove(QRegularExpression(QStringLiteral("\\s+$")));
  if ((text.trimmed().isEmpty() && pendingQuotes_.isEmpty()) ||
      outgoingPending_) {
    return;
  }
  // The typed text first, then the quotes; quotes ticked "Translate quote"
  // keep their mark until the translator has seen it.
  QList<QuotedMessage::Segment> segments;
  if (!text.trimmed().isEmpty()) segments.append({text});
  for (const auto &quote : std::as_const(pendingQuotes_)) {
    // A quote emptied while editing is left out.
    const auto quoted = trimmedQuote(quote.editor->toPlainText());
    if (quoted.isEmpty()) continue;
    segments.append({quoted, true,
                     liveTranslate_ && quote.translate->isChecked()});
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
  input_->setText(tr("Translating message..."));
  input_->setReadOnly(true);
  auto inputFont = input_->font();
  inputFont.setItalic(true);
  input_->setFont(inputFont);
  outgoingTranslator_.translate(text, myLanguage_, peerLanguage_);
}

void ChatTab::received(const QString &message, const QString &originalMessage) {
  const auto timestamp = QDateTime::currentDateTime();
  const int messageId = appendMessage(session_->peerName(), message,
                                    originalMessage,
                                    tr("Original"), false,
                                    timestamp);
  // A message we translate ourselves is stored again once translated.
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      session_->peerName(), message, timestamp,
                      originalMessage);
  const int index = incomingMessages_.size();
  incomingMessages_.append(
      {message, messageId, timestamp, !originalMessage.isEmpty(), false});
  emit incomingMessageReceived();
  if (liveTranslate_ && originalMessage.isEmpty()) {
    incomingMessages_[index].notify = true;
    translateIncomingMessage(index);
    return;
  }
  emit notifyIncoming(session_->peerName(), QuotedMessage::preview(message));
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
                         tr("Translating incoming message..."),
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
                tr("Sent translation"), false, timestamp);
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      QStringLiteral("You"), pendingOriginal_, timestamp,
                      translation);
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
  appendMessage(QStringLiteral("You"), text, {}, tr("Original"),
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

int ChatTab::appendMessage(const QString &sender, const QString &message,
                           const QString &hoverText,
                           const QString &hoverLabel, bool historical,
                           const QDateTime &timestamp) {
  return log_->appendMessage(
      sender == QStringLiteral("You"), message, timestamp, historical,
      hoverText.isEmpty() ? QString()
                          : QStringLiteral("%1: %2").arg(hoverLabel, hoverText));
}

void ChatTab::loadConversationHistory() {
  const auto entries = ChatHistory::load(session_->peerAddress(),
                                         session_->peerName(), HistoryPageSize);
  if (entries.isEmpty()) return;

  for (const auto &entry : entries) {
    const auto message = historyMessage(entry);
    log_->appendMessage(message.outgoing, message.message, message.timestamp,
                        true, message.toolTip);
  }
  historyShown_ = int(entries.size());
  appendHistorySeparator();
  log_->setEarlierMessagesAvailable(
      ChatHistory::count(session_->peerAddress(), session_->peerName()) >
      historyShown_);
}

void ChatTab::showEarlierHistory() {
  const auto entries =
      ChatHistory::load(session_->peerAddress(), session_->peerName(),
                        HistoryPageSize, historyShown_);
  QList<ChatLog::HistoryMessage> messages;
  for (const auto &entry : entries) messages.append(historyMessage(entry));
  log_->prependHistory(messages);
  historyShown_ += int(entries.size());
  log_->setEarlierMessagesAvailable(
      !entries.isEmpty() &&
      ChatHistory::count(session_->peerAddress(), session_->peerName()) >
          historyShown_);
}

ChatLog::HistoryMessage
ChatTab::historyMessage(const ChatHistory::Entry &entry) const {
  // The other version of a translated message shows on hover.
  const bool outgoing = entry.sender == QStringLiteral("You");
  return {outgoing, entry.message, entry.timestamp,
          entry.alternate.isEmpty()
              ? QString()
              : QStringLiteral("%1: %2").arg(
                    outgoing ? tr("Sent translation") : tr("Original"),
                    entry.alternate)};
}

void ChatTab::appendHistorySeparator() {
  log_->appendNotice(tr("──────── Earlier messages ────────"));
}

void ChatTab::setIncomingMessageText(IncomingMessage &incoming,
                                     const QString &text, bool translated,
                                     bool pending) {
  log_->setMessage(incoming.messageId, text, pending,
                   translated ? tr("Original: %1")
                                    .arg(incoming.originalMessage)
                              : QString());
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
    if (succeeded) {
      ChatHistory::replace(session_->peerAddress(), session_->peerName(),
                           session_->peerName(), incoming.timestamp,
                           replacement, incoming.originalMessage);
    }
    // The translation, or the original if translating failed.
    if (std::exchange(incoming.notify, false)) {
      emit notifyIncoming(session_->peerName(),
                          QuotedMessage::preview(replacement));
    }
    return;
  }
}

void ChatTab::clearOutgoingTranslationStatus() {
  input_->clear();
  input_->setReadOnly(false);
  auto inputFont = input_->font();
  inputFont.setItalic(false);
  input_->setFont(inputFont);
}

void ChatTab::saveAttachmentAs(const QString &path,
                               const QString &suggestedName) {
  const auto destination = QFileDialog::getSaveFileName(
      this, tr("Save as..."),
      suggestedName.isEmpty() ? QFileInfo(path).fileName() : suggestedName);
  if (destination.isEmpty() ||
      QFileInfo(destination).absoluteFilePath() ==
          QFileInfo(path).absoluteFilePath()) {
    return;
  }
  if (QFileInfo::exists(destination)) {
    const auto answer = QMessageBox::question(
        this, tr("Overwrite file?"),
        tr("The selected file already exists. Overwrite it?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
  }
  QFile source(path);
  QSaveFile target(destination);
  if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
    target.cancelWriting();
    QMessageBox::warning(this, tr("Save failed"),
                         tr("Cannot save the selected file."));
    return;
  }
  while (!source.atEnd()) {
    const auto bytes = source.read(64 * 1024);
    if ((bytes.isEmpty() && source.error() != QFileDevice::NoError) ||
        target.write(bytes) != bytes.size()) {
      target.cancelWriting();
      QMessageBox::warning(this, tr("Save failed"),
                           tr("Cannot save the selected file."));
      return;
    }
  }
  if (!target.commit()) {
    QMessageBox::warning(this, tr("Save failed"),
                         tr("Cannot save the selected file."));
  }
}

void ChatTab::appendFile(const QString &transferId, const QString &name,
                         const QString &path, const QString &mimeType,
                         bool received) {
  const auto transfer = transfers_.take(transferId);
  log_->appendFile(!received, name, path, mimeType,
                   QDateTime::currentDateTime(), transfer.messageId);
}

void ChatTab::showTransferProgress(const QString &id, const QString &name,
                                   bool outgoing, int percent) {
  const auto peer = session_->peerName().isEmpty() ? lastPeerName_
                                                   : session_->peerName();
  const auto text =
      outgoing ? tr("Uploading %1 to %2. %3%…").arg(name, peer).arg(percent)
               : tr("Receiving %1 from %2. %3%…").arg(name, peer).arg(percent);
  auto transfer = transfers_.find(id);
  if (transfer == transfers_.end()) {
    transfer = transfers_.insert(
        id, {log_->appendMessage(outgoing, text, QDateTime::currentDateTime()),
             name});
  }
  log_->setMessage(transfer->messageId, text, true, {});
}

void ChatTab::showTransferFailure(const QString &id, bool outgoing,
                                  const QString &reason) {
  const auto transfer = transfers_.take(id);
  if (transfer.name.isEmpty()) return;
  const auto peer = session_->peerName().isEmpty() ? lastPeerName_
                                                   : session_->peerName();
  log_->setMessage(transfer.messageId,
                   outgoing ? tr("Could not send %1 to %2: %3")
                                  .arg(transfer.name, peer, reason)
                            : tr("Did not receive %1 from %2: %3")
                                  .arg(transfer.name, peer, reason),
                   true, {});
}
