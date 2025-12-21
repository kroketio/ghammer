#include "catalog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QtDebug>

#include <algorithm>
#include <random>
#include <QRegularExpression>

#include "server_db.h"
#include "shared/lib/hdr_image.h"

namespace {

const QStringList kDefaultPopular = {"floor", "wall",   "concrete", "brick", "wood",  "tile",   "plaster",
                                     "metal", "stone",  "rock",     "ground", "grass", "dirt",   "sand",
                                     "gravel", "asphalt", "marble",  "roof",  "fabric", "paint"};
const QStringList kDefaultStop = {"pbr",       "seamless",   "texture", "textures",  "material", "materials",
                                  "procedural", "realistic", "surface", "design",    "color",    "colour",
                                  "tileable",  "1k",         "2k",      "4k",        "free",     "cc0",
                                  "texturefun", "cgambient", "polyhaven", "blenderkit", "sharetextures",
                                  "cgbookcase", "fab",       "substance", "photoscan", "scan"};
const QList<QPair<QString, QString>> kDefaultAliases = {
    {"bricks", "brick"},   {"tiles", "tile"},     {"wooden", "wood"},     {"rocks", "rock"},
    {"stones", "stone"},   {"planks", "plank"},   {"cracks", "crack"},    {"cracked", "crack"},
    {"grey", "gray"},      {"leafs", "leaves"},   {"leaf", "leaves"},     {"metallic", "metal"},
    {"tiled", "tile"},     {"pebbles", "pebble"}, {"roofing", "roof"},    {"walls", "wall"},
    {"floors", "floor"},   {"flooring", "floor"}, {"fabrics", "fabric"},  {"carpets", "carpet"},
};

QString checksumFor(const QString &relPath, qint64 size, qint64 mtimeMs) {
  const QByteArray key = relPath.toUtf8() + '|' + QByteArray::number(size) + '|' + QByteArray::number(mtimeMs);
  return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex().left(16));
}

bool formatHasAlpha(QImage::Format f) {
  return QImage::toPixelFormat(f).alphaUsage() == QPixelFormat::UsesAlpha;
}

int formatChannels(QImage::Format f) {
  switch (f) {
    case QImage::Format_Grayscale8:
    case QImage::Format_Grayscale16:
    case QImage::Format_Alpha8:
    case QImage::Format_Indexed8:
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB: return 1;
    default: return formatHasAlpha(f) ? 4 : 3;
  }
}

// Preferred file when a texture has the same map twice (png over jpg, GL normal
// over DX normal).
bool preferOver(const ParsedMapFile &a, const ParsedMapFile &b) {
  if (a.type == MapType::Normal && a.directX != b.directX)
    return !a.directX;
  if (a.ext != b.ext)
    return a.ext == QLatin1String("png");
  return false;
}

// A LUT pack's files: "<Name>.png" (Name a valid asset name), by name.
QMap<QString, QString> lutFiles(const QString &dir) {
  QMap<QString, QString> out;
  for (const QString &fn: QDir(dir).entryList({QStringLiteral("*.png"), QStringLiteral("*.PNG")}, QDir::Files)) {
    const QString name = QFileInfo(fn).completeBaseName();
    if (isValidAssetName(name))
      out.insert(name, fn);
  }
  return out;
}

// Godot's 3D LUT layout: a horizontal strip of N slices of N x N.
bool isLutStrip(const TextureEntry &e) {
  const MapFile *f = e.map(MapType::Lut);
  return f && f->h >= 2 && f->h <= 256 && f->w == f->h * f->h;
}

QStringList tagsFromMeta(const QJsonObject &m, const QJsonArray &tagTable, const TagConfig &cfg) {
  QStringList out;
  for (const auto &t: m.value(QStringLiteral("tags")).toArray()) {
    QString s;
    if (t.isDouble()) {
      const int i = t.toInt(-1);
      if (i >= 0 && i < tagTable.size())
        s = tagTable.at(i).toString();
    } else {
      s = t.toString();
    }
    s = cfg.fold(s);
    if (!s.isEmpty() && !out.contains(s))
      out.append(s);
  }
  return out;
}

} // namespace

QJsonObject CatalogPack::toJson() const {
  QJsonObject o;
  o.insert(QStringLiteral("name"), name);
  o.insert(QStringLiteral("description"), description);
  o.insert(QStringLiteral("base"), base);
  o.insert(QStringLiteral("type"), type);
  o.insert(QStringLiteral("count"), count);
  o.insert(QStringLiteral("rev"), double(rev));
  if (created)
    o.insert(QStringLiteral("created"), double(created));
  return o;
}

QString licenseKey(const QString &license) {
  const QString k = license.trimmed().toLower();
  return k.isEmpty() ? QStringLiteral("unknown") : k;
}

bool isFreeLicense(const QString &key) {
  static const QSet<QString> kFree = {
      QStringLiteral("cc0"),           QStringLiteral("cc0-1.0"),      QStringLiteral("rf"),
      QStringLiteral("royalty free"),  QStringLiteral("royalty-free"), QStringLiteral("public domain"),
      QStringLiteral("pd"),            QStringLiteral("unlicense"),    QStringLiteral("mit"),
      QStringLiteral("cc-by"),         QStringLiteral("cc by"),        QStringLiteral("cc-by-3.0"),
      QStringLiteral("cc-by-4.0"),     QStringLiteral("cc by 4.0"),    QStringLiteral("cc by 3.0"),
  };
  return kFree.contains(key);
}

