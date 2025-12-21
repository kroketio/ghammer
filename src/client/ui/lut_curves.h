#pragma once

#include "client/ui/lut_levels.h"

namespace ui {

// A curve editor over a histogram.
class CurveEditor final : public QWidget {
public:
  explicit CurveEditor(QWidget *parent = nullptr);
  void setPoints(const eng::CurvePoints &pts);
  [[nodiscard]] const eng::CurvePoints &points() const { return m_points; }
  void setHistogram(const eng::grade::Histogram &h, bool log, const QColor &color);

  std::function<void()> onPressed;                         // a new drag starts
  std::function<void(const eng::CurvePoints &)> onChanged; // while editing

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;

private:
  [[nodiscard]] QRectF area() const;
  [[nodiscard]] QPointF toScreen(const glm::dvec2 &p) const;
  [[nodiscard]] glm::dvec2 toCurve(const QPointF &p) const;
  [[nodiscard]] eng::CurvePoints shown() const; // without a point dragged off
  void emitChanged();

  eng::CurvePoints m_points = eng::identityCurve();
  eng::CurvePoints m_before; // at the start of a drag (Esc)
  int m_selected = -1;
  bool m_dragging = false;
  bool m_removed = false; // the dragged point is outside: left out
  eng::grade::Histogram m_hist {};
  bool m_log = false;
  QColor m_color;
};

class CurvesWindow final : public LutStepWindow {
  Q_OBJECT

public:
  CurvesWindow(LutWindow *owner, vk::Engine3D *engine, const QString &lut, int stepId, const QString &title);

protected:
  void load(const eng::GradeStep &s) override;

private:
  CurveEditor *m_curve = nullptr;
  int m_drag = 0;
};

} // namespace ui
