#include "io_editor.h"
#include "client/ui/scale.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QMap>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <map>
#include <set>

#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/engine/undo_stack.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/io_graph.h"
#include "client/ui/io_panel.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "shared/lib/config.h"

namespace ui {

using eng::io::Connection;
using eng::io::ConnectionList;
using namespace iocommon;

namespace {

QPointer<IOEditor> s_window;

enum Role { EntityRole = Qt::UserRole + 1, RowRole };

QString typed(const eng::io::IODef &d) {
  return d.param == eng::io::ParamType::Void ? d.name
                                             : QStringLiteral("%1  (%2)").arg(d.name, eng::io::paramTypeName(d.param));
}

QLabel *sectionLabel(const QString &text, QWidget *parent) {
  auto *l = new QLabel(text.toUpper(), parent);
  l->setStyleSheet(QStringLiteral("color:#8f9bbd; padding-top:6px;"));
  scale::setRole(l, scale::Font::Heading);
  return l;
}

QToolButton *flatButton(const QString &icon, const QString &tip, QWidget *parent, const QString &text = {}) {
  auto *b = new QToolButton(parent);
  b->setIcon(ActionManager::icon(icon));
  b->setToolTip(tip);
  b->setAutoRaise(true);
  if (!text.isEmpty()) {
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  }
  return b;
}

// Classes offered in "new entity" menus: logic first, then the rest by category.
std::vector<const eng::EntityClass *> placeableIOClasses() {
  std::vector<const eng::EntityClass *> out;
  for (const auto &c: eng::EntityRegistry::instance().classes())
    if (c.io && c.placeable)
      out.push_back(&c);
  return out;
}

} // namespace

IOEditor *IOEditor::open(vk::Engine3D *engine, QWidget *owner, const quint32 entity, const int row,
                         const QString &addOutput, const bool showTest) {
  if (!engine)
    return nullptr;
  if (!s_window)
    s_window = new IOEditor(engine, owner);
  IOEditor *w = s_window;
  if (w->isMinimized())
    w->showNormal();
  w->show();
  w->raise();
  w->activateWindow();
  w->refreshAll();
  if (entity) {
    if (!addOutput.isEmpty())
      w->addOutput(entity, addOutput);
    else if (row >= 0)
      w->selectConnections({{entity, row}});
    else
      w->selectEntity(entity);
    w->m_graph->centerOnEntity(entity, false);
  } else if (const quint32 sel = engine->ioFocusEntity()) {
    w->selectEntity(sel, true);
  }
  if (showTest) {
    w->m_testDock->show();
    w->m_testDock->raise();
    w->prepareTest(entity ? entity : w->m_entity);
  }
  return w;
}

void IOEditor::closeIfOpen() {
  if (s_window)
    s_window->close();
}

IOEditor::IOEditor(vk::Engine3D *engine, QWidget *owner) : QMainWindow(nullptr, Qt::Window), m_engine(engine) {
  setObjectName(QStringLiteral("IOEditor"));
  setWindowTitle(tr("I/O Editor"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setAttribute(Qt::WA_QuitOnClose, false);
  if (owner)
    connect(owner, &QObject::destroyed, this, &QObject::deleteLater);
  resize(scale::initialSize(this, {1500, 900}));
  setDockNestingEnabled(true);

  m_tabs = new QTabWidget(this);
  m_tabs->setDocumentMode(true);
  m_graph = new IOGraphView(engine, m_tabs);
  m_tabs->addTab(m_graph, ActionManager::icon(QStringLiteral("node_sel")), tr("Flow"));
  m_tabs->addTab(buildTable(), ActionManager::icon(QStringLiteral("spreadsheet")), tr("Connections"));
  setCentralWidget(m_tabs);

  auto dock = [this](const QString &title, const char *name, QWidget *w, Qt::DockWidgetArea area) {
    auto *d = new QDockWidget(title, this);
    d->setObjectName(QString::fromLatin1(name));
    d->setWidget(w);
    d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    addDockWidget(area, d);
    return d;
  };
  dock(tr("Entities"), "ioEditor.entities", buildEntityList(), Qt::LeftDockWidgetArea);
  dock(tr("Inspector"), "ioEditor.inspector", buildInspector(), Qt::RightDockWidgetArea);
  m_testDock = dock(tr("Test"), "ioEditor.test", buildTestPanel(), Qt::BottomDockWidgetArea);
  buildToolbar();

  m_statusCounts = new QLabel(this);
  statusBar()->addWidget(m_statusCounts, 1);

  connect(m_graph, &IOGraphView::entityClicked, this, [this](quint32 id, bool) { selectEntity(id); });
  connect(m_graph, &IOGraphView::entityActivated, this, [this](quint32 id) {
    syncToEditor(id);
    m_engine->focusSelection();
  });
  connect(m_graph, &IOGraphView::connectionsClicked, this, [this](const QVector<ConnRef> &refs, bool additive) {
    QVector<ConnRef> next = additive ? m_conns : QVector<ConnRef>();
    for (const auto &r: refs)
      if (next.contains(r))
        next.removeAll(r);
      else
        next.push_back(r);
    if (next.isEmpty())
      selectEntity(m_entity);
    else
      selectConnections(next);
  });
  connect(m_graph, &IOGraphView::backgroundClicked, this, [this] {
    if (!m_conns.isEmpty())
      selectEntity(m_entity);
  });
  connect(m_graph, &IOGraphView::connectRequested, this, &IOEditor::connectEntities);
  connect(m_graph, &IOGraphView::entityMenuRequested, this, &IOEditor::showEntityMenu);
  connect(m_graph, &IOGraphView::connectionMenuRequested, this, &IOEditor::showConnectionMenu);
  connect(m_graph, &IOGraphView::backgroundMenuRequested, this, [this](const QPointF &scenePos, const QPoint &g) {
    QMenu menu(this);
    QMenu *create = menu.addMenu(ActionManager::icon(QStringLiteral("add")), tr("New Entity Here"));
    fillNewEntityMenu(create, [this, scenePos](const QString &cls) {
      if (const quint32 id = createEntity(cls, m_entity, &scenePos))
        selectEntity(id);
    });
    menu.addSeparator();
    menu.addAction(ActionManager::icon(QStringLiteral("node_sel")), tr("Auto Layout (flow)"), this,
                   [this] { m_graph->autoLayout(false); });
    menu.addAction(ActionManager::icon(QStringLiteral("world")), tr("Layout Like the Map (top view)"), this,
                   [this] { m_graph->autoLayout(true); });
    menu.addAction(ActionManager::icon(QStringLiteral("zoom_all")), tr("Fit"), this, [this] { m_graph->fitAll(); });
    menu.exec(g);
  });
  connect(m_graph, &IOGraphView::deleteRequested, this, [this] { deleteConnections(m_conns); });

  for (auto sig: {&vk::Engine3D::entitiesChanged, &vk::Engine3D::undoRedoChanged})
    connect(m_engine, sig, this, &IOEditor::scheduleRefresh);
  connect(m_engine, &vk::Engine3D::selectionChanged, this, [this] {
    if (m_syncing || !isVisible() || !m_follow->isChecked())
      return;
    const quint32 id = m_engine->ioFocusEntity();
    const eng::Entity *e = id ? m_engine->entityById(id) : nullptr;
    if (e && e->cls() && e->cls()->io && id != m_entity)
      selectEntity(id, true);
  });
  // Another level: drop the last one's focus and log.
  connect(m_engine, &vk::Engine3D::documentChanged, this, [this] {
    m_entity = 0;
    m_conns.clear();
    m_log->clear();
    if (!isVisible())
      return;
    refreshAll();
    m_graph->fitAll();
  });
  connect(m_engine, &vk::Engine3D::ioSimChanged, this, &IOEditor::refreshTestState);
  connect(m_engine, &vk::Engine3D::ioSimLogged, this, &IOEditor::appendLog);
  m_simTimer = new QTimer(this);
  m_simTimer->setInterval(33);
  connect(m_simTimer, &QTimer::timeout, this, [this] {
    m_graph->updateSimulation();
    if (m_engine->ioSimRunning())
      m_simTime->setText(tr("t = %1 s · %2 queued").arg(m_engine->ioSimTime(), 0, 'f', 2).arg(m_engine->ioSimQueue().size()));
  });

  const QByteArray geo = config()->getRaw(QStringLiteral("IOEditor/Geometry")).toByteArray();
  if (!geo.isEmpty())
    restoreGeometry(QByteArray::fromBase64(geo));
  const QByteArray st = config()->getRaw(QStringLiteral("IOEditor/State")).toByteArray();
  if (!st.isEmpty())
    restoreState(QByteArray::fromBase64(st));
  refreshTestState();
  QTimer::singleShot(0, this, [this] {
    refreshAll();
    m_graph->fitAll();
  });
}

void IOEditor::closeEvent(QCloseEvent *e) {
  config()->setRaw(QStringLiteral("IOEditor/Geometry"), QString::fromLatin1(saveGeometry().toBase64()));
  config()->setRaw(QStringLiteral("IOEditor/State"), QString::fromLatin1(saveState().toBase64()));
  m_engine->cancelTargetPick();
  QMainWindow::closeEvent(e);
}

void IOEditor::buildToolbar() {
  auto *bar = addToolBar(tr("I/O"));
  bar->setObjectName(QStringLiteral("ioEditor.toolbar"));
  bar->setMovable(false);
  bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto add = [this, bar](const QString &text, const char *icon, const QKeySequence &key, const QString &tip, auto fn) {
    QAction *a = bar->addAction(ActionManager::icon(QString::fromLatin1(icon)), text);
    a->setToolTip(key.isEmpty() ? tip : QStringLiteral("%1  (%2)").arg(tip, key.toString(QKeySequence::NativeText)));
    if (!key.isEmpty()) {
      a->setShortcut(key);
      a->setShortcutContext(Qt::WindowShortcut);
    }
    connect(a, &QAction::triggered, this, fn);
    return a;
  };
  m_undo = add(tr("Undo"), "loop_back", QKeySequence::Undo, tr("Undo (the editor's history)"), [this] { m_engine->undo(); });
  m_redo = add(tr("Redo"), "loop_forwards", QKeySequence(QStringLiteral("Ctrl+Shift+Z")), tr("Redo"),
               [this] { m_engine->redo(); });
  bar->addSeparator();

  auto *addOut = new QToolButton(bar);
  addOut->setText(tr("Output"));
  addOut->setIcon(ActionManager::icon(QStringLiteral("add")));
  addOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  addOut->setPopupMode(QToolButton::InstantPopup);
  addOut->setToolTip(tr("Add an output to the selected entity"));
  addOut->setMenu(new QMenu(addOut));
  connect(addOut->menu(), &QMenu::aboutToShow, this, [this, addOut] {
    QMenu *m = addOut->menu();
    m->clear();
    const eng::Entity *e = m_engine->entityById(m_entity);
    if (!e || !e->cls()) {
      m->addAction(tr("No entity selected"))->setEnabled(false);
      return;
    }
    for (const auto &d: e->cls()->outputs)
      m->addAction(typed(d), this, [this, n = d.name] { addOutput(m_entity, n); })->setToolTip(d.description);
    m->setToolTipsVisible(true);
  });
  bar->addWidget(addOut);
  add(tr("Duplicate"), "duplicate", QKeySequence(QStringLiteral("Ctrl+D")), tr("Duplicate the selected connections"),
      [this] { duplicateConnections(m_conns); });
  add(tr("Delete"), "trash", QKeySequence(), tr("Delete the selected connections (Del in the graph or table)"),
      [this] { deleteConnections(m_conns); });
  bar->addSeparator();

  auto *newEnt = new QToolButton(bar);
  newEnt->setText(tr("New Entity"));
  newEnt->setIcon(ActionManager::icon(QStringLiteral("decorate_linked")));
  newEnt->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  newEnt->setPopupMode(QToolButton::InstantPopup);
  newEnt->setToolTip(tr("Place a logic entity in the level next to the selected one"));
  newEnt->setMenu(new QMenu(newEnt));
  connect(newEnt->menu(), &QMenu::aboutToShow, this, [this, newEnt] {
    newEnt->menu()->clear();
    fillNewEntityMenu(newEnt->menu(), [this](const QString &cls) {
      if (const quint32 id = createEntity(cls, m_entity, nullptr))
        selectEntity(id);
    });
  });
  bar->addWidget(newEnt);
  bar->addSeparator();

  bar->addWidget(new QLabel(tr(" Show "), bar));
  m_scope = new QComboBox(bar);
  m_scope->addItems({tr("Connected entities"), tr("Selection's chain"), tr("All I/O entities")});
  m_scope->setToolTip(tr("Which entities the Flow graph shows. Selection's chain: everything wired to the selected "
                         "entity, directly or through others."));
  m_scope->setCurrentIndex(std::clamp(config()->getRaw(QStringLiteral("IOEditor/Scope")).toInt(), 0, 2));
  bar->addWidget(m_scope);
  connect(m_scope, &QComboBox::currentIndexChanged, this, [this](int i) {
    config()->setRaw(QStringLiteral("IOEditor/Scope"), i);
    refreshAll();
    m_graph->fitAll();
  });
  add(tr("Auto layout"), "node_sel", QKeySequence(), tr("Arrange the graph left to right along the flow"),
      [this] { m_graph->autoLayout(false); });
  add(tr("Map layout"), "world", QKeySequence(), tr("Arrange the graph like the level seen from above"),
      [this] { m_graph->autoLayout(true); });
  add(tr("Fit"), "zoom_all", QKeySequence(), tr("Fit the graph in the view (F)"), [this] { m_graph->fitAll(); });

  auto *spacer = new QWidget(bar);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  bar->addWidget(spacer);
  m_search = new QLineEdit(bar);
  m_search->setPlaceholderText(tr("Search names, classes, outputs, inputs…"));
  m_search->setClearButtonEnabled(true);
  scale::bind(m_search, [=, this] { m_search->setMinimumWidth(scale::tpx(260)); });
  bar->addWidget(m_search);
  auto *findKey = new QShortcut(QKeySequence::Find, this);
  connect(findKey, &QShortcut::activated, this, [this] {
    m_search->setFocus();
    m_search->selectAll();
  });
  connect(m_search, &QLineEdit::textChanged, this, [this] { scheduleRefresh(); });
  m_follow = new QCheckBox(tr("Follow selection"), bar);
  m_follow->setToolTip(tr("Keep this window and the level editor's selection in step"));
  m_follow->setChecked(true);
  bar->addWidget(m_follow);
}

QWidget *IOEditor::buildEntityList() {
  auto *w = new QWidget(this);
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(4, 4, 4, 4);
  m_listFilter = new QComboBox(w);
  m_listFilter->addItems({tr("All I/O entities"), tr("With connections"), tr("With problems"), tr("Unnamed")});
  v->addWidget(m_listFilter);
  m_list = new QTreeWidget(w);
  m_list->setHeaderLabels({tr("Entity"), tr("→"), tr("←")});
  m_list->headerItem()->setToolTip(1, tr("Outputs"));
  m_list->headerItem()->setToolTip(2, tr("Connections arriving"));
  m_list->header()->setStretchLastSection(false);
  m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_list->setRootIsDecorated(true);
  m_list->setUniformRowHeights(true);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(m_list, 1);
  auto *create = new QToolButton(w);
  create->setText(tr("New logic entity"));
  create->setIcon(ActionManager::icon(QStringLiteral("add")));
  create->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  create->setPopupMode(QToolButton::InstantPopup);
  create->setMenu(new QMenu(create));
  connect(create->menu(), &QMenu::aboutToShow, this, [this, create] {
    create->menu()->clear();
    fillNewEntityMenu(create->menu(), [this](const QString &cls) {
      if (const quint32 id = createEntity(cls, m_entity, nullptr))
        selectEntity(id);
    });
  });
  v->addWidget(create);

  connect(m_listFilter, &QComboBox::currentIndexChanged, this, [this] { refreshEntityList(); });
  connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
    if (const quint32 id = it->data(0, EntityRole).toUInt()) {
      selectEntity(id);
      m_graph->centerOnEntity(id, true);
    }
  });
  connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
    if (const quint32 id = it->data(0, EntityRole).toUInt()) {
      syncToEditor(id);
      m_engine->focusSelection();
    }
  });
  connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &p) {
    if (QTreeWidgetItem *it = m_list->itemAt(p))
      if (const quint32 id = it->data(0, EntityRole).toUInt())
        showEntityMenu(id, m_list->viewport()->mapToGlobal(p));
  });
  return w;
}

