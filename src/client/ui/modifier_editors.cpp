#include "modifier_editors.h"

#include <cmath>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolButton>

#include "client/engine/modifiers.h"
#include "client/engine/scene.h"
#include "client/ui/scale.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

constexpr double kUnits = vk::Engine3D::kSourceUnitsPerWorldUnit; // world -> Source units

// A label | widget form whose rows can be hidden.
class Form {
public:
  explicit Form(QWidget *owner) : m_grid(new QGridLayout(owner)) {
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(6);
    m_grid->setColumnStretch(1, 1);
  }
  QWidget *row(const QString &label, QWidget *w, const QString &tip = {}) {
    const int r = m_grid->rowCount();
    if (!label.isEmpty()) {
      auto *l = new QLabel(label, w->parentWidget());
      l->setStyleSheet(QStringLiteral("color:#aab4cf;"));
      l->setToolTip(tip);
      m_grid->addWidget(l, r, 0);
      m_grid->addWidget(w, r, 1);
      w->setProperty("formLabel", QVariant::fromValue<QObject *>(l));
    } else {
      m_grid->addWidget(w, r, 0, 1, 2);
    }
    if (!tip.isEmpty())
      w->setToolTip(tip);
    return w;
  }
  static void show(QWidget *w, bool on) {
    w->setVisible(on);
    if (auto *l = qobject_cast<QWidget *>(w->property("formLabel").value<QObject *>()))
      l->setVisible(on);
  }

private:
  QGridLayout *m_grid;
};

NumberField *number(QWidget *parent, double lo, double hi, int decimals, double step, const QString &suffix = {}) {
  auto *f = new NumberField(parent);
  f->setRange(lo, hi);
  f->setDecimals(decimals);
  f->setSingleStep(step);
  if (!suffix.isEmpty())
    f->setSuffix(suffix);
  return f;
}

QComboBox *combo(QWidget *parent, const QStringList &items) {
  auto *c = new QComboBox(parent);
  c->addItems(items);
  return c;
}

void setQuiet(QComboBox *c, int i) {
  if (c->currentIndex() == i)
    return;
  QSignalBlocker b(c);
  c->setCurrentIndex(i);
}

void setQuiet(QCheckBox *c, bool on) {
  QSignalBlocker b(c);
  c->setChecked(on);
}

void setQuiet(NumberField *f, double v) {
  if (!f->hasFocus())
    f->setValueQuiet(v);
}

//
// One-axis offsets: Direction (+X … −Z, object frame, Y up), Step (× size: 1 = end to end), Gap (Source
// units, negative overlaps). Other offsets are x / y / z under Custom offset.

// The array's offset as one direction + step + gap, when it is one.
struct ArrayLine {
  bool ok = false;
  int axis = 0;    // 0 X, 1 Y, 2 Z
  int sign = 1;
  double step = 1.0;  // × size
  double gap = 0.0;   // Source units
};

ArrayLine lineOf(const eng::mods::ArrayParams &p) {
  const glm::dvec3 r = p.useRelative ? p.relative : glm::dvec3(0.0);
  const glm::dvec3 c = p.useConstant ? p.constant : glm::dvec3(0.0);
  constexpr double eps = 1e-9;
  ArrayLine l;
  int axis = -1;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(r[a]) <= eps && std::fabs(c[a]) <= eps)
      continue;
    if (axis >= 0)
      return l;  // more than one axis: custom
    axis = a;
  }
  l.ok = true;
  if (axis < 0)
    return l;  // no offset at all: +X, step 0
  l.axis = axis;
  l.sign = (std::fabs(r[axis]) > eps ? r[axis] : c[axis]) < 0.0 ? -1 : 1;
  l.step = r[axis] * l.sign;
  l.gap = c[axis] * l.sign * kUnits;
  if (l.step < -eps)
    l.ok = false;  // relative and constant pull opposite ways and the step leads: custom
  return l;
}

void applyLine(eng::mods::ArrayParams &p, const ArrayLine &l) {
  glm::dvec3 dir(0.0);
  dir[l.axis] = l.sign;
  p.useRelative = true;
  p.relative = dir * std::max(0.0, l.step);
  p.useConstant = std::fabs(l.gap) > 1e-9;
  p.constant = p.useConstant ? dir * (l.gap / kUnits) : glm::dvec3(0.0);
}

