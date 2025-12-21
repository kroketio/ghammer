#include "texture_ui.h"

#include <QListWidget>

#include <QLabel>
#include "client/ui/scale.h"

#include <QApplication>
#include <QRandomGenerator>
#include <QMenu>
#include <QListView>
#include <QActionGroup>
#include <QImageReader>
#include <QJsonArray>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QThreadPool>
#include <QTimer>

#include <map>

#include "client/engine/scene.h"
#include "client/net/texture_catalog.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"

namespace ui {

QLabel *sectionLabel(const QString &text, QWidget *parent) {
  auto *l = new QLabel(text.toUpper(), parent);
  l->setObjectName(QStringLiteral("dlgSection"));
  return l;
}

QListWidget *sidebarList(QWidget *parent) {
  auto *l = new QListWidget(parent);
  l->setObjectName(QStringLiteral("dlgList"));
  l->setSelectionMode(QAbstractItemView::SingleSelection);
  l->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  l->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  l->setUniformItemSizes(true);
  return l;
}

void fitListHeight(QListWidget *l, const int maxRows) {
  // Rows have their real height only once the stylesheet is applied.
  l->ensurePolished();
  const int rows = std::min(l->count(), maxRows);
  const int rowH = l->sizeHintForRow(0) > 0 ? l->sizeHintForRow(0) : 28;
  l->setFixedHeight(std::max(1, rows) * (rowH + 2) + 12);
}

QString sizeText(const QSize &s) {
  if (!s.isValid() || s.isEmpty())
    return {};
  return QStringLiteral("%1 × %2").arg(s.width()).arg(s.height());
}

void CountListDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const {
  QStyleOptionViewItem o = opt;
  initStyleOption(&o, index);
  const QString count = index.data(Qt::UserRole + 1).toString();
  const double frac = index.data(Qt::UserRole + 2).toDouble();
  // background + icon only; the text is drawn below
  const QWidget *w = opt.widget;
  QStyle *style = w ? w->style() : QApplication::style();
  style->drawPrimitive(QStyle::PE_PanelItemViewItem, &o, p, w);
  p->save();
  const bool selected = opt.state.testFlag(QStyle::State_Selected);
  const QRect r = opt.rect.adjusted(8, 0, -8, 0);
  int x = r.left();
  if (!o.icon.isNull()) {
    const QSize ds = o.decorationSize;
    o.icon.paint(p, QRect(x, opt.rect.center().y() - ds.height() / 2, ds.width(), ds.height()));
    x += ds.width() + 6;
  }
  if (m_bars && frac > 0 && frac < 1.0001) {
    const QRect bar(r.left(), r.bottom() - 2, std::max(2, int(r.width() * frac)), 2);
    p->fillRect(bar, QColor(72, 102, 197, selected ? 255 : 110));
  }
  const QFontMetrics fm(opt.font);
  const int countW = count.isEmpty() ? 0 : fm.horizontalAdvance(count) + 10;
  p->setPen(selected ? QColor(255, 255, 255) : (o.palette.color(QPalette::Text)));
  if (const QVariant fg = index.data(Qt::ForegroundRole); fg.isValid() && !selected)
    p->setPen(fg.value<QBrush>().color());
  p->drawText(QRect(x, r.top(), r.right() - x - countW, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
              fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, r.right() - x - countW));
  p->setPen(selected ? QColor(220, 230, 255) : QColor(128, 140, 170));
  p->drawText(r, Qt::AlignVCenter | Qt::AlignRight, count);
  p->restore();
}

QSize CountListDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const {
  QSize s = QStyledItemDelegate::sizeHint(opt, index);
  s.setHeight(std::max(s.height(), QFontMetrics(opt.font).height() + 10));
  return s;
}

ThumbCache &ThumbCache::instance() {
  static ThumbCache *c = new ThumbCache();
  return *c;
}

ThumbCache::ThumbCache() : QObject(nullptr) {
  m_cache.setMaxCost(96 * 1024);  // KB
}

void ThumbCache::decode(const QString &key, const QString &path, int edge) {
  QThreadPool::globalInstance()->start([this, key, path, edge] {
    QImageReader reader(path);
    const QSize full = reader.size();
    if (full.isValid() && (full.width() > edge || full.height() > edge))
      reader.setScaledSize(full.scaled(edge, edge, Qt::KeepAspectRatio));
    const QImage img = reader.read();
    QMetaObject::invokeMethod(this, [this, key, img] {
      m_loading.remove(key);
      if (img.isNull()) {
        m_failed.insert(key);
      } else {
        auto *pm = new QPixmap(QPixmap::fromImage(img));
        m_cache.insert(key, pm, std::max<qsizetype>(1, qsizetype(img.sizeInBytes() / 1024)));
      }
      emit ready(key);
    }, Qt::QueuedConnection);
  });
}

QPixmap ThumbCache::server(const QString &file, net::FetchPriority prio) {
  if (file.isEmpty())
    return {};
  const QString key = serverKey(file);
  if (QPixmap *pm = m_cache.object(key))
    return *pm;
  if (m_loading.contains(key) || m_failed.contains(key))
    return {};
  m_loading.insert(key);
  net::FileFetcher::instance().fetch(net::FileKind::Thumb, file, prio, this, [this, key](const QString &path) {
    if (path.isEmpty()) {
      m_loading.remove(key);
      // a failed download may be retried after 30 s
      QTimer::singleShot(30000, this, [this, key] { m_failed.remove(key); });
      m_failed.insert(key);
      emit ready(key);
      return;
    }
    decode(key, path, 256);
  });
  return {};
}

QPixmap ThumbCache::serverEditor(const QString &file) {
  if (file.isEmpty())
    return {};
  const QString key = editorKey(file);
  if (QPixmap *pm = m_cache.object(key))
    return *pm;
  if (m_loading.contains(key) || m_failed.contains(key))
    return {};
  m_loading.insert(key);
  net::FileFetcher::instance().fetch(net::FileKind::Editor, file, net::FetchPriority::Visible, this,
                                     [this, key](const QString &path) {
                                       if (path.isEmpty()) {
                                         m_loading.remove(key);
                                         emit ready(key);
                                         return;
                                       }
                                       decode(key, path, 512);
                                     });
  return {};
}

QPixmap ThumbCache::local(const QString &path) {
  if (path.isEmpty())
    return {};
  const QString key = localKey(path);
  if (QPixmap *pm = m_cache.object(key))
    return *pm;
  if (m_loading.contains(key) || m_failed.contains(key))
    return {};
  m_loading.insert(key);
  decode(key, path, 256);
  return {};
}

QString ThumbSource::key() const {
  if (file.isEmpty())
    return {};
  return local ? ThumbCache::localKey(file) : ThumbCache::serverKey(file);
}

QPixmap ThumbSource::pixmap(const net::FetchPriority prio) const {
  if (file.isEmpty())
    return {};
  return local ? ThumbCache::instance().local(file) : ThumbCache::instance().server(file, prio);
}

ThumbSource thumbSourceOf(const eng::Material &m) {
  if (m.hasEntry())
    return {m.entry.thumbFile(), false};
  if (!m.libAlbedo.isEmpty() && !m.libAlbedo.startsWith(QLatin1Char(':')))
    return {m.libAlbedo, true};
  return {};
}

ThumbSource thumbSourceOfRef(const QString &ref) {
  if (const auto m = vk::TextureManager::instance().find(ref)) {
    const ThumbSource s = thumbSourceOf(*m);
    if (!s.empty())
      return s;
  }
  if (const TextureEntry *e = net::TextureCatalog::instance().find(MaterialRef::parse(ref).base()))
    return {e->thumbFile(), false};
  return {};
}

ServerTextureModel::ServerTextureModel(Mode mode, QObject *parent) : QAbstractListModel(parent), m_mode(mode) {
  auto &cat = net::TextureCatalog::instance();
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, &ServerTextureModel::onThumb);
  connect(&cat, &net::TextureCatalog::entryChanged, this, &ServerTextureModel::onEntryChanged);
  connect(&cat, &net::TextureCatalog::entryRenamed, this,
          [this](const QString &oldRef, const QString &newRef) {
            const int row = m_rowByKey.value(refKey(oldRef), -1);
            if (row < 0)
              return;
            if (const TextureEntry *e = net::TextureCatalog::instance().find(newRef))
              m_rows[row] = *e;
            m_rowByKey.remove(refKey(oldRef));
            m_rowByKey.insert(refKey(newRef), row);
            emit dataChanged(index(row), index(row));
          });
  connect(&cat, &net::TextureCatalog::entryRemoved, this, [this](const QString &ref) {
    const int row = m_rowByKey.value(refKey(ref), -1);
    if (row >= 0)
      emit dataChanged(index(row), index(row));
  });
  connect(&cat, &net::TextureCatalog::lockChanged, this, [this](const QString &ref) {
    const int row = m_rowByKey.value(refKey(MaterialRef::parse(ref).base()), -1);
    if (row >= 0)
      emit dataChanged(index(row), index(row), {LockedByRole});
  });
  connect(&cat, &net::TextureCatalog::catalogChanged, this, [this](int) {
    if (m_args.isEmpty() && m_rows.isEmpty())
      return;
    emit outdated(++m_outdated);
  });
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, [this](net::AssetClient::State s) {
    // First connection or back online.
    if (s == net::AssetClient::State::Online && m_rows.isEmpty())
      reload();
  });
  connect(&net::AssetClient::instance(), &net::AssetClient::resyncRequired, this, [this] {
    if (!m_rows.isEmpty())
      reload();
  });
  // Another server: empty now; the first Online above reloads the query from it.
  connect(&net::AssetClient::instance(), &net::AssetClient::endpointChanged, this, [this] {
    ++m_requestSerial;  // drop answers still coming from the old server
    beginResetModel();
    m_rows.clear();
    m_rowByKey.clear();
    m_total = 0;
    m_page = 0;
    m_facets.clear();
    m_licenseFacets.clear();
    m_distinctTags = 0;
    endResetModel();
    if (m_loading) {
      m_loading = false;
      emit loadingChanged(false);
    }
    emit resultChanged();
  });
}

