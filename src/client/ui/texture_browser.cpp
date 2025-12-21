#include "texture_browser.h"
#include "client/ui/scale.h"

#include <QApplication>
#include <cmath>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "client/engine/scene.h"
#include "client/engine/sky_info.h"
#include "client/net/asset_client.h"
#include "client/net/file_fetcher.h"
#include "client/net/texture_catalog.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/dialog_style.h"
#include "client/ui/flow_layout.h"
#include "client/ui/texture_actions.h"
#include "client/ui/material_preview_3d.h"
#include "client/ui/material_props_panel.h"
#include "client/ui/tag_picker.h"
#include "client/ui/texture_ui.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"
#include "shared/lib/config.h"

namespace ui {

namespace {

QPointer<TextureBrowserWindow> s_window;

const char *kExtraStyle = R"(
QWidget#tbSidebar, QWidget#tbDetails { background: #0f1326; }
QWidget#tbCenter { background: #101428; }
QLineEdit#tbSearch { padding: 6px 10px; border-radius: 6px; border: 1px solid #2a3360; background: #0c1022; }
QLineEdit#tbSearch:focus { border-color: #4866C5; }
QLabel#tbCount { color: #8f9bbd; }
QLabel#tbPreview { background: #0c1022; border: 1px solid #232A4E; border-radius: 8px; }
QToolButton#tbPage { background: transparent; border: 1px solid #2a3360; border-radius: 5px; padding: 2px 8px; min-width: 18px; }
QToolButton#tbPage:hover { background: #171C38; border-color: #4866C5; }
QToolButton#tbPage:checked { background: #4866C5; border-color: #4866C5; color: white; }
QToolButton#tbPage:disabled { color: #4a5272; border-color: #1f2747; }
QToolButton#tbPill { background: #2f6fb4; color: white; border-radius: 10px; padding: 2px 10px; border: none; }
QToolButton#tbPill:hover { background: #3a82cf; }
QToolButton#tbSeg { background: #0f1328; border: 1px solid #2a3360; border-left: none; padding: 3px 9px; }
QToolButton#tbSeg[pos="first"] { border-left: 1px solid #2a3360; border-top-left-radius: 5px;
  border-bottom-left-radius: 5px; }
QToolButton#tbSeg[pos="last"] { border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
QToolButton#tbSeg:hover { background: #171C38; }
QToolButton#tbSeg:checked { background: #4866C5; border-color: #6A84D9; color: white; }
QLabel#tbLock { background: #3a2a08; color: #ffc864; border: 1px solid #6b4e14; border-radius: 6px; padding: 4px 8px; }
)";

// "cc0" -> "CC0", "rf" -> "Royalty free (RF)"
QString licenseLabel(const QString &key) {
  if (key == QLatin1String("rf"))
    return QObject::tr("Royalty free (RF)");
  if (key == QLatin1String("cc0"))
    return QStringLiteral("CC0");
  if (key == QLatin1String("fab"))
    return QStringLiteral("Fab");
  if (key == QLatin1String("unknown"))
    return QObject::tr("Unknown");
  return key.toUpper();
}

} // namespace

TextureBrowserWindow *TextureBrowserWindow::open(vk::Engine3D *engine, QWidget *owner, const QString &revealRef) {
  if (!s_window)
    s_window = new TextureBrowserWindow(engine, owner);
  else if (!s_window->isVisible())
    s_window->clearFilters(revealRef.isEmpty()); // every opening starts unfiltered
  if (s_window->isMinimized())
    s_window->showNormal();
  s_window->show();
  s_window->raise();
  s_window->activateWindow();
  if (!revealRef.isEmpty())
    s_window->reveal(revealRef);
  s_window->m_search->setFocus();
  return s_window;
}

TextureBrowserWindow *TextureBrowserWindow::openSkyboxes(vk::Engine3D *engine, QWidget *owner) {
  TextureBrowserWindow *w = open(engine, owner);
  w->m_libraryList->setCurrentRow(int(Library::Sky));
  return w;
}

void TextureBrowserWindow::closeIfOpen() {
  if (s_window)
    s_window->close();
}