CatalogQuery CatalogQuery::fromJson(const QJsonObject &a) {
  CatalogQuery q;
  auto list = [&a](const char *key) {
    QStringList out;
    const QJsonValue v = a.value(QLatin1String(key));
    if (v.isString()) {
      for (const auto &s: v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
        out.append(s.trimmed());
    } else {
      for (const auto &s: v.toArray())
        out.append(s.toString().trimmed());
    }
    out.removeAll(QString());
    return out;
  };
  q.text = a.value(QStringLiteral("q")).toString().trimmed();
  q.packs = list("pack");
  q.tags = list("tags");
  q.notTags = list("not_tags");
  q.refs = list("refs");
  q.sort = a.value(QStringLiteral("sort")).toString(QStringLiteral("name"));
  q.seed = quint32(a.value(QStringLiteral("seed")).toDouble(0));
  q.alphaOnly = a.value(QStringLiteral("alpha")).toBool(false);
  for (const QString &l: list("licenses"))
    q.licenses.append(licenseKey(l));
  q.freeOnly = a.value(QStringLiteral("free")).toBool(false);
  if (const QJsonValue sky = a.value(QStringLiteral("sky")); sky.isBool())
    q.sky = sky.toBool() ? 1 : 0;
  q.lut = a.value(QStringLiteral("lut")).toBool(false);
  q.offset = std::max(0, a.value(QStringLiteral("offset")).toInt(0));
  q.limit = std::clamp(a.value(QStringLiteral("limit")).toInt(60), 1, 240);
  q.facets = a.value(QStringLiteral("facets")).toBool(false);
  q.facetLimit = std::clamp(a.value(QStringLiteral("facet_limit")).toInt(40), 1, 200);
  return q;
}

QString TagConfig::fold(const QString &tag) const {
  QString t = tag.trimmed().toLower();
  auto it = aliases.constFind(t);
  return it == aliases.constEnd() ? t : it.value();
}

TagConfig TagConfig::loadOrCreate(const QString &path) {
  TagConfig c;
  QFile f(path);
  if (f.open(QIODevice::ReadOnly)) {
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (const auto &v: o.value(QStringLiteral("popular")).toArray())
      c.popular.append(v.toString().toLower());
    for (const auto &v: o.value(QStringLiteral("stoplist")).toArray())
      c.stoplist.insert(v.toString().toLower());
    c.maxTags = std::max(1, o.value(QStringLiteral("max_tags")).toInt(c.maxTags));
    const QJsonObject al = o.value(QStringLiteral("aliases")).toObject();
    for (auto it = al.begin(); it != al.end(); ++it)
      c.aliases.insert(it.key().toLower(), it.value().toString().toLower());
    return c;
  }
  c.popular = kDefaultPopular;
  for (const auto &s: kDefaultStop)
    c.stoplist.insert(s);
  for (const auto &[from, to]: kDefaultAliases)
    c.aliases.insert(from, to);
  // A new file in the server config dir; nothing under data/ is touched.
  QSaveFile out(path);
  if (out.open(QIODevice::WriteOnly)) {
    out.write(QJsonDocument(c.toJson()).toJson(QJsonDocument::Indented));
    out.commit();
    qInfo() << "catalog: wrote default tag config" << path;
  }
  return c;
}

QJsonObject TagConfig::toJson() const {
  QJsonObject o;
  o.insert(QStringLiteral("popular"), QJsonArray::fromStringList(popular));
  o.insert(QStringLiteral("max_tags"), maxTags);
  QStringList stop(stoplist.begin(), stoplist.end());
  stop.sort();
  o.insert(QStringLiteral("stoplist"), QJsonArray::fromStringList(stop));
  QJsonObject al;
  for (auto it = aliases.begin(); it != aliases.end(); ++it)
    al.insert(it.key(), it.value());
  o.insert(QStringLiteral("aliases"), al);
  return o;
}

Catalog &Catalog::instance() {
  static Catalog c;
  return c;
}

namespace {

// Image info cache shared by one load() call: source relPath -> info.
struct InfoCache {
  QHash<QString, serverdb::ImageInfo> known;
  QHash<QString, serverdb::ImageInfo> fresh;  // to store
};

thread_local InfoCache *t_info = nullptr;

serverdb::ImageInfo imageInfoFor(const QString &abs, const QString &rel) {
  const QFileInfo fi(abs);
  const qint64 size = fi.size();
  const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
  if (t_info) {
    auto it = t_info->known.constFind(rel);
    if (it != t_info->known.constEnd() && it->size == size && it->mtime == mtime)
      return *it;
  }
  serverdb::ImageInfo info;
  info.size = size;
  info.mtime = mtime;
  if (abs.endsWith(QLatin1String(".exr"), Qt::CaseInsensitive)) {
    // Skybox panoramas: the header only (QImageReader cannot read EXR).
    const QSize s = hdr::exrSize(abs);
    info.w = s.width();
    info.h = s.height();
    info.alphaChannel = false;
    info.channels = 3;
    info.alphaReal = 0;
  } else {
    QImageReader r(abs);
    const QSize s = r.size();
    info.w = s.width();
    info.h = s.height();
    const QImage::Format fmt = r.imageFormat();
    info.alphaChannel = formatHasAlpha(fmt);
    info.channels = formatChannels(fmt);
    info.alphaReal = info.alphaChannel ? -1 : 0;
  }
  if (t_info) {
    t_info->known.insert(rel, info);
    t_info->fresh.insert(rel, info);
  } else {
    serverdb::storeImageInfo(rel, info);
  }
  return info;
}

} // namespace

QVector<DerivativeSource> Catalog::load(const QString &dataTexturesDir, const QString &cacheDir,
                                        const QString &configDir) {
  QElapsedTimer timer;
  timer.start();
  m_texturesDir = QDir(dataTexturesDir).absolutePath();
  m_thumbsDir = QDir(cacheDir).absoluteFilePath(QStringLiteral("thumbs"));
  m_editorDir = QDir(cacheDir).absoluteFilePath(QStringLiteral("editor"));
  QDir().mkpath(m_thumbsDir);
  QDir().mkpath(m_editorDir);
  m_tags = TagConfig::loadOrCreate(QDir(configDir).absoluteFilePath(QStringLiteral("texture_tags.json")));

  InfoCache cache;
  cache.known = serverdb::loadImageInfo();
  t_info = &cache;

  const QDir root(m_texturesDir);
  QVector<QPair<CatalogPack, QJsonObject>> found;  // pack + its whole JSON
  QSet<QString> claimedDirs;

  // 1. Top-level <pack>.json files (production layout).
  for (const QString &fn: root.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
    if (fn.endsWith(QLatin1String(".bak")))
      continue;
    QFile f(root.absoluteFilePath(fn));
    if (!f.open(QIODevice::ReadOnly))
      continue;
    QJsonParseError pe{};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError) {
      qWarning() << "catalog: cannot parse" << fn << pe.errorString();
      m_loadErrors = true;
      continue;
    }
    const auto p = packFromJson(o, f.fileName(), m_texturesDir);
    if (!p)
      continue;
    claimedDirs.insert(QFileInfo(p->dir).canonicalFilePath());
    found.append({*p, o});
  }

  // 2. Sub-directories with a legacy meta.json (local dev layout).
  for (const QString &dn: root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
    if (dn.startsWith(QLatin1Char('.')))  // .trash, .history
      continue;
    const QString dir = root.absoluteFilePath(dn);
    if (claimedDirs.contains(QFileInfo(dir).canonicalFilePath()))
      continue;
    QFile f(QDir(dir).absoluteFilePath(QStringLiteral("meta.json")));
    if (!f.open(QIODevice::ReadOnly))
      continue;
    QJsonParseError pe{};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError) {
      qWarning() << "catalog: cannot parse" << f.fileName() << pe.errorString();
      m_loadErrors = true;
      continue;
    }
    CatalogPack p;
    p.name = o.value(QStringLiteral("name")).toString(dn);
    p.description = o.value(QStringLiteral("description")).toString();
    p.base = o.value(QStringLiteral("base")).toBool(false);
    p.dir = dir;
    p.jsonPath = f.fileName();
    p.legacyMeta = true;
    found.append({p, o});
  }

  const auto propsRows = serverdb::loadProps();
  const qint64 rev = serverdb::metaInt(QStringLiteral("revision"), 0);

  // Scan without the lock (header reads can take minutes on a cold start);
  // index under it.
  struct Scanned {
    CatalogPack pack;
    Built built;
  };
  QVector<Scanned> scanned;
  QSet<QString> names;
  for (auto &[pack, json]: found) {
    if (names.contains(pack.key())) {
      qWarning() << "catalog: duplicate pack name" << pack.name << pack.type << "- skipped" << pack.jsonPath;
      continue;
    }
    names.insert(pack.key());
    Built b = scanPack(pack, json.value(QStringLiteral("meta")).toObject(), json.value(QStringLiteral("tags")).toArray());
    pack.count = int(b.entries.size());
    qInfo().noquote() << QStringLiteral("catalog: %1 - %2 textures%3")
                             .arg(pack.name)
                             .arg(pack.count)
                             .arg(pack.base ? QStringLiteral(" (base)") : QString());
    scanned.append({pack, std::move(b)});
  }

  QVector<DerivativeSource> missing;
  {
    QWriteLocker lock(&m_lock);
    m_revision = rev;
    for (auto &s: scanned) {
      m_packs.insert(s.pack.key(), s.pack);
      for (auto &e: s.built.entries) {
        auto pr = propsRows.constFind(e.id);
        if (pr != propsRows.constEnd()) {
          e.props = pr->props;
          e.rev = std::max(e.rev, pr->rev);
        }
        indexEntry(e);
      }
      for (const auto &d: s.built.derivs)
        m_derivs.insert(d.file, d);
      missing += missingDerivatives(s.built.derivs);
    }
    ++m_structure;
  }
  m_loaded.store(true);

  t_info = nullptr;
  serverdb::storeImageInfo(cache.fresh);
  qInfo().noquote() << QStringLiteral("catalog: %1 textures in %2 packs, %3 derivatives to build, %4 ms")
                           .arg(m_entries.size())
                           .arg(m_packs.size())
                           .arg(missing.size())
                           .arg(timer.elapsed());
  return missing;
}

