#include "lut_levels.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

#include "client/engine/color_lut.h"
#include "client/ui/dialog_style.h"
#include "client/ui/lut_window.h"
#include "client/ui/scale.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

using eng::GradeChannel;

namespace {

QPushButton *makeButton(const QString &text, const char *objectName, QWidget *parent) {
  auto *b = new QPushButton(text, parent);
  b->setObjectName(QString::fromLatin1(objectName));
  return b;
}

} // namespace

QColor gradeChannelColor(GradeChannel ch) {
  switch (ch) {
  case GradeChannel::Red:
    return QColor(230, 80, 80);
  case GradeChannel::Green:
    return QColor(90, 200, 100);
  case GradeChannel::Blue:
    return QColor(90, 140, 240);
  default:
    return QColor(200, 205, 215);
  }
}

QString gradeChannelName(GradeChannel ch) {
  switch (ch) {
  case GradeChannel::Lightness:
    return QObject::tr("Lightness");
  case GradeChannel::Rgb:
    return QObject::tr("RGB");
  case GradeChannel::Red:
    return QObject::tr("Red");
  case GradeChannel::Green:
    return QObject::tr("Green");
  case GradeChannel::Blue:
    return QObject::tr("Blue");
  case GradeChannel::Count:
    break;
  }
  return {};
}

void paintHistogram(QPainter &p, const QRectF &r, const eng::grade::Histogram &h, bool log, const QColor &color) {
  double top = 0.0;
  for (double v: h)
    top = std::max(top, v);
  if (top <= 0.0)
    return;
  QPainterPath path;
  path.moveTo(r.left(), r.bottom());
  for (int i = 0; i < 256; ++i) {
    const double v = log ? std::log(h[size_t(i)] + 1.0) / std::log(top + 1.0) : h[size_t(i)] / top;
    const double x0 = r.left() + r.width() * i / 256.0, x1 = r.left() + r.width() * (i + 1) / 256.0;
    const double y = r.bottom() - v * r.height();
    path.lineTo(x0, y);
    path.lineTo(x1, y);
  }
  path.lineTo(r.right(), r.bottom());
  path.closeSubpath();
  QColor fill = color;
  fill.setAlpha(150);
  p.fillPath(path, fill);
}

HistogramView::HistogramView(QWidget *parent) : QWidget(parent) {
  scale::bind(this, [this] { setMinimumHeight(scale::px(130)); });
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HistogramView::setData(const eng::grade::Histogram &h, bool log, const QColor &color) {
  m_hist = h;
  m_log = log;
  m_color = color;
  update();
}

void HistogramView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const QRectF r = QRectF(rect()).adjusted(scale::px(7), 1, -scale::px(7), -1);
  p.fillRect(r, QColor(12, 16, 34));
  paintHistogram(p, r, m_hist, m_log, m_color);
  p.setPen(QColor(60, 68, 96));
  p.drawRect(r);
}

LevelsSlider::LevelsSlider(bool withGamma, QWidget *parent) : QWidget(parent), m_withGamma(withGamma) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFocusPolicy(Qt::ClickFocus);
}

QSize LevelsSlider::sizeHint() const {
  return {scale::px(200), scale::px(26)};
}

void LevelsSlider::setValues(double black, double white, double gamma) {
  m_black = black;
  m_white = white;
  m_gamma = gamma;
  update();
}

double LevelsSlider::xOf(double v) const {
  const double m = scale::px(7);
  return m + v * (width() - 2.0 * m);
}

double LevelsSlider::valueAt(double x) const {
  const double m = scale::px(7);
  return std::clamp((x - m) / std::max(1.0, width() - 2.0 * m), 0.0, 1.0);
}

double LevelsSlider::gammaPos() const {
  return m_black + (m_white - m_black) * std::pow(0.5, m_gamma);
}

