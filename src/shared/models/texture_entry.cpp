#include "texture_entry.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <array>

namespace {

struct MapTypeInfo {
  const char *key;
  const char *label;
  const char *shortLabel;
  const char *fileSuffix;  // what the server writes into user packs
  bool rendered;
  bool grayscale;
};

constexpr std::array<MapTypeInfo, kMapTypeCount> kMapTypes = {{
    {"color", "Color", "C", "Color", true, false},
    {"normal", "Normal", "N", "NormalGL", true, false},
    {"arm", "ARM (AO / Rough / Metal)", "ARM", "ARM", true, false},
    {"roughness", "Roughness", "R", "Roughness", true, true},
    {"metalness", "Metalness", "M", "Metalness", true, true},
    {"ao", "Ambient Occlusion", "AO", "AmbientOcclusion", true, true},
    {"displacement", "Height (Displacement)", "H", "Displacement", false, true},
    {"opacity", "Opacity", "O", "Opacity", true, true},
    {"emission", "Emission", "E", "Emission", true, false},
    {"specular", "Specular", "S", "Specular", false, false},
    {"scattering", "Scattering", "SSS", "Scattering", false, false},
    {"idmask", "ID Mask", "ID", "IdMask", false, false},
    {"panorama", "Panorama (HDR sky)", "SKY", "Panorama", true, false},
    {"lut", "Colour LUT", "LUT", "Lut", false, false},
}};

struct SuffixAlias {
  const char *suffix;  // lower case
  MapType type;
  bool directX;
};

// File name suffixes seen in the packs (and the usual synonyms).
constexpr SuffixAlias kSuffixes[] = {
    {"color", MapType::Color, false},          {"diffuse", MapType::Color, false},
    {"albedo", MapType::Color, false},         {"basecolor", MapType::Color, false},
    {"normalgl", MapType::Normal, false},      {"normal", MapType::Normal, false},
    {"normaldx", MapType::Normal, true},       {"arm", MapType::Arm, false},
    {"orm", MapType::Arm, false},              {"roughness", MapType::Roughness, false},
    {"metalness", MapType::Metalness, false},  {"metallic", MapType::Metalness, false},
    {"ambientocclusion", MapType::Ao, false},  {"ao", MapType::Ao, false},
    {"displacement", MapType::Displacement, false}, {"height", MapType::Displacement, false},
    {"opacity", MapType::Opacity, false},      {"alpha", MapType::Opacity, false},
    {"emission", MapType::Emission, false},    {"emissive", MapType::Emission, false},
    {"specular", MapType::Specular, false},    {"scattering", MapType::Scattering, false},
    {"idmask", MapType::IdMask, false},        {"panorama", MapType::Panorama, false},
};

} // namespace

const char *mapTypeKey(MapType t) { return kMapTypes[std::size_t(t)].key; }
QString mapTypeLabel(MapType t) { return QString::fromLatin1(kMapTypes[std::size_t(t)].label); }
QString mapTypeShort(MapType t) { return QString::fromLatin1(kMapTypes[std::size_t(t)].shortLabel); }
bool mapTypeRendered(MapType t) { return kMapTypes[std::size_t(t)].rendered; }
bool mapTypeGrayscale(MapType t) { return kMapTypes[std::size_t(t)].grayscale; }

bool mapTypeFromKey(const QString &key, MapType &out) {
  for (int i = 0; i < kMapTypeCount; ++i) {
    if (key == QLatin1String(kMapTypes[i].key)) {
      out = MapType(i);
      return true;
    }
  }
  return false;
}

ParsedMapFile parseMapFileName(const QString &fileName) {
  // <name>_<size>K_<Type>[_<variant>].<ext>; the name itself may contain '_'.
  static const QRegularExpression re(QStringLiteral(R"(^(.+?)_(\d+[Kk])_([A-Za-z]+)(?:_([^.]+))?\.(png|jpe?g|exr)$)"),
                                     QRegularExpression::CaseInsensitiveOption);
  ParsedMapFile out;
  const auto m = re.match(fileName);
  if (!m.hasMatch())
    return out;
  const QString suffix = m.captured(3).toLower();
  for (const auto &a: kSuffixes) {
    if (suffix == QLatin1String(a.suffix)) {
      out.type = a.type;
      out.directX = a.directX;
      out.ok = true;
      break;
    }
  }
  if (!out.ok)
    return out;
  // Panoramas are HDR (.exr), and .exr is only read as a panorama.
  if ((out.type == MapType::Panorama) != (m.captured(5).compare(QLatin1String("exr"), Qt::CaseInsensitive) == 0)) {
    out.ok = false;
    return out;
  }
  out.textureName = m.captured(1);
  out.sizeTag = m.captured(2).toUpper();
  out.variant = m.captured(4);
  out.ext = m.captured(5).toLower();
  if (out.ext == QLatin1String("jpeg"))
    out.ext = QStringLiteral("jpg");
  return out;
}

