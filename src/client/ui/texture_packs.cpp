#include "texture_packs.h"
#include "client/ui/scale.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "client/mainwindow.h"
#include "client/net/asset_client.h"
#include "client/net/file_fetcher.h"
#include "client/net/texture_catalog.h"
#include "client/net/texture_edit_sessions.h"
#include "client/ui/action_manager.h"
#include "client/ui/dialog_style.h"
#include "client/ui/flow_layout.h"
#include "client/ui/texture_actions.h"
#include "client/ui/texture_ui.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "client/ui/file_dialog.h"
#include "shared/lib/config.h"

namespace ui {

namespace {

QPointer<TexturePacksWindow> s_window;

QLabel *section(const QString &text, QWidget *parent) {
  auto *l = new QLabel(text.toUpper(), parent);
  l->setObjectName(QStringLiteral("dlgSection"));
  return l;
}

QPushButton *flatButton(const QString &text, const QString &icon, QWidget *parent) {
  auto *b = new QPushButton(ActionManager::icon(icon), text, parent);
  b->setObjectName(QStringLiteral("dlgFlat"));
  return b;
}

QToolButton *toolButton(const QString &icon, const QString &tip, QWidget *parent) {
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("dlgTool"));
  b->setIcon(ActionManager::icon(icon));
  b->setToolTip(tip);
  return b;
}

QPixmap rounded(const QPixmap &pm, int edge) {
  QPixmap shown(edge, edge);
  shown.fill(Qt::transparent);
  QPainter p(&shown);
  p.setRenderHint(QPainter::Antialiasing);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, edge, edge), 5, 5);
  p.setClipPath(clip);
  if (!pm.isNull())
    p.drawPixmap(0, 0, pm.scaled(edge, edge, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
  else
    TextureCardDelegate::paintPlaceholder(&p, QRect(0, 0, edge, edge));
  return shown;
}

} // namespace

TexturePacksWindow *TexturePacksWindow::open(vk::Engine3D *engine, QWidget *owner, const QString &selectRef) {
  if (!s_window)
    s_window = new TexturePacksWindow(engine, owner);
  if (s_window->isMinimized())
    s_window->showNormal();
  s_window->show();
  s_window->raise();
  s_window->activateWindow();
  if (!selectRef.isEmpty()) {
    const MaterialRef r = MaterialRef::parse(selectRef);
    s_window->m_pendingSelect = r.base();
    s_window->selectPack(r.pack);
    s_window->showTexture(r.base());
  }
  return s_window;
}

void TexturePacksWindow::closeIfOpen() {
  if (s_window)
    s_window->close();
}

