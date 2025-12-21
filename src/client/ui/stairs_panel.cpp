#include "stairs_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/stair_gen.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

QToolButton *segment(const QString &text, const QString &tip, QWidget *parent) {
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("shelfButton"));
  b->setText(text);
  b->setToolTip(tip);
  b->setCheckable(true);
  b->setFocusPolicy(Qt::NoFocus);
  b->setToolButtonStyle(Qt::ToolButtonTextOnly);
  b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return b;
}

NumberField *units(double lo, double hi, double step, QWidget *parent) {
  auto *f = new NumberField(parent);
  f->setRange(lo, hi);
  f->setSingleStep(step);
  f->setDecimals(0);
  f->setSuffix(QStringLiteral(" u"));
  return f;
}

QWidget *hrow(QWidget *parent, std::initializer_list<std::pair<QWidget *, int>> widgets) {
  auto *w = new QWidget(parent);
  auto *h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  for (const auto &[x, stretch]: widgets)
    h->addWidget(x, stretch);
  return w;
}

} // namespace

StairsPanel::StairsPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(7);
  grid->setColumnStretch(1, 1);
  int row = 0;
  auto add = [&](const QString &label, QWidget *w, const QString &tip = {}) {
    auto *l = new QLabel(label, this);
    l->setToolTip(tip);
    grid->addWidget(l, row, 0);
    grid->addWidget(w, row, 1);
    ++row;
    return l;
  };

  m_name = new QLineEdit(this);
  add(tr("Name"), m_name);
  connect(m_name, &QLineEdit::editingFinished, this, [this] { m_engine->renameProc(m_id, m_name->text()); });

  m_bySize = segment(tr("Step size"), tr("The step count follows the box height and the step height"), this);
  m_byCount = segment(tr("Count"), tr("A fixed number of steps, stretched to the box"), this);
  auto *sizing = new QButtonGroup(this);
  sizing->addButton(m_bySize, 0);
  sizing->addButton(m_byCount, 1);
  add(tr("Steps by"), hrow(this, {{m_bySize, 1}, {m_byCount, 1}}));
  connect(sizing, &QButtonGroup::idClicked, this, [this](int id) { setInt("sizing", id); });

  m_height = units(1, 256, 1, this);
  m_height->setToolTip(tr("Step height: the count is the box height divided by this"));
  m_depth = units(1, 512, 1, this);
  m_depth->setToolTip(tr("Tread depth used by Fit (the treads otherwise stretch to fill the box)"));
  auto *times = new QLabel(QStringLiteral("×"), this);
  m_sizeRow = hrow(this, {{m_height, 1}, {times, 0}, {m_depth, 1}});
  auto *sizeLabel = add(tr("Step size"), m_sizeRow, tr("Height × tread depth, in units"));
  m_sizeRow->setProperty("label", QVariant::fromValue<QObject *>(sizeLabel));
  connect(m_height, &NumberField::committed, this, [this](double v) { set("stepHeight", v); });
  connect(m_depth, &NumberField::committed, this, [this](double v) { set("stepDepth", v); });

  m_steps = new NumberField(this);
  m_steps->setRange(1, 256);
  m_steps->setDecimals(0);
  m_countRow = m_steps;
  auto *countLabel = add(tr("Steps"), m_steps);
  m_countRow->setProperty("label", QVariant::fromValue<QObject *>(countLabel));
  connect(m_steps, &NumberField::committed, this, [this](double v) { setInt("steps", int(v)); });

  m_direction = new QComboBox(this);
  m_direction->addItems({tr("+X"), tr("−X"), tr("+Z"), tr("−Z")});
  m_direction->setToolTip(tr("The direction the stairs climb toward"));
  auto *turn = new QPushButton(tr("Turn 90°"), this);
  turn->setToolTip(tr("Turn the climb direction a quarter turn (seen from the top)"));
  add(tr("Climbs"), hrow(this, {{m_direction, 1}, {turn, 0}}));
  connect(m_direction, &QComboBox::activated, this, [this](int i) { setInt("direction", i); });
  connect(turn, &QPushButton::clicked, this, [this] {
    static const int next[4] = {2, 3, 1, 0}; // +X -> +Z -> -X -> -Z -> +X
    setInt("direction", next[std::clamp(m_direction->currentIndex(), 0, 3)]);
  });

  m_underside[0] = segment(tr("Solid"), tr("Every step down to the floor"), this);
  m_underside[1] = segment(tr("Planks"), tr("Step-high planks: a zigzag seen from the side"), this);
  m_underside[2] = segment(tr("Slope"), tr("A sloped underside, like the soffit of a stair flight"), this);
  auto *under = new QButtonGroup(this);
  for (int i = 0; i < 3; ++i)
    under->addButton(m_underside[i], i);
  add(tr("Underside"), hrow(this, {{m_underside[0], 1}, {m_underside[1], 1}, {m_underside[2], 1}}));
  connect(under, &QButtonGroup::idClicked, this, [this](int id) { setInt("underside", id); });

  m_thickness = units(1, 512, 1, this);
  m_thickness->setToolTip(tr("How far the sloped underside sits below the step nosings"));
  m_thicknessRow = m_thickness;
  auto *thickLabel = add(tr("Thickness"), m_thickness);
  m_thicknessRow->setProperty("label", QVariant::fromValue<QObject *>(thickLabel));
  connect(m_thickness, &NumberField::committed, this, [this](double v) { set("thickness", v); });

  m_thinPlanks = new QCheckBox(tr("Thin planks"), this);
  m_thinPlanks->setToolTip(tr("Real planks of the thickness below instead of step-high blocks"));
  grid->addWidget(m_thinPlanks, row++, 0, 1, 2);
  connect(m_thinPlanks, &QCheckBox::toggled, this, [this](bool on) {
    if (m_id)
      m_engine->setProcParam(m_id, QStringLiteral("thinPlanks"), on);
  });
  m_plankThickness = units(0.5, 64, 1, this);
  m_plankThickness->setDecimals(1);
  m_plankThickness->setToolTip(tr("Thickness of every plank"));
  m_plankRow = m_plankThickness;
  auto *plankLabel = add(tr("Plank thickness"), m_plankThickness);
  m_plankRow->setProperty("label", QVariant::fromValue<QObject *>(plankLabel));
  connect(m_plankThickness, &NumberField::committed, this, [this](double v) { set("plankThickness", v); });

  m_nosing = new QCheckBox(tr("Overshoot"), this);
  m_nosing->setToolTip(tr("An extra brush on the front edge of every step, sticking out over the step below"));
  grid->addWidget(m_nosing, row++, 0, 1, 2);
  connect(m_nosing, &QCheckBox::toggled, this, [this](bool on) {
    if (m_id)
      m_engine->setProcParam(m_id, QStringLiteral("nosing"), on);
  });
  m_nosingDepth = units(0.5, 64, 1, this);
  m_nosingDepth->setDecimals(1);
  m_nosingDepth->setToolTip(tr("How far the overshoot sticks out"));
  m_nosingHeight = units(0.5, 64, 1, this);
  m_nosingHeight->setDecimals(1);
  m_nosingHeight->setToolTip(tr("How tall the overshoot is"));
  m_nosingRow = hrow(this, {{m_nosingDepth, 1}, {new QLabel(QStringLiteral("×"), this), 0}, {m_nosingHeight, 1}});
  auto *nosingLabel = add(tr("Overshoot"), m_nosingRow, tr("Depth × height, in units"));
  m_nosingRow->setProperty("label", QVariant::fromValue<QObject *>(nosingLabel));
  connect(m_nosingDepth, &NumberField::committed, this, [this](double v) { set("nosingDepth", v); });
  connect(m_nosingHeight, &NumberField::committed, this, [this](double v) { set("nosingHeight", v); });

  m_sides = new QCheckBox(tr("Sides"), this);
  m_sides->setToolTip(tr("A brush along each end of the steps (a stringer), inside the box: the steps get "
                         "narrower. Solid stairs: down to the floor; planks and slope: a band that follows the "
                         "underside"));
  grid->addWidget(m_sides, row++, 0, 1, 2);
  connect(m_sides, &QCheckBox::toggled, this, [this](bool on) {
    if (m_id)
      m_engine->setProcParam(m_id, QStringLiteral("sides"), on);
  });
  m_sideWidth = units(0.5, 128, 1, this);
  m_sideWidth->setDecimals(1);
  m_sideWidth->setToolTip(tr("How wide each side brush is"));
  m_sideHeight = units(0, 256, 1, this);
  m_sideHeight->setDecimals(1);
  m_sideHeight->setToolTip(tr("How far the sides rise above the step nosings"));
  m_sidesRow = hrow(this, {{m_sideWidth, 1}, {new QLabel(QStringLiteral("×"), this), 0}, {m_sideHeight, 1}});
  auto *sidesLabel = add(tr("Sides"), m_sidesRow, tr("Width × height above the steps, in units"));
  m_sidesRow->setProperty("label", QVariant::fromValue<QObject *>(sidesLabel));
  connect(m_sideWidth, &NumberField::committed, this, [this](double v) { set("sideWidth", v); });
  connect(m_sideHeight, &NumberField::committed, this, [this](double v) { set("sideHeight", v); });

  m_collision = new QCheckBox(tr("Collision ramp"), this);
  m_collision->setToolTip(tr("A clip brush (func_clip) sloping over the steps, so players walk up a ramp "
                             "(solid: down to the floor; planks and slope: a slab that leaves the space underneath "
                             "open)"));
  grid->addWidget(m_collision, row++, 0, 1, 2);
  connect(m_collision, &QCheckBox::toggled, this, [this](bool on) {
    if (m_id)
      m_engine->setProcParam(m_id, QStringLiteral("collision"), on);
  });

  m_info = new QLabel(this);
  m_info->setWordWrap(true);
  m_info->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  grid->addWidget(m_info, row++, 0, 1, 2);

  m_fit = new QPushButton(tr("Fit length to tread"), this);
  m_fit->setToolTip(tr("Change the box length so every tread is exactly the tread depth"));
  m_dissolve = new QPushButton(tr("Convert to brushes"), this);
  m_dissolve->setToolTip(tr("Make these plain brushes (they can no longer be tweaked here)"));
  grid->addWidget(hrow(this, {{m_fit, 1}, {m_dissolve, 1}}), row++, 0, 1, 2);
  connect(m_fit, &QPushButton::clicked, this, [this] { m_engine->fitStairsDepth(m_id); });
  connect(m_dissolve, &QPushButton::clicked, this, [this] { m_engine->dissolveProc(m_id); });
}