TextureBrowserWindow::TextureBrowserWindow(vk::Engine3D *engine, QWidget *owner)
    : QMainWindow(nullptr, Qt::Window), m_engine(engine) {
  setObjectName(QStringLiteral("TextureBrowser"));
  setWindowTitle(tr("Texture Browser"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setAttribute(Qt::WA_QuitOnClose, false);
  if (owner)
    connect(owner, &QObject::destroyed, this, &QObject::deleteLater);
  scale::setScaledStyleSheet(this, dialogStyle() + QString::fromLatin1(kExtraStyle));
  resize(scale::initialSize(this, {1580, 960}));

  m_model = new ServerTextureModel(ServerTextureModel::Mode::Paged, this);
  m_model->setPageSize(std::clamp(prefs::i("Textures/BrowserPageSize"), 30, 240));
  m_delegate = new TextureCardDelegate(this);

  // Debounce timers first: the widgets below connect to them.
  m_searchDebounce = new QTimer(this);
  m_searchDebounce->setSingleShot(true);
  m_searchDebounce->setInterval(250);
  connect(m_searchDebounce, &QTimer::timeout, this, &TextureBrowserWindow::applyQuery);
  m_seed = QRandomGenerator::global()->generate();

  m_split = new QSplitter(Qt::Horizontal, this);
  m_split->addWidget(buildSidebar());
  m_split->addWidget(buildCenter());
  m_split->addWidget(buildDetails());
  m_split->setStretchFactor(0, 0);
  m_split->setStretchFactor(1, 1);
  m_split->setStretchFactor(2, 0);
  m_split->setSizes({scale::px(300), scale::px(960), scale::px(360)});
  m_split->setChildrenCollapsible(false);
  setCentralWidget(m_split);
  statusBar()->setSizeGripEnabled(true);

  connect(m_model, &ServerTextureModel::resultChanged, this, [this] {
    const QLocale loc;
    m_resultCount->setText(m_model->total() == 1 ? tr("1 texture") : tr("%1 textures").arg(loc.toString(m_model->total())));
    m_gridMessage->setVisible(m_model->total() == 0);
    m_gridMessage->setText(tr("No textures match"));
    rebuildPager();
    m_tagPicker->setResult(m_model->facets(), m_model->distinctTags(), m_model->query());
    rebuildLicenses();
    if (!m_revealPending.isEmpty()) {
      const int row = m_model->rowOf(m_revealPending);
      if (row >= 0) {
        m_grid->setCurrentIndex(m_model->index(row, 0));
        m_grid->scrollTo(m_model->index(row, 0), QAbstractItemView::PositionAtCenter);
        showDetails(m_revealPending);
      }
      m_revealPending.clear();
    }
  });
  connect(m_model, &ServerTextureModel::loadingChanged, this, [this](bool loading) {
    statusBar()->showMessage(loading ? tr("Loading…") : QString());
  });
  connect(m_model, &ServerTextureModel::failed, this, [this](const QString &msg) {
    m_gridMessage->setText(tr("Could not load textures.\n%1").arg(msg));
    m_gridMessage->show();
  });
  connect(m_model, &ServerTextureModel::outdated, this, [this](int n) {
    m_refreshPill->setVisible(n > 0);
    m_refreshPill->setText(tr("%n change(s) on the server · Refresh", nullptr, n));
  });

  auto &cat = net::TextureCatalog::instance();
  connect(&cat, &net::TextureCatalog::packsChanged, this, &TextureBrowserWindow::rebuildPacks);
  connect(&cat, &net::TextureCatalog::entryChanged, this, [this](const QString &ref) {
    if (refKey(ref) == refKey(m_detailsRef))
      refreshDetails();
  });
  connect(&cat, &net::TextureCatalog::entryRenamed, this, [this](const QString &oldRef, const QString &newRef) {
    if (refKey(oldRef) == refKey(m_detailsRef))
      showDetails(newRef);
  });
  connect(&cat, &net::TextureCatalog::lockChanged, this, [this](const QString &ref) {
    if (refKey(MaterialRef::parse(ref).base()) == refKey(m_detailsRef))
      refreshDetails();
  });
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, [this](net::AssetClient::State s) {
    if (s == net::AssetClient::State::Online) {
      m_tagPicker->refreshPopular();
      m_model->reload();
    } else if (s == net::AssetClient::State::Offline) {
      m_gridMessage->setText(tr("Asset server offline\n%1").arg(net::AssetClient::instance().offlineDetail()));
      m_gridMessage->show();
    }
  });
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, [this](const QString &key) {
    if (m_detailsRef.isEmpty())
      return;
    const TextureEntry *e = net::TextureCatalog::instance().find(m_detailsRef);
    if (!e)
      return;
    const MapFile *c = e->map(MapType::Color);
    if (c && (key == ThumbCache::editorKey(c->editor) || key == ThumbCache::serverKey(c->thumb)))
      refreshDetails();
  });
  if (m_engine) {
    connect(m_engine, &vk::Engine3D::currentMaterialChanged, this, [this](const QString &ref) {
      m_delegate->setCurrent(ref);
      m_grid->viewport()->update();
    });
    connect(m_engine, &vk::Engine3D::materialOverridesChanged, this, &TextureBrowserWindow::refreshDetails);
    connect(m_engine, &vk::Engine3D::fillChanged, this, &TextureBrowserWindow::syncTintButton);
  }

  // V (selection tint) works in this window too.
  if (QAction *tint = ui::act(QStringLiteral("view.selectionTint")))
    addAction(tint);
  auto *find = new QShortcut(QKeySequence::Find, this);
  connect(find, &QShortcut::activated, this, [this] {
    m_search->setFocus();
    m_search->selectAll();
  });
  m_grid->installEventFilter(this);

  const QByteArray geo = config()->getRaw(QStringLiteral("TextureBrowser/Geometry")).toByteArray();
  if (!geo.isEmpty())
    restoreGeometry(QByteArray::fromBase64(geo));
  const QByteArray split = config()->getRaw(QStringLiteral("TextureBrowser/Splitter")).toByteArray();
  if (!split.isEmpty())
    m_split->restoreState(QByteArray::fromBase64(split));

  rebuildPacks();
  updatePopularFilter();
  rebuildFilterChips();
  applyQuery();
  showDetails({});
  syncTintButton();
}

void TextureBrowserWindow::syncTintButton() {
  if (!m_tintButton || !m_engine)
    return;
  QString key;
  if (QAction *a = ui::act(QStringLiteral("view.selectionTint")))
    key = a->shortcut().toString(QKeySequence::NativeText);
  const bool hidden = m_engine->maskHidden();
  m_tintButton->setChecked(hidden);
  m_tintButton->setText(key.isEmpty() ? tr("Toggle Selection") : tr("Toggle Selection (%1)").arg(key));
}

QWidget *TextureBrowserWindow::buildSidebar() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scale::bind(scroll, [=, this] { scroll->setMinimumWidth(scale::tpx(240)); });
  auto *w = new QWidget(scroll);
  w->setObjectName(QStringLiteral("tbSidebar"));
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(14, 14, 10, 14);
  v->setSpacing(8);

  v->addWidget(sectionLabel(tr("Library"), w));
  m_libraryList = sidebarList(w);
  for (const auto &[label, icon]: {std::pair{tr("All textures"), "texture"}, std::pair{tr("Used in this map"), "material"},
                                   std::pair{tr("Recently used"), "sorttime"}, std::pair{tr("Skyboxes"), "world"}})
    m_libraryList->addItem(new QListWidgetItem(ActionManager::icon(QString::fromLatin1(icon)), label));
  m_libraryList->setCurrentRow(0);
  fitListHeight(m_libraryList, 4);
  connect(m_libraryList, &QListWidget::currentRowChanged, this, [this](int row) {
    m_library = Library(std::max(0, row));
    applyQuery();
    updatePopularFilter();
  });
  v->addWidget(m_libraryList);
  m_alphaOnly = new QCheckBox(tr("Only transparent textures"), w);
  m_alphaOnly->setToolTip(tr("Textures with an alpha channel or an opacity map (leaves, fences, decals …)"));
  connect(m_alphaOnly, &QCheckBox::toggled, this, [this] {
    applyQuery();
    updatePopularFilter();
  });
  v->addWidget(m_alphaOnly);
  m_freeOnly = new QCheckBox(tr("Only free textures"), w);
  m_freeOnly->setToolTip(tr("Textures with a free license: CC0 / public domain, royalty free (RF), …"));
  connect(m_freeOnly, &QCheckBox::toggled, this, &TextureBrowserWindow::applyQuery);
  v->addWidget(m_freeOnly);

  v->addSpacing(6);
  v->addWidget(sectionLabel(tr("License"), w));
  m_licenseList = sidebarList(w);
  m_licenseList->setItemDelegate(new CountListDelegate(m_licenseList));
  m_licenseList->setSelectionMode(QAbstractItemView::NoSelection);
  connect(m_licenseList, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
    const QString key = it->data(Qt::UserRole).toString();
    if (m_licenses.contains(key))
      m_licenses.removeAll(key);
    else
      m_licenses.append(key);
    applyQuery();
  });
  v->addWidget(m_licenseList);

  v->addSpacing(6);
  m_tagPicker = new TagPicker(w);
  connect(m_tagPicker, &TagPicker::changed, this, &TextureBrowserWindow::applyQuery);
  v->addWidget(m_tagPicker);
  v->addSpacing(6);
  v->addWidget(sectionLabel(tr("Packs"), w));
  m_packList = sidebarList(w);
  auto *packDelegate = new CountListDelegate(m_packList);
  m_packList->setItemDelegate(packDelegate);
  scale::bind(m_packList, [this] { m_packList->doItemsLayout(); });
  connect(m_packList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
    const QString pack = it ? it->data(Qt::UserRole).toString() : QString();
    if (pack == m_pack)
      return;
    m_pack = pack;
    applyQuery();
  });
  v->addWidget(m_packList);

  scroll->setWidget(w);
  return scroll;
}