TexturePacksWindow::TexturePacksWindow(vk::Engine3D *engine, QWidget *owner)
    : QMainWindow(nullptr, Qt::Window), m_engine(engine) {
  setObjectName(QStringLiteral("TexturePacks"));
  setWindowTitle(tr("Texture Packs"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setAttribute(Qt::WA_QuitOnClose, false);
  if (owner)
    connect(owner, &QObject::destroyed, this, &QObject::deleteLater);
  scale::setScaledStyleSheet(this, dialogStyle() + QStringLiteral(
                                    "QWidget#tpSide, QWidget#tpEditor { background: #0f1326; }"
                                    "QWidget#tpCenter { background: #101428; }"
                                    "QLabel#tpLock { background: #3a2a08; color: #ffc864; border: 1px solid #6b4e14; "
                                    "border-radius: 6px; padding: 4px 8px; }"
                                    "QLabel#tpBase { background: #1b2140; color: #9fb0d8; border: 1px solid #2c3766; "
                                    "border-radius: 6px; padding: 6px 8px; }"));
  resize(scale::initialSize(this, {1500, 900}));

  m_model = new ServerTextureModel(ServerTextureModel::Mode::Append, this);
  m_model->setPageSize(120);
  m_delegate = new TextureCardDelegate(this);
  m_delegate->setThumbSize(112);
  m_delegate->setShowPack(false);

  m_split = new QSplitter(Qt::Horizontal, this);
  m_split->addWidget(buildPacks());
  m_split->addWidget(buildTextures());
  m_split->addWidget(buildEditor());
  m_split->setStretchFactor(1, 1);
  m_split->setSizes({scale::px(280), scale::px(760), scale::px(460)});
  m_split->setChildrenCollapsible(false);
  setCentralWidget(m_split);
  m_status = new QLabel(this);
  statusBar()->addWidget(m_status, 1);

  auto &cat = net::TextureCatalog::instance();
  connect(&cat, &net::TextureCatalog::packsChanged, this, [this] {
    rebuildPacks();
    rebuildProjectCombo();
  });
  connect(&cat, &net::TextureCatalog::entryChanged, this, [this](const QString &ref) {
    if (refKey(ref) == refKey(m_ref))
      refreshEditor();
  });
  connect(&cat, &net::TextureCatalog::entryRenamed, this, [this](const QString &oldRef, const QString &newRef) {
    if (refKey(oldRef) == refKey(m_ref))
      showTexture(newRef);
  });
  connect(&cat, &net::TextureCatalog::entryRemoved, this, [this](const QString &ref) {
    if (refKey(ref) == refKey(m_ref))
      showTexture({});
    m_model->reload();
  });
  connect(&cat, &net::TextureCatalog::lockChanged, this, [this](const QString &ref) {
    if (refKey(MaterialRef::parse(ref).base()) == refKey(m_ref))
      refreshEditor();
  });
  connect(m_model, &ServerTextureModel::outdated, this, [this](int n) {
    if (n > 0)
      m_model->reload();  // a pack's list is small
  });
  connect(m_model, &ServerTextureModel::resultChanged, this, [this] {
    m_gridInfo->setText(tr("%n texture(s)", nullptr, m_model->total()));
    if (!m_pendingSelect.isEmpty()) {
      const int row = m_model->rowOf(m_pendingSelect);
      if (row >= 0) {
        m_grid->setCurrentIndex(m_model->index(row, 0));
        m_grid->scrollTo(m_model->index(row, 0));
        m_pendingSelect.clear();
      }
    }
  });
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, [this](const QString &key) {
    if (!m_ref.isEmpty() && key.startsWith(QLatin1String("s:")))
      refreshEditor();
  });
  auto &sessions = net::TextureEditSessions::instance();
  connect(&sessions, &net::TextureEditSessions::sessionsChanged, this, &TexturePacksWindow::rebuildSessions);
  connect(&sessions, &net::TextureEditSessions::message, this, [this](const QString &m) { report(m); });
  if (m_engine)
    connect(m_engine, &vk::Engine3D::materialOverridesChanged, this, [this] {
      rebuildProjectCombo();
      rebuildPacks();
    });

  const QByteArray geo = config()->getRaw(QStringLiteral("TexturePacks/Geometry")).toByteArray();
  if (!geo.isEmpty())
    restoreGeometry(QByteArray::fromBase64(geo));
  const QByteArray split = config()->getRaw(QStringLiteral("TexturePacks/Splitter")).toByteArray();
  if (!split.isEmpty())
    m_split->restoreState(QByteArray::fromBase64(split));

  rebuildPacks();
  rebuildProjectCombo();
  rebuildSessions();
  showTexture({});
}

QWidget *TexturePacksWindow::buildPacks() {
  auto *w = new QWidget(this);
  w->setObjectName(QStringLiteral("tpSide"));
  scale::bind(w, [=, this] { w->setMinimumWidth(scale::tpx(240)); });
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(14, 14, 10, 14);
  v->setSpacing(8);
  v->addWidget(section(tr("Packs"), w));
  m_packs = new QListWidget(w);
  m_packs->setObjectName(QStringLiteral("dlgList"));
  m_packs->setItemDelegate(new CountListDelegate(m_packs));
  scale::bind(m_packs, [this] { m_packs->doItemsLayout(); });
  connect(m_packs, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
    if (it)
      selectPack(it->data(Qt::UserRole).toString());
  });
  v->addWidget(m_packs, 1);
  auto *row = new QHBoxLayout();
  auto *add = flatButton(tr("New pack"), QStringLiteral("add"), w);
  connect(add, &QPushButton::clicked, this, &TexturePacksWindow::newPack);
  m_renamePack = flatButton(QString(), QStringLiteral("greasepencil"), w);
  m_renamePack->setToolTip(tr("Rename the selected pack"));
  connect(m_renamePack, &QPushButton::clicked, this, &TexturePacksWindow::renamePack);
  m_deletePack = flatButton(QString(), QStringLiteral("x"), w);
  m_deletePack->setToolTip(tr("Delete the selected pack (its files go to the server's trash)"));
  connect(m_deletePack, &QPushButton::clicked, this, &TexturePacksWindow::deletePack);
  row->addWidget(add, 1);
  row->addWidget(m_renamePack);
  row->addWidget(m_deletePack);
  v->addLayout(row);
  m_packInfo = new QLabel(w);
  m_packInfo->setObjectName(QStringLiteral("dlgDesc"));
  m_packInfo->setWordWrap(true);
  v->addWidget(m_packInfo);

  v->addSpacing(8);
  v->addWidget(section(tr("This map"), w));
  auto *projHelp = new QLabel(tr("Project pack"), w);
  projHelp->setObjectName(QStringLiteral("dlgDesc"));
  projHelp->setWordWrap(true);
  v->addWidget(projHelp);
  m_project = new QComboBox(w);
  connect(m_project, &QComboBox::activated, this, [this] {
    if (m_engine)
      m_engine->setProjectPack(m_project->currentData().toString());
  });
  v->addWidget(m_project);
  auto *fromMap = flatButton(tr("New pack for this map"), QStringLiteral("file_new"), w);
  fromMap->setToolTip(tr("Creates a pack named after the map file and makes it the project pack"));
  connect(fromMap, &QPushButton::clicked, this, [this] {
    QString name;
    if (auto *mw = qobject_cast<MainWindow *>(m_engine ? m_engine->parent() : nullptr))
      name = QFileInfo(mw->currentMapPath()).completeBaseName();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    bool ok = false;
    name = QInputDialog::getText(this, tr("New pack for this map"), tr("Pack name:"), QLineEdit::Normal,
                                 name.isEmpty() ? QStringLiteral("my_map") : name, &ok)
               .trimmed();
    if (!ok || name.isEmpty())
      return;
    QPointer<TexturePacksWindow> self(this);
    net::TextureCatalog::instance().call(
        QStringLiteral("packs.create"), QJsonObject{{"name", name}, {"description", tr("Textures for %1").arg(name)}},
        this, [self, name](const net::Result &r) {
          if (!self)
            return;
          if (!r.ok) {
            self->report(r.message, true);
            return;
          }
          if (self->m_engine)
            self->m_engine->setProjectPack(name);
          self->report(tr("Created %1 — it is this map's project pack now.").arg(name));
        });
  });
  v->addWidget(fromMap);
  return w;
}

