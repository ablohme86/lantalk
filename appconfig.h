#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

inline constexpr quint16 DefaultPort = 45454;
inline constexpr quint16 DiscoveryPort = 45456;
inline constexpr qint64 MaximumFileSize = 50LL * 1024 * 1024;
inline constexpr auto DefaultOllamaUrl = "";
inline constexpr auto DefaultOllamaModel = "";

inline QStringList languages()
{
    return {QStringLiteral("Norwegian Bokmål"), QStringLiteral("Traditional Chinese"),
            QStringLiteral("Simplified Chinese"), QStringLiteral("English")};
}

inline QString escapeProtocolField(const QString &value)
{
    return value.simplified().replace(u'\n', u' ').replace(u'\r', u' ');
}

inline bool isOllamaConfigured()
{
    QSettings settings;
    const auto endpoint = settings.value(QStringLiteral("ollama/url"), DefaultOllamaUrl).toString().trimmed();
    const auto model = settings.value(QStringLiteral("ollama/model"), DefaultOllamaModel).toString().trimmed();
    return !endpoint.isEmpty() && !model.isEmpty() && model != QStringLiteral("translategemma");
}
