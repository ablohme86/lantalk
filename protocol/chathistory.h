#pragma once

#include <QList>
#include <QString>

namespace ChatHistory {

struct Entry {
  QString sender;
  QString message;
};

QList<Entry> load(const QString &address, const QString &peerName,
                  int limit = 20);
void append(const QString &address, const QString &peerName,
            const QString &sender, const QString &message);

} // namespace ChatHistory
