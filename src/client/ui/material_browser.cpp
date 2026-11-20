#include "material_browser.h"
#include "client/ui/scale.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QAction>
#include <QKeySequence>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/texture_library.h"
#include "client/net/asset_client.h"
#include "client/net/texture_catalog.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/flow_layout.h"
#include "client/ui/texture_actions.h"
#include "client/ui/texture_ui.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

LocalLibraryModel::LocalLibraryModel(QObject *parent) : QAbstractListModel(parent) {
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, [this](const QString &key) {
    if (!key.startsWith(QLatin1String("l:")))
      return;
    const QString path = key.mid(2);
    for (int i = 0; i < m_items.size(); ++i)
      if (m_items[i].path == path)
        emit dataChanged(index(i), index(i), {ThumbRole});
  });
}

void LocalLibraryModel::rebuild() {
  beginResetModel();
  m_items.clear();
  // The editor's own dev texture (new brushes' default): not a file in the library.
  m_items.push_back({QString::fromLatin1(vk::Engine3D::kDefaultBrushMaterial), QStringLiteral("dev_measurewall01d"),
                     tr("Built-in"), QStringLiteral(":/assets/measurewall01_Color.jpg"), QSize(512, 512)});
  for (const auto &e: eng::TextureLibrary::instance().entries())
    m_items.push_back({e.name, e.display, e.category.isEmpty() ? tr("(root)") : e.category, e.albedo, e.size});
  endResetModel();
}

QStringList LocalLibraryModel::categories() const {
  QStringList out;
  for (const auto &it: m_items)
    if (!out.contains(it.category))
      out.push_back(it.category);
  out.sort();
  return out;
}

int LocalLibraryModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QVariant LocalLibraryModel::data(const QModelIndex &idx, int role) const {
  if (!idx.isValid() || idx.row() >= m_items.size())
    return {};
  const Item &it = m_items[idx.row()];
  switch (role) {
    case Qt::DisplayRole: return it.display;
    case RefRole: return it.name;
    case PackRole: return it.category;
    case CategoryRole: return it.category;
    case ThumbRole: return ThumbCache::instance().local(it.path);
    case SizeRole: return it.size;
    case BaseRole: return true;
    case Qt::ToolTipRole:
      return QStringLiteral("<b>%1</b><br>%2<br>%3")
          .arg(it.display.toHtmlEscaped(), it.name.toHtmlEscaped(), sizeText(it.size));
    default: return {};
  }
}

Qt::ItemFlags LocalLibraryModel::flags(const QModelIndex &idx) const {
  return QAbstractListModel::flags(idx) | Qt::ItemIsDragEnabled;
}

QStringList LocalLibraryModel::mimeTypes() const { return {QString::fromLatin1(kMaterialMime), QStringLiteral("text/plain")}; }

QMimeData *LocalLibraryModel::mimeData(const QModelIndexList &indexes) const {
  if (indexes.isEmpty())
    return nullptr;
  auto *md = new QMimeData();
  const QString name = indexes.first().data(RefRole).toString();
  md->setData(QString::fromLatin1(kMaterialMime), name.toUtf8());
  md->setText(name);
  return md;
}

bool LocalFilterModel::filterAcceptsRow(int row, const QModelIndex &parent) const {
  const QModelIndex idx = sourceModel()->index(row, 0, parent);
  if (!m_category.isEmpty() && idx.data(LocalLibraryModel::CategoryRole).toString() != m_category)
    return false;
  const QString name = idx.data(RefRole).toString();
  for (const QString &w: m_text.split(QLatin1Char(' '), Qt::SkipEmptyParts))
    if (!name.contains(w, Qt::CaseInsensitive))
      return false;
  return true;
}

namespace {

const char *kSegmentStyle =
    "QToolButton { border: 1px solid #3a4258; background: #262b3a; color: #aeb8d4; padding: 3px 10px; }"
    "QToolButton:checked { background: #2f6fb4; border-color: #4a8ad0; color: white; }"
    "QToolButton:hover:!checked { background: #2f3548; }"
    "QToolButton#segFirst { border-top-left-radius: 5px; border-bottom-left-radius: 5px; }"
    "QToolButton#segLast { border-top-right-radius: 5px; border-bottom-right-radius: 5px; }";

} // namespace

