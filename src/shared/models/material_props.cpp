#include "material_props.h"

#include <QColor>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>

namespace matprops {

namespace {

// Ranges follow the editor spec; defaults follow Godot's BaseMaterial3D.
constexpr std::array<PropDef, kKeyCount> kDefs = {{
    {Key::Transparency, "transparency", Section::Transparency, Type::Bool, "Transparency", 0, 1, 0,
     "Alpha-test the albedo (or opacity map) alpha. Godot: transparency = alpha scissor."},
    {Key::ShadingMode, "shading_mode", Section::Shading, Type::Shading, "Shading mode", 0, 1, 0,
     "Per-pixel lighting, or unshaded (albedo only)."},
    {Key::Metallic, "metallic", Section::Metallic, Type::Float, "Metallic", 0, 1, 0,
     "Multiplies the metallic map (white when the texture has none)."},
    {Key::MetallicSpecular, "metallic_specular", Section::Metallic, Type::Float, "Specular", 0, 1, 0.5,
     "Specular reflectance of non-metals. 0.5 = 4% (Godot default)."},
    {Key::MetallicChannel, "metallic_texture_channel", Section::Metallic, Type::Channel, "Texture channel", 0, 4, 0,
     "Which channel of the metallic map is read."},
    {Key::Roughness, "roughness", Section::Roughness, Type::Float, "Roughness", 0, 1, 1,
     "Multiplies the roughness map (white when the texture has none)."},
    {Key::RoughnessChannel, "roughness_texture_channel", Section::Roughness, Type::Channel, "Texture channel", 0, 4, 0,
     "Which channel of the roughness map is read."},
    {Key::EmissionEnabled, "emission_enabled", Section::Emission, Type::Bool, "Enabled", 0, 1, 0,
     "Adds emitted light: (color + emission map) x energy."},
    {Key::EmissionEnergy, "emission_energy", Section::Emission, Type::Float, "Energy", 0, 16, 1,
     "Emission energy multiplier."},
    {Key::EmissionColor, "emission_color", Section::Emission, Type::Color, "Color", 0, 0, 0,
     "Emission color, added to the emission map."},
    {Key::NormalScale, "normal_scale", Section::Normal, Type::Float, "Strength", 0, 1, 1,
     "Normal map strength."},
    {Key::AoLightAffect, "ao_light_affect", Section::AmbientOcclusion, Type::Float, "Light affect", 0, 1, 0,
     "How much ambient occlusion also darkens direct light (AO always darkens ambient light)."},
    {Key::AoChannel, "ao_texture_channel", Section::AmbientOcclusion, Type::Channel, "Texture channel", 0, 4, 0,
     "Which channel of the AO map is read."},
    {Key::HeightmapScale, "heightmap_scale", Section::Height, Type::Float, "Scale", 0, 1, 0.05,
     "Height (displacement) strength. Not rendered in the editor; used by the Godot export."},
    {Key::HeightmapDeepParallax, "heightmap_deep_parallax", Section::Height, Type::Bool, "Deep parallax", 0, 1, 0,
     "Use deep parallax occlusion mapping in Godot."},
}};

constexpr std::array<const char *, kSectionCount> kSectionNames = {
    "Transparency", "Shading", "Metallic", "Roughness", "Emission", "Normal", "Ambient Occlusion", "Height",
};

constexpr std::array<const char *, 5> kChannelNames = {"red", "green", "blue", "alpha", "grey"};
constexpr std::array<const char *, 2> kShadingNames = {"per_pixel", "unshaded"};

} // namespace

std::span<const PropDef> defs() { return kDefs; }

const PropDef &def(Key key) { return kDefs[std::size_t(key)]; }

const PropDef *defByJson(const QString &json) {
  for (const auto &d: kDefs)
    if (json == QLatin1String(d.json))
      return &d;
  return nullptr;
}

const char *sectionName(Section section) { return kSectionNames[std::size_t(section)]; }

const char *channelName(Channel c) { return kChannelNames[std::size_t(c)]; }

bool channelFromName(const QString &name, Channel &out) {
  const QString n = name.trimmed().toLower();
  for (std::size_t i = 0; i < kChannelNames.size(); ++i) {
    if (n == QLatin1String(kChannelNames[i])) {
      out = Channel(i);
      return true;
    }
  }
  if (n == QLatin1String("gray") || n == QLatin1String("grayscale") || n == QLatin1String("greyscale")) {
    out = Channel::Grey;
    return true;
  }
  return false;
}

const char *shadingName(Shading s) { return kShadingNames[std::size_t(s)]; }

bool shadingFromName(const QString &name, Shading &out) {
  const QString n = name.trimmed().toLower();
  for (std::size_t i = 0; i < kShadingNames.size(); ++i) {
    if (n == QLatin1String(kShadingNames[i])) {
      out = Shading(i);
      return true;
    }
  }
  return false;
}

} // namespace matprops

