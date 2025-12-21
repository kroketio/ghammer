#include "material_overrides.h"

#include <QJsonArray>

#include "shared/models/material_defaults.h"

namespace eng {

using matprops::Key;
using matprops::kKeyCount;

namespace {

QString baseKey(const QString &ref) { return refKey(MaterialRef::parse(ref).base()); }

} // namespace

const MaterialProps *MaterialOverrides::mapLayer(const QString &ref) const {
  auto it = m_map.constFind(baseKey(ref));
  return it == m_map.constEnd() ? nullptr : &it.value();
}

void MaterialOverrides::setMapLayer(const QString &ref, const MaterialProps &props) {
  const QString key = baseKey(ref);
  if (props.isEmpty()) {
    m_map.remove(key);
    m_mapSpelling.remove(key);
  } else {
    m_map.insert(key, props);
    m_mapSpelling.insert(key, MaterialRef::parse(ref).base());
  }
  ++m_version;
}

QStringList MaterialOverrides::mapLayerRefs() const {
  QStringList out;
  for (auto it = m_map.begin(); it != m_map.end(); ++it)
    out.append(m_mapSpelling.value(it.key(), it.key()));
  out.sort(Qt::CaseInsensitive);
  return out;
}

const MaterialVariant *MaterialOverrides::variant(const QString &ref) const {
  auto it = m_variants.constFind(refKey(ref));
  return it == m_variants.constEnd() ? nullptr : &it.value();
}

void MaterialOverrides::setVariant(const MaterialVariant &v) {
  MaterialVariant c = v;
  if (c.baseRef.isEmpty())
    c.baseRef = MaterialRef::parse(c.ref).base();
  if (c.displayName.isEmpty())
    c.displayName = MaterialRef::parse(c.ref).variant;
  m_variants.insert(refKey(c.ref), c);
  ++m_version;
}

void MaterialOverrides::removeVariant(const QString &ref) {
  if (m_variants.remove(refKey(ref)))
    ++m_version;
}

QList<MaterialVariant> MaterialOverrides::variantsOf(const QString &baseRef) const {
  QList<MaterialVariant> out;
  const QString key = baseKey(baseRef);
  for (const auto &v: m_variants)
    if (refKey(v.baseRef) == key)
      out.append(v);
  std::sort(out.begin(), out.end(), [](const MaterialVariant &a, const MaterialVariant &b) {
    return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
  });
  return out;
}

QString MaterialOverrides::uniqueVariantName(const QString &baseRef, const QString &wanted) const {
  QString stem;
  for (const QChar c: wanted.trimmed())
    stem += (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-')) ? c : QLatin1Char('_');
  if (stem.isEmpty())
    stem = QStringLiteral("variant");
  const QString base = MaterialRef::parse(baseRef).base();
  auto taken = [&](const QString &n) { return m_variants.contains(refKey(base + QLatin1Char('#') + n)); };
  if (!taken(stem))
    return stem;
  for (int i = 2;; ++i) {
    const QString n = QStringLiteral("%1_%2").arg(stem).arg(i);
    if (!taken(n))
      return n;
  }
}

void MaterialOverrides::rememberId(const QString &ref, const QString &id) {
  if (id.isEmpty())
    return;
  m_ids.insert(baseKey(ref), id);
  m_idSpelling.insert(baseKey(ref), MaterialRef::parse(ref).base());
}

QString MaterialOverrides::idFor(const QString &ref) const { return m_ids.value(baseKey(ref)); }

void MaterialOverrides::renameTexture(const QString &oldRef, const QString &newRef) {
  const QString oldKey = baseKey(oldRef);
  const QString newBase = MaterialRef::parse(newRef).base();
  if (auto it = m_map.find(oldKey); it != m_map.end()) {
    const MaterialProps p = *it;
    m_map.erase(it);
    m_mapSpelling.remove(oldKey);
    m_map.insert(refKey(newBase), p);
    m_mapSpelling.insert(refKey(newBase), newBase);
  }
  QList<MaterialVariant> moved;
  for (auto it = m_variants.begin(); it != m_variants.end();) {
    if (refKey(it->baseRef) == oldKey) {
      MaterialVariant v = *it;
      v.baseRef = newBase;
      v.ref = newBase + QLatin1Char('#') + MaterialRef::parse(v.ref).variant;
      moved.append(v);
      it = m_variants.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto &v: moved)
    m_variants.insert(refKey(v.ref), v);
  if (auto id = m_ids.take(oldKey); !id.isEmpty()) {
    m_idSpelling.remove(oldKey);
    m_ids.insert(refKey(newBase), id);
    m_idSpelling.insert(refKey(newBase), newBase);
  }
  ++m_version;
}

bool MaterialOverrides::isEmpty() const {
  return projectPack.isEmpty() && m_map.isEmpty() && m_variants.isEmpty() && m_ids.isEmpty();
}

void MaterialOverrides::clear() {
  projectPack.clear();
  m_map.clear();
  m_mapSpelling.clear();
  m_variants.clear();
  m_ids.clear();
  m_idSpelling.clear();
  ++m_version;
}

ResolvedProps MaterialOverrides::resolve(const QString &ref, const TextureEntry *entry) const {
  ResolvedProps r;
  r.props = MaterialProps::godotDefaults();
  r.source.fill(PropSource::Godot);
  auto apply = [&r](const MaterialProps &layer, PropSource src) {
    for (int i = 0; i < kKeyCount; ++i) {
      const Key k = Key(i);
      if (layer.isSet(k)) {
        r.props.setValue(k, layer.value(k));
        r.source[std::size_t(i)] = src;
      }
    }
  };
  if (entry && (entry->isValid() || !entry->name.isEmpty())) {
    r.defaults = MaterialDefaults::instance().compute(*entry);
    r.server = entry->props;
  }
  if (const MaterialProps *m = mapLayer(ref))
    r.map = *m;
  if (const MaterialVariant *v = variant(ref))
    r.variant = v->props;
  apply(r.defaults, PropSource::Default);
  apply(r.server, PropSource::Server);
  apply(r.map, PropSource::Map);
  apply(r.variant, PropSource::Variant);
  return r;
}

QJsonObject MaterialOverrides::toJson(const QHash<QString, MaterialProps> &resolved) const {
  QJsonObject o;
  o.insert(QStringLiteral("version"), 1);
  if (!projectPack.isEmpty())
    o.insert(QStringLiteral("projectPack"), projectPack);
  QJsonArray textures;
  QStringList idKeys(m_ids.keyBegin(), m_ids.keyEnd());
  idKeys.sort();
  for (const auto &k: idKeys) {
    QJsonObject t;
    t.insert(QStringLiteral("ref"), m_idSpelling.value(k, k));
    t.insert(QStringLiteral("id"), m_ids.value(k));
    textures.append(t);
  }
  if (!textures.isEmpty())
    o.insert(QStringLiteral("textures"), textures);
  QJsonObject overrides;
  for (auto it = m_map.begin(); it != m_map.end(); ++it)
    overrides.insert(m_mapSpelling.value(it.key(), it.key()), it->toJson());
  if (!overrides.isEmpty())
    o.insert(QStringLiteral("overrides"), overrides);
  QJsonArray variants;
  QList<MaterialVariant> vs = m_variants.values();
  std::sort(vs.begin(), vs.end(), [](const auto &a, const auto &b) { return refKey(a.ref) < refKey(b.ref); });
  for (const auto &v: vs) {
    QJsonObject j;
    j.insert(QStringLiteral("ref"), v.ref);
    j.insert(QStringLiteral("base"), v.baseRef);
    j.insert(QStringLiteral("name"), v.displayName);
    j.insert(QStringLiteral("props"), v.props.toJson());
    variants.append(j);
  }
  if (!variants.isEmpty())
    o.insert(QStringLiteral("variants"), variants);
  QJsonObject snap;
  for (auto it = resolved.begin(); it != resolved.end(); ++it)
    snap.insert(it.key(), it->toJson());
  if (!snap.isEmpty())
    o.insert(QStringLiteral("resolved"), snap);
  return o;
}

MaterialOverrides MaterialOverrides::fromJson(const QJsonObject &o, QHash<QString, MaterialProps> *resolved) {
  MaterialOverrides m;
  m.projectPack = o.value(QStringLiteral("projectPack")).toString();
  for (const auto &v: o.value(QStringLiteral("textures")).toArray()) {
    const QJsonObject t = v.toObject();
    const QString ref = t.value(QStringLiteral("ref")).toString();
    m.rememberId(ref, t.value(QStringLiteral("id")).toString());
  }
  const QJsonObject overrides = o.value(QStringLiteral("overrides")).toObject();
  for (auto it = overrides.begin(); it != overrides.end(); ++it)
    m.setMapLayer(it.key(), MaterialProps::fromJson(it->toObject()));
  for (const auto &v: o.value(QStringLiteral("variants")).toArray()) {
    const QJsonObject j = v.toObject();
    MaterialVariant mv;
    mv.ref = j.value(QStringLiteral("ref")).toString();
    mv.baseRef = j.value(QStringLiteral("base")).toString();
    mv.displayName = j.value(QStringLiteral("name")).toString();
    mv.props = MaterialProps::fromJson(j.value(QStringLiteral("props")).toObject());
    if (!mv.ref.isEmpty())
      m.setVariant(mv);
  }
  if (resolved) {
    const QJsonObject snap = o.value(QStringLiteral("resolved")).toObject();
    for (auto it = snap.begin(); it != snap.end(); ++it)
      resolved->insert(it.key(), MaterialProps::fromJson(it->toObject()));
  }
  return m;
}

bool MaterialOverrides::operator==(const MaterialOverrides &o) const {
  return projectPack == o.projectPack && m_map == o.m_map && m_variants == o.m_variants && m_ids == o.m_ids;
}

TextureEntry libraryStandIn(const QString &name, bool hasArm, bool hasRoughness, bool hasMetalness, bool hasAo,
                            bool hasEmission) {
  TextureEntry e;
  e.name = name;
  e.maps.insert(MapType::Color, MapFile{});
  if (hasArm)
    e.maps.insert(MapType::Arm, MapFile{});
  if (hasRoughness)
    e.maps.insert(MapType::Roughness, MapFile{});
  if (hasMetalness)
    e.maps.insert(MapType::Metalness, MapFile{});
  if (hasAo)
    e.maps.insert(MapType::Ao, MapFile{});
  if (hasEmission)
    e.maps.insert(MapType::Emission, MapFile{});
  return e;
}

} // namespace eng
