#include "protocol/conversationidentity.h"

#include <QCryptographicHash>
#include <QHostAddress>

QString ConversationIdentity::normalizedAddress(const QString &address) {
  const auto trimmed = address.trimmed();
  const QHostAddress parsedAddress(trimmed);
  if (parsedAddress.isNull()) return trimmed.toCaseFolded();
  bool isIpv4 = false;
  const auto ipv4 = parsedAddress.toIPv4Address(&isIpv4);
  return isIpv4 ? QHostAddress(ipv4).toString() : parsedAddress.toString();
}

QString ConversationIdentity::normalizedNickname(const QString &nickname) {
  return nickname.normalized(QString::NormalizationForm_KC)
      .simplified()
      .toCaseFolded();
}

bool ConversationIdentity::sameAddress(const QString &left,
                                       const QString &right) {
  const auto normalizedLeft = normalizedAddress(left);
  return !normalizedLeft.isEmpty() &&
         normalizedLeft == normalizedAddress(right);
}

bool ConversationIdentity::sameNickname(const QString &left,
                                        const QString &right) {
  const auto normalizedLeft = normalizedNickname(left);
  return !normalizedLeft.isEmpty() &&
         normalizedLeft == normalizedNickname(right);
}

QString ConversationIdentity::digest(const QString &address,
                                     const QString &nickname) {
  const auto normalizedHost = normalizedAddress(address);
  const auto normalizedName = normalizedNickname(nickname);
  if (normalizedHost.isEmpty() || normalizedName.isEmpty()) return {};
  const QByteArray identity =
      normalizedHost.toUtf8() + '\0' + normalizedName.toUtf8();
  return QString::fromLatin1(
      QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}
