#include "action_manager.h"

#include <algorithm>

#include <QFile>
#include <QKeyEvent>
#include <QWidget>

#include "client/prefs/preferences.h"

namespace ui {

ActionManager &ActionManager::instance() {
  static ActionManager *m = new ActionManager();
  return *m;
}

ActionManager::ActionManager() : QObject(nullptr) {}

QIcon ActionManager::icon(const QString &name) {
  if (name.isEmpty())
    return {};
  if (name.startsWith(QLatin1Char(':')))
    return QIcon(name);
  static QHash<QString, QIcon> cache;
  auto it = cache.find(name);
  if (it != cache.end())
    return *it;
  const QIcon ic(iconPath(name));
  cache.insert(name, ic);
  return ic;
}

QString ActionManager::iconPath(const QString &name) {
  if (name.isEmpty() || name.startsWith(QLatin1Char(':')))
    return name;
  QString path = QStringLiteral(":/icons_blender/%1.svg").arg(name);
  if (!QFile::exists(path))
    path = QStringLiteral(":/icons_godot/%1.svg").arg(name);
  // A missing icon makes an icon-only toolbar show the text instead.
  if (!QFile::exists(path)) {
    qWarning().noquote() << QStringLiteral("[icons] no icon named '%1'").arg(name);
    path = QStringLiteral(":/icons_blender/dot.svg");
  }
  return path;
}

QAction *ActionManager::add(const Spec &spec, std::function<void()> trigger, std::function<bool()> enabled,
                            std::function<bool()> checked) {
  if (auto existing = m_entries.constFind(spec.id); existing != m_entries.constEnd() && existing->action)
    return existing->action;

  auto *a = new QAction(spec.text, this);
  a->setObjectName(spec.id);
  a->setIcon(icon(spec.icon));
  a->setProperty("iconPath", iconPath(spec.icon));
  a->setCheckable(spec.checkable);
  a->setIconVisibleInMenu(true);
  QString tip = spec.tooltip.isEmpty() ? QString(spec.text).remove(QLatin1Char('&')) : spec.tooltip;
  a->setToolTip(tip);
  a->setStatusTip(tip);
  // Window-wide shortcuts (text fields still win via ShortcutOverride). Viewport-only actions keep theirs for
  // display only; the viewport key filter matches them.
  a->setShortcutContext(spec.viewportOnly ? Qt::WidgetShortcut : Qt::WindowShortcut);
  if (trigger)
    connect(a, &QAction::triggered, this, [trigger] { trigger(); });

  prefs::Def def;
  def.key = QStringLiteral("Keys/") + spec.id;
  def.category = QStringLiteral("Keyboard/") + (spec.category.isEmpty() ? QStringLiteral("General") : spec.category);
  def.label = QString(spec.text).remove(QLatin1Char('&'));
  def.tooltip = tip;
  def.type = prefs::Type::Key;
  def.defaultValue = spec.shortcut;
  prefs::get().declare(def);
  QPointer<QAction> guard(a);
  const QString tipBase = tip;
  prefs::get().watch(def.key, a, [guard, tipBase](const QVariant &v) {
    if (!guard)
      return;
    const QKeySequence seq = QKeySequence::fromString(v.toString(), QKeySequence::PortableText);
    guard->setShortcut(seq);
    guard->setToolTip(seq.isEmpty() ? tipBase
                                    : QStringLiteral("%1  <span style='color:#8a9ab8'>(%2)</span>")
                                          .arg(tipBase, seq.toString(QKeySequence::NativeText)));
  });

  m_entries.insert(spec.id, Entry{a, spec.category, std::move(enabled), std::move(checked), spec.viewportOnly});
  m_order.push_back(spec.id);
  return a;
}

QAction *ActionManager::action(const QString &id) const {
  const auto it = m_entries.constFind(id);
  return it == m_entries.constEnd() ? nullptr : it->action.data();
}

QList<QAction *> ActionManager::actions() const {
  QList<QAction *> out;
  for (const QString &id: m_order)
    if (QAction *a = action(id))
      out.push_back(a);
  return out;
}

QString ActionManager::categoryOf(const QString &id) const {
  return m_entries.value(id).category;
}

void ActionManager::setGate(std::function<bool(const QString &, const QString &)> gate) {
  m_gate = std::move(gate);
  refresh();
}

void ActionManager::refresh() {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    Entry &e = it.value();
    if (!e.action)
      continue;
    const bool open = !m_gate || m_gate(it.key(), e.category);
    if (e.enabled || m_gate)
      e.action->setEnabled(open && (!e.enabled || e.enabled()));
    if (e.checked && e.action->isCheckable()) {
      const bool c = e.checked();
      if (e.action->isChecked() != c) {
        QSignalBlocker b(e.action);
        e.action->setChecked(c);
      }
    }
  }
}

QAction *ActionManager::matchViewportKey(const QKeyEvent *event) {
  // Shift+Tab arrives as Backtab; shortcuts are written Shift+Tab, as Qt's shortcut map reads them.
  const Qt::Key key = event->key() == Qt::Key_Backtab ? Qt::Key_Tab : Qt::Key(event->key());
  const Qt::KeyboardModifiers mods = (event->modifiers() & ~Qt::KeypadModifier)
                                     | (event->key() == Qt::Key_Backtab ? Qt::ShiftModifier : Qt::NoModifier);
  const QKeySequence pressed(QKeyCombination(mods, key));
  auto matches = [&](const Entry &e) { return e.viewportOnly && e.action && e.action->shortcut() == pressed; };
  if (std::none_of(m_entries.cbegin(), m_entries.cend(), matches))
    return nullptr;
  refresh();
  for (const auto &e: m_entries)
    if (matches(e) && e.action->isEnabled())
      return e.action;
  return nullptr;
}

void ActionManager::attachTo(QWidget *w) {
  for (QAction *a: actions())
    if (!w->actions().contains(a))
      w->addAction(a);
}

} // namespace ui
