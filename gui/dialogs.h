#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

class TranslationDialog final : public QDialog {
public:
  explicit TranslationDialog(QWidget *parent = nullptr);
  QString myLanguage() const;
  QString peerLanguage() const;
  void setLanguages(const QString &myLanguage, const QString &peerLanguage);

private:
  QComboBox *myLanguage_;
  QComboBox *peerLanguage_;
};

class SettingsDialog final : public QDialog {
public:
  explicit SettingsDialog(QWidget *parent = nullptr);
  void save() const;

private:
  QLineEdit *nickname_;
  QLineEdit *ollamaUrl_;
  QLineEdit *ollamaModel_;
  QSpinBox *listenPort_;
};