using namespace matprops;

namespace {

float round4(double v) { return float(std::round(v * 10000.0) / 10000.0); }

float clampFloat(Key k, double v) {
  const PropDef &d = def(k);
  if (!std::isfinite(v))
    v = d.godotDefault;
  return round4(std::clamp(v, d.min, d.max));
}

} // namespace

MaterialProps MaterialProps::godotDefaults() {
  MaterialProps p;  // member initialisers are the Godot defaults
  p.setMask = kAllKeys;
  return p;
}

QVariant MaterialProps::value(Key k) const {
  switch (k) {
    case Key::Transparency: return transparency;
    case Key::ShadingMode: return QString::fromLatin1(shadingName(shadingMode));
    case Key::Metallic: return double(metallic);
    case Key::MetallicSpecular: return double(metallicSpecular);
    case Key::MetallicChannel: return QString::fromLatin1(channelName(metallicChannel));
    case Key::Roughness: return double(roughness);
    case Key::RoughnessChannel: return QString::fromLatin1(channelName(roughnessChannel));
    case Key::EmissionEnabled: return emissionEnabled;
    case Key::EmissionEnergy: return double(emissionEnergy);
    case Key::EmissionColor: return QColor::fromRgb(emissionColor);
    case Key::NormalScale: return double(normalScale);
    case Key::AoLightAffect: return double(aoLightAffect);
    case Key::AoChannel: return QString::fromLatin1(channelName(aoChannel));
    case Key::HeightmapScale: return double(heightmapScale);
    case Key::HeightmapDeepParallax: return heightmapDeepParallax;
    case Key::Count: break;
  }
  return {};
}

bool MaterialProps::setValue(Key k, const QVariant &v, QString *error) {
  const PropDef &d = def(k);
  auto fail = [&](const char *what) {
    if (error)
      *error = QStringLiteral("%1: %2").arg(QLatin1String(d.json), QLatin1String(what));
    return false;
  };

  switch (d.type) {
    case Type::Bool: {
      if (v.typeId() != QMetaType::Bool && !v.canConvert<bool>())
        return fail("expected a boolean");
      const bool b = v.toBool();
      if (k == Key::Transparency)
        transparency = b;
      else if (k == Key::EmissionEnabled)
        emissionEnabled = b;
      else
        heightmapDeepParallax = b;
      break;
    }
    case Type::Float: {
      bool ok = false;
      const double f = v.toDouble(&ok);
      if (!ok)
        return fail("expected a number");
      const float c = clampFloat(k, f);
      switch (k) {
        case Key::Metallic: metallic = c; break;
        case Key::MetallicSpecular: metallicSpecular = c; break;
        case Key::Roughness: roughness = c; break;
        case Key::EmissionEnergy: emissionEnergy = c; break;
        case Key::NormalScale: normalScale = c; break;
        case Key::AoLightAffect: aoLightAffect = c; break;
        case Key::HeightmapScale: heightmapScale = c; break;
        default: return fail("not a float key");
      }
      break;
    }
    case Type::Channel: {
      Channel c;
      if (v.typeId() == QMetaType::Int || v.typeId() == QMetaType::LongLong || v.typeId() == QMetaType::Double) {
        const int i = v.toInt();
        if (i < 0 || i > int(Channel::Grey))
          return fail("channel out of range");
        c = Channel(i);
      } else if (!channelFromName(v.toString(), c)) {
        return fail("expected red|green|blue|alpha|grey");
      }
      if (k == Key::MetallicChannel)
        metallicChannel = c;
      else if (k == Key::RoughnessChannel)
        roughnessChannel = c;
      else
        aoChannel = c;
      break;
    }
    case Type::Shading: {
      Shading s;
      if (!shadingFromName(v.toString(), s))
        return fail("expected per_pixel|unshaded");
      shadingMode = s;
      break;
    }
    case Type::Color: {
      QColor c;
      if (v.typeId() == QMetaType::QColor)
        c = v.value<QColor>();
      else
        c = QColor::fromString(v.toString());
      if (!c.isValid())
        return fail("expected a #rrggbb color");
      emissionColor = c.rgb();
      break;
    }
  }
  setMask |= bit(k);
  return true;
}

