#include "appconfig.h"
#include "central/accountstore.h"
#include "central/centralserver.h"
#include "central/translationservice.h"
#include "central/webserver.h"
#include "protocol/centralprotocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QStandardPaths>

#include <cstdio>

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("lantalkd"));
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
  const QCommandLineOption passwordFileOption(
      QStringLiteral("password-file"),
      QStringLiteral("Require the password on the first line of <file> to sign "
                     "in. The LANTALK_CENTRAL_PASSWORD environment variable "
                     "works too."),
      QStringLiteral("file"));
  const QCommandLineOption webPortOption(
      QStringLiteral("web-port"),
      QStringLiteral("TCP port for the web client that phones and browsers "
                     "open (default %1; 0 turns it off).")
          .arg(CentralProtocol::DefaultWebPort),
      QStringLiteral("port"), QString::number(CentralProtocol::DefaultWebPort));
  const QCommandLineOption webCertificateOption(
      QStringLiteral("web-cert"),
      QStringLiteral("Serve the web client over HTTPS with the PEM certificate "
                     "(chain) in <file>; needs --web-key."),
      QStringLiteral("file"));
  const QCommandLineOption webKeyOption(
      QStringLiteral("web-key"),
      QStringLiteral("The unencrypted PEM private key for --web-cert."),
      QStringLiteral("file"));
  const QCommandLineOption ollamaUrlOption(
      QStringLiteral("ollama-url"),
      QStringLiteral("Translate for web clients with the Ollama server at "
                     "<url>, e.g. http://10.0.0.7:11434; needs --ollama-model. "
                     "Without it, lantalkd looks for Ollama on its network "
                     "when a web client asks."),
      QStringLiteral("url"));
  const QCommandLineOption ollamaModelOption(
      QStringLiteral("ollama-model"),
      QStringLiteral("The Ollama model for --ollama-url."),
      QStringLiteral("model"));
  const QCommandLineOption noOllamaDiscoveryOption(
      QStringLiteral("no-ollama-discovery"),
      QStringLiteral("Never look for Ollama servers on the network."));
  const QCommandLineOption maxFileSizeOption(
      QStringLiteral("max-file-size"),
      QStringLiteral("Relay files up to <MB> megabytes (default %1, the most "
                     "LANTalk accepts; 0 relays no files).")
          .arg(MaximumFileSize / (1024 * 1024)),
      QStringLiteral("MB"), QString::number(MaximumFileSize / (1024 * 1024)));
  const QCommandLineOption dataDirectoryOption(
      QStringLiteral("data-dir"),
      QStringLiteral("Keep personal PINs and the history of conversations in "
                     "<dir> (default: $STATE_DIRECTORY, or %1).")
          .arg(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)),
      QStringLiteral("dir"));
  const QCommandLineOption removePinOption(
      QStringLiteral("remove-pin"),
      QStringLiteral("Remove the PIN of <nickname>, and the history lantalkd "
                     "kept for it, e.g. when the PIN is forgotten; then exit."),
      QStringLiteral("nickname"));
  const QCommandLineOption listPinsOption(
      QStringLiteral("list-pins"),
      QStringLiteral("List the nicknames protected with a PIN and exit."));
  parser.addOption(portOption);
  parser.addOption(listenOption);
  parser.addOption(passwordFileOption);
  parser.addOption(webPortOption);
  parser.addOption(webCertificateOption);
  parser.addOption(webKeyOption);
  parser.addOption(ollamaUrlOption);
  parser.addOption(ollamaModelOption);
  parser.addOption(noOllamaDiscoveryOption);
  parser.addOption(maxFileSizeOption);
  parser.addOption(dataDirectoryOption);
  parser.addOption(removePinOption);
  parser.addOption(listPinsOption);
  parser.process(application);

  auto dataDirectory = parser.value(dataDirectoryOption);
  if (dataDirectory.isEmpty()) dataDirectory = qEnvironmentVariable("STATE_DIRECTORY");
  if (dataDirectory.isEmpty()) {
    dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  }
  if (parser.isSet(removePinOption) || parser.isSet(listPinsOption)) {
    AccountStore accounts;
    QString error;
    if (!accounts.open(dataDirectory, &error)) {
      std::fprintf(stderr, "Cannot open %s: %s\n", qPrintable(dataDirectory),
                   qPrintable(error));
      return 1;
    }
    if (parser.isSet(listPinsOption)) {
      for (const auto &name : accounts.names()) {
        std::fprintf(stdout, "%s\n", qPrintable(name));
      }
      return 0;
    }
    const auto nickname = parser.value(removePinOption);
    if (!accounts.find(nickname)) {
      std::fprintf(stderr, "%s has no PIN.\n", qPrintable(nickname));
      return 1;
    }
    if (!accounts.remove(nickname)) {
      std::fprintf(stderr, "Cannot save %s.\n", qPrintable(dataDirectory));
      return 1;
    }
    std::fprintf(stdout,
                 "Removed the PIN of %s. Restart lantalkd if it is running.\n",
                 qPrintable(nickname));
    return 0;
  }

  // Not a plain --password option: command lines are visible to every user.
  auto password = qEnvironmentVariable("LANTALK_CENTRAL_PASSWORD");
  if (parser.isSet(passwordFileOption)) {
    QFile file(parser.value(passwordFileOption));
    if (!file.open(QIODevice::ReadOnly)) {
      std::fprintf(stderr, "Cannot read the password file %s: %s\n",
                   qPrintable(file.fileName()), qPrintable(file.errorString()));
      return 2;
    }
    password = QString::fromUtf8(file.readLine()).remove(u'\r').remove(u'\n');
    if (password.isEmpty()) {
      std::fprintf(stderr, "The password file %s is empty.\n",
                   qPrintable(file.fileName()));
      return 2;
    }
  }

  bool portIsValid = false;
  const auto port = parser.value(portOption).toUShort(&portIsValid);
  if (!portIsValid || port == 0) {
    std::fprintf(stderr, "Invalid port: %s\n",
                 qPrintable(parser.value(portOption)));
    return 2;
  }
  bool webPortIsValid = false;
  const auto webPort = parser.value(webPortOption).toUShort(&webPortIsValid);
  if (!webPortIsValid) {
    std::fprintf(stderr, "Invalid web port: %s\n",
                 qPrintable(parser.value(webPortOption)));
    return 2;
  }
  if (parser.isSet(webCertificateOption) != parser.isSet(webKeyOption)) {
    std::fprintf(stderr, "--web-cert and --web-key go together.\n");
    return 2;
  }
  bool maxFileSizeIsValid = false;
  const auto maxFileSize = parser.value(maxFileSizeOption).toDouble(&maxFileSizeIsValid);
  if (!maxFileSizeIsValid || maxFileSize < 0 ||
      maxFileSize * 1024 * 1024 > double(MaximumFileSize)) {
    std::fprintf(stderr, "Invalid --max-file-size: %s (0 to %lld MB)\n",
                 qPrintable(parser.value(maxFileSizeOption)),
                 static_cast<long long>(MaximumFileSize / (1024 * 1024)));
    return 2;
  }
  if (parser.isSet(ollamaUrlOption) != parser.isSet(ollamaModelOption)) {
    std::fprintf(stderr, "--ollama-url and --ollama-model go together.\n");
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
  server.setPassword(password);
  server.setMaximumFileSize(qint64(maxFileSize * 1024 * 1024));
  QString dataError;
  const bool accountsAvailable = server.setDataDirectory(dataDirectory, &dataError);
  TranslationService translation;
  translation.setServer(parser.value(ollamaUrlOption),
                        parser.value(ollamaModelOption));
  translation.setDiscoveryEnabled(!parser.isSet(noOllamaDiscoveryOption));
  server.setTranslationService(&translation);
  const auto log = [](const QString &message) {
    std::fprintf(stdout, "%s %s\n",
                 qPrintable(QDateTime::currentDateTime().toString(
                     Qt::ISODate)),
                 qPrintable(message));
    std::fflush(stdout);
  };
  server.setLogger(log);
  QObject::connect(&translation, &TranslationService::availabilityChanged,
                   &server, [log](bool available, const QString &message) {
                     log(available
                             ? QStringLiteral("Ollama is available for Live Translate")
                             : QStringLiteral("Ollama is not available for Live Translate: %1")
                                   .arg(message));
                   });
  if (!server.listen(address, port)) {
    std::fprintf(stderr, "Cannot listen on %s:%u: %s\n",
                 qPrintable(address.toString()), unsigned(port),
                 qPrintable(server.errorString()));
    return 1;
  }
  std::fprintf(stdout,
               "lantalkd %s listening on %s:%u (%s)\n",
               LANTALK_VERSION, qPrintable(address.toString()),
               unsigned(server.serverPort()),
               password.isEmpty() ? "no password" : "password required");
  if (maxFileSize * 1024 * 1024 < double(MaximumFileSize)) {
    std::fprintf(stdout, "%s\n",
                 maxFileSize == 0
                     ? "File transfers are off"
                     : qPrintable(QStringLiteral("Files up to %1 MB").arg(maxFileSize)));
  }
  if (accountsAvailable) {
    std::fprintf(stdout, "Personal PINs and history in %s\n",
                 qPrintable(dataDirectory));
  } else {
    std::fprintf(stdout, "Personal PINs are off: %s\n", qPrintable(dataError));
  }
  if (translation.isConfigured()) {
    std::fprintf(stdout, "Live Translate with %s at %s\n",
                 qPrintable(parser.value(ollamaModelOption)),
                 qPrintable(parser.value(ollamaUrlOption)));
  }
  WebServer web(&server);
  if (webPort != 0) {
    if (parser.isSet(webCertificateOption) &&
        !web.setCertificate(parser.value(webCertificateOption),
                            parser.value(webKeyOption))) {
      std::fprintf(stderr, "%s\n", qPrintable(web.errorString()));
      return 2;
    }
    if (!web.listen(address, webPort)) {
      std::fprintf(stderr, "Cannot serve the web client on %s:%u: %s\n",
                   qPrintable(address.toString()), unsigned(webPort),
                   qPrintable(web.errorString()));
      return 1;
    }
    std::fprintf(stdout, "Web client at %s://<this host>:%u/\n",
                 web.isSecure() ? "https" : "http", unsigned(web.serverPort()));
  }
  std::fflush(stdout);
  return application.exec();
}
