#include "widgets.h"

#include <QPen>

#include <QPainter>
#include <QPainterPath>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QScrollBar>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJSEngine>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <QVBoxLayout>

#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/scale.h"

namespace ui {

void Section::setTitle(const QString &title, const QString &icon) {
  m_header->setText(title);
  m_header->setIcon(ActionManager::icon(icon));
}

Section::Section(const QString &title, const QString &icon, QWidget *parent) : QWidget(parent) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 2);
  v->setSpacing(0);

  auto *head = new QWidget(this);
  m_head = head;
  head->setObjectName(QStringLiteral("sectionHeader"));
  head->setStyleSheet(QStringLiteral("#sectionHeader { background: #1a2038; border-top: 1px solid #2a3355; }"));
  auto *h = new QHBoxLayout(head);
  h->setContentsMargins(4, 4, 8, 4);
  m_header = new QToolButton(head);
  m_header->setText(title);
  m_header->setIcon(ActionManager::icon(icon));
  m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_header->setCheckable(true);
  m_header->setChecked(true);
  m_header->setAutoRaise(true);
  scale::setRole(m_header, scale::Font::Heading);
  m_header->setStyleSheet(QStringLiteral("QToolButton { border: none; }"));
  scale::bind(m_header, [this] { m_header->setIconSize(scale::icon(scale::Icon::Small)); });
  m_badge = new QLabel(head);
  m_badge->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  h->addWidget(m_header);
  h->addStretch(1);
  h->addWidget(m_badge);
  v->addWidget(head);

  m_content = new QWidget(this);
  m_body = new QVBoxLayout(m_content);
  m_body->setContentsMargins(12, 8, 12, 10);
  m_body->setSpacing(8);
  m_grid = new QGridLayout();
  m_grid->setHorizontalSpacing(10);
  m_grid->setVerticalSpacing(7);
  m_grid->setColumnStretch(1, 1);
  m_body->addLayout(m_grid);
  v->addWidget(m_content);

  m_key = QStringLiteral("Inspector/Collapsed/") + title;
  prefs::Def d;
  d.key = m_key;
  d.category = QStringLiteral("Hidden");
  d.type = prefs::Type::Bool;
  d.defaultValue = false;
  prefs::get().declare(d);
  setExpanded(!prefs::b(m_key.toLatin1().constData()));
  connect(m_header, &QToolButton::toggled, this, [this](bool on) {
    m_content->setVisible(on);
    if (!m_key.isEmpty())
      prefs::get().setValue(m_key, !on);
  });
}

void Section::setExpanded(bool on) {
  m_header->setChecked(on);
  m_content->setVisible(on);
}

bool Section::expanded() const { return m_header->isChecked(); }

void Section::setBadge(const QString &text) { m_badge->setText(text); }

void Section::setSubsection() {
  m_head->setStyleSheet(QStringLiteral("#sectionHeader { background: transparent; border-top: 1px solid #232a45; }"));
  m_head->layout()->setContentsMargins(0, 2, 4, 2);
  scale::setRole(m_header, scale::Font::Body);
  m_header->setStyleSheet(QStringLiteral("QToolButton { font-weight: 600; color: #c4cde6; border: none; }"));
  m_body->setContentsMargins(6, 4, 0, 8);
  m_key.clear();  // the owner decides what is open
  setExpanded(true);
}

int Section::addRow(const QString &label, QWidget *w, const QString &tooltip) {
  const int row = m_grid->rowCount();
  if (!label.isEmpty()) {
    auto *l = new QLabel(label, this);
    l->setStyleSheet(QStringLiteral("color:#aab4cf;"));
    l->setToolTip(tooltip);
    m_grid->addWidget(l, row, 0);
    m_grid->addWidget(w, row, 1);
  } else {
    m_grid->addWidget(w, row, 0, 1, 2);
  }
  if (!tooltip.isEmpty())
    w->setToolTip(tooltip);
  return row;
}

