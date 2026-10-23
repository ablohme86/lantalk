#include "protocol/connectionregistry.h"

#include "appconfig.h"
#include "protocol/chatsession.h"
#include "protocol/conversationidentity.h"

ConnectionRegistry::ConnectionRegistry()
    : raceWindowMs_(DuplicateConnectionRaceWindowMs) {}

ConnectionRegistry *ConnectionRegistry::shared() {
  static ConnectionRegistry registry;
  return &registry;
}

ChatSession *ConnectionRegistry::findSession(const QString &address,
                                             quint16 port,
                                             const QString &nickname) const {
  for (auto *session : sessions_) {
    if (session->isRetired()) continue;
    if (!ConversationIdentity::sameAddress(session->peerAddress(), address) &&
        !ConversationIdentity::sameAddress(session->remoteHost(), address)) {
      continue;
    }
    const auto identity = session->identityName();
    if (!nickname.trimmed().isEmpty() && !identity.isEmpty()) {
      if (ConversationIdentity::sameNickname(identity, nickname)) {
        return session;
      }
      continue;
    }
    if (session->remotePort() == 0 || session->remotePort() == port) {
      return session;
    }
  }
  return nullptr;
}

QList<ChatSession *> ConnectionRegistry::sessions() const { return sessions_; }

void ConnectionRegistry::setRaceWindow(int milliseconds) {
  raceWindowMs_ = milliseconds;
}

void ConnectionRegistry::add(ChatSession *session) {
  if (!sessions_.contains(session)) sessions_.append(session);
}

void ConnectionRegistry::remove(ChatSession *session) {
  sessions_.removeAll(session);
}

ChatSession *ConnectionRegistry::resolveDuplicate(ChatSession *identified) {
  for (auto *existing : sessions_) {
    if (existing == identified || existing->isRetired() ||
        !ConversationIdentity::sameAddress(existing->peerAddress(),
                                           identified->peerAddress()) ||
        !ConversationIdentity::sameNickname(existing->identityName(),
                                            identified->identityName())) {
      continue;
    }

    // `existing` keeps representing the conversation either way; only the
    // choice of TCP connection differs. A connection that is down or has been
    // established for a while loses to the new one (the peer evidently lost
    // it, e.g. after a restart). Two fresh connections are a simultaneous
    // connect, which both peers resolve the same way by connection key.
    bool keepNewConnection = true;
    if (existing->isConnected() &&
        (!existing->isHandshakeComplete() ||
         existing->handshakeAgeMs() <= raceWindowMs_)) {
      keepNewConnection =
          identified->connectionKey() < existing->connectionKey();
    }
    if (keepNewConnection) {
      existing->adoptConnection(identified);
    } else {
      identified->retireAsDuplicate(existing);
    }
    return existing;
  }
  return identified;
}
