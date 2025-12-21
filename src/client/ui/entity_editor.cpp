#include "entity_editor.h"
#include "client/engine/physics_world.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/ui/io_panel.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "client/ui/file_dialog.h"

namespace ui {

namespace {

constexpr double kSrc = vk::Engine3D::kSourceUnitsPerWorldUnit;

QColor toColor(const QVariant &v) {
  return v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor(v.toString());
}

// The property holds its class default (unset, or set to the same value).
bool atDefault(const eng::PropDef &pd, const eng::Entity &e) {
  if (!e.props.contains(pd.key))
    return true;
  const QVariant v = e.props.value(pd.key), d = pd.defaultValue;
  switch (pd.type) {
    case eng::PropType::Bool: return v.toBool() == d.toBool();
    case eng::PropType::Color: {
      const QColor a = toColor(v), b = toColor(d);
      return a.isValid() && b.isValid() ? a.rgba() == b.rgba() : v.toString() == d.toString();
    }
    case eng::PropType::String:
    case eng::PropType::Choice:
    case eng::PropType::File:
    case eng::PropType::Target: return v.toString() == d.toString();
    default: {
      bool okA = false, okB = false;
      const double a = v.toDouble(&okA), b = d.toDouble(&okB);
      if (okA && okB)
        return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(b));
      return v.toString() == d.toString();
    }
  }
}

// Degrees typed past a half turn wrap into (-180, 180].
double wrapDegrees(const double v) {
  double d = std::fmod(v, 360.0);
  if (d <= -180.0)
    d += 360.0;
  else if (d > 180.0)
    d -= 360.0;
  return d;
}

QLabel *rowLabel(const QString &text, QWidget *parent) {
  auto *l = new QLabel(text, parent);
  l->setStyleSheet(QStringLiteral("color:#aab4cf;"));
  return l;
}

} // namespace

void EntityEditor::Row::setVisible(const bool on) const {
  for (QWidget *w: {static_cast<QWidget *>(label), editor, static_cast<QWidget *>(dot)})
    if (w)
      w->setVisible(on);
}

EntityEditor::EntityEditor(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  // label | editor | reset dot
  m_grid = new QGridLayout();
  m_grid->setContentsMargins(0, 0, 0, 0);
  m_grid->setHorizontalSpacing(10);
  m_grid->setVerticalSpacing(7);
  m_grid->setColumnStretch(1, 1);
  v->addLayout(m_grid);
}

void EntityEditor::setEntities(const std::vector<quint32> &ids) {
  m_ids = ids;
  m_id = ids.empty() ? 0 : ids.front();
  const eng::Entity *e = m_engine->entityById(m_id);
  // One entity: its own rows (name, transform); several: the class's rows.
  const QString key = !e ? QString()
                      : ids.size() == 1 ? QStringLiteral("%1:%2").arg(m_id).arg(e->classname)
                                        : QStringLiteral("multi:%1").arg(e->classname);
  if (key != m_builtFor)
    rebuild();
  refresh();
}

void EntityEditor::setProp(const QString &key, const QVariant &value) {
  m_engine->setEntitiesProperty(m_ids, key, value);
}

void EntityEditor::resetProp(const QString &key) {
  m_engine->resetEntitiesProperty(m_ids, key);
}

void EntityEditor::clear() {
  while (QLayoutItem *item = m_grid->takeAt(0)) {
    if (QWidget *w = item->widget()) {
      w->hide();
      w->deleteLater();
    }
    delete item;
  }
  m_refreshers.clear();
  m_rows.clear();
  m_sequences.clear();
  m_name = nullptr;
  m_refs = nullptr;
  m_collision = nullptr;
  m_origin = m_angles = nullptr;
}

