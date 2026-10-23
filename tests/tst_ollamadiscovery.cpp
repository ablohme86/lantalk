#include "testsupport.h"

#include "ollamadiscovery.h"

#include <QSignalSpy>

using namespace TestSupport;

class OllamaDiscoveryTest final : public QObject {
  Q_OBJECT

private slots:
  void init() { isolateSettings(); }

  void findsServersAmongCandidates() {
    FakeOllama ollama({QStringLiteral("translator:7b"), QStringLiteral("llama3")});
    QVERIFY(ollama.listen());
    OllamaDiscovery discovery;
    discovery.setPort(ollama.port());
    // 127.0.0.2 is on the loopback network too, with nothing listening.
    discovery.setCandidates({QHostAddress(QStringLiteral("127.0.0.1")),
                             QHostAddress(QStringLiteral("127.0.0.2"))});
    QSignalSpy changed(&discovery, &OllamaDiscovery::serversChanged);
    QSignalSpy finished(&discovery, &OllamaDiscovery::scanFinished);

    discovery.scan();
    QVERIFY(discovery.isScanning());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(changed.size(), 1);
    const auto servers = discovery.servers();
    QCOMPARE(servers.size(), 1);
    QCOMPARE(servers.first().url,
             QStringLiteral("http://127.0.0.1:%1").arg(ollama.port()));
    QCOMPARE(servers.first().models,
             (QStringList{QStringLiteral("translator:7b"), QStringLiteral("llama3")}));

    // The same result again does not count as a change.
    discovery.scan();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(changed.size(), 1);
  }

  void checksTheConfiguredServer() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    configureOllama(ollama.url() + QStringLiteral("/"), QStringLiteral("translator"));
    OllamaDiscovery discovery;
    discovery.setCandidates({});
    QSignalSpy finished(&discovery, &OllamaDiscovery::scanFinished);

    discovery.scan();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(discovery.servers().size(), 1);
    QCOMPARE(discovery.servers().first().url, ollama.url());
  }

  void ignoresServersWithoutModels() {
    FakeOllama ollama({});
    QVERIFY(ollama.listen());
    OllamaDiscovery discovery;
    discovery.setPort(ollama.port());
    discovery.setCandidates({QHostAddress(QStringLiteral("127.0.0.1"))});
    QSignalSpy finished(&discovery, &OllamaDiscovery::scanFinished);

    discovery.scan();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(discovery.servers().isEmpty());
  }

  void findsNothingWhenNothingListens() {
    FakeOllama ollama({QStringLiteral("translator")});
    QVERIFY(ollama.listen());
    const auto port = ollama.port();
    ollama.close();
    OllamaDiscovery discovery;
    discovery.setPort(port);
    discovery.setCandidates({QHostAddress(QStringLiteral("127.0.0.1"))});
    QSignalSpy changed(&discovery, &OllamaDiscovery::serversChanged);
    QSignalSpy finished(&discovery, &OllamaDiscovery::scanFinished);

    discovery.scan();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(discovery.servers().isEmpty());
    QCOMPARE(changed.size(), 0);
  }

  void localCandidatesIncludeThisComputer() {
    const auto candidates = OllamaDiscovery::localCandidates();
    QVERIFY(candidates.contains(QHostAddress(QHostAddress::LocalHost)));
    // Subnets are capped at a /22 per network card.
    QVERIFY(candidates.size() < 5000);
  }
};

QTEST_GUILESS_MAIN(OllamaDiscoveryTest)
#include "tst_ollamadiscovery.moc"