void ServerTextureModel::setQuery(const QJsonObject &args) {
  m_args = args;
  m_page = 0;
  reload();
}

void ServerTextureModel::reload() {
  m_outdated = 0;
  emit outdated(0);
  request(m_mode == Mode::Paged ? m_page * m_pageSize : 0, false);
}

void ServerTextureModel::setPage(int page) {
  page = std::clamp(page, 0, std::max(0, pageCount() - 1));
  if (page == m_page && !m_rows.isEmpty())
    return;
  m_page = page;
  request(m_page * m_pageSize, false);
}

void ServerTextureModel::setPageSize(int n) {
  n = std::clamp(n, 12, 240);
  if (n == m_pageSize)
    return;
  const int firstItem = m_page * m_pageSize;
  m_pageSize = n;
  m_page = firstItem / n;
  reload();
}

int ServerTextureModel::pageCount() const { return m_total <= 0 ? 1 : (m_total + m_pageSize - 1) / m_pageSize; }

void ServerTextureModel::request(int offset, bool append) {
  const int serial = ++m_requestSerial;
  QJsonObject args = m_args;
  args.insert(QStringLiteral("offset"), offset);
  args.insert(QStringLiteral("limit"), m_pageSize);
  // Facets only with the first batch (they cover the whole result).
  args.insert(QStringLiteral("facets"), !append && m_args.value(QStringLiteral("facets")).toBool(false));
  m_loading = true;
  emit loadingChanged(true);
  net::TextureCatalog::instance().query(args, this, [this, serial, append](const net::QueryPage &p) {
    if (serial != m_requestSerial)
      return;  // superseded by a newer filter / page
    m_loading = false;
    emit loadingChanged(false);
    if (!p.ok) {
      emit failed(p.error);
      return;
    }
    if (append) {
      if (p.items.isEmpty())
        return;
      const int first = int(m_rows.size());
      beginInsertRows({}, first, first + int(p.items.size()) - 1);
      for (const auto &e: p.items) {
        m_rowByKey.insert(refKey(e.ref), int(m_rows.size()));
        m_rows.append(e);
      }
      endInsertRows();
    } else {
      beginResetModel();
      m_rows = p.items;
      m_rowByKey.clear();
      for (int i = 0; i < m_rows.size(); ++i)
        m_rowByKey.insert(refKey(m_rows[i].ref), i);
      endResetModel();
      if (m_args.value(QStringLiteral("facets")).toBool(false)) {
        m_facets = p.facets;
        m_distinctTags = p.distinctTags;
        m_licenseFacets = p.licenseFacets;
      }
    }
    m_total = p.total;
    emit resultChanged();
  });
}

