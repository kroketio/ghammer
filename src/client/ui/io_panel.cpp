#include "io_panel.h"
#include "client/ui/scale.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QPointer>
#include <QSet>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/engine/undo_stack.h"
#include "client/ui/action_manager.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

using eng::io::Connection;
using eng::io::ConnectionList;
using namespace iocommon;

namespace {

QToolButton *toolButton(const QString &icon, const QString &tip, QWidget *parent) {
  auto *b = new QToolButton(parent);
  b->setIcon(ActionManager::icon(icon));
  b->setToolTip(tip);
  b->setAutoRaise(true);
  return b;
}

QString typed(const eng::io::IODef &d) {
  return d.param == eng::io::ParamType::Void ? d.name
                                             : QStringLiteral("%1  (%2)").arg(d.name, eng::io::paramTypeName(d.param));
}

QString bold(const QString &s, const QColor &c) {
  return QStringLiteral("<b style='color:%1'>%2</b>").arg(c.name(), s.toHtmlEscaped());
}

} // namespace

TargetField::TargetField(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(2);
  m_combo = new QComboBox(this);
  m_combo->setEditable(true);
  m_combo->setInsertPolicy(QComboBox::NoInsert);
  m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_combo->setMinimumContentsLength(8);
  m_combo->lineEdit()->setPlaceholderText(tr("targetname, door_*, !self, @group"));
  h->addWidget(m_combo, 1);
  setFocusProxy(m_combo);
  m_pick = toolButton(QStringLiteral("eyedropper"), tr("Pick the target in a view (Esc cancels)"), this);
  m_pick->setCheckable(true);
  h->addWidget(m_pick);

  connect(m_combo->lineEdit(), &QLineEdit::editingFinished, this, &TargetField::commit);
  connect(m_combo, &QComboBox::activated, this, [this](int) { commit(); });
  connect(m_combo->lineEdit(), &QLineEdit::textChanged, this, &TargetField::recolor);
  connect(m_pick, &QToolButton::toggled, this, [this](bool on) {
    if (!m_engine)
      return;
    if (!on) {
      m_engine->cancelTargetPick();
      return;
    }
    // The field can be deleted while the pick is pending.
    m_engine->beginTargetPick([self = QPointer<TargetField>(this)](quint32 id) {
      if (!self)
        return;
      TargetField *const t = self;
      t->picked(id);
    });
  });
  if (m_engine)
    connect(m_engine, &vk::Engine3D::targetPickChanged, this, [this](bool active) {
      if (!active) {
        QSignalBlocker b(m_pick);
        m_pick->setChecked(false);
      }
    });
}

void TargetField::picked(const quint32 id) {
  const eng::Entity *e = m_engine->entityById(id);
  if (!e)
    return;
  if (const eng::EntityClass *cls = e->cls(); cls && !cls->io) {
    qWarning().noquote() << tr("[io] %1 has no I/O and cannot be targeted").arg(cls->label);
    return;
  }
  // An unnamed entity is named first, in the same undo step as the edit.
  eng::UndoStack *undo = m_engine->undoStack();
  const bool macro = undo && !undo->isApplying();
  if (macro)
    undo->beginMacro(tr("Pick target"));
  QString name = e->name;
  if (name.isEmpty()) {
    name = m_engine->uniqueEntityName(e->classname);
    m_engine->renameEntity(id, name);
  }
  m_combo->setEditText(name);
  commit();
  if (macro)
    undo->endMacro();
}

void TargetField::setText(const QString &text) {
  if (m_combo->lineEdit()->hasFocus())
    return;
  m_last = text;
  m_combo->setEditText(text);
  recolor();
}

QString TargetField::text() const { return m_combo->currentText().trimmed(); }

void TargetField::commit() {
  const QString t = text();
  if (t == m_last)
    return;
  m_last = t;
  emit committed(t);
}

