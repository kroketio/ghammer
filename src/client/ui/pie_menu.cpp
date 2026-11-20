#include "pie_menu.h"

#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickWidget>

#include "client/lib/globals.h"
#include "client/ui/action_manager.h"
#include "client/ui/quick_material_model.h"
#include "client/ui/viewport_notices.h"

namespace ui {

namespace {

struct Slot {
  int number;     // numpad position: 7 8 9 / 4 6 / 1 2 3
  const char *id; // ActionManager action
  const char *label;
  bool hideWhenDisabled; // else shown greyed
};

const Slot kObjectSlots[] = {
    {7, "autosel.ceiling", QT_TRANSLATE_NOOP("ui::PieMenu", "Ceilings"), false},
    {8, "autosel.sameSize", QT_TRANSLATE_NOOP("ui::PieMenu", "Same Size"), false},
    {9, "autosel.sameMirror", QT_TRANSLATE_NOOP("ui::PieMenu", "Same Mirror"), false},
    {4, "autosel.touching", QT_TRANSLATE_NOOP("ui::PieMenu", "Touching"), false},
    {6, "autosel.sameMaterial", QT_TRANSLATE_NOOP("ui::PieMenu", "Same Material"), false},
    {3, "tex.quickMaterial", QT_TRANSLATE_NOOP("ui::PieMenu", "Quick Material"), true},
    {2, "select.toMaterialMode", QT_TRANSLATE_NOOP("ui::PieMenu", "Select in Material Mode"), true},
};
const Slot kMaterialSlots[] = {
    {7, "autosel.ceiling", QT_TRANSLATE_NOOP("ui::PieMenu", "Ceilings"), false},
    {8, "autosel.group", QT_TRANSLATE_NOOP("ui::PieMenu", "Group"), false},
    {9, "autosel.sameMirror", QT_TRANSLATE_NOOP("ui::PieMenu", "Same Mirror"), false},
    {4, "autosel.sameBrush", QT_TRANSLATE_NOOP("ui::PieMenu", "Same Brush"), false},
    {6, "autosel.sameMaterial", QT_TRANSLATE_NOOP("ui::PieMenu", "Same Material"), false},
    {3, "tex.quickMaterial", QT_TRANSLATE_NOOP("ui::PieMenu", "Quick Material"), true},
    {2, "select.toObjectMode", QT_TRANSLATE_NOOP("ui::PieMenu", "Select in Object Mode"), true},
};
const Slot kEditSlots[] = {
    {7, "vertex.selectAll", QT_TRANSLATE_NOOP("ui::PieMenu", "Select All Vertices"), false},
    {8, "vertex.snap", QT_TRANSLATE_NOOP("ui::PieMenu", "Snap Vertices"), false},
    {9, "vertex.weld", QT_TRANSLATE_NOOP("ui::PieMenu", "Weld Vertices"), false},
    {4, "vertex.clear", QT_TRANSLATE_NOOP("ui::PieMenu", "Deselect Vertices"), false},
    {6, "vertex.splitEdge", QT_TRANSLATE_NOOP("ui::PieMenu", "Split Edges"), false},
    {1, "mode.object", QT_TRANSLATE_NOOP("ui::PieMenu", "Object Mode"), false},
    {2, "select.toMaterialMode", QT_TRANSLATE_NOOP("ui::PieMenu", "Select in Material Mode"), true},
};

} // namespace

PieMenu::PieMenu(QQuickWidget *view, QuickMaterialModel *materials, ViewportNotices *notices, QObject *parent)
    : QObject(parent), m_view(view), m_materials(materials), m_notices(notices) {}

bool PieMenu::cursorInView(QPointF &at) const {
  if (!m_view)
    return false;
  at = m_view->mapFromGlobal(QCursor::pos()).toPointF();
  return QRectF(m_view->rect()).contains(at);
}

void PieMenu::popup() {
  QPointF at;
  // Not while flying / dragging, nor over the pie itself.
  if (capturesKeys() || QGuiApplication::mouseButtons() != Qt::NoButton || !cursorInView(at))
    return;
  ActionManager::instance().refresh();
  m_center = at;
  QString title;
  switch (g::editorMode) {
    case g::EditorMode::OBJECT: title = tr("Object"); break;
    case g::EditorMode::EDIT: title = tr("Edit"); break;
    case g::EditorMode::MATERIAL: title = tr("Material"); break;
  }
  emit openRequested(at.x(), at.y(), title);
}

QVariantList PieMenu::items() const {
  const Slot *begin = kObjectSlots, *end = std::end(kObjectSlots);
  if (g::editorMode == g::EditorMode::MATERIAL) {
    begin = kMaterialSlots;
    end = std::end(kMaterialSlots);
  } else if (g::editorMode == g::EditorMode::EDIT) {
    begin = kEditSlots;
    end = std::end(kEditSlots);
  }
  QVariantList out;
  for (const Slot *s = begin; s != end; ++s) {
    const QAction *a = act(QString::fromLatin1(s->id));
    if (!a || (s->hideWhenDisabled && !a->isEnabled()))
      continue;
    const QString icon = a->property("iconPath").toString();
    out.append(QVariantMap {{QStringLiteral("number"), s->number},
                            {QStringLiteral("id"), QString::fromLatin1(s->id)},
                            {QStringLiteral("label"), tr(s->label)},
                            {QStringLiteral("icon"), icon.isEmpty() ? QString() : QStringLiteral("qrc") + icon},
                            {QStringLiteral("enabled"), a->isEnabled()}});
  }
  return out;
}

void PieMenu::trigger(const QString &id) {
  QAction *a = act(id);
  if (!a)
    return;
  ActionManager::instance().refresh();
  if (!a->isEnabled())
    return;
  m_fromPie = true;
  a->trigger();
  m_fromPie = false;
}

void PieMenu::openQuickMaterial() {
  if (!m_materials || !m_view || m_stripOpen)
    return;
  QPointF at = m_center;
  if (!m_fromPie && !cursorInView(at))
    at = QPointF(m_view->width() * 0.5, m_view->height() * 0.5);
  m_materials->refresh();
  if (m_materials->rowCount() == 0) {
    if (m_notices)
      m_notices->flash(tr("No other materials nearby"));
    return;
  }
  emit quickMaterialRequested(at.x(), at.y());
}

void PieMenu::setPieOpen(const bool open) {
  if (m_pieOpen == open)
    return;
  m_pieOpen = open;
  emit pieOpenChanged();
}

void PieMenu::setStripOpen(const bool open) {
  if (m_stripOpen == open)
    return;
  m_stripOpen = open;
  emit stripOpenChanged();
}

bool PieMenu::handleKey(QEvent *event) {
  if (!capturesKeys())
    return false;
  if (event->type() == QEvent::ShortcutOverride) {
    event->accept(); // no window shortcuts while open
    return true;
  }
  if (event->type() != QEvent::KeyPress)
    return false; // releases pass, so held fly keys are released
  const auto *ke = static_cast<QKeyEvent *>(event);
  if (ke->isAutoRepeat())
    return true;
  if (ke->key() == Qt::Key_Escape) {
    emit closeRequested();
    return true;
  }
  if (m_pieOpen) {
    const QAction *pie = act(QStringLiteral("view.pieMenu"));
    const QKeySequence pressed(ke->keyCombination());
    if (pie && !pie->shortcut().isEmpty() && pressed.matches(pie->shortcut()) == QKeySequence::ExactMatch) {
      emit closeRequested();
      return true;
    }
    if (ke->key() >= Qt::Key_1 && ke->key() <= Qt::Key_9) {
      emit numberPressed(ke->key() - Qt::Key_0);
      return true;
    }
  }
  return true;
}

} // namespace ui
