#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QNetworkInterface>
#include <QSet>

#include <algorithm>
#include <limits>

namespace PeerDiscovery {

inline bool isPrivateIPv4Address(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }

    bool valid = false;
    const quint32 value = address.toIPv4Address(&valid);
    if (!valid) {
        return false;
    }

    const auto first = quint8(value >> 24);
    const auto second = quint8(value >> 16);
    return first == 10 || (first == 172 && second >= 16 && second <= 31) ||
           (first == 192 && second == 168);
}

inline bool isLocalIPv4Address(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    // Reading the network cards is slow on Windows with VPN adapters, and
    // this runs for every discovery datagram, so the list is reused for a
    // few seconds.
    static QSet<quint32> localAddresses;
    static QElapsedTimer age;
    if (!age.isValid() || age.hasExpired(5000)) {
        localAddresses.clear();
        for (const auto &interface : QNetworkInterface::allInterfaces()) {
            for (const auto &entry : interface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    localAddresses.insert(entry.ip().toIPv4Address());
                }
            }
        }
        age.start();
    }
    return localAddresses.contains(address.toIPv4Address());
}

inline QList<QHostAddress> privateSubnetHosts()
{
    QSet<quint32> addresses;

    // These routed LAN/VPN ranges may not appear as local interface subnets.
    for (quint32 host = 1; host <= 255; ++host) {
        addresses.insert((10u << 24) | (8u << 16) | host);
        addresses.insert((10u << 24) | (10u << 16) | (10u << 8) | host);
    }

    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp) ||
            interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const auto &entry : interface.addressEntries()) {
            const auto address = entry.ip();
            if (!isPrivateIPv4Address(address)) {
                continue;
            }

            const int prefixLength = entry.prefixLength();
            if (prefixLength < 16 || prefixLength > 31) {
                // Avoid probing more than a /16 for one interface.
                continue;
            }

            bool valid = false;
            const quint32 localAddress = address.toIPv4Address(&valid);
            if (!valid) {
                continue;
            }

            const quint32 mask = 0xffffffffu << (32 - prefixLength);
            const quint32 network = localAddress & mask;
            const quint32 broadcast = network | ~mask;
            const quint64 firstHost = prefixLength == 31 ? network : network + 1ULL;
            const quint64 lastHost = prefixLength == 31 ? broadcast : broadcast - 1ULL;

            for (quint64 candidate = firstHost; candidate <= lastHost; ++candidate) {
                const auto candidateAddress = quint32(candidate);
                if (candidateAddress != localAddress) {
                    addresses.insert(candidateAddress);
                }
            }
        }
    }

    auto sortedAddresses = addresses.values();
    std::sort(sortedAddresses.begin(), sortedAddresses.end());

    QList<QHostAddress> result;
    result.reserve(sortedAddresses.size());
    for (const auto address : sortedAddresses) {
        result.append(QHostAddress(address));
    }
    return result;
}

inline int peerExpiryMilliseconds(qsizetype targetCount)
{
    constexpr qint64 probesPerSecond = 2000;
    constexpr qint64 minimumExpiryMilliseconds = 7000;
    const qint64 estimatedSweepMilliseconds =
        (qint64(targetCount) * 1000 + probesPerSecond - 1) / probesPerSecond;
    const qint64 expiry = qMax(minimumExpiryMilliseconds,
                               2 * (estimatedSweepMilliseconds + 5000));
    return int(qMin<qint64>(expiry, std::numeric_limits<int>::max()));
}

} // namespace PeerDiscovery
