#include "console_panel.h"
#include "client/ui/scale.h"

#include <QCompleter>
#include <QKeyEvent>

#include "client/ui/console_commands.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/widgets.h"

namespace ui {

namespace {
QtMessageHandler g_previous = nullptr;

int severity(QtMsgType t) {
  switch (t) {
    case QtDebugMsg: return 0;
    case QtInfoMsg: return 1;
    case QtWarningMsg: return 2;
    case QtCriticalMsg:
    case QtFatalMsg: return 3;
  }
  return 0;
}

const char *typeColor(QtMsgType t) {
  switch (t) {
    case QtDebugMsg: return "#7d8aa8";
    case QtInfoMsg: return "#c9d3ea";
    case QtWarningMsg: return "#f0b45a";
    case QtCriticalMsg:
    case QtFatalMsg: return "#ff6b6b";
  }
  return "#ccc";
}
} // namespace

LogSink &LogSink::instance() {
  static LogSink *s = new LogSink();
  return *s;
}

LogSink::LogSink() : QObject(nullptr) {
  qRegisterMetaType<ui::LogSink::Entry>("ui::LogSink::Entry");
}

void LogSink::install() {
  instance();
  g_previous = qInstallMessageHandler(&LogSink::handler);
}

void LogSink::handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
  if (type == QtWarningMsg &&
      msg.startsWith(QLatin1String("QObject::disconnect: wildcard call disconnects from destroyed signal of QTcpSocket")))
    return;
  Entry e;
  e.time = QDateTime::currentDateTime();
  e.type = type;
  e.category = QString::fromLatin1(ctx.category ? ctx.category : "");
  if (e.category == QLatin1String("default"))
    e.category.clear();
  e.text = msg;
  instance().append(std::move(e));
  if (g_previous)
    g_previous(type, ctx, msg);
}

void LogSink::append(Entry e) {
  {
    QMutexLocker lock(&m_mutex);
    if (e.type == QtWarningMsg)
      ++m_warnings;
    else if (e.type == QtCriticalMsg || e.type == QtFatalMsg)
      ++m_errors;
    m_entries.push_back(e);
    if (m_entries.size() > kMaxEntries)
      m_entries.remove(0, m_entries.size() - kMaxEntries);
  }
  QMetaObject::invokeMethod(this, [this, e] { emit logged(e); }, Qt::QueuedConnection);
}

QVector<LogSink::Entry> LogSink::snapshot() const {
  QMutexLocker lock(&m_mutex);
  return m_entries;
}

void LogSink::clear() {
  {
    QMutexLocker lock(&m_mutex);
    m_entries.clear();
    m_warnings = m_errors = 0;
  }
  emit cleared();
}

ConsolePanel::ConsolePanel(QWidget *parent) : QWidget(parent) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);

  auto *bar = new QHBoxLayout();
  m_level = new QComboBox(this);
  m_level->addItems({tr("Debug"), tr("Info"), tr("Warnings"), tr("Errors")});
  m_level->setToolTip(tr("Minimum level shown"));
  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Filter…"));
  m_filter->setClearButtonEnabled(true);
  m_follow = new QCheckBox(tr("Follow"), this);
  m_follow->setChecked(true);
  m_counts = new QLabel(this);
  auto *copy = new QToolButton(this);
  copy->setIcon(ActionManager::icon(QStringLiteral("copydown")));
  copy->setToolTip(tr("Copy visible log"));
  auto *clear = new QToolButton(this);
  clear->setIcon(ActionManager::icon(QStringLiteral("trash")));
  clear->setToolTip(tr("Clear"));
  bar->addWidget(m_level);
  bar->addWidget(m_filter, 1);
  bar->addWidget(m_follow);
  bar->addWidget(m_counts);
  bar->addWidget(copy);
  bar->addWidget(clear);
  v->addLayout(bar);

  m_view = new QPlainTextEdit(this);
  m_view->setReadOnly(true);
  m_view->setMaximumBlockCount(8000);
  m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
  scale::setRole(m_view, scale::Font::Mono);
  v->addWidget(m_view, 1);

  m_command = new QLineEdit(this);
  m_command->setPlaceholderText(tr("Command (help lists them)"));
  scale::setRole(m_command, scale::Font::Mono);
  {
    auto *completer = new QCompleter(ConsoleCommands::instance().names(), m_command);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::InlineCompletion);
    m_command->setCompleter(completer);
  }
  v->addWidget(m_command);
  connect(m_command, &QLineEdit::returnPressed, this, [this] {
    const QString line = m_command->text().trimmed();
    if (line.isEmpty())
      return;
    if (m_history.isEmpty() || m_history.back() != line)
      m_history << line;
    m_historyPos = int(m_history.size());
    m_command->clear();
    ConsoleCommands::instance().run(line);
  });
  m_command->installEventFilter(this);

  prefs::get().watch(QStringLiteral("General/LogLevel"), this, [this](const QVariant &val) {
    QSignalBlocker b(m_level);
    m_level->setCurrentIndex(std::clamp(val.toInt(), 0, 3));
    rebuild();
  });
  connect(m_level, &QComboBox::currentIndexChanged, this,
          [](int idx) { prefs::get().setValue(QStringLiteral("General/LogLevel"), idx); });
  connect(m_filter, &QLineEdit::textChanged, this, &ConsolePanel::rebuild);
  connect(clear, &QToolButton::clicked, this, [] { LogSink::instance().clear(); });
  connect(copy, &QToolButton::clicked, this,
          [this] { QApplication::clipboard()->setText(m_view->toPlainText()); });
  connect(&LogSink::instance(), &LogSink::logged, this, &ConsolePanel::appendEntry);
  connect(&LogSink::instance(), &LogSink::cleared, this, &ConsolePanel::rebuild);
  rebuild();
}

