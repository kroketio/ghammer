#include "logger.h"

#include <QDateTime>
#include <QTextStream>

void customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
  if (type == QtWarningMsg &&
      msg.startsWith(QLatin1String("QObject::disconnect: wildcard call disconnects from destroyed signal of QTcpSocket")))
    return;
  QString logMessage;
  switch (type) {
    case QtDebugMsg:
      logMessage = "\033[90m[D]\033[0m ";
      break;
    case QtInfoMsg:
      logMessage = "[I] ";
      break;
    case QtWarningMsg:
      logMessage = "[W] ";
      break;
    case QtCriticalMsg:
      logMessage = "[C] ";
      break;
    default:
      logMessage = "[U] ";
      break;
  }

  auto fn = QString(context.file);
  auto fnspl = fn.split("/");

  int seconds = QDateTime::currentDateTime().time().second();
  logMessage += QString::number(seconds) + " ";
  logMessage +=
      QString("\033[90m%4\t%1:%2\033[0m\n    %3").arg(fnspl.last()).arg(context.line).arg(msg).arg(context.function);

  QTextStream stream(type != QtInfoMsg ? stderr : stdout);
  stream << logMessage << "\n";
}
