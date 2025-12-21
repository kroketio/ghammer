#pragma once

#include <QDialog>
#include <QImage>

#include <array>
#include <functional>
#include <optional>

#include "client/engine/color_grade.h"

class QComboBox;
class QToolButton;
class QVBoxLayout;

namespace vk {
class Engine3D;
}

namespace ui {

class LutWindow;

void paintHistogram(QPainter &p, const QRectF &r, const eng::grade::Histogram &h, bool log, const QColor &color);
[[nodiscard]] QColor gradeChannelColor(eng::GradeChannel ch);
[[nodiscard]] QString gradeChannelName(eng::GradeChannel ch);

class HistogramView final : public QWidget {
public:
  explicit HistogramView(QWidget *parent = nullptr);
  void setData(const eng::grade::Histogram &h, bool log, const QColor &color);

protected:
  void paintEvent(QPaintEvent *) override;

private:
  eng::grade::Histogram m_hist {};
  bool m_log = false;
  QColor m_color;
};

// A 0..1 bar with black / white handles (plus gamma on the input slider).
class LevelsSlider final : public QWidget {
public:
  LevelsSlider(bool withGamma, QWidget *parent = nullptr);
  void setValues(double black, double white, double gamma = 1.0);
  [[nodiscard]] double black() const { return m_black; }
  [[nodiscard]] double white() const { return m_white; }
  [[nodiscard]] double gamma() const { return m_gamma; }

  std::function<void()> onPressed;  // a new drag starts
  std::function<void()> onChanged;  // while dragging

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  [[nodiscard]] QSize sizeHint() const override;

private:
  [[nodiscard]] double xOf(double v) const;
  [[nodiscard]] double valueAt(double x) const;
  [[nodiscard]] double gammaPos() const;
  void dragTo(double x);

  bool m_withGamma;
  double m_black = 0.0, m_white = 1.0, m_gamma = 1.0;
  int m_handle = -1; // 0 black, 1 gamma, 2 white
};

// The preview picture through the steps before one step, and its histograms; cached until those change.
class GradeInputCache {
public:
  [[nodiscard]] const QImage &picture(const eng::LutGrade &g, int stepId);
  [[nodiscard]] eng::grade::Histogram histogram(const eng::LutGrade &g, int stepId, eng::GradeChannel ch);

private:
  std::vector<eng::GradeStep> m_steps;
  bool m_valid = false;
  QImage m_picture;
  std::array<std::optional<eng::grade::Histogram>, eng::kGradeChannels> m_hists;
};

// Base of the step windows: one step edited live; closes when the step goes.
class LutStepWindow : public QDialog {
  Q_OBJECT

public:
  LutStepWindow(LutWindow *owner, vk::Engine3D *engine, const QString &lut, int stepId, const QString &title);
  [[nodiscard]] QString lut() const { return m_lut; }
  [[nodiscard]] int stepId() const { return m_stepId; }
  void sync();

protected:
  [[nodiscard]] const eng::GradeStep *step() const;
  void edit(const QString &key, const std::function<void(eng::GradeStep &)> &fn);
  virtual void load(const eng::GradeStep &s) = 0;
  [[nodiscard]] eng::grade::Histogram inputHistogram(eng::GradeChannel ch);

  void addChannelRow();
  [[nodiscard]] eng::GradeChannel channel() const;
  [[nodiscard]] bool logHistogram() const;

  vk::Engine3D *m_engine;
  QVBoxLayout *m_controls = nullptr;
  QComboBox *m_channel = nullptr;
  QToolButton *m_lin = nullptr, *m_log = nullptr;

private:
  LutWindow *m_owner;
  QString m_lut;
  int m_stepId;
  bool m_self = false;
  GradeInputCache m_input;
};


} // namespace ui
