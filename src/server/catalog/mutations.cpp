#include "mutations.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrent>
#include <QtDebug>

#include "derivatives.h"
#include "server_db.h"
#include "shared/models/pack_archive.h"

namespace {

qint64 ifRevOf(const QJsonObject &args) {
  return args.contains(QStringLiteral("if_rev")) ? qint64(args.value(QStringLiteral("if_rev")).toDouble()) : -1;
}

// "type" of a pack / texture op: "texture" (default) or "lut".
QString typeArg(const QJsonObject &args) {
  return args.value(QStringLiteral("type")).toString() == QLatin1String("lut") ? QStringLiteral("lut")
                                                                               : QStringLiteral("texture");
}

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

QString stamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")); }

// Moves a file aside instead of deleting it. Never overwrites: picks a free name.
bool moveAside(const QString &src, const QString &destDir, const QString &destName) {
  QDir().mkpath(destDir);
  QString target = QDir(destDir).absoluteFilePath(destName);
  for (int i = 1; QFileInfo::exists(target); ++i)
    target = QDir(destDir).absoluteFilePath(QStringLiteral("%1.%2").arg(destName).arg(i));
  return QFile::rename(src, target);
}

QString uniqueTextureName(const CatalogPack &pack, const QString &wanted) {
  const Catalog &c = Catalog::instance();
  auto taken = [&](const QString &n) { return c.entry(MaterialRef::make(pack.name, n), pack.isLut()).isValid(); };
  if (!taken(wanted))
    return wanted;
  for (int i = 2; i < 1000; ++i) {
    const QString n = QStringLiteral("%1_%2").arg(wanted).arg(i);
    if (!taken(n))
      return n;
  }
  return {};
}

QJsonObject metaFromEntry(const TextureEntry &e) {
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
  return m;
}

} // namespace

bool freePackPaths(const QString &name, const QString &type, QString *dir, QString *json) {
  const QDir root(Catalog::instance().texturesDir());
  for (const QString &base: {name, name + QLatin1Char('.') + type}) {
    const QString d = root.absoluteFilePath(base), j = root.absoluteFilePath(base + QStringLiteral(".json"));
    if (!QFileInfo::exists(d) && !QFileInfo::exists(j)) {
      *dir = d;
      *json = j;
      return true;
    }
  }
  return false;
}

QJsonObject ClientRef::toJson() const {
  QJsonObject o;
  o.insert(QStringLiteral("client"), id);
  o.insert(QStringLiteral("name"), name);
  return o;
}

MutationResult MutationResult::fail(const QString &code, const QString &message, int http) {
  MutationResult r;
  r.ok = false;
  r.code = code;
  r.message = message;
  r.httpStatus = http;
  return r;
}

Mutations &Mutations::instance() {
  static Mutations m;
  return m;
}

Mutations::Mutations() {
  m_pool.setMaxThreadCount(1);
  m_pool.setExpiryTimeout(-1);  // keep the one thread (and its sqlite connection)
  m_sweep.setInterval(5000);
  connect(&m_sweep, &QTimer::timeout, this, [this] {
    submit([this] {
      sweepLocks();
      return MutationResult();
    });
  });
  m_sweep.start();
}

QFuture<MutationResult> Mutations::submit(std::function<MutationResult()> fn) {
  return QtConcurrent::run(&m_pool, [fn = std::move(fn)] {
    try {
      return fn();
    } catch (const std::exception &e) {
      return MutationResult::fail(QStringLiteral("io"), QString::fromUtf8(e.what()), 500);
    }
  });
}

void Mutations::clientConnected(const ClientRef &c) {
  QMutexLocker lock(&m_mutex);
  m_clients.insert(c.id, c.name);
  // A reconnecting holder keeps its locks.
  for (auto &l: m_locks)
    if (l.name == c.name && l.expiresAt)
      l.clientId = c.id, l.expiresAt = 0;
}

void Mutations::clientDisconnected(int clientId) {
  QMutexLocker lock(&m_mutex);
  m_clients.remove(clientId);
  for (auto &l: m_locks)
    if (l.clientId == clientId)
      l.expiresAt = nowSecs() + kLockGraceSeconds;
}

QString Mutations::clientName(int clientId) const {
  QMutexLocker lock(&m_mutex);
  return m_clients.value(clientId, QStringLiteral("client %1").arg(clientId));
}

QJsonArray Mutations::eventsSince(qint64 rev, bool *covered) const {
  QMutexLocker lock(&m_mutex);
  QJsonArray out;
  const qint64 current = Catalog::instance().revision();
  if (rev >= current) {
    *covered = rev == current;
    return out;
  }
  if (m_journal.empty() || qint64(m_journal.front().value(QStringLiteral("rev")).toDouble()) > rev + 1) {
    *covered = false;
    return out;
  }
  for (const auto &e: m_journal)
    if (qint64(e.value(QStringLiteral("rev")).toDouble()) > rev)
      out.append(e);
  *covered = true;
  return out;
}

