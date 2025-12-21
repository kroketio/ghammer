#include "uv_editor.h"
#include "client/ui/scale.h"

#include <cmath>

#include <QFileInfo>
#include <QGridLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <glm/gtx/rotate_vector.hpp>

#include "client/engine/material.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {
glm::dvec2 uvOf(const eng::FaceTexBasis &b, const glm::vec3 &p) {
  return {double(glm::dot(p, b.uAxis)) * b.tilesPerUnitU + b.offsetU,
          double(glm::dot(p, b.vAxis)) * b.tilesPerUnitV + b.offsetV};
}
glm::vec3 centreOf(const std::vector<glm::vec3> &poly) {
  glm::vec3 c(0.0f);
  for (const auto &p: poly)
    c += p;
  return poly.empty() ? c : c / float(poly.size());
}
} // namespace

UvCanvas::UvCanvas(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  scale::bind(this, [this] { setMinimumSize(scale::px(200), scale::px(200)); });
  setMouseTracking(false);
  setAttribute(Qt::WA_OpaquePaintEvent);
}

void UvCanvas::refresh() {
  const auto v = m_engine ? m_engine->primaryFace() : vk::Engine3D::FaceView {};
  const quint64 key = v.valid ? (quint64(v.object) << 8) ^ quint64(v.face) : 0;
  if (key != m_fittedFor && m_drag == Drag::None) {
    m_fittedFor = key;
    fit();
  }
  update();
}

QPointF UvCanvas::toScreen(const glm::dvec2 &uv) const {
  return {(uv.x - m_center.x) * m_zoom + width() * 0.5, (uv.y - m_center.y) * m_zoom + height() * 0.5};
}

glm::dvec2 UvCanvas::toUv(const QPointF &px) const {
  return {m_center.x + (px.x() - width() * 0.5) / m_zoom, m_center.y + (px.y() - height() * 0.5) / m_zoom};
}

void UvCanvas::fit() {
  const auto v = m_engine ? m_engine->primaryFace() : vk::Engine3D::FaceView {};
  if (!v.valid)
    return;
  glm::dvec2 lo(1e30), hi(-1e30);
  for (const auto &p: v.poly) {
    const glm::dvec2 q = uvOf(v.basis, p);
    lo = glm::min(lo, q);
    hi = glm::max(hi, q);
  }
  m_center = (lo + hi) * 0.5;
  const double span = std::max({hi.x - lo.x, hi.y - lo.y, 0.25});
  m_zoom = std::clamp(std::min(width(), height()) * 0.7 / span, 8.0, 4000.0);
}

QImage UvCanvas::textureFor(const QString &name, const QString &path) {
  const QString key = name + QLatin1Char('|') + path;
  auto it = m_images.find(key);
  if (it != m_images.end())
    return *it;
  QImage img;
  if (!path.isEmpty() && QFileInfo::exists(path)) {
    QImageReader r(path);
    r.setScaledSize(r.size().scaled(256, 256, Qt::KeepAspectRatio));
    img = r.read();
  }
  if (img.isNull()) {
    // No image on disk: a checker.
    img = QImage(64, 64, QImage::Format_RGB32);
    QPainter p(&img);
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x)
        p.fillRect(x * 8, y * 8, 8, 8, ((x + y) & 1) ? QColor(70, 78, 110) : QColor(110, 120, 160));
    p.fillRect(0, 0, 16, 16, QColor(200, 90, 90)); // marks the texture's origin corner
  }
  m_images.insert(key, img);
  return img;
}

void UvCanvas::resizeEvent(QResizeEvent *) {
  if (m_fittedFor)
    fit();
}