QWidget *TextureBrowserWindow::buildCenter() {
  auto *w = new QWidget(this);
  w->setObjectName(QStringLiteral("tbCenter"));
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(16, 14, 16, 10);
  v->setSpacing(10);

  auto *top = new QHBoxLayout();
  top->setSpacing(8);
  m_search = new QLineEdit(w);
  m_search->setObjectName(QStringLiteral("tbSearch"));
  m_search->setPlaceholderText(tr("Search names and tags…"));
  m_search->setClearButtonEnabled(true);
  m_search->addAction(ActionManager::icon(QStringLiteral("viewzoom")), QLineEdit::LeadingPosition);
  connect(m_search, &QLineEdit::textChanged, m_searchDebounce, qOverload<>(&QTimer::start));
  top->addWidget(m_search, 1);
  m_sort = new QComboBox(w);
  m_sort->addItem(tr("Shuffled"), QStringLiteral("random"));
  m_sort->addItem(tr("Name"), QStringLiteral("name"));
  m_sort->addItem(tr("Recently added"), QStringLiteral("recent"));
  m_sort->addItem(tr("Pack"), QStringLiteral("pack"));
  m_sort->setToolTip(tr("Sort order"));
  m_sort->setCurrentIndex(prefs::b("Textures/RandomOrder") ? 0 : 1);
  connect(m_sort, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (index <= 1)
      prefs::get().setValue(QStringLiteral("Textures/RandomOrder"), index == 0);
    applyQuery();
  });
  top->addWidget(m_sort);
  auto *shuffle = new QToolButton(w);
  shuffle->setObjectName(QStringLiteral("dlgTool"));
  shuffle->setIcon(ActionManager::icon(QStringLiteral("file_refresh")));
  shuffle->setText(tr("Shuffle"));
  shuffle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  shuffle->setToolTip(tr("A new random order of the same results (filters and search stay)"));
  connect(shuffle, &QToolButton::clicked, this, [this] {
    m_seed = QRandomGenerator::global()->generate();
    if (m_sort->currentData().toString() != QLatin1String("random"))
      m_sort->setCurrentIndex(0);  // applies the query
    else
      applyQuery();
  });
  top->addWidget(shuffle);
  auto *sizeIcon = new QLabel(w);
  sizeIcon->setPixmap(ActionManager::icon(QStringLiteral("image_data")).pixmap(16, 16));
  top->addWidget(sizeIcon);
  m_thumbSize = new QSlider(Qt::Horizontal, w);
  m_thumbSize->setRange(80, 256);
  scale::bind(m_thumbSize, [=, this] { m_thumbSize->setFixedWidth(scale::tpx(120)); });
  m_thumbSize->setToolTip(tr("Card size"));
  m_thumbSize->setValue(std::clamp(prefs::i("Textures/BrowserThumbSize"), 80, 256));
  m_delegate->setThumbSize(m_thumbSize->value());
  connect(m_thumbSize, &QSlider::valueChanged, this, [this](int px) {
    m_delegate->setThumbSize(px);
    m_grid->doItemsLayout();
    prefs::get().setValue(QStringLiteral("Textures/BrowserThumbSize"), px);
  });
  top->addWidget(m_thumbSize);
  v->addLayout(top);

  // active filters + result count + refresh pill
  auto *filterRow = new QHBoxLayout();
  filterRow->setSpacing(8);
  m_chipBox = new QWidget(w);
  m_chipFlow = new FlowLayout(m_chipBox, 6, 6);
  filterRow->addWidget(m_chipBox, 1);
  m_refreshPill = new QToolButton(w);
  m_refreshPill->setObjectName(QStringLiteral("tbPill"));
  m_refreshPill->hide();
  connect(m_refreshPill, &QToolButton::clicked, this, [this] {
    m_model->reload();
    rebuildPacks();
  });
  filterRow->addWidget(m_refreshPill, 0, Qt::AlignTop);
  m_resultCount = new QLabel(w);
  m_resultCount->setObjectName(QStringLiteral("tbCount"));
  filterRow->addWidget(m_resultCount, 0, Qt::AlignTop);
  v->addLayout(filterRow);

  auto *gridHolder = new QWidget(w);
  auto *gridStack = new QVBoxLayout(gridHolder);
  gridStack->setContentsMargins(0, 0, 0, 0);
  m_grid = makeTextureGrid(m_model, m_delegate, gridHolder, true, 6);
  m_grid->setFrameShape(QFrame::NoFrame);
  m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
  m_grid->setStyleSheet(QStringLiteral("QListView { background: #101428; }"));
  connect(m_grid->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](const QModelIndex &cur) { showDetails(cur.data(RefRole).toString()); });
  connect(m_grid, &QListView::doubleClicked, this, [this](const QModelIndex &idx) {
    if (const QString msg = useTexture(m_engine, idx.data(RefRole).toString()); !msg.isEmpty())
      statusBar()->showMessage(msg, 3000);
  });
  connect(m_grid, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
    const QModelIndex idx = m_grid->indexAt(pos);
    if (!idx.isValid())
      return;
    QMenu menu(this);
    populateTextureMenu(menu, m_engine, idx.data(RefRole).toString(), this, true);
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
  });
  gridStack->addWidget(m_grid);
  m_gridMessage = new QLabel(m_grid);
  m_gridMessage->setAlignment(Qt::AlignCenter);
  m_gridMessage->setObjectName(QStringLiteral("dlgDesc"));
  scale::setRole(m_gridMessage, scale::Font::Heading);
  m_gridMessage->hide();
  m_grid->viewport()->installEventFilter(this);
  v->addWidget(gridHolder, 1);

  // pager
  auto *pagerRow = new QHBoxLayout();
  pagerRow->setSpacing(4);
  m_pagerLayout = new QHBoxLayout();
  m_pagerLayout->setSpacing(4);
  pagerRow->addStretch(1);
  pagerRow->addLayout(m_pagerLayout);
  pagerRow->addStretch(1);
  auto *perPage = new QLabel(tr("Per page"), w);
  perPage->setObjectName(QStringLiteral("dlgDesc"));
  pagerRow->addWidget(perPage);
  m_pageSize = new QComboBox(w);
  for (int n: {60, 120, 240})
    m_pageSize->addItem(QString::number(n), n);
  m_pageSize->setCurrentIndex(std::max(0, m_pageSize->findData(m_model->pageSize())));
  connect(m_pageSize, &QComboBox::currentIndexChanged, this, [this] {
    const int n = m_pageSize->currentData().toInt();
    prefs::get().setValue(QStringLiteral("Textures/BrowserPageSize"), n);
    m_model->setPageSize(n);
  });
  pagerRow->addWidget(m_pageSize);
  v->addLayout(pagerRow);
  return w;
}