std::optional<CatalogPack> Catalog::packFromJson(const QJsonObject &o, const QString &jsonPath, const QString &root) {
  if (!o.value(QStringLiteral("meta")).isObject())
    return std::nullopt;
  const QString type = o.value(QStringLiteral("type")).toString(QStringLiteral("texture"));
  if (type != QLatin1String("texture") && type != QLatin1String("lut"))
    return std::nullopt;
  CatalogPack p;
  p.type = type;
  p.name = o.value(QStringLiteral("name")).toString(QFileInfo(jsonPath).completeBaseName());
  p.description = o.value(QStringLiteral("description")).toString();
  // Base packs say so: "base": true makes a pack read-only.
  p.base = o.value(QStringLiteral("base")).toBool(false);
  p.dir = QDir(root).absoluteFilePath(o.value(QStringLiteral("path")).toString(p.name));
  p.jsonPath = jsonPath;
  p.created = qint64(o.value(QStringLiteral("created")).toDouble());
  for (auto it = o.begin(); it != o.end(); ++it) {
    static const QSet<QString> known = {"name", "description", "base", "path", "type", "tags", "meta", "created"};
    if (!known.contains(it.key()))
      p.extra.insert(it.key(), it.value());
  }
  return p;
}

// Thumbnails of non-color maps are made on demand only (a sky's panorama is its card).
QVector<DerivativeSource> Catalog::missingDerivatives(const QVector<DerivativeSource> &derivs) const {
  QVector<DerivativeSource> missing;
  for (const auto &d: derivs) {
    if (d.thumb && d.type != MapType::Color && d.type != MapType::Panorama && d.type != MapType::Lut)
      continue;
    const QString dir = d.thumb ? m_thumbsDir : m_editorDir;
    if (!QFileInfo::exists(QDir(dir).absoluteFilePath(d.file)))
      missing.append(d);
  }
  return missing;
}