void LevelsSlider::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const double barH = scale::px(12);
  const QRectF bar(xOf(0.0), 0.5, xOf(1.0) - xOf(0.0), barH);
  QLinearGradient g(bar.topLeft(), bar.topRight());
  g.setColorAt(0.0, Qt::black);
  g.setColorAt(1.0, Qt::white);
  p.fillRect(bar, g);
  p.setPen(QColor(60, 68, 96));
  p.setBrush(Qt::NoBrush);
  p.drawRect(bar);

  const double hw = scale::px(6), hh = scale::px(10), top = barH + scale::px(2);
  const auto handle = [&](double v, const QColor &fill, const QColor &edge) {
    const double x = xOf(v);
    QPolygonF tri;
    tri << QPointF(x, top) << QPointF(x + hw, top + hh) << QPointF(x - hw, top + hh);
    p.setPen(QPen(edge, 1.2));
    p.setBrush(fill);
    p.drawPolygon(tri);
  };
  handle(m_black, Qt::black, QColor(200, 205, 215));
  if (m_withGamma)
    handle(gammaPos(), QColor(128, 128, 128), QColor(220, 225, 235));
  handle(m_white, Qt::white, QColor(40, 44, 60));
}

void LevelsSlider::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton)
    return;
  const double x = e->position().x();
  struct Cand {
    int handle;
    double pos;
  };
  std::vector<Cand> cands {{0, xOf(m_black)}, {2, xOf(m_white)}};
  if (m_withGamma)
    cands.push_back({1, xOf(gammaPos())});
  double best = 1e9;
  m_handle = -1;
  for (const Cand &c: cands) {
    const double d = std::abs(c.pos - x);
    // Black and white overlap: pick by the side clicked.
    if (d < best - 0.5 || (std::abs(d - best) <= 0.5 && ((c.handle == 2 && x > c.pos) || (c.handle == 0 && x < c.pos)))) {
      best = d;
      m_handle = c.handle;
    }
  }
  if (onPressed)
    onPressed();
  dragTo(x);
}

void LevelsSlider::mouseMoveEvent(QMouseEvent *e) {
  if ((e->buttons() & Qt::LeftButton) && m_handle >= 0)
    dragTo(e->position().x());
}

void LevelsSlider::dragTo(double x) {
  const double v = valueAt(x);
  constexpr double gap = 1.0 / 255.0;
  switch (m_handle) {
  case 0:
    m_black = m_withGamma ? std::min(v, m_white - gap) : v;
    break;
  case 2:
    m_white = m_withGamma ? std::max(v, m_black + gap) : v;
    break;
  case 1: {
    const double t = std::clamp((v - m_black) / std::max(1e-6, m_white - m_black), 0.001, 0.999);
    m_gamma = std::clamp(std::log(t) / std::log(0.5), 0.1, 10.0);
    break;
  }
  default:
    return;
  }
  update();
  if (onChanged)
    onChanged();
}

LutStepWindow::LutStepWindow(LutWindow *owner, vk::Engine3D *engine, const QString &lut, int stepId,
                             const QString &title)
    : QDialog(owner), m_engine(engine), m_owner(owner), m_lut(lut), m_stepId(stepId) {
  setWindowTitle(title);
  setAttribute(Qt::WA_DeleteOnClose, true);
  scale::setScaledStyleSheet(this, ui::dialogStyle());
  resize(scale::initialSize(this, {460, 700}));

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto *page = new QWidget(this);
  page->setObjectName(QStringLiteral("dlgPage"));
  auto *root = new QHBoxLayout(page);
  root->setContentsMargins(20, 18, 20, 18);
  root->setSpacing(18);
  outer->addWidget(page, 1);

  auto *col = new QWidget(page);
  scale::bind(col, [col] { col->setMinimumWidth(scale::tpx(420)); });
  m_controls = new QVBoxLayout(col);
  m_controls->setContentsMargins(0, 0, 0, 0);
  m_controls->setSpacing(8);
  root->addWidget(col, 1);

  auto *footer = new QFrame(this);
  footer->setObjectName(QStringLiteral("dlgFooter"));
  auto *fl = new QHBoxLayout(footer);
  fl->setContentsMargins(16, 10, 16, 10);
  auto *close = makeButton(tr("Close"), "dlgPrimary", footer);
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  fl->addStretch(1);
  fl->addWidget(close);
  outer->addWidget(footer);
}