QWidget *TextureBrowserWindow::buildDetails() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Always on: the preview's height follows the pane width, which a toggling scroll bar would change.
  scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  scale::bind(scroll, [=, this] { scroll->setMinimumWidth(scale::tpx(320)); });
  auto *outer = new QWidget(scroll);
  outer->setObjectName(QStringLiteral("tbDetails"));
  auto *ov = new QVBoxLayout(outer);
  ov->setContentsMargins(14, 14, 14, 14);

  m_detailsEmpty = new QLabel(tr("No texture selected"), outer);
  m_detailsEmpty->setObjectName(QStringLiteral("dlgDesc"));
  m_detailsEmpty->setWordWrap(true);
  m_detailsEmpty->setAlignment(Qt::AlignCenter);
  ov->addWidget(m_detailsEmpty);

  m_detailsBody = new QWidget(outer);
  m_detailsBody->installEventFilter(this);  // width changes resize the preview
  auto *v = new QVBoxLayout(m_detailsBody);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(10);
  m_previewStack = new QStackedWidget(m_detailsBody);
  m_previewStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_preview = new QLabel(m_previewStack);
  m_preview->setObjectName(QStringLiteral("tbPreview"));
  m_preview->setAlignment(Qt::AlignCenter);
  m_previewStack->addWidget(m_preview);
  auto *page3d = new QWidget(m_previewStack);
  auto *page3dLayout = new QHBoxLayout(page3d);
  page3dLayout->setContentsMargins(0, 0, 0, 0);
  m_preview3d = new MaterialPreview3D(page3d);
  page3dLayout->addWidget(m_preview3d, 0, Qt::AlignCenter);
  m_previewStack->addWidget(page3d);
  v->addWidget(m_previewStack);
  m_previewSwitch = buildPreviewSwitch();
  v->addWidget(m_previewSwitch);
  m_detailsTitle = new QLabel(m_detailsBody);
  m_detailsTitle->setObjectName(QStringLiteral("dlgTitle"));
  m_detailsTitle->setWordWrap(true);
  v->addWidget(m_detailsTitle);
  m_detailsRef_ = new QLabel(m_detailsBody);
  m_detailsRef_->setObjectName(QStringLiteral("dlgSubtitle"));
  m_detailsRef_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_detailsRef_->setWordWrap(true);
  m_detailsRef_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_detailsTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  v->addWidget(m_detailsRef_);
  m_detailsLock = new QLabel(m_detailsBody);
  m_detailsLock->setObjectName(QStringLiteral("tbLock"));
  m_detailsLock->setWordWrap(true);
  v->addWidget(m_detailsLock);

  // actions
  auto *actions = new QGridLayout();
  actions->setSpacing(6);
  m_applyButton = new QPushButton(tr("Apply"), m_detailsBody);
  m_applyButton->setToolTip(tr("Put this texture on the selected faces (or double-click the card)"));
  m_applyButton->setObjectName(QStringLiteral("dlgPrimary"));
  m_applyButton->setDefault(true);
  connect(m_applyButton, &QPushButton::clicked, this, [this] {
    if (const QString msg = useTexture(m_engine, m_detailsRef); !msg.isEmpty())
      statusBar()->showMessage(msg, 3000);
  });
  m_currentButton = new QPushButton(tr("Set current"), m_detailsBody);
  m_currentButton->setObjectName(QStringLiteral("dlgFlat"));
  m_currentButton->setToolTip(tr("New brushes and the paint tool use the current material"));
  connect(m_currentButton, &QPushButton::clicked, this, [this] {
    if (m_engine && !m_detailsRef.isEmpty())
      m_engine->setCurrentMaterial(m_detailsRef);
  });
  m_copyButton = new QPushButton(tr("Copy to pack…"), m_detailsBody);
  m_copyButton->setObjectName(QStringLiteral("dlgFlat"));
  connect(m_copyButton, &QPushButton::clicked, this, [this] { emit TextureHub::instance().copyRequested(m_detailsRef); });
  m_editButton = new QPushButton(tr("Edit maps…"), m_detailsBody);
  m_editButton->setObjectName(QStringLiteral("dlgFlat"));
  connect(m_editButton, &QPushButton::clicked, this, [this] { emit TextureHub::instance().openPacksRequested(m_detailsRef); });
  m_tintButton = new QPushButton(m_detailsBody);
  m_tintButton->setObjectName(QStringLiteral("dlgFlat"));
  m_tintButton->setCheckable(true);
  m_tintButton->setToolTip(tr("Hide the red tint on the selected faces to see the texture as it will look. "
                              "Closing this window shows the tint again."));
  connect(m_tintButton, &QPushButton::clicked, this, [this] {
    if (QAction *a = ui::act(QStringLiteral("view.selectionTint")))
      a->trigger();
  });
  actions->addWidget(m_applyButton, 0, 0, 1, 2);
  actions->addWidget(m_tintButton, 1, 0, 1, 2);
  actions->addWidget(m_currentButton, 2, 0, 1, 2);
  actions->addWidget(m_copyButton, 3, 0);
  actions->addWidget(m_editButton, 3, 1);
  v->addLayout(actions);

  v->addWidget(sectionLabel(tr("Maps"), m_detailsBody));
  m_mapChips = new QWidget(m_detailsBody);
  m_mapFlow = new FlowLayout(m_mapChips, 5, 5);
  v->addWidget(m_mapChips);

  v->addWidget(sectionLabel(tr("Details"), m_detailsBody));
  m_detailsInfo = new QLabel(m_detailsBody);
  m_detailsInfo->setWordWrap(true);
  m_detailsInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_detailsInfo->setTextFormat(Qt::RichText);
  m_detailsInfo->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
  m_detailsInfo->setOpenExternalLinks(true);  // a skybox's source page
  v->addWidget(m_detailsInfo);

  v->addWidget(sectionLabel(tr("Tags"), m_detailsBody));
  m_detailTags = new QWidget(m_detailsBody);
  m_detailTagFlow = new FlowLayout(m_detailTags, 5, 5);
  v->addWidget(m_detailTags);

  v->addWidget(sectionLabel(tr("Texture properties"), m_detailsBody));
  m_detailsProps = new MaterialPropsPanel(m_engine, MaterialPropsPanel::Mode::Pinned, m_detailsBody);
  m_detailsProps->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  v->addWidget(m_detailsProps);
  v->addStretch(1);
  ov->addWidget(m_detailsBody);
  ov->addStretch(1);
  scroll->setWidget(outer);
  return scroll;
}