void StairsPanel::set(const char *key, const double value) {
  if (m_id)
    m_engine->setProcParam(m_id, QString::fromLatin1(key), value);
}

void StairsPanel::setInt(const char *key, const int value) {
  if (m_id)
    m_engine->setProcParam(m_id, QString::fromLatin1(key), value);
}

void StairsPanel::refresh() {
  m_id = m_engine->selectedProcObject();
  if (!m_id || m_engine->procType(m_id) != QLatin1String("stairs"))
    return;
  eng::stairs::Params p = eng::stairs::fromJson(m_engine->procParams(m_id));
  glm::dvec3 mn, mx;
  if (m_engine->procBox(m_id, mn, mx)) {
    p.mn = mn;
    p.mx = mx;
  }
  const bool bySize = p.sizing == eng::stairs::Sizing::StepSize;
  {
    QSignalBlocker b1(m_bySize), b2(m_byCount), b3(m_direction), b4(m_nosing), b5(m_thinPlanks), b6(m_collision),
        b7(m_sides);
    m_nosing->setChecked(p.nosing);
    m_sides->setChecked(p.sides);
    m_collision->setChecked(p.collision);
    m_thinPlanks->setChecked(p.thinPlanks);
    m_bySize->setChecked(bySize);
    m_byCount->setChecked(!bySize);
    m_direction->setCurrentIndex(p.direction);
    for (int i = 0; i < 3; ++i) {
      QSignalBlocker b(m_underside[i]);
      m_underside[i]->setChecked(int(p.underside) == i);
    }
  }
  if (!m_name->hasFocus())
    m_name->setText(m_engine->procName(m_id));
  if (!m_height->hasFocus())
    m_height->setValueQuiet(p.stepHeight);
  if (!m_depth->hasFocus())
    m_depth->setValueQuiet(p.stepDepth);
  if (!m_steps->hasFocus())
    m_steps->setValueQuiet(p.steps);
  if (!m_thickness->hasFocus())
    m_thickness->setValueQuiet(p.thickness);
  if (!m_plankThickness->hasFocus())
    m_plankThickness->setValueQuiet(p.plankThickness);
  if (!m_nosingDepth->hasFocus())
    m_nosingDepth->setValueQuiet(p.nosingDepth);
  if (!m_nosingHeight->hasFocus())
    m_nosingHeight->setValueQuiet(p.nosingHeight);
  if (!m_sideWidth->hasFocus())
    m_sideWidth->setValueQuiet(p.sideWidth);
  if (!m_sideHeight->hasFocus())
    m_sideHeight->setValueQuiet(p.sideHeight);
  auto showRow = [](QWidget *w, bool on) {
    w->setVisible(on);
    if (auto *l = qobject_cast<QWidget *>(w->property("label").value<QObject *>()))
      l->setVisible(on);
  };
  showRow(m_sizeRow, bySize);
  showRow(m_countRow, !bySize);
  showRow(m_thicknessRow, p.underside == eng::stairs::Underside::Slope);
  const bool planks = p.underside == eng::stairs::Underside::Planks;
  m_thinPlanks->setVisible(planks);
  showRow(m_plankRow, planks && p.thinPlanks);
  showRow(m_nosingRow, p.nosing);
  showRow(m_sidesRow, p.sides);
  m_fit->setVisible(bySize);

  const int n = eng::stairs::stepCount(p);
  const double su = 10.0;
  const glm::dvec3 e = (p.mx - p.mn) * su;
  const double len = eng::stairs::climbLength(p) * su;
  const double across = (eng::stairs::climbAxis(p.direction) == 0 ? e.z : e.x);
  m_info->setText(tr("%1 steps of %2 × %3 u · box %4 long × %5 wide × %6 high")
                      .arg(n)
                      .arg(e.y / n, 0, 'g', 4)
                      .arg(len / n, 0, 'g', 4)
                      .arg(qRound(len))
                      .arg(qRound(across))
                      .arg(qRound(e.y)));
}

} // namespace ui
