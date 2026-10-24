#include "testsupport.h"

#include "appconfig.h"

#include "central/centralserver.h"
#include "central/translationservice.h"
#include "central/webserver.h"
#include "ollamadiscovery.h"
#include "protocol/centralclient.h"
#include "protocol/centralprotocol.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSignalSpy>

using namespace TestSupport;

namespace {

// A plain HTTP request; returns the whole response.
QByteArray httpGet(quint16 port, const QByteArray &path) {
  QTcpSocket socket;
  socket.connectToHost(QHostAddress::LocalHost, port);
  if (!socket.waitForConnected(3000)) return {};
  socket.write("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
  QByteArray response;
  QElapsedTimer timer;
  timer.start();
  while (socket.state() == QAbstractSocket::ConnectedState &&
         timer.elapsed() < 3000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    response += socket.readAll();
  }
  return response + socket.readAll();
}

// What the browser does: a WebSocket to /ws, one CENTRAL_ line per masked
// text frame.
class WebPeer {
public:
  explicit WebPeer(quint16 port) {
    QObject::connect(&socket_, &QTcpSocket::readyRead, &socket_,
                     [this] { read(); });
    socket_.connectToHost(QHostAddress::LocalHost, port);
    socket_.waitForConnected(3000);
    key_ = QByteArray::number(QRandomGenerator::global()->generate64())
               .toBase64();
    socket_.write("GET /ws HTTP/1.1\r\nHost: localhost\r\n"
                  "Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                  "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                  key_ + "\r\n\r\n");
  }

  void send(const QByteArray &line) {
    QByteArray frame;
    frame.append(char(0x81));
    const auto size = line.size();
    if (size < 126) {
      frame.append(char(0x80 | size));
    } else {
      frame.append(char(0x80 | 126));
      frame.append(char(size >> 8));
      frame.append(char(size & 0xff));
    }
    const QByteArray mask("\x12\x34\x56\x78", 4);
    frame += mask;
    for (qsizetype index = 0; index < size; ++index) {
      frame.append(char(line.at(index) ^ mask.at(index % 4)));
    }
    socket_.write(frame);
  }

  bool upgraded() const { return upgraded_; }
  QString acceptHeader() const { return accept_; }
  QStringList lines() const { return lines_; }
  QString lineStartingWith(const QString &prefix) const {
    for (const auto &line : lines_) {
      if (line.startsWith(prefix)) return line;
    }
    return {};
  }
  bool isConnected() const {
    return socket_.state() == QAbstractSocket::ConnectedState;
  }

private:
  void read() {
    buffer_ += socket_.readAll();
    if (!upgraded_) {
      const auto end = buffer_.indexOf("\r\n\r\n");
      if (end < 0) return;
      const auto head = buffer_.left(end);
      buffer_.remove(0, end + 4);
      upgraded_ = head.startsWith("HTTP/1.1 101");
      for (const auto &line : head.split('\n')) {
        if (line.toLower().startsWith("sec-websocket-accept:")) {
          accept_ = QString::fromLatin1(line.mid(21).trimmed());
        }
      }
    }
    // Server frames are unmasked and short enough for 16-bit lengths here.
    while (buffer_.size() >= 2) {
      qsizetype length = quint8(buffer_.at(1)) & 0x7f;
      qsizetype offset = 2;
      if (length == 126) {
        if (buffer_.size() < 4) return;
        length = (quint8(buffer_.at(2)) << 8) | quint8(buffer_.at(3));
        offset = 4;
      }
      if (buffer_.size() < offset + length) return;
      if ((quint8(buffer_.at(0)) & 0x0f) == 0x1) {
        lines_.append(QString::fromUtf8(buffer_.mid(offset, length)));
      }
      buffer_.remove(0, offset + length);
    }
  }

  QTcpSocket socket_;
  QByteArray key_;
  QByteArray buffer_;
  QString accept_;
  QStringList lines_;
  bool upgraded_ = false;
};

// Signs a WebPeer in without a password.
void signIn(WebPeer &phone, const QString &nickname) {
  QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_CHALLENGE ")).isEmpty());
  phone.send("CENTRAL_HELLO 1 " + CentralProtocol::encodeField(nickname) + " 45454 -");
  QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_WELCOME ")).isEmpty());
}

QByteArray translateRequest(const QJsonObject &request) {
  return QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64(
      QByteArray::Base64UrlEncoding);
}

QString webPeerId(CentralClient &client, const QString &nickname) {
  for (const auto &peer : client.peers()) {
    if (peer.nickname == nickname) return peer.id;
  }
  return {};
}

} // namespace

class WebTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() { isolateSettings(); }

