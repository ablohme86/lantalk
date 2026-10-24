#pragma once

#include <QDialog>

class OllamaDiscovery;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

// Live Translate's settings: the Ollama server and model (found on the
// network, or typed in) and the two languages.
class TranslationDialog final : public QDialog {
public:
  explicit TranslationDialog(OllamaDiscovery *discovery,
                             QWidget *parent = nullptr);
  QString myLanguage() const;
  QString peerLanguage() const;
  void setLanguages(const QString &myLanguage, const QString &peerLanguage);
  QString ollamaUrl() const;
  QString ollamaModel() const;

private:
  void fillServers();
  void fillModels();
  void updateState();

  OllamaDiscovery *discovery_;
  // The central host translates, so there is no server to choose.
  bool remote_;
  QComboBox *server_;
  QComboBox *model_;
  QLabel *scanStatus_;
  QComboBox *myLanguage_;
  QComboBox *peerLanguage_;
  QDialogButtonBox *buttons_;
};

// A personal PIN for a nickname on the central host: choosing one (to use
// the nickname on several devices), entering it, or changing it.
class PinDialog final : public QDialog {
public:
  enum class Mode { Create, Enter, Change };
  // `message` replaces the usual explanation, e.g. after a wrong PIN.
  PinDialog(Mode mode, const QString &nickname, const QString &message = {},
            QWidget *parent = nullptr);
  QString pin() const;
  // "Not now" when offered a PIN.
  bool declined() const;

private:
  void updateState();

  Mode mode_;
  QLineEdit *pin_;
  QLineEdit *confirmation_ = nullptr;
  QLabel *mismatch_ = nullptr;
  QDialogButtonBox *buttons_;
  bool declined_ = false;
};

class SettingsDialog final : public QDialog {
public:
  explicit SettingsDialog(QWidget *parent = nullptr);
  void save();

private:
  QLineEdit *nickname_;
  QComboBox *language_;
  QLineEdit *ollamaUrl_;
  QLineEdit *ollamaModel_;
  QSpinBox *listenPort_;
  QLineEdit *centralHost_;
  QLineEdit *centralPassword_;
  QCheckBox *notifications_;
  QCheckBox *messageSound_;
};