QJsonArray Mutations::locksJson() const {
  QMutexLocker lock(&m_mutex);
  QJsonArray out;
  for (auto it = m_locks.begin(); it != m_locks.end(); ++it) {
    QJsonObject o;
    o.insert(QStringLiteral("ref"), it.key());
    o.insert(QStringLiteral("by"), ClientRef{it->clientId, it->name}.toJson());
    o.insert(QStringLiteral("purpose"), it->purpose);
    o.insert(QStringLiteral("since"), double(it->since));
    out.append(o);
  }
  return out;
}

void Mutations::emitEvent(const QString &ev, qint64 rev, const ClientRef &by, const QJsonObject &data, bool journal) {
  QJsonObject e;
  e.insert(QStringLiteral("ev"), ev);
  e.insert(QStringLiteral("rev"), double(rev));
  e.insert(QStringLiteral("by"), by.toJson());
  e.insert(QStringLiteral("data"), data);
  if (journal) {
    QMutexLocker lock(&m_mutex);
    m_journal.push_back(e);
    while (m_journal.size() > std::size_t(kJournalSize))
      m_journal.pop_front();
  }
  if (broadcast)
    broadcast(e);
}

MutationResult Mutations::requireEditable(const QString &packName, const QString &type) const {
  const auto p = Catalog::instance().pack(packName, type);
  if (!p)
    return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("no pack '%1'").arg(packName), 404);
  if (p->base)
    return MutationResult::fail(QStringLiteral("read_only"),
                                QStringLiteral("'%1' is a base pack and cannot be modified").arg(p->name), 403);
  return {};
}

QString Mutations::trashDir(const QString &what) const {
  return QDir(Catalog::instance().texturesDir())
      .absoluteFilePath(QStringLiteral(".trash/%1-%2").arg(what, stamp()));
}

bool Mutations::writePackJson(const CatalogPack &pack, QString *error) {
  if (pack.base) {  // belt and braces: base JSON is never rewritten
    if (error)
      *error = QStringLiteral("refusing to write base pack JSON");
    return false;
  }
  QJsonObject o = pack.extra;
  o.insert(QStringLiteral("name"), pack.name);
  o.insert(QStringLiteral("description"), pack.description);
  o.insert(QStringLiteral("path"), QDir(Catalog::instance().texturesDir()).relativeFilePath(pack.dir));
  o.insert(QStringLiteral("type"), pack.type);
  o.insert(QStringLiteral("base"), false);
  if (pack.created)
    o.insert(QStringLiteral("created"), double(pack.created));
  o.insert(QStringLiteral("tags"), QJsonArray());  // tags are stored as strings per texture
  o.insert(QStringLiteral("meta"), Catalog::instance().packMetaJson(pack.name, pack.type));
  QSaveFile f(pack.jsonPath);
  if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 || !f.commit()) {
    if (error)
      *error = QStringLiteral("cannot write %1").arg(pack.jsonPath);
    return false;
  }
  return true;
}