QListView *MaterialBrowser::makeView(QAbstractItemModel *model) {
  QListView *view = makeTextureGrid(model, m_delegate, this);
  view->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(view, &QListView::clicked, this, [this](const QModelIndex &idx) {
    if (m_engine)
      m_engine->setCurrentMaterial(idx.data(RefRole).toString());
  });
  connect(view, &QListView::doubleClicked, this, [this](const QModelIndex &idx) {
    const QString ref = idx.data(RefRole).toString();
    if (m_engine) {
      m_engine->setCurrentMaterial(ref);
      m_engine->applyMaterialToSelection(ref);
    }
    emit materialActivated(ref);
  });
  connect(view, &QListView::customContextMenuRequested, this,
          [this, view](const QPoint &pos) { showContextMenu(view, pos); });
  return view;
}

MaterialBrowser::MaterialBrowser(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);

  // Row 1: source tabs + full browser
  auto *top = new QHBoxLayout();
  top->setSpacing(0);
  m_tabs = new QButtonGroup(this);
  const QStringList names = {tr("Server"), tr("This map"), tr("Local")};
  for (int i = 0; i < names.size(); ++i) {
    auto *b = new QToolButton(this);
    b->setText(names[i]);
    b->setCheckable(true);
    b->setObjectName(i == 0 ? QStringLiteral("segFirst") : (i == names.size() - 1 ? QStringLiteral("segLast") : QString()));
    b->setStyleSheet(QString::fromLatin1(kSegmentStyle));
    m_tabs->addButton(b, i);
    top->addWidget(b);
  }
  m_tabs->button(ServerTab)->setChecked(true);
  m_tabs->button(ServerTab)->setToolTip(tr("Textures on the asset server"));
  m_tabs->button(MapTab)->setToolTip(tr("Materials this map uses, including its variants"));
  m_tabs->button(LocalTab)->setToolTip(tr("The local texture library folder"));
  top->addStretch(1);
  m_shuffle = new ShuffleButton(this);
  connect(m_shuffle, &ShuffleButton::orderChanged, this, [this] { applyServerQuery(); });
  top->addWidget(m_shuffle);
  auto *openBrowser = new QToolButton(this);
  openBrowser->setIcon(ActionManager::icon(QStringLiteral("viewzoom")));
  openBrowser->setText(tr("Browse"));
  openBrowser->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  openBrowser->setAutoRaise(true);
  // The tooltip names the current shortcut; deferred first call: the actions exist once the window is set up.
  auto browseTip = [openBrowser] {
    const QAction *a = act(QStringLiteral("tex.browser"));
    const QString key = a && !a->shortcut().isEmpty() ? a->shortcut().toString(QKeySequence::NativeText) : QString();
    openBrowser->setToolTip(key.isEmpty() ? tr("Open the Texture Browser: tags, packs and details")
                                          : tr("Open the Texture Browser: tags, packs and details (%1)").arg(key));
  };
  QTimer::singleShot(0, openBrowser, [openBrowser, browseTip] {
    browseTip();
    if (QAction *a = act(QStringLiteral("tex.browser")))
      connect(a, &QAction::changed, openBrowser, browseTip);
  });
  connect(openBrowser, &QToolButton::clicked, this, [this] { emit openBrowserRequested(); });
  top->addWidget(openBrowser);
  v->addLayout(top);

  // Row 2: search + pack / category
  auto *bar = new QHBoxLayout();
  m_search = new QLineEdit(this);
  m_search->setPlaceholderText(tr("Search textures…"));
  m_search->setClearButtonEnabled(true);
  m_pack = new QComboBox(this);
  m_pack->setToolTip(tr("Texture pack"));
  m_pack->addItem(tr("All packs"), QString());
  m_category = new QComboBox(this);
  m_category->setToolTip(tr("Folder"));
  m_category->hide();
  bar->addWidget(m_search, 1);
  bar->addWidget(m_pack);
  bar->addWidget(m_category);
  v->addLayout(bar);

  // Row 3: popular tags (server tab)
  auto *chipScroll = new QScrollArea(this);
  chipScroll->setWidgetResizable(true);
  chipScroll->setFrameShape(QFrame::NoFrame);
  // One line of chips, no scroll bar.
  chipScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  chipScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_chipRow = new QWidget(chipScroll);
  m_chipLayout = new QHBoxLayout(m_chipRow);
  m_chipLayout->setContentsMargins(0, 0, 0, 0);
  m_chipLayout->setSpacing(4);
  m_chipLayout->addStretch(1);
  chipScroll->setWidget(m_chipRow);
  chipScroll->setFixedHeight(TagChip(QStringLiteral("x"), -1).sizeHint().height() + 4);
  v->addWidget(chipScroll);
  m_chipRow->setProperty("scroll", QVariant::fromValue<QObject *>(chipScroll));

  m_delegate = new TextureCardDelegate(this);
  m_serverModel = new ServerTextureModel(ServerTextureModel::Mode::Append, this);
  m_mapModel = new MapMaterialsModel(engine, this);
  m_localModel = new LocalLibraryModel(this);
  m_localProxy = new LocalFilterModel(this);
  m_localProxy->setSourceModel(m_localModel);
  m_localProxy->setSortRole(Qt::DisplayRole);
  m_localProxy->sort(0);

  m_stack = new QStackedWidget(this);
  m_serverStack = new QStackedWidget(m_stack);
  m_serverView = makeView(m_serverModel);
  m_offline = new QLabel(m_serverStack);
  m_offline->setAlignment(Qt::AlignCenter);
  m_offline->setWordWrap(true);
  m_offline->setStyleSheet(QStringLiteral("color:#8a96b4; padding: 20px;"));
  m_serverStack->addWidget(m_serverView);
  m_serverStack->addWidget(m_offline);
  m_mapView = makeView(m_mapModel);
  m_localView = makeView(m_localProxy);
  m_stack->addWidget(m_serverStack);
  m_stack->addWidget(m_mapView);
  m_stack->addWidget(m_localView);
  v->addWidget(m_stack, 1);

  // Footer: current material, count, refresh pill, size slider
  auto *foot = new QHBoxLayout();
  m_current = new QLabel(this);
  m_current->setStyleSheet(QStringLiteral("color:#ffb03c;"));
  m_current->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_count = new QLabel(this);
  m_count->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  m_refreshPill = new QToolButton(this);
  m_refreshPill->setStyleSheet(QStringLiteral(
      "QToolButton { background:#2f6fb4; color:white; border-radius: 9px; padding: 1px 9px; border: none; }"
      "QToolButton:hover { background:#3a82cf; }"));
  m_refreshPill->hide();
  connect(m_refreshPill, &QToolButton::clicked, this, [this] { m_serverModel->reload(); });
  m_size = new QSlider(Qt::Horizontal, this);
  m_size->setRange(48, 192);
  scale::bind(m_size, [=, this] { m_size->setFixedWidth(scale::tpx(100)); });
  m_size->setToolTip(tr("Thumbnail size"));
  foot->addWidget(m_current, 1);
  foot->addWidget(m_refreshPill);
  foot->addWidget(m_count);
  foot->addWidget(m_size);
  v->addLayout(foot);

  prefs::get().declare([] {
    prefs::Def d;
    d.key = QStringLiteral("Materials/ThumbSize");
    d.category = QStringLiteral("General");
    d.label = QStringLiteral("Material browser thumbnail size");
    d.type = prefs::Type::Int;
    d.defaultValue = 96;
    d.minValue = 48;
    d.maxValue = 192;
    d.suffix = QStringLiteral(" px");
    return d;
  }());
  prefs::get().watch(QStringLiteral("Materials/ThumbSize"), this, [this](const QVariant &val) {
    QSignalBlocker b(m_size);
    m_size->setValue(val.toInt());
    m_delegate->setThumbSize(val.toInt());
    for (QListView *view: {m_serverView, m_mapView, m_localView})
      view->doItemsLayout();
  });
  connect(m_size, &QSlider::valueChanged, this,
          [](int px) { prefs::get().setValue(QStringLiteral("Materials/ThumbSize"), px); });

  // search: debounced server query, instant local filter
  m_searchDebounce = new QTimer(this);
  m_searchDebounce->setSingleShot(true);
  m_searchDebounce->setInterval(250);
  connect(m_searchDebounce, &QTimer::timeout, this, &MaterialBrowser::applyServerQuery);
  connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
    m_localProxy->setText(t);
    m_searchDebounce->start();
    updateCount();
  });
  connect(m_pack, &QComboBox::currentIndexChanged, this, &MaterialBrowser::applyServerQuery);
  connect(m_category, &QComboBox::currentIndexChanged, this, [this] {
    m_localProxy->setCategory(m_category->currentData().toString());
    updateCount();
  });
  connect(m_tabs, &QButtonGroup::idClicked, this, &MaterialBrowser::setTab);

  connect(m_serverModel, &ServerTextureModel::resultChanged, this, &MaterialBrowser::updateCount);
  connect(m_serverModel, &ServerTextureModel::failed, this, [this](const QString &msg) {
    m_offline->setText(tr("Could not load textures:\n%1").arg(msg));
    m_serverStack->setCurrentWidget(m_offline);
  });
  connect(m_serverModel, &ServerTextureModel::outdated, this, [this](int n) {
    m_refreshPill->setVisible(n > 0);
    m_refreshPill->setText(tr("%n change(s) · Refresh", nullptr, n));
  });
  connect(m_mapModel, &QAbstractItemModel::modelReset, this, &MaterialBrowser::updateCount);
  connect(m_localProxy, &QAbstractItemModel::modelReset, this, &MaterialBrowser::updateCount);
  connect(m_localModel, &QAbstractItemModel::modelReset, this, [this] {
    const QString keep = m_category->currentData().toString();
    QSignalBlocker b(m_category);
    m_category->clear();
    m_category->addItem(tr("All folders"), QString());
    for (const QString &c: m_localModel->categories())
      m_category->addItem(c, c);
    const int idx = m_category->findData(keep);
    m_category->setCurrentIndex(idx < 0 ? 0 : idx);
    updateCount();
  });

  auto &catalog = net::TextureCatalog::instance();
  connect(&catalog, &net::TextureCatalog::packsChanged, this, [this] {
    const QString keep = m_pack->currentData().toString();
    QSignalBlocker b(m_pack);
    m_pack->clear();
    m_pack->addItem(tr("All packs"), QString());
    for (const auto &p: net::TextureCatalog::instance().packs())
      m_pack->addItem(ActionManager::icon(p.base ? QStringLiteral("locked") : QStringLiteral("unlocked")),
                      QStringLiteral("%1  (%2)").arg(p.name, QLocale().toString(p.count)), p.name);
    const int idx = m_pack->findData(keep);
    m_pack->setCurrentIndex(idx < 0 ? 0 : idx);
  });
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, [this] {
    refreshServerState();
    if (net::AssetClient::instance().isOnline())
      refreshChips();
  });

  if (m_engine) {
    connect(m_engine, &vk::Engine3D::currentMaterialChanged, this, [this](const QString &name) {
      m_delegate->setCurrent(name);
      m_current->setText(name.isEmpty() ? QString() : QStringLiteral("● %1").arg(name));
      for (QListView *view: {m_serverView, m_mapView, m_localView})
        view->viewport()->update();
    });
    auto *usedTimer = new QTimer(this);
    usedTimer->setSingleShot(true);
    usedTimer->setInterval(400);
    connect(usedTimer, &QTimer::timeout, this, &MaterialBrowser::refreshUsed);
    connect(m_engine, &vk::Engine3D::sceneChanged, usedTimer, qOverload<>(&QTimer::start));
    connect(m_engine, &vk::Engine3D::objectStateChanged, usedTimer, qOverload<>(&QTimer::start));
  }
  connect(&eng::TextureLibrary::instance(), &eng::TextureLibrary::changed, m_localModel, &LocalLibraryModel::rebuild);
  m_localModel->rebuild();
  refreshServerState();
  applyServerQuery();
  refreshChips();
  refreshUsed();
  setTab(ServerTab);
}

