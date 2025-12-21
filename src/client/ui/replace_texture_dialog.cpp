#include "replace_texture_dialog.h"

#include <algorithm>
#include <cmath>

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/scene.h"
#include "client/net/texture_catalog.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/dialog_style.h"
#include "client/ui/flow_layout.h"
#include "client/ui/material_preview_3d.h"
#include "client/ui/scale.h"
#include "client/ui/tag_picker.h"
#include "client/ui/texture_ui.h"
#include "client/vulkan/vk_engine.h"
#include "shared/models/material_props.h"

namespace ui {

namespace {

QPointer<ReplaceTextureWindow> s_window;

const char *kExtraStyle = R"(
QWidget#rtSide { background: #0f1326; }
QWidget#rtCenter { background: #101428; }
QLineEdit#rtSearch { padding: 6px 10px; border-radius: 6px; border: 1px solid #2a3360; background: #0c1022; }
QLineEdit#rtSearch:focus { border-color: #4866C5; }
QLabel#rtPreview { background: #0c1022; border: 1px solid #232A4E; border-radius: 8px; }
QToolButton#rtSeg { background: #0f1328; border: 1px solid #2a3360; border-left: none; padding: 3px 9px; }
QToolButton#rtSeg[pos="first"] { border-left: 1px solid #2a3360; border-top-left-radius: 5px; border-bottom-left-radius: 5px; }
QToolButton#rtSeg[pos="last"] { border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
QToolButton#rtSeg:hover { background: #171C38; }
QToolButton#rtSeg:checked { background: #4866C5; border-color: #6A84D9; color: white; }
)";

// Rounded thumbnail, or the placeholder while it loads.
QPixmap roundedThumb(const QString &ref, const int edge) {
  QPixmap shown(edge, edge);
  shown.fill(Qt::transparent);
  QPainter p(&shown);
  p.setRenderHint(QPainter::Antialiasing);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, edge, edge), scale::px(8), scale::px(8));
  p.setClipPath(clip);
  const TextureEntry *e = ref.isEmpty() ? nullptr : net::TextureCatalog::instance().find(MaterialRef::parse(ref).base());
  const QPixmap pm = e ? ThumbCache::instance().server(e->thumbFile()) : QPixmap();
  if (!pm.isNull())
    p.drawPixmap(0, 0, pm.scaled(edge, edge, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
  else
    TextureCardDelegate::paintPlaceholder(&p, QRect(0, 0, edge, edge));
  return shown;
}

} // namespace

ReplaceTextureWindow *ReplaceTextureWindow::open(vk::Engine3D *engine, QWidget *owner) {
  if (!s_window)
    s_window = new ReplaceTextureWindow(engine, owner);
  else
    s_window->refreshFromList();
  if (s_window->isMinimized())
    s_window->showNormal();
  s_window->show();
  s_window->raise();
  s_window->activateWindow();
  s_window->m_search->setFocus();
  return s_window;
}

