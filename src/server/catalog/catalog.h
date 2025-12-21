#pragma once

// In-memory texture catalogue of the asset server.
//
// Loaded once at startup from data/textures: top-level <pack>.json files plus sub-directories with a legacy
// meta.json; map files are attached by name. Reads are thread-safe (QReadWriteLock); writes only on the mutation thread.
//
// DATA SAFETY: never writes, renames or deletes anything under data/textures. Generated files live in cache/thumbs
// and cache/editor (derivatives.h); user-pack JSON writes go through mutations.cpp, which refuses base packs.

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QReadWriteLock>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <optional>

#include "shared/models/texture_entry.h"

struct CatalogPack {
  QString name;
  QString description;
  bool base = false;    // read-only: the pack JSON says "base": true
  QString type = QStringLiteral("texture");  // "texture" or "lut" (colour LUT strips, <Name>.png)
  QString dir;          // absolute directory holding the map files
  QString jsonPath;     // absolute path of the pack JSON (<pack>.json or <dir>/meta.json)
  bool legacyMeta = false;
  qint64 rev = 0;
  qint64 created = 0;
  int count = 0;
  QJsonObject extra;    // unknown top-level keys, preserved on rewrite

  [[nodiscard]] QJsonObject toJson() const;  // wire form
  [[nodiscard]] bool isLut() const { return type == QLatin1String("lut"); }
  [[nodiscard]] QString key() const { return packKey(name, type); }  // unique: name + type
};


// One generated file (thumbnail or 512 px editor map) and what it is made from.
struct DerivativeSource {
  QString file;      // derivative file name ("<checksum>.jpg")
  QString srcAbs;    // absolute source image path
  QString relPath;   // source path relative to data/textures (image_info key)
  MapType type = MapType::Color;
  bool thumb = false;      // 256 px thumbnail, else 512 px editor map
  bool flipGreen = false;  // DirectX normal -> OpenGL
  bool png = false;        // keep alpha
};

// A pack read from its JSON and scanned, not yet in the catalogue (installs: scanned off the mutation thread).
struct PackScan {
  CatalogPack pack;
  QVector<TextureEntry> entries;
  QVector<DerivativeSource> derivs;
  QString error;
};

// A texture's license as a filter key: lower case ("CC0" and "cc0" are one),
// "unknown" when the pack gives none.
QString licenseKey(const QString &license);
// Licenses that cost nothing to use in a game: public domain / CC0, royalty
// free, permissive and attribution-only licenses.
bool isFreeLicense(const QString &key);

struct CatalogQuery {
  QString text;          // free text; "-word" excludes
  QStringList packs;     // empty = all
  QStringList tags;      // all required (alias-folded)
  QStringList notTags;
  QStringList refs;      // restrict to these refs (e.g. "this map")
  bool alphaOnly = false;  // transparent textures (alpha in the color map, or an opacity map)
  QStringList licenses;    // any of these (licenseKey); empty = all
  bool freeOnly = false;   // only licenses free to use (isFreeLicense)
  int sky = -1;            // "sky": 1 = only skyboxes (a panorama map), 0 = none, -1 (absent) = both
  bool lut = false;        // "lut": true = only colour LUTs; otherwise LUTs are left out
  QString sort = QStringLiteral("name");  // name | recent | pack | random
  quint32 seed = 0;        // random order: the same seed gives the same order on every page
  int offset = 0;
  int limit = 60;
  bool facets = false;
  int facetLimit = 40;

  static CatalogQuery fromJson(const QJsonObject &args);
};

struct CatalogQueryResult {
  int total = 0;
  QVector<TextureEntry> items;
  QVector<QPair<QString, int>> facets;  // top tags within the filter
  int distinctTags = 0;
  // Every license within the filter, ignoring the license choice itself (so the
  // other licenses stay pickable), most textures first.
  QVector<QPair<QString, int>> licenseFacets;
};

// texture_tags.json in the server config dir: popular tags, tags that are noise
// for browsing (stoplist) and spelling variants folded into one (aliases).
struct TagConfig {
  QStringList popular;   // in the order the UI lists them: what people need most first
  int maxTags = 40;      // a texture with more tags than this is tag spam: only tags in its name count
  QSet<QString> stoplist;
  QHash<QString, QString> aliases;

  [[nodiscard]] QString fold(const QString &tag) const;
  static TagConfig loadOrCreate(const QString &path);
  [[nodiscard]] QJsonObject toJson() const;
};

class Catalog {
public:
  static Catalog &instance();

  // Blocking scan; call once from the main thread. `dataTexturesDir` is
  // data/textures, `cacheDir` the server cache root, `configDir` holds
  // texture_tags.json. Returns the derivative files that do not exist yet.
  QVector<DerivativeSource> load(const QString &dataTexturesDir, const QString &cacheDir, const QString &configDir);

  [[nodiscard]] bool isLoaded() const;
  [[nodiscard]] bool loadHadErrors() const { return m_loadErrors; }
  [[nodiscard]] qint64 revision() const;
  [[nodiscard]] int textureCount() const;
  [[nodiscard]] QString texturesDir() const { return m_texturesDir; }
  [[nodiscard]] QString thumbsDir() const { return m_thumbsDir; }
  [[nodiscard]] QString editorDir() const { return m_editorDir; }

