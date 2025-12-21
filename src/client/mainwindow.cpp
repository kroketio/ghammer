#include "mainwindow.h"
#include "client/ui/io_editor.h"
#include "client/engine/entity.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QAbstractButton>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include "client/ui/tool_shelf.h"
#include "client/ui/uv_editor.h"
#include "client/ui/layout_designer.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStatusBar>

#include "client/net/asset_client.h"
#include <memory>
#include "client/net/file_fetcher.h"
#include "client/net/texture_catalog.h"
#include "client/ui/server_status.h"
#include "client/ui/pie_menu.h"
#include "client/ui/quick_material_model.h"
#include "client/ui/thumb_image_provider.h"
#include "client/ui/copy_texture_dialog.h"
#include "client/ui/texture_actions.h"
#include "client/ui/texture_browser.h"
#include "client/ui/texture_packs.h"
#include "client/net/texture_edit_sessions.h"
#include "client/vulkan/vk_shared.h"
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include "shared/lib/globals.h"

#include "client/ctx.h"
#include "client/engine/map_format.h"
#include "client/engine/scene.h"
#include "client/engine/texture_library.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/autoselect_panel.h"
#include "client/ui/console_panel.h"
#include "client/ui/entity_browser.h"
#include "client/ui/issues_panel.h"
#include "client/ui/model_browser.h"
#include "client/ui/scale.h"
#include "client/ui/widgets.h"
#include "client/ui/history_panel.h"
#include "client/ui/inspector.h"
#include "client/ui/material_browser.h"
#include "client/ui/outliner.h"
#include "client/ui/viewport_notices.h"
#include "client/ui/project_panel.h"
#include "client/ui/reference_panel.h"
#include "client/project/document_manager.h"
#include <QStackedWidget>
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"
#include "shared/lib/config.h"
#include "client/ui/pack_install_window.h"
#include "client/ui/paths_page.h"
#include "client/project/project_manager.h"

using ui::ActionManager;

MainWindow *MainWindow::pMainWindow = nullptr;

MainWindow *MainWindow::getInstance() { return pMainWindow; }

MainWindow::MainWindow(Ctx *ctx, const QString &project, QWidget *parent)
    : QMainWindow(parent), ctx_(ctx),
      keys_wsad_({Qt::Key_W, Qt::Key_S, Qt::Key_A, Qt::Key_D, Qt::Key_Q, Qt::Key_E}) {
  pMainWindow = this;
  setObjectName(QStringLiteral("MainWindow"));
  setDockNestingEnabled(true);
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

  g::engine3D = new vk::Engine3D(this);
  // The engine holds one level at a time.
  m_docs = new proj::DocumentManager(g::engine3D, this);
  // Extra entity classes before any panel lists them.
  if (const QString defs = prefs::get().value(QStringLiteral("Paths/EntityDefinitions")).toString(); !defs.isEmpty()) {
    int n = 0;
    for (const QFileInfo &fi: QDir(defs).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
      n += eng::EntityRegistry::instance().loadJson(fi.absoluteFilePath());
    qInfo().noquote() << QStringLiteral("[entities] %1 class(es) from %2").arg(n).arg(defs);
  }
  g::textureManager = &vk::TextureManager::instance();
  connect(g::engine3D, &vk::Engine3D::dirtyChanged, this, &MainWindow::updateWindowTitle);
  connect(g::engine3D, &vk::Engine3D::toastRequested, this, &MainWindow::showToast);
  connect(g::engine3D, &vk::Engine3D::selectionChanged, this, [this] { m_textCopyOwner = nullptr; });
  connect(ctx_, &Ctx::editorModeChanged, this, &MainWindow::onEditorModeChanged);

  // Rescanning re-resolves any material waiting for its maps.
  auto &texlib = eng::TextureLibrary::instance();
  connect(&texlib, &eng::TextureLibrary::changed, this, [] { vk::TextureManager::instance().reresolveLibrary(); });
  eng::TextureLibrary::instance().setRoot(qEnvironmentVariable("EDITOR_TEXTURE_DIR"));

  // Connect now, follow the preferences, keep the download cache under its limit.
  net::TextureCatalog::instance();
  net::AssetClient::instance().start();
  net::FileFetcher::instance().trimCache();
  // After a server switch (not the first connection), say where the assets come from once it answers.
  {
    auto &client = net::AssetClient::instance();
    auto switched = std::make_shared<bool>(false);
    connect(&client, &net::AssetClient::endpointChanged, this, [switched] { *switched = true; });
    connect(&client, &net::AssetClient::stateChanged, this, [this, switched](net::AssetClient::State st) {
      if (st != net::AssetClient::State::Online || !*switched)
        return;
      *switched = false;
      const auto &c = net::AssetClient::instance();
      showToast(c.isLocal() ? tr("Asset server: local") : tr("Asset server: %1").arg(c.httpBase().host()));
    });
  }

  // Key events follow the mouse over the views, not the keyboard focus.
  qApp->installEventFilter(this);

  createQmlContent();
  registerActions();
  // QML shortcut tooltips were built before the actions: re-read them now and on every rebind.
  ctx_->noteShortcutsChanged();
  connect(&prefs::get(), &prefs::Preferences::changed, ctx_, [this](const QString &key) {
    if (key.startsWith(QLatin1String("Keys/")))
      ctx_->noteShortcutsChanged();
  });
  createDocks();
  createToolBars();
  createDocumentBar();
  buildMenuBar();
  createStatusBar();
  // A window shortcut needs a widget that owns it, so every action lives on the window too.
  ui::ActionManager::instance().attachTo(this);
  // An empty map from the start, so the 3D view draws its sky.
  g::engine3D->setScene(QSharedPointer<eng::Scene>(new eng::Scene()));
  g::engine3D->markClean();
  setupProjects(project);
  setupDocuments();

  const QScreen *screen = QGuiApplication::primaryScreen();
  const QRect avail = screen->availableGeometry();
  resize(std::min(1900, avail.width() - 40), std::min(1200, avail.height() - 40));
  move(avail.center() - rect().center());
  m_defaultState = saveState();
  restoreWindowLayout();
  updateWindowTitle();
  show();
  QTimer::singleShot(0, this, [this] { restoreSessionView(); });
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
    saveWindowLayout();
    config()->sync();
  });

  rebuildRecentProjectsMenu();
  startAutosave();
  QTimer::singleShot(250, this, [this] { maybeReopenLastMap(); });
  QTimer::singleShot(600, this, [this] { maybeShowFirstRun(); });
}

