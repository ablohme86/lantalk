#include <QApplication>
#include <QCoreApplication>

#include "gui/appicon.h"
#include "gui/mainwindow.h"
#include "tui/terminalui.h"

#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

bool isInteractiveTerminal()
{
#ifdef Q_OS_WIN
  return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#endif
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
    TerminalUi terminal;
    return application.exec();
  }
  QApplication application(argc, argv);
  application.setOrganizationName(QStringLiteral("LANChat"));
  application.setApplicationName(QStringLiteral("LANChat"));
  application.setWindowIcon(createAppIcon());
  MainWindow window;
  window.show();
  return application.exec();
}