void TargetField::refresh(const quint32 self) {
  m_self = self;
  const auto &ents = entitiesOf(m_engine);
  QMap<QString, int> names; // case-folded key -> count; keeps the first spelling
  QMap<QString, QString> spelling;
  QSet<QString> classes;
  for (const auto &e: ents) {
    const eng::EntityClass *c = e.cls();
    if (!c || !c->io)
      continue;
    if (!e.name.isEmpty()) {
      const QString k = e.name.toLower();
      names[k] += 1;
      if (!spelling.contains(k))
        spelling.insert(k, e.name);
    }
    classes.insert(e.classname);
  }
  const QString keep = m_combo->currentText();
  QSignalBlocker b(m_combo);
  m_combo->clear();
  for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
    m_combo->addItem(spelling.value(it.key()));
    m_combo->setItemData(m_combo->count() - 1,
                         it.value() > 1 ? tr("%1 entities share this name").arg(it.value()) : tr("1 entity"),
                         Qt::ToolTipRole);
  }
  m_combo->insertSeparator(m_combo->count());
  for (const char *s: {"!self", "!activator", "!caller", "!player"})
    m_combo->addItem(QString::fromLatin1(s));
  m_combo->setItemData(m_combo->count() - 4, tr("This entity"), Qt::ToolTipRole);
  m_combo->setItemData(m_combo->count() - 3, tr("Whatever started the chain (runtime)"), Qt::ToolTipRole);
  m_combo->setItemData(m_combo->count() - 2, tr("The entity that fired this output (runtime)"), Qt::ToolTipRole);
  m_combo->setItemData(m_combo->count() - 1, tr("The player (Godot group \"player\")"), Qt::ToolTipRole);
  if (!classes.isEmpty()) {
    m_combo->insertSeparator(m_combo->count());
    QStringList cl(classes.begin(), classes.end());
    cl.sort();
    for (const auto &c: cl) {
      m_combo->addItem(c);
      m_combo->setItemData(m_combo->count() - 1, tr("Every %1").arg(c), Qt::ToolTipRole);
    }
  }
  m_combo->setEditText(keep);
  recolor();
}

void TargetField::recolor() {
  const QString t = text();
  QString style;
  if (!t.isEmpty() && m_engine) {
    const auto r = eng::io::resolve(entitiesOf(m_engine), t, m_self);
    switch (r.kind) {
      case eng::io::TargetKind::Unresolved: style = QStringLiteral("color:%1;").arg(kBroken.name()); break;
      case eng::io::TargetKind::Runtime:
      case eng::io::TargetKind::External: style = QStringLiteral("color:%1;").arg(kRuntime.name()); break;
      case eng::io::TargetKind::Resolved:
        if (r.ids.size() > 1)
          style = QStringLiteral("font-weight:bold;");
        break;
    }
    const QString tip = r.kind == eng::io::TargetKind::Resolved
        ? (r.ids.size() == 1 ? tr("1 entity") : tr("%1 entities").arg(r.ids.size()))
        : r.kind == eng::io::TargetKind::Unresolved ? tr("Matches no entity")
        : r.kind == eng::io::TargetKind::External   ? tr("Godot group / node outside the map")
                                                    : tr("Known only while the game runs");
    m_combo->setToolTip(tip);
  }
  m_combo->lineEdit()->setStyleSheet(style);
}


