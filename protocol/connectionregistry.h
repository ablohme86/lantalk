#pragma once

#include <QList>
#include <QString>

class ChatSession;

// Tracks every ChatSession in the process so that at most one connection to a
// given peer (host address + nickname) exists at a time. Sessions register
// themselves on construction, which makes the rule hold for incoming,
// outgoing, restored, and reconnecting sessions alike, independent of the UI.
class ConnectionRegistry final {
public:
  ConnectionRegistry();
  ConnectionRegistry(const ConnectionRegistry &) = delete;
  ConnectionRegistry &operator=(const ConnectionRegistry &) = delete;

  static ConnectionRegistry *shared();

  // Returns the session a new connection to address:port would duplicate, or
  // nullptr. With a nickname (e.g. from discovery) identified sessions are
  // matched by address and nickname; without one, by address and port.
  ChatSession *findSession(const QString &address, quint16 port,
                           const QString &nickname = {}) const;
  QList<ChatSession *> sessions() const;
  void setRaceWindow(int milliseconds);

private:
  friend class ChatSession;

  void add(ChatSession *session);
  void remove(ChatSession *session);
  // Called when a session completes its handshake. If another session already
  // represents the same peer, the two are merged so that a single connection
  // remains, owned by that existing session; returns the owning session.
  ChatSession *resolveDuplicate(ChatSession *identified);

  QList<ChatSession *> sessions_;
  int raceWindowMs_;
};
