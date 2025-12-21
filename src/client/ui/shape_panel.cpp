#include "shape_panel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>

#include "client/engine/shape_gen.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

QToolButton *segment(const QString &text, QWidget *parent) {
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("shelfButton"));
  b->setText(text);
  b->setCheckable(true);
  b->setFocusPolicy(Qt::NoFocus);
  b->setToolButtonStyle(Qt::ToolButtonTextOnly);
  b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return b;
}

} // namespace

bool ShapePanel::relevant(const vk::Engine3D *engine) {
  if (engine->shapeToolActive())
    return vk::Engine3D::isShapeKind(engine->shapeToolKind());
  if (engine->materialMode())
    return false;
  eng::ShapeKind kind;
  const quint32 id = engine->selectedProcObject();
  return id && eng::shapeKindOf(engine->procType(id), kind);
}

ShapePanel::ShapePanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
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

  auto *axisGroup = new QButtonGroup(this);
  auto *axisRow = new QWidget(this);
  auto *ah = new QHBoxLayout(axisRow);
  ah->setContentsMargins(0, 0, 0, 0);
  ah->setSpacing(4);
  const char *axes[3] = {"X", "Y", "Z"};
  for (int i = 0; i < 3; ++i) {
    m_axis[i] = segment(QString::fromLatin1(axes[i]), axisRow);
    axisGroup->addButton(m_axis[i], i);
    ah->addWidget(m_axis[i], 1);
  }
  m_axisLabel = add(tr("Axis"), axisRow);
  connect(axisGroup, &QButtonGroup::idClicked, this, [this](int id) { change("axis", id); });

  m_sides = new SliderField(3, 64, 0, QString(), 1.0, this);
  add(tr("Sides"), m_sides, tr("Sides around the axis (more = rounder, more faces)"));
  connect(m_sides, &SliderField::committed, this, [this](double v) { change("sides", int(std::lround(v))); });

  m_wall = new NumberField(this);
  m_wall->setRange(0, 4096);
  m_wall->setSingleStep(1);
  m_wall->setDecimals(0);
  m_wall->setSuffix(QStringLiteral(" u"));
  m_wallLabel = add(tr("Wall"), m_wall, tr("0: solid; else a hollow tube with walls this thick"));
  connect(m_wall, &NumberField::committed, this,
          [this](double v) { change("wall", v / vk::Engine3D::kSourceUnitsPerWorldUnit); });

  m_caps = new SliderField(1, 16, 0, QString(), 1.0, this);
  m_capsLabel = add(tr("End rings"), m_caps, tr("Rings in each rounded end"));
  connect(m_caps, &SliderField::committed, this, [this](double v) { change("capSegments", int(std::lround(v))); });

  m_smooth = new QCheckBox(tr("Smooth shading"), this);
  add(QString(), m_smooth, tr("The shape shades as one smooth surface"));
  connect(m_smooth, &QCheckBox::toggled, this, [this](bool on) { change("smooth", on); });

  m_create = new QPushButton(tr("Create"), this);
  m_create->setObjectName(QStringLiteral("dlgPrimary"));
  m_create->setToolTip(tr("Make the brushes (Enter)"));
  auto *cancel = new QPushButton(tr("Cancel"), this);
  cancel->setToolTip(tr("Close the tool without creating anything (Esc)"));
  m_toolButtons = new QWidget(this);
  auto *bh = new QHBoxLayout(m_toolButtons);
  bh->setContentsMargins(0, 0, 0, 0);
  bh->addWidget(m_create, 1);
  bh->addWidget(cancel, 1);
  grid->addWidget(m_toolButtons, row++, 0, 1, 2);
  connect(m_create, &QPushButton::clicked, this, [this] { m_engine->commitShapeTool(); });
  connect(cancel, &QPushButton::clicked, this, [this] { m_engine->cancelShapeTool(); });

  m_dissolve = new QPushButton(tr("Convert to brushes"), this);
  m_dissolve->setToolTip(tr("Make it plain brushes (no longer rebuilt from these settings)"));
  grid->addWidget(m_dissolve, row++, 0, 1, 2);
  connect(m_dissolve, &QPushButton::clicked, this, [this] {
    if (const quint32 id = editedShape())
      m_engine->dissolveProc(id);
  });
}

