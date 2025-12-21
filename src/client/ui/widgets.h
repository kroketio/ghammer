#pragma once

#include <QColor>
#include <QIcon>
#include <QPointer>

class QColorDialog;
class QSlider;
#include <QFont>
#include <algorithm>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QWidget>

#include <functional>

class QVBoxLayout;
class QGridLayout;
class QLabel;

namespace ui {

// Scales a font sized in points or pixels (stylesheet pixel sizes leave pointSizeF() at -1).
inline QFont scaledFont(QFont f, double k) {
  if (f.pixelSize() > 0)
    f.setPixelSize(std::max(6, int(f.pixelSize() * k + 0.5)));
  else if (f.pointSizeF() > 0)
    f.setPointSizeF(std::max(5.0, f.pointSizeF() * k));
  return f;
}

// Wheel over a spin box, combo box or slider only changes it once it has focus; otherwise the panel scrolls.
// Installed once on the application.
void installWheelGuard(QObject *app);

// A titled block that folds open/closed. Collapsed state persists per title.
class Section final : public QWidget {
  Q_OBJECT

public:
  Section(const QString &title, const QString &icon, QWidget *parent = nullptr);
  [[nodiscard]] QGridLayout *grid() const { return m_grid; }
  [[nodiscard]] QVBoxLayout *body() const { return m_body; }
  void setExpanded(bool on);
  [[nodiscard]] bool expanded() const;
  // A small label next to the title ("3 faces").
  void setBadge(const QString &text);
  // Another title / icon for the same section (its open state stays under the first title).
  void setTitle(const QString &title, const QString &icon);
  // Nested section: lighter header, smaller margins; open/closed state not remembered.
  void setSubsection();

  // Returns the row.
  int addRow(const QString &label, QWidget *w, const QString &tooltip = {});

private:
  QToolButton *m_header;
  QWidget *m_head = nullptr;
  QLabel *m_badge;
  QWidget *m_content;
  QVBoxLayout *m_body;
  QGridLayout *m_grid;
  QString m_key;
};

// Spin box that can show "mixed", accepts expressions ("64*2", "+16") and reports commits, not keystrokes.
class NumberField final : public QDoubleSpinBox {
  Q_OBJECT

public:
  explicit NumberField(QWidget *parent = nullptr);
  void setValueQuiet(double v, bool mixed = false);
  [[nodiscard]] bool isMixed() const { return m_mixed; }

  QValidator::State validate(QString &input, int &pos) const override;
  double valueFromText(const QString &text) const override;
  QString textFromValue(double value) const override;
  // One step is singleStep() rounded to at least one unit of the shown last decimal, so no notch is lost.
  void stepBy(int steps) override;
  [[nodiscard]] double effectiveStep() const;

signals:
  void committed(double value);

protected:
  void keyPressEvent(QKeyEvent *e) override;

private:
  bool m_mixed = false;
  double m_lastCommitted = 0.0;
  void commit();
};

// Dot next to a value showing its origin: amber = changed here (click resets), grey = the default.
class ResetDot final : public QToolButton {
  Q_OBJECT

public:
  static QColor changedColor() { return QColor(240, 168, 64); }
  static QColor defaultColor() { return QColor(96, 104, 130); }

  explicit ResetDot(QWidget *parent = nullptr);
  // `resettable` enables the click; `hollow` draws a ring instead of a dot.
  void setState(const QColor &color, bool resettable, const QString &tooltip, bool hollow = false);
  // Amber and clickable when `changed`, a grey ring otherwise.
  void setChanged(bool changed, const QString &what = {});
};

// Slider with a number box for bounded values; drags apply live (the engine merges them into one undo step).
// `curve` > 1 gives the low end of the range more slider travel.
class SliderField final : public QWidget {
  Q_OBJECT

public:
  SliderField(double lo, double hi, int decimals, const QString &suffix = {}, double curve = 1.0,
              QWidget *parent = nullptr);
  void setValueQuiet(double v, bool mixed = false);
  [[nodiscard]] double value() const;
  [[nodiscard]] NumberField *field() const { return m_field; }
  // Dragging, the wheel and the arrow keys land on multiples of `step`; typed
  // values are kept as typed. Without one: a round step of about 1/200 of the
  // range, or two significant digits on a curved slider.
  void setStep(double step);
  [[nodiscard]] double step() const { return m_step; }

signals:
  void committed(double value);

private:
  [[nodiscard]] int toSlider(double v) const;
  [[nodiscard]] double fromSlider(int pos) const;
  [[nodiscard]] double snap(double v) const;
  void apply(double v, bool moveSlider);
  QSlider *m_slider = nullptr;
  NumberField *m_field = nullptr;
  double m_lo, m_hi, m_curve;
  double m_step = 0.0;
  bool m_explicitStep = false;
};

// Swatch opening a non-modal colour picker that applies live; Cancel restores the previous colour.
class ColorButton final : public QToolButton {
  Q_OBJECT

public:
  explicit ColorButton(QWidget *parent = nullptr);
  void setColor(const QColor &c);
  [[nodiscard]] QColor color() const { return m_color; }

signals:
  void colorPicked(const QColor &c);

private:
  QColor m_color;
  QPointer<QColorDialog> m_dialog;
};

// Titled x / y / z row (the Inspector's Transform) with optional reset and sliders (applied on release).
// Commits per axis.
class AxisRow final : public QWidget {
  Q_OBJECT

public:
  AxisRow(const QString &title, const QString &suffix, int decimals, double step, QWidget *parent = nullptr);
  // Sliders under the values, over [lo, hi].
  void setSliderRange(double lo, double hi);
  // Hidden until a tooltip is given.
  void setReset(const QString &tooltip);
  [[nodiscard]] NumberField *field(int axis) const { return m_fields[axis]; }
  void setValuesQuiet(const double v[3], const bool mixed[3] = nullptr);
  void setFieldsEnabled(bool on);

signals:
  void committed(int axis, double value);
  void resetRequested();

private:
  NumberField *m_fields[3] {};
  QSlider *m_sliders[3] {};
  QToolButton *m_reset = nullptr;
  double m_lo = 0.0, m_hi = 0.0;
};

// A sidebar tree's sub-item mark (Preferences, Colour LUTs): a short line bending right.
QIcon subItemGlyph(int size);

} // namespace ui
