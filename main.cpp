#include <QApplication>
#include <QCoreApplication>

#include "gui/mainwindow.h"
#include "tui/terminalui.h"

int main(int argc, char *argv[]) {
  bool terminalMode = false;
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
  MainWindow window;
  window.show();
  return application.exec();
}