ConnectionForm::ConnectionForm(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(8);

  m_sentence = new QLabel(this);
  m_sentence->setWordWrap(true);
  m_sentence->setTextFormat(Qt::RichText);
  m_sentence->setStyleSheet(QStringLiteral("QLabel { background:#151a33; border:1px solid #232A4E; border-radius:6px; "
                                           "padding:8px; }"));
  v->addWidget(m_sentence);

  auto *f = new QFormLayout();
  f->setLabelAlignment(Qt::AlignLeft);
  f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  f->setVerticalSpacing(6);
  m_output = new QComboBox(this);
  m_output->setToolTip(tr("The event on this entity that fires the connection"));
  f->addRow(tr("When"), m_output);
  m_target = new TargetField(engine, this);
  f->addRow(tr("Send to"), m_target);
  m_input = new QComboBox(this);
  m_input->setEditable(true);
  m_input->setInsertPolicy(QComboBox::NoInsert);
  m_input->setToolTip(tr("What the target does. The list holds the inputs of the classes the target resolves to."));
  f->addRow(tr("Input"), m_input);
  auto *paramBox = new QWidget(this);
  auto *pv = new QVBoxLayout(paramBox);
  pv->setContentsMargins(0, 0, 0, 0);
  pv->setSpacing(2);
  m_param = new QLineEdit(paramBox);
  m_param->setClearButtonEnabled(true);
  pv->addWidget(m_param);
  m_paramHint = new QLabel(paramBox);
  m_paramHint->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  scale::setRole(m_paramHint, scale::Font::Caption);
  m_paramHint->setWordWrap(true);
  pv->addWidget(m_paramHint);
  f->addRow(tr("Parameter"), paramBox);
  m_delay = new NumberField(this);
  m_delay->setRange(0.0, 3600.0);
  m_delay->setDecimals(2);
  m_delay->setSingleStep(0.1);
  m_delay->setSuffix(QStringLiteral(" s"));
  m_delay->setToolTip(tr("Seconds between the output firing and the input arriving"));
  f->addRow(tr("Delay"), m_delay);
  auto *timesRow = new QWidget(this);
  auto *th = new QHBoxLayout(timesRow);
  th->setContentsMargins(0, 0, 0, 0);
  m_timesMode = new QComboBox(timesRow);
  m_timesMode->addItems({tr("Every time"), tr("Only once"), tr("Limited…")});
  th->addWidget(m_timesMode, 1);
  m_times = new QSpinBox(timesRow);
  m_times->setRange(2, 9999);
  m_times->setSuffix(tr(" times"));
  m_times->setKeyboardTracking(false);
  th->addWidget(m_times);
  f->addRow(tr("Fires"), timesRow);
  m_disabled = new QCheckBox(tr("Disabled (kept in the map, never fires)"), this);
  f->addRow(QString(), m_disabled);
  v->addLayout(f);

  m_issues = new QLabel(this);
  m_issues->setWordWrap(true);
  m_issues->setTextFormat(Qt::RichText);
  v->addWidget(m_issues);

  auto *nav = new QHBoxLayout();
  m_toOwner = new QToolButton(this);
  m_toOwner->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_toOwner->setIcon(ActionManager::icon(QStringLiteral("back")));
  m_toTarget = new QToolButton(this);
  m_toTarget->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_toTarget->setIcon(ActionManager::icon(QStringLiteral("forward")));
  nav->addWidget(m_toOwner);
  nav->addStretch(1);
  nav->addWidget(m_toTarget);
  v->addLayout(nav);
  v->addStretch(1);

  connect(m_output, &QComboBox::activated, this, [this](int) {
    const QString o = m_output->currentData().toString();
    edit(tr("Set output"), [o](Connection &c) { c.output = o; });
  });
  connect(m_target, &TargetField::committed, this,
          [this](const QString &t) { edit(tr("Set target"), [t](Connection &c) { c.target = t; }); });
  auto commitInput = [this] {
    if (m_loading)
      return;
    const int idx = m_input->findText(m_input->currentText());
    const QString in = idx >= 0 && !m_input->itemData(idx).toString().isEmpty() ? m_input->itemData(idx).toString()
                                                                               : m_input->currentText().trimmed();
    edit(tr("Set input"), [in](Connection &c) { c.input = in; });
  };
  connect(m_input, &QComboBox::activated, this, [commitInput](int) { commitInput(); });
  connect(m_input->lineEdit(), &QLineEdit::editingFinished, this, commitInput);
  connect(m_param, &QLineEdit::editingFinished, this, [this] {
    const QString p = m_param->text();
    edit(tr("Set parameter"), [p](Connection &c) { c.param = p; });
  });
  connect(m_delay, &NumberField::committed, this,
          [this](double d) { edit(tr("Set delay"), [d](Connection &c) { c.delay = float(d); }); });
  connect(m_timesMode, &QComboBox::activated, this, [this](int i) {
    const int n = i == 0 ? -1 : i == 1 ? 1 : std::max(2, m_times->value());
    edit(tr("Set times"), [n](Connection &c) { c.times = n; });
  });
  connect(m_times, &QSpinBox::valueChanged, this,
          [this](int n) { edit(tr("Set times"), [n](Connection &c) { c.times = n; }); });
  connect(m_disabled, &QCheckBox::toggled, this, [this](bool on) {
    edit(on ? tr("Disable connection") : tr("Enable connection"), [on](Connection &c) { c.disabled = on; });
  });
  connect(m_toOwner, &QToolButton::clicked, this, [this] {
    if (!m_refs.isEmpty())
      emit showEntity(m_refs.front().entity);
  });
  connect(m_toTarget, &QToolButton::clicked, this, [this] {
    const Connection *c = first();
    if (!c)
      return;
    const auto r = eng::io::resolve(entitiesOf(m_engine), c->target, m_refs.front().entity);
    if (!r.ids.empty())
      emit showEntity(r.ids.front());
  });
}

