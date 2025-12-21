#include "command_palette.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include "client/ui/action_manager.h"

namespace ui {

namespace {

// Subsequence match with a bonus for word starts and contiguous runs; -1 when not a subsequence.
int fuzzyScore(const QString &hay, const QString &needle) {
  if (needle.isEmpty())
    return 0;
  int score = 0, hi = 0, run = 0;
  for (const QChar c: needle) {
    bool found = false;
    while (hi < hay.size()) {
      const QChar h = hay[hi];
      const bool wordStart = hi == 0 || !hay[hi - 1].isLetterOrNumber();
      ++hi;
      if (h.toLower() == c.toLower()) {
        run = run + 1;
        score += 1 + run * 2 + (wordStart ? 6 : 0);
        found = true;
        break;
      }
      run = 0;
    }
    if (!found)
      return -1;
  }
  return score - int(hay.size()) / 8;
}

} // namespace

void CommandPalette::open(QWidget *parent) {
  auto *p = new CommandPalette(parent);
  p->setAttribute(Qt::WA_DeleteOnClose);
  const QRect pr = parent->geometry();
  p->resize(std::min(720, pr.width() - 80), 420);
  p->move(pr.center().x() - p->width() / 2, pr.top() + 90);
  p->show();
  p->m_edit->setFocus();
}

CommandPalette::CommandPalette(QWidget *parent) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {
  setObjectName(QStringLiteral("commandPalette"));
  setStyleSheet(QStringLiteral("#commandPalette { border: 1px solid #22a0ff; background: #141a30; }"));
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(6, 6, 6, 6);
  m_edit = new QLineEdit(this);
  m_edit->setPlaceholderText(tr("Type a command…"));
  m_list = new QListWidget(this);
  m_list->setUniformItemSizes(true);
  v->addWidget(m_edit);
  v->addWidget(m_list, 1);
  m_edit->installEventFilter(this);
  connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::filter);
  connect(m_list, &QListWidget::itemActivated, this, [this] { runCurrent(); });
  filter({});
}

void CommandPalette::filter(const QString &text) {
  m_list->clear();
  QVector<QPair<int, QAction *>> hits;
  for (QAction *a: ActionManager::instance().actions()) {
    if (!a->isEnabled() || a->text().isEmpty())
      continue;
    const QString cat = ActionManager::instance().categoryOf(a->objectName());
    const QString label = (cat.isEmpty() ? QString() : cat + QStringLiteral(": ")) + QString(a->text()).remove(QLatin1Char('&'));
    const int s = fuzzyScore(label, text);
    if (s >= 0)
      hits.push_back({s, a});
  }
  std::stable_sort(hits.begin(), hits.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
  for (const auto &[score, a]: hits) {
    const QString cat = ActionManager::instance().categoryOf(a->objectName());
    QString label = (cat.isEmpty() ? QString() : cat + QStringLiteral(": ")) + QString(a->text()).remove(QLatin1Char('&'));
    if (!a->shortcut().isEmpty())
      label += QStringLiteral("    (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
    auto *item = new QListWidgetItem(a->icon(), label, m_list);
    item->setData(Qt::UserRole, QVariant::fromValue(static_cast<QObject *>(a)));
  }
  m_list->setCurrentRow(0);
}

void CommandPalette::runCurrent() {
  QListWidgetItem *item = m_list->currentItem();
  if (!item)
    return;
  auto *a = qobject_cast<QAction *>(item->data(Qt::UserRole).value<QObject *>());
  close();
  if (a)
    a->trigger();
}

bool CommandPalette::eventFilter(QObject *o, QEvent *e) {
  if (o == m_edit && e->type() == QEvent::KeyPress) {
    auto *ke = static_cast<QKeyEvent *>(e);
    if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up) {
      const int d = ke->key() == Qt::Key_Down ? 1 : -1;
      m_list->setCurrentRow(std::clamp(m_list->currentRow() + d, 0, std::max(0, m_list->count() - 1)));
      return true;
    }
    if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
      runCurrent();
      return true;
    }
    if (ke->key() == Qt::Key_Escape) {
      close();
      return true;
    }
  }
  return QDialog::eventFilter(o, e);
}

} // namespace ui