namespace {

class WheelGuard final : public QObject {
public:
  using QObject::QObject;

protected:
  bool eventFilter(QObject *obj, QEvent *e) override {
    if ((e->type() != QEvent::Wheel && e->type() != QEvent::Polish) || !obj->isWidgetType())
      return false;
    auto *w = static_cast<QWidget *>(obj);
    const bool valueWidget = qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w) ||
                             (qobject_cast<QAbstractSlider *>(w) && !qobject_cast<QScrollBar *>(w));
    if (!valueWidget)
      return false;
    // Qt focuses a WheelFocus widget before this filter sees the wheel: drop it from the policy on Polish.
    if (e->type() == QEvent::Polish) {
      if (w->focusPolicy() == Qt::WheelFocus)
        w->setFocusPolicy(Qt::StrongFocus);
      return false;
    }
    const QWidget *focus = QApplication::focusWidget();
    if (focus && (focus == w || w->isAncestorOf(focus)))
      return false;
    // Not clicked yet: an ignored wheel event propagates to the parent (the scroll area).
    e->ignore();
    return true;
  }
};

} // namespace

void installWheelGuard(QObject *app) { app->installEventFilter(new WheelGuard(app)); }

NumberField::NumberField(QWidget *parent) : QDoubleSpinBox(parent) {
  setRange(-1e7, 1e7);
  setDecimals(2);
  setKeyboardTracking(false);
  setAccelerated(true);
  setButtonSymbols(QAbstractSpinBox::NoButtons);
  setAlignment(Qt::AlignRight);
  setCorrectionMode(QAbstractSpinBox::CorrectToPreviousValue);
  scale::bind(this, [this] { setMinimumWidth(scale::tpx(60)); });
  connect(this, &QDoubleSpinBox::editingFinished, this, &NumberField::commit);
}

void NumberField::setValueQuiet(double v, bool mixed) {
  QSignalBlocker b(this);
  m_mixed = mixed;
  setValue(v);
  m_lastCommitted = value();
  if (mixed)
    lineEdit()->setText(QStringLiteral("—"));
}

QValidator::State NumberField::validate(QString &input, int &) const {
  // Parsed on commit. Qt validates the whole line incl. prefix / suffix ("80°"), so those are stripped.
  QString t = input.trimmed();
  if (const QString pre = prefix().trimmed(); !pre.isEmpty() && t.startsWith(pre))
    t = t.mid(pre.size());
  if (const QString suf = suffix().trimmed(); !suf.isEmpty() && t.endsWith(suf))
    t.chop(suf.size());
  t = t.trimmed();
  if (t.isEmpty() || t == QStringLiteral("—"))
    return QValidator::Intermediate;
  static const QRegularExpression ok(QStringLiteral("^[0-9eE+\\-*/(). ,]*$"));
  return ok.match(t).hasMatch() ? QValidator::Acceptable : QValidator::Invalid;
}

double NumberField::valueFromText(const QString &text) const {
  QString t = text.trimmed();
  t.replace(QLatin1Char(','), QLatin1Char('.'));
  if (!prefix().trimmed().isEmpty() && t.startsWith(prefix().trimmed()))
    t = t.mid(prefix().trimmed().size()).trimmed();
  if (!suffix().trimmed().isEmpty() && t.endsWith(suffix().trimmed()))
    t.chop(suffix().trimmed().size());
  t = t.trimmed();
  if (t.isEmpty() || t == QStringLiteral("—"))
    return value();
  // "+16" / "-16" / "*2" / "/2" are relative to the current value.
  if (t.startsWith(QLatin1Char('*')) || t.startsWith(QLatin1Char('/')) ||
      (t.size() > 1 && (t.startsWith(QLatin1Char('+'))) ))
    t = QString::number(value(), 'g', 12) + t;
  static QJSEngine js;
  const QJSValue r = js.evaluate(t);
  if (r.isNumber() && std::isfinite(r.toNumber()))
    return r.toNumber();
  return value();
}

