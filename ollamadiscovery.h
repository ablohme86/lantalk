#pragma once

#include <QHostAddress>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QStringList>

// Finds Ollama servers: this computer and the hosts on the local network
// cards' private subnets, on Ollama's port (11434), plus the configured URL.
// A server counts when /api/tags answers with at least one model.
class OllamaDiscovery final : public QObject {
    Q_OBJECT

public:
    struct Server {
        QString url;
        QStringList models;
    };

    static constexpr quint16 DefaultPort = 11434;

    explicit OllamaDiscovery(QObject *parent = nullptr);
    QList<Server> servers() const;
    bool isScanning() const;
    // Starts a scan unless one is running; serversChanged() follows if the
    // result differs, scanFinished() in any case.
    void scan();
    // For tests: another port, or a fixed host list instead of the network
    // cards' subnets.
    void setPort(quint16 port);
    void setCandidates(const QList<QHostAddress> &hosts);
    // Off: only this computer and the configured URL are checked. Used in
    // central host mode, where the network cards may be VPN adapters that
    // accept every connection.
    void setScanLocalNetwork(bool enabled);
    static QList<QHostAddress> localCandidates();

signals:
    void serversChanged();
    void scanFinished();

private:
    void probeNext();
    void checkServer(const QString &url);
    void finishIfDone();

    QNetworkAccessManager manager_;
    QList<Server> servers_;
    QList<Server> found_;
    QQueue<QHostAddress> pendingHosts_;
    QList<QHostAddress> candidates_;
    bool fixedCandidates_ = false;
    bool scanLocalNetwork_ = true;
    quint16 port_ = DefaultPort;
    int activeProbes_ = 0;
    int activeChecks_ = 0;
    bool scanning_ = false;
};
