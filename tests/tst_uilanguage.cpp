#include "testsupport.h"

#include "gui/uilanguage.h"

#include <QApplication>

using namespace TestSupport;

class UiLanguageTest final : public QObject {
  Q_OBJECT

private slots:
  void init() { isolateSettings(); }

  void usesTheSystemLanguageUntilOneIsChosen() {
    QCOMPARE(configuredUiLanguage(), systemUiLanguage());
    setConfiguredUiLanguage(QStringLiteral("zh_TW"));
    QCOMPARE(configuredUiLanguage(), QStringLiteral("zh_TW"));
  }

  void ignoresAnUnsupportedLanguage() {
    setConfiguredUiLanguage(QStringLiteral("xx"));
    QCOMPARE(configuredUiLanguage(), systemUiLanguage());
  }

  void systemLanguageIsASupportedOne() {
    bool supported = false;
    for (const auto &language : uiLanguages()) {
      supported = supported || language.code == systemUiLanguage();
    }
    QVERIFY(supported);
  }

  void translatesThroughTr() {
    setConfiguredUiLanguage(QStringLiteral("nb"));
    installUiLanguage(*qApp);
    QCOMPARE(QObject::tr("Settings"), QStringLiteral("Innstillinger"));
    QCOMPARE(QObject::tr("Connected to %1").arg(QStringLiteral("bob")),
             QStringLiteral("Tilkoblet bob"));
  }

  void keepsUntranslatedTextsInEnglish() {
    QCOMPARE(uiText(QStringLiteral("zh_CN"), "Not a LANTalk text"),
             QStringLiteral("Not a LANTalk text"));
    QCOMPARE(uiText(QStringLiteral("zh_CN"), "Settings"), QStringLiteral("设置"));
  }
};

QTEST_MAIN(UiLanguageTest)
#include "tst_uilanguage.moc"
