#pragma once

#include <QQuickWidget>
#include <QMouseEvent>
#include <QPointer>

// Input layers:
//   1. QML gets every press and hover first; a PanelInputBlocker under the cursor claims it, and while any is claimed
//      the editor hears nothing.
//   2. The viewport editor sees only input over bare scene (picking, gizmo, free-look).
// Don't use the event's accepted flag instead: Qt Quick tracks press acceptance per QEventPoint and leaves the
// widget-level flag set for hover everywhere ("consumed" over empty scene, "free" over a control).
// During an editor drag (free-look, gizmo) the editor owns the mouse until the button comes up.
class QQQuickWidget : public QQuickWidget {
Q_PROPERTY(int viewportWidth MEMBER m_width)
Q_PROPERTY(int viewportHeight MEMBER m_height)
Q_PROPERTY(float cameraSpeed READ cameraSpeed WRITE setCameraSpeed NOTIFY cameraSpeedChanged)
Q_PROPERTY(int cameraSpeedPct READ cameraSpeedPct WRITE setCameraSpeedPct NOTIFY cameraSpeedChangedPct)
Q_PROPERTY(bool showCameraSpeedChangedIndicator MEMBER m_showCameraSpeedChangedIndicator NOTIFY showCameraSpeedChangedIndicatorChanged)
Q_OBJECT

public:
  explicit QQQuickWidget(QWidget *parent = nullptr);

  // viewportWidth/Height are pushed from QML and track the 3D cell, not this widget.
  QRect effectiveViewport3DRect() const {
    return m_width > 0 && m_height > 0 ? QRect(0, 0, m_width, m_height) : QRect(QPoint(0, 0), size());
  }

  // Widget-local logical coordinates -> render-target pixels of the 3D cell.
  [[nodiscard]] QPointF toViewportPixels(const QPointF &logical) const;

  [[nodiscard]] float cameraSpeed() const { return m_cameraSpeed; }

  // Called from QML (PanelInputBlocker) as the cursor enters / leaves a panel. Keyed by the handler object, so
  // overlapping panels stack and a destroyed one stops counting. While any is registered the editor sees no input.
  Q_INVOKABLE void setUiOverlay(QObject *key, bool claimed) {
    m_uiOverlay.removeAll(key);
    if (claimed)
      m_uiOverlay.append(QPointer<QObject>(key));
  }

  [[nodiscard]] bool overUiOverlay() const {
    for (const auto &p: m_uiOverlay)
      if (p)
        return true;
    return false;
  }

  void setCameraSpeed(float speed);
  [[nodiscard]] int cameraSpeedPct() const { return m_cameraSpeedPct; }
  void setCameraSpeedPct(int speed);

signals:
  void cameraSpeedChanged(float);
  void cameraSpeedChangedPct(int pct);
  void showCameraSpeedChangedIndicatorChanged(bool);

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  int m_width = 0;
  int m_height = 0;

  bool m_firstMouse = true;
  bool m_rightMouseDown = false;
  bool m_draggingViewport3D = false;
  bool m_pressTakenByUi = false;
  int m_navMode = 0; // 1 orbit, 2 pan
  QPointF m_navLast;
  QList<QPointer<QObject>> m_uiOverlay;
  QRect m_viewport3DLocationOnScreen;

  // Camera speed in world units/sec; band and step from the Viewport preferences. The wheel is multiplicative.
  float kCameraSpeedMin = 6.25f; // Viewport/FlySpeedMin
  float kCameraSpeedMax = 300.0f;
  float kCameraSpeedDefault = 55.0f;
  int kCameraSpeedSteps = 10;
  static constexpr float kCameraSpeedCurve = 1.36f;
  [[nodiscard]] float cameraSpeedAt(float t) const;
  [[nodiscard]] float cameraSpeedT(float speed) const;

  float m_cameraSpeed = kCameraSpeedDefault;
  int m_cameraSpeedPct = 8;
  bool m_showCameraSpeedChangedIndicator = false;
  QTimer* m_cameraSpeedChangedTimer = nullptr;
};
