#include "gui/dialogs.h"

#include "appconfig.h"
#include "gui/uilanguage.h"
#include "messagetranslator.h"
#include "ollamadiscovery.h"
#include "protocol/centralprotocol.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSpinBox>

TranslationDialog::TranslationDialog(OllamaDiscovery *discovery,
                                     QWidget *parent)
    : QDialog(parent), discovery_(discovery),
      remote_(MessageTranslator::hasRemote()) {
  setWindowTitle(tr("Live Translate"));
  auto *layout = new QFormLayout(this);
  server_ = new QComboBox(this);
  server_->setEditable(true);
  server_->setMinimumContentsLength(28);
  server_->lineEdit()->setPlaceholderText(QStringLiteral("http://host:11434"));
  auto *rescan = new QPushButton(tr("Rescan"), this);
  auto *serverRow = new QHBoxLayout;
  serverRow->addWidget(server_, 1);
  serverRow->addWidget(rescan);
  model_ = new QComboBox(this);
  model_->setEditable(true);
  scanStatus_ = new QLabel(this);
  scanStatus_->setStyleSheet(QStringLiteral("color: palette(mid);"));
  myLanguage_ = new QComboBox(this);
  peerLanguage_ = new QComboBox(this);
  myLanguage_->addItems(languages());
  peerLanguage_->addItems(languages());
  myLanguage_->setCurrentText(QStringLiteral("Norwegian Bokmål"));
  peerLanguage_->setCurrentText(QStringLiteral("English"));
  layout->addRow(tr("Ollama server"), serverRow);
  layout->addRow(QString(), scanStatus_);
  layout->addRow(tr("Model"), model_);
  layout->addRow(tr("My Language"), myLanguage_);
  layout->addRow(tr("Peer Language"), peerLanguage_);
  if (remote_) {
    layout->setRowVisible(serverRow, false);
    layout->setRowVisible(scanStatus_, false);
    layout->setRowVisible(model_, false);
    auto *note = new QLabel(tr("The central host translates for you."), this);
    note->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->insertRow(0, note);
  }
  buttons_ = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons_);

  fillServers();
  connect(server_, &QComboBox::currentTextChanged, this, [this] {
    fillModels();
    updateState();
  });
  connect(model_, &QComboBox::currentTextChanged, this,
          &TranslationDialog::updateState);
  if (discovery_) {
    connect(rescan, &QPushButton::clicked, discovery_, &OllamaDiscovery::scan);
    connect(rescan, &QPushButton::clicked, this,
            &TranslationDialog::updateState);
    connect(discovery_, &OllamaDiscovery::scanFinished, this, [this] {
      fillServers();
      updateState();
    });
  } else {
    rescan->hide();
  }
  updateState();
}

QString TranslationDialog::ollamaUrl() const {
  return server_->currentText().trimmed().remove(
      QRegularExpression(QStringLiteral("/+$")));
}

QString TranslationDialog::ollamaModel() const {
  return model_->currentText().trimmed();
}

void TranslationDialog::fillServers() {
  // Found servers first, then the configured one if it was not found; keep
  // what the user has selected or typed.
  const auto current = server_->currentText().trimmed();
  QStringList urls;
  if (discovery_) {
    for (const auto &server : discovery_->servers()) urls << server.url;
  }
  const auto configured =
      QSettings().value(QStringLiteral("ollama/url")).toString().trimmed();
  if (!configured.isEmpty() && !urls.contains(configured)) urls << configured;
  const QSignalBlocker blocker(server_);
  server_->clear();
  server_->addItems(urls);
  server_->setCurrentText(!current.isEmpty()   ? current
                          : !configured.isEmpty() ? configured
                                                  : urls.value(0));
  fillModels();
}

void TranslationDialog::fillModels() {
  const auto url = ollamaUrl();
  QStringList models;
  if (discovery_) {
    for (const auto &server : discovery_->servers()) {
      if (server.url == url) models = server.models;
    }
  }
  const QSettings settings;
  const auto configuredModel =
      settings.value(QStringLiteral("ollama/model")).toString().trimmed();
  const bool configuredServer =
      url == settings.value(QStringLiteral("ollama/url")).toString().trimmed();
  const auto current = model_->currentText().trimmed();
  const QSignalBlocker blocker(model_);
  model_->clear();
  model_->addItems(models);
  if (configuredServer && !configuredModel.isEmpty()) {
    model_->setCurrentText(configuredModel);
  } else if (models.contains(current)) {
    model_->setCurrentText(current);
  } else {
    model_->setCurrentText(models.value(0));
  }
}