ReplaceTextureWindow::ReplaceTextureWindow(vk::Engine3D *engine, QWidget *owner)
    : QWidget(nullptr, Qt::Window), m_engine(engine) {
  setObjectName(QStringLiteral("dlgPage"));
  setWindowTitle(tr("Replace Texture"));
  setAttribute(Qt::WA_QuitOnClose, false);
  if (owner)
    connect(owner, &QObject::destroyed, this, &QObject::deleteLater);
  scale::setScaledStyleSheet(this, dialogStyle() + QString::fromLatin1(kExtraStyle));
  resize(scale::initialSize(this, {1460, 880}));

  m_model = new ServerTextureModel(ServerTextureModel::Mode::Append, this);
  m_delegate = new TextureCardDelegate(this);
  m_searchTimer = new QTimer(this);
  m_searchTimer->setSingleShot(true);
  m_searchTimer->setInterval(250);
  connect(m_searchTimer, &QTimer::timeout, this, &ReplaceTextureWindow::applyQuery);
  m_followTimer = new QTimer(this);
  m_followTimer->setSingleShot(true);
  m_followTimer->setInterval(80);
  connect(m_followTimer, &QTimer::timeout, this, &ReplaceTextureWindow::refreshFromList);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto *body = new QHBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);
  body->addWidget(buildLeft());
  body->addWidget(buildCenter(), 1);
  outer->addLayout(body, 1);

  auto *footer = new QWidget(this);
  footer->setObjectName(QStringLiteral("dlgFooter"));
  auto *fh = new QHBoxLayout(footer);
  fh->setContentsMargins(20, 10, 20, 10);
  fh->addStretch(1);
  auto *close = new QPushButton(tr("Close"), footer);
  close->setObjectName(QStringLiteral("dlgFlat"));
  connect(close, &QPushButton::clicked, this, &QWidget::close);
  fh->addWidget(close);
  outer->addWidget(footer);

  connect(m_model, &ServerTextureModel::resultChanged, this,
          [this] { m_tagPicker->setResult(m_model->facets(), m_model->distinctTags(), m_model->query()); });
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, [this](const QString &) {
    refreshFrom();
    layoutPreview();
  });
  if (m_engine) {
    connect(m_engine, &vk::Engine3D::selectionChanged, m_followTimer, qOverload<>(&QTimer::start));
    connect(m_engine, &vk::Engine3D::undoRedoChanged, m_followTimer, qOverload<>(&QTimer::start));
  }

  m_tagPicker->refreshPopular();
  refreshFromList();
  refreshPick();
  applyQuery();
}

QWidget *ReplaceTextureWindow::buildLeft() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scale::bind(scroll, [scroll] { scroll->setFixedWidth(scale::tpx(290)); });
  auto *w = new QWidget(scroll);
  w->setObjectName(QStringLiteral("rtSide"));
  m_side = w;
  w->installEventFilter(this);  // the preview follows its width
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);

  v->addWidget(sectionLabel(tr("Current texture"), w));
  m_from = new QComboBox(w);
  m_from->setToolTip(tr("The selection uses more than one texture: pick the one to replace"));
  v->addWidget(m_from);
  m_fromThumb = new QLabel(w);
  m_fromThumb->setAlignment(Qt::AlignCenter);
  scale::bind(m_fromThumb, [this] { m_fromThumb->setFixedSize(scale::px(160), scale::px(160)); });
  v->addWidget(m_fromThumb, 0, Qt::AlignHCenter);
  m_fromName = new QLabel(w);
  m_fromName->setWordWrap(true);
  m_fromName->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(m_fromName);
  m_fromCount = new QLabel(w);
  m_fromCount->setObjectName(QStringLiteral("dlgDesc"));
  m_fromCount->setWordWrap(true);
  v->addWidget(m_fromCount);

  v->addSpacing(6);
  buildPick(w, v);

  v->addSpacing(6);
  v->addWidget(sectionLabel(tr("Replace on"), w));
  m_wholeMap = new QRadioButton(tr("Every face in the map"), w);
  m_selOnly = new QRadioButton(tr("The selected faces only"), w);
  m_wholeMap->setChecked(true);
  auto *scopeGroup = new QButtonGroup(w);
  scopeGroup->addButton(m_wholeMap);
  scopeGroup->addButton(m_selOnly);
  v->addWidget(m_wholeMap);
  v->addWidget(m_selOnly);
  connect(m_wholeMap, &QRadioButton::toggled, this, &ReplaceTextureWindow::refreshFrom);
  connect(m_from, &QComboBox::currentIndexChanged, this, [this] {
    refreshFrom();
    refreshPick();
  });
  m_replace = new QPushButton(ActionManager::icon(QStringLiteral("file_refresh")), tr("Replace"), w);
  m_replace->setObjectName(QStringLiteral("dlgPrimary"));
  m_replace->setToolTip(tr("Put the picked texture on the faces using the current one (one undo step)"));
  connect(m_replace, &QPushButton::clicked, this, [this] { replaceWith(m_pick); });
  v->addWidget(m_replace);

  v->addSpacing(6);
  m_tagPicker = new TagPicker(w);
  connect(m_tagPicker, &TagPicker::changed, this, &ReplaceTextureWindow::applyQuery);
  v->addWidget(m_tagPicker);
  v->addStretch(1);
  scroll->setWidget(w);
  return scroll;
}