const eng::GradeStep *LutStepWindow::step() const {
  return m_owner ? m_owner->gradeStep(m_lut, m_stepId) : nullptr;
}

void LutStepWindow::edit(const QString &key, const std::function<void(eng::GradeStep &)> &fn) {
  if (!m_owner)
    return;
  m_self = true;
  m_owner->editStep(m_lut, m_stepId, key, fn);
  m_self = false;
}

void LutStepWindow::sync() {
  const eng::GradeStep *s = step();
  if (!s) {
    close();
    return;
  }
  if (!m_self)
    load(*s);
}

const QImage &GradeInputCache::picture(const eng::LutGrade &g, int stepId) {
  const int idx = g.indexOf(stepId);
  const std::vector<eng::GradeStep> before(g.steps.begin(), g.steps.begin() + std::max(0, idx));
  if (!m_valid || before != m_steps) {
    static const QImage small = lutPreviewPicture().scaledToWidth(360, Qt::SmoothTransformation)
                                    .convertToFormat(QImage::Format_RGB888);
    eng::LutGrade partial;
    partial.steps = before;
    m_picture = before.empty() ? small
                               : eng::lut::applyToImage(eng::grade::apply(eng::lut::templateStrip(), partial), small);
    m_steps = before;
    m_valid = true;
    m_hists = {};
  }
  return m_picture;
}

eng::grade::Histogram GradeInputCache::histogram(const eng::LutGrade &g, int stepId, GradeChannel ch) {
  const QImage &pic = picture(g, stepId);
  auto &h = m_hists[size_t(ch)];
  if (!h)
    h = eng::grade::histogram(pic, ch);
  return *h;
}

eng::grade::Histogram LutStepWindow::inputHistogram(GradeChannel ch) {
  const eng::LutGrade *g = m_owner ? m_owner->grade(m_lut) : nullptr;
  return g ? m_input.histogram(*g, m_stepId, ch) : eng::grade::Histogram {};
}

void LutStepWindow::addChannelRow() {
  auto *row = new QHBoxLayout;
  row->setSpacing(6);
  row->addWidget(new QLabel(tr("Channel"), this));
  m_channel = new QComboBox(this);
  for (int c = 0; c < eng::kGradeChannels; ++c)
    m_channel->addItem(gradeChannelName(GradeChannel(c)));
  row->addWidget(m_channel, 1);
  auto *group = new QButtonGroup(this);
  const auto toggle = [&](const QString &text) {
    auto *b = new QToolButton(this);
    b->setObjectName(QStringLiteral("dlgTool"));
    b->setText(text);
    b->setCheckable(true);
    group->addButton(b);
    row->addWidget(b);
    return b;
  };
  m_lin = toggle(tr("Linear"));
  m_log = toggle(tr("Log"));
  m_lin->setChecked(true);
  m_controls->addLayout(row);
  connect(m_channel, &QComboBox::currentIndexChanged, this, [this] {
    if (const eng::GradeStep *s = step())
      load(*s);
  });
  connect(group, &QButtonGroup::buttonClicked, this, [this] {
    if (const eng::GradeStep *s = step())
      load(*s);
  });
}

GradeChannel LutStepWindow::channel() const {
  return m_channel ? GradeChannel(std::max(0, m_channel->currentIndex())) : GradeChannel::Lightness;
}

bool LutStepWindow::logHistogram() const {
  return m_log && m_log->isChecked();
}

} // namespace ui