const Connection *ConnectionForm::first() const {
  for (const auto &r: m_refs)
    if (const eng::Entity *e = m_engine->entityById(r.entity); e && r.row >= 0 && r.row < e->outputs.size())
      return &e->outputs[r.row];
  return nullptr;
}

void ConnectionForm::focusTarget() {
  m_target->setFocus(Qt::OtherFocusReason);
}

void ConnectionForm::setConnections(const QVector<ConnRef> &refs) {
  m_refs = refs;
  refresh();
}

void ConnectionForm::edit(const QString &text, const std::function<void(Connection &)> &fn) {
  if (m_loading || m_refs.isEmpty())
    return;
  // Group the edit by owner; several owners = one macro.
  QMap<quint32, QVector<int>> byOwner;
  for (const auto &r: m_refs)
    byOwner[r.entity].push_back(r.row);
  eng::UndoStack *undo = m_engine->undoStack();
  const bool macro = byOwner.size() > 1 && undo && !undo->isApplying();
  if (macro)
    undo->beginMacro(text);
  for (auto it = byOwner.constBegin(); it != byOwner.constEnd(); ++it) {
    const eng::Entity *e = m_engine->entityById(it.key());
    if (!e)
      continue;
    ConnectionList list = e->outputs;
    for (const int row: it.value())
      if (row >= 0 && row < list.size())
        fn(list[row]);
    m_engine->setEntityOutputs(it.key(), list, text);
  }
  if (macro)
    undo->endMacro();
  refresh();
}