QWidget *ReplaceTextureWindow::buildCenter() {
  auto *w = new QWidget(this);
  w->setObjectName(QStringLiteral("rtCenter"));
  auto *v = new QVBoxLayout(w);
  v->setContentsMargins(16, 14, 16, 10);
  v->setSpacing(10);

  auto *top = new QHBoxLayout();
  top->setSpacing(8);
  m_search = new QLineEdit(w);
  m_search->setObjectName(QStringLiteral("rtSearch"));
  m_search->setPlaceholderText(tr("Search textures…  (pack/Name searches one pack)"));
  m_search->setClearButtonEnabled(true);
  m_search->addAction(ActionManager::icon(QStringLiteral("viewzoom")), QLineEdit::LeadingPosition);
  connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
  top->addWidget(m_search, 1);
  m_shuffle = new ShuffleButton(w);
  connect(m_shuffle, &ShuffleButton::orderChanged, this, &ReplaceTextureWindow::applyQuery);
  top->addWidget(m_shuffle);
  // Card size, shared with the Texture Browser.
  auto *sizeIcon = new QLabel(w);
  sizeIcon->setPixmap(ActionManager::icon(QStringLiteral("image_data")).pixmap(16, 16));
  top->addWidget(sizeIcon);
  m_thumbSize = new QSlider(Qt::Horizontal, w);
  m_thumbSize->setRange(80, 256);
  scale::bind(m_thumbSize, [this] { m_thumbSize->setFixedWidth(scale::tpx(120)); });
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

  m_chipBox = new QWidget(w);
  m_chipFlow = new FlowLayout(m_chipBox, 6, 6);
  v->addWidget(m_chipBox);

  m_grid = makeTextureGrid(m_model, m_delegate, w, false, 4);
  m_grid->setFrameShape(QFrame::NoFrame);
  m_grid->setStyleSheet(QStringLiteral("QListView { background: #0c1022; border-radius: 8px; }"));
  m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(m_grid, 1);

  // A click only picks.
  connect(m_grid, &QListView::clicked, this, [this](const QModelIndex &idx) {
    m_pick = idx.data(RefRole).toString();
    refreshPick();
  });
  connect(m_grid, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    const QModelIndex idx = m_grid->indexAt(pos);
    if (!idx.isValid())
      return;
    const QString ref = idx.data(RefRole).toString();
    m_grid->setCurrentIndex(idx);
    m_pick = ref;
    refreshPick();
    QMenu menu(this);
    QAction *apply = menu.addAction(ActionManager::icon(QStringLiteral("file_refresh")), tr("Replace with this"), this,
                                    [this, ref] { replaceWith(ref); });
    apply->setEnabled(!fromRef().isEmpty() && ref.compare(fromRef(), Qt::CaseInsensitive) != 0);
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
  });
  return w;
}