// A new editor has no textures, skyboxes or LUTs: the first time a project is open, Install Asset Packs comes up.
void MainWindow::maybeShowFirstRun() {
  if (scriptedRun() || qEnvironmentVariableIsSet("EDITOR_PROJECT") || !commandLineMap().isEmpty() ||
      !proj::ProjectManager::instance().isOpen() || !config()->get(ConfigKeys::FirstRun).toBool())
    return;
  config()->set(ConfigKeys::FirstRun, false);
  ui::PackInstallWindow::open(this, true);
}

MainWindow::~MainWindow() {
  qApp->removeEventFilter(this);
  ui::ActionManager::instance().setGate({});
  saveWindowLayout();
  // GPU teardown while the QRhi is still alive.
  if (quickContent_)
    quickContent_->setSource(QUrl());
  // Parked levels before the engine they were parked from.
  delete m_docs;
  m_docs = nullptr;
  delete g::engine3D;
  g::engine3D = nullptr;
}

void MainWindow::createQmlContent() {
  quickContent_ = new QQQuickWidget(this);
  auto *qctx = quickContent_->rootContext();
  qctx->setContextProperty("ctx", ctx_);
  qctx->setContextProperty("window", this);
  qctx->setContextProperty("widget", quickContent_);
  qctx->setContextProperty("engine3D", g::engine3D);
  m_notices = new ui::ViewportNotices(this);
  qctx->setContextProperty("viewportNotices", m_notices);
  setupViewportNotices();
  // Shift+S pie menu and Quick Material strip; thumbnails come through image://thumb.
  m_quickMaterials = new ui::QuickMaterialModel(g::engine3D, this);
  m_pie = new ui::PieMenu(quickContent_, m_quickMaterials, m_notices, this);
  qctx->setContextProperty("pieMenu", m_pie);
  qctx->setContextProperty("quickMaterials", m_quickMaterials);
  qctx->engine()->addImageProvider(QLatin1String("thumb"), new ui::ThumbImageProvider());
  qctx->setContextProperty("prefShowViewGizmo", prefs::b("Viewport/ShowViewGizmo"));
  prefs::get().watch(QStringLiteral("Viewport/ShowViewGizmo"), this, [qctx](const QVariant &v) {
    qctx->setContextProperty("prefShowViewGizmo", v.toBool());
  });
  quickContent_->setSource(QUrl(QStringLiteral("qrc:/Main/qml/Main.qml")));
  quickContent_->setResizeMode(QQuickWidget::SizeRootObjectToView);
  quickContent_->setFocusPolicy(Qt::StrongFocus);
  quickContent_->setMinimumSize(320, 240);
  // The views stay alive behind an image tab: keep the scene graph and GPU resources when hidden (QQuickWidget
  // drops them by default).
  quickContent_->quickWindow()->setPersistentGraphics(true);
  quickContent_->quickWindow()->setPersistentSceneGraph(true);
  m_central = new QStackedWidget(this);
  m_central->addWidget(quickContent_);
  auto *empty = new QLabel(tr("No level open"), m_central);
  empty->setAlignment(Qt::AlignCenter);
  empty->setEnabled(false);
  ui::scale::setRole(empty, ui::scale::Font::Heading);
  m_emptyPage = empty;
  m_central->addWidget(m_emptyPage);
  setCentralWidget(m_central);
}