QString makeMapFileName(const QString &textureName, const QString &sizeTag, MapType type, bool directX,
                        const QString &ext) {
  const char *suffix = (type == MapType::Normal && directX) ? "NormalDX" : kMapTypes[std::size_t(type)].fileSuffix;
  return QStringLiteral("%1_%2_%3.%4").arg(textureName, sizeTag, QLatin1String(suffix), ext);
}

const char *mapLayoutName(MapLayout l) {
  switch (l) {
    case MapLayout::Arm: return "arm";
    case MapLayout::Separate: return "separate";
    case MapLayout::None: break;
  }
  return "none";
}

MapLayout TextureEntry::layout() const {
  if (hasMap(MapType::Arm))
    return MapLayout::Arm;
  if (hasMap(MapType::Roughness) || hasMap(MapType::Metalness) || hasMap(MapType::Ao))
    return MapLayout::Separate;
  return MapLayout::None;
}

MapType slotMapType(const TextureEntry &e, MaterialSlot slot) {
  const auto have = [&e](MapType t) { return e.hasMap(t) ? t : MapType::Count; };
  const bool arm = e.layout() == MapLayout::Arm;
  switch (slot) {
    case MaterialSlot::Albedo: return have(MapType::Color);
    case MaterialSlot::Normal: return have(MapType::Normal);
    case MaterialSlot::Roughness: return arm ? MapType::Arm : have(MapType::Roughness);
    case MaterialSlot::Metallic: return arm ? MapType::Arm : have(MapType::Metalness);
    case MaterialSlot::Ao: return arm ? MapType::Arm : have(MapType::Ao);
    case MaterialSlot::Emission: return have(MapType::Emission);
    case MaterialSlot::Height: return have(MapType::Displacement);
    case MaterialSlot::Count: break;
  }
  return MapType::Count;
}

bool TextureEntry::normalDirectX() const {
  const MapFile *n = map(MapType::Normal);
  return n && n->directX;
}

QString TextureEntry::thumbFile() const {
  const MapFile *c = map(MapType::Color);
  if (!c)
    c = map(MapType::Panorama);
  if (!c)
    c = map(MapType::Lut);
  return c ? c->thumb : QString();
}

QJsonObject TextureEntry::toJson() const {
  QJsonObject o;
  o.insert(QStringLiteral("ref"), ref);
  o.insert(QStringLiteral("id"), id);
  o.insert(QStringLiteral("pack"), pack);
  o.insert(QStringLiteral("name"), name);
  o.insert(QStringLiteral("base"), base);
  o.insert(QStringLiteral("w"), w);
  o.insert(QStringLiteral("h"), h);
  o.insert(QStringLiteral("alpha"), alpha);
  o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
  if (!author.isEmpty())
    o.insert(QStringLiteral("author"), author);
  if (!license.isEmpty())
    o.insert(QStringLiteral("license"), license);
  if (!derivedFrom.isEmpty())
    o.insert(QStringLiteral("derived_from"), derivedFrom);
  if (added)
    o.insert(QStringLiteral("added"), double(added));
  o.insert(QStringLiteral("thumb"), thumbFile());
  o.insert(QStringLiteral("layout"), QLatin1String(mapLayoutName(layout())));
  o.insert(QStringLiteral("normal_dx"), normalDirectX());
  o.insert(QStringLiteral("has_height"), hasHeight());
  QJsonObject maps;
  for (auto it = this->maps.begin(); it != this->maps.end(); ++it) {
    const MapFile &f = it.value();
    QJsonObject m;
    m.insert(QStringLiteral("file"), f.file);
    m.insert(QStringLiteral("w"), f.w);
    m.insert(QStringLiteral("h"), f.h);
    if (f.alpha)
      m.insert(QStringLiteral("alpha"), true);
    if (f.directX)
      m.insert(QStringLiteral("dx"), true);
    m.insert(QStringLiteral("checksum"), f.checksum);
    if (!f.thumb.isEmpty())
      m.insert(QStringLiteral("thumb"), f.thumb);
    m.insert(QStringLiteral("editor"), f.editor.isEmpty() ? QJsonValue() : QJsonValue(f.editor));
    maps.insert(QLatin1String(mapTypeKey(it.key())), m);
  }
  o.insert(QStringLiteral("maps"), maps);
  o.insert(QStringLiteral("props"), props.toJson());
  if (!sky.isEmpty())
    o.insert(QStringLiteral("sky"), sky);
  if (!lut.isEmpty())
    o.insert(QStringLiteral("lut"), lut);
  o.insert(QStringLiteral("rev"), double(rev));
  return o;
}

