#include "protocol/chathistory.h"

#include "protocol/conversationidentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantMap>

namespace {

QString historyPath(const QString &address, const QString &peerName) {
  const auto digest = ConversationIdentity::digest(address, peerName);
  if (digest.isEmpty()) return {};
  const auto directory = QDir(QStandardPaths::writableLocation(
                                  QStandardPaths::AppDataLocation))
                             .filePath(QStringLiteral("history"));
  return QDir(directory).filePath(digest + QStringLiteral(".jsonl"));
}

QByteArray entryLine(const QVariantMap &entry) {
  return QJsonDocument(QJsonObject::fromVariantMap(entry))
             .toJson(QJsonDocument::Compact) +
         '\n';
}

QVariantMap entryMap(const QString &sender, const QString &message,
                     const QDateTime &timestamp, const QString &alternate) {
  QVariantMap entry{{QStringLiteral("sender"), sender},
                    {QStringLiteral("message"), message},
                    {QStringLiteral("timestamp"),
                     timestamp.toString(Qt::ISODateWithMs)}};
  if (!alternate.isEmpty()) entry.insert(QStringLiteral("alternate"), alternate);
  return entry;
}

QList<QVariantMap> readEntries(const QString &path) {
  QList<QVariantMap> entries;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return entries;
  while (!file.atEnd()) {
    const auto line = file.readLine().trimmed();
    if (line.isEmpty()) continue;
    const auto object = QJsonDocument::fromJson(line).object();
    if (!object.isEmpty()) entries.append(object.toVariantMap());
  }
  return entries;
}

bool writeEntries(const QString &path, const QList<QVariantMap> &entries) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  for (const auto &entry : entries) file.write(entryLine(entry));
  return file.commit();
}

// Histories used to live in the settings, capped at 500 entries; the first
// use of a conversation moves its history to the file.
QString preparedPath(const QString &address, const QString &peerName) {
  if (address.trimmed().isEmpty() || peerName.trimmed().isEmpty()) return {};
  const auto path = historyPath(address, peerName);
  if (path.isEmpty()) return {};
  QSettings settings;
  const auto key = QStringLiteral("chatHistory/%1")
                       .arg(ConversationIdentity::digest(address, peerName));
  if (!settings.contains(key)) return path;
  QList<QVariantMap> entries;
  for (const auto &item : settings.value(key).toList()) entries.append(item.toMap());
  entries.append(readEntries(path));
  if (writeEntries(path, entries)) settings.remove(key);
  return path;
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
                                            int limit, int skip) {
  const auto path = preparedPath(address, peerName);
  if (path.isEmpty() || limit == 0) return {};

  const auto stored = readEntries(path);
  const int end = qMax(0, int(stored.size()) - qMax(0, skip));
  const int first = limit < 0 ? 0 : qMax(0, end - limit);
  QList<Entry> entries;
  for (int index = first; index < end; ++index) {
    const auto &item = stored.at(index);
    const auto sender = item.value(QStringLiteral("sender")).toString();
    const auto message = item.value(QStringLiteral("message")).toString();
    const auto timestamp = QDateTime::fromString(
        item.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!sender.isEmpty() && !message.isEmpty()) {
      entries.append({sender, message, timestamp,
                      item.value(QStringLiteral("alternate")).toString()});
    }
  }
  return entries;
}

int ChatHistory::count(const QString &address, const QString &peerName) {
  const auto path = preparedPath(address, peerName);
  return path.isEmpty() ? 0 : int(readEntries(path).size());
}

void ChatHistory::append(const QString &address, const QString &peerName,
                         const QString &sender, const QString &message,
                         const QDateTime &timestamp,
                         const QString &alternate) {
  if (sender.isEmpty() || message.isEmpty()) return;
  const auto path = preparedPath(address, peerName);
  if (path.isEmpty()) return;
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (file.open(QIODevice::Append)) {
    file.write(entryLine(entryMap(sender, message, timestamp, alternate)));
  }
}

void ChatHistory::replace(const QString &address, const QString &peerName,
                          const QString &sender, const QDateTime &timestamp,
                          const QString &message, const QString &alternate) {
  if (message.isEmpty()) return;
  const auto path = preparedPath(address, peerName);
  if (path.isEmpty()) return;
  auto stored = readEntries(path);
  const auto when = timestamp.toString(Qt::ISODateWithMs);
  // The newest match: the entry was written moments ago.
  for (auto index = stored.size() - 1; index >= 0; --index) {
    const auto &entry = stored.at(index);
    if (entry.value(QStringLiteral("sender")).toString() != sender ||
        entry.value(QStringLiteral("timestamp")).toString() != when) {
      continue;
    }
    stored[index] = entryMap(sender, message, timestamp, alternate);
    writeEntries(path, stored);
    return;
  }
}

void ChatHistory::adopt(const QString &fromAddress, const QString &toAddress,
                        const QString &peerName) {
  if (ConversationIdentity::sameAddress(fromAddress, toAddress)) return;
  const auto from = preparedPath(fromAddress, peerName);
  const auto to = preparedPath(toAddress, peerName);
  if (from.isEmpty() || to.isEmpty() || !QFile::exists(from)) return;
  auto entries = readEntries(from);
  entries.append(readEntries(to));
  if (writeEntries(to, entries)) QFile::remove(from);
}
