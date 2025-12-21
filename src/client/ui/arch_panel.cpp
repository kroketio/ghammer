#include "arch_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>

#include "client/engine/arch_gen.h"
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

NumberField *units(double lo, double hi, QWidget *parent) {
  auto *f = new NumberField(parent);
  f->setRange(lo, hi);
  f->setSingleStep(1);
  f->setDecimals(0);
  f->setSuffix(QStringLiteral(" u"));
  return f;
}

} // namespace

ArchPanel::ArchPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(7);
  grid->setColumnStretch(1, 1);
  int row = 0;
  auto add = [&](const QString &label, QWidget *w, const QString &tip = {}) {
    auto *l = new QLabel(label, this);
    l->setToolTip(tip);
    if (!tip.isEmpty())
      w->setToolTip(tip);
    grid->addWidget(l, row, 0);
    grid->addWidget(w, row, 1);
    ++row;
    return l;
  };

  m_info = new QLabel(this);
  m_info->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  grid->addWidget(m_info, row++, 0, 1, 2);

  const char *modes[3] = {"Ring", "Archway", "Carve"};
  const char *modeTips[3] = {"The arch ring (voussoirs) with jamb posts below the spring line",
                             "A wall filling the box with an arched opening",
                             "Cut an arched opening out of the selected walls (or every wall it touches)"};
  auto *modeGroup = new QButtonGroup(this);
  auto *modeRow = new QWidget(this);
  auto *mh = new QHBoxLayout(modeRow);
  mh->setContentsMargins(0, 0, 0, 0);
  mh->setSpacing(4);
  for (int i = 0; i < 3; ++i) {
    m_mode[i] = segment(tr(modes[i]), tr(modeTips[i]), modeRow);
    modeGroup->addButton(m_mode[i], i);
    mh->addWidget(m_mode[i], 1);
  }
  add(tr("Kind"), modeRow);
  connect(modeGroup, &QButtonGroup::idClicked, this, [this](int id) {
    if (editedArch())
      setParam("mode", id);
    else
      m_engine->setArchMode(id);
  });

  m_profile = new QComboBox(this);
  m_profile->addItems({tr("Round"), tr("Segmental"), tr("Pointed"), tr("Elliptical"), tr("Tudor")});
  add(tr("Profile"), m_profile, tr("The shape of the arch's curve"));
  connect(m_profile, &QComboBox::activated, this, [this](int i) {
    if (editedArch())
      setParam("profile", i);
    else
      m_engine->setArchProfile(i);
  });

  m_segments = new SliderField(2, 32, 0, QString(), 1.0, this);
  add(tr("Segments"), m_segments, tr("Pieces along the curve (more = smoother, more brushes)"));
  connect(m_segments, &SliderField::committed, this, [this](double v) {
    if (editedArch())
      setParam("segments", int(std::lround(v)));
    else
      m_engine->setArchSegments(int(std::lround(v)));
  });

  m_thickness = units(0, 128, this);
  m_thicknessLabel = add(tr("Ring depth"), m_thickness);
  connect(m_thickness, &NumberField::committed, this, [this](double v) {
    if (editedArch())
      setParam("thickness", v / vk::Engine3D::kSourceUnitsPerWorldUnit);
    else
      m_engine->setArchThicknessSource(v);
  });

  m_spring = units(0, 256, this);
  add(tr("Spring height"), m_spring, tr("Height of the straight jambs below where the curve starts"));
  connect(m_spring, &NumberField::committed, this, [this](double v) {
    if (editedArch())
      setParam("spring", v / vk::Engine3D::kSourceUnitsPerWorldUnit);
    else
      m_engine->setArchSpringSource(v);
  });

  m_rise = new SliderField(0.05, 1.0, 2, QString(), 1.0, this);
  m_riseLabel = add(tr("Rise"), m_rise, tr("How high a segmental arch rises, as a fraction of half its width"));
  connect(m_rise, &SliderField::committed, this, [this](double v) {
    if (editedArch())
      setParam("rise", v);
    else
      m_engine->setArchRise(v);
  });

  m_smooth = new QCheckBox(tr("Smooth shading"), this);
  add(QString(), m_smooth, tr("The arch's curve shades as one smooth surface"));
  connect(m_smooth, &QCheckBox::toggled, this, [this](bool on) {
    if (editedArch())
      setParam("smooth", on);
    else
      m_engine->setArchSmooth(on);
  });

  m_create = new QPushButton(tr("Create"), this);
  m_create->setObjectName(QStringLiteral("dlgPrimary"));
  auto *cancel = new QPushButton(tr("Cancel"), this);
  cancel->setToolTip(tr("Close the arch tool without creating anything (Esc)"));
  m_toolButtons = new QWidget(this);
  auto *bh = new QHBoxLayout(m_toolButtons);
  bh->setContentsMargins(0, 0, 0, 0);
  bh->addWidget(m_create, 1);
  bh->addWidget(cancel, 1);
  grid->addWidget(m_toolButtons, row++, 0, 1, 2);
  connect(m_create, &QPushButton::clicked, this, [this] { m_engine->commitShapeTool(); });
  connect(cancel, &QPushButton::clicked, this, [this] { m_engine->cancelShapeTool(); });


  m_dissolve = new QPushButton(tr("Convert to brushes"), this);
  m_dissolve->setToolTip(tr("Make the arch plain brushes (it is no longer rebuilt from these settings)"));
  grid->addWidget(m_dissolve, row++, 0, 1, 2);
  connect(m_dissolve, &QPushButton::clicked, this, [this] {
    if (const quint32 id = editedArch())
      m_engine->dissolveProc(id);
  });
}

