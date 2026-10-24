#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

inline constexpr quint16 DefaultPort = 45454;
inline constexpr quint16 DiscoveryPort = 45456;
inline constexpr qint64 MaximumFileSize = 1024LL * 1024 * 1024;
// What LANTalk accepted before 1.13, and still sends to peers that do not
// advertise CAPABILITIES LARGEFILES.
inline constexpr qint64 LegacyMaximumFileSize = 50LL * 1024 * 1024;
inline constexpr auto DefaultOllamaUrl = "";
inline constexpr auto DefaultOllamaModel = "";
// A local user who stops editing a draft for this long is reported as no
// longer typing.
inline constexpr int TypingInactivityTimeoutMs = 60 * 1000;
// While typing, TYPING START is repeated at this interval so the peer can
// expire a stale indicator if TYPING STOP never arrives.
inline constexpr int TypingRefreshIntervalMs = 15 * 1000;
inline constexpr int TypingIndicatorTimeoutMs = 2 * TypingRefreshIntervalMs + 5 * 1000;
// A user who has not touched LANTalk for this long shows as away.
inline constexpr int AwayAfterMs = 10 * 60 * 1000;
// Connections to the same peer that complete their handshakes within this
// window are treated as a simultaneous-connect race and resolved
// deterministically; otherwise the newer connection replaces the older one.
inline constexpr int DuplicateConnectionRaceWindowMs = 5 * 1000;
inline constexpr int TranslationResumeRetryIntervalMs = 30 * 1000;

inline QStringList languages()
{
    return {QStringLiteral("Norwegian Bokmål"), QStringLiteral("Traditional Chinese"),
            QStringLiteral("Simplified Chinese"), QStringLiteral("English")};
}

inline QString escapeProtocolField(const QString &value)
{
    return value.simplified().replace(u'\n', u' ').replace(u'\r', u' ');
}

// Copies settings saved under the app's former name (LANChat) the first time
// LANTalk runs, so nickname, history, open tabs, and preferences carry over.
// The old settings are left untouched.
inline void migrateLegacySettings()
{
    QSettings settings;
    if (!settings.allKeys().isEmpty()) return;
    const QSettings legacy(QSettings::defaultFormat(), QSettings::UserScope,
                           QStringLiteral("LANChat"), QStringLiteral("LANChat"));
    for (const auto &key : legacy.allKeys()) settings.setValue(key, legacy.value(key));
}

inline bool isOllamaConfigured()
{
    QSettings settings;
    const auto endpoint = settings.value(QStringLiteral("ollama/url"), DefaultOllamaUrl).toString().trimmed();
    const auto model = settings.value(QStringLiteral("ollama/model"), DefaultOllamaModel).toString().trimmed();
    return !endpoint.isEmpty() && !model.isEmpty() && model != QStringLiteral("translategemma");
}