void UvCanvas::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor(16, 20, 40));
  const auto v = m_engine ? m_engine->primaryFace() : vk::Engine3D::FaceView {};
  if (!v.valid) {
    p.setPen(QColor(138, 154, 184));
    p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
               tr("Nothing selected"));
    return;
  }
  const QImage img = textureFor(v.material ? v.material->name : QString(), v.material ? v.material->libAlbedo : QString());
  const glm::dvec2 lo = toUv({0, 0}), hi = toUv({double(width()), double(height())});
  p.setOpacity(0.55);
  for (int ty = int(std::floor(lo.y)); ty <= int(std::ceil(hi.y)); ++ty)
    for (int tx = int(std::floor(lo.x)); tx <= int(std::ceil(hi.x)); ++tx)
      p.drawImage(QRectF(toScreen({double(tx), double(ty)}), toScreen({double(tx + 1), double(ty + 1)})), img);
  p.setOpacity(1.0);
  // Tile grid: quarters faint, whole tiles bright.
  for (int pass = 0; pass < 2; ++pass) {
    const double step = pass == 0 ? 0.25 : 1.0;
    if (step * m_zoom < 12)
      continue;
    p.setPen(QPen(pass == 0 ? QColor(255, 255, 255, 30) : QColor(255, 255, 255, 90), 1));
    for (double x = std::floor(lo.x / step) * step; x <= hi.x; x += step) {
      const double sx = toScreen({x, 0}).x();
      p.drawLine(QPointF(sx, 0), QPointF(sx, height()));
    }
    for (double y = std::floor(lo.y / step) * step; y <= hi.y; y += step) {
      const double sy = toScreen({0, y}).y();
      p.drawLine(QPointF(0, sy), QPointF(width(), sy));
    }
  }
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(160, 180, 230, 120), 1.0, Qt::DashLine));
  for (const auto &poly: v.others) {
    QPainterPath path;
    for (size_t i = 0; i < poly.size(); ++i) {
      const QPointF q = toScreen(uvOf(v.basis, poly[i]));
      i == 0 ? path.moveTo(q) : path.lineTo(q);
    }
    path.closeSubpath();
    p.drawPath(path);
  }
  QPainterPath face;
  for (size_t i = 0; i < v.poly.size(); ++i) {
    const QPointF q = toScreen(uvOf(v.basis, v.poly[i]));
    i == 0 ? face.moveTo(q) : face.lineTo(q);
  }
  face.closeSubpath();
  p.fillPath(face, QColor(255, 220, 90, 40));
  p.setPen(QPen(QColor(255, 220, 90), 2.0));
  p.drawPath(face);
  p.setBrush(QColor(255, 220, 90));
  for (const auto &pt: v.poly)
    p.drawEllipse(toScreen(uvOf(v.basis, pt)), 3.0, 3.0);
  const QPointF c = toScreen(uvOf(v.basis, centreOf(v.poly)));
  p.setPen(QPen(QColor(255, 90, 220), 1.5));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(c, 7.0, 7.0);
  p.drawLine(c - QPointF(10, 0), c + QPointF(10, 0));
  p.drawLine(c - QPointF(0, 10), c + QPointF(0, 10));
}

void UvCanvas::mousePressEvent(QMouseEvent *e) {
  m_press = m_last = e->position();
  m_pressUv = toUv(e->position());
  if (e->button() == Qt::MiddleButton) {
    m_drag = Drag::Pan;
    return;
  }
  if (e->button() != Qt::LeftButton || !m_engine || !m_engine->primaryFace().valid)
    return;
  m_drag = e->modifiers().testFlag(Qt::ControlModifier) ? Drag::Rotate
           : e->modifiers().testFlag(Qt::ShiftModifier) ? Drag::Scale
                                                        : Drag::Offset;
  m_engine->beginFaceEdit();
}

void UvCanvas::mouseMoveEvent(QMouseEvent *e) {
  if (m_drag == Drag::Pan) {
    const QPointF d = e->position() - m_last;
    m_center -= glm::dvec2(d.x(), d.y()) / m_zoom;
    m_last = e->position();
    update();
    return;
  }
  if (m_drag == Drag::None || !m_engine)
    return;
  const glm::dvec2 uv = toUv(e->position());
  const glm::dvec2 d = uv - m_pressUv;
  const QPointF dpx = e->position() - m_press;
  const auto view = m_engine->primaryFace();
  const glm::dvec2 c0 = view.valid ? uvOf(view.basis, centreOf(view.poly)) : glm::dvec2(0.0);
  const Drag mode = m_drag;
  const bool snap = !e->modifiers().testFlag(Qt::AltModifier);
  m_engine->updateFaceEdit([&](const std::vector<glm::vec3> &poly, const glm::vec3 &n, eng::FaceTexBasis &base,
                               eng::FaceTexBasis &out) {
    const glm::vec3 c = centreOf(poly);
    const glm::dvec2 cuv = uvOf(base, c);
    switch (mode) {
      case Drag::Offset: {
        // Snap the move to 1/16 tile unless Alt is held.
        glm::dvec2 dd = d;
        if (snap)
          dd = glm::round(dd * 16.0) / 16.0;
        out.offsetU = float(base.offsetU + dd.x);
        out.offsetV = float(base.offsetV + dd.y);
        break;
      }
      case Drag::Rotate: {
        const glm::dvec2 a0 = m_pressUv - c0, a1 = uv - c0;
        double ang = std::atan2(a1.y, a1.x) - std::atan2(a0.y, a0.x);
        if (snap)
          ang = std::round(ang / (3.14159265358979 / 12.0)) * (3.14159265358979 / 12.0); // 15 degrees
        out.uAxis = glm::normalize(glm::rotate(base.uAxis, float(-ang), n));
        out.vAxis = glm::normalize(glm::rotate(base.vAxis, float(-ang), n));
        // Keep the face centre fixed in texture space.
        out.offsetU = 0.0f;
        out.offsetV = 0.0f;
        const glm::dvec2 now = uvOf(out, c);
        out.offsetU = float(cuv.x - now.x);
        out.offsetV = float(cuv.y - now.y);
        break;
      }
      case Drag::Scale: {
        double fu = std::pow(2.0, dpx.x() / 150.0), fv = std::pow(2.0, dpx.y() / 150.0);
        if (snap) {
          fu = std::pow(2.0, std::round(std::log2(fu) * 4.0) / 4.0);
          fv = std::pow(2.0, std::round(std::log2(fv) * 4.0) / 4.0);
        }
        out.tilesPerUnitU = float(base.tilesPerUnitU * fu);
        out.tilesPerUnitV = float(base.tilesPerUnitV * fv);
        out.offsetU = float(cuv.x - glm::dot(c, out.uAxis) * out.tilesPerUnitU);
        out.offsetV = float(cuv.y - glm::dot(c, out.vAxis) * out.tilesPerUnitV);
        break;
      }
      default:
        break;
    }
  });
  m_last = e->position();
  update();
}

