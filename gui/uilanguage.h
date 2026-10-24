#pragma once

#include <QIcon>
#include <QList>
#include <QString>

class QApplication;

// A language the GUI can be shown in. The code is what Settings stores.
struct UiLanguage {
  QString code;
  QString nativeName;
};

QList<UiLanguage> uiLanguages();

// The best match for the system's UI languages, or English.
QString systemUiLanguage();

// The language chosen in Settings, or the system's when none is chosen.
QString configuredUiLanguage();
void setConfiguredUiLanguage(const QString &code);

QIcon uiLanguageFlag(const QString &code);

// Installs the translator for configuredUiLanguage(); call once, before the
// first window is created.
void installUiLanguage(QApplication &application);

// The translation of an English source text into the given language.
QString uiText(const QString &code, const char *source);
