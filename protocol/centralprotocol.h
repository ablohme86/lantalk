#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QString>

// The LANTALK CENTRAL protocol between LANTalk clients and
// lantalk-centralized-service. It is line based like LANCHAT PROTOCOL; every
// command starts with "CENTRAL_". A client registers with CENTRAL_HELLO and
// then opens virtual channels to other clients. Each channel carries an
// unmodified LANCHAT PROTOCOL conversation, one line per CENTRAL_DATA, so the
// service only relays lines and never interprets the chat itself.
namespace CentralProtocol {

inline constexpr quint16 DefaultCentralPort = 45455;
inline constexpr int Version = 1;
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

inline bool isValidChannelId(const QString &id) {
  if (id.isEmpty() || id.size() > MaximumChannelIdLength) return false;
  for (const auto character : id) {
    if (!character.isLetterOrNumber() && character != u'-') return false;
  }
  return true;
}

} // namespace CentralProtocol
