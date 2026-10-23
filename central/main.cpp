#include "central/centralserver.h"
#include "protocol/centralprotocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>

#include <cstdio>

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("lantalk-centralized-service"));
  application.setApplicationVersion(QStringLiteral(LANTALK_VERSION));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Relays LANTalk conversations between clients that have it set as "
      "their central host."));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption portOption(
      {QStringLiteral("p"), QStringLiteral("port")},
      QStringLiteral("TCP port to listen on (default %1).")
          .arg(CentralProtocol::DefaultCentralPort),
      QStringLiteral("port"),
      QString::number(CentralProtocol::DefaultCentralPort));
  const QCommandLineOption listenOption(
      {QStringLiteral("l"), QStringLiteral("listen")},
      QStringLiteral("Address to listen on (default: all addresses)."),
      QStringLiteral("address"));
  parser.addOption(portOption);
  parser.addOption(listenOption);
  parser.process(application);

  bool portIsValid = false;
  const auto port = parser.value(portOption).toUShort(&portIsValid);
  if (!portIsValid || port == 0) {
    std::fprintf(stderr, "Invalid port: %s\n",
                 qPrintable(parser.value(portOption)));
    return 2;
  }
  QHostAddress address(QHostAddress::Any);
  if (parser.isSet(listenOption) &&
      !address.setAddress(parser.value(listenOption))) {
    std::fprintf(stderr, "Invalid listen address: %s\n",
                 qPrintable(parser.value(listenOption)));
    return 2;
  }

  CentralServer server;
  server.setLogger([](const QString &message) {
    std::fprintf(stdout, "%s %s\n",
                 qPrintable(QDateTime::currentDateTime().toString(
                     Qt::ISODate)),
                 qPrintable(message));
    std::fflush(stdout);
  });
  if (!server.listen(address, port)) {
    std::fprintf(stderr, "Cannot listen on %s:%u: %s\n",
                 qPrintable(address.toString()), unsigned(port),
                 qPrintable(server.errorString()));
    return 1;
  }
  std::fprintf(stdout, "lantalk-centralized-service %s listening on %s:%u\n",
               LANTALK_VERSION, qPrintable(address.toString()),
               unsigned(server.serverPort()));
  std::fflush(stdout);
  return application.exec();
}