PackScan Catalog::scanPackFile(const QString &jsonPath) {
  PackScan out;
  QFile f(jsonPath);
  if (!f.open(QIODevice::ReadOnly)) {
    out.error = QStringLiteral("cannot read %1").arg(jsonPath);
    return out;
  }
  QJsonParseError pe{};
  const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &pe).object();
  const auto p = pe.error == QJsonParseError::NoError ? packFromJson(o, jsonPath, m_texturesDir) : std::nullopt;
  if (!p) {
    out.error = QStringLiteral("%1 is not a pack JSON").arg(QFileInfo(jsonPath).fileName());
    return out;
  }
  InfoCache cache;
  t_info = &cache;
  Built b = scanPack(*p, o.value(QStringLiteral("meta")).toObject(), o.value(QStringLiteral("tags")).toArray());
  t_info = nullptr;
  serverdb::storeImageInfo(cache.fresh);
  out.pack = *p;
  out.pack.count = int(b.entries.size());
  out.entries = std::move(b.entries);
  out.derivs = std::move(b.derivs);
  return out;
}

bool Catalog::addPack(const PackScan &scan, QVector<DerivativeSource> *missing) {
  QWriteLocker lock(&m_lock);
  if (m_packs.contains(scan.pack.key()))
    return false;
  m_packs.insert(scan.pack.key(), scan.pack);
  for (const auto &e: scan.entries)
    indexEntry(e);
  for (const auto &d: scan.derivs)
    m_derivs.insert(d.file, d);
  if (missing)
    *missing = missingDerivatives(scan.derivs);
  ++m_structure;
  return true;
}

Catalog::Built Catalog::scanPack(const CatalogPack &pack, const QJsonObject &meta, const QJsonArray &tagTable) {
  Built out;
  const QDir dir(pack.dir);
  if (!dir.exists()) {
    qWarning() << "catalog: pack directory missing" << pack.dir;
    m_loadErrors = true;
    return out;
  }
  if (pack.type == QLatin1String("lut")) {
    for (const auto &[name, fn]: lutFiles(pack.dir).asKeyValueRange()) {
      TextureEntry e = buildEntry(pack, name, {{MapType::Lut, fn}}, meta.value(name).toObject(), tagTable, &out.derivs);
      if (!isLutStrip(e)) {
        qWarning().noquote() << QStringLiteral("catalog: %1 - %2 is not a LUT strip (N*N x N)").arg(pack.name, fn);
        continue;
      }
      out.entries.append(e);
    }
    return out;
  }
  // texture name -> chosen file per map type
  QMap<QString, QMap<MapType, ParsedMapFile>> parsed;
  QMap<QString, QMap<MapType, QString>> files;
  int skipped = 0;
  for (const QString &fn: dir.entryList(QDir::Files, QDir::Name)) {
    const ParsedMapFile p = parseMapFileName(fn);
    if (!p.ok || !p.variant.isEmpty()) {
      if (fn.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) ||
          fn.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive) ||
          fn.endsWith(QLatin1String(".exr"), Qt::CaseInsensitive))
        ++skipped;
      continue;
    }
    auto &slot = parsed[p.textureName];
    auto it = slot.find(p.type);
    if (it == slot.end() || preferOver(p, *it)) {
      slot.insert(p.type, p);
      files[p.textureName].insert(p.type, fn);
    }
  }
  int noColor = 0;
  for (auto it = files.begin(); it != files.end(); ++it) {
    // A material needs its color map; a skybox is its panorama.
    if (!it->contains(MapType::Color) && !it->contains(MapType::Panorama)) {
      ++noColor;
      continue;
    }
    out.entries.append(
        buildEntry(pack, it.key(), it.value(), meta.value(it.key()).toObject(), tagTable, &out.derivs));
  }
  if (skipped || noColor)
    qInfo().noquote() << QStringLiteral("catalog: %1 - skipped %2 unrecognised/variant files, %3 textures without a color map")
                             .arg(pack.name)
                             .arg(skipped)
                             .arg(noColor);
  return out;
}