void MainWindow::createDocks() {
  auto makeDock = [this](const QString &title, const QString &name, QWidget *w, Qt::DockWidgetArea area) {
    auto *d = new QDockWidget(title, this);
    d->setObjectName(name);
    d->setWidget(w);
    d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    addDockWidget(area, d);
    // Docking / floating moves the title bar away without a leave event, leaving the float button hovered: clear it.
    connect(d, &QDockWidget::topLevelChanged, d, [d] {
      QTimer::singleShot(0, d, [d] {
        for (QAbstractButton *b: d->findChildren<QAbstractButton *>(QString(), Qt::FindDirectChildrenOnly)) {
          if (!b->objectName().startsWith(QLatin1String("qt_dockwidget_")))
            continue;
          QEvent leave(QEvent::Leave);
          QCoreApplication::sendEvent(b, &leave);
          b->setAttribute(Qt::WA_UnderMouse, false);
          b->setDown(false);
          b->update();
        }
      });
    });
    return d;
  };

  m_outliner = new ui::Outliner(g::engine3D, this);
  m_history = new ui::HistoryPanel(g::engine3D, this);
  m_inspector = new ui::Inspector(g::engine3D, this);
  m_materials = new ui::MaterialBrowser(g::engine3D, this);
  m_console = new ui::ConsolePanel(this);
  m_projectPanel = new ui::ProjectPanel(m_docs, this);
  m_reference = new ui::ReferencePanel(this);

  m_dockProject = makeDock(tr("Project"), QStringLiteral("dock.project"), m_projectPanel, Qt::LeftDockWidgetArea);
  m_dockOutliner = makeDock(tr("Outliner"), QStringLiteral("dock.outliner"), m_outliner, Qt::LeftDockWidgetArea);
  m_dockHistory = makeDock(tr("History"), QStringLiteral("dock.history"), m_history, Qt::LeftDockWidgetArea);
  m_dockEntities = makeDock(tr("Entities"), QStringLiteral("dock.entities"), new ui::EntityBrowser(g::engine3D, this),
                            Qt::LeftDockWidgetArea);
  m_dockModels = makeDock(tr("Models"), QStringLiteral("dock.models"), new ui::ModelBrowser(g::engine3D, this),
                          Qt::LeftDockWidgetArea);
  m_dockInspector = makeDock(tr("Inspector"), QStringLiteral("dock.inspector"), m_inspector, Qt::RightDockWidgetArea);
  m_dockMaterials = makeDock(tr("Materials"), QStringLiteral("dock.materials"), m_materials, Qt::BottomDockWidgetArea);
  m_dockAutoSelect = makeDock(tr("Auto-select"), QStringLiteral("dock.autoselect"),
                              new ui::AutoSelectDock(g::engine3D, this), Qt::RightDockWidgetArea);
  m_dockConsole = makeDock(tr("Console"), QStringLiteral("dock.console"), m_console, Qt::BottomDockWidgetArea);
  m_dockIssues = makeDock(tr("Issues"), QStringLiteral("dock.issues"), new ui::IssuesPanel(g::engine3D, this),
                          Qt::BottomDockWidgetArea);
  m_dockUv = makeDock(tr("UV Editor"), QStringLiteral("dock.uv"), new ui::UvEditor(g::engine3D, this),
                      Qt::RightDockWidgetArea);
  // Not in the default layout.
  m_dockReference = makeDock(tr("Reference Image"), QStringLiteral("dock.reference"), m_reference,
                             Qt::RightDockWidgetArea);
  // Default arrangement (a saved one wins, see restoreWindowLayout; proportions in applyDefaultDockSizes).
  splitDockWidget(m_dockProject, m_dockOutliner, Qt::Vertical);
  splitDockWidget(m_dockMaterials, m_dockConsole, Qt::Horizontal);
  tabifyDockWidget(m_dockConsole, m_dockIssues);
  m_dockConsole->raise();

  tabifyDockWidget(m_dockEntities, m_dockModels);
  tabifyDockWidget(m_dockEntities, m_dockHistory);
  m_dockEntities->hide();
  m_dockModels->hide();
  m_dockHistory->hide();
  splitDockWidget(m_dockInspector, m_dockAutoSelect, Qt::Vertical);
  m_dockInspector->raise();
  m_dockUv->hide();
  m_dockReference->hide();

  connect(m_projectPanel, &ui::ProjectPanel::openRequested, this, &MainWindow::openPath);
  connect(m_projectPanel, &ui::ProjectPanel::openExternallyRequested, this, &MainWindow::openExternally);
  connect(m_projectPanel, &ui::ProjectPanel::newLevelRequested, this, [this](const QString &dir) { newLevelDialog(dir); });
  connect(m_projectPanel, &ui::ProjectPanel::createGodotProjectRequested, this, &MainWindow::setupProjectGodot);
  connect(m_projectPanel, &ui::ProjectPanel::referenceRequested, this, &MainWindow::showReference);
  m_projectPanel->closeDocumentsUnder = [this](const QString &path) { return closeDocumentsUnder(path); };

  connect(m_inspector, &ui::Inspector::browseMaterialRequested, this, [this](const QString &name) {
    m_dockMaterials->show();
    m_dockMaterials->raise();
    m_materials->reveal(name);
  });

  // Every view asks the hub, the main window decides who answers.
  auto &hub = ui::TextureHub::instance();
  // The inspector's section when the texture is on the selected faces (Material mode), else the browser's details.
  connect(&hub, &ui::TextureHub::showPropertiesRequested, this, [this](const QString &ref) {
    bool onSelection = false;
    for (const QString &r: g::engine3D->selectionMaterialRefs())
      onSelection |= ref.isEmpty() || r.compare(ref, Qt::CaseInsensitive) == 0;
    if (onSelection) {
      m_inspector->revealTextureProperties(ref);
      if (m_inspector->showsTextureProperties()) {
        m_dockInspector->show();
        m_dockInspector->raise();
        return;
      }
    }
    ui::TextureBrowserWindow::open(g::engine3D, this, ref.isEmpty() ? g::engine3D->currentMaterial() : ref);
  });
  connect(&hub, &ui::TextureHub::openBrowserRequested, this,
          [this](const QString &ref) { ui::TextureBrowserWindow::open(g::engine3D, this, ref); });
  connect(&hub, &ui::TextureHub::openPacksRequested, this,
          [this](const QString &ref) { ui::TexturePacksWindow::open(g::engine3D, this, ref); });
  connect(&hub, &ui::TextureHub::copyRequested, this, [this](const QString &ref) {
    QWidget *parent = QApplication::activeWindow() ? QApplication::activeWindow() : this;
    ui::CopyTextureDialog::run(g::engine3D, ref, parent);
  });
  connect(&hub, &ui::TextureHub::editRequested, this, [this](const QString &ref, const QString &map) {
    net::TextureEditSessions::instance().start(ref, map, QApplication::activeWindow() ? QApplication::activeWindow() : this);
  });
  // Opens unfiltered, without searching for the current texture.
  connect(m_materials, &ui::MaterialBrowser::openBrowserRequested, this,
          [this] { ui::TextureBrowserWindow::open(g::engine3D, this); });
  connect(g::engine3D, &vk::Engine3D::currentMaterialChanged, this, [](const QString &ref) { ui::noteRecentTexture(ref); });
  connect(&vk::MaterialEvents::instance(), &vk::MaterialEvents::renamed, this, [](const QString &o, const QString &n) {
    qInfo().noquote() << QStringLiteral("[material] %1 was renamed on the server to %2; faces follow").arg(o, n);
  });
  connect(&net::TextureEditSessions::instance(), &net::TextureEditSessions::message, this,
          [this](const QString &m) { showToast(m); });

  // Panel toggles as commands so the palette and shortcut editor list them.
  {
    struct P {
      QDockWidget *dock;
      const char *id, *icon;
      const char *key = "";
    };
    for (const P &p: {P{m_dockProject, "project", "file_folder", "Alt+1"},
                      P{m_dockOutliner, "outliner", "outliner", "Alt+2"},
                      P{m_dockEntities, "entities", "outliner_ob_light"},
                      P{m_dockModels, "models", "mesh_monkey"}, P{m_dockHistory, "history", "recover_last"},
                      P{m_dockInspector, "inspector", "properties"},
                      P{m_dockAutoSelect, "autoselect", "select_extend"}, P{m_dockMaterials, "materials", "texture"},
                      P{m_dockConsole, "console", "console"}, P{m_dockIssues, "issues", "error"},
                      P{m_dockUv, "uv", "uv"}, P{m_dockReference, "reference", "file_image"}}) {
      QDockWidget *d = p.dock;
      d->toggleViewAction()->setIcon(ActionManager::icon(QString::fromLatin1(p.icon)));
      ActionManager::instance().add(
          ActionManager::Spec{QStringLiteral("view.panel.") + QLatin1String(p.id), tr("Panel: %1").arg(d->windowTitle()),
                              QString::fromLatin1(p.icon), QString::fromLatin1(p.key),
                              tr("Show or hide the %1 panel").arg(d->windowTitle()), "Panels", true},
          [this, d] {
            if (d->isVisible() && !d->visibleRegion().isEmpty()) {
              d->hide();
            } else {
              d->show();
              d->raise();
              if (d == m_dockProject)
                m_projectPanel->focusTree();
            }
          },
          {}, [d] { return d->isVisible(); });
    }
  }

  m_toolShelf = new ui::ToolShelf(this);
  addToolBar(Qt::LeftToolBarArea, m_toolShelf);
  rebuildToolShelf();

  connect(m_console, &ui::ConsolePanel::countsChanged, this, [this](int warnings, int errors) {
    if (!m_statusLog)
      return;
    // A link opening the Console on its warnings and errors.
    const auto link = [](const QString &color, const QString &text) {
      return QStringLiteral("<a href='console' style='color:%1; text-decoration:none'>%2</a>").arg(color, text);
    };
    m_statusLog->setText(errors > 0     ? link(QStringLiteral("#ff6b6b"), QStringLiteral("✖ %1").arg(errors))
                         : warnings > 0 ? link(QStringLiteral("#f0b45a"), QStringLiteral("⚠ %1").arg(warnings))
                                        : QString());
    m_statusLog->setToolTip(errors > 0 ? tr("%n error(s) in the console", nullptr, errors)
                                       : tr("%n warning(s) in the console", nullptr, warnings));
  });
}