QWidget *IOEditor::buildTable() {
  m_table = new QTreeWidget(this);
  m_table->setRootIsDecorated(false);
  m_table->setUniformRowHeights(true);
  m_table->setAlternatingRowColors(true);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setSortingEnabled(true);
  m_table->setHeaderLabels({tr("From"), tr("When"), tr("Send to"), tr("Input"), tr("Parameter"), tr("Delay"), tr("Fires")});
  m_table->header()->setSectionResizeMode(QHeaderView::Interactive);
  m_table->header()->setStretchLastSection(true);
  m_table->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_table, &QTreeWidget::itemSelectionChanged, this, [this] {
    QVector<ConnRef> refs;
    for (QTreeWidgetItem *it: m_table->selectedItems())
      refs.push_back({it->data(0, EntityRole).toUInt(), it->data(0, RowRole).toInt()});
    if (!refs.isEmpty())
      selectConnections(refs, true);
  });
  connect(m_table, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
    m_tabs->setCurrentIndex(0);
    m_graph->centerOnEntity(it->data(0, EntityRole).toUInt(), false);
  });
  connect(m_table, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &p) {
    if (QTreeWidgetItem *it = m_table->itemAt(p))
      showConnectionMenu({it->data(0, EntityRole).toUInt(), it->data(0, RowRole).toInt()},
                         m_table->viewport()->mapToGlobal(p));
  });
  auto *del = new QShortcut(QKeySequence::Delete, m_table, nullptr, nullptr, Qt::WidgetShortcut);
  connect(del, &QShortcut::activated, this, [this] { deleteConnections(m_conns); });
  return m_table;
}

