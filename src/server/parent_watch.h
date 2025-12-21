#pragma once

#include <QtGlobal>

// --parent-pid: quits once the editor that started this server is gone. A failsafe next to the editor's own
// PR_SET_PDEATHSIG (Linux) / kill-on-close job object (Windows).
//   Unix     polls getppid() once a second (it changes when the parent dies and the child is reparented).
//   Windows  waits on a SYNCHRONIZE handle to the parent process.
void watchParent(qint64 pid);