void MainWindow::createToolBars() {
  auto makeBar = [this](const QString &title, const QString &name) {
    auto *tb = addToolBar(title);
    tb->setObjectName(name);
    // Static top bar: nothing moves or floats.
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setProperty("topBar", true);  // checked buttons are light blue (theme)
    ui::scale::bind(tb, [tb] { tb->setIconSize(ui::scale::icon(ui::scale::Icon::Toolbar)); });
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    return tb;
  };
  auto add = [](QToolBar *tb, const char *id) {
    if (QAction *a = ui::act(QString::fromLatin1(id)))
      tb->addAction(a);
  };

  // The open project's name (gHammer without one).
  QToolBar *title = makeBar(tr("Title"), QStringLiteral("toolbar.title"));
  auto *name = new QLabel(QStringLiteral("gHammer"), title);
  m_titleLabel = name;
  name->setStyleSheet(QStringLiteral("font-weight: bold;")); // the size keeps following the UI scale
  name->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  name->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  name->setContentsMargins(8, 0, 12, 0);
  title->addWidget(name);

  QToolBar *mode = makeBar(tr("Mode"), QStringLiteral("toolbar.mode"));
  mode->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto *modeGroup = new QActionGroup(this);
  for (const char *id: {"mode.object", "mode.edit", "mode.material"}) {
    QAction *a = ui::act(QString::fromLatin1(id));
    modeGroup->addAction(a);
    mode->addAction(a);
  }

  QToolBar *gizmo = makeBar(tr("Gizmo"), QStringLiteral("toolbar.gizmo"));
  {
    auto *g = new QActionGroup(this);
    for (const char *id: {"tool.move", "tool.rotate", "tool.scale"}) {
      QAction *a = ui::act(QString::fromLatin1(id));
      g->addAction(a);
      gizmo->addAction(a);
    }
  }

  QToolBar *tools = makeBar(tr("Tools"), QStringLiteral("toolbar.tools"));
  {
    // One button showing the current shape, menu of the rest.
    auto *shapeBtn = new QToolButton(tools);
    shapeBtn->setPopupMode(QToolButton::MenuButtonPopup);
    auto *shapeMenu = new QMenu(shapeBtn);
    auto *shapeGroup = new QActionGroup(shapeMenu);
    for (const char *id: {"brush.shapeBox", "brush.shapeWedge", "brush.shapeCylinder", "brush.shapeCone",
                          "brush.shapePyramid", "brush.shapeSphere", "brush.shapeStairs", "brush.shapeArch"}) {
      QAction *a = ui::act(QString::fromLatin1(id));
      shapeGroup->addAction(a);
      shapeMenu->addAction(a);
    }
    shapeBtn->setMenu(shapeMenu);
    shapeBtn->setDefaultAction(ui::act(QStringLiteral("brush.create")));
    const auto syncShape = [shapeBtn, shapeMenu] {
      for (QAction *a: shapeMenu->actions())
        if (a->isChecked())
          shapeBtn->setIcon(a->icon());
    };
    connect(shapeMenu, &QMenu::triggered, shapeBtn, syncShape);
    QTimer::singleShot(0, shapeBtn, syncShape);
    tools->addWidget(shapeBtn);
  }
  for (const char *id: {"brush.cut", "brush.bevel", "csg.subtract", "csg.hollow", "csg.merge"})
    add(tools, id);
  tools->addSeparator();
  for (const char *id: {"tex.apply", "tex.pick", "tex.alignWorld", "tex.fit"})
    add(tools, id);

  QToolBar *grid = makeBar(tr("Grid"), QStringLiteral("toolbar.grid"));
  auto *gridLabel = new QLabel(tr(" Grid "), grid);
  gridLabel->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  grid->addWidget(gridLabel);
  m_gridCombo = new QComboBox(grid);
  m_gridCombo->setToolTip(tr("Grid size ( [ and ] )"));
  for (int level = 1; level <= 10; ++level)
    m_gridCombo->addItem(QString::number(1 << (level - 1)), level);
  grid->addWidget(m_gridCombo);
  connect(m_gridCombo, &QComboBox::activated, this,
          [this](int idx) { g::engine3D->setGridLevel(m_gridCombo->itemData(idx).toInt()); });
  const auto syncGrid = [this] {
    const int idx = m_gridCombo->findData(g::engine3D->gridLevel());
    if (idx >= 0) {
      QSignalBlocker b(m_gridCombo);
      m_gridCombo->setCurrentIndex(idx);
    }
    refreshStatus();
  };
  connect(g::engine3D, &vk::Engine3D::gridLevelChanged, this, syncGrid);
  syncGrid();
  grid->addSeparator();
  add(grid, "brush.textureLock");
  add(grid, "brush.scaleLock");

  QToolBar *view = makeBar(tr("View"), QStringLiteral("toolbar.view"));
  for (const char *id: {"view.layoutSingle", "view.layoutQuad", "view.layoutSplit"})
    add(view, id);
  auto *layoutGroup = new QActionGroup(this);
  for (const char *id: {"view.layoutSingle", "view.layoutQuad", "view.layoutSplit"})
    layoutGroup->addAction(ui::act(QString::fromLatin1(id)));
  ui::act(QStringLiteral("view.layoutQuad"))->setChecked(true);
  view->addSeparator();
  add(view, "view.lit");
  add(view, "view.showGenerated");
  add(view, "view.focus");
  add(view, "view.showAll");
  view->addSeparator();
  add(view, "physics.playAll");
  add(view, "physics.playSelected");
  add(view, "physics.pause");
  add(view, "physics.reset");
  view->addSeparator();
  add(view, "view.zen");
  {
    auto *zenSize = new QComboBox(view);
    zenSize->setToolTip(tr("Zen mode box (Source units)"));
    if (const prefs::Def *d = prefs::get().def(QStringLiteral("Viewport/ZenBox")))
      for (int i = 0; i < d->choices.size(); ++i)
        zenSize->addItem(d->choices[i], d->choiceValues.value(i));
    view->addWidget(zenSize);
    connect(zenSize, &QComboBox::activated, this, [zenSize](int idx) {
      prefs::get().setValue(QStringLiteral("Viewport/ZenBox"), zenSize->itemData(idx));
    });
    prefs::get().watch(QStringLiteral("Viewport/ZenBox"), zenSize, [zenSize](const QVariant &v) {
      const QSignalBlocker b(zenSize);
      zenSize->setCurrentIndex(std::max(0, zenSize->findData(v)));
    });
  }
  view->addSeparator();
  add(view, "file.exportOnSave");
  setupGodotSaveTarget(view);
  add(view, "file.startGodot");
}

