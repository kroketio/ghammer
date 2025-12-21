#include "modifier_panel.h"

#include <QAction>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/modifiers.h"
#include "client/engine/scene.h"
#include "client/ui/action_manager.h"
#include "client/ui/modifier_editors.h"
#include "client/ui/scale.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

QToolButton *iconButton(const QString &icon, const QString &tip, QWidget *parent, bool checkable = false) {
  auto *b = new QToolButton(parent);
  b->setIcon(ActionManager::icon(icon));
  b->setToolTip(tip);
  b->setAutoRaise(true);
  b->setCheckable(checkable);
  b->setFocusPolicy(Qt::NoFocus);
  return b;
}

} // namespace

bool ModifierPanel::relevant(vk::Engine3D *engine) {
  if (!engine)
    return false;
  if (engine->panelModHost())
    return true;
  return !engine->materialMode() && !engine->selectedObjects().empty();
}

ModifierPanel::ModifierPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);

  auto *head = new QHBoxLayout;
  head->setSpacing(4);
  m_name = new QLineEdit(this);
  m_name->setPlaceholderText(tr("Modified object"));
  m_name->setToolTip(tr("The modified object's name"));
  head->addWidget(m_name, 1);
  m_editSource = new QPushButton(tr("Edit Source"), this);
  m_editSource->setCheckable(true);
  head->addWidget(m_editSource);
  m_add = new QToolButton(this);
  m_add->setText(tr("Add Modifier"));
  m_add->setIcon(ActionManager::icon(QStringLiteral("add")));
  m_add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_add->setPopupMode(QToolButton::InstantPopup);
  auto *menu = new QMenu(m_add);
  for (const eng::mods::KindInfo &k: eng::mods::kinds())
    menu->addAction(ActionManager::icon(k.icon), k.label, this, [this, id = k.id] { m_engine->addModifier(id); });
  m_add->setMenu(menu);
  head->addWidget(m_add);
  v->addLayout(head);

  m_showResult = new QCheckBox(tr("Show generated"), this);
  m_showResult->setToolTip(tr("Show the generated result next to the sources while editing them"));
  v->addWidget(m_showResult);
  connect(m_showResult, &QCheckBox::toggled, this, [this](bool on) { m_engine->setShowModOutput(on); });


  // Edit Source: the selected brushes' membership.
  m_sourceRow = new QWidget(this);
  auto *sr = new QHBoxLayout(m_sourceRow);
  sr->setContentsMargins(0, 0, 0, 0);
  m_addSel = new QPushButton(tr("Add selected"), m_sourceRow);
  m_addSel->setToolTip(tr("Make the selected brushes sources of the object being edited"));
  m_remSel = new QPushButton(tr("Remove selected"), m_sourceRow);
  m_remSel->setToolTip(tr("Take the selected brushes out of the object being edited"));
  sr->addWidget(m_addSel);
  sr->addWidget(m_remSel);
  v->addWidget(m_sourceRow);
  connect(m_addSel, &QPushButton::clicked, this, [this] { m_engine->addSelectionToModHost(); });
  connect(m_remSel, &QPushButton::clicked, this, [this] { m_engine->removeSelectionFromModHost(); });

  m_cards = new QWidget(this);
  m_cardsLayout = new QVBoxLayout(m_cards);
  m_cardsLayout->setContentsMargins(0, 0, 0, 0);
  m_cardsLayout->setSpacing(6);
  v->addWidget(m_cards);

  m_footer = new QWidget(this);
  auto *fr = new QHBoxLayout(m_footer);
  fr->setContentsMargins(0, 0, 0, 0);
  auto *applyAll = new QPushButton(tr("Apply all"), m_footer);
  applyAll->setToolTip(tr("Turn the result into ordinary brushes (the originals go)"));
  auto *dissolve = new QPushButton(tr("Remove all"), m_footer);
  dissolve->setToolTip(tr("Drop the modifiers: the original brushes come back"));
  fr->addWidget(applyAll);
  fr->addWidget(dissolve);
  v->addWidget(m_footer);
  connect(applyAll, &QPushButton::clicked, this, [this] {
    if (m_host)
      m_engine->applyModifiers(m_host, false);
  });
  connect(dissolve, &QPushButton::clicked, this, [this] {
    if (m_host)
      m_engine->dissolveModHost(m_host);
  });

  connect(m_name, &QLineEdit::editingFinished, this, [this] {
    if (m_host)
      m_engine->renameModHost(m_host, m_name->text());
  });
  connect(m_editSource, &QPushButton::clicked, this, [this] {
    // Exit edit leaves whichever host is being edited; Edit Source enters this one.
    m_engine->setEditSourceHost(m_engine->editSourceHost() ? 0 : m_host);
  });

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(30);
  connect(m_timer, &QTimer::timeout, this, &ModifierPanel::refresh);
  for (auto sig: {&vk::Engine3D::modHostsChanged, &vk::Engine3D::modOutputsChanged, &vk::Engine3D::selectionChanged,
                  &vk::Engine3D::editSourceChanged, &vk::Engine3D::pathsChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void ModifierPanel::rebuildCards() {
  for (Card &c: m_cardList)
    delete c.box;
  m_cardList.clear();
  const eng::mods::Host *h = m_engine->modHost(m_host);
  if (!h)
    return;
  const int n = int(h->stack.size());
  for (int i = 0; i < n; ++i) {
    const eng::mods::Modifier &m = h->stack[size_t(i)];
    Card c;
    c.id = m.id;
    c.kind = m.kind;
    auto *box = new QFrame(m_cards);
    box->setObjectName(QStringLiteral("modCard"));
    scale::setScaledStyleSheet(box, QStringLiteral("QFrame#modCard { background: #1b1f2a; border-radius: 6px; }"));
    auto *bv = new QVBoxLayout(box);
    bv->setContentsMargins(8, 4, 8, 6);
    bv->setSpacing(4);
    auto *hh = new QHBoxLayout;
    hh->setSpacing(2);
    c.expand = iconButton(m.expanded ? QStringLiteral("disclosure_tri_down") : QStringLiteral("disclosure_tri_right"),
                          tr("Show / hide the settings"), box);
    hh->addWidget(c.expand);
    const eng::mods::KindInfo *k = eng::mods::kindInfo(m.kind);
    auto *icon = new QLabel(box);
    icon->setPixmap(ActionManager::icon(k ? k->icon : QStringLiteral("modifier"))
                        .pixmap(scale::icon(scale::Icon::Small), devicePixelRatioF()));
    hh->addWidget(icon);
    c.title = new QLabel(m.name, box);
    hh->addWidget(c.title, 1);
    c.editSource = iconButton(QStringLiteral("editmode_hlt"), tr("Also while editing the source"), box, true);
    c.enabled = iconButton(QStringLiteral("restrict_view_off"), tr("Enabled (off shows what is below it)"), box, true);
    auto *up = iconButton(QStringLiteral("tria_up"), tr("Move up"), box);
    auto *down = iconButton(QStringLiteral("tria_down"), tr("Move down"), box);
    auto *more = iconButton(QStringLiteral("downarrow_hlt"), tr("More"), box);
    more->setPopupMode(QToolButton::InstantPopup);
    auto *remove = iconButton(QStringLiteral("x"), tr("Remove"), box);
    up->setEnabled(i > 0);
    down->setEnabled(i + 1 < n);
    for (QToolButton *b: {c.editSource, c.enabled, up, down, more, remove})
      hh->addWidget(b);
    bv->addLayout(hh);
    const quint32 host = m_host, mod = m.id;
    auto *menu = new QMenu(more);
    QAction *applyFirst = menu->addAction(tr("Apply (first modifier only)"), this,
                                          [this, host] { m_engine->applyModifiers(host, true); });
    applyFirst->setEnabled(i == 0);
    menu->addAction(tr("Apply all"), this, [this, host] { m_engine->applyModifiers(host, false); });
    more->setMenu(menu);
    c.editor = makeModifierEditor(m.kind, m_engine, box);
    if (c.editor) {
      bv->addWidget(c.editor);
      c.editor->setVisible(m.expanded);
      connect(c.editor, &ModifierEditor::changed, this, [this, host, mod](const QJsonObject &p, const QString &key) {
        m_engine->setModifierParams(host, mod, p, key);
      });
    } else {
      auto *unknown = new QLabel(tr("Unknown modifier: kept, but skipped"), box);
      unknown->setStyleSheet(QStringLiteral("color:#e0a060;"));
      bv->addWidget(unknown);
    }
    c.status = new QLabel(box);
    c.status->setWordWrap(true);
    bv->addWidget(c.status);
    connect(c.expand, &QToolButton::clicked, this, [this, host, mod] {
      for (Card &cc: m_cardList)
        if (cc.id == mod && cc.editor) {
          const bool open = !cc.editor->isVisible();
          cc.editor->setVisible(open);
          cc.expand->setIcon(ActionManager::icon(open ? QStringLiteral("disclosure_tri_down")
                                                      : QStringLiteral("disclosure_tri_right")));
          m_engine->setModifierExpanded(host, mod, open);
        }
    });
    connect(c.enabled, &QToolButton::clicked, this,
            [this, host, mod](bool on) { m_engine->setModifierEnabled(host, mod, on); });
    connect(c.editSource, &QToolButton::clicked, this,
            [this, host, mod](bool on) { m_engine->setModifierShowInEditSource(host, mod, on); });
    connect(up, &QToolButton::clicked, this, [this, host, mod] { m_engine->moveModifier(host, mod, -1); });
    connect(down, &QToolButton::clicked, this, [this, host, mod] { m_engine->moveModifier(host, mod, 1); });
    connect(remove, &QToolButton::clicked, this, [this, host, mod] { m_engine->removeModifier(host, mod); });
    c.box = box;
    m_cardsLayout->addWidget(box);
    m_cardList.push_back(c);
  }
}

void ModifierPanel::refresh() {
  if (!m_engine)
    return;
  const quint32 host = m_engine->panelModHost();
  const eng::mods::Host *h = host ? m_engine->modHost(host) : nullptr;
  m_host = h ? host : 0;
  {
    QSignalBlocker b(m_name);
    if (!m_name->hasFocus())
      m_name->setText(h ? h->name : QString());
  }
  const bool matMode = m_engine->materialMode();
  const quint32 editing = m_engine->editSourceHost();
  const bool editingThis = h && editing == host;
  m_name->setVisible(h);
  m_editSource->setVisible(h);
  m_editSource->setText(editingThis ? tr("Exit edit") : tr("Edit Source"));
  {
    const QAction *a = act(QStringLiteral("mod.editSource"));
    const QString key = a && !a->shortcut().isEmpty() ? a->shortcut().toString(QKeySequence::NativeText) : QString();
    const QString tip = editingThis ? tr("Back to the generated result") : tr("Show and edit the original brushes; the result keeps following");
    m_editSource->setToolTip(key.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, key));
  }
  {
    QSignalBlocker b(m_editSource);
    m_editSource->setChecked(editingThis);
  }
  m_showResult->setVisible(editingThis);
  {
    QSignalBlocker b(m_showResult);
    m_showResult->setChecked(m_engine->showModOutput());
  }
  m_add->setVisible(!matMode);
  m_footer->setVisible(!matMode && h && !h->stack.empty());
  m_cards->setVisible(!matMode);
  bool outside = false, inside = false;
  if (editing && !matMode && m_engine->scene())
    for (const quint32 id: m_engine->selectedObjects())
      if (const auto o = m_engine->scene()->findById(id))
        (o->modHost() == editing ? inside : outside) = true;
  const eng::mods::Host *eh = editing ? m_engine->modHost(editing) : nullptr;
  m_addSel->setText(eh ? tr("Add to %1").arg(eh->name) : tr("Add selected"));
  m_addSel->setVisible(outside);
  m_remSel->setVisible(inside);
  m_sourceRow->setVisible(outside || inside);

  // Cards: rebuilt when the stack's shape changes, else updated in place.
  std::vector<std::pair<quint32, QString>> shape;
  if (h)
    for (const auto &m: h->stack)
      shape.emplace_back(m.id, m.kind);
  if (shape != m_shape || m_host != m_cardsHost) {
    m_shape = shape;
    m_cardsHost = m_host;
    rebuildCards();
  }
  if (!h)
    return;
  const vk::Engine3D::ModStatus st = m_engine->modStatus(host);
  for (Card &c: m_cardList) {
    const eng::mods::Modifier *m = nullptr;
    for (const auto &x: h->stack)
      if (x.id == c.id)
        m = &x;
    if (!m)
      continue;
    c.title->setText(m->name);
    {
      QSignalBlocker b1(c.enabled), b2(c.editSource);
      c.enabled->setChecked(m->enabled);
      c.enabled->setIcon(ActionManager::icon(m->enabled ? QStringLiteral("restrict_view_off")
                                                        : QStringLiteral("restrict_view_on")));
      c.editSource->setChecked(m->showInEditSource);
    }
    if (c.editor)
      c.editor->load(m->params);
    QStringList lines;
    QString color = QStringLiteral("#8a9ab8");
    const auto sum = st.summary.find(m->id);
    if (sum != st.summary.end() && !sum->second.isEmpty())
      lines << sum->second;
    for (const auto &d: st.diagnostics) {
      if (d.modifierId != m->id)
        continue;
      lines << d.text;
      if (d.severity == eng::mods::Diagnostic::Severity::Error)
        color = QStringLiteral("#ff6b6b");
      else if (d.severity == eng::mods::Diagnostic::Severity::Warning && color != QStringLiteral("#ff6b6b"))
        color = QStringLiteral("#e0a060");
    }
    c.status->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    c.status->setText(lines.join(QLatin1Char('\n')));
    c.status->setVisible(!lines.isEmpty());
  }
}

} // namespace ui