TextureEntry Catalog::buildEntry(const CatalogPack &pack, const QString &textureName,
                                 const QMap<MapType, QString> &files, const QJsonObject &meta,
                                 const QJsonArray &tagTable, QVector<DerivativeSource> *derivs) {
  TextureEntry e;
  e.pack = pack.name;
  e.name = textureName;
  e.ref = MaterialRef::make(pack.name, textureName);
  e.base = pack.base;
  e.id = meta.value(QStringLiteral("id")).toString();
  if (e.id.isEmpty())  // stable even without a pack-provided id
    e.id = QUuid::createUuidV5(QUuid(), e.ref.toUtf8()).toString(QUuid::WithoutBraces);
  e.author = meta.value(QStringLiteral("author")).toString();
  e.license = meta.value(QStringLiteral("license")).toString();
  e.derivedFrom = meta.value(QStringLiteral("derived_from")).toString();
  e.tags = tagsFromMeta(meta, tagTable, m_tags);
  if (e.tags.size() > m_tags.maxTags) {
    // Tag spam (some packs list hundreds of unrelated tags per texture): keep
    // only the tags its name backs up, so it stops matching every filter.
    QStringList words;
    static const QRegularExpression camel(QStringLiteral("[A-Z]?[a-z]+|[A-Z]+(?![a-z])"));
    for (auto it = camel.globalMatch(textureName); it.hasNext();)
      words << m_tags.fold(it.next().captured());
    QStringList kept;
    for (const auto &t: e.tags)
      if (words.contains(t))
        kept << t;
    e.tags = kept;
  }
  e.added = qint64(meta.value(QStringLiteral("added")).toDouble());
  e.sky = meta.value(QStringLiteral("sky")).toObject();
  e.lut = meta.value(QStringLiteral("lut")).toObject();

  const QDir root(m_texturesDir);
  for (auto it = files.begin(); it != files.end(); ++it) {
    const MapType type = it.key();
    const QString abs = QDir(pack.dir).absoluteFilePath(it.value());
    const QString rel = root.relativeFilePath(abs);
    const serverdb::ImageInfo info = imageInfoFor(abs, rel);
    const ParsedMapFile p = parseMapFileName(it.value());
    MapFile f;
    f.file = it.value();
    f.w = info.w;
    f.h = info.h;
    f.directX = p.directX;
    f.alpha = type == MapType::Color && info.alphaReal == 1;
    f.checksum = checksumFor(rel, info.size, info.mtime);
    // Only color maps keep an alpha channel in their derivatives.
    const bool png = type == MapType::Color && info.alphaChannel;
    const QString ext = png ? QStringLiteral("png") : QStringLiteral("jpg");
    f.thumb = f.checksum + QStringLiteral("_256.") + ext;
    if (derivs)
      derivs->append({f.thumb, abs, rel, type, true, false, png});
    if (type == MapType::Panorama) {
      // The 3D view's sky: a 2048 x 1024 half-float EXR (derivatives.cpp).
      f.editor = f.checksum + QStringLiteral("_sky.exr");
      if (derivs)
        derivs->append({f.editor, abs, rel, type, false, false, false});
    } else if (mapTypeRendered(type)) {
      f.editor = f.checksum + QStringLiteral("_512.") + ext;
      if (derivs)
        derivs->append({f.editor, abs, rel, type, false, type == MapType::Normal && p.directX, png});
    }
    e.maps.insert(type, f);
    if (type == MapType::Color || type == MapType::Lut ||
        (type == MapType::Panorama && !files.contains(MapType::Color))) {
      e.w = info.w;
      e.h = info.h;
      e.alpha = f.alpha;
      if (!e.added)
        e.added = QFileInfo(abs).lastModified().toSecsSinceEpoch();
    }
  }
  return e;
}

void Catalog::indexEntry(const TextureEntry &e) {
  const QString key = assetKey(e.ref, e.isLut());
  const QDir root(m_texturesDir);
  const QDir packDir(m_packs.value(packKey(e.pack, e.isLut() ? QStringLiteral("lut") : QStringLiteral("texture"))).dir);
  for (auto it = e.maps.begin(); it != e.maps.end(); ++it)
    m_relToRef.insert(root.relativeFilePath(packDir.absoluteFilePath(it->file)), key);
  m_entries.insert(key, e);
  m_byId.insert(e.id, key);
  const QString bare = e.name.toLower();
  if (!e.isLut() && !m_byName.contains(bare))
    m_byName.insert(bare, key);
  m_aliases.remove(key);
}

void Catalog::unindexEntry(const TextureEntry &e) {
  const QString key = assetKey(e.ref, e.isLut());
  m_entries.remove(key);
  m_byId.remove(e.id);
  const QString bare = e.name.toLower();
  if (m_byName.value(bare) == key)
    m_byName.remove(bare);
  for (const auto &f: e.maps) {
    m_derivs.remove(f.thumb);
    m_derivs.remove(f.editor);
  }
}

bool Catalog::isLoaded() const { return m_loaded.load(); }

qint64 Catalog::revision() const {
  QReadLocker lock(&m_lock);
  return m_revision;
}

int Catalog::textureCount() const {
  QReadLocker lock(&m_lock);
  int n = 0;
  for (const auto &e: m_entries)
    n += e.isLut() ? 0 : 1;
  return n;
}

QVector<CatalogPack> Catalog::packs() const {
  QReadLocker lock(&m_lock);
  QVector<CatalogPack> out;
  for (const auto &p: m_packs)
    out.append(p);
  return out;
}