QString NumberField::textFromValue(double v) const {
  QString s = QLocale::c().toString(v, 'f', decimals());
  if (s.contains(QLatin1Char('.'))) {
    while (s.endsWith(QLatin1Char('0')))
      s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
      s.chop(1);
  }
  return s;
}

void NumberField::commit() {
  const double v = value();
  if (!m_mixed && std::fabs(v - m_lastCommitted) < 1e-12)
    return;
  m_mixed = false;
  m_lastCommitted = v;
  emit committed(v);
}

void NumberField::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Escape) {
    setValueQuiet(m_lastCommitted, m_mixed);
    clearFocus();
    return;
  }
  QDoubleSpinBox::keyPressEvent(e);
}

double NumberField::effectiveStep() const {
  const double unit = std::pow(10.0, -decimals());
  return std::max(unit, std::round(singleStep() / unit) * unit);
}

void NumberField::stepBy(int steps) {
  m_mixed = false;
  setValue(value() + double(steps) * effectiveStep());
  selectAll(); // as QAbstractSpinBox does
  commit();
}

namespace {
QIcon resetDotIcon(const QColor &c, bool hollow) {
  QPixmap pm(14, 14);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  if (hollow) {
    p.setPen(QPen(c, 1.5));
    p.setBrush(Qt::NoBrush);
  } else {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
  }
  p.drawEllipse(QRectF(3, 3, 8, 8));
  return QIcon(pm);
}
} // namespace

ResetDot::ResetDot(QWidget *parent) : QToolButton(parent) {
  setAutoRaise(true);
  setFocusPolicy(Qt::NoFocus);
  scale::bind(this, [this] { setFixedSize(scale::px(18), scale::px(18)); });
  setChanged(false);
}

void ResetDot::setState(const QColor &color, const bool resettable, const QString &tooltip, const bool hollow) {
  setIcon(resetDotIcon(color, hollow));
  setEnabled(resettable);
  setToolTip(tooltip);
}

void ResetDot::setChanged(const bool changed, const QString &what) {
  if (changed)
    setState(changedColor(), true,
             what.isEmpty() ? tr("Changed — click to reset to the default") : tr("%1 — click to reset").arg(what));
  else
    setState(defaultColor(), false, tr("Default"), true);
}

ColorButton::ColorButton(QWidget *parent) : QToolButton(parent) {
  scale::bind(this, [this] { setMinimumWidth(scale::tpx(70)); });
  setCursor(Qt::PointingHandCursor);
  connect(this, &QToolButton::clicked, this, [this] {
    if (m_dialog) {
      m_dialog->raise();
      m_dialog->activateWindow();
      return;
    }
    const QColor original = m_color;
    auto *dlg = new QColorDialog(m_color, window());
    m_dialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(toolTip().isEmpty() ? tr("Pick colour") : toolTip());
    // The platform picker cannot preview live on every desktop.
    dlg->setOption(QColorDialog::DontUseNativeDialog);
    dlg->setModal(false);
    connect(dlg, &QColorDialog::currentColorChanged, this, [this](const QColor &c) {
      if (c.isValid() && c != m_color) {
        setColor(c);
        emit colorPicked(c);
      }
    });
    connect(dlg, &QDialog::rejected, this, [this, original] {
      if (m_color != original) {
        setColor(original);
        emit colorPicked(original);
      }
    });
    dlg->show();
  });
}

void ColorButton::setColor(const QColor &c) {
  m_color = c;
  setText(c.name().toUpper());
  setStyleSheet(QStringLiteral("QToolButton { background:%1; color:%2; border:1px solid #445; padding:2px 6px;"
                               " border-radius:3px; }")
                    .arg(c.name(), c.lightnessF() > 0.55 ? QStringLiteral("#111") : QStringLiteral("#eee")));
}

