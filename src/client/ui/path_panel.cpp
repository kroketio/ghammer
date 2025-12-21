#include "path_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "client/engine/scene.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

constexpr double kUnits = vk::Engine3D::kSourceUnitsPerWorldUnit;

QGridLayout *form(QWidget *w) {
  auto *g = new QGridLayout(w);
  g->setContentsMargins(0, 0, 0, 0);
  g->setHorizontalSpacing(10);
  g->setVerticalSpacing(6);
  g->setColumnStretch(1, 1);
  return g;
}

void row(QGridLayout *g, const QString &label, QWidget *w, const QString &tip = {}) {
  const int r = g->rowCount();
  auto *l = new QLabel(label, w->parentWidget());
  l->setStyleSheet(QStringLiteral("color:#aab4cf;"));
  g->addWidget(l, r, 0);
  g->addWidget(w, r, 1);
  if (!tip.isEmpty()) {
    l->setToolTip(tip);
    w->setToolTip(tip);
  }
}

} // namespace

PathPanel::PathPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);
  {
    auto *w = new QWidget(this);
    auto *g = form(w);
    m_name = new QLineEdit(w);
    row(g, tr("Name"), m_name);
    v->addWidget(w);
  }
  m_info = new QLabel(this);
  m_info->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_info);

  // Drawn paths.
  m_drawn = new QWidget(this);
  {
    auto *dv = new QVBoxLayout(m_drawn);
    dv->setContentsMargins(0, 0, 0, 0);
    m_closed = new QCheckBox(tr("Closed loop"), m_drawn);
    dv->addWidget(m_closed);
    m_pointBox = new QWidget(m_drawn);
    auto *pv = new QVBoxLayout(m_pointBox);
    pv->setContentsMargins(0, 0, 0, 0);
    m_point = new AxisRow(tr("Point"), tr("u"), 1, 16.0, m_pointBox);
    pv->addWidget(m_point);
    auto *tw = new QWidget(m_pointBox);
    auto *tg = form(tw);
    m_pointType = new QComboBox(tw);
    m_pointType->addItems({tr("Auto"), tr("Aligned"), tr("Free"), tr("Vector (corner)")});
    row(tg, tr("Handles"), m_pointType, tr("Auto: smooth by itself; Aligned / Free: drag the handles; Vector: a corner"));
    pv->addWidget(tw);
    dv->addWidget(m_pointBox);
    m_reverse = new QPushButton(tr("Reverse direction"), m_drawn);
    dv->addWidget(m_reverse);
  }
  v->addWidget(m_drawn);

  // Connect paths.
  m_connect = new QWidget(this);
  {
    auto *g = form(m_connect);
    m_anchors = new QLabel(m_connect);
    m_anchors->setWordWrap(true);
    row(g, tr("Anchors"), m_anchors);
    auto *swap = new QPushButton(tr("Swap A and B"), m_connect);
    row(g, QString(), swap);
    connect(swap, &QPushButton::clicked, this, [this] {
      m_engine->editPath(m_path, [](eng::path::Path &p) { std::swap(p.connect.anchorA, p.connect.anchorB); },
                         tr("Swap anchors"));
    });
    m_sharp = new SliderField(0.0, 1.0, 2, QString(), 1.0, m_connect);
    row(g, tr("Turn"), m_sharp, tr("0: a gradual turn using all the room · 1: the tightest turn, at the corner"));
    m_position = new SliderField(-1.0, 1.0, 2, QString(), 1.0, m_connect);
    row(g, tr("Turn position"), m_position, tr("Toward A (−1) or toward B (+1)"));
    auto *sw = new QWidget(m_connect);
    auto *sh = new QHBoxLayout(sw);
    sh->setContentsMargins(0, 0, 0, 0);
    m_straightAuto = new QCheckBox(tr("Auto"), sw);
    m_straight = new NumberField(sw);
    m_straight->setRange(0, 1e6);
    m_straight->setDecimals(0);
    m_straight->setSuffix(tr(" u"));
    sh->addWidget(m_straightAuto);
    sh->addWidget(m_straight, 1);
    row(g, tr("Straight ends"), sw, tr("A straight run at each anchor before the turn (auto: half the corridor width)"));
    m_height = new QComboBox(m_connect);
    m_height->addItems({tr("Ease over the turn"), tr("Ease over the whole"), tr("Linear")});
    row(g, tr("Height change"), m_height, tr("Where a difference in height between the anchors is climbed"));
    m_maxSlope = new NumberField(m_connect);
    m_maxSlope->setRange(1, 85);
    m_maxSlope->setDecimals(0);
    m_maxSlope->setSuffix(QStringLiteral("°"));
    row(g, tr("Max slope"), m_maxSlope);
    auto *rw = new QWidget(m_connect);
    auto *rh = new QHBoxLayout(rw);
    rh->setContentsMargins(0, 0, 0, 0);
    m_radiusAuto = new QCheckBox(tr("Auto"), rw);
    m_minRadius = new NumberField(rw);
    m_minRadius->setRange(0, 1e6);
    m_minRadius->setDecimals(0);
    m_minRadius->setSuffix(tr(" u"));
    rh->addWidget(m_radiusAuto);
    rh->addWidget(m_minRadius, 1);
    row(g, tr("Min radius"), rw, tr("The tightest turn allowed (auto: what the objects following it need)"));
    m_toDrawn = new QPushButton(tr("Convert to drawn path"), m_connect);
    row(g, QString(), m_toDrawn);
  }
  v->addWidget(m_connect);

  m_error = new QLabel(this);
  m_error->setWordWrap(true);
  m_error->setStyleSheet(QStringLiteral("color:#ff6b6b;"));
  v->addWidget(m_error);
  m_users = new QLabel(this);
  m_users->setWordWrap(true);
  m_users->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_users);

  auto conn = [this](const QString &label, const QString &key, std::function<void(eng::path::ConnectParams &)> fn) {
    m_engine->editPath(m_path, [fn](eng::path::Path &p) { fn(p.connect); }, label, key);
  };
  connect(m_name, &QLineEdit::editingFinished, this, [this] { m_engine->renamePath(m_path, m_name->text()); });
  connect(m_closed, &QCheckBox::toggled, this, [this](bool on) {
    m_engine->editPath(m_path, [on](eng::path::Path &p) { p.closed = on && p.points.size() >= 3; },
                       on ? tr("Close path") : tr("Open path"));
  });
  connect(m_point, &AxisRow::committed, this, [this](int axis, double v) {
    const std::set<int> sel = m_engine->selectedPathPoints();
    if (sel.size() != 1)
      return;
    const int i = *sel.begin();
    m_engine->editPath(m_path, [i, axis, v](eng::path::Path &p) {
      if (i < int(p.points.size()))
        p.points[size_t(i)].pos[axis] = v / kUnits;
    }, tr("Move point"));
  });
  connect(m_pointType, &QComboBox::activated, this,
          [this](int i) { m_engine->setPathPointType(eng::path::HandleType(i)); });
  connect(m_reverse, &QPushButton::clicked, this, [this] {
    m_engine->editPath(m_path, [](eng::path::Path &p) {
      std::reverse(p.points.begin(), p.points.end());
      for (auto &q: p.points)
        std::swap(q.in, q.out);
    }, tr("Reverse path"));
  });
  connect(m_sharp, &SliderField::committed, this,
          [conn](double v) { conn(tr("Turn sharpness"), QStringLiteral("sharp"), [v](auto &c) { c.sharpness = v; }); });
  connect(m_position, &SliderField::committed, this,
          [conn](double v) { conn(tr("Turn position"), QStringLiteral("pos"), [v](auto &c) { c.position = v; }); });
  connect(m_straightAuto, &QCheckBox::toggled, this, [this, conn](bool on) {
    const double v = m_straight->value() / kUnits;
    conn(tr("Straight ends"), {}, [on, v](auto &c) { c.straightEnds = on ? -1.0 : v; });
  });
  connect(m_straight, &NumberField::committed, this,
          [conn](double v) { conn(tr("Straight ends"), QStringLiteral("straight"), [v](auto &c) { c.straightEnds = v / kUnits; }); });
  connect(m_height, &QComboBox::activated, this,
          [conn](int i) { conn(tr("Height change"), {}, [i](auto &c) { c.heightMode = i; }); });
  connect(m_maxSlope, &NumberField::committed, this,
          [conn](double v) { conn(tr("Max slope"), QStringLiteral("slope"), [v](auto &c) { c.maxSlopeDeg = v; }); });
  connect(m_radiusAuto, &QCheckBox::toggled, this, [this, conn](bool on) {
    const double v = m_minRadius->value() / kUnits;
    conn(tr("Min radius"), {}, [on, v](auto &c) { c.minRadius = on ? -1.0 : v; });
  });
  connect(m_minRadius, &NumberField::committed, this,
          [conn](double v) { conn(tr("Min radius"), QStringLiteral("radius"), [v](auto &c) { c.minRadius = v / kUnits; }); });
  connect(m_toDrawn, &QPushButton::clicked, this, [this] { m_engine->convertPathToDrawn(m_path); });

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(30);
  connect(m_timer, &QTimer::timeout, this, &PathPanel::refresh);
  for (auto sig: {&vk::Engine3D::pathsChanged, &vk::Engine3D::selectionChanged, &vk::Engine3D::modOutputsChanged,
                  &vk::Engine3D::entitiesChanged, &vk::Engine3D::penChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
}

void PathPanel::refresh() {
  m_path = m_engine->selectedPaths().size() == 1 ? *m_engine->selectedPaths().begin() : 0;
  const eng::path::Path *p = m_engine->path(m_path);
  if (!p)
    return;
  const bool connectKind = p->kind == eng::path::Path::Kind::Connect;
  {
    QSignalBlocker b(m_name);
    if (!m_name->hasFocus())
      m_name->setText(p->name);
  }
  const auto curve = m_engine->pathCurve(m_path);
  QString info;
  if (curve) {
    double at = 0.0;
    const double r = curve->minRadius(at);
    info = tr("Length %1 u").arg(std::lround(curve->length() * kUnits));
    if (r < 1e100)
      info += tr(" · tightest turn %1 u at %2%")
                  .arg(std::lround(r * kUnits))
                  .arg(std::lround(100.0 * at / std::max(1e-9, curve->length())));
  }
  m_info->setText(info);
  m_drawn->setVisible(!connectKind);
  m_connect->setVisible(connectKind);
  if (!connectKind) {
    QSignalBlocker b(m_closed);
    m_closed->setChecked(p->closed);
    const std::set<int> &sel = m_engine->selectedPathPoints();
    m_pointBox->setVisible(sel.size() == 1);
    if (sel.size() == 1 && *sel.begin() < int(p->points.size())) {
      const eng::path::Point &q = p->points[size_t(*sel.begin())];
      const double v[3] = {q.pos.x * kUnits, q.pos.y * kUnits, q.pos.z * kUnits};
      m_point->setValuesQuiet(v);
      QSignalBlocker bt(m_pointType);
      m_pointType->setCurrentIndex(int(q.type));
    }
  } else {
    const eng::path::ConnectParams &c = p->connect;
    auto name = [this](quint32 id) {
      return id && m_engine->entityById(id) ? m_engine->entityDisplayName(id) : tr("(missing)");
    };
    m_anchors->setText(tr("A: %1\nB: %2").arg(name(c.anchorA), name(c.anchorB)));
    m_sharp->setValueQuiet(c.sharpness);
    m_position->setValueQuiet(c.position);
    {
      QSignalBlocker b1(m_straightAuto), b2(m_radiusAuto), b3(m_height);
      m_straightAuto->setChecked(c.straightEnds < 0.0);
      m_radiusAuto->setChecked(c.minRadius < 0.0);
      m_height->setCurrentIndex(c.heightMode);
    }
    if (!m_straight->hasFocus())
      m_straight->setValueQuiet(std::max(0.0, c.straightEnds) * kUnits);
    m_straight->setEnabled(c.straightEnds >= 0.0);
    if (!m_minRadius->hasFocus())
      m_minRadius->setValueQuiet(std::max(0.0, c.minRadius) * kUnits);
    m_minRadius->setEnabled(c.minRadius >= 0.0);
    if (!m_maxSlope->hasFocus())
      m_maxSlope->setValueQuiet(c.maxSlopeDeg);
  }
  const QString err = m_engine->pathError(m_path);
  m_error->setText(err);
  m_error->setVisible(!err.isEmpty());
  QStringList users;
  for (const quint32 h: m_engine->pathUsers(m_path))
    if (const auto *host = m_engine->modHost(h))
      users << host->name;
  m_users->setText(users.isEmpty() ? tr("Not used")
                                   : tr("Followed by: %1").arg(users.join(QStringLiteral(", "))));
}

} // namespace ui