void ReplaceTextureWindow::buildPick(QWidget *side, QVBoxLayout *v) {
  v->addWidget(sectionLabel(tr("New texture"), side));

  auto *modes = new QHBoxLayout();
  modes->setSpacing(0);
  m_previewModes = new QButtonGroup(side);
  m_previewModes->setExclusive(true);
  const QStringList labels {tr("2D"), tr("Cube"), tr("Sphere")};
  for (int i = 0; i < labels.size(); ++i) {
    auto *b = new QToolButton(side);
    b->setObjectName(QStringLiteral("rtSeg"));
    b->setProperty("pos", i == 0 ? QStringLiteral("first") : i == labels.size() - 1 ? QStringLiteral("last")
                                                                                     : QStringLiteral("mid"));
    b->setText(labels[i]);
    b->setCheckable(true);
    b->setChecked(i == 0);
    m_previewModes->addButton(b, i);
    modes->addWidget(b);
  }
  modes->addStretch(1);
  connect(m_previewModes, &QButtonGroup::idClicked, this,
          [this](int id) { setPreviewMode(PreviewMode(id)); });
  v->addLayout(modes);

  m_previewStack = new QStackedWidget(side);
  m_preview = new QLabel(m_previewStack);
  m_preview->setObjectName(QStringLiteral("rtPreview"));
  m_preview->setAlignment(Qt::AlignCenter);
  m_previewStack->addWidget(m_preview);
  m_preview3d = new MaterialPreview3D(m_previewStack);
  m_previewStack->addWidget(m_preview3d);
  v->addWidget(m_previewStack);

  m_pickName = new QLabel(side);
  m_pickName->setWordWrap(true);
  v->addWidget(m_pickName);
  m_pickRef = new QLabel(side);
  m_pickRef->setObjectName(QStringLiteral("dlgDesc"));
  m_pickRef->setWordWrap(true);
  m_pickRef->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(m_pickRef);
}

bool ReplaceTextureWindow::eventFilter(QObject *obj, QEvent *e) {
  if (obj == m_side && e->type() == QEvent::Resize) {
    const auto *re = static_cast<QResizeEvent *>(e);
    if (re->size().width() != re->oldSize().width())
      layoutPreview();
  }
  return QWidget::eventFilter(obj, e);
}

QString ReplaceTextureWindow::fromRef() const { return m_from ? m_from->currentData().toString() : QString(); }

bool ReplaceTextureWindow::selectionOnly() const { return m_selOnly && m_selOnly->isChecked(); }

void ReplaceTextureWindow::applyQuery() {
  QJsonObject args = serverQuery(m_search->text(), {}, m_tagPicker->tags(), m_shuffle);
  if (!m_tagPicker->notTags().isEmpty())
    args.insert(QStringLiteral("not_tags"), QJsonArray::fromStringList(m_tagPicker->notTags()));
  args.insert(QStringLiteral("facets"), true);
  args.insert(QStringLiteral("facet_limit"), 200);
  m_model->setQuery(args);
  m_grid->scrollToTop();
  rebuildChips();
}

void ReplaceTextureWindow::refreshFromList() {
  const QStringList refs = m_engine ? m_engine->selectionMaterialRefs() : QStringList();
  const QString keep = fromRef();
  {
    QSignalBlocker b(m_from);
    m_from->clear();
    for (const QString &ref: refs)
      m_from->addItem(ref, ref);
    const int at = m_from->findData(keep);
    m_from->setCurrentIndex(at >= 0 ? at : 0);
  }
  m_from->setVisible(m_from->count() > 1);
  refreshFrom();
  refreshPick();
}

void ReplaceTextureWindow::refreshFrom() {
  const QString ref = fromRef();
  m_delegate->setCurrent(ref);
  m_grid->viewport()->update();
  m_fromThumb->setPixmap(roundedThumb(ref, scale::px(160)));
  if (ref.isEmpty()) {
    m_fromName->setText(tr("<b>No faces selected</b>"));
    m_fromCount->setText(tr("No faces selected"));
    return;
  }
  m_fromName->setText(QStringLiteral("<b>%1</b>").arg(ref.toHtmlEscaped()));
  const int n = m_engine ? m_engine->facesUsingMaterial(ref, selectionOnly()) : 0;
  m_fromCount->setText(n == 1 ? tr("Used by 1 face") : tr("Used by %1 faces").arg(n));
}

