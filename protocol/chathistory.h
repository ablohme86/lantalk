#pragma once

#include <QList>
#include <QDateTime>
#include <QString>

namespace ChatHistory {

struct Entry {
  QString sender;
  QString message;
  QDateTime timestamp;
};

QString timestampPrefix(const QDateTime &timestamp);
QList<Entry> load(const QString &address, const QString &peerName,
                  int limit = 20);
void append(const QString &address, const QString &peerName,
            const QString &sender, const QString &message,
            const QDateTime &timestamp = QDateTime::currentDateTime());

} // namespace ChatHistory