QWidget *IOEditor::buildInspector() {
  m_pages = new QStackedWidget(this);
  scale::bind(m_pages, [=, this] { m_pages->setMinimumWidth(scale::tpx(320)); });

  // 0: nothing selected.
  auto *empty = new QLabel(tr("<p style='color:#8a9ab8'>Nothing selected</p>"), m_pages);
  empty->setWordWrap(true);
  empty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  empty->setMargin(12);
  m_pages->addWidget(empty);

  // 1: entity.
  auto *entScroll = new QScrollArea(m_pages);
  entScroll->setWidgetResizable(true);
  entScroll->setFrameShape(QFrame::NoFrame);
  entScroll->setWidget(buildEntityPage());
  m_pages->addWidget(entScroll);

  // 2: connection(s).
  auto *connPage = new QWidget(m_pages);
  auto *cv = new QVBoxLayout(connPage);
  cv->setContentsMargins(10, 8, 10, 8);
  auto *head = new QHBoxLayout();
  m_connTitle = new QLabel(connPage);
  scale::setRole(m_connTitle, scale::Font::Title);
  head->addWidget(m_connTitle, 1);
  auto *dup = flatButton(QStringLiteral("duplicate"), tr("Duplicate"), connPage);
  auto *up = new QToolButton(connPage);
  up->setArrowType(Qt::UpArrow);
  up->setAutoRaise(true);
  up->setToolTip(tr("Move up in the entity's list (order matters for outputs with the same delay)"));
  auto *down = new QToolButton(connPage);
  down->setArrowType(Qt::DownArrow);
  down->setAutoRaise(true);
  down->setToolTip(tr("Move down"));
  auto *fire = flatButton(QStringLiteral("play"), tr("Simulate: send this input now"), connPage);
  auto *del = flatButton(QStringLiteral("trash"), tr("Delete"), connPage);
  for (QToolButton *b: {dup, up, down, fire, del})
    head->addWidget(b);
  cv->addLayout(head);
  m_form = new ConnectionForm(m_engine, connPage);
  cv->addWidget(m_form, 1);
  m_pages->addWidget(connPage);
  connect(dup, &QToolButton::clicked, this, [this] { duplicateConnections(m_conns); });
  connect(del, &QToolButton::clicked, this, [this] { deleteConnections(m_conns); });
  connect(up, &QToolButton::clicked, this, [this] { moveConnection(-1); });
  connect(down, &QToolButton::clicked, this, [this] { moveConnection(1); });
  connect(fire, &QToolButton::clicked, this, [this] {
    for (const auto &r: m_conns)
      if (const eng::Entity *e = m_engine->entityById(r.entity); e && r.row < e->outputs.size()) {
        const auto &c = e->outputs[r.row];
        m_engine->ioSimFire(c.target, c.input, c.param, 0.0);
      }
    m_testDock->show();
  });
  connect(m_form, &ConnectionForm::showEntity, this, [this](quint32 id) {
    selectEntity(id);
    m_graph->centerOnEntity(id, true);
  });
  return m_pages;
}

