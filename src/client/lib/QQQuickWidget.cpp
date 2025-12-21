#include "QQQuickWidget.h"

#include <algorithm>
#include <cmath>
#include <QCursor>
#include <QQuickItem>
#include <QTimer>

#include "client/lib/globals.h"
#include "client/vulkan/vk_engine.h"
#include "client/prefs/preferences.h"

QQQuickWidget::QQQuickWidget(QWidget *parent) : QQuickWidget(parent), m_rightMouseDown(false), m_firstMouse(true) {
  setMouseTracking(true);

  m_cameraSpeedChangedTimer = new QTimer(this);
  m_cameraSpeedChangedTimer->setSingleShot(true);
  m_cameraSpeedChangedTimer->setInterval(1100);
  connect(m_cameraSpeedChangedTimer, &QTimer::timeout, [this] {
    m_showCameraSpeedChangedIndicator = false;
    emit showCameraSpeedChangedIndicatorChanged(m_showCameraSpeedChangedIndicator);
  });

  auto &p = prefs::get();
  p.watch(QStringLiteral("Viewport/FlySpeedMin"), this, [this](const QVariant &v) {
    kCameraSpeedMin = v.toFloat();
    setCameraSpeed(m_cameraSpeed);
  });
  p.watch(QStringLiteral("Viewport/FlySpeedMax"), this, [this](const QVariant &v) {
    kCameraSpeedMax = std::max(kCameraSpeedMin + 1.0f, v.toFloat());
    setCameraSpeed(m_cameraSpeed);
  });
  p.watch(QStringLiteral("Viewport/FlySpeedSteps"), this, [this](const QVariant &v) { kCameraSpeedSteps = std::max(2, v.toInt()); });
  p.watch(QStringLiteral("Viewport/FlySpeed"), this, [this](const QVariant &v) {
    kCameraSpeedDefault = v.toFloat();
    setCameraSpeed(kCameraSpeedDefault);
    if (g::engine3D)
      g::engine3D->setSpeed(m_cameraSpeed);
  });
}

void QQQuickWidget::wheelEvent(QWheelEvent *event) {
  // Input can change hover highlights and the gizmo: request a frame.
  if (g::engine3D)
    g::engine3D->requestRedraw();
  const QPoint p = event->position().toPoint();
  const bool in3d = effectiveViewport3DRect().contains(p);

  // QML gets first refusal; only a wheel nothing in the scene wants becomes camera speed.
  if (!m_rightMouseDown) {
    QQuickWidget::wheelEvent(event);
    if (event->isAccepted())
      return;
  }

  if (!in3d) {
    return;
  }

  if (g::engine3D && g::engine3D->cutModeActive() && !m_rightMouseDown) {
    if (event->angleDelta().y() + event->angleDelta().x() != 0)
      g::engine3D->turnCut3D();
    event->accept();
    return;
  }

  if (g::engine3D && g::engine3D->mirrorPlacing() && !m_rightMouseDown) {
    if (event->angleDelta().y() + event->angleDelta().x() != 0)
      g::engine3D->turnMirror3D();
    event->accept();
    return;
  }

  if (event->modifiers().testFlag(Qt::AltModifier) && g::engine3D) {
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->beginOrbit(float(px.x()), float(px.y()));
    g::engine3D->dolly(float(event->angleDelta().y() + event->angleDelta().x()) / 120.0f);
    event->accept();
    return;
  }

  if (m_cameraSpeedChangedTimer->isActive())
    m_cameraSpeedChangedTimer->stop();

  const int delta = event->angleDelta().y();
  // Notches step along a curve that spends fewer on the slow half: half-bar speed is ~60% of the way up.
  if (delta != 0) {
    const float n = cameraSpeedT(m_cameraSpeed) * float(kCameraSpeedSteps);
    const float next = delta > 0 ? std::floor(n + 1e-3f) + 1.0f : std::ceil(n - 1e-3f) - 1.0f;
    setCameraSpeed(cameraSpeedAt(next / float(kCameraSpeedSteps)));
  }

  if (g::engine3D)
    g::engine3D->setSpeed(m_cameraSpeed);

  m_showCameraSpeedChangedIndicator = true;
  emit showCameraSpeedChangedIndicatorChanged(m_showCameraSpeedChangedIndicator);
  m_cameraSpeedChangedTimer->start();

  event->accept();
}