QWidget *TexturePacksWindow::buildTextures() {
  auto *w = new QWidget(this);
  w->setObjectName(QStringLiteral("tpCenter"));
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(14, 14, 14, 10);
  v->setSpacing(8);
  auto *top = new QHBoxLayout();
  m_search = new QLineEdit(w);
  m_search->setPlaceholderText(tr("Search in this pack…"));
  m_search->setClearButtonEnabled(true);
  auto *debounce = new QTimer(this);
  debounce->setSingleShot(true);
  debounce->setInterval(250);
  connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
  connect(debounce, &QTimer::timeout, this, [this] { selectPack(m_pack); });
  top->addWidget(m_search, 1);
  m_gridInfo = new QLabel(w);
  m_gridInfo->setObjectName(QStringLiteral("dlgDesc"));
  top->addWidget(m_gridInfo);
  v->addLayout(top);
  m_grid = new QListView(w);
  m_grid->setModel(m_model);
  m_grid->setItemDelegate(m_delegate);
  scale::bind(m_grid, [this] { m_grid->doItemsLayout(); });
  m_grid->setViewMode(QListView::IconMode);
  m_grid->setResizeMode(QListView::Adjust);
  m_grid->setMovement(QListView::Static);
  m_grid->setUniformItemSizes(true);
  m_grid->setMouseTracking(true);
  m_grid->setSpacing(6);
  m_grid->setFrameShape(QFrame::NoFrame);
  m_grid->setDragEnabled(true);
  m_grid->setDragDropMode(QAbstractItemView::DragOnly);
  m_grid->setStyleSheet(QStringLiteral("QListView { background: #101428; }"));
  m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_grid->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](const QModelIndex &idx) { showTexture(idx.data(RefRole).toString()); });
  connect(m_grid, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
    const QModelIndex idx = m_grid->indexAt(pos);
    if (!idx.isValid())
      return;
    QMenu menu(this);
    populateTextureMenu(menu, m_engine, idx.data(RefRole).toString(), this);
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
  });
  v->addWidget(m_grid, 1);

  v->addWidget(section(tr("Being edited in the image editor"), w));
  m_sessions = new QListWidget(w);
  m_sessions->setObjectName(QStringLiteral("dlgList"));
  scale::bind(m_sessions, [=, this] { m_sessions->setMaximumHeight(scale::tpx(130)); });
  m_sessions->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_sessions, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    QListWidgetItem *it = m_sessions->itemAt(pos);
    if (!it)
      return;
    const QString ref = it->data(Qt::UserRole).toString();
    const QString map = it->data(Qt::UserRole + 1).toString();
    const QString path = it->data(Qt::UserRole + 2).toString();
    QMenu menu(this);
    menu.addAction(tr("Open in the image editor again"), this,
                   [ref, map, this] { net::TextureEditSessions::instance().start(ref, map, this); });
    menu.addAction(tr("Show the working file"), this,
                   [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
    menu.addSeparator();
    menu.addAction(ActionManager::icon(QStringLiteral("x")), tr("Stop editing (release the lock)"), this,
                   [ref, map] { net::TextureEditSessions::instance().stop(ref, map); });
    menu.exec(m_sessions->viewport()->mapToGlobal(pos));
  });
  v->addWidget(m_sessions);
  return w;
}