void Mutations::sweepLocks() {
  QVector<QPair<QString, Lock>> expired;
  {
    QMutexLocker lock(&m_mutex);
    const qint64 now = nowSecs();
    for (auto it = m_locks.begin(); it != m_locks.end();) {
      if (it->expiresAt && it->expiresAt <= now) {
        expired.append({it.key(), *it});
        it = m_locks.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto &[ref, l]: expired) {
    QJsonObject d;
    d.insert(QStringLiteral("ref"), ref);
    d.insert(QStringLiteral("reason"), QStringLiteral("expired"));
    emitEvent(QStringLiteral("texture.unlocked"), Catalog::instance().revision(), ClientRef{l.clientId, l.name}, d,
              false);
  }
}

QFuture<MutationResult> Mutations::createPack(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const QString name = args.value(QStringLiteral("name")).toString().trimmed();
    if (!isValidAssetName(name))
      return MutationResult::fail(QStringLiteral("invalid"),
                                  QStringLiteral("pack names use letters, digits, '_' and '-' (max 64)"));
    const QString type = args.value(QStringLiteral("type")).toString(QStringLiteral("texture"));
    if (type != QLatin1String("texture") && type != QLatin1String("lut"))
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("unknown pack type '%1'").arg(type));
    if (cat.pack(name, type) || m_installing.contains(packKey(name, type)))
      return MutationResult::fail(QStringLiteral("exists"), QStringLiteral("pack '%1' already exists").arg(name), 409);
    const QDir root(cat.texturesDir());
    CatalogPack p;
    p.name = name;
    p.type = type;
    p.description = args.value(QStringLiteral("description")).toString().trimmed();
    p.base = false;
    p.created = nowSecs();
    if (!freePackPaths(name, type, &p.dir, &p.jsonPath))
      return MutationResult::fail(QStringLiteral("exists"),
                                  QStringLiteral("a file or directory named '%1' already exists").arg(name), 409);
    if (!root.mkdir(QFileInfo(p.dir).fileName()))
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot create %1").arg(p.dir), 500);
    const qint64 rev = cat.bumpRevision();
    p.rev = rev;
    cat.upsertPack(p);
    QString err;
    if (!writePackJson(p, &err)) {
      cat.removePack(p.name, p.type);
      return MutationResult::fail(QStringLiteral("io"), err, 500);
    }
    MutationResult r;
    r.data = p.toJson();
    emitEvent(QStringLiteral("pack.created"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::updatePack(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const QString name = args.value(QStringLiteral("name")).toString();
    if (auto g = requireEditable(name, typeArg(args)); !g.ok)
      return g;
    CatalogPack p = *cat.pack(name, typeArg(args));
    const qint64 ifRev = ifRevOf(args);
    if (ifRev >= 0 && ifRev != p.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"), QStringLiteral("pack was changed by someone else"), 409);
      r.current = p.toJson();
      return r;
    }
    const QString oldName = p.name;
    const QString newName = args.value(QStringLiteral("new_name")).toString(p.name).trimmed();
    if (args.contains(QStringLiteral("description")))
      p.description = args.value(QStringLiteral("description")).toString().trimmed();

    QVector<QPair<QString, QString>> renamed;  // old ref -> new ref
    if (newName != oldName) {
      if (!isValidAssetName(newName))
        return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("invalid pack name"));
      if (newName.compare(oldName, Qt::CaseInsensitive) != 0 &&
          (cat.pack(newName, p.type) || m_installing.contains(packKey(newName, p.type))))
        return MutationResult::fail(QStringLiteral("exists"), QStringLiteral("pack '%1' exists").arg(newName), 409);
      QString newDir, newJson;
      if (!freePackPaths(newName, p.type, &newDir, &newJson))
        return MutationResult::fail(QStringLiteral("exists"), QStringLiteral("'%1' exists on disk").arg(newName), 409);
      // Collect the textures before touching anything.
      CatalogQuery q;
      q.packs = {oldName};
      q.lut = p.isLut();
      q.limit = 240;
      QVector<TextureEntry> entries;
      for (int off = 0;; off += 240) {
        q.offset = off;
        const auto page = cat.query(q);
        entries += page.items;
        if (off + 240 >= page.total)
          break;
      }
      // Rename = move: the directory moves, nothing is deleted. The old JSON
      // goes to the trash, the new one is written below.
      if (!QDir().rename(p.dir, newDir))
        return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot rename %1").arg(p.dir), 500);
      moveAside(p.jsonPath, trashDir(oldName), QFileInfo(p.jsonPath).fileName());
      cat.removePack(oldName, p.type);
      p.name = newName;
      p.dir = newDir;
      p.jsonPath = newJson;
      p.count = 0;
      cat.upsertPack(p);
      const qint64 rev = cat.bumpRevision();
      for (const auto &e: entries) {
        QVector<DerivativeSource> needed;
        TextureEntry ne = cat.rescanTexture(p, e.name, metaFromEntry(e), rev, &needed);
        if (!ne.isValid())
          continue;
        cat.setProps(ne.ref, e.props, rev);
        cat.addAlias(e.ref, ne.ref, p.isLut());
        renamed.append({e.ref, ne.ref});
        DerivativeBuilder::instance().enqueue(needed);
      }
      p.rev = rev;
      cat.upsertPack(p);
      p = *cat.pack(newName, p.type);
    } else {
      p.rev = cat.bumpRevision();
      cat.upsertPack(p);
    }
    QString err;
    if (!writePackJson(p, &err))
      return MutationResult::fail(QStringLiteral("io"), err, 500);
    MutationResult r;
    r.data = p.toJson();
    QJsonObject d = r.data;
    d.insert(QStringLiteral("old_name"), oldName);
    QJsonObject refMap;
    for (const auto &[o, n]: renamed)
      refMap.insert(o, n);
    d.insert(QStringLiteral("renamed"), refMap);
    emitEvent(QStringLiteral("pack.updated"), p.rev, by, d);
    return r;
  });
}

QFuture<MutationResult> Mutations::deletePack(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const QString name = args.value(QStringLiteral("name")).toString();
    if (auto g = requireEditable(name, typeArg(args)); !g.ok)
      return g;
    const CatalogPack p = *cat.pack(name, typeArg(args));
    const qint64 ifRev = ifRevOf(args);
    if (ifRev >= 0 && ifRev != p.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"), QStringLiteral("pack was changed by someone else"), 409);
      r.current = p.toJson();
      return r;
    }
    // Move, never delete: the whole directory and its JSON go to the trash.
    const QString trash = trashDir(p.name);
    QDir().mkpath(QFileInfo(trash).absolutePath());
    if (!QDir().rename(p.dir, trash))
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot move %1 to the trash").arg(p.dir), 500);
    moveAside(p.jsonPath, trash, QFileInfo(p.jsonPath).fileName());
    cat.removePack(p.name, p.type);
    const qint64 rev = cat.bumpRevision();
    MutationResult r;
    r.data.insert(QStringLiteral("name"), p.name);
    r.data.insert(QStringLiteral("type"), p.type);
    r.data.insert(QStringLiteral("trash"), QDir(cat.texturesDir()).relativeFilePath(trash));
    emitEvent(QStringLiteral("pack.deleted"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::copyTexture(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const QString type = typeArg(args);
    const TextureEntry src = cat.entry(args.value(QStringLiteral("from")).toString(), type == QLatin1String("lut"));
    if (!src.isValid())
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("source texture not found"), 404);
    const QString packName = args.value(QStringLiteral("pack")).toString();
    if (auto g = requireEditable(packName, type); !g.ok)
      return g;
    const CatalogPack pack = *cat.pack(packName, type);
    const auto srcPack = cat.pack(src.pack, type);
    if (!srcPack)
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("source pack not found"), 404);
    if (srcPack->type != pack.type)
      return MutationResult::fail(QStringLiteral("invalid"),
                                  QStringLiteral("a %1 cannot go into a %2 pack")
                                      .arg(src.isLut() ? QStringLiteral("LUT") : QStringLiteral("texture"), pack.type));

    QString name = args.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
      name = uniqueTextureName(pack, src.name);
    if (!isValidAssetName(name))
      return MutationResult::fail(QStringLiteral("invalid"),
                                  QStringLiteral("texture names use letters, digits, '_' and '-' (max 64)"));
    if (cat.entry(MaterialRef::make(pack.name, name), pack.isLut()).isValid())
      return MutationResult::fail(QStringLiteral("exists"),
                                  QStringLiteral("'%1' already exists in %2").arg(name, pack.name), 409);

    // Copy every map, rendered or not (displacement, scattering, ...).
    QStringList created;
    for (auto it = src.maps.begin(); it != src.maps.end(); ++it) {
      const ParsedMapFile pf = parseMapFileName(it->file);
      const QString dstName = it.key() == MapType::Lut
                                  ? name + QStringLiteral(".png")
                                  : makeMapFileName(name, pf.ok ? pf.sizeTag : QStringLiteral("1K"), it.key(),
                                                    it->directX, pf.ok ? pf.ext : QStringLiteral("png"));
      const QString dst = QDir(pack.dir).absoluteFilePath(dstName);
      if (QFileInfo::exists(dst) || !QFile::copy(QDir(srcPack->dir).absoluteFilePath(it->file), dst)) {
        for (const auto &c: created)  // only files this call just created
          moveAside(c, trashDir(QStringLiteral("failed-copy")), QFileInfo(c).fileName());
        return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot copy %1").arg(it->file), 500);
      }
      created.append(dst);
    }

    TextureEntry meta = src;
    meta.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    meta.derivedFrom = src.ref;
    meta.added = nowSecs();
    const qint64 rev = cat.bumpRevision();
    QVector<DerivativeSource> needed;
    TextureEntry e = cat.rescanTexture(pack, name, metaFromEntry(meta), rev, &needed);
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("copied files were not recognised"), 500);

    // The copies are byte-identical: reuse the source's derivatives under the
    // new names instead of re-encoding them.
    QVector<DerivativeSource> rest;
    for (const auto &d: needed) {
      const MapFile *sf = src.map(d.type);
      const QString from = sf ? DerivativeBuilder::instance().pathFor(
                                    DerivativeSource{d.thumb ? sf->thumb : sf->editor, {}, {}, d.type, d.thumb})
                              : QString();
      const QString to = DerivativeBuilder::instance().pathFor(d);
      if (!from.isEmpty() && QFileInfo::exists(from) && !QFileInfo::exists(to) && QFile::copy(from, to))
        continue;
      rest.append(d);
    }
    DerivativeBuilder::instance().enqueue(rest);

    if (args.value(QStringLiteral("copy_props")).toBool(true) && !src.props.isEmpty()) {
      serverdb::storeProps(e.id, e.ref, src.props, rev);
      cat.setProps(e.ref, src.props, rev);
      e.props = src.props;
    }
    CatalogPack p = *cat.pack(pack.name, pack.type);
    p.rev = rev;
    cat.upsertPack(p);
    QString err;
    if (!writePackJson(p, &err))
      return MutationResult::fail(QStringLiteral("io"), err, 500);
    MutationResult r;
    r.data.insert(QStringLiteral("entry"), e.toJson());
    emitEvent(QStringLiteral("texture.created"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::updateTexture(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const QString type = typeArg(args);
    const bool lut = type == QLatin1String("lut");
    const TextureEntry e = cat.entry(args.value(QStringLiteral("ref")).toString(), lut);
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("texture not found"), 404);
    if (auto g = requireEditable(e.pack, type); !g.ok)
      return g;
    const qint64 ifRev = ifRevOf(args);
    if (ifRev >= 0 && ifRev != e.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"), QStringLiteral("texture was changed by someone else"),
                                    409);
      r.current = e.toJson();
      return r;
    }
    const CatalogPack pack = *cat.pack(e.pack, type);
    TextureEntry meta = e;
    if (args.contains(QStringLiteral("tags"))) {
      meta.tags.clear();
      for (const auto &t: args.value(QStringLiteral("tags")).toArray())
        meta.tags.append(t.toString());
    }
    const QString newName = args.value(QStringLiteral("name")).toString(e.name).trimmed();
    if (newName != e.name) {
      if (!isValidAssetName(newName))
        return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("invalid texture name"));
      if (newName.compare(e.name, Qt::CaseInsensitive) != 0 &&
          cat.entry(MaterialRef::make(pack.name, newName), lut).isValid())
        return MutationResult::fail(QStringLiteral("exists"), QStringLiteral("'%1' exists").arg(newName), 409);
    }
    const qint64 rev = cat.bumpRevision();
    TextureEntry ne;
    if (newName != e.name) {
      // Rename every map file (a move within the pack, never a delete).
      for (const auto &f: e.maps) {
        const ParsedMapFile pf = parseMapFileName(f.file);
        const QString to = e.isLut() ? newName + QStringLiteral(".png")
                                     : makeMapFileName(newName, pf.sizeTag, pf.type, pf.directX, pf.ext);
        if (QFileInfo::exists(QDir(pack.dir).absoluteFilePath(to)) ||
            !QFile::rename(QDir(pack.dir).absoluteFilePath(f.file), QDir(pack.dir).absoluteFilePath(to)))
          return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot rename %1").arg(f.file), 500);
      }
      cat.removeTexture(e.ref, lut);
      QVector<DerivativeSource> needed;
      ne = cat.rescanTexture(pack, newName, metaFromEntry(meta), rev, &needed);
      cat.addAlias(e.ref, ne.ref, lut);
      cat.setProps(ne.ref, e.props, rev);
      ne.props = e.props;
      DerivativeBuilder::instance().enqueue(needed);
      if (!e.props.isEmpty())
        serverdb::storeProps(e.id, ne.ref, e.props, rev);
    } else {
      cat.setTags(e.ref, lut, meta.tags, rev);
      ne = cat.entry(e.ref, lut);
    }
    CatalogPack p = *cat.pack(pack.name, type);
    p.rev = rev;
    cat.upsertPack(p);
    QString err;
    if (!writePackJson(p, &err))
      return MutationResult::fail(QStringLiteral("io"), err, 500);
    MutationResult r;
    r.data.insert(QStringLiteral("entry"), ne.toJson());
    if (ne.ref != e.ref)
      r.data.insert(QStringLiteral("old_ref"), e.ref);
    emitEvent(QStringLiteral("texture.updated"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::deleteTexture(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const QString type = typeArg(args);
    const bool lut = type == QLatin1String("lut");
    const TextureEntry e = cat.entry(args.value(QStringLiteral("ref")).toString(), lut);
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("texture not found"), 404);
    if (auto g = requireEditable(e.pack, type); !g.ok)
      return g;
    const qint64 ifRev = ifRevOf(args);
    if (ifRev >= 0 && ifRev != e.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"), QStringLiteral("texture was changed by someone else"),
                                    409);
      r.current = e.toJson();
      return r;
    }
    const CatalogPack pack = *cat.pack(e.pack, type);
    // Move every map file to the trash; nothing is deleted.
    const QString trash = trashDir(e.pack + QLatin1Char('_') + e.name);
    for (const auto &f: e.maps)
      if (!moveAside(QDir(pack.dir).absoluteFilePath(f.file), trash, f.file))
        return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot move %1").arg(f.file), 500);
    cat.removeTexture(e.ref, lut);
    const qint64 rev = cat.bumpRevision();
    CatalogPack p = *cat.pack(pack.name, type);
    p.rev = rev;
    cat.upsertPack(p);
    QString err;
    if (!writePackJson(p, &err))
      return MutationResult::fail(QStringLiteral("io"), err, 500);
    MutationResult r;
    r.data.insert(QStringLiteral("ref"), e.ref);
    r.data.insert(QStringLiteral("id"), e.id);
    r.data.insert(QStringLiteral("type"), type);
    emitEvent(QStringLiteral("texture.deleted"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::setProps(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    Catalog &cat = Catalog::instance();
    const TextureEntry e = cat.entry(args.value(QStringLiteral("ref")).toString());
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("texture not found"), 404);
    const qint64 ifRev = ifRevOf(args);
    if (ifRev >= 0 && ifRev != e.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"),
                                    QStringLiteral("properties were changed by someone else"), 409);
      r.current = e.toJson();
      return r;
    }
    QStringList errors;
    const MaterialProps props =
        MaterialProps::fromJson(args.value(QStringLiteral("props")).toObject(), &errors, true);
    if (!errors.isEmpty())
      return MutationResult::fail(QStringLiteral("invalid"), errors.join(QStringLiteral("; ")));
    // Base textures may carry server overrides: they live in sqlite, the
    // base pack files and JSON stay untouched.
    const qint64 rev = cat.bumpRevision();
    if (props.isEmpty())
      serverdb::deleteProps(e.id);
    else if (!serverdb::storeProps(e.id, e.ref, props, rev))
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot store properties"), 500);
    cat.setProps(e.ref, props, rev);
    MutationResult r;
    r.data.insert(QStringLiteral("ref"), e.ref);
    r.data.insert(QStringLiteral("id"), e.id);
    r.data.insert(QStringLiteral("props"), props.toJson());
    r.data.insert(QStringLiteral("rev"), double(rev));
    emitEvent(QStringLiteral("texture.props"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::clearProps(const QJsonObject &args, const ClientRef &by) {
  QJsonObject a = args;
  a.insert(QStringLiteral("props"), QJsonObject());
  return setProps(a, by);
}

QFuture<MutationResult> Mutations::lockTexture(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    const TextureEntry e = Catalog::instance().entry(args.value(QStringLiteral("ref")).toString());
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("texture not found"), 404);
    const QString key = refKey(e.ref);
    const bool takeOver = args.value(QStringLiteral("take_over")).toBool(false);
    Lock previous;
    bool hadOther = false;
    {
      QMutexLocker lock(&m_mutex);
      auto it = m_locks.constFind(key);
      if (it != m_locks.constEnd() && it->clientId != by.id) {
        if (!takeOver) {
          auto r = MutationResult::fail(QStringLiteral("locked"),
                                        QStringLiteral("%1 is editing this texture").arg(it->name), 423);
          r.current.insert(QStringLiteral("by"), ClientRef{it->clientId, it->name}.toJson());
          r.current.insert(QStringLiteral("purpose"), it->purpose);
          return r;
        }
        previous = *it;
        hadOther = true;
      }
      Lock l;
      l.clientId = by.id;
      l.name = by.name;
      l.purpose = args.value(QStringLiteral("purpose")).toString(QStringLiteral("edit"));
      l.since = nowSecs();
      m_locks.insert(key, l);
    }
    QJsonObject d;
    d.insert(QStringLiteral("ref"), e.ref);
    d.insert(QStringLiteral("purpose"), args.value(QStringLiteral("purpose")).toString(QStringLiteral("edit")));
    if (hadOther)
      d.insert(QStringLiteral("taken_from"), ClientRef{previous.clientId, previous.name}.toJson());
    emitEvent(QStringLiteral("texture.locked"), Catalog::instance().revision(), by, d, false);
    MutationResult r;
    r.data = d;
    return r;
  });
}

QFuture<MutationResult> Mutations::unlockTexture(const QJsonObject &args, const ClientRef &by) {
  return submit([this, args, by] {
    const QString ref = args.value(QStringLiteral("ref")).toString();
    const TextureEntry e = Catalog::instance().entry(ref);
    const QString key = refKey(e.isValid() ? e.ref : ref);
    {
      QMutexLocker lock(&m_mutex);
      auto it = m_locks.constFind(key);
      if (it == m_locks.constEnd())
        return MutationResult();
      if (it->clientId != by.id)
        return MutationResult::fail(QStringLiteral("locked"), QStringLiteral("the lock belongs to %1").arg(it->name),
                                    423);
      m_locks.remove(key);
    }
    QJsonObject d;
    d.insert(QStringLiteral("ref"), e.isValid() ? e.ref : ref);
    emitEvent(QStringLiteral("texture.unlocked"), Catalog::instance().revision(), by, d, false);
    MutationResult r;
    r.data = d;
    return r;
  });
}

QFuture<MutationResult> Mutations::uploadMap(const QString &ref, const QString &mapType, const QByteArray &bytes,
                                            qint64 ifRev, const ClientRef &by) {
  return submit([this, ref, mapType, bytes, ifRev, by] {
    Catalog &cat = Catalog::instance();
    const TextureEntry e = cat.entry(ref);
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("not_found"), QStringLiteral("texture not found"), 404);
    if (auto g = requireEditable(e.pack); !g.ok)
      return g;
    MapType type;
    if (!mapTypeFromKey(mapType, type) || type == MapType::Lut || e.isLut())
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("unknown map type '%1'").arg(mapType));
    {
      QMutexLocker lock(&m_mutex);
      auto it = m_locks.constFind(refKey(e.ref));
      if (it != m_locks.constEnd() && it->clientId != by.id) {
        auto r = MutationResult::fail(QStringLiteral("locked"),
                                      QStringLiteral("%1 holds the edit lock").arg(it->name), 423);
        r.current.insert(QStringLiteral("by"), ClientRef{it->clientId, it->name}.toJson());
        return r;
      }
    }
    if (ifRev >= 0 && ifRev != e.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"), QStringLiteral("texture was changed by someone else"),
                                    409);
      r.current = e.toJson();
      return r;
    }
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    const QByteArray fmt = reader.format().toLower();
    if (fmt != "png" && fmt != "jpeg" && fmt != "jpg")
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("upload a PNG or JPEG image"));
    const QImage probe = reader.read();
    if (probe.isNull())
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("the image cannot be decoded"));
    const QString ext = fmt == "png" ? QStringLiteral("png") : QStringLiteral("jpg");

    const CatalogPack pack = *cat.pack(e.pack);
    const MapFile *old = e.map(type);
    const MapFile *base = old ? old : e.map(MapType::Color);
    if (!base)
      base = e.map(MapType::Panorama);
    if (!base)
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("the texture has no map to size this one by"));
    const QString sizeTag = parseMapFileName(base->file).sizeTag;
    const bool dx = old && old->directX;
    const QString newName = makeMapFileName(e.name, sizeTag, type, dx, ext);
    const qint64 rev = cat.bumpRevision();
    // The previous file moves to .history (kept, never deleted).
    if (old) {
      const QString histDir = QDir(cat.texturesDir())
                                  .absoluteFilePath(QStringLiteral(".history/%1/%2").arg(pack.name, e.name));
      if (!moveAside(QDir(pack.dir).absoluteFilePath(old->file), histDir,
                     QStringLiteral("%1.r%2").arg(old->file).arg(e.rev)))
        return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot archive %1").arg(old->file), 500);
    }
    const QString dst = QDir(pack.dir).absoluteFilePath(newName);
    if (QFileInfo::exists(dst))  // a same-type file with the other extension
      moveAside(dst, QDir(cat.texturesDir()).absoluteFilePath(QStringLiteral(".history/%1/%2").arg(pack.name, e.name)),
                QStringLiteral("%1.r%2").arg(newName).arg(e.rev));
    QSaveFile out(dst);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot write %1").arg(newName), 500);

    QVector<DerivativeSource> needed;
    TextureEntry ne = cat.rescanTexture(pack, e.name, metaFromEntry(e), rev, &needed);
    // Build this texture's editor maps right away so every editor that reacts
    // to the event finds them; thumbnails follow in the background.
    QVector<DerivativeSource> later;
    for (const auto &d: needed) {
      if (!d.thumb)
        DerivativeBuilder::instance().ensure(d);
      else
        later.append(d);
    }
    DerivativeBuilder::instance().enqueue(later);
    MutationResult r;
    r.data.insert(QStringLiteral("entry"), ne.toJson());
    r.data.insert(QStringLiteral("map"), mapType);
    emitEvent(QStringLiteral("texture.updated"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::putLut(const QString &packName, const QString &name, const QByteArray &png,
                                          const QJsonObject &grade, qint64 ifRev, const ClientRef &by) {
  return submit([this, packName, name, png, grade, ifRev, by] {
    Catalog &cat = Catalog::instance();
    if (auto g = requireEditable(packName, QStringLiteral("lut")); !g.ok)
      return g;
    const CatalogPack pack = *cat.pack(packName, QStringLiteral("lut"));
    if (pack.type != QLatin1String("lut"))
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("'%1' is not a LUT pack").arg(pack.name));
    if (!isValidAssetName(name))
      return MutationResult::fail(QStringLiteral("invalid"),
                                  QStringLiteral("LUT names use letters, digits, '_' and '-' (max 64)"));
    QBuffer buf;
    buf.setData(png);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "png");
    const QImage probe = reader.read();
    if (probe.isNull())
      return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("upload a PNG image"));
    const int n = probe.height();
    if (n < 2 || n > 256 || probe.width() != n * n)
      return MutationResult::fail(QStringLiteral("invalid"),
                                  QStringLiteral("a LUT is a strip of N slices of N x N (got %1 x %2)")
                                      .arg(probe.width())
                                      .arg(probe.height()));

    const QString ref = MaterialRef::make(pack.name, name);
    const TextureEntry old = cat.entry(ref, true);
    const bool existed = old.isValid() && old.pack.compare(pack.name, Qt::CaseInsensitive) == 0;
    if (existed && ifRev >= 0 && ifRev != old.rev) {
      auto r = MutationResult::fail(QStringLiteral("conflict"), QStringLiteral("the LUT was changed by someone else"),
                                    409);
      r.current = old.toJson();
      return r;
    }
    const QString fileName = (existed ? old.name : name) + QStringLiteral(".png");
    const QString dst = QDir(pack.dir).absoluteFilePath(fileName);
    const qint64 rev = cat.bumpRevision();
    // An imported strip cannot be made again: keep the old one. A graded one can (from its grade).
    if (existed && old.lut.value(QStringLiteral("grade")).toObject().isEmpty() && QFileInfo::exists(dst)) {
      const QString histDir =
          QDir(cat.texturesDir()).absoluteFilePath(QStringLiteral(".history/%1/%2").arg(pack.name, old.name));
      if (!moveAside(dst, histDir, QStringLiteral("%1.r%2").arg(fileName).arg(old.rev)))
        return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot archive %1").arg(fileName), 500);
    }
    QSaveFile out(dst);
    if (!out.open(QIODevice::WriteOnly) || out.write(png) != png.size() || !out.commit())
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot write %1").arg(fileName), 500);

    QJsonObject meta;
    if (existed) {
      meta = metaFromEntry(old);
    } else {
      meta.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
      meta.insert(QStringLiteral("added"), double(nowSecs()));
      meta.insert(QStringLiteral("tags"), QJsonArray());
    }
    QJsonObject lut;
    if (!grade.isEmpty())
      lut.insert(QStringLiteral("grade"), grade);
    if (lut.isEmpty())
      meta.remove(QStringLiteral("lut"));
    else
      meta.insert(QStringLiteral("lut"), lut);

    QVector<DerivativeSource> needed;
    TextureEntry e = cat.rescanTexture(pack, existed ? old.name : name, meta, rev, &needed);
    if (!e.isValid())
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("the LUT was not recognised"), 500);
    // Its card right away: every editor that reacts to the event can show it.
    for (const auto &d: needed)
      DerivativeBuilder::instance().ensure(d);
    CatalogPack p = *cat.pack(pack.name, QStringLiteral("lut"));
    p.rev = rev;
    cat.upsertPack(p);
    QString err;
    if (!writePackJson(p, &err))
      return MutationResult::fail(QStringLiteral("io"), err, 500);
    MutationResult r;
    r.data.insert(QStringLiteral("entry"), e.toJson());
    emitEvent(existed ? QStringLiteral("texture.updated") : QStringLiteral("texture.created"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::placeInstalledPack(const QString &stagedDir, const QJsonObject &json,
                                                      const QString &name, const QString &type) {
  return submit([this, stagedDir, json, name, type] {
    Catalog &cat = Catalog::instance();
    const QString key = packKey(name, type);
    if (cat.pack(name, type) || m_installing.contains(key))
      return MutationResult::fail(QStringLiteral("exists"),
                                  QStringLiteral("%1 '%2' is already installed").arg(packTypeLabel(type), name), 409);
    QString dir, jsonPath;
    if (!freePackPaths(name, type, &dir, &jsonPath))
      return MutationResult::fail(QStringLiteral("exists"),
                                  QStringLiteral("a file or directory named '%1' already exists").arg(name), 409);
    if (!QDir().rename(stagedDir, dir))
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot move the pack into place"), 500);
    QJsonObject o = json;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("path"), QFileInfo(dir).fileName());
    QSaveFile f(jsonPath);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 || !f.commit()) {
      QDir().rename(dir, stagedDir);
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot write %1").arg(jsonPath), 500);
    }
    m_installing.insert(key);
    MutationResult r;
    r.data.insert(QStringLiteral("dir"), dir);
    r.data.insert(QStringLiteral("json"), jsonPath);
    return r;
  });
}

QFuture<MutationResult> Mutations::addInstalledPack(const PackScan &scan, const ClientRef &by) {
  return submit([this, scan, by] {
    Catalog &cat = Catalog::instance();
    m_installing.remove(scan.pack.key());
    PackScan s = scan;
    const qint64 rev = cat.bumpRevision();
    s.pack.rev = rev;
    if (!s.pack.created)
      s.pack.created = nowSecs();
    for (auto &e: s.entries)
      e.rev = rev;
    QVector<DerivativeSource> missing;
    if (!cat.addPack(s, &missing))
      return MutationResult::fail(QStringLiteral("exists"),
                                  QStringLiteral("%1 '%2' is already installed").arg(packTypeLabel(s.pack.type), s.pack.name),
                                  409);
    DerivativeBuilder::instance().enqueue(missing);
    qInfo().noquote() << QStringLiteral("install: %1 (%2) - %3 assets, %4 derivatives to build")
                             .arg(s.pack.name, s.pack.type)
                             .arg(s.pack.count)
                             .arg(missing.size());
    MutationResult r;
    r.data = s.pack.toJson();
    emitEvent(QStringLiteral("pack.created"), rev, by, r.data);
    return r;
  });
}

QFuture<MutationResult> Mutations::abandonInstalledPack(const QString &name, const QString &type, const QString &dir,
                                                        const QString &jsonPath) {
  return submit([this, name, type, dir, jsonPath] {
    m_installing.remove(packKey(name, type));
    const QString trash = trashDir(QStringLiteral("failed-install-") + name);
    QDir().mkpath(QFileInfo(trash).absolutePath());
    QDir().rename(dir, trash);
    moveAside(jsonPath, trash, QFileInfo(jsonPath).fileName());
    return MutationResult{};
  });
}