namespace {
constexpr int kSliderSteps = 1000;

double niceStep(double range, int decimals) {
  const double unit = std::pow(10.0, -decimals);
  const double raw = range / 200.0;
  if (raw <= unit)
    return unit;
  const double base = std::pow(10.0, std::floor(std::log10(raw)));
  for (const double f: {1.0, 2.0, 5.0, 10.0})
    if (raw <= f * base)
      return std::max(unit, f * base);
  return std::max(unit, 10.0 * base);
}

double snapTo(double v, double step) {
  return step > 0.0 ? std::round(v / step) * step : v;
}

// The wheel scrolls the panel unless the slider was clicked first. With `wheelSteps`, a notch is
// handed over as whole steps instead of nudging the slider.
class PanelSlider final : public QSlider {
public:
  using QSlider::QSlider;
  std::function<void(int)> wheelSteps;

protected:
  void wheelEvent(QWheelEvent *e) override {
    if (!hasFocus()) {
      e->ignore();
      return;
    }
    if (!wheelSteps) {
      QSlider::wheelEvent(e);
      return;
    }
    // 120 per notch; finer wheels add up until they make one.
    m_wheel += e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    const int notches = m_wheel / 120;
    m_wheel -= notches * 120;
    if (notches != 0)
      wheelSteps(notches);
    e->accept();
  }

  void keyPressEvent(QKeyEvent *e) override {
    if (wheelSteps) {
      int steps = 0;
      switch (e->key()) {
        case Qt::Key_Right:
        case Qt::Key_Up: steps = 1; break;
        case Qt::Key_Left:
        case Qt::Key_Down: steps = -1; break;
        case Qt::Key_PageUp: steps = 10; break;
        case Qt::Key_PageDown: steps = -10; break;
        default: break;
      }
      if (steps != 0) {
        wheelSteps(steps);
        e->accept();
        return;
      }
    }
    QSlider::keyPressEvent(e);
  }

private:
  int m_wheel = 0;
};
} // namespace

SliderField::SliderField(double lo, double hi, int decimals, const QString &suffix, double curve, QWidget *parent)
    : QWidget(parent), m_lo(lo), m_hi(hi), m_curve(std::max(1.0, curve)) {
  auto *h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(6);
  auto *slider = new PanelSlider(Qt::Horizontal, this);
  m_slider = slider;
  m_slider->setFocusPolicy(Qt::StrongFocus);
  m_slider->setRange(0, kSliderSteps);
  m_slider->setPageStep(kSliderSteps / 20);
  m_field = new NumberField(this);
  m_field->setRange(lo, hi);
  m_field->setDecimals(decimals);
  m_field->setSuffix(suffix);
  m_step = niceStep(hi - lo, decimals);
  m_field->setSingleStep(m_step);
  scale::bind(m_field, [this, suffix] {
    const QFontMetrics fm(scale::font(scale::Font::Body, m_field->font()));
    m_field->setFixedWidth(fm.horizontalAdvance(QStringLiteral("-0000.000") + suffix) + scale::tpx(24));
  });
  h->addWidget(m_slider, 1);
  h->addWidget(m_field);

  slider->wheelSteps = [this](int steps) { apply(snap(m_field->value() + steps * m_step), true); };
  connect(m_slider, &QSlider::valueChanged, this, [this](int pos) { apply(snap(fromSlider(pos)), false); });
  connect(m_slider, &QSlider::sliderReleased, this, [this] {
    QSignalBlocker b(m_slider);
    m_slider->setValue(toSlider(m_field->value()));
  });
  connect(m_field, &NumberField::committed, this, [this](double v) {
    QSignalBlocker b(m_slider);
    m_slider->setValue(toSlider(v));
    emit committed(v);
  });
}

int SliderField::toSlider(double v) const {
  const double t = m_hi > m_lo ? std::clamp((v - m_lo) / (m_hi - m_lo), 0.0, 1.0) : 0.0;
  return int(std::lround(std::pow(t, 1.0 / m_curve) * kSliderSteps));
}

double SliderField::fromSlider(int pos) const {
  const double t = std::pow(double(pos) / kSliderSteps, m_curve);
  return m_lo + t * (m_hi - m_lo);
}

double SliderField::value() const {
  return m_field->value();
}

