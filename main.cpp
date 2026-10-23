#include <QApplication>
#include <QAbstractSocket>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

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

enum class InstanceResult { Primary, Existing, Error };

bool isInteractiveTerminal()
{
#ifdef Q_OS_WIN
  return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#endif
}

QString instanceServerName() {
  return QStringLiteral("lanchat-instance-%1")
      .arg(QCoreApplication::applicationName().toLower());
}

bool activateExistingInstance(const QString &name) {
  QLocalSocket socket;
  socket.connectToServer(name, QIODevice::WriteOnly);
  if (!socket.waitForConnected(350)) {
    return false;
  }
  if (socket.write("A", 1) != 1) {
    return false;
  }
  socket.flush();
  socket.waitForBytesWritten(350);
  return true;
}

InstanceResult startInstanceServer(QLocalServer &server) {
  const auto name = instanceServerName();
  if (activateExistingInstance(name)) {
    return InstanceResult::Existing;
  }
  server.setSocketOptions(QLocalServer::UserAccessOption);
  if (server.listen(name)) {
    return InstanceResult::Primary;
  }
  if (server.serverError() == QAbstractSocket::AddressInUseError) {
    if (activateExistingInstance(name)) {
      return InstanceResult::Existing;
    }
    QLocalServer::removeServer(name);
    if (server.listen(name)) {
      return InstanceResult::Primary;
    }
    if (activateExistingInstance(name)) {
      return InstanceResult::Existing;
    }
  }
  return InstanceResult::Error;
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
    application.setOrganizationName(QStringLiteral("LANChat"));
    application.setApplicationName(QStringLiteral("LANChat"));
    QLocalServer instanceServer;
    const auto instanceResult = startInstanceServer(instanceServer);
    if (instanceResult == InstanceResult::Existing) return 0;
    if (instanceResult == InstanceResult::Error) {
      std::fprintf(stderr, "LANChat could not create its single-instance listener.\n");
      return 1;
    }
    TerminalUi terminal;
    watchForActivation(instanceServer, &application,
                       [&terminal] { terminal.activateFromAnotherLaunch(); });
    return application.exec();
  }
  QApplication application(argc, argv);
  application.setOrganizationName(QStringLiteral("LANChat"));
  application.setApplicationName(QStringLiteral("LANChat"));
  QLocalServer instanceServer;
  const auto instanceResult = startInstanceServer(instanceServer);
  if (instanceResult == InstanceResult::Existing) return 0;
  if (instanceResult == InstanceResult::Error) {
    std::fprintf(stderr, "LANChat could not create its single-instance listener.\n");
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