void MaterialBrowser::setTab(int tab) {
  m_tab = tab;
  if (QAbstractButton *b = m_tabs->button(tab))
    b->setChecked(true);
  m_stack->setCurrentIndex(tab);
  const bool server = tab == ServerTab;
  m_pack->setVisible(server);
  m_shuffle->setVisible(server);
  m_category->setVisible(tab == LocalTab);
  m_search->setVisible(tab != MapTab);
  if (auto *scroll = qobject_cast<QWidget *>(m_chipRow->property("scroll").value<QObject *>()))
    scroll->setVisible(server);
  m_refreshPill->setVisible(server && !m_refreshPill->text().isEmpty() && m_refreshPill->isVisible());
  if (tab == MapTab)
    m_mapModel->rebuild();
  updateCount();
}

void MaterialBrowser::applyServerQuery() {
  m_serverModel->setQuery(serverQuery(m_search->text(), m_pack->currentData().toString(), m_activeTags, m_shuffle));
  m_serverView->scrollToTop();
}

void MaterialBrowser::refreshChips() {
  QPointer<MaterialBrowser> self(this);
  net::TextureCatalog::instance().popularTags({{QStringLiteral("sky"), false}}, this,
                                              [self](const QVector<QPair<QString, int>> &tags) {
    if (!self)
      return;
    // keep the trailing stretch
    while (self->m_chipLayout->count() > 1) {
      QLayoutItem *it = self->m_chipLayout->takeAt(0);
      if (it->widget())
        it->widget()->deleteLater();
      delete it;
    }
    int shown = 0;
    // in the server's order
    for (const auto &[tag, count]: tags) {
      if (count <= 0 || shown >= 8)
        continue;
      auto *chip = new TagChip(tag, -1, self->m_chipRow);
      chip->setState(self->m_activeTags.contains(tag) ? TagChip::State::Include : TagChip::State::Off);
      connect(chip, &TagChip::toggledInclude, self, [self](const QString &t, bool on) {
        if (on && !self->m_activeTags.contains(t))
          self->m_activeTags.append(t);
        else if (!on)
          self->m_activeTags.removeAll(t);
        self->applyServerQuery();
      });
      self->m_chipLayout->insertWidget(self->m_chipLayout->count() - 1, chip);
      ++shown;
    }
    // the row's height follows the chips (font from the dock's stylesheet)
    if (auto *scroll = qobject_cast<QWidget *>(self->m_chipRow->property("scroll").value<QObject *>()))
      if (self->m_chipLayout->count() > 1)
        scroll->setFixedHeight(self->m_chipLayout->itemAt(0)->widget()->sizeHint().height() + 4);
  });
}

