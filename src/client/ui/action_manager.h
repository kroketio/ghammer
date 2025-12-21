#pragma once

#include <QAction>
#include <QKeyEvent>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <functional>

namespace ui {

// One registry for every editor command: menus, toolbars, QML menus, the palette and the keyboard prefs share
// these QActions. Each shortcut is the preference "Keys/<id>", applied live.
class ActionManager final : public QObject {
  Q_OBJECT

public:
  static ActionManager &instance();

  struct Spec {
    QString id;          // "edit.undo" - also the preference key suffix
    QString text;        // "&Undo"
    QString icon;        // blender icon name ("loop_back") or a full resource path
    QString shortcut;    // portable text, e.g. "Ctrl+Z"; empty = none
    QString tooltip;
    QString category;    // shown in the palette / keyboard page ("Edit")
    bool checkable = false;
    // Matched only over the viewports by the viewport key filter, never as a window shortcut
    // (arrows, Tab, ... mean something else in lists and text fields).
    bool viewportOnly = false;
  };

  // Creates the action once. `enabled` / `checked` are polled by refresh().
  QAction *add(const Spec &spec, std::function<void()> trigger,
               std::function<bool()> enabled = {}, std::function<bool()> checked = {});
  [[nodiscard]] QAction *action(const QString &id) const;
  [[nodiscard]] QList<QAction *> actions() const;
  [[nodiscard]] QString categoryOf(const QString &id) const;

  // Re-evaluates enabled / checked state for every action. Cheap.
  void refresh();
  // An extra condition over every action: false disables it whatever its own state.
  void setGate(std::function<bool(const QString &id, const QString &category)> gate);

  // So shortcuts work while `w` has focus (floating panels need this).
  void attachTo(QWidget *w);

  // An enabled viewport-only action matching the key, or null. Refreshes enabled states first on a match,
  // so a stale state cannot swallow the key.
  [[nodiscard]] QAction *matchViewportKey(const QKeyEvent *event);

  static QIcon icon(const QString &name);
  // Blender set, then Godot set, then a generic glyph; empty for no name. Actions keep it as "iconPath" (QML).
  static QString iconPath(const QString &name);

private:
  ActionManager();

  struct Entry {
    QPointer<QAction> action;
    QString category;
    std::function<bool()> enabled;
    std::function<bool()> checked;
    bool viewportOnly = false;
  };
  QHash<QString, Entry> m_entries;
  QVector<QString> m_order;
  std::function<bool(const QString &, const QString &)> m_gate;
};

inline ActionManager &actions() { return ActionManager::instance(); }
inline QAction *act(const QString &id) { return ActionManager::instance().action(id); }

} // namespace ui