void EntityEditor::rebuild() {
  clear();
  const eng::Entity *e = m_engine->entityById(m_id);
  const bool multi = m_ids.size() > 1;
  m_builtFor = !e ? QString() : multi ? QStringLiteral("multi:%1").arg(e->classname)
                                      : QStringLiteral("%1:%2").arg(m_id).arg(e->classname);
  if (!e)
    return;
  const eng::EntityClass *cls = e->cls();
  const quint32 id = m_id;
  int row = 0;
  if (!multi) {
    // Name (targetname) and its incoming connection count.
    auto *nameRow = new QWidget(this);
    auto *nh = new QHBoxLayout(nameRow);
    nh->setContentsMargins(0, 0, 0, 0);
    m_name = new QLineEdit(nameRow);
    m_name->setPlaceholderText(tr("targetname"));
    m_name->setToolTip(tr("The name other entities' outputs use to address this one. It need not be unique: "
                          "every entity with the name is addressed together."));
    nh->addWidget(m_name, 1);
    m_refs = new QLabel(nameRow);
    m_refs->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
    nh->addWidget(m_refs);
    connect(m_name, &QLineEdit::editingFinished, this, [this, id] {
      if (const eng::Entity *x = m_engine->entityById(id); x && x->name != m_name->text().trimmed())
        m_engine->renameEntity(id, m_name->text());
    });
    m_grid->addWidget(rowLabel(tr("Name"), this), row, 0);
    m_grid->addWidget(nameRow, row, 1, 1, 2);
    ++row;

    // A brush entity's origin follows its brushes: no transform rows.
    if (!(cls && cls->isBrushEntity())) {
      m_origin = new AxisRow(tr("Position"), tr("u"), 1, 16.0, this);
      m_origin->setToolTip(tr("The entity's origin, in Source units"));
      connect(m_origin, &AxisRow::committed, this, [this, id](int axis, double val) {
        if (const eng::Entity *x = m_engine->entityById(id)) {
          glm::vec3 o = x->origin;
          o[axis] = float(val / kSrc);
          m_engine->setEntityTransform(id, o, x->angles, x->name);
        }
      });
      m_grid->addWidget(m_origin, row++, 0, 1, 3);
    }
    const bool oriented = cls && cls->kind != eng::EntityClass::Kind::PointLight && cls->oriented;
    if (oriented) {
      m_angles = new AxisRow(tr("Rotation"), QStringLiteral("°"), 1, 15.0, this);
      m_angles->setSliderRange(-180.0, 180.0);
      m_angles->setReset(tr("Reset the rotation to 0"));
      m_angles->field(0)->setToolTip(tr("Pitch"));
      m_angles->field(1)->setToolTip(tr("Yaw"));
      m_angles->field(2)->setToolTip(tr("Roll"));
      connect(m_angles, &AxisRow::committed, this, [this, id](int axis, double val) {
        if (const eng::Entity *x = m_engine->entityById(id)) {
          glm::vec3 a = x->angles;
          a[axis] = float(wrapDegrees(val));
          m_engine->setEntityTransform(id, x->origin, a, x->name);
        }
      });
      connect(m_angles, &AxisRow::resetRequested, this, [this, id] {
        if (const eng::Entity *x = m_engine->entityById(id); x && x->angles != glm::vec3(0.0f))
          m_engine->setEntityTransform(id, x->origin, glm::vec3(0.0f), x->name);
      });
      m_grid->addWidget(m_angles, row++, 0, 1, 3);
    }
  }

  if (!cls)
    return;
  for (const eng::PropDef &pd: cls->props) {
    const QString key = pd.key;
    QWidget *w = nullptr;
    switch (pd.type) {
      case eng::PropType::Bool: {
        auto *cb = new QCheckBox(this);
        connect(cb, &QCheckBox::toggled, this, [this, key](bool on) { setProp(key, on); });
        m_refreshers.insert(key, [this, cb, key] {
          if (const eng::Entity *x = m_engine->entityById(m_id)) {
            QSignalBlocker b(cb);
            cb->setChecked(x->value(key).toBool());
          }
        });
        w = cb;
        break;
      }
      case eng::PropType::Color: {
        auto *btn = new ColorButton(this);
        connect(btn, &ColorButton::colorPicked, this,
                [this, key](const QColor &c) { setProp(key, c.name()); });
        m_refreshers.insert(key, [this, btn, key] {
          if (const eng::Entity *x = m_engine->entityById(m_id))
            btn->setColor(x->color(key));
        });
        w = btn;
        break;
      }
      case eng::PropType::Choice: {
        auto *combo = new QComboBox(this);
        combo->addItems(pd.choices);
        connect(combo, &QComboBox::currentTextChanged, this,
                [this, key](const QString &t) { setProp(key, t); });
        m_refreshers.insert(key, [this, combo, key] {
          if (const eng::Entity *x = m_engine->entityById(m_id)) {
            QSignalBlocker b(combo);
            combo->setCurrentText(x->value(key).toString());
          }
        });
        w = combo;
        break;
      }
      case eng::PropType::Target: {
        auto *tf = new TargetField(m_engine, this);
        connect(tf, &TargetField::committed, this,
                [this, key](const QString &t) { setProp(key, t); });
        m_refreshers.insert(key, [this, tf, key] {
          if (const eng::Entity *x = m_engine->entityById(m_id)) {
            tf->refresh(m_id);
            tf->setText(x->value(key).toString());
          }
        });
        w = tf;
        break;
      }
      case eng::PropType::File:
      case eng::PropType::String: {
        auto *box = new QWidget(this);
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *line = new QLineEdit(box);
        h->addWidget(line, 1);
        connect(line, &QLineEdit::editingFinished, this,
                [this, line, key] { setProp(key, line->text()); });
        if (pd.type == eng::PropType::File) {
          auto *browse = new QToolButton(box);
          browse->setText(QStringLiteral("…"));
          h->addWidget(browse);
          connect(browse, &QToolButton::clicked, this, [this, line, key, label = pd.label] {
            const QString f = FileDialog::getOpenFileName(this, label, line->text());
            if (!f.isEmpty()) {
              line->setText(f);
              setProp(key, f);
            }
          });
        }
        m_refreshers.insert(key, [this, line, key] {
          if (const eng::Entity *x = m_engine->entityById(m_id); x && !line->hasFocus())
            line->setText(x->value(key).toString());
        });
        w = box;
        break;
      }
      default: {
        const bool angle = pd.type == eng::PropType::Angle;
        if (pd.maxValue > pd.minValue || angle) {
          const double lo = pd.maxValue > pd.minValue ? pd.minValue : -180.0;
          const double hi = pd.maxValue > pd.minValue ? pd.maxValue : 180.0;
          const double curve = lo >= 0.0 && hi - lo > 100.0 ? 3.0 : 1.0;
          auto *sf = new SliderField(lo, hi, pd.type == eng::PropType::Int ? 0 : (angle ? 1 : 2),
                                     !pd.suffix.isEmpty() ? pd.suffix : angle ? QStringLiteral("°") : QString(), curve,
                                     this);
          if (angle)
            sf->setStep(1.0);
          connect(sf, &SliderField::committed, this,
                  [this, key](double val) { setProp(key, val); });
          m_refreshers.insert(key, [this, sf, key] {
            if (const eng::Entity *x = m_engine->entityById(m_id))
              sf->setValueQuiet(x->num(key));
          });
          w = sf;
          break;
        }
        auto *nf = new NumberField(this);
        if (pd.maxValue > pd.minValue)
          nf->setRange(pd.minValue, pd.maxValue);
        nf->setDecimals(pd.type == eng::PropType::Int ? 0 : 2);
        nf->setSingleStep(pd.maxValue - pd.minValue > 100 ? 8.0 : 0.1);
        connect(nf, &NumberField::committed, this, [this, key](double val) { setProp(key, val); });
        m_refreshers.insert(key, [this, nf, key] {
          if (const eng::Entity *x = m_engine->entityById(m_id); x && !nf->hasFocus())
            nf->setValueQuiet(x->num(key));
        });
        w = nf;
        break;
      }
    }
    w->setToolTip(pd.tooltip);
    Row r;
    r.label = rowLabel(pd.label, this);
    r.label->setToolTip(pd.tooltip);
    r.editor = w;
    r.dot = new ResetDot(this);
    connect(r.dot, &QToolButton::clicked, this, [this, key] { resetProp(key); });
    m_grid->addWidget(r.label, row, 0);
    m_grid->addWidget(r.editor, row, 1);
    m_grid->addWidget(r.dot, row, 2);
    ++row;
    m_rows.insert(key, r);
    // Numbered props (case01..case16) show the used ones plus one empty row; see refresh().
    static const QRegularExpression seq(QStringLiteral("^(.*\\D)(\\d\\d)$"));
    if (const auto m = seq.match(key); m.hasMatch() && pd.type == eng::PropType::String)
      m_sequences[m.captured(1)].push_back(key);
  }
  // A rigid body's collision from the last physics build.
  if (!multi && e->classname == eng::kRigidBodyClass) {
    m_collision = new QLabel(this);
    m_collision->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
    m_grid->addWidget(rowLabel(tr("Collision"), this), row, 0);
    m_grid->addWidget(m_collision, row, 1, 1, 2);
    ++row;
  }
}

