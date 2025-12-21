#include "autoselect_panel.h"
#include "client/ui/scale.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/face_select.h"
#include "client/ui/action_manager.h"
#include "client/ui/flow_layout.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

QToolButton *shelfStyleButton(const QString &text, const QString &icon, const QString &tip, QWidget *parent,
                              bool withIcon = true) {
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("shelfButton"));
  b->setText(text);
  b->setToolTip(tip);
  b->setCheckable(true);
  b->setAutoRaise(true);
  b->setFocusPolicy(Qt::NoFocus);
  b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  if (withIcon) {
    b->setIcon(ActionManager::icon(icon));
    // 1.5 text lines: the icon grows with the text size.
    scale::bind(b, [b] {
      const int h = QFontMetrics(scale::font(scale::Font::Body, b->font())).height();
      b->setIconSize(QSize(int(h * 1.5), int(h * 1.5)));
    });
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  } else {
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
  }
  return b;
}

// A small icon beside the name.
QToolButton *compactButton(const QString &text, const QString &icon, const QString &tip, QWidget *parent) {
  auto *b = shelfStyleButton(text, icon, tip, parent);
  b->setProperty("compact", true); // DarkBlue.qss: a size up from the tool shelf's buttons
  b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  scale::bind(b, [b] {
    const int h = QFontMetrics(scale::font(scale::Font::Body, b->font())).height();
    const int s = int(std::lround(h * 1.2));
    b->setIconSize(QSize(s, s));
  });
  return b;
}

QWidget *row(QWidget *parent, std::initializer_list<QWidget *> widgets) {
  auto *w = new QWidget(parent);
  auto *h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  for (QWidget *x: widgets)
    h->addWidget(x, 1);
  return w;
}

QTimer *coalesce(QWidget *owner) {
  auto *t = new QTimer(owner);
  t->setSingleShot(true);
  t->setInterval(40);
  return t;
}

} // namespace

namespace {

struct IntentDef {
  const char *text, *icon, *tip;
};

// Indexed by eng::sel::Intent.
const IntentDef kFaceIntents[5] = {
  {"Wall", "axis_front", "Walls of a room or of a building (then choose indoor or outdoor)"},
  {"Floor", "axis_top", "Floors (then choose indoor or outdoor). Contiguous stays at one height"},
  {"Ceiling", "anchor_top", "Ceilings"},
  {"Facade", "home", "The outside of a building side, with its window reveals, trim and ledges"},
  {"Group", "group", "Faces sharing a face group with the picked face"},
};

// Indexed by eng::sel::BrushIntent.
const IntentDef kBrushIntents[7] = {
  {"Wall", "axis_front", "Wall brushes of a room or of a building (then choose indoor or outdoor)"},
  {"Floor", "axis_top", "Floor brushes: slabs whose top is a floor (then choose indoor or outdoor)"},
  {"Ceiling", "anchor_top", "Ceiling brushes: slabs whose underside is a ceiling"},
  {"Facade", "home", "The brushes making up the picked building side: walls, trim, ledges"},
  {"Same size", "con_sizelike", "Brushes with the same dimensions as the picked one (turned by 90° counts)"},
  {"Same material", "material", "Brushes whose main material is the picked brush's"},
  {"Touching", "linked", "Every brush connected to the picked one through contact"},
};

} // namespace