void MainWindow::createStatusBar() {
  QStatusBar *sb = statusBar();
  sb->setSizeGripEnabled(false);
  auto mk = [sb](bool permanent, int minWidth = 0) {
    auto *l = new QLabel(sb);
    ui::scale::bind(l, [l, minWidth] { l->setMinimumWidth(ui::scale::tpx(minWidth)); });
    l->setStyleSheet(QStringLiteral("color:#aab4cf; padding: 0 8px;"));
    if (permanent)
      sb->addPermanentWidget(l);
    else
      sb->addWidget(l, 1);
    return l;
  };
  m_statusLog = mk(true);
  m_statusLog->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  connect(m_statusLog, &QLabel::linkActivated, this, [this] {
    m_dockConsole->show();
    m_dockConsole->raise();
    m_console->showProblems();
  });
  m_statusSelection = mk(true, 160);
  m_statusCamera = mk(true, 220);
  m_statusFrame = mk(true, 90);
  sb->addPermanentWidget(new ui::ServerStatus(sb));

  m_statusTimer = new QTimer(this);
  m_statusTimer->setInterval(250);
  connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatus);
  m_statusTimer->start();
  for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::sceneChanged})
    connect(g::engine3D, sig, this, &MainWindow::refreshStatus);
}

void MainWindow::refreshStatus() {
  auto *e = g::engine3D;
  if (!e || !m_statusSelection)
    return;
  const size_t o = e->selectedObjects().size();
  const size_t f = e->selectedFaces().size();
  QString sel;
  if (f > 0)
    sel = tr("%1 face%2").arg(f).arg(f == 1 ? "" : "s");
  else if (o > 0)
    sel = tr("%1 brush%2").arg(o).arg(o == 1 ? "" : "es");
  glm::vec3 mn, mx;
  if (!sel.isEmpty() && e->selectionBounds(mn, mx)) {
    const glm::vec3 s = (mx - mn) * vk::Engine3D::kSourceUnitsPerWorldUnit;
    sel += QStringLiteral("  <span style='color:#6c7898'>%1×%2×%3</span>")
               .arg(s.x, 0, 'f', 0).arg(s.y, 0, 'f', 0).arg(s.z, 0, 'f', 0);
  }
  const int brushes = e->scene() ? int(e->scene()->gameObjects.size()) : 0;
  m_statusSelection->setText(sel.isEmpty() ? tr("%1 brushes").arg(brushes) : sel);
  const glm::vec3 p = e->camera()->getPosition() * vk::Engine3D::kSourceUnitsPerWorldUnit;
  m_statusCamera->setText(QStringLiteral("cam %1 %2 %3").arg(p.x, 0, 'f', 0).arg(p.y, 0, 'f', 0).arg(p.z, 0, 'f', 0));
  if (prefs::b("Viewport/ShowFps"))
    m_statusFrame->setText(QStringLiteral("%1 ms").arg(e->lastFrameMs(), 0, 'f', 1));
  else
    m_statusFrame->clear();
  const int pending = vk::TextureManager::instance().pendingCount();
  if (pending > 0)
    m_statusFrame->setText(m_statusFrame->text() + tr("  ⟳ %1 tex").arg(pending));
}

