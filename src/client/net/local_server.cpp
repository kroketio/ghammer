#include "local_server.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QtDebug>

#include <algorithm>

#if defined(Q_OS_LINUX)
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace net {

namespace {

constexpr int kMaxExitsPerMinute = 3;
constexpr int kStopTimeoutMs = 3000;
constexpr int kReadyTimeoutMs = 20000;
const QByteArray kReadyLine = QByteArrayLiteral("GHAMMER_SERVER_READY");

QString randomPassword(int length) {
  // No look-alikes (0/O, 1/l/I), nothing that needs quoting.
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  QString s;
  s.reserve(length);
  for (int i = 0; i < length; ++i)
    s += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]);
  return s;
}

quint16 freeLocalPort() {
  QTcpServer probe;
  if (!probe.listen(QHostAddress::LocalHost, 0))
    return 0;
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

} // namespace

LocalServer &LocalServer::instance() {
  static auto *s = new LocalServer();  // lives for the whole process
  return *s;
}

LocalServer::LocalServer() : QObject(nullptr) {
  m_user = QStringLiteral("local-%1").arg(QRandomGenerator::system()->bounded(0x1000000), 6, 16, QLatin1Char('0'));
  m_password = randomPassword(24);
  m_restart.setSingleShot(true);
  connect(&m_restart, &QTimer::timeout, this, &LocalServer::launch);
  m_readyWatch.setSingleShot(true);
  m_readyWatch.setInterval(kReadyTimeoutMs);
  connect(&m_readyWatch, &QTimer::timeout, this, [this] {
    if (m_state != State::Starting || !m_proc)
      return;
    QProcess *proc = m_proc;
    m_proc = nullptr;
    proc->disconnect(this);
    proc->kill();
    proc->waitForFinished(2000);
    proc->deleteLater();
    fail(tr("The local server did not start within %1 s").arg(kReadyTimeoutMs / 1000));
  });
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
    m_quitting = true;
    stop();
  });
}

QString LocalServer::binaryPath() {
  // applicationDirPath() resolves symlinks, so a link to ghammer_client still finds the real install.
  QString name = QStringLiteral("ghammer_server");
#if defined(Q_OS_WIN)
  name += QStringLiteral(".exe");
#endif
  return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

QUrl LocalServer::url() const {
  if (m_port == 0)
    return {};
  return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_port));
}

void LocalServer::setState(const State s) {
  if (s == m_state)
    return;
  m_state = s;
  emit stateChanged(s);
}

void LocalServer::fail(const QString &error) {
  m_lastError = error;
  qWarning().noquote() << "[local server]" << error;
  setState(State::Failed);
}

void LocalServer::start() {
  // Failed stays failed until restart() / stop(): start() runs on every state change and must not loop.
  if (m_proc || m_restart.isActive() || m_quitting || m_state == State::Failed)
    return;
  if (!m_allowed) {
    m_lastError = tr("Not started in scripted runs");
    setState(State::Stopped);
    return;
  }
  m_exits.clear();
  launch();
}

void LocalServer::restart() {
  stop();  // may already start it again through stateChanged (local mode)
  start();
}

void LocalServer::launch() {
  if (m_proc)
    return;
  const QString bin = binaryPath();
  const QFileInfo fi(bin);
  if (!fi.exists()) {
    fail(tr("%1 not found").arg(QDir::toNativeSeparators(bin)));
    return;
  }
  if (!fi.isExecutable()) {
    fail(tr("%1 is not executable").arg(QDir::toNativeSeparators(bin)));
    return;
  }
  m_port = freeLocalPort();
  if (m_port == 0) {
    fail(tr("No free port on 127.0.0.1"));
    return;
  }
  m_lastError.clear();
  m_buffer.clear();

  auto *proc = new QProcess(this);
  m_proc = proc;
  proc->setProgram(bin);
  proc->setArguments({QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
                      QStringLiteral("--port"), QString::number(m_port),
                      QStringLiteral("--auth"), m_user + QLatin1Char(':') + m_password,
                      QStringLiteral("--parent-pid"), QString::number(QCoreApplication::applicationPid())});
  proc->setProcessChannelMode(QProcess::MergedChannels);
#if defined(Q_OS_LINUX)
  // Runs in the child between fork and exec (async-signal-safe calls only): SIGTERM when the forking
  // thread exits; exit now if the editor already died.
  const pid_t parent = getpid();
  proc->setChildProcessModifier([parent] {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent)
      _exit(1);
  });
#endif
  connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] { onOutput(proc); });
  connect(proc, &QProcess::finished, this,
          [this, proc](int code, QProcess::ExitStatus status) { onFinished(proc, code, status); });
  connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
    if (e != QProcess::FailedToStart || proc != m_proc)
      return;  // crashes arrive through finished()
    m_proc = nullptr;
    m_readyWatch.stop();
    proc->deleteLater();
    fail(tr("Could not start %1: %2").arg(QDir::toNativeSeparators(proc->program()), proc->errorString()));
  });