QWidget *IOEditor::buildEntityPage() {
  auto *w = new QWidget(this);
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(10, 8, 10, 8);
  v->setSpacing(4);

  auto *head = new QHBoxLayout();
  m_entIcon = new QLabel(w);
  scale::bind(m_entIcon, [this] { m_entIcon->setFixedSize(scale::icon(scale::Icon::Large)); });
  head->addWidget(m_entIcon);
  auto *hv = new QVBoxLayout();
  hv->setSpacing(0);
  m_entName = new QLineEdit(w);
  m_entName->setPlaceholderText(tr("unnamed — type a name to make it targetable"));
  scale::setRole(m_entName, scale::Font::Title);
  m_entName->setToolTip(tr("The targetname. Several entities may share one; they are addressed together. "
                           "Renaming updates the connections that used the old name."));
  hv->addWidget(m_entName);
  m_entClass = new QLabel(w);
  m_entClass->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  hv->addWidget(m_entClass);
  head->addLayout(hv, 1);
  v->addLayout(head);
  m_entDesc = new QLabel(w);
  m_entDesc->setWordWrap(true);
  m_entDesc->setStyleSheet(QStringLiteral("color:#8a9ab8; padding:2px 0 4px 0;"));
  v->addWidget(m_entDesc);

  auto *actions = new QHBoxLayout();
  auto *select = flatButton(QStringLiteral("restrict_select_off"), tr("Select it in the level"), w, tr("Select"));
  auto *frame = flatButton(QStringLiteral("zoom_selected"), tr("Frame it in the 3D view"), w, tr("Frame"));
  auto *test = flatButton(QStringLiteral("play"), tr("Send it an input in the simulator"), w, tr("Test"));
  actions->addWidget(select);
  actions->addWidget(frame);
  actions->addWidget(test);
  actions->addStretch(1);
  v->addLayout(actions);
  connect(select, &QToolButton::clicked, this, [this] { syncToEditor(m_entity); });
  connect(frame, &QToolButton::clicked, this, [this] {
    syncToEditor(m_entity);
    m_engine->focusSelection();
  });
  connect(test, &QToolButton::clicked, this, [this] {
    m_testDock->show();
    m_testDock->raise();
    prepareTest(m_entity);
  });

  auto *outHead = new QHBoxLayout();
  m_outHeader = sectionLabel(tr("Outputs"), w);
  outHead->addWidget(m_outHeader, 1);
  m_addOut = flatButton(QStringLiteral("add"), tr("Add an output"), w, tr("Add"));
  m_addOut->setPopupMode(QToolButton::InstantPopup);
  m_addOut->setMenu(new QMenu(m_addOut));
  outHead->addWidget(m_addOut);
  v->addLayout(outHead);
  connect(m_addOut->menu(), &QMenu::aboutToShow, this, [this] {
    QMenu *m = m_addOut->menu();
    m->clear();
    const eng::Entity *e = m_engine->entityById(m_entity);
    if (!e || !e->cls())
      return;
    for (const auto &d: e->cls()->outputs)
      m->addAction(typed(d), this, [this, n = d.name] { addOutput(m_entity, n); })->setToolTip(d.description);
    m->setToolTipsVisible(true);
  });
  m_outList = new QListWidget(w);
  m_outList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  scale::bind(m_outList, [=, this] { m_outList->setMinimumHeight(scale::tpx(90)); });
  m_outList->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(m_outList);
  connect(m_outList, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
    selectConnections({{m_entity, it->data(RowRole).toInt()}});
  });
  connect(m_outList, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
    QVector<ConnRef> refs;
    for (QListWidgetItem *it: m_outList->selectedItems())
      refs.push_back({m_entity, it->data(RowRole).toInt()});
    if (!refs.isEmpty())
      selectConnections(refs);
  });
  connect(m_outList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &p) {
    if (QListWidgetItem *it = m_outList->itemAt(p))
      showConnectionMenu({m_entity, it->data(RowRole).toInt()}, m_outList->viewport()->mapToGlobal(p));
  });

  m_inHeader = sectionLabel(tr("Arriving"), w);
  v->addWidget(m_inHeader);
  m_inList = new QListWidget(w);
  scale::bind(m_inList, [=, this] { m_inList->setMinimumHeight(scale::tpx(60)); });
  v->addWidget(m_inList);
  connect(m_inList, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
    selectConnections({{it->data(EntityRole).toUInt(), it->data(RowRole).toInt()}});
  });

  v->addWidget(sectionLabel(tr("Class reference"), w));
  m_reference = new QTreeWidget(w);
  m_reference->setHeaderHidden(true);
  m_reference->setColumnCount(2);
  m_reference->setRootIsDecorated(true);
  scale::bind(m_reference, [=, this] { m_reference->setMinimumHeight(scale::tpx(160)); });
  m_reference->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_reference->setToolTip(tr("Double-click an output to add it"));
  v->addWidget(m_reference, 1);
  connect(m_reference, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
    if (it->parent() && it->parent()->data(0, RowRole).toInt() == 1)
      addOutput(m_entity, it->data(0, EntityRole).toString());
  });

  connect(m_entName, &QLineEdit::editingFinished, this, [this] {
    const eng::Entity *e = m_engine->entityById(m_entity);
    if (e && e->name != m_entName->text().trimmed())
      m_engine->renameEntity(m_entity, m_entName->text());
  });
  return w;
}

QWidget *IOEditor::buildTestPanel() {
  auto *w = new QWidget(this);
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(6, 4, 6, 4);
  v->setSpacing(4);

  auto *controls = new QHBoxLayout();
  m_run = flatButton(QStringLiteral("play"), tr("Run the map's logic in the editor (logic_auto fires, timers tick)"), w,
                     tr("Run"));
  m_pause = flatButton(QStringLiteral("pause"), tr("Pause / resume"), w, tr("Pause"));
  m_pause->setCheckable(true);
  m_step = flatButton(QStringLiteral("frame_next"), tr("Deliver the next queued event"), w, tr("Step"));
  m_simTime = new QLabel(w);
  m_simTime->setStyleSheet(QStringLiteral("color:#8a9ab8; padding:0 8px;"));
  m_speed = new QComboBox(w);
  for (const double s: {0.25, 0.5, 1.0, 2.0, 4.0, 10.0})
    m_speed->addItem(QStringLiteral("%1×").arg(s), s);
  m_speed->setToolTip(tr("Simulation speed"));
  {
    const double cur = prefs::get().value(QStringLiteral("IO/SimTimeScale")).toDouble();
    int best = 2;
    for (int i = 0; i < m_speed->count(); ++i)
      if (std::abs(m_speed->itemData(i).toDouble() - cur) < 1e-6)
        best = i;
    m_speed->setCurrentIndex(best);
  }
  auto *clear = flatButton(QStringLiteral("trash"), tr("Clear the log"), w);
  controls->addWidget(m_run);
  controls->addWidget(m_pause);
  controls->addWidget(m_step);
  controls->addWidget(m_simTime);
  controls->addStretch(1);
  controls->addWidget(new QLabel(tr("Speed"), w));
  controls->addWidget(m_speed);
  controls->addWidget(clear);
  v->addLayout(controls);

  auto *fireRow = new QHBoxLayout();
  fireRow->addWidget(new QLabel(tr("Send"), w));
  m_fireInput = new QComboBox(w);
  m_fireInput->setEditable(true);
  m_fireInput->setInsertPolicy(QComboBox::NoInsert);
  scale::bind(m_fireInput, [=, this] { m_fireInput->setMinimumWidth(scale::tpx(150)); });
  fireRow->addWidget(m_fireInput);
  fireRow->addWidget(new QLabel(tr("to"), w));
  m_fireTarget = new TargetField(m_engine, w);
  scale::bind(m_fireTarget, [=, this] { m_fireTarget->setMinimumWidth(scale::tpx(180)); });
  fireRow->addWidget(m_fireTarget, 1);
  m_fireParam = new QLineEdit(w);
  m_fireParam->setPlaceholderText(tr("parameter"));
  scale::bind(m_fireParam, [=, this] { m_fireParam->setMaximumWidth(scale::tpx(140)); });
  fireRow->addWidget(m_fireParam);
  m_fireDelay = new NumberField(w);
  m_fireDelay->setRange(0.0, 3600.0);
  m_fireDelay->setDecimals(2);
  m_fireDelay->setSuffix(QStringLiteral(" s"));
  scale::bind(m_fireDelay, [=, this] { m_fireDelay->setMaximumWidth(scale::tpx(80)); });
  fireRow->addWidget(m_fireDelay);
  auto *fire = new QPushButton(ActionManager::icon(QStringLiteral("play")), tr("Fire"), w);
  fire->setToolTip(tr("ent_fire: send the input (starts the simulation if needed)"));
  fireRow->addWidget(fire);
  v->addLayout(fireRow);

  auto *outRow = new QHBoxLayout();
  outRow->addWidget(new QLabel(tr("Fire the selected entity's"), w));
  m_fireOutput = new QComboBox(w);
  scale::bind(m_fireOutput, [=, this] { m_fireOutput->setMinimumWidth(scale::tpx(150)); });
  m_fireOutput->setToolTip(tr("As if it happened in the game, e.g. a trigger's OnStartTouch"));
  outRow->addWidget(m_fireOutput);
  auto *fireOut = new QPushButton(tr("Fire output"), w);
  outRow->addWidget(fireOut);
  outRow->addSpacing(20);
  outRow->addWidget(new QLabel(tr("Godot event"), w));
  m_raiseName = new QLineEdit(w);
  m_raiseName->setPlaceholderText(tr("IO.raise(\"name\")"));
  scale::bind(m_raiseName, [=, this] { m_raiseName->setMaximumWidth(scale::tpx(180)); });
  outRow->addWidget(m_raiseName);
  auto *raise = new QPushButton(tr("Raise"), w);
  raise->setToolTip(tr("What Godot's IO.raise(name) does: fires OnRaised on logic_event entities with that name"));
  outRow->addWidget(raise);
  outRow->addStretch(1);
  v->addLayout(outRow);

  m_log = new QListWidget(w);
  scale::setRole(m_log, scale::Font::Mono);
  m_log->setUniformItemSizes(true);
  v->addWidget(m_log, 1);

  connect(m_run, &QToolButton::clicked, this, [this] {
    if (m_engine->ioSimRunning())
      m_engine->ioSimStop();
    else
      m_engine->ioSimStart();
  });
  connect(m_pause, &QToolButton::toggled, this, [this](bool on) { m_engine->setIOSimPaused(on); });
  connect(m_step, &QToolButton::clicked, this, [this] { m_engine->ioSimStep(1); });
  connect(m_speed, &QComboBox::currentIndexChanged, this,
          [this](int i) { prefs::get().setValue(QStringLiteral("IO/SimTimeScale"), m_speed->itemData(i).toDouble()); });
  connect(clear, &QToolButton::clicked, m_log, &QListWidget::clear);
  connect(m_fireTarget, &TargetField::committed, this, [this](const QString &t) {
    const auto *ents = m_engine->sceneEntities();
    const QString keep = m_fireInput->currentText();
    m_fireInput->clear();
    if (ents)
      for (const auto &d: eng::io::inputsForTarget(*ents, t, m_entity))
        m_fireInput->addItem(typed(d), d.name);
    m_fireInput->setEditText(keep);
  });
  auto doFire = [this] {
    QString input = m_fireInput->currentText().trimmed();
    if (const int idx = m_fireInput->findText(input); idx >= 0 && !m_fireInput->itemData(idx).toString().isEmpty())
      input = m_fireInput->itemData(idx).toString();
    const QString target = m_fireTarget->text();
    if (input.isEmpty() || target.isEmpty())
      return;
    m_engine->ioSimFire(target, input, m_fireParam->text(), m_fireDelay->value());
  };
  connect(fire, &QPushButton::clicked, this, doFire);
  connect(m_fireParam, &QLineEdit::returnPressed, this, doFire);
  connect(fireOut, &QPushButton::clicked, this, [this] {
    if (m_entity && !m_fireOutput->currentData().toString().isEmpty())
      m_engine->ioSimFireOutput(m_entity, m_fireOutput->currentData().toString());
  });
  auto doRaise = [this] {
    if (!m_raiseName->text().trimmed().isEmpty())
      m_engine->ioSimRaise(m_raiseName->text().trimmed());
  };
  connect(raise, &QPushButton::clicked, this, doRaise);
  connect(m_raiseName, &QLineEdit::returnPressed, this, doRaise);
  connect(m_log, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
    const quint32 to = it->data(RowRole).toUInt(), from = it->data(EntityRole).toUInt();
    if (const quint32 id = to ? to : from) {
      selectEntity(id);
      m_graph->centerOnEntity(id, true);
    }
  });
  return w;
}

