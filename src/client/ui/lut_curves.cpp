#include "lut_curves.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

#include "client/ui/scale.h"

namespace ui {

using eng::GradeChannel;

CurveEditor::CurveEditor(QWidget *parent) : QWidget(parent) {
  setFocusPolicy(Qt::ClickFocus);
  setMouseTracking(false);
  scale::bind(this, [this] { setMinimumSize(scale::px(320), scale::px(320)); });
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void CurveEditor::setPoints(const eng::CurvePoints &pts) {
  if (m_dragging)
    return;
  m_points = pts;
  if (m_selected >= int(m_points.size()))
    m_selected = -1;
  update();
}

void CurveEditor::setHistogram(const eng::grade::Histogram &h, bool log, const QColor &color) {
  m_hist = h;
  m_log = log;
  m_color = color;
  update();
}

QRectF CurveEditor::area() const {
  const double pad = scale::px(8);
  const double side = std::min(width(), height()) - 2.0 * pad;
  return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

QPointF CurveEditor::toScreen(const glm::dvec2 &p) const {
  const QRectF a = area();
  return {a.left() + p.x * a.width(), a.bottom() - p.y * a.height()};
}

glm::dvec2 CurveEditor::toCurve(const QPointF &p) const {
  const QRectF a = area();
  return {(p.x() - a.left()) / a.width(), (a.bottom() - p.y()) / a.height()};
}

eng::CurvePoints CurveEditor::shown() const {
  if (!m_removed || m_selected < 0)
    return m_points;
  eng::CurvePoints pts = m_points;
  pts.erase(pts.begin() + m_selected);
  return pts;
}

void CurveEditor::emitChanged() {
  update();
  if (onChanged)
    onChanged(shown());
}

void CurveEditor::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF a = area();
  p.fillRect(a, QColor(12, 16, 34));
  paintHistogram(p, a, m_hist, m_log, QColor(m_color.red(), m_color.green(), m_color.blue(), 90));
  p.setPen(QPen(QColor(48, 56, 82), 1));
  for (int i = 1; i < 4; ++i) {
    p.drawLine(QPointF(a.left() + a.width() * i / 4.0, a.top()), QPointF(a.left() + a.width() * i / 4.0, a.bottom()));
    p.drawLine(QPointF(a.left(), a.top() + a.height() * i / 4.0), QPointF(a.right(), a.top() + a.height() * i / 4.0));
  }
  p.setPen(QPen(QColor(80, 88, 116), 1, Qt::DashLine));
  p.drawLine(a.bottomLeft(), a.topRight());
  p.setPen(QColor(60, 68, 96));
  p.setBrush(Qt::NoBrush);
  p.drawRect(a);

  const eng::CurvePoints pts = shown();
  QPainterPath path;
  const int samples = std::max(64, int(a.width()));
  for (int i = 0; i <= samples; ++i) {
    const double x = double(i) / samples;
    const QPointF s = toScreen({x, eng::curveValue(pts, x)});
    if (i == 0)
      path.moveTo(s);
    else
      path.lineTo(s);
  }
  p.setPen(QPen(m_color.isValid() ? m_color : QColor(220, 225, 235), scale::px(2)));
  p.drawPath(path);

  const double r = scale::px(4);
  for (int i = 0; i < int(m_points.size()); ++i) {
    if (m_removed && i == m_selected)
      continue;
    const QPointF s = toScreen(m_points[size_t(i)]);
    p.setPen(QPen(QColor(230, 235, 245), 1.2));
    p.setBrush(i == m_selected ? QColor(230, 235, 245) : QColor(12, 16, 34));
    p.drawRect(QRectF(s.x() - r, s.y() - r, 2 * r, 2 * r));
  }
}

void CurveEditor::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton)
    return;
  const QPointF pos = e->position();
  const double grab = scale::px(8);
  m_selected = -1;
  double best = grab;
  for (int i = 0; i < int(m_points.size()); ++i) {
    const QPointF s = toScreen(m_points[size_t(i)]);
    const double d = std::hypot(s.x() - pos.x(), s.y() - pos.y());
    if (d <= best) {
      best = d;
      m_selected = i;
    }
  }
  m_before = m_points;
  if (onPressed)
    onPressed();
  if (m_selected < 0) {
    glm::dvec2 c = glm::clamp(toCurve(pos), glm::dvec2(0.0), glm::dvec2(1.0));
    const auto it = std::upper_bound(m_points.begin(), m_points.end(), c.x,
                                     [](double x, const glm::dvec2 &q) { return x < q.x; });
    const int idx = int(it - m_points.begin());
    if (idx > 0 && c.x - m_points[size_t(idx - 1)].x < 0.002)
      return;
    if (idx < int(m_points.size()) && m_points[size_t(idx)].x - c.x < 0.002)
      return;
    m_points.insert(m_points.begin() + idx, c);
    m_selected = idx;
    m_dragging = true;
    emitChanged();
    return;
  }
  m_dragging = true;
  update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent *e) {
  if (!m_dragging || m_selected < 0)
    return;
  const QPointF pos = e->position();
  const int n = int(m_points.size());
  const bool end = m_selected == 0 || m_selected == n - 1;
  // Dragged far outside: removed (ends always stay).
  const QRectF far = area().adjusted(-scale::px(20), -scale::px(20), scale::px(20), scale::px(20));
  const bool out = !far.contains(pos) && !end && n > 2;
  glm::dvec2 c = toCurve(pos);
  constexpr double gap = 0.001;
  const double lo = m_selected > 0 ? m_points[size_t(m_selected - 1)].x + gap : 0.0;
  const double hi = m_selected < n - 1 ? m_points[size_t(m_selected + 1)].x - gap : 1.0;
  c.x = std::clamp(c.x, lo, std::max(lo, hi));
  c.y = std::clamp(c.y, 0.0, 1.0);
  m_points[size_t(m_selected)] = c;
  m_removed = out;
  emitChanged();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *) {
  if (!m_dragging)
    return;
  m_dragging = false;
  if (m_removed) {
    m_points.erase(m_points.begin() + m_selected);
    m_selected = -1;
    m_removed = false;
  }
  update();
}