void MainWindow::restoreWindowLayout() {
  // Bump when the dock set changes so an old saved arrangement is not applied.
  if (config()->getRaw(QStringLiteral("Window/LayoutVersion")).toInt() != kLayoutVersion)
    return;
  const QByteArray geo = config()->getRaw(QStringLiteral("Window/Geometry")).toByteArray();
  const QByteArray state = config()->getRaw(QStringLiteral("Window/State")).toByteArray();
  if (!geo.isEmpty())
    restoreGeometry(QByteArray::fromBase64(geo));
  if (!state.isEmpty())
    m_layoutRestored = restoreState(QByteArray::fromBase64(state));
}

// The default arrangement's proportions from the real window size. Runs once laid out, on first start and on
// Reset Window Layout.
void MainWindow::applyDefaultDockSizes() {
  using ui::scale::px;
  const auto split = [this](QDockWidget *a, QDockWidget *b, Qt::Orientation o, double first) {
    const int total = o == Qt::Vertical ? a->height() + b->height() : a->width() + b->width();
    if (total <= 0)
      return;
    const int sa = int(std::lround(total * first));
    resizeDocks({a, b}, {sa, total - sa}, o);
  };
  resizeDocks({m_dockProject, m_dockInspector}, {px(300), px(320)}, Qt::Horizontal);
  resizeDocks({m_dockMaterials}, {px(240)}, Qt::Vertical);
  split(m_dockProject, m_dockOutliner, Qt::Vertical, 0.5);
  split(m_dockInspector, m_dockAutoSelect, Qt::Vertical, 0.7);
  split(m_dockMaterials, m_dockConsole, Qt::Horizontal, 0.5);
}