std::optional<CatalogPack> Catalog::pack(const QString &name, const QString &type) const {
  QReadLocker lock(&m_lock);
  auto it = m_packs.constFind(packKey(name, type));
  if (it == m_packs.constEnd())
    return std::nullopt;
  return *it;
}

TextureEntry Catalog::entry(const QString &ref, bool lut) const {
  QReadLocker lock(&m_lock);
  const MaterialRef r = MaterialRef::parse(ref);
  QString key = assetKey(r.base(), lut);
  if (r.isLegacy() && !lut)
    key = m_byName.value(key);
  for (int hops = 0; hops < 8 && !key.isEmpty(); ++hops) {
    auto it = m_entries.constFind(key);
    if (it != m_entries.constEnd())
      return *it;
    key = m_aliases.value(key);
  }
  return {};
}

TextureEntry Catalog::entryById(const QString &id) const {
  QReadLocker lock(&m_lock);
  return m_entries.value(m_byId.value(id));
}

bool Catalog::derivative(const QString &file, DerivativeSource &out) const {
  QReadLocker lock(&m_lock);
  auto it = m_derivs.constFind(file);
  if (it == m_derivs.constEnd())
    return false;
  out = *it;
  return true;
}

QSet<QString> Catalog::derivativeNames() const {
  QReadLocker lock(&m_lock);
  QSet<QString> out;
  out.reserve(m_derivs.size());
  for (auto it = m_derivs.begin(); it != m_derivs.end(); ++it)
    out.insert(it.key());
  return out;
}

QString Catalog::sourcePath(const QString &ref, MapType type) const {
  const bool lut = type == MapType::Lut;
  const TextureEntry e = entry(ref, lut);
  const MapFile *f = e.map(type);
  if (!f)
    return {};
  const auto p = pack(e.pack, lut ? QStringLiteral("lut") : QStringLiteral("texture"));
  return p ? QDir(p->dir).absoluteFilePath(f->file) : QString();
}

QVector<const TextureEntry *> Catalog::sortedLocked(const QString &sort, quint32 seed) const {
  const bool random = sort == QLatin1String("random");
  const QString cacheKey = random ? QStringLiteral("random:%1").arg(seed) : sort;
  QMutexLocker lock(&m_sortMutex);
  auto it = m_sorted.find(cacheKey);
  if (it != m_sorted.end() && it->first == m_structure)
    return it->second;
  if (random) {
    // A shuffle of the name order: the same seed gives the same order, so the
    // pages of one random view fit together.
    lock.unlock();
    QVector<const TextureEntry *> v = sortedLocked(QStringLiteral("name"));
    std::mt19937 rng(seed);
    std::shuffle(v.begin(), v.end(), rng);
    lock.relock();
    // keep a few random orders, not one per click forever
    for (auto r = m_sorted.begin(); r != m_sorted.end();)
      r = r.key().startsWith(QLatin1String("random:")) && m_sorted.size() > 12 ? m_sorted.erase(r) : std::next(r);
    m_sorted.insert(cacheKey, {m_structure, v});
    return v;
  }
  QVector<const TextureEntry *> v;
  v.reserve(m_entries.size());
  for (auto e = m_entries.constBegin(); e != m_entries.constEnd(); ++e)
    v.append(&e.value());
  auto byName = [](const TextureEntry *a, const TextureEntry *b) {
    const int c = QString::compare(a->name, b->name, Qt::CaseInsensitive);
    return c != 0 ? c < 0 : a->pack < b->pack;
  };
  if (sort == QLatin1String("recent")) {
    std::sort(v.begin(), v.end(), [&](const TextureEntry *a, const TextureEntry *b) {
      return a->added != b->added ? a->added > b->added : byName(a, b);
    });
  } else if (sort == QLatin1String("pack")) {
    std::sort(v.begin(), v.end(), [&](const TextureEntry *a, const TextureEntry *b) {
      const int c = QString::compare(a->pack, b->pack, Qt::CaseInsensitive);
      return c != 0 ? c < 0 : byName(a, b);
    });
  } else {
    std::sort(v.begin(), v.end(), byName);
  }
  m_sorted.insert(cacheKey, {m_structure, v});
  return v;
}

bool Catalog::matches(const TextureEntry &e, const CatalogQuery &q, const QStringList &words,
                      const QStringList &notWords, const QSet<QString> &refs) const {
  if (!refs.isEmpty() && !refs.contains(refKey(e.ref)))
    return false;
  if (q.alphaOnly && !e.alpha && !e.hasMap(MapType::Opacity))
    return false;
  if (q.sky >= 0 && e.isSky() != (q.sky == 1))
    return false;
  if (e.isLut() != q.lut)
    return false;
  if (q.freeOnly || !q.licenses.isEmpty()) {
    const QString lic = licenseKey(e.license);
    if (q.freeOnly && !isFreeLicense(lic))
      return false;
    if (!q.licenses.isEmpty() && !q.licenses.contains(lic))
      return false;
  }
  if (!q.packs.isEmpty()) {
    bool ok = false;
    for (const auto &p: q.packs)
      if (e.pack.compare(p, Qt::CaseInsensitive) == 0) {
        ok = true;
        break;
      }
    if (!ok)
      return false;
  }
  for (const auto &t: q.tags)
    if (!e.tags.contains(m_tags.fold(t)))
      return false;
  for (const auto &t: q.notTags)
    if (e.tags.contains(m_tags.fold(t)))
      return false;
  const QString nameLower = e.name.toLower();
  for (const auto &w: words) {
    if (nameLower.contains(w))
      continue;
    bool hit = false;
    for (const auto &t: e.tags)
      if (t.startsWith(w)) {
        hit = true;
        break;
      }
    if (!hit)
      return false;
  }
  for (const auto &w: notWords)
    if (nameLower.contains(w) || e.tags.contains(w))
      return false;
  return true;
}

