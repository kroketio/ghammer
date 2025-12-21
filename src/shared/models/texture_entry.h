#pragma once

// One texture of the asset server as the wire and the editor see it: a lean, copyable value (no Qt object, no
// GPU handles). Lists every source map, rendered or not: the 3D view draws a subset (mapTypeRendered), but the UI,
// copies, external editing and the Godot export need all of them.

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include "material_props.h"

enum class MapType : quint8 {
  Color = 0,
  Normal,
  Arm,
  Roughness,
  Metalness,
  Ao,
  Displacement,
  Opacity,
  Emission,
  Specular,
  Scattering,
  IdMask,
  Panorama,  // an equirectangular HDR sky (.exr only): the entry is a skybox
  Lut,       // a colour LUT strip (Godot's 3D LUT layout, N*N x N .png) in a "lut" pack
  Count
};

constexpr int kMapTypeCount = int(MapType::Count);

const char *mapTypeKey(MapType t);        // "color", "normal", ... (JSON / URL key)
QString mapTypeLabel(MapType t);          // "Color", "Ambient Occlusion", ...
QString mapTypeShort(MapType t);          // "C", "N", "ARM", ... (chips)
bool mapTypeFromKey(const QString &key, MapType &out);
// Drawn by the editor's 3D view (gets a 512 px editor derivative).
bool mapTypeRendered(MapType t);
// One meaningful channel (stored grayscale as an editor derivative).
bool mapTypeGrayscale(MapType t);

// `AerialAsphalt001_1K_Color.png`, `Wood_Floor_01_2K_NormalDX_v2.jpg` ...
struct ParsedMapFile {
  bool ok = false;
  QString textureName;  // "AerialAsphalt001"
  QString sizeTag;      // "1K"
  MapType type = MapType::Color;
  bool directX = false; // NormalDX
  QString variant;      // trailing "_v2" etc., empty for the main map
  QString ext;          // "png" / "jpg" / "exr" (Panorama only), lower case
};
ParsedMapFile parseMapFileName(const QString &fileName);
// Inverse of parseMapFileName for files the server writes into user packs.
QString makeMapFileName(const QString &textureName, const QString &sizeTag, MapType type, bool directX,
                        const QString &ext);

struct MapFile {
  QString file;      // source file name inside the pack directory
  int w = 0;
  int h = 0;
  bool alpha = false;
  bool directX = false;
  QString checksum;  // changes whenever the source file changes
  QString thumb;     // 256 px derivative file name (empty until generated)
  QString editor;    // 512 px derivative file name; empty for non-rendered maps
                     // (Panorama: a 2048 x 1024 half-float .exr)

  bool operator==(const MapFile &) const = default;
};

enum class MapLayout : quint8 {
  None = 0,  // no AO / roughness / metal maps
  Arm,       // one packed ARM file (R=AO, G=rough, B=metal); separate files ignored
  Separate,  // individual Roughness / Metalness / AmbientOcclusion files
};

const char *mapLayoutName(MapLayout l);

// The texture slots of a Godot StandardMaterial3D the editor fills.
enum class MaterialSlot : quint8 { Albedo = 0, Normal, Roughness, Metallic, Ao, Emission, Height, Count };

struct TextureEntry {
  QString ref;   // "pack/Name"
  QString id;    // uuid from the pack JSON; survives renames
  QString pack;
  QString name;
  bool base = true;  // read-only base pack
  int w = 0;         // source size of the color map, px
  int h = 0;
  bool alpha = false;
  QStringList tags;  // lower case
  QString author;
  QString license;
  QString derivedFrom;  // ref this texture was copied from
  qint64 added = 0;     // unix seconds; for "recently added"
  QMap<MapType, MapFile> maps;
  MaterialProps props;  // server overrides (sparse)
  // Skyboxes: what the pack JSON says about the sky ("sky" key: sun angles,
  // light, ambient, fog colours, source...), passed through as is.
  QJsonObject sky;
  // Colour LUTs: the pack JSON's "lut" object ({"grade": {...}} for LUTs made from adjustments), passed through.
  QJsonObject lut;
  qint64 rev = 0;       // revision of the last change on the server

  [[nodiscard]] bool isValid() const { return !ref.isEmpty(); }
  [[nodiscard]] bool hasMap(MapType t) const { return maps.contains(t); }
  [[nodiscard]] const MapFile *map(MapType t) const {
    auto it = maps.constFind(t);
    return it == maps.constEnd() ? nullptr : &it.value();
  }
  [[nodiscard]] MapLayout layout() const;
  [[nodiscard]] bool normalDirectX() const;
  [[nodiscard]] bool hasHeight() const { return hasMap(MapType::Displacement); }
  // A skybox (a Panorama map) rather than a material.
  [[nodiscard]] bool isSky() const { return hasMap(MapType::Panorama); }
  [[nodiscard]] bool isLut() const { return hasMap(MapType::Lut); }
  [[nodiscard]] QString thumbFile() const;  // color map's thumbnail (a sky's panorama)

  [[nodiscard]] QJsonObject toJson() const;
  static TextureEntry fromJson(const QJsonObject &obj);

  bool operator==(const TextureEntry &) const;
};

// Refs are "pack/Name" with an optional "#variant" (map-local variants).
// Lookups are case-insensitive; refKey() is the canonical hash key.
struct MaterialRef {
  QString pack;
  QString name;
  QString variant;

  // A bare name without a pack (old maps, VMF imports). The server resolves it
  // by texture name; the editor rewrites it to a full ref once resolved.
  [[nodiscard]] bool isLegacy() const { return pack.isEmpty(); }
  [[nodiscard]] bool isVariant() const { return !variant.isEmpty(); }
  [[nodiscard]] bool isEmpty() const { return name.isEmpty(); }
  [[nodiscard]] QString base() const;  // without the variant
  [[nodiscard]] QString full() const;

  static MaterialRef parse(const QString &s);
  static QString make(const QString &pack, const QString &name, const QString &variant = {});
};

// Which map feeds a material slot (MapType::Count: none). AO / roughness / metallic follow the ORM rule: an ARM
// file feeds all three, else each uses its own file. The renderer and the Godot export both go through this.
MapType slotMapType(const TextureEntry &e, MaterialSlot slot);

QString refKey(const QString &ref);  // lower-cased, trimmed
// A texture pack and a LUT pack may share a name (and their entries a ref): the key of a LUT is "lut:" + refKey.
QString assetKey(const QString &ref, bool lut);
QString packKey(const QString &name, const QString &type);
bool isValidAssetName(const QString &name);  // [A-Za-z0-9_-]{1,64}
