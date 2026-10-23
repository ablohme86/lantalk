#include "ollamadiscovery.h"

#include "appconfig.h"
#include "protocol/peerdiscovery.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>

namespace {

constexpr int ParallelProbes = 64;
constexpr int ConnectTimeoutMs = 700;
constexpr int CheckTimeoutMs = 3000;
// Larger subnets are only scanned around our own address.
constexpr int LargestScannedPrefix = 22;

} // namespace

OllamaDiscovery::OllamaDiscovery(QObject *parent) : QObject(parent) {}

QList<OllamaDiscovery::Server> OllamaDiscovery::servers() const
{
    return servers_;
}

bool OllamaDiscovery::isScanning() const
{
    return scanning_;
}

void OllamaDiscovery::setPort(quint16 port)
{
    port_ = port;
}

void OllamaDiscovery::setCandidates(const QList<QHostAddress> &hosts)
{
    candidates_ = hosts;
    fixedCandidates_ = true;
}

QList<QHostAddress> OllamaDiscovery::localCandidates()
{
    QSet<quint32> addresses;
    QSet<quint32> ownAddresses;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)
            || interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto &entry : interface.addressEntries()) {
            if (!PeerDiscovery::isPrivateIPv4Address(entry.ip())) continue;
            const quint32 local = entry.ip().toIPv4Address();
            ownAddresses.insert(local);
            const int prefix = std::max(entry.prefixLength(), LargestScannedPrefix);
            if (prefix > 30) continue;
            const quint32 mask = 0xffffffffu << (32 - prefix);
            const quint32 network = local & mask;
            const quint32 broadcast = network | ~mask;
            for (quint32 host = network + 1; host < broadcast; ++host) addresses.insert(host);
        }
    }
    // This computer is reached as localhost, not again by its LAN addresses.
    addresses.subtract(ownAddresses);
    addresses.insert(QHostAddress(QHostAddress::LocalHost).toIPv4Address());
    auto sorted = addresses.values();
    std::sort(sorted.begin(), sorted.end());
    QList<QHostAddress> hosts;
    for (const auto address : sorted) hosts.append(QHostAddress(address));
    return hosts;
}

void OllamaDiscovery::scan()
{
    if (scanning_) return;
    scanning_ = true;
    found_.clear();
    const auto hosts = fixedCandidates_ ? candidates_ : localCandidates();
    for (const auto &host : hosts) pendingHosts_.enqueue(host);

    // The configured server may be anywhere, on any port.
    const auto configured = QSettings().value(QStringLiteral("ollama/url")).toString().trimmed();
    if (!configured.isEmpty()) checkServer(configured);

    for (int probe = 0; probe < ParallelProbes; ++probe) probeNext();
    finishIfDone();
}

void OllamaDiscovery::probeNext()
{
    if (pendingHosts_.isEmpty()) return;
    const auto host = pendingHosts_.dequeue();
    ++activeProbes_;
    auto *socket = new QTcpSocket(this);
    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    // Each probe ends once: connected, refused, or timed out.
    const auto done = [this, socket](bool open) {
        if (socket->property("done").toBool()) return;
        socket->setProperty("done", true);
        const auto address = socket->peerAddress().isNull() ? socket->property("host").toString()
                                                            : socket->peerAddress().toString();
        socket->abort();
        socket->deleteLater();
        if (open) checkServer(QStringLiteral("http://%1:%2").arg(address).arg(port_));
        --activeProbes_;
        probeNext();
        finishIfDone();
    };
    socket->setProperty("host", host.toString());
    connect(socket, &QTcpSocket::connected, this, [done] { done(true); });
    connect(socket, &QTcpSocket::errorOccurred, this, [done] { done(false); });
    connect(timeout, &QTimer::timeout, this, [done] { done(false); });
    timeout->start(ConnectTimeoutMs);
    socket->connectToHost(host, port_);
}

void OllamaDiscovery::checkServer(const QString &url)
{
    const auto base = QString(url).remove(QRegularExpression(QStringLiteral("/+$")));
    ++activeChecks_;
    QNetworkRequest request(QUrl(base + QStringLiteral("/api/tags")));
    request.setTransferTimeout(CheckTimeoutMs);
    auto *reply = manager_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, base] {
        reply->deleteLater();
        QStringList models;
        if (reply->error() == QNetworkReply::NoError) {
            const auto entries = QJsonDocument::fromJson(reply->readAll())
                                     .object()
                                     .value(QStringLiteral("models"))
                                     .toArray();
            for (const auto &entry : entries) {
                const auto name = entry.toObject().value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) models << name;
            }
            models.removeDuplicates();
        }
        const bool known = std::any_of(found_.cbegin(), found_.cend(),
                                       [&base](const Server &server) { return server.url == base; });
        if (!models.isEmpty() && !known) found_.append({base, models});
        --activeChecks_;
        finishIfDone();
    });
}

void OllamaDiscovery::finishIfDone()
{
    if (!scanning_ || activeProbes_ > 0 || activeChecks_ > 0 || !pendingHosts_.isEmpty()) return;
    scanning_ = false;
    std::sort(found_.begin(), found_.end(), [](const Server &left, const Server &right) {
        return left.url < right.url;
    });
    const bool changed = found_.size() != servers_.size()
                         || !std::equal(found_.cbegin(), found_.cend(), servers_.cbegin(),
                                        [](const Server &left, const Server &right) {
                                            return left.url == right.url && left.models == right.models;
                                        });
    servers_ = found_;
    if (changed) emit serversChanged();
    emit scanFinished();
}