QWidget *TexturePacksWindow::buildEditor() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scale::bind(scroll, [=, this] { scroll->setMinimumWidth(scale::tpx(400)); });
  auto *outer = new QWidget(scroll);
  outer->setObjectName(QStringLiteral("tpEditor"));
  auto *ov = new QVBoxLayout(outer);
  ov->setContentsMargins(16, 14, 16, 14);
  m_editorEmpty = new QLabel(tr("No texture selected"), outer);
  m_editorEmpty->setObjectName(QStringLiteral("dlgDesc"));
  m_editorEmpty->setWordWrap(true);
  m_editorEmpty->setAlignment(Qt::AlignCenter);
  ov->addWidget(m_editorEmpty);

  m_editor = new QWidget(outer);
  auto *v = new QVBoxLayout(m_editor);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(10);
  m_title = new QLabel(m_editor);
  m_title->setObjectName(QStringLiteral("dlgTitle"));
  m_subtitle = new QLabel(m_editor);
  m_subtitle->setObjectName(QStringLiteral("dlgSubtitle"));
  m_subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_subtitle->setWordWrap(true);
  m_lockNote = new QLabel(m_editor);
  m_lockNote->setWordWrap(true);
  v->addWidget(m_title);
  v->addWidget(m_subtitle);
  v->addWidget(m_lockNote);

  auto *nameRow = new QHBoxLayout();
  m_nameEdit = new QLineEdit(m_editor);
  m_renameButton = flatButton(tr("Rename"), QStringLiteral("greasepencil"), m_editor);
  connect(m_renameButton, &QPushButton::clicked, this, &TexturePacksWindow::renameTexture);
  connect(m_nameEdit, &QLineEdit::returnPressed, this, &TexturePacksWindow::renameTexture);
  nameRow->addWidget(m_nameEdit, 1);
  nameRow->addWidget(m_renameButton);
  v->addLayout(nameRow);

  v->addWidget(section(tr("Tags"), m_editor));
  m_tagBox = new QWidget(m_editor);
  m_tagFlow = new FlowLayout(m_tagBox, 5, 5);
  v->addWidget(m_tagBox);
  m_tagAdd = new QLineEdit(m_editor);
  m_tagAdd->setPlaceholderText(tr("New tag"));
  connect(m_tagAdd, &QLineEdit::returnPressed, this, [this] {
    const TextureEntry *e = net::TextureCatalog::instance().find(m_ref);
    const QString t = m_tagAdd->text().trimmed().toLower();
    if (!e || t.isEmpty())
      return;
    QStringList tags = e->tags;
    if (!tags.contains(t))
      tags.append(t);
    m_tagAdd->clear();
    saveTags(tags);
  });
  v->addWidget(m_tagAdd);

  v->addWidget(section(tr("Maps"), m_editor));
  m_mapHolder = new QWidget(m_editor);
  m_mapGrid = new QGridLayout(m_mapHolder);
  m_mapGrid->setContentsMargins(0, 0, 0, 0);
  m_mapGrid->setHorizontalSpacing(8);
  m_mapGrid->setVerticalSpacing(6);
  v->addWidget(m_mapHolder);

  m_derived = new QLabel(m_editor);
  m_derived->setObjectName(QStringLiteral("dlgDesc"));
  m_derived->setWordWrap(true);
  v->addWidget(m_derived);

  auto *row = new QHBoxLayout();
  m_copyButton = flatButton(tr("Copy into pack…"), QStringLiteral("duplicate"), m_editor);
  connect(m_copyButton, &QPushButton::clicked, this, [this] { emit TextureHub::instance().copyRequested(m_ref); });
  m_deleteButton = flatButton(tr("Delete texture…"), QStringLiteral("x"), m_editor);
  connect(m_deleteButton, &QPushButton::clicked, this, &TexturePacksWindow::deleteTexture);
  row->addWidget(m_copyButton);
  row->addWidget(m_deleteButton);
  row->addStretch(1);
  v->addLayout(row);
  v->addStretch(1);
  ov->addWidget(m_editor);
  ov->addStretch(1);
  scroll->setWidget(outer);
  return scroll;
}

