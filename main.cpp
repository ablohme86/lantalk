#include <QApplication>
#include <QAbstractSocket>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>

#include "appconfig.h"
#include "gui/appicon.h"
#include "gui/mainwindow.h"
#include "tui/terminalui.h"

#include <cstdio>
#include <functional>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

enum class InstanceResult { Primary, Existing, Unresponsive, Error };
enum class Activation { Activated, NotResponding, NoInstance };

bool isInteractiveTerminal()
{
#ifdef Q_OS_WIN
  return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#endif
}

QString instanceServerName() {
  return QStringLiteral("lantalk-instance-%1")
      .arg(QCoreApplication::applicationName().toLower());
}

// The running instance answers 'K' once it has shown its window (versions
// before 1.9 just hang up). An instance that accepts the connection but
// neither answers nor hangs up is hung; its listener would otherwise make
// every later launch exit without a word.
Activation activateExistingInstance(const QString &name) {
  QLocalSocket socket;
  socket.connectToServer(name, QIODevice::ReadWrite);
  if (!socket.waitForConnected(350)) {
    return Activation::NoInstance;
  }
  if (socket.write("A", 1) != 1) {
    return Activation::NotResponding;
  }
  socket.flush();
  socket.waitForBytesWritten(350);
  QByteArray reply;
  QElapsedTimer elapsed;
  elapsed.start();
  while (!reply.contains('K') && elapsed.elapsed() < 3000 &&
         socket.state() == QLocalSocket::ConnectedState) {
    if (socket.waitForReadyRead(int(3000 - elapsed.elapsed()))) {
      reply += socket.readAll();
    }
  }
  reply += socket.readAll();
  return reply.contains('K') || socket.state() != QLocalSocket::ConnectedState
             ? Activation::Activated
             : Activation::NotResponding;
}

InstanceResult instanceResultFor(Activation activation) {
  return activation == Activation::Activated ? InstanceResult::Existing
                                             : InstanceResult::Unresponsive;
}

InstanceResult startInstanceServer(QLocalServer &server) {
  const auto name = instanceServerName();
  if (const auto activation = activateExistingInstance(name);
      activation != Activation::NoInstance) {
    return instanceResultFor(activation);
  }
  server.setSocketOptions(QLocalServer::UserAccessOption);
  if (server.listen(name)) {
    return InstanceResult::Primary;
  }
  if (server.serverError() == QAbstractSocket::AddressInUseError) {
    if (const auto activation = activateExistingInstance(name);
        activation != Activation::NoInstance) {
      return instanceResultFor(activation);
    }
    QLocalServer::removeServer(name);
    if (server.listen(name)) {
      return InstanceResult::Primary;
    }
    if (const auto activation = activateExistingInstance(name);
        activation != Activation::NoInstance) {
      return instanceResultFor(activation);
    }
  }
  return InstanceResult::Error;
}

QString unresponsiveInstanceMessage() {
#ifdef Q_OS_WIN
  const auto howToEnd = QStringLiteral("End lantalk.exe in Task Manager");
#else
  const auto howToEnd = QStringLiteral("Stop the lantalk process");
#endif
  return QStringLiteral("LANTalk is already running but does not respond. "
                        "%1 and start LANTalk again.")
      .arg(howToEnd);
}

void watchForActivation(QLocalServer &server, QObject *context,
                        const std::function<void()> &activate) {
  QObject::connect(&server, &QLocalServer::newConnection, context,
                   [&server, context, activate] {
                     while (auto *socket = server.nextPendingConnection()) {
                       socket->setParent(context);
                       const auto consumeRequest = [socket, activate] {
                         const auto request = socket->readAll();
                         if (request.contains('A')) {
                           activate();
                           socket->write("K", 1);
                         }
                         socket->disconnectFromServer();
                       };
                       QObject::connect(socket, &QLocalSocket::readyRead,
                                        context, consumeRequest);
                       QObject::connect(socket, &QLocalSocket::disconnected,
                                        socket, [socket] { socket->deleteLater(); });
                       if (socket->bytesAvailable() > 0) {
                         consumeRequest();
                       }
                       QTimer::singleShot(2000, socket, [socket] {
                         if (socket->state() != QLocalSocket::UnconnectedState) {
                           socket->disconnectFromServer();
                         }
                       });
                     }
                   });
}

} // namespace

int main(int argc, char *argv[]) {
  bool terminalMode = isInteractiveTerminal();
  for (int index = 1; index < argc; ++index) {
    if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--tui")) {
      terminalMode = true;
      break;
    }
  }
  if (terminalMode) {
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("LANTalk"));
    application.setApplicationName(QStringLiteral("LANTalk"));
    application.setApplicationVersion(QStringLiteral(LANTALK_VERSION));
    migrateLegacySettings();
    QLocalServer instanceServer;
    const auto instanceResult = startInstanceServer(instanceServer);
    if (instanceResult == InstanceResult::Existing) return 0;
    if (instanceResult == InstanceResult::Unresponsive) {
      std::fprintf(stderr, "%s\n", qPrintable(unresponsiveInstanceMessage()));
      return 1;
    }
    if (instanceResult == InstanceResult::Error) {
      std::fprintf(stderr, "LANTalk could not create its single-instance listener.\n");
      return 1;
    }
    TerminalUi terminal;
    watchForActivation(instanceServer, &application,
                       [&terminal] { terminal.activateFromAnotherLaunch(); });
    return application.exec();
  }
  QApplication application(argc, argv);
  application.setOrganizationName(QStringLiteral("LANTalk"));
  application.setApplicationName(QStringLiteral("LANTalk"));
  application.setApplicationVersion(QStringLiteral(LANTALK_VERSION));
  migrateLegacySettings();
  auto font = application.font();
  const int configuredFontSize =
      QSettings()
          .value(QStringLiteral("appearance/fontSize"), 11)
          .toInt();
  if (configuredFontSize >= 8 && configuredFontSize <= 32) {
    font.setPointSize(configuredFontSize);
    application.setFont(font);
  }
  QLocalServer instanceServer;
  const auto instanceResult = startInstanceServer(instanceServer);
  if (instanceResult == InstanceResult::Existing) return 0;
  if (instanceResult == InstanceResult::Unresponsive) {
    QMessageBox::warning(nullptr, QStringLiteral("LANTalk"),
                         unresponsiveInstanceMessage());
    return 1;
  }
  if (instanceResult == InstanceResult::Error) {
    std::fprintf(stderr, "LANTalk could not create its single-instance listener.\n");
    return 1;
  }
  application.setWindowIcon(createAppIcon());
  MainWindow *windowPointer = nullptr;
  bool activationPending = false;
  watchForActivation(instanceServer, &application, [&] {
    if (windowPointer) windowPointer->activateFromAnotherLaunch();
    else activationPending = true;
  });
  MainWindow window;
  windowPointer = &window;
  window.show();
  if (activationPending) window.activateFromAnotherLaunch();
  return application.exec();
}