QVector<const TextureEntry *> Catalog::filterLocked(const CatalogQuery &q) const {
  QStringList words, notWords;
  for (const auto &tok: q.text.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
    if (tok.startsWith(QLatin1Char('-')) && tok.size() > 1)
      notWords.append(m_tags.fold(tok.mid(1)));
    else if (!tok.startsWith(QLatin1Char('-')))
      words.append(tok);
  }
  QSet<QString> refs;
  for (const auto &r: q.refs)
    refs.insert(refKey(MaterialRef::parse(r).base()));
  QVector<const TextureEntry *> out;
  for (const TextureEntry *e: sortedLocked(q.sort, q.seed))
    if (matches(*e, q, words, notWords, refs))
      out.append(e);
  return out;
}

namespace {

QVector<QPair<QString, int>> topCounts(const QHash<QString, int> &counts, int limit) {
  QVector<QPair<QString, int>> v;
  v.reserve(counts.size());
  for (auto it = counts.begin(); it != counts.end(); ++it)
    v.append({it.key(), it.value()});
  std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  if (limit > 0 && v.size() > limit)
    v.resize(limit);
  return v;
}

} // namespace

CatalogQueryResult Catalog::query(const CatalogQuery &q) const {
  QReadLocker lock(&m_lock);
  const auto hits = filterLocked(q);
  CatalogQueryResult r;
  r.total = int(hits.size());
  for (int i = q.offset; i < hits.size() && i < q.offset + q.limit; ++i)
    r.items.append(*hits[i]);
  if (q.facets) {
    QHash<QString, int> counts;
    const QSet<QString> active(q.tags.begin(), q.tags.end());
    for (const TextureEntry *e: hits)
      for (const auto &t: e->tags)
        if (!m_tags.stoplist.contains(t) && !active.contains(t))
          ++counts[t];
    r.distinctTags = int(counts.size());
    r.facets = topCounts(counts, q.facetLimit);
    // Licenses: over the same filter without the license choice.
    QHash<QString, int> lic;
    if (q.licenses.isEmpty()) {
      for (const TextureEntry *e: hits)
        ++lic[licenseKey(e->license)];
    } else {
      CatalogQuery all = q;
      all.licenses.clear();
      for (const TextureEntry *e: filterLocked(all))
        ++lic[licenseKey(e->license)];
    }
    r.licenseFacets = topCounts(lic, 0);
  }
  return r;
}

QVector<QPair<QString, int>> Catalog::tagSearch(const CatalogQuery &filter, const QString &text, int limit) const {
  QReadLocker lock(&m_lock);
  const QString needle = text.trimmed().toLower();
  QHash<QString, int> counts;
  for (const TextureEntry *e: filterLocked(filter))
    for (const auto &t: e->tags)
      if (!m_tags.stoplist.contains(t) && (needle.isEmpty() || t.contains(needle)))
        ++counts[t];
  auto v = topCounts(counts, 0);
  // prefix matches first, then by count
  if (!needle.isEmpty())
    std::stable_sort(v.begin(), v.end(), [&](const auto &a, const auto &b) {
      return a.first.startsWith(needle) && !b.first.startsWith(needle);
    });
  if (limit > 0 && v.size() > limit)
    v.resize(limit);
  return v;
}

QVector<QPair<QString, int>> Catalog::popular(const CatalogQuery &filter) const {
  QReadLocker lock(&m_lock);
  QHash<QString, int> counts;
  const QSet<QString> wanted(m_tags.popular.begin(), m_tags.popular.end());
  for (const TextureEntry *e: filterLocked(filter))
    for (const auto &t: e->tags)
      if (wanted.contains(t))
        ++counts[t];
  QVector<QPair<QString, int>> out;
  for (const auto &t: m_tags.popular)
    out.append({t, counts.value(t)});
  return out;
}

qint64 Catalog::bumpRevision() {
  qint64 r;
  {
    QWriteLocker lock(&m_lock);
    r = ++m_revision;
  }
  serverdb::setMetaInt(QStringLiteral("revision"), r);
  return r;
}

void Catalog::setImageAlpha(const QString &relPath, bool hasTransparentPixels) {
  QWriteLocker lock(&m_lock);
  const QString key = m_relToRef.value(relPath);
  auto it = m_entries.find(key);
  if (it == m_entries.end())
    return;
  auto c = it->maps.find(MapType::Color);
  if (c == it->maps.end() || c->file != QFileInfo(relPath).fileName())
    return;
  c->alpha = hasTransparentPixels;
  it->alpha = hasTransparentPixels;
}