void EntityEditor::refresh() {
  const eng::Entity *e = m_engine->entityById(m_id);
  if (!e)
    return;
  if (m_name && !m_name->hasFocus())
    m_name->setText(e->name);
  if (m_collision) {
    QString text = m_engine->physicsBusy() ? tr("building…") : tr("none");
    if (const eng::phys::World *w = m_engine->physicsWorld(); w && !m_engine->physicsBusy())
      for (const auto &b: w->bodies())
        if (b.entity == e->id)
          text = tr("%n hull(s)", nullptr, int(b.hulls.size())) + QStringLiteral(" · ") +
                 tr("%n brush(es)", nullptr, b.brushes);
    m_collision->setText(text);
  }
  if (m_refs) {
    const auto *ents = m_engine->sceneEntities();
    const int refs = ents ? int(eng::io::incoming(*ents, e->id).size()) : 0;
    m_refs->setText(refs ? tr("← %1").arg(refs) : QString());
    m_refs->setToolTip(refs ? tr("%1 connection(s) target this entity").arg(refs) : QString());
  }
  if (m_origin) {
    const double o[3] = {e->origin.x * kSrc, e->origin.y * kSrc, e->origin.z * kSrc};
    m_origin->setValuesQuiet(o);
  }
  if (m_angles) {
    const double a[3] = {e->angles.x, e->angles.y, e->angles.z};
    m_angles->setValuesQuiet(a);
  }
  for (const auto &fn: m_refreshers)
    fn();
  if (const eng::EntityClass *cls = e->cls())
    for (const eng::PropDef &pd: cls->props)
      if (const auto it = m_rows.constFind(pd.key); it != m_rows.constEnd() && it->dot) {
        bool changed = false;
        for (const quint32 id: m_ids)
          if (const eng::Entity *x = m_engine->entityById(id))
            changed = changed || !atDefault(pd, *x);
        it->dot->setChanged(changed, pd.label);
      }
  for (auto it = m_sequences.cbegin(); it != m_sequences.cend(); ++it) {
    int last = -1;
    // (sequences follow the first entity)
    for (int i = 0; i < it.value().size(); ++i)
      if (!e->value(it.value()[i]).toString().isEmpty())
        last = i;
    for (int i = 0; i < it.value().size(); ++i)
      if (const auto r = m_rows.constFind(it.value()[i]); r != m_rows.constEnd())
        r->setVisible(i <= std::max(1, last + 1));
  }
}

} // namespace ui