// The switches under the preview; both choices are kept in the config.
QWidget *TextureBrowserWindow::buildPreviewSwitch() {
  auto *box = new QWidget(m_detailsBody);
  auto *flow = new FlowLayout(box, 12, 6);
  auto segmented = [this](QWidget *parent, const QList<QPair<QString, QString>> &items, QButtonGroup *group) {
    auto *row = new QHBoxLayout(parent);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    for (int i = 0; i < items.size(); ++i) {
      auto *b = new QToolButton(parent);
      b->setObjectName(QStringLiteral("tbSeg"));
      b->setProperty("pos", i == 0 ? "first" : i == items.size() - 1 ? "last" : "middle");
      b->setText(items[i].first);
      b->setToolTip(items[i].second);
      b->setCheckable(true);
      group->addButton(b, i);
      row->addWidget(b);
    }
  };

  auto *shapes = new QWidget(box);
  m_previewModes = new QButtonGroup(this);
  segmented(shapes,
            {{tr("2D"), tr("The flat colour map")},
             {tr("Cube"), tr("3D preview on a cube")},
             {tr("Sphere"), tr("3D preview on a sphere")},
             {tr("Plane"), tr("3D preview on a flat square")}},
            m_previewModes);
  flow->addWidget(shapes);

  m_previewLights = new QWidget(box);
  auto *lights = new QButtonGroup(this);
  segmented(m_previewLights,
            {{tr("Sun"), tr("Outdoor: a directional sun with sky light")},
             {tr("Omni"), tr("Indoor: one omni light near the object")}},
            lights);
  flow->addWidget(m_previewLights);

  m_previewTiling = new QWidget(box);
  auto *tiling = new QButtonGroup(this);
  segmented(m_previewTiling,
            {{tr("Single"), tr("The texture once")},
             {tr("Repeated"), tr("The texture 2 × 2: seams show whether it tiles")}},
            tiling);
  flow->addWidget(m_previewTiling);
  m_previewRepeated = config()->getRaw(QStringLiteral("TextureBrowser/PreviewRepeated")).toBool();
  tiling->button(m_previewRepeated ? 1 : 0)->setChecked(true);
  connect(tiling, &QButtonGroup::idClicked, this, [this](int id) {
    m_previewRepeated = id == 1;
    config()->setRaw(QStringLiteral("TextureBrowser/PreviewRepeated"), m_previewRepeated);
    layoutPreview();
  });

  const bool omni = config()->getRaw(QStringLiteral("TextureBrowser/PreviewLight")).toString() == QLatin1String("omni");
  lights->button(omni ? 1 : 0)->setChecked(true);
  m_preview3d->setLighting(omni ? MaterialPreview3D::Lighting::Omni : MaterialPreview3D::Lighting::Sun);
  connect(lights, &QButtonGroup::idClicked, this, [this](int id) {
    m_preview3d->setLighting(id == 1 ? MaterialPreview3D::Lighting::Omni : MaterialPreview3D::Lighting::Sun);
    config()->setRaw(QStringLiteral("TextureBrowser/PreviewLight"), id == 1 ? "omni" : "sun");
  });

  // The sphere until another view is saved.
  const QString saved =
      config()->getRaw(QStringLiteral("TextureBrowser/PreviewMode"), QStringLiteral("sphere")).toString();
  setPreviewMode(saved == QLatin1String("2d")       ? PreviewMode::Flat
                 : saved == QLatin1String("cube")   ? PreviewMode::Cube
                 : saved == QLatin1String("plane")  ? PreviewMode::Plane
                                                    : PreviewMode::Sphere);
  connect(m_previewModes, &QButtonGroup::idClicked, this, [this](int id) {
    setPreviewMode(PreviewMode(id));
    config()->setRaw(QStringLiteral("TextureBrowser/PreviewMode"),
                     id == 1 ? "cube" : id == 2 ? "sphere" : id == 3 ? "plane" : "2d");
  });
  return box;
}

void TextureBrowserWindow::setPreviewMode(PreviewMode mode) {
  m_previewMode = mode;
  m_previewModes->button(int(mode))->setChecked(true);
  const bool is3d = mode != PreviewMode::Flat;
  m_previewLights->setVisible(is3d);
  if (m_previewTiling)
    m_previewTiling->setVisible(!is3d);
  if (is3d)
    m_preview3d->setShape(mode == PreviewMode::Sphere  ? MaterialPreview3D::Shape::Sphere
                          : mode == PreviewMode::Plane ? MaterialPreview3D::Shape::Plane
                                                       : MaterialPreview3D::Shape::Cube);
  m_previewStack->setCurrentIndex(is3d ? 1 : 0);
  if (!m_detailsRef.isEmpty())
    refreshDetails();  // the preview's height, and the texture for the 3D view
}

QStringList TextureBrowserWindow::mapRefs() const {
  QStringList out;
  if (!m_engine)
    return out;
  QSet<QString> seen;
  for (const QString &name: m_engine->materialsInUse()) {
    const auto m = vk::TextureManager::instance().find(name);
    if (!m || m->kind != eng::Material::Kind::Server)
      continue;
    const QString base = MaterialRef::parse(m->ref).base();
    if (!seen.contains(refKey(base))) {
      seen.insert(refKey(base));
      out.append(base);
    }
  }
  return out;
}

QStringList TextureBrowserWindow::recentRefs() { return recentTextures(); }

void TextureBrowserWindow::applyQuery() {
  QJsonObject args;
  // A typed ref ("pack/Name") searches its name in its pack, overriding the pack list.
  const TextureSearch search = parseTextureSearch(m_search ? m_search->text() : QString());
  args.insert(QStringLiteral("q"), search.text);
  if (const QString pack = !search.pack.isEmpty() ? search.pack : m_pack; !pack.isEmpty())
    args.insert(QStringLiteral("pack"), pack);
  if (!m_tagPicker->tags().isEmpty())
    args.insert(QStringLiteral("tags"), QJsonArray::fromStringList(m_tagPicker->tags()));
  if (!m_tagPicker->notTags().isEmpty())
    args.insert(QStringLiteral("not_tags"), QJsonArray::fromStringList(m_tagPicker->notTags()));
  if (m_sort) {
    args.insert(QStringLiteral("sort"), m_sort->currentData().toString());
    args.insert(QStringLiteral("seed"), double(m_seed));
  }
  if (m_alphaOnly && m_alphaOnly->isChecked())
    args.insert(QStringLiteral("alpha"), true);
  if (m_freeOnly && m_freeOnly->isChecked())
    args.insert(QStringLiteral("free"), true);
  if (!m_licenses.isEmpty())
    args.insert(QStringLiteral("licenses"), QJsonArray::fromStringList(m_licenses));
  args.insert(QStringLiteral("sky"), m_library == Library::Sky);
  args.insert(QStringLiteral("facets"), true);
  args.insert(QStringLiteral("facet_limit"), 200);
  if (m_library == Library::Map || m_library == Library::Recent) {
    QStringList refs = m_library == Library::Map ? mapRefs() : recentRefs();
    if (refs.isEmpty())
      refs = {QStringLiteral("-/-")};  // an empty result, not "everything"
    args.insert(QStringLiteral("refs"), QJsonArray::fromStringList(refs));
  }
  m_model->setQuery(args);
  if (m_grid)
    m_grid->scrollToTop();
  rebuildFilterChips();
  // used-in-map dots
  QSet<QString> used;
  for (const auto &r: mapRefs())
    used.insert(refKey(r));
  m_model->setUsedRefs(used);
}

