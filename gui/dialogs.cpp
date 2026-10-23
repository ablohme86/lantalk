#include "gui/dialogs.h"

#include "appconfig.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>

TranslationDialog::TranslationDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Live Translate"));
  auto *layout = new QFormLayout(this);
  myLanguage_ = new QComboBox(this);
  peerLanguage_ = new QComboBox(this);
  myLanguage_->addItems(languages());
  peerLanguage_->addItems(languages());
  myLanguage_->setCurrentText(QStringLiteral("Norwegian Bokmål"));
  peerLanguage_->setCurrentText(QStringLiteral("English"));
  layout->addRow(QStringLiteral("My Language"), myLanguage_);
  layout->addRow(QStringLiteral("Peer Language"), peerLanguage_);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
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
  settings.setValue(QStringLiteral("notifications/enabled"),
                    notifications_->isChecked());
  settings.setValue(QStringLiteral("ollama/url"),
                    ollamaUrl_->text().trimmed().remove(
                        QRegularExpression(QStringLiteral("/+$"))));
  settings.setValue(QStringLiteral("ollama/model"),
                    ollamaModel_->text().trimmed());
}