void TexturePacksWindow::rebuildPacks() {
  QSignalBlocker b(m_packs);
  m_packs->clear();
  const QString project = m_engine ? m_engine->projectPack() : QString();
  int row = -1;
  // user packs first (the catalogue's order)
  for (const auto &p: net::TextureCatalog::instance().packs()) {
    const bool isProject = p.name.compare(project, Qt::CaseInsensitive) == 0;
    auto *it = new QListWidgetItem(
        ActionManager::icon(isProject ? QStringLiteral("pinned") : (p.base ? QStringLiteral("locked") : QStringLiteral("unlocked"))),
        p.name);
    it->setData(Qt::UserRole, p.name);
    it->setData(Qt::UserRole + 1, QLocale().toString(p.count));
    if (p.base)
      it->setForeground(QColor(140, 150, 178));
    it->setToolTip(QStringLiteral("%1<br>%2").arg(p.description.toHtmlEscaped(),
                                                  p.base ? tr("Base pack: read-only") : tr("Editable pack")));
    m_packs->addItem(it);
    if (p.name.compare(m_pack, Qt::CaseInsensitive) == 0)
      row = m_packs->count() - 1;
  }
  if (row >= 0)
    m_packs->setCurrentRow(row);
  else if (m_packs->count() && m_pack.isEmpty())
    QTimer::singleShot(0, this, [this] {
      if (m_packs->count() && m_pack.isEmpty())  // open(ref) may have picked one meanwhile
        m_packs->setCurrentRow(0);
    });
}

void TexturePacksWindow::rebuildProjectCombo() {
  QSignalBlocker b(m_project);
  m_project->clear();
  m_project->addItem(tr("(none)"), QString());
  const QString project = m_engine ? m_engine->projectPack() : QString();
  for (const auto &p: net::TextureCatalog::instance().packs())
    if (!p.base)
      m_project->addItem(ActionManager::icon(QStringLiteral("unlocked")), p.name, p.name);
  m_project->setCurrentIndex(std::max(0, m_project->findData(project)));
}

void TexturePacksWindow::selectPack(const QString &pack) {
  if (pack.isEmpty())
    return;
  m_pack = pack;
  const net::PackInfo *p = net::TextureCatalog::instance().pack(pack);
  const bool editable = p && !p->base;
  m_renamePack->setEnabled(editable);
  m_deletePack->setEnabled(editable);
  m_packInfo->setText(!p ? QString()
                      : p->base ? tr("%1 is a base pack: its textures are never changed. Copy one into a user pack "
                                     "to change it.")
                                      .arg(p->name)
                                : tr("%1 · editable · %n texture(s)", nullptr, p->count).arg(p->name));
  for (int i = 0; i < m_packs->count(); ++i)
    if (m_packs->item(i)->data(Qt::UserRole).toString().compare(pack, Qt::CaseInsensitive) == 0 &&
        m_packs->currentRow() != i) {
      QSignalBlocker b(m_packs);
      m_packs->setCurrentRow(i);
    }
  QJsonObject args;
  args.insert(QStringLiteral("pack"), pack);
  args.insert(QStringLiteral("q"), m_search->text().trimmed());
  m_model->setQuery(args);
}

void TexturePacksWindow::newPack() {
  bool ok = false;
  const QString name =
      QInputDialog::getText(this, tr("New texture pack"), tr("Name (letters, digits, '_' and '-'):"), QLineEdit::Normal,
                            QString(), &ok)
          .trimmed();
  if (!ok || name.isEmpty())
    return;
  if (!isValidAssetName(name)) {
    report(tr("'%1' is not a valid pack name.").arg(name), true);
    return;
  }
  const QString desc =
      QInputDialog::getText(this, tr("New texture pack"), tr("Description (optional):"), QLineEdit::Normal, QString(), &ok);
  QPointer<TexturePacksWindow> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("packs.create"),
                                       QJsonObject{{"name", name}, {"description", desc.trimmed()}}, this,
                                       [self, name](const net::Result &r) {
                                         if (!self)
                                           return;
                                         if (!r.ok) {
                                           self->report(r.message, true);
                                           return;
                                         }
                                         self->m_pack = name;
                                         self->report(tr("Created the pack %1.").arg(name));
                                       });
}