void IOEditor::scheduleRefresh() {
  if (m_refreshPending || !isVisible())
    return;
  m_refreshPending = true;
  QTimer::singleShot(40, this, [this] {
    m_refreshPending = false;
    refreshAll();
  });
}

bool IOEditor::matchesSearch(const eng::Entity &e) const {
  const QString q = m_search ? m_search->text().trimmed() : QString();
  if (q.isEmpty())
    return true;
  auto has = [&q](const QString &s) { return s.contains(q, Qt::CaseInsensitive); };
  if (has(e.name) || has(e.classname) || (e.cls() && has(e.cls()->label)))
    return true;
  for (const auto &c: e.outputs)
    if (has(c.output) || has(c.target) || has(c.input) || has(c.param))
      return true;
  return false;
}

QSet<quint32> IOEditor::visibleEntities() const {
  const auto &ents = entitiesOf(m_engine);
  QSet<quint32> out;
  // Adjacency over resolved connections, both ways.
  std::map<quint32, std::set<quint32>> adj;
  QSet<quint32> wired;
  for (const auto &e: ents) {
    if (!e.cls() || !e.cls()->io)
      continue;
    if (!e.outputs.isEmpty())
      wired.insert(e.id);
    for (const auto &c: e.outputs)
      for (const quint32 t: eng::io::resolve(ents, c.target, e.id).ids) {
        adj[e.id].insert(t);
        adj[t].insert(e.id);
        wired.insert(t);
      }
  }
  const int scope = m_scope ? m_scope->currentIndex() : 0;
  if (scope == 1 && m_entity) {
    std::vector<quint32> stack {m_entity};
    out.insert(m_entity);
    while (!stack.empty()) {
      const quint32 v = stack.back();
      stack.pop_back();
      for (const quint32 w: adj[v])
        if (!out.contains(w)) {
          out.insert(w);
          stack.push_back(w);
        }
    }
  } else {
    for (const auto &e: ents)
      if (e.cls() && e.cls()->io && (scope == 2 || wired.contains(e.id)))
        out.insert(e.id);
  }
  if (m_entity)
    out.insert(m_entity);
  // Search: matches and their direct neighbours.
  if (m_search && !m_search->text().trimmed().isEmpty()) {
    QSet<quint32> hit;
    for (const auto &e: ents)
      if (out.contains(e.id) && matchesSearch(e)) {
        hit.insert(e.id);
        for (const quint32 w: adj[e.id])
          if (out.contains(w))
            hit.insert(w);
      }
    out = hit;
  }
  return out;
}

void IOEditor::refreshAll() {
  if (!m_engine)
    return;
  // Drop what no longer exists.
  if (m_entity && !m_engine->entityById(m_entity))
    m_entity = 0;
  QVector<ConnRef> valid;
  for (const auto &r: m_conns)
    if (const eng::Entity *e = m_engine->entityById(r.entity); e && r.row >= 0 && r.row < e->outputs.size())
      valid.push_back(r);
  m_conns = valid;

  m_graph->rebuild(visibleEntities());
  m_graph->setHighlight(m_entity, m_conns);
  m_graph->updateSimulation();
  refreshEntityList();
  refreshTable();
  if (!m_conns.isEmpty()) {
    m_form->setConnections(m_conns);
    m_pages->setCurrentIndex(2);
    m_connTitle->setText(m_conns.size() == 1 ? tr("Connection") : tr("%1 connections").arg(m_conns.size()));
  } else if (m_entity) {
    m_pages->setCurrentIndex(1);
    refreshEntityPage();
  } else {
    m_pages->setCurrentIndex(0);
  }
  m_undo->setEnabled(m_engine->canUndo());
  m_redo->setEnabled(m_engine->canRedo());
  refreshStatus();
}

void IOEditor::refreshStatus() {
  const auto &ents = entitiesOf(m_engine);
  int entities = 0, conns = 0, problems = 0;
  for (const auto &e: ents)
    if (e.cls() && e.cls()->io) {
      ++entities;
      conns += int(e.outputs.size());
    }
  for (const auto &is: eng::io::validate(ents))
    problems += is.severity != eng::io::Issue::Severity::Info ? 1 : 0;
  m_statusCounts->setText(tr("%1 I/O entities · %2 connections%3")
                              .arg(entities)
                              .arg(conns)
                              .arg(problems ? tr(" · <span style='color:%1'>%2 problem(s)</span>").arg(kBroken.name()).arg(problems)
                                            : QString()));
}

