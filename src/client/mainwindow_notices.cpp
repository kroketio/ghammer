// The 3D view's lower-left notices (ui::ViewportNotices): one source per lasting state, as a key, its text (empty
// while off) and the signals that can change it.

#include "mainwindow.h"

#include <QAction>
#include <QKeySequence>

#include "ctx.h"
#include "ui/action_manager.h"
#include "ui/viewport_notices.h"
#include "vulkan/vk_engine.h"

namespace {

vk::Engine3D *E() { return g::engine3D; }

// " (Ctrl+M)": the action's current shortcut.
QString keyOf(const char *id) {
  const QAction *a = ui::act(QString::fromLatin1(id));
  return a && !a->shortcut().isEmpty() ? QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText))
                                       : QString();
}

} // namespace

void MainWindow::setupViewportNotices() {
  ui::ViewportNotices *n = m_notices;
  auto *e = E();
  if (!n || !e)
    return;

  // A drag in progress ("Rotate Y 45°").
  n->addSource(QStringLiteral("gizmoReadout"), [] { return E()->gizmoReadout(); });

  // Tools and modes.
  n->addSource(QStringLiteral("cut"), [] {
    const auto *e = E();
    if (!e->cutModeActive())
      return QString();
    if (e->isCutout())
      return tr("Cut-out · %1 · Enter · Esc").arg(e->cutShapeLabel());
    QString keep;
    switch (e->cutMode()) {
      case vk::Engine3D::CutMode::Positive: keep = tr("removes +"); break;
      case vk::Engine3D::CutMode::Negative: keep = tr("removes −"); break;
      case vk::Engine3D::CutMode::KeepBoth: keep = tr("keeps both"); break;
      case vk::Engine3D::CutMode::Off: break;
    }
    return tr("Cut · %1 · Enter · Esc").arg(keep);
  });
  n->addSource(QStringLiteral("zen"), [] {
    return E()->zenMode() ? tr("Zen mode") : QString();
  });
  n->addSource(QStringLiteral("shapeTool"), [] {
    const auto *e = E();
    if (!e->shapeToolActive())
      return QString();
    const int kind = e->shapeToolKind();
    const QString name = kind == int(eng::PrimitiveKind::Arch)        ? tr("Arch")
                         : kind == vk::Engine3D::kShapeCylinder       ? tr("Cylinder")
                         : kind == vk::Engine3D::kShapeCapsule        ? tr("Capsule")
                         : kind == vk::Engine3D::kShapePyramid        ? tr("Pyramid")
                                                                      : tr("Stairs");
    return tr("%1 · Enter · Esc").arg(name);
  });
  n->addSource(QStringLiteral("vertexScale"), [] {
    return E()->vertexScaleActive() ? tr("Scale vertices · Enter · Esc") : QString();
  });
  n->addSource(QStringLiteral("targetPick"), [] {
    return E()->targetPickActive() ? tr("Pick a target entity · Esc") : QString();
  });
  n->addSource(QStringLiteral("pen"), [] {
    return E()->penActive() ? tr("Path pen · Enter · Esc%1").arg(keyOf("path.pen"))
                            : QString();
  });
  n->addSource(QStringLiteral("editSource"), [] {
    const auto *e = E();
    const eng::mods::Host *h = e->editSourceHost() ? e->modHost(e->editSourceHost()) : nullptr;
    return h ? tr("Editing the source of %1%2").arg(h->name, keyOf("mod.editSource")) : QString();
  });
  n->addSource(QStringLiteral("modError"), [] {
    auto *e = E();
    const quint32 h = e->panelModHost();
    if (!h)
      return QString();
    const vk::Engine3D::ModStatus st = e->modStatus(h);
    if (!st.error)
      return QString();
    for (const auto &d: st.diagnostics)
      if (d.severity == eng::mods::Diagnostic::Severity::Error)
        return tr("%1: %2").arg(e->modHost(h) ? e->modHost(h)->name : QString(), d.text);
    return QString();
  });
  n->addSource(QStringLiteral("mirror"), [] {
    const auto *e = E();
    if (e->mirrorPlacing())
      return tr("Mirror: place the line%1").arg(keyOf("brush.mirrorMode"));
    if (!e->mirrorActive())
      return QString();
    static const char *names[3] = {"X", "Y", "Z"};
    const eng::mirror::Plane l = e->mirrorLine();
    return tr("Mirror %1 = %2%3")
        .arg(QString::fromLatin1(names[l.axis]))
        .arg(l.d * vk::Engine3D::kSourceUnitsPerWorldUnit, 0, 'g', 6)
        .arg(keyOf("brush.mirrorMode"));
  });
  n->addSource(QStringLiteral("autoSelect"), [] {
    const auto *e = E();
    if (!e->autoSelectActive())
      return QString();
    const QString summary = e->autoSelectSummary();
    return summary.isEmpty() ? tr("Auto-select") : tr("Auto-select · %1").arg(summary);
  });
  n->addSource(QStringLiteral("ioSim"), [] {
    const auto *e = E();
    if (!e->ioSimRunning())
      return QString();
    return e->ioSimPaused() ? tr("I/O simulation paused") : tr("I/O simulation running");
  });
  n->addSource(QStringLiteral("bevel"), [] {
    auto *e = E();
    if (!e->bevelMode())
      return QString();
    const int picked = e->bevelPickedCount();
    if (picked == 0)
      return tr("Bevel mode");
    const QString status = e->bevelStatus();
    return tr("Bevel mode · %n edge(s) · %1", "", picked).arg(status);
  });
  n->addSource(QStringLiteral("physicsSim"), [] {
    using PS = vk::Engine3D::PhysicsSimState;
    const auto *e = E();
    const PS state = e->physicsSimState();
    if (state == PS::None)
      return QString();
    const QString what = state == PS::Running ? tr("Physics simulation running") : tr("Physics simulation paused");
    const int dirty = e->physicsDirtyCount();
    return dirty > 0 ? tr("%1 · %n body(ies) moved", "", dirty).arg(what) : what;
  });

  // The selection.
  n->addSource(QStringLiteral("faceCount"), [] {
    const auto *e = E();
    const int faces = e->materialMode() ? e->selectedFaceCount() : 0;
    return faces > 0 ? tr("%n face(s) selected", nullptr, faces) : QString();
  });
  n->addSource(QStringLiteral("mirrorLinked"), [] {
    const int linked = E()->selectedMirrorLinked();
    if (linked <= 0)
      return QString();
    const QString text = tr("Mirror-linked · %n brush(es)", nullptr, linked);
    const QStringList sets = E()->selectedMirrorSetNames();
    return sets.isEmpty() ? text : tr("%1 · %2").arg(text, sets.join(QStringLiteral(", ")));
  });

  // Settings that change what an edit does or shows.
  n->addSource(QStringLiteral("selectionTint"), [] {
    if (!E()->maskHidden())
      return QString();
    return E()->materialMode() ? tr("Face(s) selected — hidden%1").arg(keyOf("view.selectionTint"))
                               : tr("Brush(es) selected — hidden%1").arg(keyOf("view.selectionTint"));
  });
  // Show Generated Brushes off: output hidden (not evaluated), the sources stand in.
  n->addSource(QStringLiteral("generatedHidden"), [] {
    if (E()->showModOutput() || !E()->scene() || E()->scene()->modHosts.empty())
      return QString();
    return tr("Generated brushes hidden%1").arg(keyOf("view.showGenerated"));
  });
  n->addSource(QStringLiteral("textureLock"), [] {
    return E()->textureLock() ? tr("Texture lock%1").arg(keyOf("brush.textureLock")) : QString();
  });
  n->addSource(QStringLiteral("textureScaleLock"), [] {
    return E()->textureScaleLock() ? tr("Texture scale lock%1").arg(keyOf("brush.scaleLock")) : QString();
  });

  n->refreshOn(e, &vk::Engine3D::cutModeChanged);
  n->refreshOn(e, &vk::Engine3D::cutHoleChanged);
  n->refreshOn(e, &vk::Engine3D::cutShapeChanged);
  n->refreshOn(e, &vk::Engine3D::shapeToolChanged);
  n->refreshOn(e, &vk::Engine3D::vertexToolChanged);
  n->refreshOn(e, &vk::Engine3D::targetPickChanged);
  n->refreshOn(e, &vk::Engine3D::mirrorChanged);
  n->refreshOn(e, &vk::Engine3D::penChanged);
  n->refreshOn(e, &vk::Engine3D::editSourceChanged);
  n->refreshOn(e, &vk::Engine3D::gizmoReadoutChanged);
  n->refreshOn(e, &vk::Engine3D::modHostsChanged);
  n->refreshOn(e, &vk::Engine3D::modOutputsChanged);
  n->refreshOn(e, &vk::Engine3D::autoSelectChanged);
  n->refreshOn(e, &vk::Engine3D::selectionChanged); // auto-select follows it
  n->refreshOn(e, &vk::Engine3D::marqueeChanged);
  n->refreshOn(e, &vk::Engine3D::ioSimChanged);
  n->refreshOn(e, &vk::Engine3D::physicsSimChanged);
  n->refreshOn(e, &vk::Engine3D::bevelChanged);
  n->refreshOn(e, &vk::Engine3D::fillChanged); // maskHidden
  n->refreshOn(e, &vk::Engine3D::textureLockChanged);
  n->refreshOn(e, &vk::Engine3D::textureScaleLockChanged);
  n->refreshOn(e, &vk::Engine3D::zenModeChanged);
  n->refreshOn(ctx_, &Ctx::editorModeChanged);
  n->refresh();
}