const TextureEntry *ServerTextureModel::entryAt(int row) const {
  return row >= 0 && row < m_rows.size() ? &m_rows[row] : nullptr;
}

int ServerTextureModel::rowOf(const QString &ref) const { return m_rowByKey.value(refKey(MaterialRef::parse(ref).base()), -1); }

void ServerTextureModel::setUsedRefs(const QSet<QString> &keys) {
  if (keys == m_used)
    return;
  m_used = keys;
  if (!m_rows.isEmpty())
    emit dataChanged(index(0), index(int(m_rows.size()) - 1), {UsedRole});
}

void ServerTextureModel::onThumb(const QString &key) {
  if (!key.startsWith(QLatin1String("s:")))
    return;
  const QString file = key.mid(2);
  for (int i = 0; i < m_rows.size(); ++i)
    if (m_rows[i].thumbFile() == file)
      emit dataChanged(index(i), index(i), {ThumbRole, Qt::DecorationRole});
}

void ServerTextureModel::onEntryChanged(const QString &ref) {
  const int row = m_rowByKey.value(refKey(ref), -1);
  if (row < 0)
    return;
  if (const TextureEntry *e = net::TextureCatalog::instance().find(ref)) {
    m_rows[row] = *e;
    emit dataChanged(index(row), index(row));
  }
}

int ServerTextureModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant ServerTextureModel::data(const QModelIndex &idx, int role) const {
  if (!idx.isValid() || idx.row() >= m_rows.size())
    return {};
  const TextureEntry &e = m_rows[idx.row()];
  switch (role) {
    case Qt::DisplayRole: return e.name;
    case RefRole: return e.ref;
    case PackRole: return e.pack;
    case ThumbRole:
    case Qt::DecorationRole: return ThumbCache::instance().server(e.thumbFile());
    case UsedRole: return m_used.contains(refKey(e.ref));
    case BaseRole: return e.base;
    case SizeRole: return QSize(e.w, e.h);
    case MissingRole: return net::TextureCatalog::instance().isKnownMissing(e.ref);
    case LockedByRole: {
      const net::LockInfo *l = net::TextureCatalog::instance().lockFor(e.ref);
      return l ? l->name : QString();
    }
    case Qt::ToolTipRole: {
      QStringList lines;
      lines << QStringLiteral("<b>%1</b>").arg(e.name.toHtmlEscaped());
      lines << QStringLiteral("%1%2").arg(e.pack.toHtmlEscaped(), e.base ? QString() : tr(" (editable)"));
      if (e.w > 0)
        lines << sizeText({e.w, e.h});
      QStringList maps;
      for (auto it = e.maps.begin(); it != e.maps.end(); ++it)
        maps << mapTypeShort(it.key());
      lines << tr("Maps: %1").arg(maps.join(QStringLiteral(" · ")));
      if (!e.tags.isEmpty())
        lines << QStringLiteral("<i>%1</i>").arg(e.tags.mid(0, 10).join(QStringLiteral(", ")).toHtmlEscaped());
      if (m_used.contains(refKey(e.ref)))
        lines << tr("<span style='color:#5fd08a'>used in this map</span>");
      return lines.join(QStringLiteral("<br>"));
    }
    default: return {};
  }
}