void TranslationDialog::updateState() {
  if (discovery_ && discovery_->isScanning()) {
    scanStatus_->setText(tr("Scanning the network for Ollama…"));
  } else if (discovery_) {
    const auto count = discovery_->servers().size();
    scanStatus_->setText(
        count == 0 ? tr("No Ollama server found on the network.")
        : count == 1 ? tr("1 Ollama server found on the network.")
                     : tr("%1 Ollama servers found on the network.").arg(count));
  }
  scanStatus_->setVisible(discovery_ != nullptr && !remote_);
  buttons_->button(QDialogButtonBox::Ok)
      ->setEnabled(remote_ ||
                   (!ollamaUrl().isEmpty() && !ollamaModel().isEmpty()));
}

QString TranslationDialog::myLanguage() const {
  return myLanguage_->currentText();
}

QString TranslationDialog::peerLanguage() const {
  return peerLanguage_->currentText();
}

void TranslationDialog::setLanguages(const QString &myLanguage,
                                     const QString &peerLanguage) {
  myLanguage_->setCurrentText(myLanguage);
  peerLanguage_->setCurrentText(peerLanguage);
}

PinDialog::PinDialog(Mode mode, const QString &nickname,
                     const QString &message, QWidget *parent)
    : QDialog(parent), mode_(mode) {
  setWindowTitle(mode == Mode::Enter ? tr("Enter your PIN") : tr("Personal PIN"));
  auto *layout = new QFormLayout(this);
  QString text = message;
  if (text.isEmpty()) {
    switch (mode) {
    case Mode::Create:
      text = tr("Protect %1 with a personal 4-digit PIN?\n\nWith the PIN, "
                "in addition to the central host's password, you can use %1 "
                "on several devices at once, such as LANTalk on your computer "
                "and the web client on your phone. Everyone sees one %1, and "
                "every device gets your conversations.\n\nNobody can sign in "
                "as %1 without the PIN, and the central host keeps your "
                "conversations so a new device can catch up.")
                 .arg(nickname);
      break;
    case Mode::Enter:
      text = tr("%1 is protected with a PIN. Enter it to use %1 on this "
                "device too.")
                 .arg(nickname);
      break;
    case Mode::Change:
      text = tr("Choose a new PIN for %1. Your other devices need it the "
                "next time they sign in.")
                 .arg(nickname);
      break;
    }
  }
  auto *explanation = new QLabel(text, this);
  explanation->setWordWrap(true);
  explanation->setMinimumWidth(360);
  layout->addRow(explanation);
  const auto pinField = [this] {
    auto *field = new QLineEdit(this);
    field->setEchoMode(QLineEdit::Password);
    field->setMaxLength(CentralProtocol::PinLength);
    field->setInputMethodHints(Qt::ImhDigitsOnly);
    field->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{0,4}")), field));
    field->setPlaceholderText(QStringLiteral("••••"));
    connect(field, &QLineEdit::textChanged, this, &PinDialog::updateState);
    return field;
  };
  pin_ = pinField();
  layout->addRow(mode == Mode::Enter ? tr("PIN:") : tr("New PIN:"), pin_);
  if (mode != Mode::Enter) {
    confirmation_ = pinField();
    layout->addRow(tr("Repeat PIN:"), confirmation_);
    mismatch_ = new QLabel(tr("The PINs are not the same."), this);
    mismatch_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    mismatch_->hide();
    layout->addRow(mismatch_);
  }
  buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  if (mode == Mode::Create) {
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("Create PIN"));
    buttons_->button(QDialogButtonBox::Cancel)->setText(tr("Not now"));
  }
  connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons_, &QDialogButtonBox::rejected, this, [this] {
    declined_ = true;
    reject();
  });
  layout->addRow(buttons_);
  updateState();
}

QString PinDialog::pin() const { return pin_->text(); }

bool PinDialog::declined() const { return declined_; }

