#include "testsupport.h"

#include "appconfig.h"

using namespace TestSupport;

class SettingsMigrationTest final : public QObject {
  Q_OBJECT

private:
  static QSettings legacySettings() {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                     QStringLiteral("LANChat"), QStringLiteral("LANChat"));
  }

private slots:
  void init() {
    isolateSettings();
    legacySettings().clear();
  }

  void copiesLegacySettingsOnFirstRun() {
    {
      auto legacy = legacySettings();
      legacy.setValue(QStringLiteral("profile/nickname"), QStringLiteral("alice"));
      legacy.setValue(QStringLiteral("network/listenPort"), 46000);
      legacy.setValue(QStringLiteral("chatHistory/abc"),
                      QVariantList{QStringLiteral("entry")});
    }

    migrateLegacySettings();

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("profile/nickname")).toString(),
             QStringLiteral("alice"));
    QCOMPARE(settings.value(QStringLiteral("network/listenPort")).toInt(), 46000);
    QCOMPARE(settings.value(QStringLiteral("chatHistory/abc")).toList().size(), 1);
    // The old settings stay in place.
    QCOMPARE(legacySettings().value(QStringLiteral("profile/nickname")).toString(),
             QStringLiteral("alice"));
  }

  void doesNotOverwriteExistingSettings() {
    legacySettings().setValue(QStringLiteral("profile/nickname"),
                              QStringLiteral("old"));
    QSettings().setValue(QStringLiteral("profile/nickname"), QStringLiteral("new"));

    migrateLegacySettings();

    QCOMPARE(QSettings().value(QStringLiteral("profile/nickname")).toString(),
             QStringLiteral("new"));
  }

  void doesNothingWithoutLegacySettings() {
    migrateLegacySettings();
    QVERIFY(QSettings().allKeys().isEmpty());
  }
};

QTEST_GUILESS_MAIN(SettingsMigrationTest)
#include "tst_settingsmigration.moc"
