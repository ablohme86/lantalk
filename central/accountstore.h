#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// lantalkd's accounts, nicknames protected with a personal PIN, and the
// history of every conversation each account has had through lantalkd. Both
// live in a data folder: accounts.json and history/<account id>.jsonl.
class AccountStore {
public:
  struct Account {
    QString name;
    // CentralProtocol::pinVerifier() of the PIN.
    QByteArray verifier;
    // New for every account, so a nickname that is freed and protected again
    // starts with a history of its own.
    QString id;
    QDateTime created;
  };

  // Loads the accounts in `directory`, creating it if needed.
  bool open(const QString &directory, QString *error = nullptr);
  bool isOpen() const;
  QString directory() const;

  std::optional<Account> find(const QString &nickname) const;
  QStringList names() const;
  // Protects a nickname nobody has protected yet.
  std::optional<Account> create(const QString &nickname,
                                const QByteArray &verifier);
  bool setVerifier(const QString &nickname, const QByteArray &verifier);
  // Frees the nickname and deletes its history.
  bool remove(const QString &nickname);

  // Wrong PINs lock the nickname for a while, longer after every few.
  bool isLocked(const QString &nickname, qint64 *remainingMs = nullptr) const;
  void noteWrongPin(const QString &nickname);
  void noteRightPin(const QString &nickname);

  // Stores `entry` with the next sequence number ("seq"), which it returns;
  // 0 if it could not be stored.
  qint64 append(const Account &account, QJsonObject entry);
  // Entries after `seq`, oldest first; only the newest when there are many.
  QList<QJsonObject> entriesAfter(const Account &account, qint64 seq) const;
  qint64 lastSequence(const Account &account) const;

private:
  QString historyPath(const Account &account) const;
  bool save(QString *error = nullptr) const;

  QString directory_;
  bool open_ = false;
  // By ConversationIdentity::normalizedNickname().
  QHash<QString, Account> accounts_;
  struct Failures {
    int count = 0;
    QElapsedTimer last;
  };
  QHash<QString, Failures> failures_;
  mutable QHash<QString, qint64> lastSequences_;
};