class ArrayEditor final : public ModifierEditor {
public:
  explicit ArrayEditor(QWidget *parent) : ModifierEditor(parent) {
    Form form(this);
    m_fit = combo(this, {tr("Fixed count"), tr("Fit length"), tr("Fit curve")});
    form.row(tr("Fit"), m_fit, tr("Number of copies: fixed, as many as fit a length, or along the curve"));
    m_count = number(this, 1, 1000, 0, 1);
    form.row(tr("Count"), m_count);
    m_length = number(this, 1, 1e6, 1, 16, tr(" u"));
    form.row(tr("Length"), m_length, tr("Length to fill with copies"));

    auto *dirBox = new QWidget(this);
    auto *dh = new QHBoxLayout(dirBox);
    dh->setContentsMargins(0, 0, 0, 0);
    dh->setSpacing(0);
    m_dirs = new QButtonGroup(this);
    m_dirs->setExclusive(false);  // none lit for a custom offset
    struct D {
      const char *text;
      const char *color;
      const char *tip;
    };
    const D dirs[6] = {{"+X", "#f07a72", QT_TR_NOOP("Repeat along +X")}, {"−X", "#f07a72", QT_TR_NOOP("Repeat along −X")},
                       {"+Y", "#8fd16a", QT_TR_NOOP("Repeat upward (+Y)")}, {"−Y", "#8fd16a", QT_TR_NOOP("Repeat downward (−Y)")},
                       {"+Z", "#7aa8ff", QT_TR_NOOP("Repeat along +Z")}, {"−Z", "#7aa8ff", QT_TR_NOOP("Repeat along −Z")}};
    for (int i = 0; i < 6; ++i) {
      auto *b = new QToolButton(dirBox);
      b->setText(QString::fromUtf8(dirs[i].text));
      b->setToolTip(tr(dirs[i].tip));
      b->setCheckable(true);
      b->setProperty("pos", i == 0 ? "first" : i == 5 ? "last" : "mid");
      b->setStyleSheet(QStringLiteral("QToolButton { color: %1; } QToolButton:checked { color: white; }")
                           .arg(QLatin1String(dirs[i].color)));
      b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      m_dirs->addButton(b, i);
      dh->addWidget(b);
    }
    scale::setScaledStyleSheet(dirBox, QStringLiteral(
        "QToolButton { background: #0f1328; border: 1px solid #2a3360; border-left: none; padding: 3px 4px; "
        "font-weight: bold; }"
        "QToolButton[pos=\"first\"] { border-left: 1px solid #2a3360; border-top-left-radius: 4px; "
        "border-bottom-left-radius: 4px; }"
        "QToolButton[pos=\"last\"] { border-top-right-radius: 4px; border-bottom-right-radius: 4px; }"
        "QToolButton:hover { background: #171C38; }"
        "QToolButton:checked { background: #4866C5; border-color: #6A84D9; color: white; }"));
    form.row(tr("Direction"), dirBox);
    m_step = number(this, 0, 1000, 3, 0.25, tr(" × size"));
    form.row(tr("Step"), m_step, tr("Distance between copies in object sizes (1 = end to end)"));
    m_gap = number(this, -1e6, 1e6, 1, 8, tr(" u"));
    form.row(tr("Gap"), m_gap, tr("Extra space between copies (negative overlaps)"));

    m_custom = new QToolButton(this);
    m_custom->setText(tr("Custom offset"));
    m_custom->setCheckable(true);
    m_custom->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_custom->setArrowType(Qt::RightArrow);
    m_custom->setAutoRaise(true);
    form.row(QString(), m_custom);
    m_useRel = new QCheckBox(tr("Relative offset"), this);
    m_useRel->setToolTip(tr("Offset in object sizes (1 = end to end)"));
    form.row(QString(), m_useRel);
    m_rel = new AxisRow(QString(), QString(), 3, 0.1, this);
    form.row(QString(), m_rel);
    m_useConst = new QCheckBox(tr("Constant offset"), this);
    m_useConst->setToolTip(tr("Offset in Source units"));
    form.row(QString(), m_useConst);
    m_const = new AxisRow(QString(), tr("u"), 1, 16.0, this);
    form.row(QString(), m_const);

    m_tex = combo(this, {tr("Follow the copies"), tr("World aligned")});
    form.row(tr("Textures"), m_tex);

    connect(m_fit, &QComboBox::activated, this, [this](int i) {
      m_p.fit = eng::mods::ArrayParams::Fit(i);
      emitChange({});
    });
    connect(m_count, &NumberField::committed, this, [this](double v) {
      m_p.count = int(std::lround(v));
      emitChange(QStringLiteral("count"));
    });
    connect(m_length, &NumberField::committed, this, [this](double v) {
      m_p.length = v / kUnits;
      emitChange(QStringLiteral("length"));
    });
    connect(m_dirs, &QButtonGroup::idClicked, this, [this](int id) {
      ArrayLine l = lineOf(m_p);
      if (!l.ok) {
        // From a custom offset: keep its sizes, along the picked axis.
        l.step = m_p.useRelative ? glm::length(m_p.relative) : 0.0;
        l.gap = m_p.useConstant ? glm::length(m_p.constant) * kUnits : 0.0;
        if (l.step <= 1e-9 && std::fabs(l.gap) <= 1e-9)
          l.step = 1.0;
      }
      l.axis = id / 2;
      l.sign = id % 2 ? -1 : 1;
      applyLine(m_p, l);
      emitChange({});
    });
    connect(m_step, &NumberField::committed, this, [this](double v) {
      ArrayLine l = lineOf(m_p);
      if (!l.ok)
        return;
      l.step = v;
      applyLine(m_p, l);
      emitChange(QStringLiteral("relative"));
    });
    connect(m_gap, &NumberField::committed, this, [this](double v) {
      ArrayLine l = lineOf(m_p);
      if (!l.ok)
        return;
      l.gap = v;
      applyLine(m_p, l);
      emitChange(QStringLiteral("constant"));
    });
    connect(m_custom, &QToolButton::toggled, this, [this](bool on) {
      m_customOpen = on;
      load(m_p.toJson());
    });
    connect(m_useRel, &QCheckBox::toggled, this, [this](bool on) {
      m_p.useRelative = on;
      emitChange({});
    });
    connect(m_rel, &AxisRow::committed, this, [this](int a, double v) {
      m_p.relative[a] = v;
      emitChange(QStringLiteral("relative"));
    });
    connect(m_useConst, &QCheckBox::toggled, this, [this](bool on) {
      m_p.useConstant = on;
      emitChange({});
    });
    connect(m_const, &AxisRow::committed, this, [this](int a, double v) {
      m_p.constant[a] = v / kUnits;
      emitChange(QStringLiteral("constant"));
    });
    connect(m_tex, &QComboBox::activated, this, [this](int i) {
      m_p.textureFollow = i == 0;
      emitChange({});
    });
  }

