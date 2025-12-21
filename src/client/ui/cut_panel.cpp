#include "cut_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>

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

QWidget *hrow(QWidget *parent, std::initializer_list<QWidget *> widgets) {
  auto *w = new QWidget(parent);
  auto *h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  for (QWidget *x: widgets)
    h->addWidget(x, 1);
  return w;
}

NumberField *number(double lo, double hi, double step, int decimals, const QString &suffix, QWidget *parent) {
  auto *f = new NumberField(parent);
  f->setRange(lo, hi);
  f->setSingleStep(step);
  f->setDecimals(decimals);
  f->setSuffix(suffix);
  return f;
}

void showRow(QWidget *w, bool on) {
  w->setVisible(on);
  if (auto *l = qobject_cast<QWidget *>(w->property("label").value<QObject *>()))
    l->setVisible(on);
}

} // namespace

CutPanel::CutPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
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
    w->setProperty("label", QVariant::fromValue<QObject *>(l));
    ++row;
  };

  const char *shapes[4] = {"Slice", "Door", "Window", "Air vent"};
  const char *shapeTips[4] = {"Split the selection along a line", "Punch a door opening through the selection",
                              "Punch a window opening (sill above the floor)", "Punch a square duct opening"};
  auto *shapeGroup = new QButtonGroup(this);
  for (int i = 0; i < 4; ++i) {
    m_shape[i] = segment(tr(shapes[i]), tr(shapeTips[i]), this);
    shapeGroup->addButton(m_shape[i], i);
  }
  add(tr("Cut"), hrow(this, {m_shape[0], m_shape[1], m_shape[2], m_shape[3]}));
  connect(shapeGroup, &QButtonGroup::idClicked, this, [this](int id) { m_engine->setCutShape(id); });

  m_mode[0] = segment(tr("Remove +"), tr("Remove the part on the positive side of the line (red)"), this);
  m_mode[1] = segment(tr("Remove −"), tr("Remove the part on the negative side of the line (red)"), this);
  m_mode[2] = segment(tr("Keep both"), tr("Split only: both halves stay"), this);
  auto *modeGroup = new QButtonGroup(this);
  for (int i = 0; i < 3; ++i)
    modeGroup->addButton(m_mode[i], i + 1);
  m_modeRow = hrow(this, {m_mode[0], m_mode[1], m_mode[2]});
  add(tr("Keeps"), m_modeRow, tr("Ctrl+X cycles these"));
  connect(modeGroup, &QButtonGroup::idClicked, this, [this](int id) { m_engine->setCutMode(id); });


  m_template = new QComboBox(this);
  add(tr("Preset"), m_template, tr("A starting size; width and height below still override it"));
  connect(m_template, &QComboBox::activated, this, [this](int i) { m_engine->setCutoutTemplate(i); });

  m_width = number(4, 256, 1, 0, QStringLiteral(" u"), this);
  m_height = number(4, 256, 1, 0, QStringLiteral(" u"), this);
  add(tr("Size"), hrow(this, {m_width, new QLabel(QStringLiteral("×"), this), m_height}), tr("Width × height"));
  connect(m_width, &NumberField::committed, this, [this](double v) { m_engine->setCutoutWidthSource(v); });
  connect(m_height, &NumberField::committed, this, [this](double v) { m_engine->setCutoutHeightSource(v); });

  m_sill = number(0.0, 0.6, 0.01, 2, QStringLiteral(" × wall"), this);
  m_sillInfo = new QLabel(this);
  m_sillInfo->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  add(tr("Sill"), hrow(this, {m_sill, m_sillInfo}), tr("Window sill height as a share of the wall height"));
  connect(m_sill, &NumberField::committed, this, [this](double v) { m_engine->setCutoutSillFraction(v); });

  m_autoFill = new QCheckBox(tr("Fill the wall with a row"), this);
  m_autoFill->setToolTip(tr("Lay out as many openings as fit along the wall"));
  grid->addWidget(m_autoFill, row++, 0, 1, 2);
  connect(m_autoFill, &QCheckBox::toggled, this, [this](bool on) { m_engine->setCutoutAutoFill(on); });
  m_gap = number(0, 256, 1, 0, QStringLiteral(" u"), this);
  m_margin = number(0, 256, 1, 0, QStringLiteral(" u"), this);
  add(tr("Gap / edge"), hrow(this, {m_gap, m_margin}), tr("Space between openings / at the wall ends"));
  connect(m_gap, &NumberField::committed, this, [this](double v) { m_engine->setCutoutSpacingSource(v); });
  connect(m_margin, &NumberField::committed, this, [this](double v) { m_engine->setCutoutMarginSource(v); });


  m_apply = new QPushButton(tr("Apply"), this);
  m_apply->setToolTip(tr("Apply the cut (Enter)"));
  m_cancel = new QPushButton(tr("Cancel"), this);
  m_cancel->setToolTip(tr("Leave cut mode (Esc)"));
  grid->addWidget(hrow(this, {m_apply, m_cancel}), row++, 0, 1, 2);
  connect(m_apply, &QPushButton::clicked, this, [this] { m_engine->applyCut(); });
  connect(m_cancel, &QPushButton::clicked, this, [this] { m_engine->exitCutMode(); });

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(30);
  connect(m_timer, &QTimer::timeout, this, &CutPanel::refresh);
  for (auto sig: {&vk::Engine3D::cutModeChanged, &vk::Engine3D::cutShapeChanged, &vk::Engine3D::cutoutSizeChanged,
                  &vk::Engine3D::cutoutTemplateChanged, &vk::Engine3D::cutoutLayoutChanged,
                  &vk::Engine3D::cutHoleChanged, &vk::Engine3D::cutLineChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void CutPanel::refresh() {
  auto *e = m_engine;
  if (!e->cutModeActive())
    return;
  const int shape = e->cutShapeInt();
  const bool slice = shape == 0;
  for (int i = 0; i < 4; ++i) {
    QSignalBlocker b(m_shape[i]);
    m_shape[i]->setChecked(i == shape);
  }
  for (int i = 0; i < 3; ++i) {
    QSignalBlocker b(m_mode[i]);
    m_mode[i]->setChecked(int(e->cutMode()) == i + 1);
  }
  {
    QSignalBlocker b2(m_autoFill), b3(m_template);
    m_autoFill->setChecked(e->cutoutAutoFill());
    m_template->clear();
    m_template->addItems(e->cutoutTemplateNames());
    m_template->setCurrentIndex(e->cutoutTemplate());
  }
  showRow(m_modeRow, slice);
  showRow(m_template, !slice);
  showRow(m_width->parentWidget(), !slice);
  showRow(m_sill->parentWidget(), shape == 2);
  m_autoFill->setVisible(e->autoFillAllowed());
  showRow(m_gap->parentWidget(), e->autoFillAllowed() && e->cutoutAutoFill());
  if (!m_width->hasFocus())
    m_width->setValueQuiet(e->cutoutWidthSource());
  if (!m_height->hasFocus())
    m_height->setValueQuiet(e->cutoutHeightSource());
  if (!m_sill->hasFocus())
    m_sill->setValueQuiet(e->cutoutSillFraction());
  if (!m_gap->hasFocus())
    m_gap->setValueQuiet(e->cutoutSpacingSource());
  if (!m_margin->hasFocus())
    m_margin->setValueQuiet(e->cutoutMarginSource());
  m_sillInfo->setText(tr("= %1 u").arg(qRound(e->cutoutSillSource())));
  m_apply->setEnabled(slice ? e->hasCutLine() : e->hasCutHole());
}

} // namespace ui
