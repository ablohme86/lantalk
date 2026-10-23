#include "gui/chattab.h"

#include "appconfig.h"
#include "gui/dialogs.h"
#include "protocol/chatsession.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTextImageFormat>
#include <QUrl>
#include <QVBoxLayout>

ChatTab::ChatTab(ChatSession *session, QWidget *parent)
    : QWidget(parent), session_(session) {
  setAcceptDrops(true);
  auto *layout = new QVBoxLayout(this);
  status_ = new QLabel(QStringLiteral("Performing nickname handshake…"), this);
  log_ = new QTextEdit(this);
  log_->setReadOnly(true);
  auto *controls = new QHBoxLayout;
  input_ = new QLineEdit(this);
  translateButton_ = new QPushButton(QStringLiteral("Live Translate"), this);
  translateButton_->setCheckable(true);
  auto *send = new QPushButton(QStringLiteral("Send"), this);
  controls->addWidget(input_);
  controls->addWidget(translateButton_);
  controls->addWidget(send);
  layout->addWidget(status_);
  layout->addWidget(log_);
  layout->addLayout(controls);
  refreshTranslationAvailability();

  connect(send, &QPushButton::clicked, this, &ChatTab::send);
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
  connect(session_, &ChatSession::messageReceived, this, &ChatTab::received);
  connect(
      session_, &ChatSession::fileSent, this,
      [this](const QString &name, const QString &path, const QString &mime) {
        appendFile(QStringLiteral("Me"), name, path, mime);
      });
  connect(
      session_, &ChatSession::fileReceived, this,
      [this](const QString &name, const QString &path, const QString &mime) {
        appendFile(session_->peerName(), name, path, mime);
      });
  connect(session_, &ChatSession::liveTranslationGranted, this, [this] {
    liveTranslate_ = true;
    status_->setText(QStringLiteral("Live Translate: %1 ↔ %2")
                         .arg(myLanguage_, peerLanguage_));
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
            active ? QStringLiteral(
                         "The other user enabled Live Translate (%1 → %2).")
                         .arg(sourceLanguage, targetLanguage)
                   : QStringLiteral("The other user disabled Live Translate."));
      });
  connect(session_, &ChatSession::disconnected, this, [this] {
    status_->setText(QStringLiteral("Disconnected"));
    input_->setEnabled(false);
  });
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
            replaceTranslationStatus(originalMessage, originalMessage);
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
  if (!liveTranslate_) {
    appendMessage(session_->peerName(), message, originalMessage);
    return;
  }
  auto statusCursor = appendTranslationStatus(
      session_->peerName(),
      QStringLiteral("Translating incoming message... Stand by."));
  pendingIncomingTranslations_.append({message, statusCursor});
  incomingTranslator_.translate(message, peerLanguage_, myLanguage_);
}

void ChatTab::outgoingTranslationFinished(const QString &translation,
                                          const QString &) {
  outgoingPending_ = false;
  clearOutgoingTranslationStatus();
  session_->sendMessage(translation, pendingOriginal_);
  appendMessage(QStringLiteral("Me"), pendingOriginal_, translation,
                QStringLiteral("Sent translation"));
  pendingOriginal_.clear();
  input_->setFocus(Qt::OtherFocusReason);
}

void ChatTab::incomingTranslationFinished(const QString &translation,
                                          const QString &originalMessage) {
  replaceTranslationStatus(originalMessage, translation);
}

void ChatTab::sendText(const QString &text) {
  session_->sendMessage(text);
  appendMessage(QStringLiteral("Me"), text);
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

void ChatTab::appendMessage(const QString &sender, const QString &message,
                            const QString &hoverText,
                            const QString &hoverLabel) {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat senderFormat;
  senderFormat.setFontWeight(QFont::Bold);
  cursor.insertText(sender + QStringLiteral(": "), senderFormat);
  QTextCharFormat messageFormat;
  if (!hoverText.isEmpty()) {
    messageFormat.setToolTip(
        QStringLiteral("%1: %2").arg(hoverLabel, hoverText));
  }
  cursor.insertText(message, messageFormat);
  cursor.insertBlock();
  log_->setTextCursor(cursor);
}

QTextCursor ChatTab::appendTranslationStatus(const QString &sender,
                                             const QString &message) {
  auto cursor = log_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat senderFormat;
  senderFormat.setFontWeight(QFont::Bold);
  cursor.insertText(sender + QStringLiteral(": "), senderFormat);
  const int messageStart = cursor.position();
  QTextCharFormat messageFormat;
  messageFormat.setFontItalic(true);
  cursor.insertText(message, messageFormat);
  const int messageEnd = cursor.position();
  cursor.insertBlock();
  log_->setTextCursor(cursor);

  QTextCursor statusCursor(log_->document());
  statusCursor.setPosition(messageStart);
  statusCursor.setPosition(messageEnd, QTextCursor::KeepAnchor);
  return statusCursor;
}

void ChatTab::replaceTranslationStatus(const QString &originalMessage,
                                      const QString &replacement) {
  for (auto pending = pendingIncomingTranslations_.begin();
       pending != pendingIncomingTranslations_.end(); ++pending) {
    if (pending->originalMessage != originalMessage) {
      continue;
    }
    auto cursor = pending->statusCursor;
    pendingIncomingTranslations_.erase(pending);
    QTextCharFormat messageFormat;
    messageFormat.setFontItalic(false);
    if (replacement != originalMessage) {
      messageFormat.setToolTip(
          QStringLiteral("Original: %1").arg(originalMessage));
    }
    cursor.insertText(replacement, messageFormat);
    auto endCursor = log_->textCursor();
    endCursor.movePosition(QTextCursor::End);
    log_->setTextCursor(endCursor);
    return;
  }
  appendMessage(session_->peerName(), replacement,
                replacement == originalMessage ? QString() : originalMessage);
}

void ChatTab::clearOutgoingTranslationStatus() {
  input_->clear();
  input_->setReadOnly(false);
  auto inputFont = input_->font();
  inputFont.setItalic(false);
  input_->setFont(inputFont);
}

void ChatTab::appendFile(const QString &sender, const QString &name,
                         const QString &path, const QString &mimeType) {
  log_->append(QStringLiteral("<b>%1:</b> sent <i>%2</i>")
                   .arg(sender.toHtmlEscaped(), name.toHtmlEscaped()));
  if (mimeType.startsWith(QStringLiteral("image/"))) {
    auto cursor = log_->textCursor();
    QTextImageFormat image;
    image.setName(QUrl::fromLocalFile(path).toString());
    image.setWidth(360);
    cursor.insertImage(image);
    cursor.insertBlock();
  }
}