void IOEditor::refreshEntityList() {
  const auto &ents = entitiesOf(m_engine);
  QSet<QString> collapsed;
  for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    if (!m_list->topLevelItem(i)->isExpanded())
      collapsed.insert(m_list->topLevelItem(i)->text(0));
  QSignalBlocker b(m_list);
  m_list->clear();
  const int filter = m_listFilter->currentIndex();
  QHash<QString, QTreeWidgetItem *> groups;
  // Problems per entity, once.
  QSet<quint32> problems;
  for (const auto &is: eng::io::validate(ents))
    if (is.severity != eng::io::Issue::Severity::Info)
      problems.insert(is.entity);
  for (const auto &e: ents) {
    const eng::EntityClass *c = e.cls();
    if (!c || !c->io || !matchesSearch(e))
      continue;
    const int incoming = int(eng::io::incoming(ents, e.id).size());
    if (filter == 1 && e.outputs.isEmpty() && incoming == 0)
      continue;
    if (filter == 2 && !problems.contains(e.id))
      continue;
    if (filter == 3 && !e.name.isEmpty())
      continue;
    QTreeWidgetItem *g = groups.value(c->category);
    if (!g) {
      g = new QTreeWidgetItem(m_list, {c->category});
      QFont f = g->font(0);
      f.setBold(true);
      g->setFont(0, f);
      g->setForeground(0, kMuted);
      g->setFlags(Qt::ItemIsEnabled);
      groups.insert(c->category, g);
    }
    auto *it = new QTreeWidgetItem(g);
    it->setText(0, e.name.isEmpty() ? QStringLiteral("%1  ·  unnamed").arg(c->label) : e.name);
    it->setToolTip(0, QStringLiteral("%1 (%2)").arg(c->label, e.classname));
    it->setIcon(0, ActionManager::icon(c->icon));
    if (e.name.isEmpty())
      it->setForeground(0, kMuted);
    if (problems.contains(e.id)) {
      it->setForeground(0, kBroken);
      it->setToolTip(0, it->toolTip(0) + tr("\nHas I/O problems"));
    }
    it->setText(1, e.outputs.isEmpty() ? QString() : QString::number(e.outputs.size()));
    it->setText(2, incoming ? QString::number(incoming) : QString());
    it->setForeground(1, kAccent);
    it->setForeground(2, kIncoming);
    it->setData(0, EntityRole, e.id);
    if (e.id == m_entity)
      it->setSelected(true);
  }
  for (QTreeWidgetItem *g: std::as_const(groups)) {
    g->setText(0, QStringLiteral("%1  (%2)").arg(g->text(0)).arg(g->childCount()));
    g->setExpanded(!collapsed.contains(g->text(0)));
  }
  m_list->sortItems(0, Qt::AscendingOrder);
}

void IOEditor::refreshTable() {
  const auto &ents = entitiesOf(m_engine);
  QSignalBlocker b(m_table);
  m_table->setSortingEnabled(false);
  m_table->clear();
  for (const auto &e: ents) {
    if (!e.cls() || !e.cls()->io)
      continue;
    const bool entityMatch = matchesSearch(e);
    for (int i = 0; i < e.outputs.size(); ++i) {
      const auto &c = e.outputs[i];
      if (!entityMatch)
        continue;
      QStringList msgs;
      const QColor st = statusColor(ents, e, i, &msgs);
      auto *it = new QTreeWidgetItem(m_table);
      it->setText(0, entityLabel(e));
      it->setIcon(0, dot(st));
      it->setText(1, c.output);
      it->setText(2, c.target);
      it->setText(3, c.input);
      it->setText(4, c.param);
      it->setText(5, delayText(c.delay));
      it->setText(6, c.times < 0 ? tr("always") : c.times == 1 ? tr("once") : tr("%1×").arg(c.times));
      it->setData(0, EntityRole, e.id);
      it->setData(0, RowRole, i);
      const auto r = eng::io::resolve(ents, c.target, e.id);
      if (r.kind == eng::io::TargetKind::Unresolved)
        it->setForeground(2, kBroken);
      else if (r.kind != eng::io::TargetKind::Resolved)
        it->setForeground(2, kRuntime);
      if (c.disabled)
        for (int col = 0; col < 7; ++col)
          it->setForeground(col, kDisabled);
      const QString tip = msgs.isEmpty() ? shortText(c) : shortText(c) + QStringLiteral("\n") + msgs.join('\n');
      for (int col = 0; col < 7; ++col)
        it->setToolTip(col, tip);
      if (m_conns.contains(ConnRef {e.id, i}))
        it->setSelected(true);
    }
  }
  m_table->setSortingEnabled(true);
}

void IOEditor::refreshEntityPage() {
  const eng::Entity *e = m_engine->entityById(m_entity);
  if (!e)
    return;
  const eng::EntityClass *c = e->cls();
  const auto &ents = entitiesOf(m_engine);
  m_entIcon->setPixmap(ActionManager::icon(c ? c->icon : QStringLiteral("empty_data")).pixmap(scale::icon(scale::Icon::Large), devicePixelRatioF()));
  if (!m_entName->hasFocus())
    m_entName->setText(e->name);
  m_entClass->setText(c ? QStringLiteral("%1 · %2").arg(c->label, e->classname) : e->classname);
  m_entDesc->setText(c ? c->description : QString());

  m_outList->clear();
  for (int i = 0; i < e->outputs.size(); ++i) {
    QStringList msgs;
    auto *it = new QListWidgetItem(dot(statusColor(ents, *e, i, &msgs)), shortText(e->outputs[i]), m_outList);
    it->setData(RowRole, i);
    it->setToolTip(msgs.isEmpty() ? tr("Click to edit") : msgs.join('\n'));
    if (e->outputs[i].disabled)
      it->setForeground(kDisabled);
  }
  if (e->outputs.isEmpty()) {
    auto *it = new QListWidgetItem(tr("No outputs"), m_outList);
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(kMuted);
  }
  m_outHeader->setText(tr("OUTPUTS (%1)").arg(e->outputs.size()));

  m_inList->clear();
  const auto refs = eng::io::incoming(ents, m_entity);
  for (const auto &r: refs) {
    const eng::Entity *src = m_engine->entityById(r.source);
    if (!src || r.connection >= src->outputs.size())
      continue;
    const auto &cn = src->outputs[r.connection];
    QString text = QStringLiteral("%1 . %2  →  %3").arg(entityLabel(*src), cn.output, cn.input);
    if (!cn.param.isEmpty())
      text += QStringLiteral("(%1)").arg(cn.param);
    if (cn.delay > 0)
      text += QStringLiteral(" · %1").arg(delayText(cn.delay));
    auto *it = new QListWidgetItem(dot(statusColor(ents, *src, r.connection)), text, m_inList);
    it->setData(EntityRole, r.source);
    it->setData(RowRole, r.connection);
    if (cn.disabled)
      it->setForeground(kDisabled);
  }
  if (refs.empty()) {
    auto *it = new QListWidgetItem(e->name.isEmpty() ? tr("Nothing can target it until it has a name.")
                                                     : tr("Nothing targets it yet."),
                                   m_inList);
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(kMuted);
  }
  m_inHeader->setText(tr("ARRIVING (%1)").arg(refs.size()));

  m_reference->clear();
  if (c) {
    auto *ins = new QTreeWidgetItem(m_reference, {tr("Inputs it accepts (%1)").arg(c->inputs.size())});
    ins->setData(0, RowRole, 0);
    for (const auto &d: c->inputs) {
      auto *it = new QTreeWidgetItem(ins, {typed(d), d.description});
      it->setToolTip(0, d.description);
      it->setToolTip(1, d.description);
      it->setForeground(1, kMuted);
      it->setForeground(0, QColor(255, 196, 140));
    }
    auto *outs = new QTreeWidgetItem(m_reference, {tr("Outputs it fires (%1)").arg(c->outputs.size())});
    outs->setData(0, RowRole, 1);
    for (const auto &d: c->outputs) {
      auto *it = new QTreeWidgetItem(outs, {typed(d), d.description});
      it->setData(0, EntityRole, d.name);
      it->setToolTip(0, tr("%1\nDouble-click to add").arg(d.description));
      it->setToolTip(1, d.description);
      it->setForeground(1, kMuted);
      it->setForeground(0, QColor(150, 220, 255));
    }
    outs->setExpanded(true);
  }
  m_fireOutput->clear();
  if (c)
    for (const auto &d: c->outputs)
      m_fireOutput->addItem(d.name, d.name);
}