void ConnectionForm::refresh() {
  const Connection *cp = first();
  setEnabled(cp != nullptr);
  if (!cp) {
    m_sentence->setText(tr("<span style='color:#8a9ab8'>No connection selected.</span>"));
    m_issues->clear();
    return;
  }
  m_loading = true;
  const Connection c = *cp;
  const quint32 ownerId = m_refs.front().entity;
  const eng::Entity *owner = m_engine->entityById(ownerId);
  const eng::EntityClass *cls = owner->cls();
  const auto &ents = entitiesOf(m_engine);
  const auto res = eng::io::resolve(ents, c.target, ownerId);

  {
    QString who;
    if (res.kind == eng::io::TargetKind::Resolved)
      who = res.ids.size() == 1 ? bold(c.target, kIncoming)
                                : tr("%1 (%2 entities)").arg(bold(c.target, kIncoming)).arg(res.ids.size());
    else if (res.kind == eng::io::TargetKind::Unresolved)
      who = bold(c.target.isEmpty() ? tr("nobody") : c.target, kBroken) + tr(" <i>(matches nothing)</i>");
    else
      who = bold(c.target, kRuntime);
    QString s = tr("When %1 fires %2, send %3 to %4")
                    .arg(bold(entityLabel(*owner), QColor(220, 228, 245)), bold(c.output, kAccent),
                         bold(c.input.isEmpty() ? QStringLiteral("?") : c.input, kAccent), who);
    if (!c.param.isEmpty())
      s += tr(" with %1").arg(bold(QStringLiteral("\"%1\"").arg(c.param), QColor(220, 228, 245)));
    if (c.delay > 0.0f)
      s += tr(" after %1").arg(bold(delayText(c.delay), QColor(220, 228, 245)));
    if (c.times == 1)
      s += tr(", only once");
    else if (c.times > 1)
      s += tr(", at most %1 times").arg(c.times);
    s += QStringLiteral(".");
    if (c.disabled)
      s = QStringLiteral("<span style='color:#828896'><s>%1</s> (disabled)</span>").arg(s);
    if (m_refs.size() > 1)
      s += tr("<br><span style='color:#8a9ab8'>Editing %1 connections</span>").arg(m_refs.size());
    m_sentence->setText(s);
  }

  {
    QSignalBlocker b(m_output);
    m_output->clear();
    if (cls)
      for (const auto &d: cls->outputs) {
        m_output->addItem(typed(d), d.name);
        m_output->setItemData(m_output->count() - 1, d.description, Qt::ToolTipRole);
      }
    int idx = m_output->findData(c.output, Qt::UserRole, Qt::MatchFixedString);
    if (idx < 0 && !c.output.isEmpty()) {
      m_output->addItem(dot(kBroken), c.output, c.output);
      idx = m_output->count() - 1;
    }
    m_output->setCurrentIndex(idx);
  }
  m_target->refresh(ownerId);
  m_target->setText(c.target);

  const std::vector<eng::io::IODef> defs = eng::io::inputsForTarget(ents, c.target, ownerId);
  const eng::io::IODef *inDef = nullptr;
  for (const auto &d: defs)
    if (d.name.compare(c.input, Qt::CaseInsensitive) == 0)
      inDef = &d;
  if (!m_input->lineEdit()->hasFocus()) {
    QSignalBlocker b(m_input);
    m_input->clear();
    for (const auto &d: defs) {
      m_input->addItem(typed(d), d.name);
      m_input->setItemData(m_input->count() - 1, d.description, Qt::ToolTipRole);
    }
    const int idx = m_input->findData(c.input, Qt::UserRole, Qt::MatchFixedString);
    if (idx >= 0)
      m_input->setCurrentIndex(idx);
    else
      m_input->setEditText(c.input);
  }

  if (!m_param->hasFocus())
    m_param->setText(c.param);
  QString hint;
  if (inDef && inDef->param == eng::io::ParamType::Void) {
    hint = tr("%1 takes no parameter.").arg(inDef->name);
    m_param->setPlaceholderText(tr("none"));
  } else {
    const QString type = inDef ? eng::io::paramTypeName(inDef->param) : tr("any");
    m_param->setPlaceholderText(type);
    hint = inDef ? tr("Expects a %1.").arg(type) : QString();
    if (inDef && inDef->param == eng::io::ParamType::Bool)
      hint += tr(" 1 / 0, true / false.");
    else if (inDef && inDef->param == eng::io::ParamType::Color)
      hint += tr(" #rrggbb or \"r g b\".");
    else if (inDef && inDef->param == eng::io::ParamType::Vec3)
      hint += tr(" \"x y z\".");
  }
  if (inDef && !inDef->description.isEmpty())
    hint = inDef->description + QStringLiteral(". ") + hint;
  m_paramHint->setText(hint.trimmed());
  m_paramHint->setVisible(!hint.trimmed().isEmpty());
  if (!m_delay->hasFocus())
    m_delay->setValueQuiet(double(c.delay));
  {
    QSignalBlocker b1(m_timesMode), b2(m_times);
    m_timesMode->setCurrentIndex(c.times < 0 ? 0 : c.times == 1 ? 1 : 2);
    m_times->setVisible(c.times > 1 || c.times == 0);
    m_times->setValue(std::max(2, c.times));
  }
  {
    QSignalBlocker b(m_disabled);
    m_disabled->setChecked(c.disabled);
  }

  QStringList lines;
  for (const auto &r: m_refs) {
    const eng::Entity *e = m_engine->entityById(r.entity);
    if (!e)
      continue;
    for (const auto &is: eng::io::validateConnection(ents, *e, r.row)) {
      const QColor col = is.severity == eng::io::Issue::Severity::Error ? kBroken
          : is.severity == eng::io::Issue::Severity::Warning           ? kWarn
                                                                         : kRuntime;
      const QString line = QStringLiteral("<span style='color:%1'>● %2</span>").arg(col.name(), is.text.toHtmlEscaped());
      if (!lines.contains(line))
        lines << line;
    }
  }
  m_issues->setText(lines.join(QStringLiteral("<br>")));
  m_issues->setVisible(!lines.isEmpty());

  m_toOwner->setText(tr("%1").arg(entityLabel(*owner)));
  m_toOwner->setToolTip(tr("Show the entity that fires this"));
  m_toTarget->setVisible(res.kind == eng::io::TargetKind::Resolved);
  m_toTarget->setText(c.target);
  m_toTarget->setToolTip(tr("Show the target"));
  m_loading = false;
}

