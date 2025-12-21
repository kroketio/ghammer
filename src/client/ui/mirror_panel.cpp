#include "mirror_panel.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QAction>
#include <QApplication>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>

#include "client/engine/scene.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

QWidget *hrow(QWidget *parent, std::initializer_list<QWidget *> widgets) {
  auto *w = new QWidget(parent);
  auto *h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  for (QWidget *x: widgets)
    h->addWidget(x, 1);
  return w;
}

} // namespace

MirrorPanel::MirrorPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(7);
  grid->setColumnStretch(1, 1);
  int row = 0;

  // Only while mirror mode is on.
  m_modeRow = new QWidget(this);
  auto *mg = new QVBoxLayout(m_modeRow);
  mg->setContentsMargins(0, 0, 0, 0);
  mg->setSpacing(7);
  m_mirrorSel = new QPushButton(tr("Mirror selection"), m_modeRow);
  m_mirrorSel->setToolTip(tr("Mirror the selected brushes across the line (an identical copy already there is linked)"));
  mg->addWidget(hrow(m_modeRow, {m_mirrorSel}));
  connect(m_mirrorSel, &QPushButton::clicked, this, [] {
    if (QAction *a = ui::act(QStringLiteral("brush.mirrorSelection")))
      a->trigger();
  });

  m_reset = new QPushButton(tr("Reset line"), m_modeRow);
  m_reset->setToolTip(tr("Remove the mirror line and draw a new one (linked brushes stay linked)"));
  m_off = new QPushButton(tr("Turn off"), m_modeRow);
  mg->addWidget(hrow(m_modeRow, {m_reset, m_off}));
  connect(m_reset, &QPushButton::clicked, this, [this] {
    m_engine->resetMirrorLine();
    m_engine->setMirrorActive(true);
  });
  connect(m_off, &QPushButton::clicked, this, [this] { m_engine->setMirrorActive(false); });

  m_textures = new QCheckBox(tr("Mirror textures"), m_modeRow);
  m_textures->setToolTip(tr("New copies show the mirror image of the texture"));
  mg->addWidget(m_textures);
  connect(m_textures, &QCheckBox::toggled, this, [this](bool on) { m_engine->setMirrorTextures(on); });
  grid->addWidget(m_modeRow, row++, 0, 1, 2);

  m_linked = new QLabel(this);
  auto *twin = new QPushButton(tr("Select twin"), this);
  twin->setToolTip(tr("Select the mirror copy of the selection (Shift: keep the selection too)"));
  auto *detach = new QPushButton(tr("Detach"), this);
  detach->setToolTip(tr("Unlink the selection from its mirror copies"));
  m_linkRow = new QWidget(this);
  auto *lg = new QGridLayout(m_linkRow);
  lg->setContentsMargins(0, 0, 0, 0);
  lg->setHorizontalSpacing(10);
  lg->setVerticalSpacing(7);
  lg->setColumnStretch(1, 1);
  lg->addWidget(new QLabel(tr("Linked"), m_linkRow), 0, 0);
  lg->addWidget(m_linked, 0, 1);
  lg->addWidget(hrow(m_linkRow, {twin, detach}), 1, 0, 1, 2);
  // Add to Mirror N.
  m_addTo = new QPushButton(m_linkRow);
  m_addTo->setToolTip(tr("The selected brushes that are in no mirror join this one (each gets its twin across its "
                         "line; stairs and arches as a whole)"));
  lg->addWidget(m_addTo, 3, 0, 1, 2);
  connect(m_addTo, &QPushButton::clicked, this, [] {
    if (QAction *a = ui::act(QStringLiteral("brush.addToMirror")))
      a->trigger();
  });
  m_hideCopies = new QCheckBox(tr("Hide mirrored copies"), m_linkRow);
  m_hideCopies->setToolTip(tr("Hide the mirrored side of this mirror in the 2D and 3D views (saved with the map)"));
  lg->addWidget(m_hideCopies, 2, 0, 1, 2);
  connect(m_hideCopies, &QCheckBox::clicked, this, [this] {
    m_engine->setMirrorSetsHidden(m_engine->selectedMirrorSets(), m_hideCopies->checkState() != Qt::Unchecked);
  });
  grid->addWidget(m_linkRow, row++, 0, 1, 2);
  connect(twin, &QPushButton::clicked, this, [this] {
    m_engine->selectMirrorTwins(QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier));
  });
  connect(detach, &QPushButton::clicked, this, [this] { m_engine->detachMirror(); });

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(30);
  connect(m_timer, &QTimer::timeout, this, &MirrorPanel::refresh);
  for (auto sig: {&vk::Engine3D::mirrorChanged, &vk::Engine3D::selectionChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void MirrorPanel::refresh() {
  const bool on = m_engine->mirrorActive();
  const bool line = m_engine->mirrorLineValid();
  m_modeRow->setVisible(on);
  m_reset->setEnabled(line);
  m_mirrorSel->setEnabled(line && !m_engine->selectedBrushIds().empty());
  {
    QSignalBlocker b(m_textures);
    m_textures->setChecked(m_engine->mirrorTextures());
  }
  const int linked = m_engine->selectedMirrorLinked();
  m_linkRow->setVisible(linked > 0);
  m_linked->setText(tr("%n brush(es)", nullptr, linked));
  const quint32 addSet = m_engine->mirrorSetToAddTo();
  const eng::Scene::MirrorSet *ms = addSet && m_engine->scene() ? m_engine->scene()->findMirrorSet(addSet) : nullptr;
  m_addTo->setVisible(addSet != 0);
  m_addTo->setText(tr("Add to %1").arg(ms ? ms->name : tr("mirror")));
  {
    // Tri-state when the selection spans sets that differ.
    int hidden = 0, sets = 0;
    if (const eng::Scene *scene = m_engine->scene().data())
      for (const quint32 id: m_engine->selectedMirrorSets())
        if (const auto *st = scene->findMirrorSet(id)) {
          ++sets;
          hidden += st->hideMirrored ? 1 : 0;
        }
    QSignalBlocker b(m_hideCopies);
    m_hideCopies->setTristate(hidden > 0 && hidden < sets);
    m_hideCopies->setCheckState(hidden == 0 ? Qt::Unchecked : hidden == sets ? Qt::Checked : Qt::PartiallyChecked);
  }
}

} // namespace ui