void TextureBrowserWindow::rebuildFilterChips() {
  if (!m_chipFlow)
    return;
  m_chipFlow->clear();
  auto addChip = [this](const QString &label, std::function<void()> onRemove) {
    addFilterChip(m_chipFlow, m_chipBox, label, false, std::move(onRemove));
  };
  if (m_library == Library::Map)
    addChip(tr("in this map"), [this] { m_libraryList->setCurrentRow(0); });
  if (m_library == Library::Recent)
    addChip(tr("recently used"), [this] { m_libraryList->setCurrentRow(0); });
  if (m_library == Library::Sky)
    addChip(tr("skyboxes"), [this] { m_libraryList->setCurrentRow(0); });
  if (!m_pack.isEmpty())
    addChip(tr("pack: %1").arg(m_pack), [this] { m_packList->setCurrentRow(0); });
  if (m_alphaOnly && m_alphaOnly->isChecked())
    addChip(tr("transparent"), [this] { m_alphaOnly->setChecked(false); });
  if (m_freeOnly && m_freeOnly->isChecked())
    addChip(tr("free"), [this] { m_freeOnly->setChecked(false); });
  for (const QString &l: m_licenses)
    addChip(tr("license: %1").arg(licenseLabel(l)), [this, l] {
      m_licenses.removeAll(l);
      applyQuery();
    });
  m_tagPicker->addActiveChips(m_chipFlow, m_chipBox);
  const bool any = m_library != Library::All || !m_pack.isEmpty() || !m_tagPicker->empty() ||
                   (m_search && !m_search->text().isEmpty()) || (m_alphaOnly && m_alphaOnly->isChecked()) ||
                   (m_freeOnly && m_freeOnly->isChecked()) || !m_licenses.isEmpty();
  if (any) {
    auto *clear = new QToolButton(m_chipBox);
    clear->setObjectName(QStringLiteral("tbLink"));
    clear->setText(tr("Clear all"));
    connect(clear, &QToolButton::clicked, this, [this] { clearFilters(true); });
    m_chipFlow->addWidget(clear);
  }
  m_chipBox->setVisible(m_chipFlow->count() > 0);
}

void TextureBrowserWindow::clearFilters(const bool requery) {
  m_tagPicker->clear(false);
  m_licenses.clear();
  {
    QSignalBlocker b1(m_search), b2(m_libraryList), b3(m_packList), b4(m_alphaOnly), b5(m_freeOnly);
    m_search->clear();
    m_alphaOnly->setChecked(false);
    m_freeOnly->setChecked(false);
    m_libraryList->setCurrentRow(0);
    m_library = Library::All;
    m_packList->setCurrentRow(0);
    m_pack.clear();
  }
  if (requery) {
    applyQuery();
    updatePopularFilter();
  } else {
    rebuildFilterChips();
  }
}

void TextureBrowserWindow::rebuildLicenses() {
  if (!m_licenseList)
    return;
  m_licenseList->clear();
  QVector<QPair<QString, int>> lic = m_model->licenseFacets();
  // A chosen license stays listed even with no matches.
  for (const QString &k: m_licenses)
    if (std::none_of(lic.begin(), lic.end(), [&](const auto &p) { return p.first == k; }))
      lic.append({k, 0});
  int maxCount = 1;
  for (const auto &l: lic)
    maxCount = std::max(maxCount, l.second);
  for (const auto &[key, count]: lic) {
    const bool on = m_licenses.contains(key);
    auto *it = new QListWidgetItem(ActionManager::icon(on ? QStringLiteral("checkbox_hlt") : QStringLiteral("checkbox_dehlt")),
                                   licenseLabel(key));
    it->setData(Qt::UserRole, key);
    it->setData(Qt::UserRole + 1, QLocale().toString(count));
    it->setData(Qt::UserRole + 2, double(count) / maxCount);
    m_licenseList->addItem(it);
  }
  m_licenseList->setVisible(m_licenseList->count() > 0);
  fitListHeight(m_licenseList, 8);
}

void TextureBrowserWindow::rebuildPacks() {
  const QString keep = m_pack;
  QSignalBlocker b(m_packList);
  m_packList->clear();
  const QString project = m_engine ? m_engine->projectPack() : QString();
  int total = 0;
  for (const auto &p: net::TextureCatalog::instance().packs())
    total += p.count;
  auto *all = new QListWidgetItem(ActionManager::icon(QStringLiteral("texture")), tr("All packs"));
  all->setData(Qt::UserRole, QString());
  all->setData(Qt::UserRole + 1, QLocale().toString(total));
  m_packList->addItem(all);
  for (const auto &p: net::TextureCatalog::instance().packs()) {
    const bool isProject = !project.isEmpty() && p.name.compare(project, Qt::CaseInsensitive) == 0;
    auto *it = new QListWidgetItem(
        ActionManager::icon(isProject ? QStringLiteral("pinned") : (p.base ? QStringLiteral("locked") : QStringLiteral("unlocked"))),
        p.name);
    it->setData(Qt::UserRole, p.name);
    it->setData(Qt::UserRole + 1, QLocale().toString(p.count));
    it->setToolTip(QStringLiteral("%1<br>%2%3")
                       .arg(p.description.toHtmlEscaped(),
                            p.base ? tr("Base pack (read-only)") : tr("Editable pack"),
                            isProject ? tr("<br><b>Project pack of this map</b>") : QString()));
    m_packList->addItem(it);
  }
  int row = 0;
  for (int i = 0; i < m_packList->count(); ++i)
    if (m_packList->item(i)->data(Qt::UserRole).toString() == keep)
      row = i;
  m_packList->setCurrentRow(row);
  fitListHeight(m_packList, 12);
}

void TextureBrowserWindow::updatePopularFilter() {
  QJsonObject filter;
  if (m_alphaOnly && m_alphaOnly->isChecked())
    filter.insert(QStringLiteral("alpha"), true);
  filter.insert(QStringLiteral("sky"), m_library == Library::Sky);
  m_tagPicker->setPopularFilter(filter);
}