IOSummary::IOSummary(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(3);
  m_counts = new QLabel(this);
  m_counts->setTextFormat(Qt::RichText);
  m_counts->setWordWrap(true);
  v->addWidget(m_counts);
  m_rows = new QVBoxLayout();
  m_rows->setSpacing(1);
  v->addLayout(m_rows);
  auto *bar = new QHBoxLayout();
  bar->setSpacing(4);
  auto *edit = new QToolButton(this);
  edit->setText(tr("Edit I/O…"));
  edit->setIcon(ActionManager::icon(QStringLiteral("decorate_linked")));
  edit->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  edit->setToolTip(tr("Open the I/O Editor on this entity"));
  m_add = new QToolButton(this);
  m_add->setText(tr("Output"));
  m_add->setIcon(ActionManager::icon(QStringLiteral("add")));
  m_add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_add->setPopupMode(QToolButton::InstantPopup);
  m_add->setToolTip(tr("Add an output: pick the event, then fill in the target in the I/O Editor"));
  m_add->setMenu(new QMenu(m_add));
  bar->addWidget(edit);
  bar->addWidget(m_add);
  bar->addStretch(1);
  v->addLayout(bar);
  connect(edit, &QToolButton::clicked, this, [this] { emit openEditor(m_id, -1, {}); });
  connect(m_add->menu(), &QMenu::aboutToShow, this, [this] {
    QMenu *m = m_add->menu();
    m->clear();
    const eng::Entity *e = m_engine->entityById(m_id);
    const eng::EntityClass *cls = e ? e->cls() : nullptr;
    if (!cls)
      return;
    for (const auto &d: cls->outputs) {
      QAction *a = m->addAction(typed(d), this, [this, n = d.name] { emit openEditor(m_id, -1, n); });
      a->setToolTip(d.description);
    }
    m->setToolTipsVisible(true);
  });
}

void IOSummary::setEntity(const quint32 id) {
  m_id = id;
  refresh();
}

void IOSummary::refresh() {
  while (QLayoutItem *it = m_rows->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  const eng::Entity *e = m_engine->entityById(m_id);
  if (!e)
    return;
  const auto &ents = entitiesOf(m_engine);
  const int incoming = int(eng::io::incoming(ents, m_id).size());
  int problems = 0;
  for (int i = 0; i < e->outputs.size(); ++i)
    if (statusColor(ents, *e, i) == kBroken || statusColor(ents, *e, i) == kWarn)
      ++problems;
  QString counts = e->outputs.isEmpty() ? tr("No outputs")
                                        : (e->outputs.size() == 1 ? tr("1 output") : tr("%1 outputs").arg(e->outputs.size()));
  counts += QStringLiteral(" · ") + (incoming == 0 ? tr("nothing targets it")
                                     : incoming == 1 ? tr("targeted by 1") : tr("targeted by %1").arg(incoming));
  if (problems)
    counts += QStringLiteral(" · <span style='color:%1'>%2</span>").arg(kBroken.name(), tr("%n problem(s)", nullptr, problems));
  m_counts->setText(counts);

  // At most eight outputs.
  const int shown = std::min<int>(int(e->outputs.size()), 8);
  for (int i = 0; i < shown; ++i) {
    const Connection &c = e->outputs[i];
    QStringList msgs;
    const QColor st = statusColor(ents, *e, i, &msgs);
    auto *b = new QToolButton(this);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setIcon(dot(st));
    b->setText(shortText(c));
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setStyleSheet(QStringLiteral("QToolButton { text-align:left; padding:2px 4px; %1 }")
                         .arg(c.disabled ? QStringLiteral("color:#828896;") : QString()));
    b->setToolTip(msgs.isEmpty() ? tr("Click to edit") : msgs.join('\n'));
    connect(b, &QToolButton::clicked, this, [this, i] { emit openEditor(m_id, i, {}); });
    m_rows->addWidget(b);
  }
  if (e->outputs.size() > shown) {
    auto *more = new QLabel(tr("… and %1 more").arg(e->outputs.size() - shown), this);
    more->setStyleSheet(QStringLiteral("color:#8a9ab8; padding-left:6px;"));
    m_rows->addWidget(more);
  }
}

} // namespace ui