TextureEntry TextureEntry::fromJson(const QJsonObject &o) {
  TextureEntry e;
  e.ref = o.value(QStringLiteral("ref")).toString();
  e.id = o.value(QStringLiteral("id")).toString();
  e.pack = o.value(QStringLiteral("pack")).toString();
  e.name = o.value(QStringLiteral("name")).toString();
  e.base = o.value(QStringLiteral("base")).toBool(true);
  e.w = o.value(QStringLiteral("w")).toInt();
  e.h = o.value(QStringLiteral("h")).toInt();
  e.alpha = o.value(QStringLiteral("alpha")).toBool();
  for (const auto &t: o.value(QStringLiteral("tags")).toArray())
    e.tags.append(t.toString());
  e.author = o.value(QStringLiteral("author")).toString();
  e.license = o.value(QStringLiteral("license")).toString();
  e.derivedFrom = o.value(QStringLiteral("derived_from")).toString();
  e.added = qint64(o.value(QStringLiteral("added")).toDouble());
  const QJsonObject maps = o.value(QStringLiteral("maps")).toObject();
  for (auto it = maps.begin(); it != maps.end(); ++it) {
    MapType t;
    if (!mapTypeFromKey(it.key(), t))
      continue;
    const QJsonObject m = it.value().toObject();
    MapFile f;
    f.file = m.value(QStringLiteral("file")).toString();
    f.w = m.value(QStringLiteral("w")).toInt();
    f.h = m.value(QStringLiteral("h")).toInt();
    f.alpha = m.value(QStringLiteral("alpha")).toBool();
    f.directX = m.value(QStringLiteral("dx")).toBool();
    f.checksum = m.value(QStringLiteral("checksum")).toString();
    f.thumb = m.value(QStringLiteral("thumb")).toString();
    f.editor = m.value(QStringLiteral("editor")).toString();
    e.maps.insert(t, f);
  }
  e.props = MaterialProps::fromJson(o.value(QStringLiteral("props")).toObject());
  e.sky = o.value(QStringLiteral("sky")).toObject();
  e.lut = o.value(QStringLiteral("lut")).toObject();
  e.rev = qint64(o.value(QStringLiteral("rev")).toDouble());
  return e;
}

bool TextureEntry::operator==(const TextureEntry &o) const {
  return ref == o.ref && id == o.id && pack == o.pack && name == o.name && base == o.base && w == o.w && h == o.h &&
         alpha == o.alpha && tags == o.tags && author == o.author && license == o.license &&
         derivedFrom == o.derivedFrom && added == o.added && maps == o.maps && props == o.props && sky == o.sky && lut == o.lut && rev == o.rev;
}

QString MaterialRef::base() const { return pack.isEmpty() ? name : pack + QLatin1Char('/') + name; }

QString MaterialRef::full() const { return variant.isEmpty() ? base() : base() + QLatin1Char('#') + variant; }

MaterialRef MaterialRef::parse(const QString &s) {
  MaterialRef r;
  QString rest = s.trimmed();
  const int hash = rest.indexOf(QLatin1Char('#'));
  if (hash >= 0) {
    r.variant = rest.mid(hash + 1).trimmed();
    rest = rest.left(hash);
  }
  const int slash = rest.indexOf(QLatin1Char('/'));
  if (slash > 0) {
    r.pack = rest.left(slash);
    r.name = rest.mid(slash + 1);
  } else {
    r.name = rest;
  }
  return r;
}

QString MaterialRef::make(const QString &pack, const QString &name, const QString &variant) {
  MaterialRef r{pack, name, variant};
  return r.full();
}

QString refKey(const QString &ref) { return ref.trimmed().toLower(); }

QString assetKey(const QString &ref, bool lut) {
  return lut ? QStringLiteral("lut:") + refKey(ref) : refKey(ref);
}

QString packKey(const QString &name, const QString &type) {
  const QString lower = name.trimmed().toLower();
  return type == QLatin1String("lut") ? QStringLiteral("lut:") + lower : lower;
}

bool isValidAssetName(const QString &name) {
  static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
  return re.match(name).hasMatch();
}