void TexturePacksWindow::renamePack() {
  const net::PackInfo *p = net::TextureCatalog::instance().pack(m_pack);
  if (!p || p->base)
    return;
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("Rename pack"),
                                             tr("New name for %1:\n(maps that use its textures keep working: the old "
                                                "references are redirected)")
                                                 .arg(p->name),
                                             QLineEdit::Normal, p->name, &ok)
                           .trimmed();
  if (!ok || name.isEmpty() || name == p->name)
    return;
  QPointer<TexturePacksWindow> self(this);
  const QString old = p->name;
  net::TextureCatalog::instance().call(
      QStringLiteral("packs.update"),
      QJsonObject{{"name", old}, {"new_name", name}, {"if_rev", double(p->rev)}}, this,
      [self, old, name](const net::Result &r) {
        if (!self)
          return;
        if (!r.ok) {
          self->report(r.code == QLatin1String("conflict") ? tr("Someone changed the pack meanwhile — try again.")
                                                           : r.message,
                       true);
          return;
        }
        if (self->m_engine && self->m_engine->projectPack().compare(old, Qt::CaseInsensitive) == 0)
          self->m_engine->setProjectPack(name);
        self->m_pack = name;
        self->report(tr("Renamed %1 to %2.").arg(old, name));
      });
}