void ReplaceTextureWindow::refreshPick() {
  const TextureEntry *e = m_pick.isEmpty() ? nullptr : net::TextureCatalog::instance().find(MaterialRef::parse(m_pick).base());
  const bool same = !m_pick.isEmpty() && m_pick.compare(fromRef(), Qt::CaseInsensitive) == 0;
  m_previewStack->setVisible(e);
  m_pickName->setText(e ? QStringLiteral("<b>%1</b>").arg(e->name.toHtmlEscaped()) : tr("No texture picked"));
  m_pickRef->setText(!e ? QString() : same ? tr("%1 · the current texture").arg(m_pick) : m_pick);
  m_replace->setEnabled(e && !same && !fromRef().isEmpty());
  if (!e)
    return;
  layoutPreview();
  if (m_previewMode != PreviewMode::Flat && m_engine && m_engine->scene()) {
    const eng::MaterialOverrides &ov = m_engine->scene()->materialOverrides;
    m_preview3d->showTexture(*e, ov.resolve(e->ref, e).props);
  }
}

void ReplaceTextureWindow::layoutPreview() {
  const TextureEntry *e = m_pick.isEmpty() ? nullptr : net::TextureCatalog::instance().find(MaterialRef::parse(m_pick).base());
  if (!e)
    return;
  const QMargins mg = m_side->layout() ? m_side->layout()->contentsMargins() : QMargins();
  const int w = std::max(scale::tpx(120), m_side->width() - mg.left() - mg.right());
  m_previewStack->setFixedHeight(w);
  m_preview3d->setFixedSize(w, w);
  if (m_previewMode != PreviewMode::Flat)
    return;
  // The 512 px editor map; the thumbnail until it arrives.
  const MapFile *color = e->map(MapType::Color);
  QPixmap pm = color ? ThumbCache::instance().serverEditor(color->editor) : QPixmap();
  if (pm.isNull() && color)
    pm = ThumbCache::instance().server(color->thumb);
  if (pm.isNull()) {
    m_preview->setPixmap({});
    m_preview->setText(tr("loading…"));
    return;
  }
  const qreal dpr = devicePixelRatioF();
  QPixmap shown(QSize(w, w) * dpr);
  shown.setDevicePixelRatio(dpr);
  shown.fill(Qt::transparent);
  QPainter p(&shown);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(0, 0, w, w), 8, 8);
  p.setClipPath(clip);
  const QSize target(int(std::ceil(w * dpr)), int(std::ceil(w * dpr)));
  const QPixmap scaled = pm.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
  p.drawPixmap(QRectF(0, 0, w, w), scaled,
               QRectF((scaled.width() - target.width()) / 2.0, (scaled.height() - target.height()) / 2.0,
                      target.width(), target.height()));
  p.end();
  m_preview->setPixmap(shown);
}

void ReplaceTextureWindow::setPreviewMode(const PreviewMode mode) {
  m_previewMode = mode;
  if (mode != PreviewMode::Flat)
    m_preview3d->setShape(mode == PreviewMode::Sphere ? MaterialPreview3D::Shape::Sphere
                                                      : MaterialPreview3D::Shape::Cube);
  m_previewStack->setCurrentIndex(mode == PreviewMode::Flat ? 0 : 1);
  refreshPick();
}

void ReplaceTextureWindow::rebuildChips() {
  m_chipFlow->clear();
  m_tagPicker->addActiveChips(m_chipFlow, m_chipBox);
  if (!m_tagPicker->empty()) {
    auto *clear = new QToolButton(m_chipBox);
    clear->setObjectName(QStringLiteral("tbLink"));
    clear->setText(tr("Clear all"));
    connect(clear, &QToolButton::clicked, this, [this] { m_tagPicker->clear(true); });
    m_chipFlow->addWidget(clear);
  }
  m_chipBox->setVisible(!m_tagPicker->empty());
}

void ReplaceTextureWindow::replaceWith(const QString &ref) {
  const QString from = fromRef();
  if (!m_engine || ref.isEmpty() || from.isEmpty() || ref.compare(from, Qt::CaseInsensitive) == 0)
    return;
  m_engine->replaceMaterial(from, ref, selectionOnly());
  // The new texture is now the current one.
  refreshFromList();
}

} // namespace ui