void IOEditor::refreshTestState() {
  const bool running = m_engine->ioSimRunning();
  m_run->setText(running ? tr("Stop") : tr("Run"));
  m_run->setIcon(ActionManager::icon(running ? QStringLiteral("cancel") : QStringLiteral("play")));
  m_pause->setEnabled(running);
  {
    QSignalBlocker b(m_pause);
    m_pause->setChecked(m_engine->ioSimPaused());
  }
  if (running && !m_simTimer->isActive())
    m_simTimer->start();
  if (!running) {
    m_simTimer->stop();
    m_graph->clearSimulation();
    m_simTime->setText(tr("not running"));
  } else {
    m_simTime->setText(tr("t = %1 s · %2 queued").arg(m_engine->ioSimTime(), 0, 'f', 2).arg(m_engine->ioSimQueue().size()));
  }
}

void IOEditor::appendLog(const double time, const QString &text, const quint32 from, const quint32 to, const bool note) {
  auto *it = new QListWidgetItem(QStringLiteral("%1  %2").arg(time, 7, 'f', 2).arg(text), m_log);
  it->setData(EntityRole, from);
  it->setData(RowRole, to);
  if (note)
    it->setForeground(text.contains(QLatin1String("Godot")) ? kRuntime : kMuted);
  while (m_log->count() > 2000)
    delete m_log->takeItem(0);
  m_log->scrollToBottom();
}

void IOEditor::selectEntity(const quint32 id, const bool fromEditor) {
  const bool wasVisible = visibleEntities().contains(id);
  m_entity = id;
  m_conns.clear();
  if (!fromEditor)
    syncToEditor(id);
  if (!wasVisible) {
    refreshAll(); // the shown set depends on the selection
  } else {
    m_graph->setHighlight(m_entity, m_conns);
    m_pages->setCurrentIndex(id ? 1 : 0);
    refreshEntityPage();
    {
      QSignalBlocker b(m_table);
      m_table->clearSelection();
    }
    for (QTreeWidgetItemIterator it(m_list); *it; ++it)
      (*it)->setSelected((*it)->data(0, EntityRole).toUInt() == id && id);
  }
  if (fromEditor)
    m_graph->centerOnEntity(id, true);
}

void IOEditor::selectConnections(const QVector<ConnRef> &refs, const bool fromTable) {
  if (refs.isEmpty())
    return;
  m_conns = refs;
  m_entity = refs.front().entity;
  m_graph->setHighlight(m_entity, m_conns);
  m_form->setConnections(m_conns);
  m_connTitle->setText(refs.size() == 1 ? tr("Connection") : tr("%1 connections").arg(refs.size()));
  m_pages->setCurrentIndex(2);
  if (!fromTable) {
    QSignalBlocker b(m_table);
    for (QTreeWidgetItemIterator it(m_table); *it; ++it)
      (*it)->setSelected(refs.contains(ConnRef {(*it)->data(0, EntityRole).toUInt(), (*it)->data(0, RowRole).toInt()}));
  }
  for (QTreeWidgetItemIterator it(m_list); *it; ++it)
    (*it)->setSelected((*it)->data(0, EntityRole).toUInt() == m_entity);
  if (m_follow->isChecked())
    syncToEditor(m_entity);
}

void IOEditor::syncToEditor(const quint32 id) {
  if (!id || !m_follow->isChecked())
    return;
  m_syncing = true;
  m_engine->selectEntitiesWithBrushes({id});
  m_syncing = false;
}

QString IOEditor::ensureName(const quint32 id) {
  const eng::Entity *e = m_engine->entityById(id);
  if (!e)
    return {};
  if (!e->name.isEmpty())
    return e->name;
  const QString name = m_engine->uniqueEntityName(e->classname);
  m_engine->renameEntity(id, name);
  return name;
}

void IOEditor::addOutput(const quint32 entity, const QString &output, const QString &target, const QString &input) {
  const eng::Entity *e = m_engine->entityById(entity);
  if (!e)
    return;
  ConnectionList list = e->outputs;
  Connection c;
  c.output = output;
  c.target = target;
  c.input = input;
  list.push_back(c);
  m_engine->setEntityOutputs(entity, list, tr("Add %1").arg(output));
  refreshAll();
  selectConnections({{entity, int(list.size()) - 1}});
  if (target.isEmpty())
    m_form->focusTarget();
}

void IOEditor::deleteConnections(const QVector<ConnRef> &refs) {
  if (refs.isEmpty())
    return;
  QMap<quint32, QVector<int>> byOwner;
  for (const auto &r: refs)
    byOwner[r.entity].push_back(r.row);
  eng::UndoStack *undo = m_engine->undoStack();
  const QString text = refs.size() == 1 ? tr("Delete connection") : tr("Delete %1 connections").arg(refs.size());
  const bool macro = byOwner.size() > 1 && undo && !undo->isApplying();
  if (macro)
    undo->beginMacro(text);
  for (auto it = byOwner.begin(); it != byOwner.end(); ++it) {
    const eng::Entity *e = m_engine->entityById(it.key());
    if (!e)
      continue;
    ConnectionList list = e->outputs;
    QVector<int> rows = it.value();
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for (const int r: rows)
      if (r >= 0 && r < list.size())
        list.removeAt(r);
    m_engine->setEntityOutputs(it.key(), list, text);
  }
  if (macro)
    undo->endMacro();
  const quint32 owner = refs.front().entity;
  m_conns.clear();
  m_entity = owner;
  refreshAll();
}

void IOEditor::duplicateConnections(const QVector<ConnRef> &refs) {
  if (refs.isEmpty())
    return;
  QMap<quint32, QVector<int>> byOwner;
  for (const auto &r: refs)
    byOwner[r.entity].push_back(r.row);
  eng::UndoStack *undo = m_engine->undoStack();
  const bool macro = byOwner.size() > 1 && undo && !undo->isApplying();
  if (macro)
    undo->beginMacro(tr("Duplicate connections"));
  QVector<ConnRef> fresh;
  for (auto it = byOwner.begin(); it != byOwner.end(); ++it) {
    const eng::Entity *e = m_engine->entityById(it.key());
    if (!e)
      continue;
    ConnectionList list = e->outputs;
    for (const int r: it.value())
      if (r >= 0 && r < e->outputs.size()) {
        list.push_back(e->outputs[r]);
        fresh.push_back({it.key(), int(list.size()) - 1});
      }
    m_engine->setEntityOutputs(it.key(), list, tr("Duplicate connections"));
  }
  if (macro)
    undo->endMacro();
  refreshAll();
  selectConnections(fresh);
}

void IOEditor::moveConnection(const int dir) {
  if (m_conns.size() != 1)
    return;
  const ConnRef r = m_conns.front();
  const eng::Entity *e = m_engine->entityById(r.entity);
  if (!e || r.row + dir < 0 || r.row + dir >= e->outputs.size())
    return;
  ConnectionList list = e->outputs;
  list.swapItemsAt(r.row, r.row + dir);
  m_engine->setEntityOutputs(r.entity, list, tr("Reorder outputs"));
  refreshAll();
  selectConnections({{r.entity, r.row + dir}});
}

quint32 IOEditor::createEntity(const QString &classname, const quint32 near, const QPointF *graphPos) {
  const quint32 id = m_engine->addEntityNear(classname, near);
  if (id && graphPos)
    m_engine->setIOGraphPos(id, glm::vec2(float(graphPos->x()), float(graphPos->y())));
  if (id)
    ensureName(id); // a logic entity needs a name to be targeted
  return id;
}