void TexturePacksWindow::deletePack() {
  const net::PackInfo *p = net::TextureCatalog::instance().pack(m_pack);
  if (!p || p->base)
    return;
  const auto answer = QMessageBox::warning(
      this, tr("Delete pack"),
      tr("Delete the pack %1 with its %n texture(s)?\n\nThe files are moved to the server's trash folder, not erased. "
         "Maps that use them show the missing-texture checkerboard.",
         nullptr, p->count)
          .arg(p->name),
      QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
  if (answer != QMessageBox::Yes)
    return;
  QPointer<TexturePacksWindow> self(this);
  const QString name = p->name;
  net::TextureCatalog::instance().call(QStringLiteral("packs.delete"),
                                       QJsonObject{{"name", name}, {"if_rev", double(p->rev)}}, this,
                                       [self, name](const net::Result &r) {
                                         if (!self)
                                           return;
                                         if (!r.ok) {
                                           self->report(r.message, true);
                                           return;
                                         }
                                         self->m_pack.clear();
                                         self->showTexture({});
                                         self->report(tr("Moved the pack %1 to the server's trash.").arg(name));
                                       });
}

void TexturePacksWindow::showTexture(const QString &ref) {
  m_ref = MaterialRef::parse(ref).base();
  if (!m_ref.isEmpty())
    net::TextureCatalog::instance().resolve({m_ref}, this, [this] { refreshEditor(); });
  refreshEditor();
}

void TexturePacksWindow::refreshEditor() {
  const TextureEntry *e = m_ref.isEmpty() ? nullptr : net::TextureCatalog::instance().find(m_ref);
  m_editorEmpty->setVisible(!e);
  m_editor->setVisible(e);
  if (!e)
    return;
  const bool editable = !e->base;
  m_title->setText(e->name);
  m_subtitle->setText(QStringLiteral("%1 · %2").arg(e->ref, sizeText({e->w, e->h})));
  if (const net::LockInfo *l = net::TextureCatalog::instance().lockFor(e->ref)) {
    m_lockNote->setObjectName(QStringLiteral("tpLock"));
    m_lockNote->setText(l->clientId == net::AssetClient::instance().clientId()
                            ? tr("✎ You are editing this texture (%1)").arg(l->purpose)
                            : tr("✎ %1 is editing this texture (%2)").arg(l->name, l->purpose));
    m_lockNote->show();
  } else if (!editable) {
    m_lockNote->setObjectName(QStringLiteral("tpBase"));
    m_lockNote->setText(tr("Base pack (read-only)"));
    m_lockNote->show();
  } else {
    m_lockNote->hide();
  }
  m_lockNote->style()->unpolish(m_lockNote);
  m_lockNote->style()->polish(m_lockNote);

  m_nameEdit->setText(e->name);
  m_nameEdit->setEnabled(editable);
  m_renameButton->setEnabled(editable);
  m_tagAdd->setVisible(editable);
  m_deleteButton->setVisible(editable);
  // Only base (read-only) textures are copied.
  m_copyButton->setVisible(e->base);

  m_tagFlow->clear();
  const QFont chipFont = scale::font(scale::Font::Caption, font());
  for (const QString &t: e->tags) {
    auto *chip = new TagChip(t, -1, m_tagBox);
    chip->setFont(chipFont);
    if (editable) {
      chip->setRemovable(true);
      connect(chip, &TagChip::removeRequested, this, [this](const QString &tag) {
        if (const TextureEntry *cur = net::TextureCatalog::instance().find(m_ref)) {
          QStringList tags = cur->tags;
          tags.removeAll(tag);
          saveTags(tags);
        }
      });
    } else {
      chip->setEnabled(false);
    }
    m_tagFlow->addWidget(chip);
  }
  if (e->tags.isEmpty()) {
    auto *none = new QLabel(tr("no tags"), m_tagBox);
    none->setObjectName(QStringLiteral("dlgDesc"));
    m_tagFlow->addWidget(none);
  }

  // one slot per map type
  while (QLayoutItem *it = m_mapGrid->takeAt(0)) {
    if (it->widget())
      it->widget()->deleteLater();
    delete it;
  }
  int row = 0;
  for (int t = 0; t < kMapTypeCount; ++t) {
    const MapType type = MapType(t);
    const MapFile *f = e->map(type);
    if (!f && !editable)
      continue;
    auto *thumb = new QLabel(m_mapHolder);
    const int edge = scale::px(48);
    thumb->setFixedSize(edge, edge);
    if (f)
      thumb->setPixmap(rounded(ThumbCache::instance().server(f->thumb), edge));
    else
      thumb->setPixmap(rounded(QPixmap(), edge));
    auto *label = new QLabel(m_mapHolder);
    label->setTextFormat(Qt::RichText);
    QString text = QStringLiteral("<b>%1</b>").arg(mapTypeLabel(type));
    if (f)
      text += QStringLiteral("<br><span style='color:#8a96b4'>%1%2</span>")
                  .arg(sizeText({f->w, f->h}), mapTypeRendered(type) ? QString() : tr(" · not shown in viewport"));
    else
      text += tr("<br><span style='color:#5a6382'>missing</span>");
    label->setText(text);
    label->setToolTip(f ? f->file : tr("This texture has no %1 map").arg(mapTypeLabel(type)));
    m_mapGrid->addWidget(thumb, row, 0);
    m_mapGrid->addWidget(label, row, 1);
    auto *buttons = new QWidget(m_mapHolder);
    auto *bh = new QHBoxLayout(buttons);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(4);
    const QString key = QLatin1String(mapTypeKey(type));
    if (f) {
      if (editable) {
        auto *edit = toolButton(QStringLiteral("greasepencil"), tr("Edit in the image editor (saving uploads it)"), buttons);
        connect(edit, &QToolButton::clicked, this,
                [this, key] { net::TextureEditSessions::instance().start(m_ref, key, this); });
        bh->addWidget(edit);
        auto *replace = toolButton(QStringLiteral("file_refresh"), tr("Replace from a file…"), buttons);
        connect(replace, &QToolButton::clicked, this, [this, key] {
          const QString file = FileDialog::getOpenFileName(this, tr("Replace map"), QString(),
                                                           tr("Images (*.png *.jpg *.jpeg)"), tr("Replace"));
          if (!file.isEmpty())
            net::TextureEditSessions::instance().uploadFile(m_ref, key, file, this);
        });
        bh->addWidget(replace);
      }
      auto *download = toolButton(QStringLiteral("import"), tr("Download the original…"), buttons);
      connect(download, &QToolButton::clicked, this, [this, key, file = f->file] {
        const QString dest = FileDialog::getSaveFileName(
            this, tr("Save original"),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + QLatin1Char('/') + file);
        if (dest.isEmpty())
          return;
        const MaterialRef r = MaterialRef::parse(m_ref);
        QPointer<TexturePacksWindow> self(this);
        net::AssetClient::instance().httpGet(QStringLiteral("/api/2/source/%1/%2/%3").arg(r.pack, r.name, key), this,
                                             [self, dest](QNetworkReply *reply) {
                                               if (!self)
                                                 return;
                                               QSaveFile out(dest);
                                               if (reply->error() == QNetworkReply::NoError &&
                                                   out.open(QIODevice::WriteOnly) && out.write(reply->readAll()) >= 0 &&
                                                   out.commit())
                                                 self->report(tr("Saved %1").arg(dest));
                                               else
                                                 self->report(tr("Download failed: %1").arg(reply->errorString()), true);
                                             });
      });
      bh->addWidget(download);
    } else {
      auto *add = toolButton(QStringLiteral("add"), tr("Add a %1 map from a file…").arg(mapTypeLabel(type)), buttons);
      connect(add, &QToolButton::clicked, this, [this, key] {
        const QString file = FileDialog::getOpenFileName(this, tr("Add map"), QString(), tr("Images (*.png *.jpg *.jpeg)"), tr("Add"));
        if (!file.isEmpty())
          net::TextureEditSessions::instance().uploadFile(m_ref, key, file, this);
      });
      bh->addWidget(add);
    }
    bh->addStretch(1);
    m_mapGrid->addWidget(buttons, row, 2);
    ++row;
  }
  m_mapGrid->setColumnStretch(1, 1);
  m_derived->setText(e->derivedFrom.isEmpty() ? QString() : tr("Copied from %1").arg(e->derivedFrom));
}

void TexturePacksWindow::renameTexture() {
  const TextureEntry *e = net::TextureCatalog::instance().find(m_ref);
  const QString name = m_nameEdit->text().trimmed();
  if (!e || e->base || name.isEmpty() || name == e->name)
    return;
  if (!isValidAssetName(name)) {
    report(tr("Texture names use letters, digits, '_' and '-'."), true);
    return;
  }
  QPointer<TexturePacksWindow> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("textures.update"),
                                       QJsonObject{{"ref", e->ref}, {"name", name}, {"if_rev", double(e->rev)}}, this,
                                       [self, name](const net::Result &r) {
                                         if (!self)
                                           return;
                                         if (!r.ok)
                                           self->report(r.code == QLatin1String("conflict")
                                                            ? tr("Someone changed this texture meanwhile — try again.")
                                                            : r.message,
                                                        true);
                                         else
                                           self->report(tr("Renamed to %1. Faces using it follow automatically.").arg(name));
                                       });
}

