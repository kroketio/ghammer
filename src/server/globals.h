#pragma once
#include <QString>
#include <QtGlobal>

namespace g {
  inline quint16 port = 3000;
  // --host; the editor's local server passes 127.0.0.1.
  inline QString host = QStringLiteral("0.0.0.0");
  // HTTP Basic credentials (--auth user:password). Empty user = no auth.
  inline QString authUser;
  inline QString authPassword;
  // --parent-pid: the editor that started this server (0 = standalone).
  inline qint64 parentPid = 0;
  inline int derivativeThreads = 0;  // 0 = idealThreadCount - 1
}
