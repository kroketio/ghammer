#include "lut_step_editors.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <optional>
#include <cmath>

#include "client/ui/lut_levels.h"
#include "client/ui/scale.h"
#include "client/ui/widgets.h"

namespace ui {

using Kind = eng::GradeStep::Kind;
using eng::GradeStep;

QString gradeKindName(Kind kind) {
  switch (kind) {
  case Kind::Basic:
    return QObject::tr("Basic");
  case Kind::Levels:
    return QObject::tr("Levels");
  case Kind::Curves:
    return QObject::tr("Curves");
  case Kind::ColorBalance:
    return QObject::tr("Color balance");
  case Kind::Hsv:
    return QObject::tr("HSV/HSL");
  case Kind::Cdl:
    return QObject::tr("Slope/Offset/Power");
  case Kind::Count:
    break;
  }
  return {};
}

StepEditor::StepEditor(EditFn edit, QWidget *parent) : QWidget(parent), m_edit(std::move(edit)) {
  setObjectName(QStringLiteral("dlgPage"));
  m_v = new QVBoxLayout(this);
  m_v->setContentsMargins(0, 0, 4, 0);
  m_v->setSpacing(6);
}

void StepEditor::load(const GradeStep &s) {
  for (const auto &b: m_binders)
    b(s);
}

QVBoxLayout *StepEditor::card(const QString &heading) {
  if (!heading.isEmpty()) {
    auto *l = new QLabel(heading, this);
    l->setObjectName(QStringLiteral("dlgSection"));
    m_v->addWidget(l);
  }
  auto *c = new QFrame(this);
  c->setObjectName(QStringLiteral("dlgCard"));
  auto *cl = new QVBoxLayout(c);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(0);
  m_v->addWidget(c);
  return cl;
}

QFrame *StepEditor::row(QVBoxLayout *card, const QString &label, QWidget *editor, QWidget *trailing) {
  auto *r = new QFrame(card->parentWidget());
  r->setObjectName(QStringLiteral("dlgRow"));
  r->setProperty("first", card->count() == 0);
  auto *h = new QHBoxLayout(r);
  h->setContentsMargins(14, 4, 8, 4);
  auto *l = new QLabel(label, r);
  l->setFixedWidth(l->fontMetrics().horizontalAdvance(QObject::tr("Magenta–Green")) + 12);
  h->addWidget(l);
  editor->setParent(r);
  h->addWidget(editor, 1);
  if (trailing) {
    trailing->setParent(r);
    h->addWidget(trailing);
  }
  card->addWidget(r);
  return r;
}

QFrame *StepEditor::slider(QVBoxLayout *card, const QString &label, double lo, double hi, int decimals,
                           const QString &suffix, Get get, Set set, double neutral, const QString &key) {
  auto *f = new SliderField(lo, hi, decimals, suffix, 1.0, this);
  auto *dot = new ResetDot(this);
  QFrame *r = row(card, label, f, dot);
  connect(f, &SliderField::committed, this, [this, set, key](double v) {
    m_edit(key, [&](GradeStep &s) { set(s, v); });
  });
  connect(dot, &QToolButton::clicked, this, [this, set, neutral, key] {
    m_edit(key, [&](GradeStep &s) { set(s, neutral); });
  });
  onLoad([f, dot, get, neutral](const GradeStep &s) {
    const double v = get(s);
    f->setValueQuiet(v);
    dot->setChanged(std::abs(v - neutral) > 1e-9);
  });
  return r;
}

void StepEditor::colour(QVBoxLayout *card, const QString &label, std::function<QColor(const GradeStep &)> get,
                        std::function<void(GradeStep &, const QColor &)> set, const QColor &neutral,
                        const QString &key) {
  auto *box = new QWidget(this);
  auto *bh = new QHBoxLayout(box);
  bh->setContentsMargins(0, 0, 0, 0);
  auto *b = new ColorButton(box);
  scale::bind(b, [b] { b->setMinimumWidth(scale::tpx(120)); });
  bh->addWidget(b);
  bh->addStretch(1);
  auto *dot = new ResetDot(this);
  row(card, label, box, dot);
  connect(b, &ColorButton::colorPicked, this,
          [this, set, key](const QColor &c) { m_edit(key, [&](GradeStep &s) { set(s, c); }); });
  connect(dot, &QToolButton::clicked, this,
          [this, set, neutral, key] { m_edit(key, [&](GradeStep &s) { set(s, neutral); }); });
  onLoad([b, dot, get, neutral](const GradeStep &s) {
    b->setColor(get(s));
    dot->setChanged(get(s) != neutral);
  });
}

void StepEditor::addResetAll(std::function<void(GradeStep &)> reset) {
  auto *h = new QHBoxLayout;
  auto *b = new QPushButton(QObject::tr("Reset all"), this);
  b->setObjectName(QStringLiteral("dlgFlat"));
  connect(b, &QPushButton::clicked, this, [this, reset] { m_edit(QStringLiteral("reset"), reset); });
  h->addStretch(1);
  h->addWidget(b);
  m_v->addLayout(h);
}

namespace {

StepEditor *basicEditor(StepEditor::EditFn edit, QWidget *parent) {
  struct E : StepEditor {
    E(EditFn e, QWidget *p) : StepEditor(std::move(e), p) {
      using A = eng::LutAdjustments;
      const auto add = [this](QVBoxLayout *c, const QString &label, double lo, double hi, int dec, double A::*m,
                              const char *key, const QString &suffix = {}) {
        slider(c, label, lo, hi, dec, suffix, [m](const GradeStep &s) { return s.basic.*m; },
               [m](GradeStep &s, double v) { s.basic.*m = v; }, A().*m, QString::fromLatin1(key));
      };
      QVBoxLayout *light = card(QObject::tr("LIGHT"));
      add(light, QObject::tr("Exposure"), -3.0, 3.0, 2, &A::exposure, "exposure");
      add(light, QObject::tr("Contrast"), 0.0, 2.0, 2, &A::contrast, "contrast");
      add(light, QObject::tr("Gamma"), 0.2, 3.0, 2, &A::gamma, "gamma");
      QVBoxLayout *col = card(QObject::tr("COLOUR"));
      add(col, QObject::tr("Saturation"), 0.0, 2.0, 2, &A::saturation, "saturation");
      add(col, QObject::tr("Vibrance"), -1.0, 1.0, 2, &A::vibrance, "vibrance");
      add(col, QObject::tr("Hue shift"), -180.0, 180.0, 1, &A::hue, "hue", QStringLiteral("°"));
      add(col, QObject::tr("Temperature"), -1.0, 1.0, 2, &A::temperature, "temperature");
      add(col, QObject::tr("Tint"), -1.0, 1.0, 2, &A::tint, "tint");
      QVBoxLayout *ton = card(QObject::tr("SPLIT TONING"));
      colour(ton, QObject::tr("Shadows"), [](const GradeStep &s) { return s.basic.shadows; },
             [](GradeStep &s, const QColor &c) { s.basic.shadows = c; }, A().shadows, QStringLiteral("shadows"));
      colour(ton, QObject::tr("Highlights"), [](const GradeStep &s) { return s.basic.highlights; },
             [](GradeStep &s, const QColor &c) { s.basic.highlights = c; }, A().highlights,
             QStringLiteral("highlights"));
      add(ton, QObject::tr("Strength"), 0.0, 1.0, 2, &A::toningStrength, "toningStrength");
      addResetAll([](GradeStep &s) { s.basic = {}; });
      m_v->addStretch(1);
    }
  };
  return new E(std::move(edit), parent);
}

StepEditor *balanceEditor(StepEditor::EditFn edit, QWidget *parent) {
  struct E : StepEditor {
    E(EditFn e, QWidget *p) : StepEditor(std::move(e), p) {
      using B = eng::LutColorBalance;
      const auto range = [this](const QString &heading, glm::dvec3 B::*m, const char *key) {
        QVBoxLayout *c = card(heading);
        const QString names[3] = {QObject::tr("Cyan–Red"), QObject::tr("Magenta–Green"), QObject::tr("Yellow–Blue")};
        for (int i = 0; i < 3; ++i)
          slider(c, names[i], -100.0, 100.0, 0, {}, [m, i](const GradeStep &s) { return (s.balance.*m)[i] * 100.0; },
                 [m, i](GradeStep &s, double v) { (s.balance.*m)[i] = v / 100.0; }, 0.0,
                 QStringLiteral("%1.%2").arg(QLatin1String(key)).arg(i));
      };
      range(QObject::tr("SHADOWS"), &B::shadows, "shadows");
      range(QObject::tr("MIDTONES"), &B::midtones, "midtones");
      range(QObject::tr("HIGHLIGHTS"), &B::highlights, "highlights");
      auto *preserve = new QCheckBox(QObject::tr("Preserve luminosity"), this);
      m_v->addWidget(preserve);
      connect(preserve, &QCheckBox::clicked, this, [this](bool on) {
        m_edit(QStringLiteral("preserve"), [on](GradeStep &s) { s.balance.preserveLuminosity = on; });
      });
      onLoad([preserve](const GradeStep &s) {
        const QSignalBlocker b(preserve);
        preserve->setChecked(s.balance.preserveLuminosity);
      });
      addResetAll([](GradeStep &s) { s.balance = {}; });
      m_v->addStretch(1);
    }
  };
  return new E(std::move(edit), parent);
}

StepEditor *hsvEditor(StepEditor::EditFn edit, QWidget *parent) {
  struct E : StepEditor {
    E(EditFn e, QWidget *p) : StepEditor(std::move(e), p) {
      QVBoxLayout *c = card({});
      auto *model = new QComboBox(this);
      model->addItems({QStringLiteral("HSV"), QStringLiteral("HSL"), QStringLiteral("HSI"), QStringLiteral("HSY")});
      row(c, QObject::tr("Model"), model);
      connect(model, &QComboBox::activated, this,
              [this](int m) { m_edit(QStringLiteral("model"), [m](GradeStep &s) { s.hsv.model = m; }); });
      QFrame *hue = slider(c, QObject::tr("Hue"), -180.0, 180.0, 0, QStringLiteral("°"),
                           [](const GradeStep &s) { return s.hsv.hue; }, [](GradeStep &s, double v) { s.hsv.hue = v; },
                           0.0, QStringLiteral("hue"));
      QFrame *hueC = slider(c, QObject::tr("Hue"), 0.0, 360.0, 0, QStringLiteral("°"),
                            [](const GradeStep &s) { return s.hsv.hue; },
                            [](GradeStep &s, double v) { s.hsv.hue = v; }, 0.0, QStringLiteral("hue"));
      QFrame *sat = slider(c, QObject::tr("Saturation"), -100.0, 100.0, 0, {},
                           [](const GradeStep &s) { return s.hsv.saturation * 100.0; },
                           [](GradeStep &s, double v) { s.hsv.saturation = v / 100.0; }, 0.0,
                           QStringLiteral("saturation"));
      QFrame *satC = slider(c, QObject::tr("Saturation"), 0.0, 100.0, 0, {},
                            [](const GradeStep &s) { return s.hsv.saturation * 100.0; },
                            [](GradeStep &s, double v) { s.hsv.saturation = v / 100.0; }, 0.0,
                            QStringLiteral("saturation"));
      slider(c, QObject::tr("Value"), -100.0, 100.0, 0, {}, [](const GradeStep &s) { return s.hsv.value * 100.0; },
             [](GradeStep &s, double v) { s.hsv.value = v / 100.0; }, 0.0, QStringLiteral("value"));
      auto *colorize = new QCheckBox(QObject::tr("Colorize"), this);
      m_v->addWidget(colorize);
      connect(colorize, &QCheckBox::clicked, this, [this](bool on) {
        m_edit(QStringLiteral("colorize"), [on](GradeStep &s) {
          s.hsv.colorize = on;
          if (on) {
            s.hsv.hue = std::fmod(s.hsv.hue + 360.0, 360.0);
            s.hsv.saturation = std::clamp(s.hsv.saturation, 0.0, 1.0);
          } else {
            s.hsv.hue = s.hsv.hue > 180.0 ? s.hsv.hue - 360.0 : s.hsv.hue;
          }
        });
      });
      onLoad([=](const GradeStep &s) {
        {
          const QSignalBlocker b(model);
          model->setCurrentIndex(s.hsv.model);
        }
        {
          const QSignalBlocker b(colorize);
          colorize->setChecked(s.hsv.colorize);
        }
        model->setEnabled(!s.hsv.colorize);
        hue->setVisible(!s.hsv.colorize);
        sat->setVisible(!s.hsv.colorize);
        hueC->setVisible(s.hsv.colorize);
        satC->setVisible(s.hsv.colorize);
      });
      addResetAll([](GradeStep &s) { s.hsv = {}; });
      m_v->addStretch(1);
    }
  };
  return new E(std::move(edit), parent);
}

StepEditor *cdlEditor(StepEditor::EditFn edit, QWidget *parent) {
  struct E : StepEditor {
    E(EditFn e, QWidget *p) : StepEditor(std::move(e), p) {
      using C = eng::LutCdl;
      const auto triple = [this](const QString &heading, glm::dvec3 C::*m, double lo, double hi, double neutral,
                                 const char *key) {
        QVBoxLayout *c = card(heading);
        const QString names[3] = {QObject::tr("Red"), QObject::tr("Green"), QObject::tr("Blue")};
        for (int i = 0; i < 3; ++i)
          slider(c, names[i], lo, hi, 3, {}, [m, i](const GradeStep &s) { return (s.cdl.*m)[i]; },
                 [m, i](GradeStep &s, double v) { (s.cdl.*m)[i] = v; }, neutral,
                 QStringLiteral("%1.%2").arg(QLatin1String(key)).arg(i));
      };
      triple(QObject::tr("SLOPE"), &C::slope, 0.0, 4.0, 1.0, "slope");
      triple(QObject::tr("OFFSET"), &C::offset, -1.0, 1.0, 0.0, "offset");
      triple(QObject::tr("POWER"), &C::power, 0.1, 4.0, 1.0, "power");
      QVBoxLayout *c = card({});
      slider(c, QObject::tr("Saturation"), 0.0, 4.0, 3, {}, [](const GradeStep &s) { return s.cdl.saturation; },
             [](GradeStep &s, double v) { s.cdl.saturation = v; }, 1.0, QStringLiteral("saturation"));
      addResetAll([](GradeStep &s) { s.cdl = {}; });
      m_v->addStretch(1);
    }
  };
  return new E(std::move(edit), parent);
}

StepEditor *levelsEditor(StepEditor::EditFn edit, InputHistogramFn histogram, QWidget *parent) {
  struct E : StepEditor {
    InputHistogramFn hist;
    QComboBox *channel = nullptr;
    QToolButton *log = nullptr;
    HistogramView *view = nullptr;
    LevelsSlider *in = nullptr, *out = nullptr;
    NumberField *inBlack = nullptr, *gamma = nullptr, *inWhite = nullptr, *outBlack = nullptr, *outWhite = nullptr;
    int drag = 0; // one undo step per drag
    std::optional<GradeStep> last;

    [[nodiscard]] int ch() const { return std::max(0, channel->currentIndex()); }

    void editCh(const QString &field, const std::function<void(eng::LevelsCurve &)> &fn) {
      const int c = ch();
      m_edit(QStringLiteral("levels.%1.%2").arg(c).arg(field), [&](GradeStep &s) { fn(s.levels.ch[size_t(c)]); });
    }

    void show(const GradeStep &s) {
      last = s;
      const auto c = eng::GradeChannel(ch());
      const eng::LevelsCurve &lc = s.levels.ch[size_t(c)];
      view->setData(hist ? hist(c) : eng::grade::Histogram {}, log->isChecked(), gradeChannelColor(c));
      in->setValues(lc.inBlack, lc.inWhite, lc.gamma);
      out->setValues(lc.outBlack, lc.outWhite);
      const auto set = [](NumberField *f, double v) {
        if (!f->hasFocus())
          f->setValueQuiet(v);
      };
      set(inBlack, lc.inBlack * 255.0);
      set(inWhite, lc.inWhite * 255.0);
      set(gamma, lc.gamma);
      set(outBlack, lc.outBlack * 255.0);
      set(outWhite, lc.outWhite * 255.0);
    }

    E(EditFn e, InputHistogramFn h, QWidget *p) : StepEditor(std::move(e), p), hist(std::move(h)) {
      auto *top = new QHBoxLayout;
      top->setSpacing(6);
      top->addWidget(new QLabel(QObject::tr("Channel"), this));
      channel = new QComboBox(this);
      for (int c = 0; c < eng::kGradeChannels; ++c)
        channel->addItem(gradeChannelName(eng::GradeChannel(c)));
      top->addWidget(channel, 1);
      auto *group = new QButtonGroup(this);
      const auto toggle = [&](const QString &text) {
        auto *b = new QToolButton(this);
        b->setObjectName(QStringLiteral("dlgTool"));
        b->setText(text);
        b->setCheckable(true);
        group->addButton(b);
        top->addWidget(b);
        return b;
      };
      toggle(QObject::tr("Linear"))->setChecked(true);
      log = toggle(QObject::tr("Log"));
      m_v->addLayout(top);
      view = new HistogramView(this);
      m_v->addWidget(view);

      const auto field = [this](double lo, double hi, int decimals, double stepSize) {
        auto *f = new NumberField(this);
        f->setRange(lo, hi);
        f->setDecimals(decimals);
        f->setSingleStep(stepSize);
        return f;
      };
      m_v->addWidget(new QLabel(QObject::tr("Input"), this));
      in = new LevelsSlider(true, this);
      m_v->addWidget(in);
      auto *inRow = new QHBoxLayout;
      inBlack = field(0, 254, 0, 1);
      gamma = field(0.1, 10.0, 2, 0.01);
      inWhite = field(1, 255, 0, 1);
      inRow->addWidget(inBlack);
      inRow->addStretch(1);
      inRow->addWidget(gamma);
      inRow->addStretch(1);
      inRow->addWidget(inWhite);
      m_v->addLayout(inRow);
      m_v->addSpacing(6);
      m_v->addWidget(new QLabel(QObject::tr("Output"), this));
      out = new LevelsSlider(false, this);
      m_v->addWidget(out);
      auto *outRow = new QHBoxLayout;
      outBlack = field(0, 255, 0, 1);
      outWhite = field(0, 255, 0, 1);
      outRow->addWidget(outBlack);
      outRow->addStretch(1);
      outRow->addWidget(outWhite);
      m_v->addLayout(outRow);
      m_v->addSpacing(6);
      auto *buttons = new QHBoxLayout;
      const auto button = [this, buttons](const QString &text) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QStringLiteral("dlgFlat"));
        buttons->addWidget(b);
        return b;
      };
      QPushButton *autoB = button(QObject::tr("Auto"));
      buttons->addStretch(1);
      QPushButton *resetCh = button(QObject::tr("Reset channel"));
      QPushButton *resetAll = button(QObject::tr("Reset all"));
      m_v->addLayout(buttons);
      m_v->addStretch(1);

      const auto reshow = [this] {
        if (last)
          show(*last);
      };
      connect(channel, &QComboBox::currentIndexChanged, this, reshow);
      connect(group, &QButtonGroup::buttonClicked, this, reshow);
      in->onPressed = [this] { ++drag; };
      out->onPressed = [this] { ++drag; };
      in->onChanged = [this] {
        const double b = in->black(), w = in->white(), g = in->gamma();
        editCh(QStringLiteral("in%1").arg(drag), [=](eng::LevelsCurve &c) {
          c.inBlack = b;
          c.inWhite = w;
          c.gamma = g;
        });
      };
      out->onChanged = [this] {
        const double b = out->black(), w = out->white();
        editCh(QStringLiteral("out%1").arg(drag), [=](eng::LevelsCurve &c) {
          c.outBlack = b;
          c.outWhite = w;
        });
      };
      connect(inBlack, &NumberField::committed, this, [this](double v) {
        editCh(QStringLiteral("inBlack"),
               [v](eng::LevelsCurve &c) { c.inBlack = std::min(v / 255.0, c.inWhite - 1.0 / 255.0); });
      });
      connect(inWhite, &NumberField::committed, this, [this](double v) {
        editCh(QStringLiteral("inWhite"),
               [v](eng::LevelsCurve &c) { c.inWhite = std::max(v / 255.0, c.inBlack + 1.0 / 255.0); });
      });
      connect(gamma, &NumberField::committed, this, [this](double v) {
        editCh(QStringLiteral("gamma"), [v](eng::LevelsCurve &c) { c.gamma = std::clamp(v, 0.1, 10.0); });
      });
      connect(outBlack, &NumberField::committed, this,
              [this](double v) { editCh(QStringLiteral("outBlack"), [v](eng::LevelsCurve &c) { c.outBlack = v / 255.0; }); });
      connect(outWhite, &NumberField::committed, this,
              [this](double v) { editCh(QStringLiteral("outWhite"), [v](eng::LevelsCurve &c) { c.outWhite = v / 255.0; }); });
      connect(autoB, &QPushButton::clicked, this, [this] {
        const auto c = eng::GradeChannel(ch());
        const eng::LevelsCurve a = eng::grade::autoLevels(hist ? hist(c) : eng::grade::Histogram {}, c);
        editCh(QStringLiteral("auto%1").arg(++drag), [a](eng::LevelsCurve &lc) {
          lc.inBlack = a.inBlack;
          lc.inWhite = a.inWhite;
          lc.gamma = a.gamma;
        });
      });
      connect(resetCh, &QPushButton::clicked, this,
              [this] { editCh(QStringLiteral("reset%1").arg(++drag), [](eng::LevelsCurve &c) { c = {}; }); });
      connect(resetAll, &QPushButton::clicked, this, [this] {
        m_edit(QStringLiteral("levels.resetAll%1").arg(++drag), [](GradeStep &s) { s.levels = {}; });
      });
      onLoad([this](const GradeStep &s) { show(s); });
    }
  };
  return new E(std::move(edit), std::move(histogram), parent);
}