bool ConsolePanel::eventFilter(QObject *obj, QEvent *ev) {
  // Up / Down: command history.
  if (obj == m_command && ev->type() == QEvent::KeyPress) {
    const auto *k = static_cast<QKeyEvent *>(ev);
    if ((k->key() == Qt::Key_Up || k->key() == Qt::Key_Down) && !m_history.isEmpty()) {
      m_historyPos = std::clamp(m_historyPos + (k->key() == Qt::Key_Up ? -1 : 1), 0, int(m_history.size()));
      m_command->setText(m_historyPos < m_history.size() ? m_history[m_historyPos] : QString());
      return true;
    }
  }
  return QWidget::eventFilter(obj, ev);
}

void ConsolePanel::showProblems() {
  m_level->setCurrentIndex(2);  // Warnings (and errors)
  m_view->moveCursor(QTextCursor::End);
  m_view->ensureCursorVisible();
}

bool ConsolePanel::passes(const LogSink::Entry &e) const {
  if (severity(e.type) < m_level->currentIndex())
    return false;
  const QString f = m_filter->text();
  return f.isEmpty() || e.text.contains(f, Qt::CaseInsensitive) || e.category.contains(f, Qt::CaseInsensitive);
}

void ConsolePanel::appendEntry(const LogSink::Entry &e) {
  auto &sink = LogSink::instance();
  m_counts->setText(QStringLiteral("<span style='color:#f0b45a'>⚠ %1</span>  <span style='color:#ff6b6b'>✖ %2</span>")
                        .arg(sink.warningCount())
                        .arg(sink.errorCount()));
  emit countsChanged(sink.warningCount(), sink.errorCount());
  if (!passes(e))
    return;
  const QString line = QStringLiteral("<span style='color:#56607a'>%1</span> "
                                      "<span style='color:%2'>%3%4</span>")
                           .arg(e.time.toString(QStringLiteral("HH:mm:ss.zzz")),
                                QString::fromLatin1(typeColor(e.type)),
                                e.category.isEmpty() ? QString()
                                                     : QStringLiteral("[%1] ").arg(e.category.toHtmlEscaped()),
                                e.text.toHtmlEscaped());
  QScrollBar *sb = m_view->verticalScrollBar();
  const bool atBottom = sb->value() >= sb->maximum() - 2;
  m_view->appendHtml(line);
  if (m_follow->isChecked() && atBottom)
    sb->setValue(sb->maximum());
}

void ConsolePanel::rebuild() {
  m_view->clear();
  const auto entries = LogSink::instance().snapshot();
  // Appending thousands of lines one at a time is slow.
  QStringList lines;
  lines.reserve(entries.size());
  for (const auto &e: entries) {
    if (!passes(e))
      continue;
    lines << QStringLiteral("<span style='color:#56607a'>%1</span> <span style='color:%2'>%3%4</span>")
                 .arg(e.time.toString(QStringLiteral("HH:mm:ss.zzz")), QString::fromLatin1(typeColor(e.type)),
                      e.category.isEmpty() ? QString() : QStringLiteral("[%1] ").arg(e.category.toHtmlEscaped()),
                      e.text.toHtmlEscaped());
  }
  m_view->appendHtml(lines.join(QStringLiteral("<br>")));
  m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
  auto &sink = LogSink::instance();
  m_counts->setText(QStringLiteral("<span style='color:#f0b45a'>⚠ %1</span>  <span style='color:#ff6b6b'>✖ %2</span>")
                        .arg(sink.warningCount())
                        .arg(sink.errorCount()));
}

} // namespace ui