void SliderField::setStep(const double step) {
  if (step <= 0.0)
    return;
  m_step = step;
  m_explicitStep = true;
  m_field->setSingleStep(step);
}

double SliderField::snap(double v) const {
  const double unit = std::pow(10.0, -m_field->decimals());
  if (!m_explicitStep && m_curve > 1.0 && v != 0.0)
    v = snapTo(v, std::pow(10.0, std::floor(std::log10(std::abs(v))) - 1.0));
  else
    v = snapTo(v, m_step);
  v = std::round(v / unit) * unit;
  return std::clamp(v, m_lo, m_hi);
}

void SliderField::apply(const double v, const bool moveSlider) {
  if (moveSlider) {
    QSignalBlocker b(m_slider);
    m_slider->setValue(toSlider(v));
  }
  if (v == m_field->value() && !m_field->isMixed())
    return;
  m_field->setValueQuiet(v);
  emit committed(v);
}

void SliderField::setValueQuiet(double v, bool mixed) {
  // A refresh while dragging or typing is ignored.
  if (m_slider->isSliderDown() || m_field->hasFocus())
    return;
  m_field->setValueQuiet(v, mixed);
  QSignalBlocker b(m_slider);
  m_slider->setValue(toSlider(v));
}

AxisRow::AxisRow(const QString &title, const QString &suffix, const int decimals, const double step, QWidget *parent)
    : QWidget(parent) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(3);

  auto *head = new QHBoxLayout;
  head->setContentsMargins(2, 0, 0, 0);
  auto *t = new QLabel(title, this);
  t->setStyleSheet(QStringLiteral("color:#c8cfdd;"));
  head->addWidget(t);
  head->addStretch(1);
  m_reset = new QToolButton(this);
  m_reset->setAutoRaise(true);
  m_reset->setFocusPolicy(Qt::NoFocus);
  m_reset->setIcon(ActionManager::icon(QStringLiteral("loop_back")));
  m_reset->hide();
  connect(m_reset, &QToolButton::clicked, this, &AxisRow::resetRequested);
  head->addWidget(m_reset);
  v->addLayout(head);

  auto *box = new QFrame(this);
  box->setObjectName(QStringLiteral("axisRowBox"));
  scale::setScaledStyleSheet(box, QStringLiteral(
      "QFrame#axisRowBox { background: #1b1f2a; border-radius: 6px; }"
      "QFrame#axisRowBox QDoubleSpinBox { background: transparent; border: none; padding: 0px; }"
      "QFrame#axisRowBox QDoubleSpinBox:focus { background: #262c3b; border-radius: 3px; }"
      "QFrame#axisRowBox QLabel { background: transparent; }"
      "QFrame#axisRowBox QSlider::groove:horizontal { height: 2px; background: #4b5163; }"
      "QFrame#axisRowBox QSlider::sub-page:horizontal { background: #4b5163; }"
      "QFrame#axisRowBox QSlider::handle:horizontal { width: 6px; height: 8px; margin: -3px 0; background: #e8ecf4;"
      " border-radius: 1px; }"));
  auto *h = new QHBoxLayout(box);
  h->setContentsMargins(10, 5, 10, 5);
  h->setSpacing(14);
  static const char *names[3] = {"x", "y", "z"};
  static const char *colors[3] = {"#d0646f", "#a9b64f", "#5f8fd8"};
  for (int a = 0; a < 3; ++a) {
    auto *cell = new QWidget(box);
    auto *cv = new QVBoxLayout(cell);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    auto *l = new QLabel(QString::fromLatin1(names[a]), cell);
    l->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(colors[a])));
    m_fields[a] = new NumberField(cell);
    m_fields[a]->setDecimals(decimals);
    m_fields[a]->setSingleStep(step);
    m_fields[a]->setFrame(false);
    m_fields[a]->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_fields[a]->setMinimumWidth(10);
    row->addWidget(l);
    row->addWidget(m_fields[a], 1);
    if (!suffix.isEmpty()) {
      auto *u = new QLabel(suffix, cell);
      u->setStyleSheet(QStringLiteral("color:#6c7488;"));
      row->addWidget(u);
    }
    cv->addLayout(row);
    m_sliders[a] = new PanelSlider(Qt::Horizontal, cell);
    m_sliders[a]->setFocusPolicy(Qt::NoFocus);
    m_sliders[a]->hide();
    cv->addWidget(m_sliders[a]);
    h->addWidget(cell, 1);

    connect(m_fields[a], &NumberField::committed, this, [this, a](double value) {
      if (m_sliders[a]->isVisible()) {
        QSignalBlocker b(m_sliders[a]);
        m_sliders[a]->setValue(int(std::lround(std::clamp(value, m_lo, m_hi) * 10.0)));
      }
      emit committed(a, value);
    });
    // The edit lands on release (one undo step); a track click or key step applies at once.
    const auto sliderValue = [this, a](int pos) {
      return std::clamp(snapTo(pos / 10.0, m_fields[a]->effectiveStep()), m_lo, m_hi);
    };
    connect(m_sliders[a], &QSlider::valueChanged, this, [this, a, sliderValue](int pos) {
      const double v = sliderValue(pos);
      m_fields[a]->setValueQuiet(v);
      if (!m_sliders[a]->isSliderDown())
        emit committed(a, v);
    });
    connect(m_sliders[a], &QSlider::sliderReleased, this,
            [this, a, sliderValue] { emit committed(a, sliderValue(m_sliders[a]->value())); });
  }
  v->addWidget(box);
}