void IOEditor::fillNewEntityMenu(QMenu *menu, const std::function<void(const QString &)> &onPick) {
  menu->setToolTipsVisible(true);
  std::map<QString, QMenu *> sub;
  for (const eng::EntityClass *c: placeableIOClasses()) {
    QMenu *target = menu;
    if (c->category != QLatin1String("Logic")) {
      auto &m = sub[c->category];
      if (!m)
        m = new QMenu(c->category, menu);
      target = m;
    }
    QAction *a = target->addAction(ActionManager::icon(c->icon), QStringLiteral("%1   (%2)").arg(c->label, c->name), this,
                                   [onPick, n = c->name] { onPick(n); });
    a->setToolTip(c->description);
  }
  if (!sub.empty())
    menu->addSeparator();
  for (auto &[name, m]: sub) {
    m->setToolTipsVisible(true);
    menu->addMenu(m);
  }
}

void IOEditor::connectEntities(const quint32 from, const QString &output, const quint32 to, const QPointF &scenePos,
                               const QPoint &globalPos) {
  const eng::Entity *src = m_engine->entityById(from);
  const eng::EntityClass *sc = src ? src->cls() : nullptr;
  if (!sc)
    return;
  QMenu menu(this);
  menu.setToolTipsVisible(true);
  if (!to) {
    // Dropped on empty space: create the target first, then wire it.
    menu.addSection(tr("Create and connect"));
    fillNewEntityMenu(&menu, [this, from, output, scenePos, globalPos](const QString &cls) {
      if (const quint32 id = createEntity(cls, from, &scenePos)) {
        refreshAll();
        // Deferred so this menu has closed first.
        QTimer::singleShot(0, this, [this, from, output, id, scenePos, globalPos] {
          connectEntities(from, output, id, scenePos, globalPos);
        });
      }
    });
    menu.exec(globalPos);
    return;
  }
  const eng::Entity *dst = m_engine->entityById(to);
  const eng::EntityClass *dc = dst ? dst->cls() : nullptr;
  if (!dc || !dc->io)
    return;
  auto make = [this, from, to](const QString &out, const QString &in) {
    eng::UndoStack *undo = m_engine->undoStack();
    const bool macro = undo && !undo->isApplying();
    if (macro)
      undo->beginMacro(tr("Connect %1 → %2").arg(out, in));
    const QString target = from == to ? QStringLiteral("!self") : ensureName(to);
    addOutput(from, out, target, in);
    if (macro)
      undo->endMacro();
  };
  auto fillInputs = [&](QMenu *m, const QString &out) {
    for (const auto &d: dc->inputs) {
      QAction *a = m->addAction(typed(d), this, [make, out, in = d.name] { make(out, in); });
      a->setToolTip(d.description);
    }
  };
  const QString dstLabel = dst->name.isEmpty() ? dc->label : dst->name;
  if (!output.isEmpty()) {
    menu.addSection(tr("%1 → %2: which input?").arg(output, dstLabel));
    fillInputs(&menu, output);
  } else {
    menu.addSection(tr("Connect to %1").arg(dstLabel));
    for (const auto &d: sc->outputs) {
      QMenu *m = menu.addMenu(typed(d));
      m->setToolTipsVisible(true);
      fillInputs(m, d.name);
    }
  }
  menu.exec(globalPos);
}

void IOEditor::showEntityMenu(const quint32 id, const QPoint &globalPos) {
  const eng::Entity *e = m_engine->entityById(id);
  const eng::EntityClass *c = e ? e->cls() : nullptr;
  if (!c)
    return;
  selectEntity(id);
  QMenu menu(this);
  menu.setToolTipsVisible(true);
  menu.addSection(entityLabel(*e));
  QMenu *add = menu.addMenu(ActionManager::icon(QStringLiteral("add")), tr("Add Output"));
  add->setToolTipsVisible(true);
  for (const auto &d: c->outputs)
    add->addAction(typed(d), this, [this, id, n = d.name] { addOutput(id, n); })->setToolTip(d.description);
  menu.addAction(tr("Rename…"), this, [this, id] {
    const eng::Entity *x = m_engine->entityById(id);
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Rename"), tr("Targetname"), QLineEdit::Normal, x ? x->name : QString(), &ok);
    if (ok)
      m_engine->renameEntity(id, n);
  });
  menu.addSeparator();
  menu.addAction(ActionManager::icon(QStringLiteral("restrict_select_off")), tr("Select in Level"), this,
                 [this, id] { m_engine->selectEntitiesWithBrushes({id}); });
  menu.addAction(ActionManager::icon(QStringLiteral("zoom_selected")), tr("Frame in 3D View"), this, [this, id] {
    m_engine->selectEntitiesWithBrushes({id});
    m_engine->focusSelection();
  });
  menu.addAction(tr("Select Its Targets in Level"), this, [this, id] {
    m_engine->selectEntitiesWithBrushes({id});
    m_engine->selectIOTargets();
  });
  menu.addAction(tr("Select Its Sources in Level"), this, [this, id] {
    m_engine->selectEntitiesWithBrushes({id});
    m_engine->selectIOSources();
  });
  menu.addSeparator();
  QMenu *fire = menu.addMenu(ActionManager::icon(QStringLiteral("play")), tr("Simulate: Send Input"));
  for (const auto &d: c->inputs)
    fire->addAction(typed(d), this, [this, id, n = d.name] {
      m_engine->ioSimFire(QStringLiteral("#%1").arg(id), n);
      m_testDock->show();
    });
  QMenu *fireOut = menu.addMenu(tr("Simulate: Fire Output"));
  for (const auto &d: c->outputs)
    fireOut->addAction(typed(d), this, [this, id, n = d.name] {
      m_engine->ioSimFireOutput(id, n);
      m_testDock->show();
    });
  menu.addSeparator();
  menu.addAction(ActionManager::icon(QStringLiteral("trash")), tr("Delete Entity"), this,
                 [this, id] { m_engine->deleteEntities({id}); });
  menu.exec(globalPos);
}

void IOEditor::showConnectionMenu(const ConnRef &ref, const QPoint &globalPos) {
  const eng::Entity *e = m_engine->entityById(ref.entity);
  if (!e || ref.row < 0 || ref.row >= e->outputs.size())
    return;
  if (!m_conns.contains(ref))
    selectConnections({ref});
  const Connection c = e->outputs[ref.row];
  QMenu menu(this);
  menu.addSection(shortText(c));
  menu.addAction(ActionManager::icon(QStringLiteral("duplicate")), tr("Duplicate"), this,
                 [this] { duplicateConnections(m_conns); });
  menu.addAction(c.disabled ? tr("Enable") : tr("Disable"), this, [this, ref, c] {
    const eng::Entity *x = m_engine->entityById(ref.entity);
    if (!x || ref.row >= x->outputs.size())
      return;
    ConnectionList list = x->outputs;
    list[ref.row].disabled = !c.disabled;
    m_engine->setEntityOutputs(ref.entity, list, c.disabled ? tr("Enable connection") : tr("Disable connection"));
  });
  menu.addAction(ActionManager::icon(QStringLiteral("play")), tr("Simulate: Send It Now"), this, [this, c] {
    m_engine->ioSimFire(c.target, c.input, c.param, 0.0);
    m_testDock->show();
  });
  const auto r = eng::io::resolve(entitiesOf(m_engine), c.target, ref.entity);
  if (!r.ids.empty())
    menu.addAction(ActionManager::icon(QStringLiteral("forward")), tr("Go to Target"), this, [this, id = r.ids.front()] {
      selectEntity(id);
      m_graph->centerOnEntity(id, true);
    });
  menu.addSeparator();
  menu.addAction(ActionManager::icon(QStringLiteral("trash")), tr("Delete"), this, [this] { deleteConnections(m_conns); });
  menu.exec(globalPos);
}

void IOEditor::prepareTest(const quint32 entity) {
  const eng::Entity *e = m_engine->entityById(entity);
  m_fireTarget->refresh(entity);
  if (e) {
    m_fireTarget->setText(e->name.isEmpty() ? QStringLiteral("#%1").arg(entity) : e->name);
    m_fireInput->clear();
    if (e->cls())
      for (const auto &d: e->cls()->inputs)
        m_fireInput->addItem(typed(d), d.name);
  }
}

} // namespace ui
