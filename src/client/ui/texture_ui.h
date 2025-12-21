#pragma once

// Building blocks shared by every texture view.

#include <QAbstractListModel>
#include <QCache>
#include <QHash>
#include <QJsonObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QStyledItemDelegate>
#include <QToolButton>

#include "client/net/file_fetcher.h"
#include "shared/models/texture_entry.h"

class QLabel;
class QListView;
class QListWidget;
class QAbstractItemDelegate;

namespace eng {
class Material;
}

namespace vk {
class Engine3D;
}

namespace ui {

inline constexpr const char *kMaterialMime = "application/x-editor-material";

// Roles every texture model answers.
enum TextureRoles {
  RefRole = Qt::UserRole + 100,  // ref to put on faces
  PackRole,                      // pack name (or a subtitle)
  ThumbRole,                     // QPixmap (null while loading)
  UsedRole,                      // the open map uses it
  BaseRole,                      // read-only base pack
  LockedByRole,                  // name of the editor holding the edit lock
  SizeRole,                      // QSize, source pixels
  VariantRole,                   // a map-local variant
  MissingRole,                   // the server does not have it (any more)
  FacesRole,                     // faces using it (map model)
};

class ThumbCache final : public QObject {
  Q_OBJECT

public:
  static ThumbCache &instance();

  // Fetched and decoded in the background; null until ready(key) fires.
  QPixmap server(const QString &thumbFile, net::FetchPriority prio = net::FetchPriority::Visible);
  // A server editor map (512 px).
  QPixmap serverEditor(const QString &file);
  QPixmap local(const QString &path);

  static QString serverKey(const QString &file) { return QStringLiteral("s:") + file; }
  static QString editorKey(const QString &file) { return QStringLiteral("e:") + file; }
  static QString localKey(const QString &path) { return QStringLiteral("l:") + path; }

signals:
  void ready(const QString &key);

private:
  ThumbCache();
  void decode(const QString &key, const QString &path, int edge);
  QCache<QString, QPixmap> m_cache;
  QSet<QString> m_loading;
  QSet<QString> m_failed;
};

// A server thumbnail file or a local library image; empty for built-in materials.
struct ThumbSource {
  QString file;
  bool local = false;
  [[nodiscard]] bool empty() const { return file.isEmpty(); }
  [[nodiscard]] QString key() const;
  // Null until ThumbCache has it.
  [[nodiscard]] QPixmap pixmap(net::FetchPriority prio = net::FetchPriority::Visible) const;
};
// The server entry's thumbnail, else the local albedo.
ThumbSource thumbSourceOf(const eng::Material &m);
// The loaded material's, else the catalog entry's (a variant uses its base's).
ThumbSource thumbSourceOfRef(const QString &ref);

class ServerTextureModel final : public QAbstractListModel {
  Q_OBJECT

public:
  enum class Mode { Append, Paged };

  explicit ServerTextureModel(Mode mode, QObject *parent = nullptr);

  // textures.query args without offset/limit. Resets and loads the first page.
  void setQuery(const QJsonObject &args);
  [[nodiscard]] QJsonObject query() const { return m_args; }
  void reload();
  void setPage(int page);
  void setPageSize(int n);
  [[nodiscard]] int page() const { return m_page; }
  [[nodiscard]] int pageSize() const { return m_pageSize; }
  [[nodiscard]] int pageCount() const;
  [[nodiscard]] int total() const { return m_total; }
  [[nodiscard]] QVector<QPair<QString, int>> facets() const { return m_facets; }
  [[nodiscard]] int distinctTags() const { return m_distinctTags; }
  // Within the filter, ignoring the license choice.
  [[nodiscard]] QVector<QPair<QString, int>> licenseFacets() const { return m_licenseFacets; }
  [[nodiscard]] const TextureEntry *entryAt(int row) const;
  [[nodiscard]] int rowOf(const QString &ref) const;
  void setUsedRefs(const QSet<QString> &refKeys);

  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  QStringList mimeTypes() const override;
  QMimeData *mimeData(const QModelIndexList &indexes) const override;
  bool canFetchMore(const QModelIndex &parent) const override;
  void fetchMore(const QModelIndex &parent) override;

signals:
  void loadingChanged(bool loading);
  void resultChanged();  // total / facets / page changed
  void failed(const QString &message);
  // Server changes that may alter this result; views offer a refresh instead of reshuffling.
  void outdated(int changes);

private:
  void request(int offset, bool append);
  void onThumb(const QString &key);
  void onEntryChanged(const QString &ref);