quint32 ShapePanel::editedShape() const {
  if (m_engine->shapeToolActive() || m_engine->materialMode())
    return 0;
  eng::ShapeKind kind;
  const quint32 id = m_engine->selectedProcObject();
  return id && eng::shapeKindOf(m_engine->procType(id), kind) ? id : 0;
}

void ShapePanel::change(const char *key, const QJsonValue &value) {
  if (const quint32 id = editedShape()) {
    m_engine->setProcParam(id, QString::fromLatin1(key), value);
    return;
  }
  eng::ShapeParams p = m_engine->shapeToolParams();
  const QString k = QString::fromLatin1(key);
  if (k == QLatin1String("axis"))
    p.axis = value.toInt();
  else if (k == QLatin1String("sides"))
    p.sides = value.toInt();
  else if (k == QLatin1String("wall"))
    p.wall = value.toDouble();
  else if (k == QLatin1String("capSegments"))
    p.capSegments = value.toInt();
  else if (k == QLatin1String("smooth"))
    p.smooth = value.toBool();
  m_engine->setShapeToolParams(p);
}

QString ShapePanel::title() const {
  switch (eng::ShapeKind(m_kind)) {
    case eng::ShapeKind::Capsule: return tr("Capsule");
    case eng::ShapeKind::Pyramid: return tr("Pyramid");
    case eng::ShapeKind::Cylinder:
    default: return tr("Cylinder");
  }
}

QString ShapePanel::icon() const {
  switch (eng::ShapeKind(m_kind)) {
    case eng::ShapeKind::Capsule: return QStringLiteral("mesh_capsule");
    case eng::ShapeKind::Pyramid: return QStringLiteral("mesh_cone");
    case eng::ShapeKind::Cylinder:
    default: return QStringLiteral("mesh_cylinder");
  }
}

void ShapePanel::refresh() {
  const quint32 edited = editedShape();
  eng::ShapeParams p;
  glm::dvec3 mn(0.0), mx(0.0);
  int brushes = 0;
  if (edited) {
    eng::shapeKindOf(m_engine->procType(edited), p.kind);
    eng::shapeFromJson(m_engine->procCurrentParams(edited), mn, mx, p);
    brushes = int(m_engine->procMembers(edited).size());
  } else if (m_engine->shapeToolActive() && vk::Engine3D::isShapeKind(m_engine->shapeToolKind())) {
    p = m_engine->shapeToolParams();
    if (m_engine->hasMarquee())
      marqueeToWorld(m_engine->marquee(), mn, mx);
    brushes = m_engine->shapeToolBrushCount();
  } else {
    return;
  }
  m_kind = int(p.kind);
  const double k = vk::Engine3D::kSourceUnitsPerWorldUnit;
  const glm::dvec3 size = (mx - mn) * k;
  m_info->setText(tr("%1 × %2 × %3 u · %n brush(es)", nullptr, brushes)
                      .arg(size.x, 0, 'f', 0)
                      .arg(size.y, 0, 'f', 0)
                      .arg(size.z, 0, 'f', 0));
  for (int i = 0; i < 3; ++i) {
    QSignalBlocker b(m_axis[i]);
    m_axis[i]->setChecked(p.axis == i);
    m_axis[i]->setToolTip(p.kind == eng::ShapeKind::Pyramid ? tr("Point along %1").arg(m_axis[i]->text())
                                                            : tr("Run along %1").arg(m_axis[i]->text()));
  }
  m_sides->setValueQuiet(p.sides);
  const bool cylinder = p.kind == eng::ShapeKind::Cylinder, capsule = p.kind == eng::ShapeKind::Capsule;
  if (!m_wall->hasFocus())
    m_wall->setValueQuiet(p.wall * k);
  m_wallLabel->setVisible(cylinder);
  m_wall->setVisible(cylinder);
  m_caps->setValueQuiet(p.capSegments);
  m_capsLabel->setVisible(capsule);
  m_caps->setVisible(capsule);
  {
    QSignalBlocker b(m_smooth);
    m_smooth->setChecked(p.smooth);
  }
  m_toolButtons->setVisible(!edited);
  m_dissolve->setVisible(edited != 0);
}

} // namespace ui
