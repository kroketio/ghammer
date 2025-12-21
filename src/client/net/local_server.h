#pragma once

// The asset server the editor runs itself (Server/Mode = local): ghammer_server from this executable's directory,
// on 127.0.0.1, a free port and per-run credentials. It exits with the editor (--parent-pid, plus
// PR_SET_PDEATHSIG on Linux, a kill-on-close job object on Windows).

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVector>

namespace net {

class LocalServer final : public QObject {
  Q_OBJECT

public:
  enum class State {
    Stopped,  // not running (remote mode, or not started yet)
    Starting, // launched, waiting for its ready line
    Running,  // listening on url()
    Failed,   // missing binary, failed to start, or kept exiting: lastError()
  };
  Q_ENUM(State)

  static LocalServer &instance();

  [[nodiscard]] static QString binaryPath();

  // Scripted runs (EDITOR_* hooks) never start a server of their own.
  void setAllowed(bool allowed) { m_allowed = allowed; }
  [[nodiscard]] bool allowed() const { return m_allowed; }

  // No-op while starting or running, or after a failure until restart() / stop().
  void start();
  void stop();
  void restart();

  [[nodiscard]] State state() const { return m_state; }
  [[nodiscard]] bool isActive() const { return m_state == State::Starting || m_state == State::Running; }
  [[nodiscard]] quint16 port() const { return m_port; }
  // Empty until a port has been picked.
  [[nodiscard]] QUrl url() const;
  // Per editor run; the same across restarts.
  [[nodiscard]] QString user() const { return m_user; }
  [[nodiscard]] QString password() const { return m_password; }
  [[nodiscard]] QString lastError() const { return m_lastError; }

signals:
  void stateChanged(net::LocalServer::State state);
  void ready();  // GHAMMER_SERVER_READY line seen

private:
  LocalServer();
  void setState(State s);
  void fail(const QString &error);
  void launch();
  void onOutput(QProcess *proc);
  void onFinished(QProcess *proc, int exitCode, QProcess::ExitStatus status);
  void logLine(const QString &line);
#if defined(Q_OS_WIN)
  void attachToJob(QProcess *proc);
  void *m_job = nullptr;  // HANDLE, open for the editor's lifetime
#endif

  QProcess *m_proc = nullptr;
  QByteArray m_buffer;
  QtMsgType m_lineLevel = QtInfoMsg;  // level of the server log entry being read
  State m_state = State::Stopped;
  bool m_allowed = true;
  bool m_quitting = false;
  quint16 m_port = 0;
  QString m_user;
  QString m_password;
  QString m_lastError;
  QVector<qint64> m_exits;  // unexpected exits in the last minute (ms since epoch)
  QTimer m_restart;
  QTimer m_readyWatch;
};

} // namespace net
