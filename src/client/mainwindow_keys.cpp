// Keyboard routing: viewport keys and contextual editor keys that are not actions (flying, Enter/Escape in tools).
#include <QAbstractSpinBox>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTextEdit>

#include "client/ctx.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/pie_menu.h"
#include "client/vulkan/vk_engine.h"
#include "mainwindow.h"

namespace {

bool isTextEntryWidget(const QWidget *w) {
  return w != nullptr
      && (qobject_cast<const QLineEdit *>(w) != nullptr
          || qobject_cast<const QTextEdit *>(w) != nullptr
          || qobject_cast<const QPlainTextEdit *>(w) != nullptr
          || qobject_cast<const QAbstractSpinBox *>(w) != nullptr);
}

bool isTextEntryQuickItem(const QQuickWidget *w) {
  if (!w || !w->rootObject())
    return false;
  const QQuickWindow *win = w->rootObject()->window();
  const QQuickItem *focus = win ? win->activeFocusItem() : nullptr;
  return focus != nullptr
      && (focus->inherits("QQuickTextInput") || focus->inherits("QQuickTextEdit"));
}

// The selectable text widget a mouse press landed on: a label with selectable text, or a read-only text view (the
// press arrives at its viewport).
QWidget *selectableTextWidget(QObject *watched) {
  auto *w = qobject_cast<QWidget *>(watched);
  if (!w)
    return nullptr;
  if (auto *l = qobject_cast<QLabel *>(w))
    return (l->textInteractionFlags() & Qt::TextSelectableByMouse) ? l : nullptr;
  auto *area = qobject_cast<QAbstractScrollArea *>(w->parentWidget());
  if (!area || area->viewport() != w)
    return nullptr;
  if (auto *t = qobject_cast<QTextEdit *>(area); t && t->isReadOnly())
    return t;
  if (auto *t = qobject_cast<QPlainTextEdit *>(area); t && t->isReadOnly())
    return t;
  return nullptr;
}

bool hasSelectedText(const QWidget *w) {
  if (const auto *l = qobject_cast<const QLabel *>(w))
    return l->hasSelectedText();
  if (const auto *t = qobject_cast<const QTextEdit *>(w))
    return t->textCursor().hasSelection();
  if (const auto *t = qobject_cast<const QPlainTextEdit *>(w))
    return t->textCursor().hasSelection();
  return false;
}

// Qt reports one ShiftModifier for both shifts; the side is only in the scan code. xcb uses its own keycodes;
// Wayland's evdev passthrough and Windows use 42 / 54.
enum class ShiftSide { Neither, Left, Right };

ShiftSide shiftSideOf(const quint32 scanCode) {
  static const bool xcb = QGuiApplication::platformName() == QLatin1String("xcb");
  if (scanCode == 0)
    return ShiftSide::Left;
  if (xcb)
    return scanCode == 50u ? ShiftSide::Left : (scanCode == 62u ? ShiftSide::Right : ShiftSide::Neither);
  return scanCode == 42u ? ShiftSide::Left : (scanCode == 54u ? ShiftSide::Right : ShiftSide::Neither);
}

} // namespace

// Ctrl+C copies the text selected in a label or read-only text view when that was the last thing clicked; any other
// press, or a change of the editor's selection, gives Ctrl+C back to the editor (brushes, entities).
bool MainWindow::isTextCopy(const QKeyEvent *ke) const {
  return ke->matches(QKeySequence::Copy) && m_textCopyOwner && m_textCopyOwner->isVisible()
      && hasSelectedText(m_textCopyOwner);
}

void MainWindow::copySelectedText() {
  if (auto *l = qobject_cast<QLabel *>(m_textCopyOwner.data()))
    QApplication::clipboard()->setText(l->selectedText());
  else if (auto *t = qobject_cast<QTextEdit *>(m_textCopyOwner.data()))
    t->copy();
  else if (auto *t = qobject_cast<QPlainTextEdit *>(m_textCopyOwner.data()))
    t->copy();
}