void MainWindow::saveWindowLayout() {
  if (!m_sessionRestored)
    return;
  config()->setRaw(QStringLiteral("Window/LayoutVersion"), kLayoutVersion);
  config()->setRaw(QStringLiteral("Window/Geometry"), QString::fromLatin1(saveGeometry().toBase64()));
  config()->setRaw(QStringLiteral("Window/State"), QString::fromLatin1(saveState().toBase64()));
}

void MainWindow::restoreSessionView() {
  ctx_->switchMode(g::EditorMode::OBJECT);
  setViewportLayout(1);
  if (!m_layoutRestored)
    applyDefaultDockSizes();
  m_sessionRestored = true;
}

void MainWindow::resetWindowLayout() {
  restoreState(m_defaultState);
  QTimer::singleShot(0, this, [this] { applyDefaultDockSizes(); });
}

int MainWindow::viewportLayout() const {
  // QML owns the layout, so ask it.
  if (QObject *root = quickContent_ ? quickContent_->rootObject() : nullptr)
    return root->property("layoutMode").toInt();
  return 1;
}

void MainWindow::setViewportLayout(int mode) {
  if (QObject *root = quickContent_ ? quickContent_->rootObject() : nullptr)
    QMetaObject::invokeMethod(root, "setLayout", Q_ARG(QVariant, mode));
  const char *ids[3] = {"view.layoutSingle", "view.layoutQuad", "view.layoutSplit"};
  if (mode >= 0 && mode < 3)
    if (QAction *a = ui::act(QString::fromLatin1(ids[mode])))
      a->setChecked(true);
}

void MainWindow::showToast(const QString &text) {
  if (m_notices)
    m_notices->flash(text);
}

bool MainWindow::dropOnViewport(const QString &kind, const QString &name, qreal x, qreal y) {
  if (!quickContent_ || name.isEmpty())
    return false;
  const QPointF px = quickContent_->toViewportPixels(QPointF(x, y));
  if (kind == QLatin1String("entity")) {
    const bool ok = g::engine3D->placeEntityAt(name, float(px.x()), float(px.y())) != 0;
    if (ok)
      showToast(tr("Placed %1").arg(name));
    return ok;
  }
  if (kind == QLatin1String("model")) {
    const glm::vec3 at = g::engine3D->placementPoint(float(px.x()), float(px.y()), 0.0f);
    return g::engine3D->addEntity(QStringLiteral("prop_static"), at, {{QStringLiteral("model"), name}}) != 0;
  }
  if (kind == QLatin1String("material")) {
    g::engine3D->setCurrentMaterial(name);
    return g::engine3D->applyMaterialAt(name, float(px.x()), float(px.y()));
  }
  return false;
}