TextureEntry Catalog::rescanTexture(const CatalogPack &packIn, const QString &textureName, const QJsonObject &meta,
                                    qint64 rev, QVector<DerivativeSource> *needed) {
  QWriteLocker lock(&m_lock);
  auto pit = m_packs.find(packIn.key());
  if (pit == m_packs.end())
    return {};
  const CatalogPack pack = *pit;
  QMap<MapType, ParsedMapFile> parsed;
  QMap<MapType, QString> files;
  const bool lutPack = pack.type == QLatin1String("lut");
  if (lutPack) {
    const auto lf = lutFiles(pack.dir);
    if (auto it = lf.constFind(textureName); it != lf.constEnd())
      files.insert(MapType::Lut, *it);
  }
  for (const QString &fn: lutPack ? QStringList() : QDir(pack.dir).entryList(QDir::Files, QDir::Name)) {
    const ParsedMapFile p = parseMapFileName(fn);
    if (!p.ok || !p.variant.isEmpty() || p.textureName != textureName)
      continue;
    auto it = parsed.find(p.type);
    if (it == parsed.end() || preferOver(p, *it)) {
      parsed.insert(p.type, p);
      files.insert(p.type, fn);
    }
  }
  const QString key = assetKey(MaterialRef::make(pack.name, textureName), lutPack);
  MaterialProps props;
  bool existed = false;
  if (auto old = m_entries.constFind(key); old != m_entries.constEnd()) {
    props = old->props;
    existed = true;
    unindexEntry(*old);
  }
  if (!files.contains(lutPack ? MapType::Lut : MapType::Color)) {
    ++m_structure;
    return {};
  }
  QVector<DerivativeSource> derivs;
  TextureEntry e = buildEntry(pack, textureName, files, meta, {}, &derivs);
  if (lutPack && !isLutStrip(e)) {
    ++m_structure;
    return {};
  }
  e.props = props;
  e.rev = rev;
  // tags in `meta` are already strings for rescans
  indexEntry(e);
  for (const auto &d: derivs) {
    m_derivs.insert(d.file, d);
    if (needed && !(d.thumb && d.type != MapType::Color && d.type != MapType::Lut))
      needed->append(d);
  }
  if (!existed)
    pit->count += 1;
  ++m_structure;
  return e;
}

void Catalog::removeTexture(const QString &ref, bool lut, const QString &aliasTo) {
  QWriteLocker lock(&m_lock);
  const QString key = assetKey(MaterialRef::parse(ref).base(), lut);
  auto it = m_entries.constFind(key);
  if (it == m_entries.constEnd())
    return;
  const QString pack = packKey(it->pack, lut ? QStringLiteral("lut") : QStringLiteral("texture"));
  unindexEntry(*it);
  if (!aliasTo.isEmpty())
    m_aliases.insert(key, assetKey(aliasTo, lut));
  if (auto p = m_packs.find(pack); p != m_packs.end())
    p->count = std::max(0, p->count - 1);
  ++m_structure;
}

void Catalog::addAlias(const QString &oldRef, const QString &newRef, bool lut) {
  QWriteLocker lock(&m_lock);
  const QString from = assetKey(MaterialRef::parse(oldRef).base(), lut);
  const QString to = assetKey(MaterialRef::parse(newRef).base(), lut);
  if (from != to && !m_entries.contains(from))
    m_aliases.insert(from, to);
}

void Catalog::upsertPack(const CatalogPack &p) {
  QWriteLocker lock(&m_lock);
  m_packs.insert(p.key(), p);
}

void Catalog::removePack(const QString &name, const QString &type) {
  QWriteLocker lock(&m_lock);
  const QString lower = name.toLower();
  const bool lut = type == QLatin1String("lut");
  QVector<TextureEntry> gone;
  for (const auto &e: m_entries)
    if (e.isLut() == lut && e.pack.toLower() == lower)
      gone.append(e);
  for (const auto &e: gone)
    unindexEntry(e);
  m_packs.remove(packKey(name, type));
  ++m_structure;
}

void Catalog::setProps(const QString &ref, const MaterialProps &props, qint64 rev) {
  QWriteLocker lock(&m_lock);
  auto it = m_entries.find(refKey(MaterialRef::parse(ref).base()));
  if (it == m_entries.end())
    return;
  it->props = props;
  it->rev = rev;
}

void Catalog::setTags(const QString &ref, bool lut, const QStringList &tags, qint64 rev) {
  QWriteLocker lock(&m_lock);
  auto it = m_entries.find(assetKey(MaterialRef::parse(ref).base(), lut));
  if (it == m_entries.end())
    return;
  QStringList folded;
  for (const auto &t: tags) {
    const QString f = m_tags.fold(t);
    if (!f.isEmpty() && !folded.contains(f))
      folded.append(f);
  }
  it->tags = folded;
  it->rev = rev;
}

QJsonObject Catalog::packMetaJson(const QString &packName, const QString &type) const {
  QReadLocker lock(&m_lock);
  const QString lower = packName.toLower();
  const bool lut = type == QLatin1String("lut");
  QJsonObject meta;
  for (const auto &e: m_entries) {
    if (e.isLut() != lut || e.pack.toLower() != lower)
      continue;
    QJsonObject m;
    m.insert(QStringLiteral("id"), e.id);
    if (!e.author.isEmpty())
      m.insert(QStringLiteral("author"), e.author);
    if (!e.license.isEmpty())
      m.insert(QStringLiteral("license"), e.license);
    if (!e.derivedFrom.isEmpty())
      m.insert(QStringLiteral("derived_from"), e.derivedFrom);
    if (e.added)
      m.insert(QStringLiteral("added"), double(e.added));
    m.insert(QStringLiteral("tags"), QJsonArray::fromStringList(e.tags));
    if (!e.sky.isEmpty())
      m.insert(QStringLiteral("sky"), e.sky);
    if (!e.lut.isEmpty())
      m.insert(QStringLiteral("lut"), e.lut);
    meta.insert(e.name, m);
  }
  return meta;
}