void MaterialProps::overlay(const MaterialProps &top) {
  for (int i = 0; i < kKeyCount; ++i) {
    const Key k = Key(i);
    if (top.isSet(k))
      setValue(k, top.value(k));
  }
}

MaterialProps MaterialProps::diff(const MaterialProps &base) const {
  MaterialProps out;
  for (int i = 0; i < kKeyCount; ++i) {
    const Key k = Key(i);
    if (!isSet(k))
      continue;
    if (base.isSet(k) && base.value(k) == value(k))
      continue;
    out.setValue(k, value(k));
  }
  return out;
}

MaterialProps MaterialProps::masked(quint32 mask) const {
  MaterialProps out = *this;
  out.setMask &= mask;
  return out;
}

QJsonObject MaterialProps::toJson() const {
  QJsonObject obj;
  for (int i = 0; i < kKeyCount; ++i) {
    const Key k = Key(i);
    if (!isSet(k))
      continue;
    const PropDef &d = def(k);
    const QVariant v = value(k);
    switch (d.type) {
      case Type::Bool: obj.insert(QLatin1String(d.json), v.toBool()); break;
      case Type::Float: obj.insert(QLatin1String(d.json), std::round(v.toDouble() * 10000.0) / 10000.0); break;
      case Type::Channel:
      case Type::Shading: obj.insert(QLatin1String(d.json), v.toString()); break;
      case Type::Color: obj.insert(QLatin1String(d.json), v.value<QColor>().name(QColor::HexRgb)); break;
    }
  }
  return obj;
}

MaterialProps MaterialProps::fromJson(const QJsonObject &obj, QStringList *errors, bool strict) {
  MaterialProps p;
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    const PropDef *d = defByJson(it.key());
    QString err;
    if (!d) {
      err = QStringLiteral("%1: unknown key").arg(it.key());
    } else {
      const QJsonValue jv = it.value();
      bool typeOk = true;
      switch (d->type) {
        case Type::Bool: typeOk = jv.isBool(); break;
        case Type::Float: typeOk = jv.isDouble(); break;
        case Type::Channel: typeOk = jv.isString() || jv.isDouble(); break;
        case Type::Shading:
        case Type::Color: typeOk = jv.isString(); break;
      }
      if (!typeOk)
        err = QStringLiteral("%1: wrong type").arg(it.key());
      else
        p.setValue(d->key, jv.toVariant(), &err);
    }
    if (!err.isEmpty()) {
      if (errors)
        errors->append(err);
      if (strict)
        return {};
    }
  }
  return p;
}

bool MaterialProps::operator==(const MaterialProps &o) const {
  if (setMask != o.setMask)
    return false;
  for (int i = 0; i < kKeyCount; ++i) {
    const Key k = Key(i);
    if (isSet(k) && value(k) != o.value(k))
      return false;
  }
  return true;
}
