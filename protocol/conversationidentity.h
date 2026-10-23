#pragma once

#include <QString>

// A conversation is identified by the peer's host address and nickname. All
// comparisons and persisted keys (chat history, Live Translate preferences,
// duplicate-connection detection) go through these helpers so that they agree.
namespace ConversationIdentity {

QString normalizedAddress(const QString &address);
QString normalizedNickname(const QString &nickname);
bool sameAddress(const QString &left, const QString &right);
bool sameNickname(const QString &left, const QString &right);
// Stable, settings-safe digest of the identity; empty if either part is empty.
QString digest(const QString &address, const QString &nickname);

} // namespace ConversationIdentity
