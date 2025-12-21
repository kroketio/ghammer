#include "parent_watch.h"

#include <QCoreApplication>
#include <QTimer>
#include <QtDebug>

#if defined(Q_OS_WIN)
#include <QWinEventNotifier>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

void parentGone(qint64 pid) {
  qWarning().noquote() << QStringLiteral("[server] parent process %1 is gone - exiting").arg(pid);
  QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
}

} // namespace

void watchParent(const qint64 pid) {
  if (pid <= 0)
    return;
#if defined(Q_OS_WIN)
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!h) {
    parentGone(pid);  // already gone (or never existed)
    return;
  }
  // Lives for the rest of the process, like the handle it waits on.
  auto *notifier = new QWinEventNotifier(h, QCoreApplication::instance());
  QObject::connect(notifier, &QWinEventNotifier::activated, notifier, [notifier, h, pid] {
    notifier->setEnabled(false);
    CloseHandle(h);
    parentGone(pid);
  });
#else
  if (static_cast<qint64>(getppid()) != pid) {
    parentGone(pid);
    return;
  }
  auto *timer = new QTimer(QCoreApplication::instance());
  timer->setInterval(1000);
  QObject::connect(timer, &QTimer::timeout, timer, [timer, pid] {
    if (static_cast<qint64>(getppid()) == pid)
      return;
    timer->stop();
    parentGone(pid);
  });
  timer->start();
#endif
  qInfo().noquote() << QStringLiteral("[server] started by process %1: exiting when it does").arg(pid);
}