// Text inputs (line edits, spinboxes, QML TextFields) and other top-level windows own the keyboard instead.
bool MainWindow::keyboardBelongsToViewport() const {
  if (!quickContent_)
    return false;

  const QWidget *focus = QApplication::focusWidget();
  if (focus && focus->window() != this)
    return false;
  if (isTextEntryWidget(focus) || isTextEntryQuickItem(qobject_cast<const QQuickWidget *>(focus)))
    return false;

  return quickContent_->rect().contains(quickContent_->mapFromGlobal(QCursor::pos()));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
    auto *ke = static_cast<QKeyEvent *>(event);
    if (ke->key() == Qt::Key_Shift) {
      const ShiftSide side = shiftSideOf(ke->nativeScanCode());
      const bool down = event->type() == QEvent::KeyPress;
      if (side == ShiftSide::Left)
        m_leftShiftHeld = down;
    }
  } else if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::ApplicationDeactivate) {
    m_leftShiftHeld = false;
    if (g::engine3D)
      g::engine3D->releaseAllKeys();
    if (m_pie)
      m_pie->close();
  }

  // The pie menu and Quick Material strip take every key while open.
  if (m_pie && m_pie->capturesKeys() && m_pie->handleKey(event))
    return true;

  if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
    if (QWidget *text = selectableTextWidget(watched))
      m_textCopyOwner = text;
    else if (watched->isWidgetType())
      m_textCopyOwner = nullptr;
  }
  if (event->type() == QEvent::ShortcutOverride && isTextCopy(static_cast<QKeyEvent *>(event))) {
    copySelectedText();
    event->accept();
    return true;
  }
  if (event->type() == QEvent::KeyPress && isTextCopy(static_cast<QKeyEvent *>(event))) {
    copySelectedText();
    return true;
  }

  // Undo / redo in a focused main-window field goes to the editor's undo; dropping focus first commits what was
  // typed. Multi-line text and other windows keep their own undo.
  if (event->type() == QEvent::ShortcutOverride) {
    auto *ke = static_cast<QKeyEvent *>(event);
    const QKeySequence pressed(ke->keyCombination());
    const QAction *undo = ui::act(QStringLiteral("edit.undo"));
    const QAction *redo = ui::act(QStringLiteral("edit.redo"));
    QWidget *focus = QApplication::focusWidget();
    if (focus && focus->window() == this
        && (qobject_cast<QLineEdit *>(focus) != nullptr || qobject_cast<QAbstractSpinBox *>(focus) != nullptr)
        && ((undo && !undo->shortcut().isEmpty() && undo->shortcut() == pressed)
            || (redo && !redo->shortcut().isEmpty() && redo->shortcut() == pressed))) {
      focus->clearFocus();
      event->ignore(); // not claimed: the window shortcut runs
      return true;
    }
  }

  // While flying, WASDQE are movement, not tool shortcuts: claim the override so they arrive as key presses.
  if (event->type() == QEvent::ShortcutOverride && keyboardBelongsToViewport()) {
    auto *ke = static_cast<QKeyEvent *>(event);
    if (keys_wsad_.contains(ke->key()) && flyKeysActive()
        && !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
      event->accept();
      return true;
    }
  }

  const bool keyEvent = event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease;
  if (keyEvent && watched != this && keyboardBelongsToViewport()) {
    auto *ke = static_cast<QKeyEvent *>(event);
    if (ke->type() == QEvent::KeyPress) {
      if (QAction *a = ui::ActionManager::instance().matchViewportKey(ke)) {
        a->trigger();
        return true;
      }
      if (handleEditorKey(ke))
        return true;
    } else if (keys_wsad_.contains(ke->key())) {
      g::engine3D->handleKeyRelease(ke);
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

// Input that is not a command: held fly keys and context-dependent Enter / Escape (and C) / Shift+X.
bool MainWindow::flyKeysActive() const {
  // Fly keys need the right mouse button held unless the pref says otherwise.
  return !prefs::b("Viewport/FlyRequiresRightMouse")
         || QApplication::mouseButtons().testFlag(Qt::RightButton);
}

bool MainWindow::handleEditorKey(QKeyEvent *event) {
  const int key = event->key();
  if (g::engine3D == nullptr)
    return false;

  if (keys_wsad_.contains(key) && flyKeysActive()
      && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
    g::engine3D->handleKeyPress(event);
    return true;
  }

  // Left Shift+X enters cut mode, then cycles Positive -> Negative -> KeepBoth; only Escape leaves. The side comes
  // from the scan-code tracking.
  if (key == Qt::Key_X && m_leftShiftHeld && !(event->modifiers() & Qt::ControlModifier)) {
    if (g::engine3D->cutMode() == vk::Engine3D::CutMode::Off) {
      if (ctx_->editorMode != g::EditorMode::OBJECT) {
        qInfo() << "[cut] refused: cutting is object-mode only";
        return true;
      }
      g::engine3D->enterCutMode();
    } else {
      g::engine3D->cycleCutMode();
    }
    return true;
  }

  if (key == Qt::Key_Backspace && g::engine3D->penActive() && g::engine3D->penBackspace())
    return true;

  if (key == Qt::Key_B && g::engine3D->bevelMode()
      && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
    g::engine3D->toggleBevelInverse();
    return true;
  }

  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    if (g::engine3D->bevelMode()) {
      g::engine3D->applyBevel();
      return true;
    }
    if (g::engine3D->penActive()) {
      g::engine3D->penFinish();
      return true;
    }
    if (g::engine3D->vertexScaleActive()) {
      g::engine3D->commitVertexScale();
      return true;
    }
    if (g::engine3D->shapeToolActive()) {
      g::engine3D->commitShapeTool();
      return true;
    }
    if (g::engine3D->cutMode() != vk::Engine3D::CutMode::Off) {
      g::engine3D->applyCut();
      return true;
    }
    if (g::engine3D->hasMarquee()) {
      g::engine3D->commitMarquee();
      return true;
    }
    return false;
  }

  // C is a second Escape.
  const bool plainC = key == Qt::Key_C && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
  if (key == Qt::Key_Escape || plainC) {
    if (g::engine3D->bevelMode()) {
      g::engine3D->leaveBevelMode();
      return true;
    }
    // The pen: the unfinished path first, then the tool itself.
    if (g::engine3D->penActive()) {
      g::engine3D->penCancel();
      return true;
    }
    if (g::engine3D->targetPickActive()) {
      g::engine3D->cancelTargetPick();
      return true;
    }
    if (g::engine3D->vertexScaleActive()) {
      g::engine3D->cancelVertexScale();
      return true;
    }
    // Edit mode leaves to the previous mode (the brushes stay selected).
    if (g::engine3D->editMode()) {
      ctx_->switchMode(ctx_->modeBeforeEdit == g::EditorMode::EDIT ? g::EditorMode::OBJECT : ctx_->modeBeforeEdit);
      return true;
    }
    if (g::engine3D->shapeToolActive()) {
      g::engine3D->cancelShapeTool();
      return true;
    }
    // Cut mode: abort the cut, keep the selection.
    if (g::engine3D->cutMode() != vk::Engine3D::CutMode::Off) {
      g::engine3D->exitCutMode();
      return true;
    }
    if (g::engine3D->mirrorPlacing()) {
      g::engine3D->setMirrorActive(false);
      return true;
    }
    if (g::engine3D->gizmoMode() == vk::Engine3D::GizmoMode::Rotate && !g::engine3D->materialMode()) {
      g::engine3D->setGizmoMode(vk::Engine3D::GizmoMode::Translate);
      return true;
    }
    // Material mode: the first Escape only drops the auto-select box.
    if (g::engine3D->materialMode() && g::engine3D->hasMarquee()) {
      g::engine3D->clearMarquee();
      return true;
    }
    if (g::engine3D->editSourceHost() && g::engine3D->selectedObjects().empty()
        && g::engine3D->selectedFaces().empty()) {
      g::engine3D->setEditSourceHost(0);
      return true;
    }
    g::engine3D->clearMarquee();
    g::engine3D->clearSelection();
    return true;
  }

  return false;
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
  if (handleEditorKey(event))
    return;

  QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event) {
  const int key = event->key();
  if (keys_wsad_.contains(key)) {
    g::engine3D->handleKeyRelease(event);
    return;
  }

  QMainWindow::keyReleaseEvent(event);
}

