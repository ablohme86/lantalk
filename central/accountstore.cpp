#include "central/accountstore.h"

#include "protocol/conversationidentity.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

// Wrong PINs in a row before the nickname is locked.
constexpr int AllowedWrongPins = 5;
constexpr qint64 FirstLockMs = 60 * 1000;
constexpr qint64 LongestLockMs = 15 * 60 * 1000;
// A new device catches up with at most this many entries.
constexpr int MaximumSyncEntries = 20000;

QString keyOf(const QString &nickname) {
  return ConversationIdentity::normalizedNickname(nickname);
}

qint64 lockDuration(int failures) {
  if (failures < AllowedWrongPins) return 0;
  const int doublings = qMin(failures - AllowedWrongPins, 4);
  return qMin(FirstLockMs << doublings, LongestLockMs);
}

} // namespace

bool AccountStore::open(const QString &directory, QString *error) {
  open_ = false;
  accounts_.clear();
  lastSequences_.clear();
  directory_ = directory;
  if (!QDir().mkpath(QDir(directory).filePath(QStringLiteral("history")))) {
    if (error) *error = QStringLiteral("Cannot create %1").arg(directory);
    return false;
  }
  QFile file(QDir(directory).filePath(QStringLiteral("accounts.json")));
  if (file.exists()) {
    if (!file.open(QIODevice::ReadOnly)) {
      if (error) *error = file.errorString();
      return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
      if (error) {
        *error = QStringLiteral("%1 is damaged: %2")
                     .arg(file.fileName(), parseError.errorString());
      }
      return false;
    }
    for (const auto &value : document.object().value(QStringLiteral("accounts")).toArray()) {
      const auto object = value.toObject();
      Account account{object.value(QStringLiteral("name")).toString(),
                      object.value(QStringLiteral("verifier")).toString().toLatin1(),
                      object.value(QStringLiteral("id")).toString(),
                      QDateTime::fromString(
                          object.value(QStringLiteral("created")).toString(),
                          Qt::ISODate)};
      if (keyOf(account.name).isEmpty() || account.verifier.isEmpty() ||
          account.id.isEmpty()) {
        continue;
      }
      accounts_.insert(keyOf(account.name), account);
    }
  }
  open_ = true;
  return true;
}

bool AccountStore::isOpen() const { return open_; }

QString AccountStore::directory() const { return directory_; }

std::optional<AccountStore::Account>
AccountStore::find(const QString &nickname) const {
  const auto found = accounts_.constFind(keyOf(nickname));
  if (found == accounts_.cend()) return std::nullopt;
  return *found;
}

QStringList AccountStore::names() const {
  QStringList names;
  for (const auto &account : accounts_) names.append(account.name);
  names.sort(Qt::CaseInsensitive);
  return names;
}

std::optional<AccountStore::Account>
AccountStore::create(const QString &nickname, const QByteArray &verifier) {
  const auto key = keyOf(nickname);
  if (!open_ || key.isEmpty() || accounts_.contains(key)) return std::nullopt;
  const Account account{nickname, verifier,
                        QUuid::createUuid().toString(QUuid::WithoutBraces),
                        QDateTime::currentDateTimeUtc()};
  accounts_.insert(key, account);
  if (!save()) {
    accounts_.remove(key);
    return std::nullopt;
  }
  return account;
}

bool AccountStore::setVerifier(const QString &nickname,
                               const QByteArray &verifier) {
  const auto found = accounts_.find(keyOf(nickname));
  if (!open_ || found == accounts_.end()) return false;
  const auto previous = std::exchange(found->verifier, verifier);
  if (save()) return true;
  found->verifier = previous;
  return false;
}

bool AccountStore::remove(const QString &nickname) {
  const auto key = keyOf(nickname);
  if (!open_ || !accounts_.contains(key)) return false;
  const auto account = accounts_.take(key);
  if (!save()) {
    accounts_.insert(key, account);
    return false;
  }
  QFile::remove(historyPath(account));
  lastSequences_.remove(account.id);
  failures_.remove(key);
  return true;
}