void PinDialog::updateState() {
  const bool complete = CentralProtocol::isValidPin(pin_->text());
  const bool confirmed =
      !confirmation_ || confirmation_->text() == pin_->text();
  if (mismatch_) {
    mismatch_->setVisible(complete && confirmation_->text().size() ==
                                          CentralProtocol::PinLength &&
                          !confirmed);
  }
  buttons_->button(QDialogButtonBox::Ok)->setEnabled(complete && confirmed);
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Settings"));
  auto *layout = new QFormLayout(this);
  nickname_ = new QLineEdit(this);
  ollamaUrl_ = new QLineEdit(this);
  ollamaModel_ = new QLineEdit(this);
  listenPort_ = new QSpinBox(this);
  listenPort_->setRange(1, 65535);
  QSettings settings;
  nickname_->setText(
      settings
          .value(QStringLiteral("profile/nickname"), QStringLiteral("Guest"))
          .toString());
  ollamaUrl_->setText(
      settings.value(QStringLiteral("ollama/url"), DefaultOllamaUrl)
          .toString());
  const auto configuredModel =
      settings.value(QStringLiteral("ollama/model"), DefaultOllamaModel)
          .toString();
  ollamaModel_->setText(configuredModel == QStringLiteral("translategemma")
                            ? QString::fromLatin1(DefaultOllamaModel)
                            : configuredModel);
  listenPort_->setValue(
      settings.value(QStringLiteral("network/listenPort"), DefaultPort)
          .toUInt());
  layout->addRow(tr("Nickname"), nickname_);
  language_ = new QComboBox(this);
  language_->setIconSize(QSize(24, 16));
  for (const auto &language : uiLanguages()) {
    language_->addItem(uiLanguageFlag(language.code), language.nativeName,
                       language.code);
  }
  language_->setCurrentIndex(language_->findData(configuredUiLanguage()));
  layout->addRow(tr("Language"), language_);
  layout->addRow(tr("Listen port"), listenPort_);
  centralHost_ = new QLineEdit(this);
  centralHost_->setPlaceholderText(
      tr("None (chat directly on the LAN)"));
  centralHost_->setToolTip(QStringLiteral(
      "IP address or DNS name of a server running lantalkd, optionally "
      "with :port (default %1). With a central host, LANTalk stops listening "
      "and sends every conversation through it.")
                               .arg(CentralProtocol::DefaultCentralPort));
  centralHost_->setText(
      settings.value(QStringLiteral("network/centralHost")).toString());
  layout->addRow(tr("Central host"), centralHost_);
  centralPassword_ = new QLineEdit(this);
  centralPassword_->setEchoMode(QLineEdit::Password);
  centralPassword_->setPlaceholderText(tr("None"));
  centralPassword_->setToolTip(
      tr("The password the central host was started with, if any."));
  centralPassword_->setText(
      settings.value(QStringLiteral("network/centralPassword")).toString());
  centralPassword_->setEnabled(!centralHost_->text().trimmed().isEmpty());
  connect(centralHost_, &QLineEdit::textChanged, centralPassword_,
          [this](const QString &host) {
            centralPassword_->setEnabled(!host.trimmed().isEmpty());
          });
  layout->addRow(tr("Central host password"), centralPassword_);
  notifications_ = new QCheckBox(
      tr("Notify me about new messages"), this);
  notifications_->setChecked(
      settings.value(QStringLiteral("notifications/enabled"), true).toBool());
  layout->addRow(tr("Notifications"), notifications_);
  messageSound_ = new QCheckBox(
      tr("Play a sound for new messages"), this);
  messageSound_->setChecked(
      settings.value(QStringLiteral("notifications/sound"), true).toBool());
  layout->addRow(QString(), messageSound_);
  layout->addRow(tr("Ollama URL"), ollamaUrl_);
  layout->addRow(tr("Ollama model"), ollamaModel_);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void SettingsDialog::save() {
  const auto language = language_->currentData().toString();
  if (language != configuredUiLanguage()) {
    setConfiguredUiLanguage(language);
    // Shown in the chosen language, since that is the one the user reads.
    QMessageBox::information(
        this, QStringLiteral("LANTalk"),
        uiText(language,
               "LANTalk will use the new language the next time it starts."));
  }
  QSettings settings;
  const auto nickname = escapeProtocolField(nickname_->text());
  settings.setValue(QStringLiteral("profile/nickname"),
                    nickname.isEmpty() ? QStringLiteral("Guest") : nickname);
  settings.setValue(QStringLiteral("network/listenPort"), listenPort_->value());
  settings.setValue(QStringLiteral("network/centralHost"),
                    centralHost_->text().trimmed());
  settings.setValue(QStringLiteral("network/centralPassword"),
                    centralPassword_->text());
  settings.setValue(QStringLiteral("notifications/enabled"),
                    notifications_->isChecked());
  settings.setValue(QStringLiteral("notifications/sound"),
                    messageSound_->isChecked());
  settings.setValue(QStringLiteral("ollama/url"),
                    ollamaUrl_->text().trimmed().remove(
                        QRegularExpression(QStringLiteral("/+$"))));
  settings.setValue(QStringLiteral("ollama/model"),
                    ollamaModel_->text().trimmed());
}