void TextureBrowserWindow::rebuildPager() {
  while (QLayoutItem *it = m_pagerLayout->takeAt(0)) {
    if (it->widget())
      it->widget()->deleteLater();
    delete it;
  }
  const int pages = m_model->pageCount();
  const int cur = m_model->page();
  auto button = [this](const QString &text, int page, bool enabled, bool checked) {
    auto *b = new QToolButton(this);
    b->setObjectName(QStringLiteral("tbPage"));
    b->setText(text);
    b->setCheckable(checked);
    b->setChecked(checked);
    b->setEnabled(enabled);
    connect(b, &QToolButton::clicked, this, [this, page] {
      m_model->setPage(page);
      m_grid->scrollToTop();
    });
    m_pagerLayout->addWidget(b);
  };
  auto gap = [this] {
    auto *l = new QLabel(QStringLiteral("…"), this);
    l->setObjectName(QStringLiteral("dlgDesc"));
    m_pagerLayout->addWidget(l);
  };
  if (pages <= 1)
    return;
  button(QStringLiteral("‹"), cur - 1, cur > 0, false);
  // 1 … (cur-2 .. cur+2) … last
  QList<int> show = {0, pages - 1};
  for (int p = cur - 2; p <= cur + 2; ++p)
    if (p >= 0 && p < pages)
      show.append(p);
  std::sort(show.begin(), show.end());
  show.erase(std::unique(show.begin(), show.end()), show.end());
  int prev = -1;
  for (int p: show) {
    if (prev >= 0 && p > prev + 1)
      gap();
    button(QString::number(p + 1), p, true, p == cur);
    prev = p;
  }
  button(QStringLiteral("›"), cur + 1, cur < pages - 1, false);
}

QString TextureBrowserWindow::selectedRef() const { return m_grid->currentIndex().data(RefRole).toString(); }

void TextureBrowserWindow::reveal(const QString &ref) {
  const MaterialRef r = MaterialRef::parse(ref);
  m_revealPending = r.base();
  {
    QSignalBlocker b(m_search);
    m_search->setText(r.name);
  }
  m_tagPicker->clear(false);
  m_licenses.clear();
  const TextureEntry *e = net::TextureCatalog::instance().find(r.base());
  m_library = e && e->isSky() ? Library::Sky : Library::All;
  {
    QSignalBlocker b(m_libraryList), b2(m_alphaOnly), b3(m_freeOnly);
    m_libraryList->setCurrentRow(int(m_library));
    m_alphaOnly->setChecked(false);
    m_freeOnly->setChecked(false);
  }
  m_pack = r.pack;
  rebuildPacks();
  applyQuery();
  updatePopularFilter();
}

void TextureBrowserWindow::showDetails(const QString &ref) {
  m_detailsRef = MaterialRef::parse(ref).base();
  if (const TextureEntry *e = net::TextureCatalog::instance().find(m_detailsRef)) {
    // Prefetch every map's thumbnail for the chips' hover previews.
    for (const auto &f: e->maps)
      if (!f.thumb.isEmpty())
        net::FileFetcher::instance().fetch(net::FileKind::Thumb, f.thumb, net::FetchPriority::Prefetch, this, {});
  }
  refreshDetails();
}

// Runs on every pane width change: the flat map at the texture's aspect, the 3D view square.
void TextureBrowserWindow::layoutPreview() {
  const TextureEntry *e = m_detailsRef.isEmpty() ? nullptr : net::TextureCatalog::instance().find(m_detailsRef);
  if (!e)
    return;
  const int w = std::max(scale::tpx(200), m_detailsBody->width() - 2);
  // A skybox is shown flat as its 2:1 thumbnail.
  const bool sky = e->isSky();
  const bool flat = sky || m_previewMode == PreviewMode::Flat;
  // 4:1 at most, so long strips do not push everything out of view.
  const double aspect = sky ? 0.5 : e->w > 0 && e->h > 0 ? std::clamp(double(e->h) / e->w, 0.25, 4.0) : 1.0;
  const int h = std::max(1, int(std::lround(w * aspect)));
  m_previewStack->setFixedHeight(flat ? h : w);
  m_preview3d->setFixedSize(w, w);

  // the 512 px editor map, the thumbnail until it arrives
  const MapFile *color = e->map(MapType::Color);
  QPixmap pm = color ? ThumbCache::instance().serverEditor(color->editor) : QPixmap();
  if (pm.isNull() && color)
    pm = ThumbCache::instance().server(color->thumb);
  if (pm.isNull() && sky)
    pm = ThumbCache::instance().server(e->thumbFile());
  if (pm.isNull()) {
    m_preview->setText(tr("loading…"));
    return;
  }
  const qreal dpr = devicePixelRatioF();
  QPixmap shown(QSize(w, h) * dpr);
  shown.setDevicePixelRatio(dpr);
  shown.fill(Qt::transparent);
  QPainter p(&shown);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, w, h), 8, 8);
  p.setClipPath(clip);
  // Fills the frame; only a clamped texture loses its ends. Repeated: 2 × 2 at half size.
  const int n = m_previewRepeated && m_previewMode == PreviewMode::Flat && !sky ? 2 : 1;
  const double cw = double(w) / n, ch = double(h) / n;
  const QSize target = QSize(int(std::ceil(cw * dpr)), int(std::ceil(ch * dpr)));
  const QPixmap scaled = pm.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
  const QRectF src((scaled.width() - target.width()) / 2.0, (scaled.height() - target.height()) / 2.0,
                   target.width(), target.height());
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      p.drawPixmap(QRectF(x * cw, y * ch, cw, ch), scaled, src);
  p.end();
  m_preview->setPixmap(shown);
}

