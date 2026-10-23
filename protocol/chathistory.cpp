#include "protocol/chathistory.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QSettings>
#include <QVariantMap>

namespace {

constexpr int MaximumStoredEntries = 500;

QString settingsKey(const QString &address, const QString &peerName) {
  QHostAddress parsedAddress(address.trimmed());
  const QString normalizedAddress =
      parsedAddress.isNull() ? address.trimmed().toCaseFolded()
                             : parsedAddress.toString();
  const QString normalizedName =
      peerName.normalized(QString::NormalizationForm_KC)
          .simplified()
          .toCaseFolded();
  const QByteArray identity = normalizedAddress.toUtf8() + '\0' +
                              normalizedName.toUtf8();
  const auto digest =
      QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
  return QStringLiteral("chatHistory/%1").arg(QString::fromLatin1(digest));
}

} // namespace

QString ChatHistory::timestampPrefix(const QDateTime &timestamp) {
  return timestamp.isValid()
             ? QStringLiteral("[%1] ").arg(
                   timestamp.toLocalTime().toString(QStringLiteral("HH:mm")))
             : QString();
}

QList<ChatHistory::Entry> ChatHistory::load(const QString &address,
                                            const QString &peerName,
                                            int limit) {
  if (address.trimmed().isEmpty() || peerName.trimmed().isEmpty() ||
      limit <= 0) {
    return {};
  }

  const auto stored = QSettings().value(settingsKey(address, peerName)).toList();
  QList<Entry> entries;
  const int first = qMax(0, stored.size() - limit);
  for (int index = first; index < stored.size(); ++index) {
    const auto item = stored.at(index).toMap();
    const auto sender = item.value(QStringLiteral("sender")).toString();
    const auto message = item.value(QStringLiteral("message")).toString();
    const auto timestamp = QDateTime::fromString(
        item.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!sender.isEmpty() && !message.isEmpty()) {
      entries.append({sender, message, timestamp});
    }
  }
  return entries;
}

void ChatHistory::append(const QString &address, const QString &peerName,
                         const QString &sender, const QString &message,
                         const QDateTime &timestamp) {
  if (address.trimmed().isEmpty() || peerName.trimmed().isEmpty() ||
      sender.isEmpty() || message.isEmpty()) {
    return;
  }

  QSettings settings;
  const auto key = settingsKey(address, peerName);
  auto stored = settings.value(key).toList();
  QVariantMap entry;
  entry.insert(QStringLiteral("sender"), sender);
  entry.insert(QStringLiteral("message"), message);
  entry.insert(QStringLiteral("timestamp"),
               timestamp.toString(Qt::ISODateWithMs));
  stored.append(entry);
  while (stored.size() > MaximumStoredEntries) {
    stored.removeFirst();
  }
  settings.setValue(key, stored);
}