void UvCanvas::mouseReleaseEvent(QMouseEvent *) {
  const Drag mode = m_drag;
  m_drag = Drag::None;
  if (!m_engine || mode == Drag::Pan || mode == Drag::None)
    return;
  m_engine->endFaceEdit(mode == Drag::Offset   ? QStringLiteral("Move texture")
                        : mode == Drag::Rotate ? QStringLiteral("Rotate texture")
                                               : QStringLiteral("Scale texture"));
}

void UvCanvas::wheelEvent(QWheelEvent *e) {
  const glm::dvec2 before = toUv(e->position());
  m_zoom = std::clamp(m_zoom * std::pow(1.0015, e->angleDelta().y()), 8.0, 4000.0);
  m_center += before - toUv(e->position());
  update();
}

UvEditor::UvEditor(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);
  auto *grid = new QGridLayout;
  grid->setSpacing(2);
  int col = 0, row = 0;
  for (const char *id: {"tex.justifyL", "tex.justifyR", "tex.justifyT", "tex.justifyB", "tex.justifyC", "tex.fit",
                        "tex.alignEdge", "tex.alignWorld", "tex.alignFace", "tex.reset", "tex.rotCCW", "tex.rotCW",
                        "tex.flipU", "tex.flipV", "tex.copyAttr", "tex.pasteAttr", "tex.wrap"}) {
    QAction *a = act(QString::fromLatin1(id));
    if (!a)
      continue;
    auto *b = new QToolButton(this);
    b->setDefaultAction(a);
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    scale::bind(b, [b] { b->setIconSize(scale::icon(scale::Icon::Medium)); });
    b->setAutoRaise(true);
    grid->addWidget(b, row, col);
    if (++col == 9) {
      col = 0;
      ++row;
    }
  }
  v->addLayout(grid);
  m_canvas = new UvCanvas(engine, this);
  v->addWidget(m_canvas, 1);
  m_info = new QLabel(this);
  m_info->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  m_info->setWordWrap(true);
  v->addWidget(m_info);
  if (engine) {
    for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::uvTransformChanged, &vk::Engine3D::objectStateChanged,
                    &vk::Engine3D::sceneChanged})
      connect(engine, sig, this, [this] { refresh(); });
  }
  refresh();
}

void UvEditor::refresh() {
  m_canvas->refresh();
  if (!m_engine)
    return;
  const auto s = m_engine->faceSummary();
  if (s.count == 0) {
    m_info->setText(tr("No faces selected."));
    return;
  }
  auto num = [](bool mixed, double v, int prec) { return mixed ? QStringLiteral("—") : QString::number(v, 'f', prec); };
  m_info->setText(tr("%1 face(s) · %2 %3×%4<br>tile %5 × %6 u · offset %7, %8 · rotation %9°")
                      .arg(s.count)
                      .arg(s.mixedMaterial ? tr("(mixed materials)") : s.material.toHtmlEscaped())
                      .arg(s.texWidth)
                      .arg(s.texHeight)
                      .arg(num(s.mixedScaleU, s.tileSourceU, 1), num(s.mixedScaleV, s.tileSourceV, 1),
                           num(s.mixedOffsetU, s.offsetU, 3), num(s.mixedOffsetV, s.offsetV, 3),
                           num(s.mixedRotation, s.rotation, 1)));
}

} // namespace ui
