#include "gui/dialogs.h"

#include "appconfig.h"
#include "ollamadiscovery.h"
#include "protocol/centralprotocol.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>

TranslationDialog::TranslationDialog(OllamaDiscovery *discovery,
                                     QWidget *parent)
    : QDialog(parent), discovery_(discovery) {
  setWindowTitle(QStringLiteral("Live Translate"));
  auto *layout = new QFormLayout(this);
  server_ = new QComboBox(this);
  server_->setEditable(true);
  server_->setMinimumContentsLength(28);
  server_->lineEdit()->setPlaceholderText(QStringLiteral("http://host:11434"));
  auto *rescan = new QPushButton(QStringLiteral("Rescan"), this);
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
  layout->addRow(QStringLiteral("Ollama server"), serverRow);
  layout->addRow(QString(), scanStatus_);
  layout->addRow(QStringLiteral("Model"), model_);
  layout->addRow(QStringLiteral("My Language"), myLanguage_);
  layout->addRow(QStringLiteral("Peer Language"), peerLanguage_);
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
    scanStatus_->setText(QStringLiteral("Scanning the network for Ollama…"));
  } else if (discovery_) {
    const auto count = discovery_->servers().size();
    scanStatus_->setText(
        count == 0 ? QStringLiteral("No Ollama server found on the network.")
                   : QStringLiteral("%1 Ollama server%2 found on the network.")
                         .arg(count)
                         .arg(count == 1 ? QString() : QStringLiteral("s")));
  }
  scanStatus_->setVisible(discovery_ != nullptr);
  buttons_->button(QDialogButtonBox::Ok)
      ->setEnabled(!ollamaUrl().isEmpty() && !ollamaModel().isEmpty());
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

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Settings"));
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
  layout->addRow(QStringLiteral("Nickname"), nickname_);
  layout->addRow(QStringLiteral("Listen port"), listenPort_);
  centralHost_ = new QLineEdit(this);
  centralHost_->setPlaceholderText(
      QStringLiteral("None (chat directly on the LAN)"));
  centralHost_->setToolTip(QStringLiteral(
      "IP address or DNS name of a lantalk-centralized-service, optionally "
      "with :port (default %1). With a central host, LANTalk stops listening "
      "and sends every conversation through it.")
                               .arg(CentralProtocol::DefaultCentralPort));
  centralHost_->setText(
      settings.value(QStringLiteral("network/centralHost")).toString());
  layout->addRow(QStringLiteral("Central host"), centralHost_);
  notifications_ = new QCheckBox(
      QStringLiteral("Notify me about new messages"), this);
  notifications_->setChecked(
      settings.value(QStringLiteral("notifications/enabled"), true).toBool());
  layout->addRow(QStringLiteral("Notifications"), notifications_);
  layout->addRow(QStringLiteral("Ollama URL"), ollamaUrl_);
  layout->addRow(QStringLiteral("Ollama model"), ollamaModel_);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void SettingsDialog::save() const {
  QSettings settings;
  const auto nickname = escapeProtocolField(nickname_->text());
  settings.setValue(QStringLiteral("profile/nickname"),
                    nickname.isEmpty() ? QStringLiteral("Guest") : nickname);
  settings.setValue(QStringLiteral("network/listenPort"), listenPort_->value());
  settings.setValue(QStringLiteral("network/centralHost"),
                    centralHost_->text().trimmed());
  settings.setValue(QStringLiteral("notifications/enabled"),
                    notifications_->isChecked());
  settings.setValue(QStringLiteral("ollama/url"),
                    ollamaUrl_->text().trimmed().remove(
                        QRegularExpression(QStringLiteral("/+$"))));
  settings.setValue(QStringLiteral("ollama/model"),
                    ollamaModel_->text().trimmed());
}