void QQQuickWidget::mousePressEvent(QMouseEvent *event) {
  // Input can change hover highlights and the gizmo: request a frame.
  if (g::engine3D)
    g::engine3D->requestRedraw();
  const QPoint p = event->position().toPoint();

  // Layer 1: QML always gets the press; layer 2 only when the cursor is not over QML chrome.
  QQuickWidget::mousePressEvent(event);
  const bool overUi = overUiOverlay();
  // The release must go back to whoever took the press, or a QML control never sees its click end.
  m_pressTakenByUi = overUi;
  if (overUi)
    return;

  m_draggingViewport3D = effectiveViewport3DRect().contains(p) && g::engine3D != nullptr;
  if (!m_draggingViewport3D)
    return;

  // Alt+LMB orbits, MMB pans.
  if ((event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))
      || event->button() == Qt::MiddleButton) {
    m_navMode = event->button() == Qt::MiddleButton ? 2 : 1;
    m_navLast = event->position();
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->beginOrbit(float(px.x()), float(px.y()));
    setCursor(m_navMode == 1 ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
    return;
  }

  // Bevel mode: a click picks / drops the edge under the pointer (the camera keeps its buttons).
  if (event->button() == Qt::LeftButton && g::engine3D->bevelMode()) {
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->pressBevel3D(float(px.x()), float(px.y()));
    m_draggingViewport3D = false;
    return;
  }

  if (event->button() == Qt::LeftButton && g::engine3D->cutModeActive()) {
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->clickCut3D(float(px.x()), float(px.y()), event->modifiers().testFlag(Qt::ShiftModifier));
    m_draggingViewport3D = false;
    return;
  }

  if (event->button() == Qt::LeftButton && g::engine3D->mirrorPlacing()) {
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->clickMirror3D(float(px.x()), float(px.y()));
    m_draggingViewport3D = false;
    return;
  }

  if (event->button() == Qt::LeftButton) {
    // A press on a gizmo handle grabs it; only a miss falls through to picking.
    const QPointF px = toViewportPixels(event->position());
    if (!g::engine3D->beginGizmoDrag(float(px.x()), float(px.y()))) {
      const bool additive = event->modifiers().testFlag(Qt::ShiftModifier);
      qDebug().noquote() << QStringLiteral("[pick] LMB (%1,%2) mode=%3")
                               .arg(p.x()).arg(p.y())
                               .arg(g::editorMode == g::EditorMode::MATERIAL ? QStringLiteral("material")
                                                                           : QStringLiteral("object"));
      g::engine3D->performPicking(float(px.x()), float(px.y()), additive);
    }
    return;
  }

  if (event->button() == Qt::RightButton) {
    m_rightMouseDown = true;
    m_firstMouse = true;
    m_viewport3DLocationOnScreen = effectiveViewport3DRect();

    // Warp the cursor to the viewport centre for free-look.
    const QPoint center = m_viewport3DLocationOnScreen.center();
    QCursor::setPos(mapToGlobal(center));
    setCursor(Qt::BlankCursor);
  }
}

void QQQuickWidget::mouseReleaseEvent(QMouseEvent *event) {
  // Input can change hover highlights and the gizmo: request a frame.
  if (g::engine3D)
    g::engine3D->requestRedraw();
  if (m_navMode != 0 && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
    m_navMode = 0;
    m_draggingViewport3D = false;
    unsetCursor();
    return;
  }
  const bool gizmoDragging = g::engine3D && g::engine3D->isGizmoDragging();

  // QML must see the release whenever it took the press, or whenever the editor is not holding the mouse
  // (m_draggingViewport3D alone outlives its click and swallowed every later panel release).
  if (!m_draggingViewport3D || m_pressTakenByUi)
    QQuickWidget::mouseReleaseEvent(event);

  if (event->button() == Qt::LeftButton && g::engine3D != nullptr)
    g::engine3D->endGizmoDrag();

  // A left press that became no gizmo drag owns nothing.
  if (event->button() == Qt::LeftButton && !gizmoDragging)
    m_draggingViewport3D = false;

  if (event->button() == Qt::RightButton) {
    m_rightMouseDown = false;
    if (g::engine3D)
      g::engine3D->resetFreelookDelta();

    if (m_draggingViewport3D) {
      const QPoint center = m_viewport3DLocationOnScreen.center();
      QCursor::setPos(mapToGlobal(center));
      unsetCursor();
    }

    m_draggingViewport3D = false;
  }
}