  void load(const QJsonObject &params) override {
    m_p = eng::mods::ArrayParams::fromJson(params);
    setQuiet(m_fit, int(m_p.fit));
    setQuiet(m_count, m_p.count);
    setQuiet(m_length, m_p.length * kUnits);
    Form::show(m_count, m_p.fit == eng::mods::ArrayParams::Fit::Count);
    Form::show(m_length, m_p.fit == eng::mods::ArrayParams::Fit::Length);

    const ArrayLine l = lineOf(m_p);
    const int lit = l.ok ? l.axis * 2 + (l.sign < 0 ? 1 : 0) : -1;
    for (QAbstractButton *b: m_dirs->buttons()) {
      QSignalBlocker block(b);
      b->setChecked(m_dirs->id(b) == lit);
    }
    m_step->setEnabled(l.ok);
    m_gap->setEnabled(l.ok);
    if (l.ok) {
      setQuiet(m_step, l.step);
      setQuiet(m_gap, l.gap);
    }

    const bool open = m_customOpen || !l.ok;
    {
      QSignalBlocker block(m_custom);
      m_custom->setChecked(open);
    }
    m_custom->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    setQuiet(m_useRel, m_p.useRelative);
    const double rel[3] = {m_p.relative.x, m_p.relative.y, m_p.relative.z};
    m_rel->setValuesQuiet(rel);
    setQuiet(m_useConst, m_p.useConstant);
    const double con[3] = {m_p.constant.x * kUnits, m_p.constant.y * kUnits, m_p.constant.z * kUnits};
    m_const->setValuesQuiet(con);
    m_useRel->setVisible(open);
    m_useConst->setVisible(open);
    m_rel->setVisible(open && m_p.useRelative);
    m_const->setVisible(open && m_p.useConstant);
    setQuiet(m_tex, m_p.textureFollow ? 0 : 1);
  }

private:
  void emitChange(const QString &key) {
    emit changed(m_p.toJson(), key);
    load(m_p.toJson());
  }
  eng::mods::ArrayParams m_p;
  bool m_customOpen = false;
  QComboBox *m_fit, *m_tex;
  NumberField *m_count, *m_length, *m_step, *m_gap;
  QButtonGroup *m_dirs;
  QToolButton *m_custom;
  QCheckBox *m_useRel, *m_useConst;
  AxisRow *m_rel, *m_const;
};

