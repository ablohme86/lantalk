#pragma once

#include <QList>
#include <QDateTime>
#include <QString>

namespace ChatHistory {

struct Entry {
  QString sender;
  QString message;
  QDateTime timestamp;
  // The other version of a translated message: the original of a received
  // one, or the translation sent for one of ours.
  QString alternate = {};
};

// Each conversation's whole history is kept, in a file of its own under the
// app data folder (one JSON object per line).

QString timestampPrefix(const QDateTime &timestamp);
// Up to `limit` entries, oldest first, ending `skip` entries before the
// newest; a negative limit returns them all.
QList<Entry> load(const QString &address, const QString &peerName,
                  int limit = 20, int skip = 0);
int count(const QString &address, const QString &peerName);
void append(const QString &address, const QString &peerName,
            const QString &sender, const QString &message,
            const QDateTime &timestamp = QDateTime::currentDateTime(),
            const QString &alternate = {});
// Replaces the stored text of the entry from `sender` at `timestamp`, e.g.
// with its translation once that arrives.
void replace(const QString &address, const QString &peerName,
             const QString &sender, const QDateTime &timestamp,
             const QString &message, const QString &alternate);

} // namespace ChatHistory