void QQQuickWidget::mouseMoveEvent(QMouseEvent *event) {
  // Input can change hover highlights and the gizmo: request a frame.
  if (g::engine3D)
    g::engine3D->requestRedraw();
  if (m_navMode != 0 && g::engine3D) {
    const QPointF d = event->position() - m_navLast;
    m_navLast = event->position();
    const qreal dpr = devicePixelRatioF();
    if (m_navMode == 1)
      g::engine3D->orbit(float(d.x()), float(d.y()));
    else
      g::engine3D->pan(float(d.x() * dpr), float(d.y() * dpr));
    return;
  }
  // The editor holds the mouse: QML gets nothing.
  if (m_rightMouseDown && m_draggingViewport3D) {
    const QPoint p = event->position().toPoint();
    const QPoint center = m_viewport3DLocationOnScreen.center();

    if (m_firstMouse) {
      m_firstMouse = false;
      return;
    }

    // Warping makes X send a synthetic motion event at the centre; handling it would loop warp -> event -> warp and
    // starve the render loop. Drop anything already at the centre.
    if (p == center)
      return;

    // Accumulate: WASD is integrated once per frame in Engine3D::updateCamera(), and rotating at raw mouse rate on
    // top reads as stutter.
    if (g::engine3D)
      g::engine3D->addFreelookDelta(p.x() - center.x(), center.y() - p.y());

    QCursor::setPos(mapToGlobal(center));
    return;
  }

  const QPoint p = event->position().toPoint();
  if (m_draggingViewport3D && g::engine3D && g::engine3D->isGizmoDragging()) {
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->updateGizmoDrag(float(px.x()), float(px.y()),
                                event->modifiers().testFlag(Qt::ShiftModifier));
    event->accept();
    return;
  }

  // Layer 1: hover inside QML chrome is QML's (no gizmo highlight under a panel). Gated on the overlay registry:
  // hover delivery leaves the event's accepted flag set everywhere.
  QQuickWidget::mouseMoveEvent(event);
  if (overUiOverlay())
    return;

  if (event->buttons() == Qt::NoButton && g::engine3D != nullptr
      && effectiveViewport3DRect().contains(p)) {
    const QPointF px = toViewportPixels(event->position());
    g::engine3D->hoverBevel3D(float(px.x()), float(px.y()));
    if (g::engine3D->cutModeActive())
      g::engine3D->hoverCut3D(float(px.x()), float(px.y()), event->modifiers().testFlag(Qt::ShiftModifier));
    else if (g::engine3D->mirrorPlacing())
      g::engine3D->hoverMirror3D(float(px.x()), float(px.y()));
    else
      g::engine3D->hoverGizmo(float(px.x()), float(px.y()));
  } else if (event->buttons() == Qt::NoButton && g::engine3D && g::engine3D->mirrorPlacing()) {
    g::engine3D->leaveMirror3D();
  }
  if (g::engine3D && g::engine3D->bevelMode() && !effectiveViewport3DRect().contains(p))
    g::engine3D->hoverBevel3D(-1.0f, -1.0f);
}

float QQQuickWidget::cameraSpeedAt(float t) const {
  t = std::clamp(t, 0.0f, 1.0f);
  return kCameraSpeedMin + (kCameraSpeedMax - kCameraSpeedMin) * std::pow(t, kCameraSpeedCurve);
}

float QQQuickWidget::cameraSpeedT(float speed) const {
  const float f = std::clamp((speed - kCameraSpeedMin) / (kCameraSpeedMax - kCameraSpeedMin), 0.0f, 1.0f);
  return std::pow(f, 1.0f / kCameraSpeedCurve);
}

void QQQuickWidget::setCameraSpeed(float speed) {
  speed = qBound(kCameraSpeedMin, speed, kCameraSpeedMax);
  if (qFuzzyCompare(m_cameraSpeed, speed)) return;
  m_cameraSpeed = speed;
  const int pct = static_cast<int>(
      qRound((m_cameraSpeed - kCameraSpeedMin) / (kCameraSpeedMax - kCameraSpeedMin) * 100.f));
  if (pct != m_cameraSpeedPct) {
    m_cameraSpeedPct = pct;
    emit cameraSpeedChangedPct(m_cameraSpeedPct);
  }
  emit cameraSpeedChanged(m_cameraSpeed);
}

void QQQuickWidget::setCameraSpeedPct(const int speed) {
  const int pct = qBound(1, speed, 100);
  if (m_cameraSpeedPct == pct) return;
  m_cameraSpeedPct = pct;
  setCameraSpeed(kCameraSpeedMin
                 + (kCameraSpeedMax - kCameraSpeedMin) * static_cast<float>(pct) / 100.f);
  emit cameraSpeedChangedPct(m_cameraSpeedPct);
}
QPointF QQQuickWidget::toViewportPixels(const QPointF &logical) const {
  // The engine works in render-target (physical) pixels; Qt delivers device-independent ones (HiDPI,
  // QT_SCALE_FACTOR).
  const QRect vp = effectiveViewport3DRect();
  const qreal dpr = devicePixelRatioF();
  return QPointF((logical.x() - vp.x()) * dpr, (logical.y() - vp.y()) * dpr);
}