Qt::ItemFlags ServerTextureModel::flags(const QModelIndex &idx) const {
  return QAbstractListModel::flags(idx) | Qt::ItemIsDragEnabled;
}

QStringList ServerTextureModel::mimeTypes() const { return {QString::fromLatin1(kMaterialMime), QStringLiteral("text/plain")}; }

QMimeData *ServerTextureModel::mimeData(const QModelIndexList &indexes) const {
  if (indexes.isEmpty())
    return nullptr;
  auto *md = new QMimeData();
  const QString ref = indexes.first().data(RefRole).toString();
  md->setData(QString::fromLatin1(kMaterialMime), ref.toUtf8());
  md->setText(ref);
  return md;
}

bool ServerTextureModel::canFetchMore(const QModelIndex &parent) const {
  return !parent.isValid() && m_mode == Mode::Append && !m_loading && m_rows.size() < m_total;
}

void ServerTextureModel::fetchMore(const QModelIndex &parent) {
  if (canFetchMore(parent))
    request(int(m_rows.size()), true);
}

MapMaterialsModel::MapMaterialsModel(vk::Engine3D *engine, QObject *parent)
    : QAbstractListModel(parent), m_engine(engine) {
  m_debounce = new QTimer(this);
  m_debounce->setSingleShot(true);
  m_debounce->setInterval(350);
  connect(m_debounce, &QTimer::timeout, this, &MapMaterialsModel::rebuild);
  if (engine) {
    connect(engine, &vk::Engine3D::sceneChanged, m_debounce, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::objectStateChanged, m_debounce, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::materialOverridesChanged, m_debounce, qOverload<>(&QTimer::start));
  }
  connect(&net::TextureCatalog::instance(), &net::TextureCatalog::entryChanged, m_debounce,
          qOverload<>(&QTimer::start));
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, [this](const QString &key) {
    for (int i = 0; i < m_rows.size(); ++i)
      if (m_rows[i].thumbKey == key)
        emit dataChanged(index(i), index(i), {ThumbRole, Qt::DecorationRole});
  });
  rebuild();
}

void MapMaterialsModel::rebuild() {
  if (!m_engine || !m_engine->scene())
    return;
  // material -> face count
  std::map<QString, int> counts;
  for (const auto &o: m_engine->scene()->gameObjects)
    if (const auto mesh = o->getMesh())
      for (int f = 0; f < mesh->faceCount(); ++f)
        if (const auto m = mesh->faceMaterialRef(f))
          ++counts[m->name];
  // Also variants not on any face yet.
  for (const auto &v: m_engine->scene()->materialOverrides.variants())
    counts.emplace(v.ref.toLower(), 0);

  QVector<Row> rows;
  auto &tm = vk::TextureManager::instance();
  for (const auto &[name, n]: counts) {
    const auto m = tm.materialFor(name);
    Row r;
    r.ref = m->ref.isEmpty() ? m->name : m->ref;
    r.faces = n;
    const MaterialRef mr = MaterialRef::parse(r.ref);
    r.variant = mr.isVariant();
    r.display = r.variant ? QStringLiteral("%1 · %2").arg(mr.name, mr.variant) : mr.name;
    r.server = m->kind == eng::Material::Kind::Server;
    r.missing = m->state == eng::Material::State::Error;
    const ThumbSource thumb = thumbSourceOf(*m);
    r.thumbFile = thumb.file;
    r.thumbKey = thumb.key();
    if (m->hasEntry()) {
      r.subtitle = m->entry.pack;
      r.size = QSize(m->entry.w, m->entry.h);
    } else if (thumb.local) {
      r.subtitle = tr("local library");
    } else {
      r.subtitle = r.server ? mr.pack : tr("built-in");
    }
    rows.append(r);
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    return a.ref.compare(b.ref, Qt::CaseInsensitive) < 0;
  });
  beginResetModel();
  m_rows = rows;
  endResetModel();
}

int MapMaterialsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant MapMaterialsModel::data(const QModelIndex &idx, int role) const {
  if (!idx.isValid() || idx.row() >= m_rows.size())
    return {};
  const Row &r = m_rows[idx.row()];
  switch (role) {
    case Qt::DisplayRole: return r.display;
    case RefRole: return r.ref;
    case PackRole: return r.subtitle;
    case ThumbRole:
    case Qt::DecorationRole:
      if (r.thumbKey.startsWith(QLatin1String("s:")))
        return ThumbCache::instance().server(r.thumbFile);
      if (r.thumbKey.startsWith(QLatin1String("l:")))
        return ThumbCache::instance().local(r.thumbFile);
      return QPixmap();
    case UsedRole: return r.faces > 0;
    case BaseRole: return false;
    case VariantRole: return r.variant;
    case MissingRole: return r.missing;
    case FacesRole: return r.faces;
    case SizeRole: return r.size;
    case Qt::ToolTipRole:
      return QStringLiteral("<b>%1</b><br>%2<br>%3")
          .arg(r.ref.toHtmlEscaped(), r.subtitle.toHtmlEscaped(),
               r.faces ? tr("%n face(s)", nullptr, r.faces) : tr("not on any face"));
    default: return {};
  }
}

Qt::ItemFlags MapMaterialsModel::flags(const QModelIndex &idx) const {
  return QAbstractListModel::flags(idx) | Qt::ItemIsDragEnabled;
}

QStringList MapMaterialsModel::mimeTypes() const { return {QString::fromLatin1(kMaterialMime), QStringLiteral("text/plain")}; }

QMimeData *MapMaterialsModel::mimeData(const QModelIndexList &indexes) const {
  if (indexes.isEmpty())
    return nullptr;
  auto *md = new QMimeData();
  const QString ref = indexes.first().data(RefRole).toString();
  md->setData(QString::fromLatin1(kMaterialMime), ref.toUtf8());
  md->setText(ref);
  return md;
}

TextureCardDelegate::TextureCardDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize TextureCardDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const {
  const QFontMetrics fm(scale::font(scale::Font::Caption, opt.font));
  const int lines = m_showPack ? 2 : 1;
  return {m_size + 10, m_size + 10 + lines * fm.height() + 4};
}

void TextureCardDelegate::paintPlaceholder(QPainter *p, const QRect &img) {
  const int c = std::max(8, img.width() / 6);
  for (int y = 0; y < img.height(); y += c)
    for (int x = 0; x < img.width(); x += c)
      p->fillRect(QRect(img.left() + x, img.top() + y, c, c),
                  ((x / c + y / c) & 1) ? QColor(52, 58, 78) : QColor(42, 47, 64));
}

void TextureCardDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const {
  p->save();
  p->setRenderHint(QPainter::Antialiasing);
  p->setRenderHint(QPainter::SmoothPixmapTransform);
  const QRect r = opt.rect.adjusted(3, 3, -3, -3);
  const QRect img(r.left() + (r.width() - m_size) / 2, r.top() + 2, m_size, m_size);
  const QString ref = index.data(RefRole).toString();
  const bool selected = opt.state.testFlag(QStyle::State_Selected);
  const bool hover = opt.state.testFlag(QStyle::State_MouseOver);
  const bool current = !m_current.isEmpty() && ref.toLower() == m_current;

  // card
  p->setPen(Qt::NoPen);
  if (selected)
    p->setBrush(QColor(34, 140, 240, 70));
  else if (hover)
    p->setBrush(QColor(255, 255, 255, 20));
  else
    p->setBrush(Qt::NoBrush);
  p->drawRoundedRect(r, 6, 6);

  // thumbnail (cover-fit, rounded)
  QPainterPath clip;
  clip.addRoundedRect(img, 5, 5);
  p->setClipPath(clip);
  const QPixmap pm = index.data(ThumbRole).value<QPixmap>();
  if (!pm.isNull()) {
    const QPixmap scaled = pm.scaled(img.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    p->drawPixmap(img, scaled, QRect((scaled.width() - img.width()) / 2, (scaled.height() - img.height()) / 2,
                                     img.width(), img.height()));
  } else {
    paintPlaceholder(p, img);
  }
  if (index.data(MissingRole).toBool()) {
    p->fillRect(img, QColor(20, 0, 20, 150));
    p->setPen(QPen(QColor(230, 80, 200), 2));
    p->drawLine(img.topLeft() + QPoint(8, 8), img.bottomRight() - QPoint(8, 8));
    p->drawLine(img.topRight() + QPoint(-8, 8), img.bottomLeft() + QPoint(8, -8));
  }
  p->setClipping(false);

  // current material ring
  if (current) {
    p->setPen(QPen(QColor(255, 176, 60), 2.5));
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(QRectF(img).adjusted(1.25, 1.25, -1.25, -1.25), 5, 5);
  } else if (selected) {
    p->setPen(QPen(QColor(80, 170, 255), 1.5));
    p->setBrush(Qt::NoBrush);
    p->drawRoundedRect(QRectF(img).adjusted(0.75, 0.75, -0.75, -0.75), 5, 5);
  }

  const QFont small = scaledFont(scale::font(scale::Font::Caption, opt.font), 0.9);
  const QFontMetrics sfm(small);
  // badges
  int bx = img.left() + 5;
  auto pill = [&](const QString &text, const QColor &bg, const QColor &fg) {
    const int w = sfm.horizontalAdvance(text) + 10;
    const QRect pr(bx, img.top() + 5, std::min(w, img.width() - 10), sfm.height() + 2);
    p->setPen(Qt::NoPen);
    p->setBrush(bg);
    p->drawRoundedRect(pr, pr.height() / 2.0, pr.height() / 2.0);
    p->setPen(fg);
    p->setFont(small);
    p->drawText(pr, Qt::AlignCenter, sfm.elidedText(text, Qt::ElideRight, pr.width() - 6));
    bx += pr.width() + 3;
  };
  if (index.data(VariantRole).toBool())
    pill(tr("variant"), QColor(140, 90, 220, 230), Qt::white);
  if (!index.data(BaseRole).toBool() && index.data(PackRole).isValid() && !index.data(VariantRole).toBool() &&
      index.data(SizeRole).toSize().isValid() && qobject_cast<const ServerTextureModel *>(index.model()))
    pill(tr("editable"), QColor(30, 120, 210, 230), Qt::white);
  if (const QString lockedBy = index.data(LockedByRole).toString(); !lockedBy.isEmpty())
    pill(QStringLiteral("✎ %1").arg(lockedBy), QColor(220, 150, 40, 235), QColor(30, 20, 0));
  // used in the map: green dot
  if (index.data(UsedRole).toBool()) {
    p->setPen(QPen(QColor(16, 20, 28), 1.5));
    p->setBrush(QColor(80, 205, 125));
    p->drawEllipse(QPointF(img.right() - 8, img.top() + 8), 4.5, 4.5);
  }

  // text
  const QFont nameFont = scale::font(scale::Font::Caption, opt.font);
  const QFontMetrics nfm(nameFont);
  QRect tr1(r.left() + 2, img.bottom() + 4, r.width() - 4, nfm.height());
  p->setFont(nameFont);
  p->setPen(selected ? QColor(255, 255, 255) : QColor(214, 220, 236));
  p->drawText(tr1, Qt::AlignHCenter | Qt::AlignVCenter,
              nfm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, tr1.width()));
  if (m_showPack) {
    QString sub = index.data(PackRole).toString();
    if (const int faces = index.data(FacesRole).toInt(); faces > 0)
      sub = QStringLiteral("%1 · %2").arg(sub).arg(faces);
    QRect tr2(r.left() + 2, tr1.bottom() + 1, r.width() - 4, sfm.height());
    p->setFont(small);
    p->setPen(QColor(128, 140, 168));
    p->drawText(tr2, Qt::AlignHCenter | Qt::AlignVCenter, sfm.elidedText(sub, Qt::ElideMiddle, tr2.width()));
  }
  p->restore();
}