#if defined(Q_OS_WIN)
  connect(proc, &QProcess::started, this, [this, proc] { attachToJob(proc); });
#endif

  qInfo().noquote() << QStringLiteral("[local server] starting %1 on 127.0.0.1:%2")
                           .arg(QDir::toNativeSeparators(bin)).arg(m_port);
  setState(State::Starting);
  m_readyWatch.start();
  proc->start();
}

void LocalServer::stop() {
  m_restart.stop();
  m_readyWatch.stop();
  QProcess *proc = m_proc;
  m_proc = nullptr;  // finished() of a process we stopped is not a crash
  if (proc) {
    qInfo() << "[local server] stopping";
    proc->disconnect(this);
#if defined(Q_OS_WIN)
    // A console program has no window to close: terminate() would do nothing.
    proc->kill();
#else
    proc->terminate();
    if (!proc->waitForFinished(kStopTimeoutMs))
      proc->kill();
#endif
    proc->waitForFinished(2000);
    proc->deleteLater();
  }
  setState(State::Stopped);
}

void LocalServer::onOutput(QProcess *proc) {
  if (proc != m_proc)
    return;
  m_buffer += proc->readAllStandardOutput();
  qsizetype nl;
  while ((nl = m_buffer.indexOf('\n')) >= 0) {
    QByteArray line = m_buffer.left(nl);
    m_buffer.remove(0, nl + 1);
    if (line.endsWith('\r'))
      line.chop(1);
    if (line.startsWith(kReadyLine)) {
      m_readyWatch.stop();
      qInfo().noquote() << QStringLiteral("[local server] ready at %1").arg(url().toString());
      setState(State::Running);
      emit ready();
      continue;
    }
    logLine(QString::fromUtf8(line));
  }
}

// Server log entries are two lines: "[I] <sec> <function> <file:line>" and the indented message.
// Only the message is kept, at the entry's level.
void LocalServer::logLine(const QString &raw) {
  static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*m"));
  static const QRegularExpression header(QStringLiteral("^\\[([DIWCU])\\] \\d+ "));
  QString line = raw;
  line.remove(ansi);
  if (const auto m = header.match(line); m.hasMatch()) {
    switch (m.captured(1).at(0).toLatin1()) {
      case 'D': m_lineLevel = QtDebugMsg; break;
      case 'W': m_lineLevel = QtWarningMsg; break;
      case 'C': m_lineLevel = QtCriticalMsg; break;
      default: m_lineLevel = QtInfoMsg; break;
    }
    return;
  }
  line = line.trimmed();
  if (line.isEmpty())
    return;
  switch (m_lineLevel) {
    case QtDebugMsg: qDebug().noquote() << "[local server]" << line; break;
    case QtWarningMsg: qWarning().noquote() << "[local server]" << line; break;
    case QtCriticalMsg: qCritical().noquote() << "[local server]" << line; break;
    default: qInfo().noquote() << "[local server]" << line; break;
  }
}

void LocalServer::onFinished(QProcess *proc, const int exitCode, const QProcess::ExitStatus status) {
  proc->deleteLater();
  if (proc != m_proc)
    return;  // stopped on purpose
  m_proc = nullptr;
  m_readyWatch.stop();
  const QString how = status == QProcess::CrashExit ? tr("crashed") : tr("exited with code %1").arg(exitCode);
  qWarning().noquote() << "[local server]" << how;

  // Restart with growing delay; give up after a few exits a minute.
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  m_exits.erase(std::remove_if(m_exits.begin(), m_exits.end(), [now](qint64 t) { return now - t > 60000; }),
                m_exits.end());
  m_exits.append(now);
  if (m_exits.size() > kMaxExitsPerMinute) {
    fail(tr("The local server %1 (%2 times in a minute)").arg(how).arg(m_exits.size()));
    return;
  }
  m_lastError = tr("The local server %1, restarting").arg(how);
  m_restart.start(1000 * int(m_exits.size()));  // before the signal: start() then waits for it
  setState(State::Starting);
}

#if defined(Q_OS_WIN)
// Children in a KILL_ON_JOB_CLOSE job die when the editor exits, crashes or is killed.
void LocalServer::attachToJob(QProcess *proc) {
  if (!m_job) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
      info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        job = nullptr;
      }
    }
    m_job = job;
  }
  if (!m_job)
    return;
  HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(proc->processId()));
  if (!h)
    return;
  if (!AssignProcessToJobObject(static_cast<HANDLE>(m_job), h))
    qWarning() << "[local server] could not tie the server to the editor's lifetime (job object)";
  CloseHandle(h);
}
#endif

} // namespace net