void TexturePacksWindow::saveTags(const QStringList &tags) {
  const TextureEntry *e = net::TextureCatalog::instance().find(m_ref);
  if (!e || e->base)
    return;
  QPointer<TexturePacksWindow> self(this);
  net::TextureCatalog::instance().call(
      QStringLiteral("textures.update"),
      QJsonObject{{"ref", e->ref}, {"tags", QJsonArray::fromStringList(tags)}, {"if_rev", double(e->rev)}}, this,
      [self](const net::Result &r) {
        if (self && !r.ok)
          self->report(r.message, true);
      });
}

void TexturePacksWindow::deleteTexture() {
  const TextureEntry *e = net::TextureCatalog::instance().find(m_ref);
  if (!e || e->base)
    return;
  const int faces = m_engine ? m_engine->facesUsingMaterial(e->ref, false) : 0;
  const auto answer = QMessageBox::warning(
      this, tr("Delete texture"),
      tr("Delete %1?\n\nIts files move to the server's trash folder (not erased).%2")
          .arg(e->ref, faces ? tr("\n%n face(s) in this map use it and will show the missing checkerboard.", nullptr, faces)
                             : QString()),
      QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
  if (answer != QMessageBox::Yes)
    return;
  QPointer<TexturePacksWindow> self(this);
  const QString ref = e->ref;
  net::TextureCatalog::instance().call(QStringLiteral("textures.delete"),
                                       QJsonObject{{"ref", ref}, {"if_rev", double(e->rev)}}, this,
                                       [self, ref](const net::Result &r) {
                                         if (!self)
                                           return;
                                         if (!r.ok)
                                           self->report(r.message, true);
                                         else
                                           self->report(tr("Moved %1 to the server's trash.").arg(ref));
                                       });
}

void TexturePacksWindow::rebuildSessions() {
  m_sessions->clear();
  const auto sessions = net::TextureEditSessions::instance().sessions();
  for (const auto &s: sessions) {
    auto *it = new QListWidgetItem(ActionManager::icon(QStringLiteral("greasepencil")),
                                   QStringLiteral("%1 · %2 — %3%4")
                                       .arg(s.ref, s.mapType, s.state,
                                            s.lastSync.isValid() ? tr(", %1").arg(s.lastSync.toString(QStringLiteral("HH:mm:ss")))
                                                                 : QString()));
    it->setData(Qt::UserRole, s.ref);
    it->setData(Qt::UserRole + 1, s.mapType);
    it->setData(Qt::UserRole + 2, s.path);
    it->setToolTip(s.path);
    m_sessions->addItem(it);
  }
  if (sessions.isEmpty()) {
    auto *it = new QListWidgetItem(tr("Nothing being edited"));
    it->setFlags(Qt::NoItemFlags);
    m_sessions->addItem(it);
  }
}

void TexturePacksWindow::report(const QString &text, bool error) {
  m_status->setStyleSheet(error ? QStringLiteral("color:#ff8f8f;") : QStringLiteral("color:#9fb0d8;"));
  m_status->setText(text);
}

void TexturePacksWindow::saveWindowState() {
  config()->setRaw(QStringLiteral("TexturePacks/Geometry"), QString::fromLatin1(saveGeometry().toBase64()));
  config()->setRaw(QStringLiteral("TexturePacks/Splitter"), QString::fromLatin1(m_split->saveState().toBase64()));
}

void TexturePacksWindow::closeEvent(QCloseEvent *e) {
  saveWindowState();
  QMainWindow::closeEvent(e);
}

} // namespace ui