class CurveEditor final : public ModifierEditor {
public:
  CurveEditor(vk::Engine3D *engine, QWidget *parent) : ModifierEditor(parent), m_engine(engine) {
    Form form(this);
    m_path = new QComboBox(this);
    form.row(tr("Path"), m_path, tr("The path the object follows (draw one with the Path tool)"));
    m_axis = combo(this, {QStringLiteral("+X"), QStringLiteral("−X"), QStringLiteral("+Y"), QStringLiteral("−Y"),
                          QStringLiteral("+Z"), QStringLiteral("−Z")});
    form.row(tr("Deform axis"), m_axis, tr("The object's axis that runs along the path"));
    m_mode = combo(this, {tr("Bend"), tr("Rigid pieces")});
    form.row(tr("Mode"), m_mode,
             tr("Bend: the shape follows the curve (cut into segments as it turns).\n"
                "Rigid pieces: every brush keeps its exact shape and is only placed and turned."));
    m_unit = combo(this, {tr("Each brush"), tr("Each array copy")});
    form.row(tr("Pieces"), m_unit);
    m_frame = combo(this, {tr("Keep vertical"), tr("Follow slope")});
    form.row(tr("Up"), m_frame,
             tr("Keep vertical: walls stay upright and floors level across the path while it climbs.\n"
                "Follow slope: the object pitches with the path."));
    m_anchor = combo(this, {tr("Bottom"), tr("Center"), tr("Top")});
    form.row(tr("On the path"), m_anchor, tr("Which height of the object sits on the path"));
    m_along = number(this, -1e6, 1e6, 1, 16, tr(" u"));
    form.row(tr("Offset along"), m_along);
    m_side = number(this, -1e6, 1e6, 1, 16, tr(" u"));
    form.row(tr("Offset side"), m_side);
    m_height = number(this, -1e6, 1e6, 1, 16, tr(" u"));
    form.row(tr("Offset height"), m_height);
    m_segAngle = number(this, 0.5, 90, 1, 1, QStringLiteral("°"));
    form.row(tr("Segment every"), m_segAngle, tr("Bend: a cut every this much turning (smaller = smoother)"));
    m_segLen = number(this, 0, 1e6, 1, 16, tr(" u"));
    form.row(tr("Max segment"), m_segLen, tr("Bend: cut at least this often (0 = only where it turns)"));
    m_stretch = new QCheckBox(tr("Stretch to fit the path"), this);
    m_stretch->setToolTip(tr("Scale along the path so the object spans it exactly (with Array: Fit curve)"));
    form.row(QString(), m_stretch);
    m_reverse = new QCheckBox(tr("Reverse direction"), this);
    form.row(QString(), m_reverse);
    // Order matches CurveParams::Texture (Path, Bend, World).
    m_tex = combo(this, {tr("Follow the path"), tr("Follow the bend"), tr("World aligned")});
    m_tex->setItemData(0, tr("True size along the path: walls unrolled, their lines following a ramp"), Qt::ToolTipRole);
    m_tex->setItemData(1, tr("Carried by the brushes as they bend and stretch"), Qt::ToolTipRole);
    form.row(tr("Textures"), m_tex);

    connect(m_path, &QComboBox::activated, this, [this](int i) {
      m_p.path = m_path->itemData(i).toUInt();
      emitChange({});
    });
    auto comboSet = [this](QComboBox *c, auto setter) {
      connect(c, &QComboBox::activated, this, [this, setter](int i) {
        setter(i);
        emitChange({});
      });
    };
    comboSet(m_axis, [this](int i) { m_p.axis = i; });
    comboSet(m_mode, [this](int i) { m_p.mode = eng::mods::CurveParams::Mode(i); });
    comboSet(m_unit, [this](int i) { m_p.rigidUnit = eng::mods::CurveParams::Unit(i); });
    comboSet(m_frame, [this](int i) { m_p.frame = eng::mods::CurveParams::Frame(i); });
    comboSet(m_anchor, [this](int i) { m_p.anchor = eng::mods::CurveParams::Anchor(i); });
    comboSet(m_tex, [this](int i) { m_p.texture = eng::mods::CurveParams::Texture(i); });
    auto numSet = [this](NumberField *f, const QString &key, auto setter) {
      connect(f, &NumberField::committed, this, [this, key, setter](double v) {
        setter(v);
        emitChange(key);
      });
    };
    numSet(m_along, QStringLiteral("along"), [this](double v) { m_p.offsetAlong = v / kUnits; });
    numSet(m_side, QStringLiteral("side"), [this](double v) { m_p.offsetSide = v / kUnits; });
    numSet(m_height, QStringLiteral("height"), [this](double v) { m_p.offsetHeight = v / kUnits; });
    numSet(m_segAngle, QStringLiteral("segAngle"), [this](double v) { m_p.segAngle = v; });
    numSet(m_segLen, QStringLiteral("segLen"), [this](double v) { m_p.segMaxLen = v / kUnits; });
    connect(m_stretch, &QCheckBox::toggled, this, [this](bool on) {
      m_p.stretch = on;
      emitChange({});
    });
    connect(m_reverse, &QCheckBox::toggled, this, [this](bool on) {
      m_p.reverse = on;
      emitChange({});
    });
  }