void MainWindow::rebuildToolShelf() {
  if (!m_toolShelf)
    return;
  using S = ui::ToolShelf::Section;
  QList<S> sections;
  switch (ctx_->editorMode) {
    case g::EditorMode::EDIT:
      sections = {
          S{tr("Select"), {{"vertex.subAll", tr("Vert+\nEdge")}, {"vertex.subVerts", tr("Verts")},
                           {"vertex.subEdges", tr("Edges")}, {"vertex.subFaces", tr("Faces")},
                           {"vertex.selectAll", tr("All")}, {"vertex.clear", tr("None")}}},
          S{tr("Modify"), {{"vertex.scale", tr("Scale")}, {"vertex.weld", tr("Weld")},
                           {"vertex.splitEdge", tr("Split")}, {"brush.bevel", tr("Bevel")}, {"vertex.snap", tr("Snap")},
                           {"brush.extrudeOut", tr("Extrude")}, {"brush.extrudeNew", tr("Extr. New")}}},
          S{tr("Brush"), {{"csg.merge", tr("Merge")}, {"xform.snap", tr("Snap All")},
                          {"brush.textureLock", tr("Tex Lock")}, {"edit.delete", tr("Delete")}}},
      };
      break;
    case g::EditorMode::MATERIAL:
      sections = {
          S{tr("Faces"), {{"brush.extrudeOut", tr("Extrude")}, {"brush.extrudeIn", tr("Push In")},
                          {"brush.extrudeNew", tr("Extr. New")}}},
          S{tr("Material"), {{"tex.apply", tr("Apply")}, {"tex.pick", tr("Pick")},
                             {"tex.copyAttr", tr("Copy")}, {"tex.pasteAttr", tr("Paste")},
                             {"tex.wrap", tr("Wrap")}, {"edit.selectMaterial", tr("Same Mat")}}},
          S{tr("Align"), {{"tex.alignWorld", tr("World")}, {"tex.alignFace", tr("Face")},
                          {"tex.alignEdge", tr("To Edge")}, {"tex.fit", tr("Fit")},
                          {"tex.reset", tr("Reset")}, {"view.uvEditor", tr("UV")}}},
          S{tr("Justify"), {{"tex.justifyL", tr("Left")}, {"tex.justifyR", tr("Right")},
                            {"tex.justifyT", tr("Top")}, {"tex.justifyB", tr("Bottom")},
                            {"tex.justifyC", tr("Center")}}},
          S{tr("Transform"), {{"tex.rotCCW", tr("−90°")}, {"tex.rotCW", tr("+90°")},
                              {"tex.flipU", tr("Flip U")}, {"tex.flipV", tr("Flip V")}}},
      };
      break;
    default:
      sections = {
          S{tr("Create"), {{"brush.create", tr("Brush")}, {"brush.stairs", tr("Stairs")},
                           {"brush.arch", tr("Arch")}, {"brush.cylinder", tr("Cylinder")},
                           {"brush.capsule", tr("Capsule")}, {"brush.pyramid", tr("Pyramid")},
                           {"tool.layoutDesigner", tr("Layout")},
                           {"brush.mirrorMode", tr("Mirror")}, {"brush.mirrorSelection", tr("Mirror Sel.")}}},
          S{tr("Modifiers"), {{"mod.addArray", tr("Array")}, {"mod.addCurve", tr("Curve")},
                              {"mod.editSource", tr("Edit Src")}, {"path.pen", tr("Path")},
                              {"add.pathAnchor", tr("Anchor")}, {"path.brushesBetween", tr("Connect")}}},
          S{tr("CSG"), {{"csg.subtract", tr("Carve")}, {"csg.hollow", tr("Hollow")},
                        {"csg.merge", tr("Merge")}, {"csg.intersect", tr("Inter-\nsect")},
                        {"brush.cut", tr("Cut")}, {"brush.bevel", tr("Bevel")}}},
          S{tr("Logic"), {{"io.editor", tr("I/O")}}},
          S{tr("Group"), {{"entity.makeBrush", tr("Brush Ent")}, {"entity.toWorld", tr("To World")},
                          {"group.new", tr("New Group")}, {"group.toWorldspawn", tr("To World-\nspawn")}}},
          S{tr("Transform"), {{"xform.rotYcw", tr("Rotate")}, {"xform.mirrorX", tr("Mirror X")},
                              {"xform.mirrorZ", tr("Mirror Z")}, {"xform.snap", tr("Snap")},
                              {"xform.shear", tr("Shear")}}},
          S{tr("Edit"), {{"edit.duplicate", tr("Clone")}, {"edit.delete", tr("Delete")}}},
      };
      break;
  }
  m_toolShelf->setSections(sections);
}

void MainWindow::onEditorModeChanged(g::EditorMode mode) {
  ActionManager::instance().refresh();
  rebuildToolShelf();
  switch (mode) {
    case g::EditorMode::MATERIAL:
      m_dockMaterials->show();
      m_dockMaterials->raise();
      break;
    default:
      break;
  }
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (!confirmDiscardChanges()) {
    event->ignore();
    return;
  }
  writeWorkspace();
  saveWindowLayout();
  config()->sync();
  ui::LayoutDesigner::closeIfOpen();
  ui::IOEditor::closeIfOpen();
  ui::TextureBrowserWindow::closeIfOpen();
  ui::TexturePacksWindow::closeIfOpen();
  net::TextureEditSessions::instance().stopAll();  // releases the server edit locks
  event->accept();
}

void MainWindow::startAutosave() {
  m_autosaveTimer = new QTimer(this);
  connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::runAutosave);
  prefs::get().watch(QStringLiteral("General/AutosaveMinutes"), this, [this](const QVariant &v) {
    const int minutes = v.toInt();
    if (minutes <= 0)
      m_autosaveTimer->stop();
    else
      m_autosaveTimer->start(minutes * 60 * 1000);
  });
}

void MainWindow::runAutosave() {
  // Every unsaved level, shown or not; never the level's own file.
  if (!g::engine3D)
    return;
  const QString dir = ui::autosaveFolder();
  QDir().mkpath(dir);
  int saved = 0;
  for (proj::Document *d: m_docs->dirtyLevels()) {
    const QString base = !d->path.isEmpty() ? QFileInfo(d->path).completeBaseName()
                       : d->untitled > 1    ? QStringLiteral("untitled_%1").arg(d->untitled)
                                            : QStringLiteral("untitled");
    const QString path = QStringLiteral("%1/%2-%3.%4").arg(dir, base, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")),
                                                QLatin1String(fmap::kFileSuffix));
    fmap::Document doc;
    if (!m_docs->withLevel(d, [&doc] { doc = g::engine3D->captureMapDocument(); }))
      continue;
    doc.meta.mapName = base;
    QString error;
    if (!fmap::saveMap(path, doc, &error)) {
      qWarning().noquote() << QStringLiteral("[autosave] failed: %1").arg(error);
      continue;
    }
    // Keep the newest N for this map.
    QDir d2(dir);
    QFileInfoList files = d2.entryInfoList({base + QStringLiteral("-*.") + QLatin1String(fmap::kFileSuffix)},
                                           QDir::Files, QDir::Time);
    const int keep = prefs::i("General/AutosaveBackups");
    for (int i2 = keep; i2 < files.size(); ++i2)
      QFile::remove(files[i2].absoluteFilePath());
    qInfo().noquote() << QStringLiteral("[autosave] %1").arg(path);
    ++saved;
  }
  if (saved)
    showToast(tr("Autosaved"));
}
