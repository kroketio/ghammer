#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QVector>
#include <QWidget>

class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;

namespace ui {

// Process-wide log collector: the Qt message handler, installed early in main(); keeps the last N messages and
// forwards everything to the previous handler.
class LogSink final : public QObject {
  Q_OBJECT

public:
  struct Entry {
    QDateTime time;
    QtMsgType type = QtDebugMsg;
    QString category;
    QString text;
  };

  static LogSink &instance();
  static void install();

  [[nodiscard]] QVector<Entry> snapshot() const;
  void clear();
  [[nodiscard]] int warningCount() const { return m_warnings; }
  [[nodiscard]] int errorCount() const { return m_errors; }

signals:
  // Queued: emitted from any thread.
  void logged(const ui::LogSink::Entry &entry);
  void cleared();

private:
  LogSink();
  static void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg);
  void append(Entry e);

  mutable QMutex m_mutex;
  QVector<Entry> m_entries;
  int m_warnings = 0;
  int m_errors = 0;
  static constexpr int kMaxEntries = 8000;
};

// Bottom-dock console: filterable live log.
class ConsolePanel final : public QWidget {
  Q_OBJECT

public:
  explicit ConsolePanel(QWidget *parent = nullptr);
  // Level filter to warnings and errors, scrolled to the newest.
  void showProblems();

signals:
  void countsChanged(int warnings, int errors);

protected:
  bool eventFilter(QObject *obj, QEvent *ev) override;

private:
  void rebuild();
  void appendEntry(const LogSink::Entry &e);
  [[nodiscard]] bool passes(const LogSink::Entry &e) const;

  QPlainTextEdit *m_view = nullptr;
  QLineEdit *m_filter = nullptr;
  QComboBox *m_level = nullptr;
  QCheckBox *m_follow = nullptr;
  QLabel *m_counts = nullptr;
  QLineEdit *m_command = nullptr;
  QStringList m_history;
  int m_historyPos = 0;
};

} // namespace ui

Q_DECLARE_METATYPE(ui::LogSink::Entry)