// Curves: the transfer and Edit…

QImage transferThumb(const GradeStep &s, int side) {
  QImage img(side, side, QImage::Format_ARGB32_Premultiplied);
  img.fill(QColor(12, 16, 34));
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(48, 56, 82), 1));
  for (int i = 1; i < 4; ++i) {
    p.drawLine(QPointF(side * i / 4.0, 0), QPointF(side * i / 4.0, side));
    p.drawLine(QPointF(0, side * i / 4.0), QPointF(side, side * i / 4.0));
  }
  p.setPen(QPen(QColor(80, 88, 116), 1, Qt::DashLine));
  p.drawLine(QPointF(0, side), QPointF(side, 0));
  for (int c = 0; c < eng::kGradeChannels; ++c) {
    const bool identity = s.kind == GradeStep::Kind::Levels ? s.levels.ch[size_t(c)].isIdentity()
                                                            : eng::isIdentityCurve(s.curves.ch[size_t(c)]);
    if (identity)
      continue;
    QPainterPath path;
    for (int i = 0; i <= side; ++i) {
      const double x = double(i) / side;
      const double y = s.kind == GradeStep::Kind::Levels ? s.levels.ch[size_t(c)].map(x)
                                                         : eng::curveValue(s.curves.ch[size_t(c)], x);
      const QPointF pt(i, side - std::clamp(y, 0.0, 1.0) * side);
      if (i == 0)
        path.moveTo(pt);
      else
        path.lineTo(pt);
    }
    p.setPen(QPen(gradeChannelColor(eng::GradeChannel(c)), 1.6));
    p.drawPath(path);
  }
  return img;
}