  Mode m_mode;
  QJsonObject m_args;
  QVector<TextureEntry> m_rows;
  QHash<QString, int> m_rowByKey;  // refKey -> row
  QSet<QString> m_used;
  int m_total = 0;
  int m_page = 0;
  int m_pageSize = 120;
  bool m_loading = false;
  int m_requestSerial = 0;
  int m_outdated = 0;
  QVector<QPair<QString, int>> m_facets;
  int m_distinctTags = 0;
  QVector<QPair<QString, int>> m_licenseFacets;
};

// Materials used by the open map, variants included. Rebuilt (debounced) as the map changes.
class MapMaterialsModel final : public QAbstractListModel {
  Q_OBJECT

public:
  explicit MapMaterialsModel(vk::Engine3D *engine, QObject *parent = nullptr);
  void rebuild();

  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  QStringList mimeTypes() const override;
  QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
  struct Row {
    QString ref;
    QString display;
    QString subtitle;
    QString thumbKey;   // ThumbCache key
    QString thumbFile;  // server thumb file or local path
    bool server = false;
    bool variant = false;
    bool missing = false;
    int faces = 0;
    QSize size;
  };
  QPointer<vk::Engine3D> m_engine;
  QVector<Row> m_rows;
  QTimer *m_debounce = nullptr;
};

// With `draggable` the cards drag onto faces (kMaterialMime).
QListView *makeTextureGrid(QAbstractItemModel *model, QAbstractItemDelegate *delegate, QWidget *parent,
                           bool draggable = true, int spacing = 2);

// Shuffled or name order; starts as Textures/RandomOrder says.
class ShuffleButton final : public QToolButton {
  Q_OBJECT

public:
  explicit ShuffleButton(QWidget *parent = nullptr);
  [[nodiscard]] quint32 seed() const { return m_seed; }
  // Adds "sort" and "seed" to textures.query args.
  void addOrder(QJsonObject &args) const;

signals:
  void orderChanged();

private:
  bool m_shuffled = true;
  quint32 m_seed = 0;
};

// textures.query args ("pack/Name" searches that pack; empty `pack` = all). Always sky: false.
QJsonObject serverQuery(const QString &typed, const QString &pack, const QStringList &tags,
                        const ShuffleButton *order);

class TextureCardDelegate final : public QStyledItemDelegate {
  Q_OBJECT

public:
  explicit TextureCardDelegate(QObject *parent = nullptr);
  void setThumbSize(int px) { m_size = px; }
  void setCurrent(const QString &ref) { m_current = ref.toLower(); }
  void setShowPack(bool on) { m_showPack = on; }
  void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
  QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;

  // Placeholder pattern while a thumbnail loads.
  static void paintPlaceholder(QPainter *p, const QRect &r);

private:
  int m_size = 96;
  QString m_current;
  bool m_showPack = true;
};

// "icon  name ....... count" rows, optionally with a bar.
// Data: DisplayRole name, UserRole+1 count text, UserRole+2 fraction 0..1.
class CountListDelegate final : public QStyledItemDelegate {
  Q_OBJECT

public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void setBars(bool on) { m_bars = on; }
  void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
  QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;

private:
  bool m_bars = false;
};

// "1024 × 512"
QString sizeText(const QSize &s);

// Texture window sidebar helpers.
QLabel *sectionLabel(const QString &text, QWidget *parent);
QListWidget *sidebarList(QWidget *parent);
void fitListHeight(QListWidget *list, int maxRows);

// A typed search; a word shaped like a ref ("pack/Name", optionally "#variant") searches its name in its pack.
struct TextureSearch {
  QString text;  // the server's "q"
  QString pack;  // empty: no pack in the search
};
TextureSearch parseTextureSearch(const QString &typed);

} // namespace ui