void CurveEditor::keyPressEvent(QKeyEvent *e) {
  const int n = int(m_points.size());
  if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m_selected > 0 && m_selected < n - 1 &&
      !m_dragging) {
    if (onPressed)
      onPressed();
    m_points.erase(m_points.begin() + m_selected);
    m_selected = -1;
    emitChanged();
    return;
  }
  if (e->key() == Qt::Key_Escape && m_dragging) {
    m_dragging = false;
    m_removed = false;
    m_points = m_before;
    m_selected = -1;
    emitChanged();
    return;
  }
  QWidget::keyPressEvent(e);
}

CurvesWindow::CurvesWindow(LutWindow *owner, vk::Engine3D *engine, const QString &lut, int stepId,
                           const QString &title)
    : LutStepWindow(owner, engine, lut, stepId, title) {
  addChannelRow();
  m_curve = new CurveEditor(this);
  m_controls->addWidget(m_curve, 1);
  auto *buttons = new QHBoxLayout;
  auto *resetCh = new QPushButton(tr("Reset channel"), this);
  resetCh->setObjectName(QStringLiteral("dlgFlat"));
  auto *resetAll = new QPushButton(tr("Reset all"), this);
  resetAll->setObjectName(QStringLiteral("dlgFlat"));
  buttons->addStretch(1);
  buttons->addWidget(resetCh);
  buttons->addWidget(resetAll);
  m_controls->addLayout(buttons);

  m_curve->onPressed = [this] { ++m_drag; };
  m_curve->onChanged = [this](const eng::CurvePoints &pts) {
    const int ch = int(channel());
    edit(QStringLiteral("curves.%1.%2").arg(ch).arg(m_drag),
         [&](eng::GradeStep &s) { s.curves.ch[size_t(ch)] = pts; });
  };
  connect(resetCh, &QPushButton::clicked, this, [this] {
    const int ch = int(channel());
    edit(QStringLiteral("curves.reset.%1").arg(++m_drag),
         [ch](eng::GradeStep &s) { s.curves.ch[size_t(ch)] = eng::identityCurve(); });
    if (const eng::GradeStep *s = step())
      load(*s);
  });
  connect(resetAll, &QPushButton::clicked, this, [this] {
    edit(QStringLiteral("curves.resetAll.%1").arg(++m_drag), [](eng::GradeStep &s) { s.curves = {}; });
    if (const eng::GradeStep *s = step())
      load(*s);
  });

  if (const eng::GradeStep *s = step())
    load(*s);
}

void CurvesWindow::load(const eng::GradeStep &s) {
  const GradeChannel ch = channel();
  m_curve->setHistogram(inputHistogram(ch), logHistogram(), gradeChannelColor(ch));
  m_curve->setPoints(s.curves.ch[size_t(ch)]);
}

} // namespace ui
