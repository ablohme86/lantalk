#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QMessageAuthenticationCode>
#include <QString>

// The LANTALK CENTRAL protocol between LANTalk clients and
// lantalkd. It is line based like LANCHAT PROTOCOL; every
// command starts with "CENTRAL_". A client registers with CENTRAL_HELLO and
// then opens virtual channels to other clients. Each channel carries an
// unmodified LANCHAT PROTOCOL conversation, one line per CENTRAL_DATA, so the
// service only relays lines and never interprets the chat itself.
//
// The service opens with CENTRAL_CHALLENGE <nonce>. A service started with a
// password only accepts a CENTRAL_HELLO whose proof is authProof(password,
// nonce), so the password itself never crosses the network.
//
// Version 2 adds personal PINs. A nickname protected with a PIN is an account:
// its devices sign in with CENTRAL_HELLO 2 ... <pinProof> <deviceId>, all of
// them appear as one peer, lantalkd hands every conversation to each of them,
// mirrors what one device sends to the others (CENTRAL_MIRROR), and keeps the
// account's history so a device can catch up (CENTRAL_SYNC, CENTRAL_HISTORY).
namespace CentralProtocol {

inline constexpr quint16 DefaultCentralPort = 45455;
// lantalkd serves the web client for browsers on this port.
inline constexpr quint16 DefaultWebPort = 45456;
inline constexpr int Version = 2;
inline constexpr int PinLength = 4;
// Clients ping this often; either side drops a connection that has been
// silent for IdleTimeoutMs.
inline constexpr int PingIntervalMs = 30 * 1000;
inline constexpr int IdleTimeoutMs = 3 * PingIntervalMs;
// A LANCHAT line is at most a 16 KB file chunk in base64; anything far
// beyond that is not a LANTalk client.
inline constexpr qsizetype MaximumLineLength = 4 * 1024 * 1024;
inline constexpr qsizetype MaximumChannelIdLength = 64;

inline QByteArray encodeField(const QString &value) {
  return value.toUtf8().toBase64(QByteArray::Base64UrlEncoding);
}

inline QString decodeField(const QString &value) {
  return QString::fromUtf8(QByteArray::fromBase64(
      value.toLatin1(), QByteArray::Base64UrlEncoding));
}

// IPv4 addresses (including IPv4-mapped IPv6 ones) as dotted quads, so that
// the same host is always written the same way.
inline QString formatAddress(const QHostAddress &address) {
  if (address.isNull()) return {};
  if (address.protocol() != QAbstractSocket::IPv6Protocol) {
    return address.toString();
  }
  const auto ipv6 = address.toIPv6Address();
  bool isIpv4Mapped = ipv6[10] == 0xff && ipv6[11] == 0xff;
  for (int index = 0; index < 10 && isIpv4Mapped; ++index) {
    isIpv4Mapped = ipv6[index] == 0;
  }
  if (!isIpv4Mapped) return address.toString();
  return QStringLiteral("%1.%2.%3.%4")
      .arg(ipv6[12])
      .arg(ipv6[13])
      .arg(ipv6[14])
      .arg(ipv6[15]);
}

// Splits "host", "host:port", or "[ipv6]:port"; the port defaults to
// DefaultCentralPort. Returns false for an empty host or an invalid port.
inline bool parseHostSpec(const QString &spec, QString *host, quint16 *port) {
  auto value = spec.trimmed();
  quint16 parsedPort = DefaultCentralPort;
  if (value.startsWith(u'[')) {
    const int end = value.indexOf(u']');
    if (end < 0) return false;
    const auto rest = value.mid(end + 1);
    value = value.mid(1, end - 1);
    if (!rest.isEmpty()) {
      bool valid = false;
      if (!rest.startsWith(u':')) return false;
      parsedPort = rest.mid(1).toUShort(&valid);
      if (!valid || parsedPort == 0) return false;
    }
  } else if (value.count(u':') == 1) {
    bool valid = false;
    parsedPort = value.mid(value.indexOf(u':') + 1).toUShort(&valid);
    if (!valid || parsedPort == 0) return false;
    value = value.left(value.indexOf(u':'));
  }
  value = value.trimmed();
  if (value.isEmpty() || value.contains(u' ')) return false;
  if (host) *host = value;
  if (port) *port = parsedPort;
  return true;
}

// HMAC-SHA256 of the challenge, keyed with the password, in hex; "-" when
// there is no password.
inline QByteArray authProof(const QString &password, const QByteArray &nonce) {
  if (password.isEmpty()) return QByteArrayLiteral("-");
  return QMessageAuthenticationCode::hash("LANTALK_CENTRAL " + nonce,
                                          password.toUtf8(),
                                          QCryptographicHash::Sha256)
      .toHex();
}

// Compares in constant time, so timing does not reveal how much matched.
inline bool sameProof(const QByteArray &left, const QByteArray &right) {
  if (left.size() != right.size()) return false;
  char difference = 0;
  for (qsizetype index = 0; index < left.size(); ++index) {
    difference |= char(left.at(index) ^ right.at(index));
  }
  return difference == 0;
}

// What lantalkd stores for a PIN and a device proves it knows. A four-digit
// PIN keeps others from signing in with a nickname by accident or on a whim;
// it is no protection against someone who records the network traffic.
inline QByteArray pinVerifier(const QString &pin) {
  return QCryptographicHash::hash("LANTALK_PIN " + pin.toUtf8(),
                                  QCryptographicHash::Sha256)
      .toHex();
}

inline bool isValidPin(const QString &pin) {
  if (pin.size() != PinLength) return false;
  for (const auto character : pin) {
    if (character < u'0' || character > u'9') return false;
  }
  return true;
}

// HMAC-SHA256 of the challenge, keyed with the PIN verifier, in hex; "-"
// without a PIN.
inline QByteArray pinProof(const QByteArray &verifier, const QByteArray &nonce) {
  if (verifier.isEmpty()) return QByteArrayLiteral("-");
  return QMessageAuthenticationCode::hash("LANTALK_PIN " + nonce, verifier,
                                          QCryptographicHash::Sha256)
      .toHex();
}

inline bool isValidChannelId(const QString &id) {
  if (id.isEmpty() || id.size() > MaximumChannelIdLength) return false;
  for (const auto character : id) {
    if (!character.isLetterOrNumber() && character != u'-') return false;
  }
  return true;
}

} // namespace CentralProtocol