  void servesThePage() {
    CentralServer central;
    WebServer web(&central);
    QVERIFY(web.listen(QHostAddress::LocalHost, 0));

    const auto page = httpGet(web.serverPort(), "/");
    QVERIFY(page.startsWith("HTTP/1.1 200 OK"));
    QVERIFY(page.contains("Content-Type: text/html"));
    QVERIFY(page.contains("<script src=\"app.js\""));
    QVERIFY(httpGet(web.serverPort(), "/app.js").contains("CENTRAL_HELLO"));
    QVERIFY(httpGet(web.serverPort(), "/manifest.webmanifest")
                .startsWith("HTTP/1.1 200 OK"));
    QVERIFY(httpGet(web.serverPort(), "/../etc/passwd")
                .startsWith("HTTP/1.1 404"));
    QVERIFY(httpGet(web.serverPort(), "/ws").startsWith("HTTP/1.1 426"));
  }

  void webClientChatsWithLanTalk() {
    CentralServer central;
    central.setPassword(QStringLiteral("hemmelig"));
    QVERIFY(central.listen(QHostAddress::LocalHost, 0));
    WebServer web(&central);
    QVERIFY(web.listen(QHostAddress::LocalHost, 0));

    ConnectionRegistry registry;
    QObject owner;
    QList<ChatSession *> incoming;
    CentralClient alice;
    QObject::connect(&alice, &CentralClient::incomingConnection, &owner,
                     [&](RelayTransport *transport) {
                       auto *session =
                           new ChatSession(transport, &owner, &registry);
                       session->beginHandshake(QStringLiteral("alice"));
                       incoming.append(session);
                     });
    alice.start(QStringLiteral("127.0.0.1:%1").arg(central.serverPort()),
                QStringLiteral("alice"), DefaultPort,
                QStringLiteral("hemmelig"));
    QTRY_VERIFY(alice.isReady());

    WebPeer phone(web.serverPort());
    QTRY_VERIFY(phone.upgraded());
    QCOMPARE(phone.acceptHeader().isEmpty(), false);
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_CHALLENGE "))
                     .isEmpty());
    const auto nonce = phone.lineStartingWith(QStringLiteral("CENTRAL_CHALLENGE "))
                           .section(u' ', 1, 1)
                           .toLatin1();
    phone.send("CENTRAL_HELLO 1 " +
               CentralProtocol::encodeField(QStringLiteral("Mobil Mia")) +
               " 45454 " +
               CentralProtocol::authProof(QStringLiteral("hemmelig"), nonce));
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_WELCOME "))
                     .isEmpty());
    QVERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_PEER ")).isEmpty());
    QTRY_VERIFY(!webPeerId(alice, QStringLiteral("Mobil Mia")).isEmpty());
    // LANTalk learns that Mia uses the web client.
    QTRY_VERIFY([&] {
      for (const auto &peer : alice.peers()) {
        if (peer.nickname == QStringLiteral("Mobil Mia")) return peer.web;
      }
      return false;
    }());

    // The phone opens a conversation with alice and they shake hands.
    const auto aliceId =
        phone.lineStartingWith(QStringLiteral("CENTRAL_PEER ")).section(u' ', 1, 1);
    phone.send("CENTRAL_OPEN web-1 " + aliceId.toLatin1());
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_OPENED web-1"))
                     .isEmpty());
    phone.send("CENTRAL_DATA web-1 NICKNAME Mobil Mia");
    QTRY_COMPARE(incoming.size(), 1);
    auto *session = incoming.first();
    QTRY_VERIFY(session->isHandshakeComplete());
    QCOMPARE(session->peerName(), QStringLiteral("Mobil Mia"));
    QVERIFY(session->peerIsWebClient());
    QTRY_VERIFY(!phone.lineStartingWith(
                          QStringLiteral("CENTRAL_DATA web-1 NICKNAME alice"))
                     .isEmpty());

    QSignalSpy received(session, &ChatSession::messageReceived);
    phone.send("CENTRAL_DATA web-1 MESSAGE " +
               QStringLiteral("Hei fra mobilen 📱").toUtf8().toBase64(
                   QByteArray::Base64UrlEncoding));
    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.first().first().toString(),
             QStringLiteral("Hei fra mobilen 📱"));

    session->sendMessage(QStringLiteral("Hei tilbake"));
    const auto expected =
        QStringLiteral("CENTRAL_DATA web-1 MESSAGE ") +
        QString::fromLatin1(QStringLiteral("Hei tilbake").toUtf8().toBase64(
            QByteArray::Base64UrlEncoding));
    QTRY_VERIFY(!phone.lineStartingWith(expected).isEmpty());

    // Closing the page ends the conversation on alice's side.
    QSignalSpy disconnected(session, &ChatSession::disconnected);
    phone.send(QByteArray("CENTRAL_CLOSE web-1"));
    QTRY_COMPARE(disconnected.size(), 1);
  }

  void serverTranslatesWithTheConfiguredOllama() {
    FakeOllama ollama({QStringLiteral("gemma")});
    QVERIFY(ollama.listen());
    CentralServer central;
    TranslationService translation;
    translation.setServer(ollama.url(), QStringLiteral("gemma"));
    central.setTranslationService(&translation);
    WebServer web(&central);
    QVERIFY(web.listen(QHostAddress::LocalHost, 0));
    WebPeer phone(web.serverPort());
    signIn(phone, QStringLiteral("mia"));
    // Once checked, the configured server is reported as available.
    QTRY_VERIFY([&] {
      for (const auto &line : phone.lines()) {
        const auto info = QByteArray::fromBase64(line.section(u' ', 1).toLatin1(),
                                                 QByteArray::Base64UrlEncoding);
        if (line.startsWith(QStringLiteral("CENTRAL_OLLAMA ")) &&
            info.contains("\"configured\":true") && info.contains("\"available\":true")) {
          return true;
        }
      }
      return false;
    }());

    phone.send("CENTRAL_TRANSLATE t-1 " +
               translateRequest({{QStringLiteral("text"), QStringLiteral("Hei verden")},
                                 {QStringLiteral("source"), QStringLiteral("Norwegian Bokmål")},
                                 {QStringLiteral("target"), QStringLiteral("English")}}));
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_TRANSLATED t-1 ")).isEmpty());
    QCOMPARE(CentralProtocol::decodeField(
                 phone.lineStartingWith(QStringLiteral("CENTRAL_TRANSLATED t-1 "))
                     .section(u' ', 2)),
             QStringLiteral("<<Hei verden>>"));
  }

  void serverOnlyUsesOllamaItFound() {
    FakeOllama ollama({QStringLiteral("gemma")});
    QVERIFY(ollama.listen());
    CentralServer central;
    TranslationService translation;
    translation.discovery()->setCandidates({QHostAddress(QHostAddress::LocalHost)});
    translation.discovery()->setPort(ollama.port());
    central.setTranslationService(&translation);
    WebServer web(&central);
    QVERIFY(web.listen(QHostAddress::LocalHost, 0));
    WebPeer phone(web.serverPort());
    signIn(phone, QStringLiteral("mia"));
    const QJsonObject request{
        {QStringLiteral("text"), QStringLiteral("Hei")},
        {QStringLiteral("source"), QStringLiteral("Norwegian Bokmål")},
        {QStringLiteral("target"), QStringLiteral("English")},
        {QStringLiteral("url"), ollama.url()},
        {QStringLiteral("model"), QStringLiteral("gemma")}};

    // Before a search finds it, the address is refused, not contacted.
    phone.send("CENTRAL_TRANSLATE t-2 " + translateRequest(request));
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_TRANSLATE_FAILED t-2 "))
                     .isEmpty());
    QVERIFY(ollama.translatedTexts().isEmpty());

    // A search finds it; then the server is listed and used.
    phone.send(QByteArray("CENTRAL_OLLAMA_SCAN"));
    QTRY_VERIFY(!translation.discovery()->servers().isEmpty());
    QTRY_VERIFY([&] {
      for (const auto &line : phone.lines()) {
        if (line.startsWith(QStringLiteral("CENTRAL_OLLAMA ")) &&
            QByteArray::fromBase64(line.section(u' ', 1).toLatin1(),
                                   QByteArray::Base64UrlEncoding)
                .contains(ollama.url().toUtf8())) {
          return true;
        }
      }
      return false;
    }());
    phone.send("CENTRAL_TRANSLATE t-3 " + translateRequest(request));
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_TRANSLATED t-3 ")).isEmpty());
    QCOMPARE(ollama.translatedTexts(), QStringList{QStringLiteral("Hei")});
  }

  void serverRefusesFilesOverTheLimit() {
    CentralServer central;
    central.setMaximumFileSize(1000);
    QVERIFY(central.listen(QHostAddress::LocalHost, 0));
    WebServer web(&central);
    QVERIFY(web.listen(QHostAddress::LocalHost, 0));
    ConnectionRegistry registry;
    QObject owner;
    QList<ChatSession *> incoming;
    CentralClient alice;
    QObject::connect(&alice, &CentralClient::incomingConnection, &owner,
                     [&](RelayTransport *transport) {
                       auto *session = new ChatSession(transport, &owner, &registry);
                       session->beginHandshake(QStringLiteral("alice"));
                       incoming.append(session);
                     });
    alice.start(QStringLiteral("127.0.0.1:%1").arg(central.serverPort()),
                QStringLiteral("alice"), DefaultPort);
    QTRY_VERIFY(alice.isReady());

    // A client that ignores CENTRAL_FILE_LIMIT, like an older LANTalk.
    WebPeer phone(web.serverPort());
    signIn(phone, QStringLiteral("mia"));
    QCOMPARE(phone.lineStartingWith(QStringLiteral("CENTRAL_FILE_LIMIT ")),
             QStringLiteral("CENTRAL_FILE_LIMIT 1000"));
    const auto aliceId =
        phone.lineStartingWith(QStringLiteral("CENTRAL_PEER ")).section(u' ', 1, 1);
    phone.send("CENTRAL_OPEN f-1 " + aliceId.toLatin1());
    phone.send("CENTRAL_DATA f-1 NICKNAME mia");
    QTRY_COMPARE(incoming.size(), 1);
    QTRY_VERIFY(incoming.first()->isHandshakeComplete());
    QSignalSpy progress(incoming.first(), &ChatSession::fileTransferProgress);
    QSignalSpy received(incoming.first(), &ChatSession::messageReceived);

    const auto field = [](const QByteArray &value) {
      return value.toBase64(QByteArray::Base64UrlEncoding);
    };
    phone.send("CENTRAL_DATA f-1 FILE_BEGIN big " + field("big.bin") + ' ' +
               field("application/octet-stream") + " 2000");
    phone.send("CENTRAL_DATA f-1 FILE_CHUNK big " + field(QByteArray(2000, 'x')));
    phone.send(QByteArray("CENTRAL_DATA f-1 FILE_END big"));
    // A file within the limit, then a message, still get through.
    phone.send("CENTRAL_DATA f-1 FILE_BEGIN small " + field("small.txt") + ' ' +
               field("text/plain") + " 5");
    phone.send("CENTRAL_DATA f-1 FILE_CHUNK small " + field("hello"));
    phone.send(QByteArray("CENTRAL_DATA f-1 FILE_END small"));
    phone.send("CENTRAL_DATA f-1 MESSAGE " + field("done"));
    QTRY_COMPARE(received.size(), 1);
    QTRY_VERIFY(!phone.lineStartingWith(
                          QStringLiteral("CENTRAL_FILE_REJECTED f-1 big 1000"))
                     .isEmpty());
    for (const auto &event : progress) {
      QCOMPARE(event.first().toString(), QStringLiteral("small"));
    }
    QVERIFY(!progress.isEmpty());
  }

  void wrongPasswordIsRejected() {
    CentralServer central;
    central.setPassword(QStringLiteral("hemmelig"));
    WebServer web(&central);
    QVERIFY(web.listen(QHostAddress::LocalHost, 0));
    WebPeer phone(web.serverPort());
    QTRY_VERIFY(!phone.lineStartingWith(QStringLiteral("CENTRAL_CHALLENGE "))
                     .isEmpty());
    phone.send("CENTRAL_HELLO 1 " +
               CentralProtocol::encodeField(QStringLiteral("mallory")) +
               " 45454 " + QByteArray(64, 'a'));
    QTRY_VERIFY_WITH_TIMEOUT(
        !phone.lineStartingWith(QStringLiteral("CENTRAL_REJECT ")).isEmpty(),
        5000);
    QTRY_VERIFY(!phone.isConnected());
    QCOMPARE(central.clientCount(), 0);
  }
};

QTEST_GUILESS_MAIN(WebTest)
#include "tst_web.moc"
