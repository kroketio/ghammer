#pragma once

// Godot-style material properties for one texture, shared by the asset server and the level editor.
// Keys and base values mirror Godot 4's BaseMaterial3D (scene/resources/material.cpp): a texture with nothing set
// renders like a fresh StandardMaterial3D. Ranges follow the editor's spec, not Godot's (heightmap_scale is 0..1).
// Every layer (tag defaults, server / map overrides, variants) is sparse: only keys in `setMask` count, overlay()
// stacks layers (see client/engine/material_overrides.h).

#include <QJsonObject>
#include <QRgb>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <span>

namespace matprops {

enum class Key : quint8 {
  Transparency = 0,
  ShadingMode,
  Metallic,
  MetallicSpecular,
  MetallicChannel,
  Roughness,
  RoughnessChannel,
  EmissionEnabled,
  EmissionEnergy,
  EmissionColor,
  NormalScale,
  AoLightAffect,
  AoChannel,
  HeightmapScale,
  HeightmapDeepParallax,
  Count
};

enum class Section : quint8 {
  Transparency = 0,
  Shading,
  Metallic,
  Roughness,
  Emission,
  Normal,
  AmbientOcclusion,
  Height,
  Count
};

enum class Type : quint8 { Bool, Float, Channel, Shading, Color };

// Which channel of a map holds the value (Godot's *_texture_channel).
enum class Channel : quint8 { Red = 0, Green, Blue, Alpha, Grey };

enum class Shading : quint8 { PerPixel = 0, Unshaded };

struct PropDef {
  Key key;
  const char *json;   // JSON / fmap / server key
  Section section;
  Type type;
  const char *label;  // UI label
  double min;
  double max;
  double godotDefault;  // floats and bools (0/1); enums as their index; colors unused
  const char *tip;
};

constexpr int kKeyCount = int(Key::Count);
constexpr int kSectionCount = int(Section::Count);

std::span<const PropDef> defs();
const PropDef &def(Key key);
const PropDef *defByJson(const QString &json);
const char *sectionName(Section section);

const char *channelName(Channel c);  // "red" ...
bool channelFromName(const QString &name, Channel &out);
const char *shadingName(Shading s);  // "per_pixel" / "unshaded"
bool shadingFromName(const QString &name, Shading &out);

constexpr quint32 bit(Key k) { return 1u << quint32(k); }
constexpr quint32 kAllKeys = (1u << quint32(Key::Count)) - 1u;

} // namespace matprops

struct MaterialProps {
  bool transparency = false;
  matprops::Shading shadingMode = matprops::Shading::PerPixel;
  float metallic = 0.0f;
  float metallicSpecular = 0.5f;
  matprops::Channel metallicChannel = matprops::Channel::Red;
  float roughness = 1.0f;
  matprops::Channel roughnessChannel = matprops::Channel::Red;
  bool emissionEnabled = false;
  float emissionEnergy = 1.0f;
  QRgb emissionColor = qRgb(0, 0, 0);
  float normalScale = 1.0f;
  float aoLightAffect = 0.0f;
  matprops::Channel aoChannel = matprops::Channel::Red;
  float heightmapScale = 0.05f;
  bool heightmapDeepParallax = false;

  // Keys that carry a value in this layer.
  quint32 setMask = 0;

  // Every key set to Godot's BaseMaterial3D default: the bottom of the stack.
  static MaterialProps godotDefaults();

  [[nodiscard]] bool isSet(matprops::Key k) const { return (setMask & matprops::bit(k)) != 0; }
  [[nodiscard]] bool isEmpty() const { return setMask == 0; }
  void unset(matprops::Key k) { setMask &= ~matprops::bit(k); }
  void clear() { *this = MaterialProps(); }

  // Generic access for UI / JSON. value() returns bool, double, QString (enums)
  // or QColor. setValue() validates and clamps; it marks the key set.
  [[nodiscard]] QVariant value(matprops::Key k) const;
  bool setValue(matprops::Key k, const QVariant &v, QString *error = nullptr);

  // Copies every key set in `top` over this layer.
  void overlay(const MaterialProps &top);
  // Keys of this layer whose value differs from `base` (or that `base` lacks).
  [[nodiscard]] MaterialProps diff(const MaterialProps &base) const;
  [[nodiscard]] MaterialProps masked(quint32 mask) const;

  // Sparse JSON: only set keys. Floats are rounded to 4 decimals so a value
  // survives a round trip through JSON unchanged.
  [[nodiscard]] QJsonObject toJson() const;
  // Unknown keys and wrong types are errors (listed in `errors`); with
  // `strict` the first error aborts and returns an empty layer.
  static MaterialProps fromJson(const QJsonObject &obj, QStringList *errors = nullptr, bool strict = false);

  bool operator==(const MaterialProps &o) const;
  bool operator!=(const MaterialProps &o) const { return !(*this == o); }
};