void AxisRow::setSliderRange(const double lo, const double hi) {
  m_lo = lo;
  m_hi = hi;
  for (int a = 0; a < 3; ++a) {
    QSlider *s = m_sliders[a];
    s->setRange(int(std::lround(lo * 10.0)), int(std::lround(hi * 10.0)));
    s->setSingleStep(std::max(1, int(std::lround(m_fields[a]->effectiveStep() * 10.0))));
    s->setPageStep(150);
    s->show();
  }
}

void AxisRow::setReset(const QString &tooltip) {
  m_reset->setToolTip(tooltip);
  m_reset->setVisible(!tooltip.isEmpty());
}

void AxisRow::setValuesQuiet(const double v[3], const bool mixed[3]) {
  for (int a = 0; a < 3; ++a) {
    if (m_fields[a]->hasFocus() || m_sliders[a]->isSliderDown())
      continue;
    const bool mx = mixed && mixed[a];
    m_fields[a]->setValueQuiet(v[a], mx);
    if (m_sliders[a]->isVisible() || m_hi > m_lo) {
      QSignalBlocker b(m_sliders[a]);
      m_sliders[a]->setValue(int(std::lround(std::clamp(mx ? 0.0 : v[a], m_lo, m_hi) * 10.0)));
    }
  }
}

void AxisRow::setFieldsEnabled(const bool on) {
  for (int a = 0; a < 3; ++a) {
    m_fields[a]->setEnabled(on);
    m_sliders[a]->setEnabled(on);
  }
  m_reset->setEnabled(on);
}

QIcon subItemGlyph(int size) {
  const qreal dpr = qApp->devicePixelRatio();
  QPixmap pm(QSize(size, size) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QPen pen(QColor(0x4a, 0x54, 0x86));
  pen.setWidthF(std::max(1.0, size / 12.0));
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  const qreal x = size * 0.3;
  const qreal y = size * 0.55;
  const qreal r = size * 0.18;
  QPainterPath path;
  path.moveTo(x, size * 0.08);
  path.lineTo(x, y - r);
  path.quadTo(x, y, x + r, y);
  path.lineTo(size * 0.9, y);
  p.drawPath(path);
  p.end();
  QIcon icon;
  icon.addPixmap(pm, QIcon::Normal);
  icon.addPixmap(pm, QIcon::Selected);
  icon.addPixmap(pm, QIcon::Active);
  return icon;
}

} // namespace ui