quint32 ArchPanel::editedArch() const {
  if (m_engine->shapeToolActive() || m_engine->materialMode())
    return 0;
  const quint32 id = m_engine->selectedProcObject();
  return id && m_engine->procType(id) == QLatin1String("arch") ? id : 0;
}

void ArchPanel::setParam(const char *key, const QJsonValue &value) {
  if (const quint32 id = editedArch())
    m_engine->setProcParam(id, QString::fromLatin1(key), value);
}

void ArchPanel::refresh() {
  const quint32 edited = editedArch();
  const bool tool = m_engine->shapeToolActive() && m_engine->shapeToolKind() == 7;
  if (!tool && !edited)
    return;
  int mode = m_engine->archMode(), profile = m_engine->archProfile(), segments = m_engine->archSegments();
  double thickness = m_engine->archThicknessSource(), spring = m_engine->archSpringSource(), rise = m_engine->archRise();
  bool smooth = m_engine->archSmooth();
  double width = m_engine->archWidthSource(), height = m_engine->archHeightSource();
  int brushes = m_engine->shapeToolBrushCount();
  if (edited) {
    eng::ArchPlacement pl;
    eng::ArchParams ap;
    eng::archFromJson(m_engine->procCurrentParams(edited), pl, ap);
    const double k = vk::Engine3D::kSourceUnitsPerWorldUnit;
    mode = int(ap.mode);
    profile = int(ap.profile);
    segments = ap.segments;
    thickness = ap.thickness * k;
    spring = ap.spring * k;
    rise = ap.rise;
    smooth = ap.smooth;
    width = (pl.mx[pl.spanAxis] - pl.mn[pl.spanAxis]) * k;
    height = (pl.mx.y - pl.mn.y) * k;
    brushes = int(m_engine->procMembers(edited).size());
  }
  m_info->setText(tr("%1 × %2 u · %n brush(es)", nullptr, brushes).arg(width, 0, 'f', 0).arg(height, 0, 'f', 0));
  // Carving is a one-off edit: only while creating.
  m_mode[2]->setVisible(!edited);
  m_toolButtons->setVisible(!edited);
  m_dissolve->setVisible(edited != 0);
  for (int i = 0; i < 3; ++i) {
    QSignalBlocker b(m_mode[i]);
    m_mode[i]->setChecked(mode == i);
  }
  {
    QSignalBlocker b(m_profile);
    m_profile->setCurrentIndex(profile);
  }
  m_segments->setValueQuiet(segments);
  if (!m_thickness->hasFocus())
    m_thickness->setValueQuiet(thickness);
  if (!m_spring->hasFocus())
    m_spring->setValueQuiet(spring);
  m_rise->setValueQuiet(rise);
  {
    QSignalBlocker b(m_smooth);
    m_smooth->setChecked(smooth);
  }
  m_smooth->setVisible(mode != 2);
  m_thicknessLabel->setText(mode == 0 ? tr("Ring depth") : tr("Jamb / crown"));
  m_thickness->setRange(mode == 0 ? 2 : 0, 128);
  m_thicknessLabel->setVisible(mode != 2);
  m_thickness->setVisible(mode != 2);
  m_riseLabel->setVisible(profile == 1);
  m_rise->setVisible(profile == 1);
  m_create->setText(mode == 2 ? tr("Carve") : tr("Create"));
  m_create->setToolTip(mode == 2 ? tr("Cut the opening (Enter)") : tr("Make the arch brushes (Enter)"));
}

} // namespace ui