AutoSelectPanel::AutoSelectPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);

  // The kinds: one page per mode.
  m_kinds = new QStackedWidget(this);
  auto makeButton = [this](const IntentDef &d, QWidget *parent, int intent) {
    auto *b = compactButton(tr(d.text), QString::fromLatin1(d.icon), tr(d.tip) + tr("\nShift+click adds to the selection"),
                            parent);
    connect(b, &QToolButton::clicked, this,
            [this, intent] { pickIntent(intent, QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    return b;
  };
  // Not a kind: selects at once.
  auto makeSameMirror = [this](QWidget *page, const QString &tip) {
    auto *b = compactButton(tr("Same mirror"), QStringLiteral("mod_mirror"),
                            tip + tr("\nShift+click adds to the selection"), page);
    b->setCheckable(false);
    connect(b, &QToolButton::clicked, this, [this] {
      m_pending = -1;
      m_engine->selectSameMirror(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    return b;
  };
  {
    auto *page = new QWidget(m_kinds);
    auto *line = new FlowLayout(page, 2, 2);
    // Not a kind: selects at once (no box).
    m_sameBrush = compactButton(tr("Same brush"), QStringLiteral("mesh_cube"),
                                tr("Every face of the brushes the picked faces are on") +
                                    tr("\nShift+click adds to the selection"),
                                page);
    m_sameBrush->setCheckable(false);
    connect(m_sameBrush, &QToolButton::clicked, this, [this] {
      m_pending = -1;
      m_engine->selectSameBrushFaces(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    line->addWidget(m_sameBrush);
    for (int i = 0; i < 5; ++i) {
      m_faceIntents[size_t(i)] = makeButton(kFaceIntents[i], page, i);
      line->addWidget(m_faceIntents[size_t(i)]);
    }
    // Not a kind: the whole scene, at once.
    m_sameMaterialFaces = compactButton(tr("Same material"), QStringLiteral("material"),
                                        tr("Every face in the map with a material of the picked faces") +
                                            tr("\nShift+click adds to the selection"),
                                        page);
    m_sameMaterialFaces->setCheckable(false);
    connect(m_sameMaterialFaces, &QToolButton::clicked, this, [this] {
      m_pending = -1;
      m_engine->selectSameMaterialFaces(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    line->addWidget(m_sameMaterialFaces);
    m_sameMirrorFaces = makeSameMirror(page, tr("Every face of the mirror collections the picked faces are in"));
    line->addWidget(m_sameMirrorFaces);
    m_kinds->addWidget(page); // 0: Material mode
  }
  {
    auto *page = new QWidget(m_kinds);
    auto *line = new FlowLayout(page, 2, 2);
    for (int i = 0; i < 7; ++i) {
      m_brushIntents[size_t(i)] = makeButton(kBrushIntents[i], page, i);
      line->addWidget(m_brushIntents[size_t(i)]);
    }
    m_sameMirror = makeSameMirror(page, tr("Every brush of the mirror collections the picked brushes are in, "
                                           "both sides"));
    line->addWidget(m_sameMirror);
    m_kinds->addWidget(page); // 1: Object mode
  }
  // Size to the shown page.
  connect(m_kinds, &QStackedWidget::currentChanged, this, [this](int cur) {
    for (int i = 0; i < m_kinds->count(); ++i)
      m_kinds->widget(i)->setSizePolicy(QSizePolicy::Preferred, i == cur ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    m_kinds->adjustSize();
  });
  v->addWidget(m_kinds);

  m_indoor = shelfStyleButton(tr("Indoor"), {}, tr("Enclosed spaces: rooms, corridors"), this, false);
  m_outdoor = shelfStyleButton(tr("Outdoor"), {}, tr("Open to the sky or the outside"), this, false);
  m_exposureRow = row(this, {m_indoor, m_outdoor});
  v->addWidget(m_exposureRow);
  // Not a QButtonGroup: neither may be checked, and refresh() decides the checked state.
  connect(m_indoor, &QToolButton::clicked, this, [this] { pickExposure(int(eng::sel::Exposure::Indoor)); });
  connect(m_outdoor, &QToolButton::clicked, this, [this] { pickExposure(int(eng::sel::Exposure::Outdoor)); });

  m_thisSide = shelfStyleButton(tr("This side"), {}, tr("Only the side of the building the picked face is on"), this, false);
  m_allSides = shelfStyleButton(tr("All sides"), {}, tr("Continue around the building's outer corners"), this, false);
  m_thisSide->setChecked(true);
  auto *sides = new QButtonGroup(this);
  sides->addButton(m_thisSide, 0);
  sides->addButton(m_allSides, 1);
  m_sideRow = row(this, {m_thisSide, m_allSides});
  v->addWidget(m_sideRow);
  connect(sides, &QButtonGroup::idClicked, this, [this] { rerunIfActive(); });

  m_group = new QComboBox(this);
  m_group->setToolTip(tr("Which of the picked face's groups to follow"));
  auto *groupLabel = new QLabel(tr("Group"), this);
  m_groupRow = new QWidget(this);
  auto *gh = new QHBoxLayout(m_groupRow);
  gh->setContentsMargins(0, 0, 0, 0);
  gh->addWidget(groupLabel);
  gh->addWidget(m_group, 1);
  v->addWidget(m_groupRow);
  connect(m_group, &QComboBox::activated, this, [this] { run(int(eng::sel::Intent::Group), 0, m_add); });

  m_contiguous = new QCheckBox(tr("Contiguous"), this);
  m_contiguous->setChecked(true);
  m_sameMaterial = new QCheckBox(tr("Same material"), this);
  m_byObject = new QCheckBox(tr("By object"), this);
  m_byObject->setToolTip(tr("Only faces of the generated object (stairs) the picked face is on: the treads of one "
                            "stairs, not of every stairs in the map"));
  m_clear = new QPushButton(tr("Clear"), this);
  m_clear->setToolTip(tr("Drop the auto-select box and the selection"));
  auto *opts = new QHBoxLayout();
  opts->addWidget(m_contiguous);
  opts->addWidget(m_sameMaterial);
  opts->addWidget(m_byObject);
  opts->addStretch(1);
  opts->addWidget(m_clear);
  v->addLayout(opts);
  connect(m_contiguous, &QCheckBox::toggled, this, [this] { rerunIfActive(); });
  connect(m_sameMaterial, &QCheckBox::toggled, this, [this] { rerunIfActive(); });
  connect(m_byObject, &QCheckBox::toggled, this, [this] { rerunIfActive(); });
  connect(m_clear, &QPushButton::clicked, this, [this] {
    m_pending = -1;
    m_engine->clearAutoSelect();
    m_engine->clearSelection();
  });

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  m_status->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_status);

  m_material = !m_engine->materialMode(); // forces the first refresh to set the page
  m_timer = coalesce(this);
  connect(m_timer, &QTimer::timeout, this, &AutoSelectPanel::refresh);
  for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::autoSelectChanged, &vk::Engine3D::marqueeChanged,
                  &vk::Engine3D::faceGroupsChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
  connect(m_engine, &vk::Engine3D::documentChanged, this, [this] {
    m_pending = -1;
    m_timer->start();
  });
  refresh();
}

bool AutoSelectPanel::needsExposure(const int intent) const {
  // Wall = 0 and Floor = 1 in both eng::sel::Intent and eng::sel::BrushIntent.
  return intent == int(eng::sel::Intent::Wall) || intent == int(eng::sel::Intent::Floor);
}

int AutoSelectPanel::shownIntent() const {
  if (m_pending >= 0)
    return m_pending;
  return m_engine->autoSelectActive() ? m_engine->autoSelectIntent() : -1;
}

void AutoSelectPanel::pickIntent(const int intent, const bool add) {
  m_add = add;
  // Wall / floor wait for Indoor or Outdoor every time.
  if (needsExposure(intent)) {
    m_pending = intent;
    refresh();
    return;
  }
  if (m_engine->materialMode() && intent == int(eng::sel::Intent::Group)) {
    m_pending = intent;
    refresh(); // fills the group list from the picked face
    if (m_group->count() == 0)
      return;
  }
  run(intent, 0, add);
}

void AutoSelectPanel::pickExposure(const int exposure) {
  if (m_pending >= 0 && needsExposure(m_pending)) {
    run(m_pending, exposure, m_add);
    return;
  }
  if (m_engine->autoSelectActive() && needsExposure(m_engine->autoSelectIntent())) {
    run(m_engine->autoSelectIntent(), exposure, m_add);
    return;
  }
  refresh();
}

void AutoSelectPanel::run(const int intent, const int exposure, const bool add) {
  if (intent < 0)
    return;
  m_pending = -1;
  m_add = add;
  const quint32 group = m_group->currentData().toUInt();
  m_engine->runAutoSelect(intent, needsExposure(intent) ? exposure : 0, m_allSides->isChecked(), group,
                          m_contiguous->isChecked(), add, m_sameMaterial->isChecked(),
                          m_engine->materialMode() && m_byObject->isChecked());
  refresh();
}

void AutoSelectPanel::rerunIfActive() {
  if (m_pending < 0 && m_engine->autoSelectActive())
    run(m_engine->autoSelectIntent(), m_engine->autoSelectExposure(), m_add);
  else
    refresh();
}

void AutoSelectPanel::refresh() {
  auto *e = m_engine;
  const bool material = e->materialMode();
  if (material != m_material) {
    // A pending kind does not carry over to the other mode; a finished run does (one per mode in the engine).
    m_material = material;
    m_pending = -1;
    m_kinds->setCurrentIndex(material ? 0 : 1);
  }
  const bool active = e->autoSelectActive();
  const bool seed = material ? e->selectedFaceCount() > 0 : !e->selectedObjects().empty();
  const bool seedOrBox = seed || e->hasMarquee();
  // A pending kind ends with the selection.
  if (!seedOrBox && !active)
    m_pending = -1;
  const int shown = shownIntent();

  auto setButtons = [&](auto &buttons) {
    for (int i = 0; i < int(buttons.size()); ++i) {
      QSignalBlocker b(buttons[size_t(i)]);
      buttons[size_t(i)]->setChecked(i == shown);
      buttons[size_t(i)]->setEnabled(seedOrBox || active);
    }
  };
  const bool mirrored = !e->selectedMirrorSets().empty() || active;
  if (material) {
    setButtons(m_faceIntents);
    m_sameBrush->setEnabled(seed || active);
    m_sameMaterialFaces->setEnabled(seed || active);
    m_sameMirrorFaces->setEnabled(mirrored);
  } else {
    setButtons(m_brushIntents);
    m_sameMirror->setEnabled(mirrored);
  }

  const bool exposureRow = shown >= 0 && needsExposure(shown);
  m_exposureRow->setVisible(exposureRow);
  {
    // Lit only for the running result.
    const int exposure = m_pending < 0 && active ? e->autoSelectExposure() : 0;
    QSignalBlocker b1(m_indoor), b2(m_outdoor);
    m_indoor->setChecked(exposure == int(eng::sel::Exposure::Indoor));
    m_outdoor->setChecked(exposure == int(eng::sel::Exposure::Outdoor));
  }
  const bool facade = material ? shown == int(eng::sel::Intent::Facade) : shown == int(eng::sel::BrushIntent::Facade);
  m_sideRow->setVisible(facade);
  const QString side = material ? tr("face") : tr("brush");
  m_thisSide->setToolTip(tr("Only the side of the building the picked %1 is on").arg(side));
  m_groupRow->setVisible(material && shown == int(eng::sel::Intent::Group));
  m_byObject->setVisible(material);

  m_contiguous->setToolTip(material ? tr("On: grow from the picked face over shared edges.\n"
                                         "Off: every matching face (inside the box, when there is one).")
                                    : tr("On: grow from the picked brush over brushes it touches.\n"
                                         "Off: every matching brush (inside the box, when there is one)."));
  m_sameMaterial->setToolTip(material ? tr("Only faces with the same material as the picked face(s)")
                                      : tr("Only brushes whose main material is the picked brush's"));

  if (m_groupRow->isVisible() && !active) {
    const quint32 keep = m_group->currentData().toUInt();
    QSignalBlocker b(m_group);
    m_group->clear();
    for (const auto &g: e->seedFaceGroups())
      m_group->addItem(g.name, g.id);
    const int idx = m_group->findData(keep);
    m_group->setCurrentIndex(idx >= 0 ? idx : std::max(0, m_group->findData(e->defaultSeedGroup())));
  }
  m_clear->setEnabled(active || e->hasMarquee());

  QString status = m_pending >= 0 ? QString() : e->autoSelectSummary();
  if (status.isEmpty() && seedOrBox) {
    if (exposureRow && m_pending >= 0)
      status = tr("Indoor or outdoor?");
    else if (m_groupRow->isVisible() && m_group->count() == 0)
      status = tr("The picked face is in no group.");
  }
  m_status->setText(status);
  m_status->setVisible(!status.isEmpty());
}

FacadePanel::FacadePanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);
  const char *tips[6] = {
    "Front faces on the main wall plane",
    "Front faces set back from the wall (window and door panels)",
    "Sides, sills and heads inside openings",
    "Front faces standing out from the wall, and their ends",
    "Up-facing faces standing out (ledges, cornice tops)",
    "Down-facing faces standing out (soffits, undersides)",
  };
  auto *grid = new QGridLayout();
  grid->setHorizontalSpacing(10);
  for (int t = 0; t < 6; ++t) {
    m_types[size_t(t)] = new QCheckBox(this);
    m_types[size_t(t)]->setToolTip(tr(tips[t]));
    grid->addWidget(m_types[size_t(t)], t / 3, t % 3);
    connect(m_types[size_t(t)], &QCheckBox::toggled, this, [this, t](bool on) { m_engine->setFacadeTypeEnabled(t, on); });
  }
  v->addLayout(grid);
  m_storeys = new QPushButton(tr("Storeys…"), this);
  m_storeys->setCheckable(true);
  m_storeys->setToolTip(tr("Split this facade into storeys and give each its own material"));
  auto *h = new QHBoxLayout();
  h->addStretch(1);
  h->addWidget(m_storeys);
  v->addLayout(h);
  connect(m_storeys, &QPushButton::toggled, this, [this](bool on) {
    if (on)
      m_engine->showStoreys();
    else
      m_engine->hideStoreys();
  });

  m_timer = coalesce(this);
  connect(m_timer, &QTimer::timeout, this, &FacadePanel::refresh);
  connect(m_engine, &vk::Engine3D::autoSelectChanged, m_timer, qOverload<>(&QTimer::start));
  connect(m_engine, &vk::Engine3D::fillChanged, m_timer, qOverload<>(&QTimer::start));
  connect(m_engine, &vk::Engine3D::selectionChanged, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void FacadePanel::refresh() {
  const auto counts = m_engine->facadeTypeCounts();
  for (int t = 0; t < 6; ++t) {
    QSignalBlocker b(m_types[size_t(t)]);
    m_types[size_t(t)]->setText(QStringLiteral("%1 %2").arg(eng::sel::facadeTypeName(eng::sel::FacadeType(t))).arg(counts[size_t(t)]));
    m_types[size_t(t)]->setChecked(m_engine->facadeTypeEnabled(t));
    m_types[size_t(t)]->setEnabled(counts[size_t(t)] > 0);
  }
  // Storeys split facade faces: Material mode only.
  m_storeys->setVisible(m_engine->materialMode());
  QSignalBlocker b(m_storeys);
  m_storeys->setChecked(m_engine->storeysShown());
}

StoreysPanel::StoreysPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);
  m_note = new QLabel(this);
  m_note->setWordWrap(true);
  m_note->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_note);
  m_layers = new QListWidget(this);
  scale::bind(m_layers, [=, this] { m_layers->setMaximumHeight(scale::tpx(130)); });
  v->addWidget(m_layers);
  auto *matRow = new QHBoxLayout();
  m_layerMat = new QLineEdit(this);
  m_layerMat->setPlaceholderText(tr("material for the picked storey"));
  m_set = new QPushButton(tr("Set"), this);
  matRow->addWidget(m_layerMat, 1);
  matRow->addWidget(m_set);
  v->addLayout(matRow);
  auto *act = new QHBoxLayout();
  m_apply = new QPushButton(tr("Apply per storey"), this);
  m_all = new QPushButton(tr("Apply to all"), this);
  m_all->setToolTip(tr("Put the material above (or the current one) on every selected face"));
  m_slice = new QPushButton(tr("Slice to storeys"), this);
  m_slice->setToolTip(tr("Split the brushes along the storey seams"));
  act->addWidget(m_apply);
  act->addWidget(m_all);
  act->addWidget(m_slice);
  v->addLayout(act);
  auto *bottom = new QHBoxLayout();
  m_outline = new QCheckBox(tr("Show storey outline"), this);
  m_close = new QPushButton(tr("Close"), this);
  bottom->addWidget(m_outline, 1);
  bottom->addWidget(m_close);
  v->addLayout(bottom);

  connect(m_layers, &QListWidget::currentRowChanged, this, [this](int r) {
    if (r >= 0)
      m_layerMat->setText(m_engine->layerMaterial(r));
  });
  connect(m_set, &QPushButton::clicked, this, [this] {
    if (const int r = m_layers->currentRow(); r >= 0)
      m_engine->setLayerMaterial(r, m_layerMat->text().trimmed());
  });
  connect(m_apply, &QPushButton::clicked, this, [this] { m_engine->applyFillMaterials(); });
  connect(m_all, &QPushButton::clicked, this, [this] {
    const QString m = m_layerMat->text().trimmed();
    m_engine->applyFillMaterial(m.isEmpty() ? m_engine->currentMaterial() : m);
  });
  connect(m_slice, &QPushButton::clicked, this, [this] { m_engine->sliceFillToLayers(); });
  connect(m_outline, &QCheckBox::toggled, this, [this](bool on) { m_engine->setFillWireVisible(on); });
  connect(m_close, &QPushButton::clicked, this, [this] { m_engine->hideStoreys(); });

  m_timer = coalesce(this);
  connect(m_timer, &QTimer::timeout, this, &StoreysPanel::refresh);
  connect(m_engine, &vk::Engine3D::fillChanged, m_timer, qOverload<>(&QTimer::start));
  connect(m_engine, &vk::Engine3D::autoSelectChanged, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void StoreysPanel::refresh() {
  auto *e = m_engine;
  const int n = e->fillLevelCount();
  m_note->setText(!e->fillNote().isEmpty() ? e->fillNote()
                  : n > 0 ? tr("%1 storey(s)").arg(n)
                          : tr("No storeys found."));
  {
    QSignalBlocker b(m_outline);
    m_outline->setChecked(e->fillWireVisible());
  }
  m_apply->setEnabled(n > 0);
  m_slice->setEnabled(e->fillHasLayers());
  m_all->setEnabled(n > 0);
  const QStringList labels = e->fillLevelLabels();
  const int cur = m_layers->currentRow();
  QSignalBlocker b(m_layers);
  m_layers->clear();
  for (int i = 0; i < labels.size(); ++i) {
    const QString mat = e->layerMaterial(i);
    m_layers->addItem(mat.isEmpty() ? labels[i] : QStringLiteral("%1  —  %2").arg(labels[i], mat));
  }
  if (cur >= 0 && cur < m_layers->count())
    m_layers->setCurrentRow(cur);
}

AutoSelectDock::AutoSelectDock(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *body = new QWidget(scroll);
  auto *v = new QVBoxLayout(body);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(2);
  scroll->setWidget(body);
  outer->addWidget(scroll);

  auto *main = new Section(tr("Auto-select"), QStringLiteral("select_extend"), body);
  main->body()->addWidget(new AutoSelectPanel(m_engine, main));
  v->addWidget(main);
  auto *facade = new Section(tr("Facade"), QStringLiteral("home"), body);
  facade->body()->addWidget(new FacadePanel(m_engine, facade));
  v->addWidget(facade);
  m_facade = facade;
  auto *storeys = new Section(tr("Storeys"), QStringLiteral("sort_asc"), body);
  storeys->body()->addWidget(new StoreysPanel(m_engine, storeys));
  v->addWidget(storeys);
  m_storeys = storeys;
  v->addStretch(1);

  m_timer = coalesce(this);
  connect(m_timer, &QTimer::timeout, this, &AutoSelectDock::refresh);
  for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::autoSelectChanged, &vk::Engine3D::fillChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void AutoSelectDock::refresh() {
  m_facade->setVisible(m_engine->autoSelectIsFacade());
  m_storeys->setVisible(m_engine->materialMode() && m_engine->storeysShown());
}

} // namespace ui
