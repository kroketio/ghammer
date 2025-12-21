#include "bevel_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>

#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {
constexpr double kSrc = vk::Engine3D::kSourceUnitsPerWorldUnit;
}

BevelPanel::BevelPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(7);
  grid->setColumnStretch(1, 1);
  int row = 0;
  auto add = [&](const QString &label, QWidget *w, const QString &tip = {}) {
    auto *l = new QLabel(label, this);
    l->setToolTip(tip);
    w->setToolTip(tip);
    grid->addWidget(l, row, 0);
    grid->addWidget(w, row, 1);
    ++row;
  };

  // Presets: radius and segments.
  {
    struct Preset {
      const char *name;
      double radius;
      int segments;
    };
    static const Preset kPresets[] = {{"Tiny", 4.0, 3}, {"Small", 8.0, 4}, {"Normal", 16.0, 4}, {"Big", 32.0, 8}};
    auto *presets = new QWidget(this);
    auto *ph = new QHBoxLayout(presets);
    ph->setContentsMargins(0, 0, 0, 0);
    ph->setSpacing(4);
    for (const Preset &p: kPresets) {
      auto *b = new QPushButton(tr(p.name), presets);
      b->setToolTip(tr("Radius %1 u, %n segment(s)", "", p.segments).arg(p.radius));
      connect(b, &QPushButton::clicked, this, [this, p] {
        m_engine->setBevelRadius(p.radius / kSrc);
        m_engine->setBevelSegments(p.segments);
      });
      ph->addWidget(b, 1);
    }
    grid->addWidget(presets, row++, 0, 1, 2);
  }
  m_radius = new NumberField(this);
  m_radius->setRange(0.25, 16384.0);
  m_radius->setSingleStep(8.0);
  m_radius->setDecimals(2);
  m_radius->setSuffix(tr(" u"));
  add(tr("Radius"), m_radius, tr("Size of the round, in Source units (limited by the faces)"));
  m_segments = new NumberField(this);
  m_segments->setRange(1, 32);
  m_segments->setSingleStep(1);
  m_segments->setDecimals(0);
  add(tr("Segments"), m_segments, tr("1 is a chamfer"));
  m_inverse = new QCheckBox(tr("Inverse"), this);
  add(QString(), m_inverse, tr("A concave cove, or an inner-corner fill against a brush (B)"));
  m_smooth = new QCheckBox(tr("Smooth shading"), this);
  add(QString(), m_smooth, tr("The bevelled brushes shade their round as one smooth surface"));
  m_newFaces = new QComboBox(this);
  m_newFaces->addItem(tr("Edge faces"));
  m_newFaces->addItem(tr("Current material"));
  add(tr("New faces"), m_newFaces, tr("The texture of the round: the faces around the edge, or the current material"));
  m_limit = new QLabel(this);
  add(tr("Largest radius"), m_limit);
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  grid->addWidget(m_status, row++, 0, 1, 2);

  auto *buttons = new QWidget(this);
  auto *h = new QHBoxLayout(buttons);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  m_go = new QPushButton(tr("Apply"), this);
  m_cancel = new QPushButton(tr("Cancel"), this);
  m_clear = new QPushButton(tr("Clear Edges"), this);
  h->addWidget(m_go, 1);
  h->addWidget(m_cancel, 1);
  h->addWidget(m_clear, 1);
  grid->addWidget(buttons, row++, 0, 1, 2);

  connect(m_radius, &NumberField::committed, this, [this](double v) { m_engine->setBevelRadius(v / kSrc); });
  connect(m_segments, &NumberField::committed, this, [this](double v) { m_engine->setBevelSegments(int(std::lround(v))); });
  connect(m_inverse, &QCheckBox::toggled, this, [this](bool on) { m_engine->setBevelInverse(on); });
  connect(m_smooth, &QCheckBox::toggled, this, [this](bool on) { m_engine->setBevelSmooth(on); });
  connect(m_newFaces, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int i) { m_engine->setBevelCurrentMaterial(i == 1); });
  m_go->setToolTip(tr("Bevel the picked edges and leave bevel mode (Enter)"));
  m_cancel->setToolTip(tr("Leave bevel mode without bevelling (Esc)"));
  connect(m_go, &QPushButton::clicked, this, [this] { m_engine->applyBevel(); });
  connect(m_cancel, &QPushButton::clicked, this, [this] { m_engine->leaveBevelMode(); });
  connect(m_clear, &QPushButton::clicked, this, [this] { m_engine->clearBevelEdges(); });

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(30);
  connect(m_timer, &QTimer::timeout, this, &BevelPanel::refresh);
  connect(m_engine, &vk::Engine3D::bevelChanged, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void BevelPanel::refresh() {
  m_radius->setValueQuiet(m_engine->bevelRadius() * kSrc);
  m_segments->setValueQuiet(m_engine->bevelSegments());
  {
    const QSignalBlocker b1(m_inverse), b2(m_newFaces), b3(m_smooth);
    m_inverse->setChecked(m_engine->bevelInverse());
    m_smooth->setChecked(m_engine->bevelSmooth());
    m_newFaces->setCurrentIndex(m_engine->bevelCurrentMaterial() ? 1 : 0);
  }
  const int picked = m_engine->bevelPickedCount();
  bool error = false;
  const QString status = picked > 0 ? m_engine->bevelStatus(&error) : QString();
  const double lim = picked > 0 ? m_engine->bevelLimit() : 0.0;
  m_limit->setText(lim > 0.0 ? tr("%1 u").arg(lim * kSrc, 0, 'g', 5) : QStringLiteral("–"));
  m_status->setText(picked == 0 ? tr("No edge picked") : tr("%n edge(s) picked", "", picked) + QStringLiteral(" · ") + status);
  m_status->setStyleSheet(error ? QStringLiteral("color: #f07a7a;") : QString());
  m_go->setEnabled(picked > 0 && !error);
  m_clear->setEnabled(picked > 0);
}

} // namespace ui