  // reads (any thread)
  [[nodiscard]] QVector<CatalogPack> packs() const;
  [[nodiscard]] std::optional<CatalogPack> pack(const QString &name,
                                                const QString &type = QStringLiteral("texture")) const;
  [[nodiscard]] CatalogQueryResult query(const CatalogQuery &q) const;
  [[nodiscard]] QVector<QPair<QString, int>> tagSearch(const CatalogQuery &filter, const QString &text, int limit) const;
  [[nodiscard]] QVector<QPair<QString, int>> popular(const CatalogQuery &filter) const;
  // By ref (case-insensitive, follows rename aliases); a bare legacy name
  // resolves by texture name. Invalid entry when unknown.
  [[nodiscard]] TextureEntry entry(const QString &ref, bool lut = false) const;
  [[nodiscard]] TextureEntry entryById(const QString &id) const;
  [[nodiscard]] bool derivative(const QString &file, DerivativeSource &out) const;
  [[nodiscard]] QSet<QString> derivativeNames() const;
  [[nodiscard]] QString sourcePath(const QString &ref, MapType type) const;

  // writes (mutation thread / derivative workers)
  qint64 bumpRevision();  // returns the new revision (persisted)
  // Real alpha of a source image became known after decoding it.
  void setImageAlpha(const QString &relPath, bool hasTransparentPixels);
  // (Re)builds one texture's entry from its pack directory (after copies / uploads); returns it and the derivatives
  // it needs.
  TextureEntry rescanTexture(const CatalogPack &pack, const QString &textureName, const QJsonObject &meta,
                             qint64 rev, QVector<DerivativeSource> *needed);
  void removeTexture(const QString &ref, bool lut, const QString &aliasTo = {});
  // Old ref keeps resolving to the new one (after renames).
  void addAlias(const QString &oldRef, const QString &newRef, bool lut);
  void upsertPack(const CatalogPack &p);
  void removePack(const QString &name, const QString &type);
  void setProps(const QString &ref, const MaterialProps &props, qint64 rev);
  void setTags(const QString &ref, bool lut, const QStringList &tags, qint64 rev);

  // A pack JSON in data/textures read and scanned (any thread; header reads can take a while).
  [[nodiscard]] PackScan scanPackFile(const QString &jsonPath);
  // Adds a scanned pack (mutation thread); returns the derivatives to build. False when its name + type is taken.
  bool addPack(const PackScan &scan, QVector<DerivativeSource> *missing);

  // JSON object of one pack's meta (for rewriting a user pack JSON).
  [[nodiscard]] QJsonObject packMetaJson(const QString &packName, const QString &type) const;

private:
  Catalog() = default;

  struct Built {
    QVector<TextureEntry> entries;
    QVector<DerivativeSource> derivs;
  };
  Built scanPack(const CatalogPack &pack, const QJsonObject &meta, const QJsonArray &tagTable);
  static std::optional<CatalogPack> packFromJson(const QJsonObject &o, const QString &jsonPath, const QString &root);
  QVector<DerivativeSource> missingDerivatives(const QVector<DerivativeSource> &derivs) const;
  TextureEntry buildEntry(const CatalogPack &pack, const QString &textureName, const QMap<MapType, QString> &files,
                          const QJsonObject &meta, const QJsonArray &tagTable, QVector<DerivativeSource> *derivs);
  void indexEntry(const TextureEntry &e);
  void unindexEntry(const TextureEntry &e);
  QVector<const TextureEntry *> sortedLocked(const QString &sort, quint32 seed = 0) const;
  bool matches(const TextureEntry &e, const CatalogQuery &q, const QStringList &words, const QStringList &notWords,
               const QSet<QString> &refs) const;
  QVector<const TextureEntry *> filterLocked(const CatalogQuery &q) const;

  mutable QReadWriteLock m_lock;
  std::atomic<bool> m_loaded{false};
  bool m_loadErrors = false;
  qint64 m_revision = 0;
  QString m_texturesDir, m_thumbsDir, m_editorDir;
  TagConfig m_tags;

  QMap<QString, CatalogPack> m_packs;            // packKey -> pack
  QHash<QString, TextureEntry> m_entries;        // assetKey -> entry
  QHash<QString, QString> m_byId;                // uuid -> refKey
  QHash<QString, QString> m_byName;              // lower bare name -> refKey (legacy refs)
  QHash<QString, QString> m_aliases;             // old refKey -> new refKey (renames)
  QHash<QString, DerivativeSource> m_derivs;     // derivative file -> source
  QHash<QString, QString> m_relToRef;            // source relPath -> refKey (alpha updates)

  // Sorted views, rebuilt lazily after structural changes.
  mutable QMutex m_sortMutex;
  mutable qint64 m_structure = 0;
  mutable QHash<QString, QPair<qint64, QVector<const TextureEntry *>>> m_sorted;
};
