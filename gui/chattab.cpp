#include "gui/chattab.h"

#include "appconfig.h"
#include "gui/dialogs.h"
#include "protocol/chathistory.h"
#include "protocol/chatsession.h"

#include <QColor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
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
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

QUrl attachmentActionUrl(const QString &action, const QString &path,
                         const QString &name = {}) {
  QUrl url;
  url.setScheme(QStringLiteral("lanchat"));
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
  auto *controls = new QHBoxLayout;
  input_ = new QLineEdit(this);
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
  layout->addLayout(controls);
  refreshTranslationAvailability();

  connect(attach, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open file"));
    if (!path.isEmpty()) {
      session_->sendFile(path);
    }
  });
  connect(input_, &QLineEdit::returnPressed, this, &ChatTab::send);
  connect(translateButton_, &QPushButton::toggled, this, [this](bool enabled) {
    if (enabled) {
      configureTranslation();
    } else {
      liveTranslate_ = false;
      session_->stopLiveTranslation();
      status_->setText(QStringLiteral("Live Translate is off."));
    }
  });
  connect(session_, &ChatSession::statusChanged, status_, &QLabel::setText);
  connect(session_, &ChatSession::peerIdentified, this,
          [this](const QString &name) {
            if (!historyLoaded_) {
              historyLoaded_ = true;
              loadConversationHistory();
            }
            if (!connected_) {
              appendSystemMessage(QStringLiteral("Connected to %1").arg(name));
            }
            connected_ = true;
            reconnectTimer_->stop();
            input_->setEnabled(true);
            status_->setText(QStringLiteral("Connected to %1").arg(name));
            emit titleChanged();
          });
  connect(log_, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
    if (url.scheme() != QStringLiteral("lanchat")) {
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
    status_->setText(QStringLiteral("Live Translate: %1 ↔ %2")
                         .arg(myLanguage_, peerLanguage_));
    translatePendingIncomingMessages();
  });
  connect(session_, &ChatSession::liveTranslationDenied, this,
          [this](const QString &message) {
            liveTranslate_ = false;
            const QSignalBlocker blocker(translateButton_);
            translateButton_->setChecked(false);
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
    liveTranslate_ = false;
    {
      const QSignalBlocker blocker(translateButton_);
      translateButton_->setChecked(false);
    }
    status_->setText(reconnectAddress_.isEmpty()
                         ? QStringLiteral("Disconnected")
                         : QStringLiteral("Disconnected; retrying in 10 seconds"));
    input_->setEnabled(false);
    if (!reconnectAddress_.isEmpty() && reconnectPort_ != 0 &&
        !reconnectTimer_->isActive()) {
      reconnectTimer_->start();
    }
  });
  connect(reconnectTimer_, &QTimer::timeout, this, &ChatTab::retryConnection);
  connect(&outgoingTranslator_, &OllamaClient::translated, this,
          &ChatTab::outgoingTranslationFinished);
  connect(&outgoingTranslator_, &OllamaClient::failed, this,
          [this](const QString &error, const QString &) {
            status_->setText(error);
            clearOutgoingTranslationStatus();
            outgoingPending_ = false;
            pendingOriginal_.clear();
            input_->setFocus(Qt::OtherFocusReason);
          });
  connect(&incomingTranslator_, &OllamaClient::translated, this,
          &ChatTab::incomingTranslationFinished);
  connect(&incomingTranslator_, &OllamaClient::failed, this,
          [this](const QString &error, const QString &originalMessage) {
            status_->setText(error);
            replaceTranslationStatus(originalMessage, originalMessage, false);
          });
}

QString ChatTab::title() const {
  const auto peer = session_->peerName();
  const auto address = session_->peerAddress();
  if (peer.isEmpty()) {
    return address.isEmpty() ? QStringLiteral("Unknown host") : address;
  }
  return address.isEmpty() ? peer
                           : QStringLiteral("%1 (%2)").arg(peer, address);
}

QString ChatTab::reconnectAddress() const { return reconnectAddress_; }

quint16 ChatTab::reconnectPort() const { return reconnectPort_; }

void ChatTab::setReconnectEndpoint(const QString &address, quint16 port) {
  reconnectAddress_ = address.trimmed();
  reconnectPort_ = port;
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
}

void ChatTab::configureTranslation() {
  TranslationDialog dialog(this);
  if (dialog.exec() == QDialog::Accepted) {
    myLanguage_ = dialog.myLanguage();
    peerLanguage_ = dialog.peerLanguage();
    session_->requestLiveTranslation(myLanguage_, peerLanguage_);
  } else {
    const QSignalBlocker blocker(translateButton_);
    translateButton_->setChecked(false);
  }
}

void ChatTab::send() {
  if (fileSendUiActive_) {
    return;
  }
  const auto text = input_->text().trimmed();
  if (text.isEmpty() || outgoingPending_) {
    return;
  }
  input_->clear();
  if (!liveTranslate_) {
    sendText(text);
    return;
  }
  outgoingPending_ = true;
  pendingOriginal_ = text;
  input_->setText(QStringLiteral("Translating message..."));
  input_->setReadOnly(true);
  auto inputFont = input_->font();
  inputFont.setItalic(true);
  input_->setFont(inputFont);
  outgoingTranslator_.translate(text, myLanguage_, peerLanguage_);
}

void ChatTab::received(const QString &message, const QString &originalMessage) {
  const auto cursor = appendMessage(session_->peerName(), message,
                                    originalMessage);
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      session_->peerName(), message);
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
  session_->sendMessage(translation, pendingOriginal_);
  appendMessage(QStringLiteral("You"), pendingOriginal_, translation,
                QStringLiteral("Sent translation"));
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      QStringLiteral("You"), pendingOriginal_);
  pendingOriginal_.clear();
  input_->setFocus(Qt::OtherFocusReason);
}

void ChatTab::incomingTranslationFinished(const QString &translation,
                                          const QString &originalMessage) {
  replaceTranslationStatus(originalMessage, translation, true);
}

void ChatTab::sendText(const QString &text) {
  session_->sendMessage(text);
  appendMessage(QStringLiteral("You"), text);
  ChatHistory::append(session_->peerAddress(), session_->peerName(),
                      QStringLiteral("You"), text);
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
                                   bool historical) {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
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
  cursor.insertText(message, messageFormat);
  const int messageEnd = cursor.position();
  cursor.insertBlock();
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
                  QStringLiteral("Original"), true);
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
  incoming.messageCursor.insertText(text, format);
  const int end = incoming.messageCursor.position();
  incoming.messageCursor.setPosition(end - text.size());
  incoming.messageCursor.setPosition(end, QTextCursor::KeepAnchor);
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