StepEditor *channelsEditor(StepEditor::EditFn edit, std::function<void()> openWindow, QWidget *parent) {
  struct E : StepEditor {
    E(EditFn e, std::function<void()> open, QWidget *p) : StepEditor(std::move(e), p) {
      auto *thumb = new QLabel(this);
      thumb->setAlignment(Qt::AlignCenter);
      m_v->addWidget(thumb);
      auto *h = new QHBoxLayout;
      auto *b = new QPushButton(QObject::tr("Edit…"), this);
      b->setObjectName(QStringLiteral("dlgPrimary"));
      connect(b, &QPushButton::clicked, this, [open] { open(); });
      h->addStretch(1);
      h->addWidget(b);
      h->addStretch(1);
      m_v->addLayout(h);
      m_v->addStretch(1);
      onLoad([thumb](const GradeStep &s) {
        thumb->setPixmap(QPixmap::fromImage(transferThumb(s, scale::px(200))));
      });
    }
  };
  return new E(std::move(edit), std::move(openWindow), parent);
}

} // namespace

StepEditor *makeStepEditor(Kind kind, StepEditor::EditFn edit, std::function<void()> openWindow,
                           InputHistogramFn histogram, QWidget *parent) {
  switch (kind) {
  case Kind::Levels:
    return levelsEditor(std::move(edit), std::move(histogram), parent);
  case Kind::Basic:
    return basicEditor(std::move(edit), parent);
  case Kind::ColorBalance:
    return balanceEditor(std::move(edit), parent);
  case Kind::Hsv:
    return hsvEditor(std::move(edit), parent);
  case Kind::Cdl:
    return cdlEditor(std::move(edit), parent);
  case Kind::Curves:
  case Kind::Count:
    break;
  }
  return channelsEditor(std::move(edit), std::move(openWindow), parent);
}

} // namespace ui