TextureSearch parseTextureSearch(const QString &typed) {
  TextureSearch out;
  QStringList words;
  for (const QString &w: typed.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
    const int slash = w.indexOf(QLatin1Char('/'));
    if (slash > 0 && slash < w.size() - 1) {
      const MaterialRef r = MaterialRef::parse(w);
      out.pack = r.pack;  // the last ref wins; one pack filter at a time
      words << r.name;
    } else {
      words << w;
    }
  }
  out.text = words.join(QLatin1Char(' '));
  return out;
}

QListView *makeTextureGrid(QAbstractItemModel *model, QAbstractItemDelegate *delegate, QWidget *parent,
                           const bool draggable, const int spacing) {
  auto *view = new QListView(parent);
  view->setModel(model);
  view->setItemDelegate(delegate);
  view->setViewMode(QListView::IconMode);
  view->setResizeMode(QListView::Adjust);
  view->setMovement(QListView::Static);
  view->setUniformItemSizes(true);
  view->setSelectionMode(QAbstractItemView::SingleSelection);
  if (draggable) {
    view->setDragEnabled(true);
    view->setDragDropMode(QAbstractItemView::DragOnly);
  }
  view->setMouseTracking(true);
  view->setSpacing(spacing);
  view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  // Item sizes come from the font.
  scale::bind(view, [view] { view->doItemsLayout(); });
  return view;
}

ShuffleButton::ShuffleButton(QWidget *parent) : QToolButton(parent) {
  m_shuffled = prefs::b("Textures/RandomOrder");
  m_seed = QRandomGenerator::global()->generate();
  setIcon(ActionManager::icon(QStringLiteral("file_refresh")));
  setText(tr("Shuffle"));
  setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  setAutoRaise(true);
  setPopupMode(QToolButton::MenuButtonPopup);
  setToolTip(tr("A new random order of the same results (search, pack and tags stay)"));
  auto *orderMenu = new QMenu(this);
  auto *orderGroup = new QActionGroup(orderMenu);
  QAction *shuffledOrder = orderMenu->addAction(tr("Shuffled"));
  QAction *nameOrder = orderMenu->addAction(tr("By name"));
  for (QAction *a: {shuffledOrder, nameOrder}) {
    a->setCheckable(true);
    orderGroup->addAction(a);
  }
  (m_shuffled ? shuffledOrder : nameOrder)->setChecked(true);
  setMenu(orderMenu);
  connect(this, &QToolButton::clicked, this, [this, shuffledOrder] {
    m_seed = QRandomGenerator::global()->generate();
    m_shuffled = true;
    shuffledOrder->setChecked(true);
    prefs::get().setValue(QStringLiteral("Textures/RandomOrder"), true);
    emit orderChanged();
  });
  connect(shuffledOrder, &QAction::triggered, this, [this] {
    m_shuffled = true;
    prefs::get().setValue(QStringLiteral("Textures/RandomOrder"), true);
    emit orderChanged();
  });
  connect(nameOrder, &QAction::triggered, this, [this] {
    m_shuffled = false;
    prefs::get().setValue(QStringLiteral("Textures/RandomOrder"), false);
    emit orderChanged();
  });
}

void ShuffleButton::addOrder(QJsonObject &args) const {
  args.insert(QStringLiteral("sort"), m_shuffled ? QStringLiteral("random") : QStringLiteral("name"));
  args.insert(QStringLiteral("seed"), double(m_seed));
}

QJsonObject serverQuery(const QString &typed, const QString &pack, const QStringList &tags,
                        const ShuffleButton *order) {
  QJsonObject args;
  // A typed ref ("pack/Name") searches its name in its pack, overriding the pack box.
  const TextureSearch search = parseTextureSearch(typed);
  args.insert(QStringLiteral("q"), search.text);
  if (const QString p = !search.pack.isEmpty() ? search.pack : pack; !p.isEmpty())
    args.insert(QStringLiteral("pack"), p);
  if (!tags.isEmpty())
    args.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
  // Skyboxes are not materials.
  args.insert(QStringLiteral("sky"), false);
  if (order)
    order->addOrder(args);
  return args;
}

} // namespace ui