void TextureBrowserWindow::refreshDetails() {
  const TextureEntry *e = m_detailsRef.isEmpty() ? nullptr : net::TextureCatalog::instance().find(m_detailsRef);
  m_detailsEmpty->setVisible(!e);
  m_detailsBody->setVisible(e);
  if (!e)
    return;

  const bool sky = e->isSky();
  m_previewSwitch->setVisible(!sky);
  m_previewStack->setCurrentIndex(sky || m_previewMode == PreviewMode::Flat ? 0 : 1);
  m_applyButton->setText(sky ? tr("Use as Skybox") : tr("Apply"));
  m_applyButton->setToolTip(sky ? tr("Make this the map's sky and take its sun, ambient light and fog colour "
                                     "(or double-click the card)")
                                : tr("Put this texture on the selected faces (or double-click the card)"));
  m_currentButton->setVisible(!sky);
  m_detailsProps->setVisible(!sky);
  layoutPreview();
  if (!sky && m_previewMode != PreviewMode::Flat) {
    const eng::MaterialOverrides fallback;
    const eng::MaterialOverrides &ov = m_engine ? m_engine->scene()->materialOverrides : fallback;
    m_preview3d->showTexture(*e, ov.resolve(e->ref, e).props);
  }

  m_detailsTitle->setText(e->name);
  m_detailsRef_->setText(e->ref);
  const net::LockInfo *lock = net::TextureCatalog::instance().lockFor(e->ref);
  m_detailsLock->setVisible(lock);
  if (lock)
    m_detailsLock->setText(tr("✎ %1 is editing this texture right now").arg(lock->name));

  // Base textures are read-only: copy them. User-pack textures are edited in place.
  m_copyButton->setVisible(e->base && !sky);
  m_editButton->setVisible(!sky);
  disconnect(m_editButton, nullptr, this, nullptr);
  if (e->base) {
    m_editButton->setMenu(nullptr);
    m_editButton->setText(tr("Copy && edit…"));
    m_editButton->setToolTip(tr("Base textures are read-only: copy into your project pack, then edit the copy"));
    connect(m_editButton, &QPushButton::clicked, this,
            [this] { emit TextureHub::instance().copyRequested(m_detailsRef); });
  } else {
    m_editButton->setText(tr("Edit map externally"));
    m_editButton->setToolTip(tr("Open one of its maps in the image editor; saving uploads it"));
    auto *menu = m_editButton->menu();
    if (!menu) {
      menu = new QMenu(m_editButton);
      connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        if (const TextureEntry *t = net::TextureCatalog::instance().find(m_detailsRef))
          addEditMapActions(menu, *t, this);
        menu->addSeparator();
        menu->addAction(ActionManager::icon(QStringLiteral("file_folder")), tr("Open in Texture Packs…"), this,
                        [this] { emit TextureHub::instance().openPacksRequested(m_detailsRef); });
      });
      m_editButton->setMenu(menu);
    }
  }

  // maps the viewport does not draw are dimmed
  m_mapFlow->clear();
  for (auto it = e->maps.begin(); it != e->maps.end(); ++it) {
    const bool rendered = mapTypeRendered(it.key());
    auto *chip = new QLabel(mapTypeLabel(it.key()), m_mapChips);
    chip->setStyleSheet(rendered ? QStringLiteral("background:#1d2a4d; color:#cfe0ff; border:1px solid #31477e; "
                                                  "border-radius:9px; padding:2px 9px;")
                                 : QStringLiteral("background:#191d2e; color:#7f88a6; border:1px dashed #333a55; "
                                                  "border-radius:9px; padding:2px 9px;"));
    QString tip = QStringLiteral("<b>%1</b><br>%2<br>%3")
                      .arg(mapTypeLabel(it.key()), it->file.toHtmlEscaped(), sizeText({it->w, it->h}));
    if (!rendered)
      tip += tr("<br><i>Not shown in the viewport (exported to Godot)</i>");
    if (const QString p = net::FileFetcher::instance().cachedPath(net::FileKind::Thumb, it->thumb); !p.isEmpty())
      tip += QStringLiteral("<br><img src=\"file://%1\" width=\"180\">").arg(p.toHtmlEscaped());
    chip->setToolTip(tip);
    m_mapFlow->addWidget(chip);
  }

  QStringList info;
  auto row = [&](const QString &k, const QString &v) {
    if (!v.isEmpty())
      info << QStringLiteral("<tr><td style='color:#8f9bbd; padding-right:12px'>%1</td><td>%2</td></tr>").arg(k, v);
  };
  row(tr("Pack"), e->pack + (e->base ? tr(" · base (read-only)") : tr(" · editable")));
  row(tr("Size"), sizeText({e->w, e->h}));
  if (sky) {
    const eng::SkyInfo si = eng::skyInfoFromJson(e->sky);
    row(tr("Sky"), si.title.toHtmlEscaped());
    row(tr("Time"), si.time.toHtmlEscaped());
    row(tr("Weather"), si.weather.toHtmlEscaped());
    if (si.valid)
      row(tr("Sun"), tr("%1° up, yaw %2°").arg(-si.pitch, 0, 'f', 0).arg(si.yaw, 0, 'f', 0));
    if (!si.source.isEmpty())
      row(tr("Source"), QStringLiteral("<a href=\"%1\" style='color:#8fb0ff'>%2</a>")
                            .arg(si.source.toHtmlEscaped(), QUrl(si.source).host().toHtmlEscaped()));
  } else {
    row(tr("Layout"), e->layout() == MapLayout::Arm        ? tr("ARM (AO · roughness · metal in one map)")
                      : e->layout() == MapLayout::Separate ? tr("separate roughness / metal / AO maps")
                                                           : tr("no roughness / metal / AO maps"));
  }
  row(tr("Author"), e->author.toHtmlEscaped());
  row(tr("License"), e->license.toHtmlEscaped());
  row(tr("Copied from"), e->derivedFrom.toHtmlEscaped());
  m_detailsInfo->setText(QStringLiteral("<table>%1</table>").arg(info.join(QString())));

  m_detailTagFlow->clear();
  const QFont f = scale::font(scale::Font::Caption, font());
  for (const QString &t: e->tags) {
    auto *chip = new TagChip(t, -1, m_detailTags);
    chip->setFont(f);
    if (m_tagPicker->tags().contains(t))
      chip->setState(TagChip::State::Include);
    connect(chip, &TagChip::toggledInclude, this, [this](const QString &tag, bool on) {
      on ? m_tagPicker->addTag(tag, false) : m_tagPicker->removeTag(tag);
    });
    connect(chip, &TagChip::excludeRequested, this, [this](const QString &tag) { m_tagPicker->addTag(tag, true); });
    m_detailTagFlow->addWidget(chip);
  }

  if (m_detailsProps->shownRef() != e->ref)
    m_detailsProps->showMaterial(e->ref);
}

bool TextureBrowserWindow::eventFilter(QObject *obj, QEvent *e) {
  if (obj == m_grid && e->type() == QEvent::KeyPress) {
    auto *ke = static_cast<QKeyEvent *>(e);
    if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !selectedRef().isEmpty()) {
      m_applyButton->click();
      return true;
    }
  }
  if (obj == m_detailsBody && e->type() == QEvent::Resize) {
    const auto *re = static_cast<QResizeEvent *>(e);
    if (re->size().width() != re->oldSize().width())
      layoutPreview();
  }
  if (obj == m_grid->viewport() && e->type() == QEvent::Resize)
    m_gridMessage->setGeometry(m_grid->viewport()->rect());
  return QMainWindow::eventFilter(obj, e);
}

void TextureBrowserWindow::saveWindowState() {
  config()->setRaw(QStringLiteral("TextureBrowser/Geometry"), QString::fromLatin1(saveGeometry().toBase64()));
  config()->setRaw(QStringLiteral("TextureBrowser/Splitter"), QString::fromLatin1(m_split->saveState().toBase64()));
}

void TextureBrowserWindow::closeEvent(QCloseEvent *e) {
  saveWindowState();
  // Closing the browser shows the selection tint again.
  if (m_engine && m_engine->maskHidden()) {
    m_engine->setMaskHidden(false);
    m_engine->requestRedraw();
  }
  QMainWindow::closeEvent(e);
}

} // namespace ui