void MaterialBrowser::refreshServerState() {
  const auto st = net::AssetClient::instance().state();
  switch (st) {
    case net::AssetClient::State::Online: m_serverStack->setCurrentWidget(m_serverView); break;
    case net::AssetClient::State::Loading:
      m_offline->setText(tr("The asset server is loading its catalogue…"));
      m_serverStack->setCurrentWidget(m_offline);
      break;
    case net::AssetClient::State::Connecting:
      m_offline->setText(tr("Connecting to the asset server…"));
      m_serverStack->setCurrentWidget(m_offline);
      break;
    case net::AssetClient::State::Offline:
      m_offline->setText(tr("Asset server offline\n%1").arg(net::AssetClient::instance().offlineDetail()));
      m_serverStack->setCurrentWidget(m_offline);
      break;
  }
  updateCount();
}

void MaterialBrowser::updateCount() {
  const QLocale loc;
  switch (m_tab) {
    case ServerTab:
      m_count->setText(m_serverModel->total() > 0 ? tr("%1 of %2")
                                                        .arg(loc.toString(m_serverModel->rowCount()))
                                                        .arg(loc.toString(m_serverModel->total()))
                                                  : QString());
      break;
    case MapTab: m_count->setText(tr("%n material(s)", nullptr, m_mapModel->rowCount())); break;
    case LocalTab:
      m_count->setText(tr("%1 / %2").arg(m_localProxy->rowCount()).arg(m_localModel->rowCount()));
      break;
  }
}

void MaterialBrowser::refreshUsed() {
  if (!m_engine)
    return;
  QSet<QString> keys;
  for (const QString &n: m_engine->materialsInUse())
    keys.insert(refKey(MaterialRef::parse(n).base()));
  m_serverModel->setUsedRefs(keys);
}

void MaterialBrowser::reveal(const QString &materialName) {
  for (int r = 0; r < m_mapModel->rowCount(); ++r) {
    const QModelIndex idx = m_mapModel->index(r, 0);
    if (idx.data(RefRole).toString().compare(materialName, Qt::CaseInsensitive) == 0) {
      setTab(MapTab);
      m_mapView->setCurrentIndex(idx);
      m_mapView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
      return;
    }
  }
  setTab(ServerTab);
  m_search->setText(MaterialRef::parse(materialName).name);
}

void MaterialBrowser::showContextMenu(QListView *view, const QPoint &pos) {
  const QModelIndex idx = view->indexAt(pos);
  if (!idx.isValid())
    return;
  QMenu menu(this);
  populateTextureMenu(menu, m_engine, idx.data(RefRole).toString(), this);
  menu.exec(view->viewport()->mapToGlobal(pos));
}

} // namespace ui