bool AccountStore::isLocked(const QString &nickname, qint64 *remainingMs) const {
  const auto found = failures_.constFind(keyOf(nickname));
  if (found == failures_.cend()) return false;
  const auto remaining = lockDuration(found->count) - found->last.elapsed();
  if (remaining <= 0) return false;
  if (remainingMs) *remainingMs = remaining;
  return true;
}

void AccountStore::noteWrongPin(const QString &nickname) {
  auto &failures = failures_[keyOf(nickname)];
  ++failures.count;
  failures.last.start();
}

void AccountStore::noteRightPin(const QString &nickname) {
  failures_.remove(keyOf(nickname));
}

qint64 AccountStore::append(const Account &account, QJsonObject entry) {
  if (!open_) return 0;
  const auto sequence = lastSequence(account) + 1;
  entry.insert(QStringLiteral("seq"), sequence);
  QFile file(historyPath(account));
  if (!file.open(QIODevice::Append) ||
      file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n') < 0) {
    return 0;
  }
  lastSequences_.insert(account.id, sequence);
  return sequence;
}

QList<QJsonObject> AccountStore::entriesAfter(const Account &account,
                                              qint64 sequence) const {
  QList<QJsonObject> entries;
  QFile file(historyPath(account));
  if (!file.open(QIODevice::ReadOnly)) return entries;
  while (!file.atEnd()) {
    const auto line = file.readLine().trimmed();
    if (line.isEmpty()) continue;
    const auto entry = QJsonDocument::fromJson(line).object();
    if (entry.value(QStringLiteral("seq")).toInteger() > sequence) {
      entries.append(entry);
    }
  }
  if (entries.size() > MaximumSyncEntries) {
    entries.remove(0, entries.size() - MaximumSyncEntries);
  }
  return entries;
}

qint64 AccountStore::lastSequence(const Account &account) const {
  const auto cached = lastSequences_.constFind(account.id);
  if (cached != lastSequences_.cend()) return *cached;
  qint64 last = 0;
  QFile file(historyPath(account));
  if (file.open(QIODevice::ReadOnly)) {
    while (!file.atEnd()) {
      const auto line = file.readLine().trimmed();
      if (line.isEmpty()) continue;
      last = qMax(last, QJsonDocument::fromJson(line)
                            .object()
                            .value(QStringLiteral("seq"))
                            .toInteger());
    }
  }
  lastSequences_.insert(account.id, last);
  return last;
}

QString AccountStore::historyPath(const Account &account) const {
  return QDir(directory_).filePath(
      QStringLiteral("history/%1.jsonl").arg(account.id));
}

bool AccountStore::save(QString *error) const {
  QJsonArray accounts;
  for (const auto &account : accounts_) {
    accounts.append(QJsonObject{
        {QStringLiteral("name"), account.name},
        {QStringLiteral("verifier"), QString::fromLatin1(account.verifier)},
        {QStringLiteral("id"), account.id},
        {QStringLiteral("created"), account.created.toString(Qt::ISODate)}});
  }
  QSaveFile file(QDir(directory_).filePath(QStringLiteral("accounts.json")));
  if (!file.open(QIODevice::WriteOnly)) {
    if (error) *error = file.errorString();
    return false;
  }
  file.write(QJsonDocument(QJsonObject{{QStringLiteral("accounts"), accounts}})
                 .toJson());
  if (!file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  // Only lantalkd needs to read the PIN verifiers.
  QFile::setPermissions(file.fileName(),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#ifdef Q_OS_UNIX
  // Run as root, e.g. for --remove-pin, the file stays the service's.
  const QFileInfo folder(directory_);
  if (::geteuid() == 0 &&
      ::chown(QFile::encodeName(file.fileName()).constData(), folder.ownerId(),
              folder.groupId()) != 0) {
    if (error) *error = QStringLiteral("Cannot hand accounts.json to the service's user");
  }
#endif
  return true;
}