  void load(const QJsonObject &params) override {
    m_p = eng::mods::CurveParams::fromJson(params);
    // The path list (refilled: names change, paths come and go).
    {
      QSignalBlocker b(m_path);
      m_path->clear();
      m_path->addItem(tr("— none —"), 0u);
      if (m_engine && m_engine->scene())
        for (const auto &p: m_engine->scene()->paths)
          m_path->addItem(p.name.isEmpty() ? tr("Path %1").arg(p.id) : p.name, p.id);
      const int i = m_path->findData(m_p.path);
      m_path->setCurrentIndex(i < 0 ? 0 : i);
    }
    setQuiet(m_axis, m_p.axis);
    setQuiet(m_mode, int(m_p.mode));
    setQuiet(m_unit, int(m_p.rigidUnit));
    setQuiet(m_frame, int(m_p.frame));
    setQuiet(m_anchor, int(m_p.anchor));
    setQuiet(m_along, m_p.offsetAlong * kUnits);
    setQuiet(m_side, m_p.offsetSide * kUnits);
    setQuiet(m_height, m_p.offsetHeight * kUnits);
    setQuiet(m_segAngle, m_p.segAngle);
    setQuiet(m_segLen, m_p.segMaxLen * kUnits);
    setQuiet(m_stretch, m_p.stretch);
    setQuiet(m_reverse, m_p.reverse);
    setQuiet(m_tex, int(m_p.texture));
    const bool bend = m_p.mode == eng::mods::CurveParams::Mode::Bend;
    Form::show(m_unit, !bend);
    Form::show(m_segAngle, bend);
    Form::show(m_segLen, bend);
  }

private:
  void emitChange(const QString &key) {
    emit changed(m_p.toJson(), key);
    load(m_p.toJson());
  }
  vk::Engine3D *m_engine;
  eng::mods::CurveParams m_p;
  QComboBox *m_path, *m_axis, *m_mode, *m_unit, *m_frame, *m_anchor, *m_tex;
  NumberField *m_along, *m_side, *m_height, *m_segAngle, *m_segLen;
  QCheckBox *m_stretch, *m_reverse;
};

} // namespace

ModifierEditor *makeModifierEditor(const QString &kind, vk::Engine3D *engine, QWidget *parent) {
  if (kind == QLatin1String("array"))
    return new ArrayEditor(parent);
  if (kind == QLatin1String("curve"))
    return new CurveEditor(engine, parent);
  return nullptr;
}

} // namespace ui
