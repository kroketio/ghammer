#include "texture_catalog.h"

#include <QtDebug>

namespace net {

namespace {

QVector<QPair<QString, int>> pairsFrom(const QJsonArray &a) {
  QVector<QPair<QString, int>> out;
  out.reserve(a.size());
  for (const auto &v: a) {
    const QJsonArray p = v.toArray();
    out.append({p.at(0).toString(), p.at(1).toInt()});
  }
  return out;
}

QString baseKey(const QString &ref, bool lut = false) { return assetKey(MaterialRef::parse(ref).base(), lut); }

const QString kLutPrefix = QStringLiteral("lut:");

QString queued(const QString &ref, bool lut) { return lut ? kLutPrefix + ref : ref; }

} // namespace

TextureCatalog &TextureCatalog::instance() {
  static TextureCatalog *c = new TextureCatalog();
  return *c;
}

TextureCatalog::TextureCatalog() : QObject(nullptr) {
  auto &client = AssetClient::instance();
  connect(&client, &AssetClient::serverEvent, this, &TextureCatalog::onServerEvent);
  connect(&client, &AssetClient::resyncRequired, this, &TextureCatalog::onResync);
  connect(&client, &AssetClient::stateChanged, this, [this](AssetClient::State s) {
    if (s == AssetClient::State::Offline && !m_resolveWaiters.empty()) {
      // Tell the waiters now; their refs stay queued for the next connection.
      auto waiters = std::move(m_resolveWaiters);
      m_resolveWaiters.clear();
      for (auto &w: waiters)
        if (w.done && (!w.hasContext || w.context))
          w.done();
    }
    if (s == AssetClient::State::Online) {
      refreshPacks();
      m_missing.clear();
      if (!m_resolveQueue.isEmpty())
        m_resolveTimer.start();
    }
  });
  connect(&client, &AssetClient::locksReset, this, [this](const QJsonArray &locks) {
    QSet<QString> touched(m_locks.keyBegin(), m_locks.keyEnd());
    m_locks.clear();
    for (const auto &v: locks) {
      const QJsonObject o = v.toObject();
      LockInfo l;
      l.ref = o.value(QStringLiteral("ref")).toString();
      const QJsonObject by = o.value(QStringLiteral("by")).toObject();
      l.clientId = by.value(QStringLiteral("client")).toInt();
      l.name = by.value(QStringLiteral("name")).toString();
      l.purpose = o.value(QStringLiteral("purpose")).toString();
      m_locks.insert(baseKey(l.ref), l);
      touched.insert(baseKey(l.ref));
    }
    for (const auto &k: touched)
      emit lockChanged(k);
  });
  connect(&client, &AssetClient::endpointChanged, this, [this] {
    m_entries.clear();
    m_aliases.clear();
    m_missing.clear();
    m_resolving.clear();
    m_packs.clear();
    const QList<QString> locked = m_locks.keys();
    m_locks.clear();
    for (const auto &k: locked)
      emit lockChanged(k);
    emit packsChanged();
    emit serverChanged();
  });
  m_resolveTimer.setSingleShot(true);
  m_resolveTimer.setInterval(30);
  connect(&m_resolveTimer, &QTimer::timeout, this, &TextureCatalog::flushResolve);
  m_packRefresh.setSingleShot(true);
  m_packRefresh.setInterval(250);
  connect(&m_packRefresh, &QTimer::timeout, this, &TextureCatalog::refreshPacks);
}

QString TextureCatalog::canonicalKey(const QString &ref, bool lut) const {
  QString key = baseKey(ref, lut);
  for (int hops = 0; hops < 8; ++hops) {
    if (m_entries.contains(key))
      return key;
    auto a = m_aliases.constFind(key);
    if (a == m_aliases.constEnd())
      return key;
    key = *a;
  }
  return key;
}

const TextureEntry *TextureCatalog::find(const QString &ref, bool lut) const {
  if (ref.isEmpty())
    return nullptr;
  auto it = m_entries.constFind(canonicalKey(ref, lut));
  return it == m_entries.constEnd() ? nullptr : &it.value();
}

TextureEntry TextureCatalog::entry(const QString &ref, bool lut) const {
  const TextureEntry *e = find(ref, lut);
  return e ? *e : TextureEntry();
}

bool TextureCatalog::isKnownMissing(const QString &ref, bool lut) const {
  return m_missing.contains(baseKey(ref, lut));
}

const PackInfo *TextureCatalog::pack(const QString &name, const QString &type) const {
  for (const auto &p: m_packs)
    if (p.type == type && p.name.compare(name, Qt::CaseInsensitive) == 0)
      return &p;
  return nullptr;
}

const LockInfo *TextureCatalog::lockFor(const QString &ref) const {
  auto it = m_locks.constFind(canonicalKey(ref));
  return it == m_locks.constEnd() ? nullptr : &it.value();
}

void TextureCatalog::store(const TextureEntry &e) {
  const QString key = assetKey(e.ref, e.isLut());
  m_entries.insert(key, e);
  m_missing.remove(key);
  m_aliases.remove(key);
}

void TextureCatalog::resolve(const QStringList &refs, QObject *context, std::function<void()> done, bool lut) {
  ResolveWaiter w;
  w.context = context;
  w.hasContext = context != nullptr;
  w.done = std::move(done);
  for (const auto &r: refs) {
    if (r.trimmed().isEmpty())
      continue;
    if (find(r, lut) || m_missing.contains(baseKey(r, lut)))
      continue;
    const QString key = baseKey(r, lut);
    w.keys.insert(key);
    if (!m_resolving.contains(key))
      m_resolveQueue.insert(queued(MaterialRef::parse(r).base(), lut));
  }
  if (w.keys.isEmpty()) {
    if (w.done)
      QTimer::singleShot(0, context ? context : this, w.done);
    return;
  }
  if (AssetClient::instance().state() == AssetClient::State::Offline) {
    if (w.done)
      QTimer::singleShot(0, context ? context : this, w.done);
    return;  // the refs stay queued for the next connection
  }
  m_resolveWaiters.push_back(std::move(w));
  if (AssetClient::instance().isOnline())
    m_resolveTimer.start();
}

void TextureCatalog::flushResolve() {
  if (m_resolveQueue.isEmpty())
    return;
  QStringList textures, luts;
  for (const QString &q: std::as_const(m_resolveQueue))
    (q.startsWith(kLutPrefix) ? luts : textures).append(q.startsWith(kLutPrefix) ? q.mid(kLutPrefix.size()) : q);
  m_resolveQueue.clear();
  if (!textures.isEmpty())
    sendResolve(textures, false);
  if (!luts.isEmpty())
    sendResolve(luts, true);
}

void TextureCatalog::sendResolve(const QStringList &refs, bool lut) {
  for (const auto &r: refs)
    m_resolving.insert(baseKey(r, lut));
  QJsonObject args;
  args.insert(QStringLiteral("refs"), QJsonArray::fromStringList(refs));
  if (lut)
    args.insert(QStringLiteral("type"), QStringLiteral("lut"));
  AssetClient::instance().call(QStringLiteral("textures.resolve"), args, this, [this, refs, lut](const Result &r) {
    QStringList answered;
    for (const auto &ref: refs) {
      const QString key = baseKey(ref, lut);
      m_resolving.remove(key);
      if (!r.ok)
        continue;  // offline / timeout: stays unresolved, retried on reconnect
      const QJsonObject e = r.data.value(QStringLiteral("entries")).toObject().value(ref).toObject();
      if (e.isEmpty()) {
        m_missing.insert(key);
      } else {
        const TextureEntry entry = TextureEntry::fromJson(e);
        store(entry);
        if (assetKey(entry.ref, lut) != key)
          m_aliases.insert(key, assetKey(entry.ref, lut));  // legacy name or old ref
      }
      answered.append(ref);
    }
    if (!r.ok) {
      // keep them queued for the next connection
      for (const auto &ref: refs)
        m_resolveQueue.insert(queued(ref, lut));
    }
    // Wake waiters whose keys are all answered (or cannot be answered now).
    std::vector<ResolveWaiter> still;
    for (auto &w: m_resolveWaiters) {
      bool pending = false;
      for (const auto &k: w.keys)
        if (m_resolving.contains(k) || (r.ok && m_resolveQueue.contains(k)))
          pending = true;
      if (pending && r.ok) {
        still.push_back(std::move(w));
        continue;
      }
      if (w.done && (!w.hasContext || w.context))
        w.done();
    }
    m_resolveWaiters = std::move(still);
    if (!answered.isEmpty())
      emit resolved(answered);
    for (const auto &ref: answered)
      if (find(ref, lut))
        emit entryChanged(find(ref, lut)->ref);
  });
}

void TextureCatalog::query(const QJsonObject &args, QObject *context, std::function<void(const QueryPage &)> fn) {
  AssetClient::instance().call(QStringLiteral("textures.query"), args, context, [this, fn](const Result &r) {
    QueryPage page;
    page.ok = r.ok;
    page.error = r.message;
    if (r.ok) {
      page.total = r.data.value(QStringLiteral("total")).toInt();
      page.offset = r.data.value(QStringLiteral("offset")).toInt();
      for (const auto &v: r.data.value(QStringLiteral("items")).toArray()) {
        TextureEntry e = TextureEntry::fromJson(v.toObject());
        store(e);
        page.items.append(std::move(e));
      }
      page.facets = pairsFrom(r.data.value(QStringLiteral("facets")).toArray());
      page.distinctTags = r.data.value(QStringLiteral("distinct_tags")).toInt();
      page.licenseFacets = pairsFrom(r.data.value(QStringLiteral("license_facets")).toArray());
    }
    if (fn)
      fn(page);
  });
}

void TextureCatalog::tagSearch(const QString &text, const QJsonObject &filter, int limit, QObject *context,
                               std::function<void(const QVector<QPair<QString, int>> &)> fn) {
  QJsonObject args;
  args.insert(QStringLiteral("q"), text);
  args.insert(QStringLiteral("filter"), filter);
  args.insert(QStringLiteral("limit"), limit);
  AssetClient::instance().call(QStringLiteral("tags.query"), args, context, [fn](const Result &r) {
    if (fn)
      fn(r.ok ? pairsFrom(r.data.value(QStringLiteral("tags")).toArray()) : QVector<QPair<QString, int>>());
  });
}

void TextureCatalog::popularTags(const QJsonObject &filter, QObject *context,
                                 std::function<void(const QVector<QPair<QString, int>> &)> fn) {
  QJsonObject args;
  args.insert(QStringLiteral("filter"), filter);
  AssetClient::instance().call(QStringLiteral("tags.popular"), args, context, [fn](const Result &r) {
    if (fn)
      fn(r.ok ? pairsFrom(r.data.value(QStringLiteral("tags")).toArray()) : QVector<QPair<QString, int>>());
  });
}

void TextureCatalog::refreshPacks() {
  AssetClient::instance().call(QStringLiteral("packs.list"), {}, this, [this](const Result &r) {
    if (!r.ok)
      return;
    QVector<PackInfo> packs;
    for (const auto &v: r.data.value(QStringLiteral("packs")).toArray()) {
      const QJsonObject o = v.toObject();
      PackInfo p;
      p.name = o.value(QStringLiteral("name")).toString();
      p.description = o.value(QStringLiteral("description")).toString();
      p.base = o.value(QStringLiteral("base")).toBool(true);
      p.type = o.value(QStringLiteral("type")).toString(QStringLiteral("texture"));
      p.count = o.value(QStringLiteral("count")).toInt();
      p.rev = qint64(o.value(QStringLiteral("rev")).toDouble());
      packs.append(p);
    }
    std::sort(packs.begin(), packs.end(), [](const PackInfo &a, const PackInfo &b) {
      if (a.base != b.base)
        return !a.base;  // user packs first
      return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    m_packs = packs;
    emit packsChanged();
  });
}

void TextureCatalog::call(const QString &op, const QJsonObject &args, QObject *context, ResultFn fn) {
  AssetClient::instance().call(op, args, context, std::move(fn));
}

void TextureCatalog::onServerEvent(const QString &ev, const QJsonObject &msg) {
  const QJsonObject data = msg.value(QStringLiteral("data")).toObject();
  auto bumpChanges = [this] { emit catalogChanged(++m_pendingChanges); };

  if (ev == QLatin1String("texture.created") || ev == QLatin1String("texture.updated")) {
    const TextureEntry e = TextureEntry::fromJson(data.value(QStringLiteral("entry")).toObject());
    if (!e.isValid())
      return;
    const QString oldRef = data.value(QStringLiteral("old_ref")).toString();
    if (!oldRef.isEmpty() && refKey(oldRef) != refKey(e.ref)) {
      m_entries.remove(assetKey(oldRef, e.isLut()));
      m_aliases.insert(assetKey(oldRef, e.isLut()), assetKey(e.ref, e.isLut()));
      if (auto l = m_locks.take(refKey(oldRef)); !l.ref.isEmpty()) {
        l.ref = e.ref;
        m_locks.insert(refKey(e.ref), l);
      }
    }
    store(e);
    if (!oldRef.isEmpty() && refKey(oldRef) != refKey(e.ref))
      emit entryRenamed(oldRef, e.ref);
    emit entryChanged(e.ref);
    if (ev == QLatin1String("texture.created") || !oldRef.isEmpty() || data.contains(QStringLiteral("tags")))
      bumpChanges();
    // A new texture, or a rename into another pack, changes the pack counts.
    if (ev == QLatin1String("texture.created") ||
        (!oldRef.isEmpty() && MaterialRef::parse(oldRef).pack.compare(e.pack, Qt::CaseInsensitive) != 0))
      m_packRefresh.start();
    return;
  }
  if (ev == QLatin1String("texture.deleted")) {
    const QString ref = data.value(QStringLiteral("ref")).toString();
    const bool lut = data.value(QStringLiteral("type")).toString() == QLatin1String("lut");
    m_entries.remove(assetKey(ref, lut));
    m_missing.insert(assetKey(ref, lut));
    emit entryRemoved(ref);
    bumpChanges();
    m_packRefresh.start();  // its pack has one texture fewer
    return;
  }
  if (ev == QLatin1String("texture.props")) {
    const QString ref = data.value(QStringLiteral("ref")).toString();
    auto it = m_entries.find(canonicalKey(ref));
    if (it != m_entries.end()) {
      it->props = MaterialProps::fromJson(data.value(QStringLiteral("props")).toObject());
      it->rev = qint64(data.value(QStringLiteral("rev")).toDouble());
      emit propsChanged(it->ref);
      emit entryChanged(it->ref);
    }
    return;
  }
  if (ev == QLatin1String("texture.locked") || ev == QLatin1String("texture.unlocked")) {
    const QString ref = data.value(QStringLiteral("ref")).toString();
    const QString key = canonicalKey(ref);
    if (ev == QLatin1String("texture.locked")) {
      const QJsonObject by = msg.value(QStringLiteral("by")).toObject();
      LockInfo l;
      l.ref = ref;
      l.clientId = by.value(QStringLiteral("client")).toInt();
      l.name = by.value(QStringLiteral("name")).toString();
      l.purpose = data.value(QStringLiteral("purpose")).toString();
      m_locks.insert(key, l);
    } else {
      m_locks.remove(key);
    }
    emit lockChanged(ref);
    return;
  }
  if (ev.startsWith(QLatin1String("pack."))) {
    const bool lut = data.value(QStringLiteral("type")).toString() == QLatin1String("lut");
    if (ev == QLatin1String("pack.updated")) {
      const QJsonObject renamed = data.value(QStringLiteral("renamed")).toObject();
      for (auto it = renamed.begin(); it != renamed.end(); ++it) {
        const QString oldRef = it.key();
        const QString newRef = it.value().toString();
        if (auto e = m_entries.take(assetKey(oldRef, lut)); e.isValid()) {
          e.ref = newRef;
          e.pack = MaterialRef::parse(newRef).pack;
          store(e);
        }
        m_aliases.insert(assetKey(oldRef, lut), assetKey(newRef, lut));
        emit entryRenamed(oldRef, newRef);
      }
    }
    if (ev == QLatin1String("pack.deleted")) {
      const QString name = data.value(QStringLiteral("name")).toString();
      QStringList gone;
      for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->isLut() == lut && it->pack.compare(name, Qt::CaseInsensitive) == 0) {
          gone.append(it->ref);
          m_missing.insert(it.key());
          it = m_entries.erase(it);
        } else {
          ++it;
        }
      }
      for (const auto &r: gone)
        emit entryRemoved(r);
    }
    refreshPacks();
    bumpChanges();
    return;
  }
}

void TextureCatalog::onResync() {
  // The journal could not cover our gap: re-fetch every entry held (still usable meanwhile).
  m_missing.clear();
  for (const auto &e: m_entries)
    m_resolveQueue.insert(queued(e.ref, e.isLut()));
  if (!m_resolveQueue.isEmpty())
    m_resolveTimer.start();
  refreshPacks();
}

} // namespace net
