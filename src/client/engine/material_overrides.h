#pragma once

// The material property stack of one map:
//   1. Godot defaults          BaseMaterial3D's own values
//   2. tag / map defaults      MaterialDefaults (computed from the texture)
//   3. server overrides        per texture, shared by every map (sqlite on the server)
//   4. map overrides           this map only, per texture
//   5. variant overrides       this map only, per named variant
// "pack/Name" uses layers 1-4, "pack/Name#variant" 1-5. Layers are sparse (unset keys fall through). 4 and 5 are
// saved in the map (MATL chunk), 3 never.

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>

#include "shared/models/material_props.h"
#include "shared/models/texture_entry.h"

namespace eng {

enum class PropSource : quint8 { Godot = 0, Default, Server, Map, Variant };

struct MaterialVariant {
  QString ref;      // "pack/Name#shiny" (original spelling)
  QString baseRef;  // "pack/Name"
  QString displayName;
  MaterialProps props;

  bool operator==(const MaterialVariant &) const = default;
};

struct ResolvedProps {
  MaterialProps props;   // every key set
  std::array<PropSource, matprops::kKeyCount> source{};
  MaterialProps defaults;  // layer 2 (sparse)
  MaterialProps server;    // layer 3 (sparse)
  MaterialProps map;       // layer 4 (sparse)
  MaterialProps variant;   // layer 5 (sparse)
};

class MaterialOverrides {
public:
  QString projectPack;  // default destination for "copy into pack" from this map

  // Layer 4, keyed by refKey(base ref).
  [[nodiscard]] const MaterialProps *mapLayer(const QString &ref) const;
  void setMapLayer(const QString &ref, const MaterialProps &props);  // empty removes
  [[nodiscard]] QStringList mapLayerRefs() const;

  // Layer 5, keyed by refKey(full ref).
  [[nodiscard]] const MaterialVariant *variant(const QString &ref) const;
  void setVariant(const MaterialVariant &v);
  void removeVariant(const QString &ref);
  [[nodiscard]] QList<MaterialVariant> variants() const { return m_variants.values(); }
  [[nodiscard]] QList<MaterialVariant> variantsOf(const QString &baseRef) const;
  // Unused variant name for this base ("shiny", "shiny_2", ...).
  [[nodiscard]] QString uniqueVariantName(const QString &baseRef, const QString &wanted) const;

  // Texture uuids by ref (MATL), so a renamed texture is still found.
  void rememberId(const QString &ref, const QString &id);
  [[nodiscard]] QString idFor(const QString &ref) const;

  // Renames every reference to `oldRef` (server rename, copy-and-replace).
  void renameTexture(const QString &oldRef, const QString &newRef);

  [[nodiscard]] bool isEmpty() const;
  void clear();
  [[nodiscard]] quint64 version() const { return m_version; }

  // `entry` may be null (local library, or a server texture not resolved yet).
  [[nodiscard]] ResolvedProps resolve(const QString &ref, const TextureEntry *entry) const;

  // MATL chunk body. `resolved`: effective properties of every material used (offline use, Godot export).
  [[nodiscard]] QJsonObject toJson(const QHash<QString, MaterialProps> &resolved = {}) const;
  static MaterialOverrides fromJson(const QJsonObject &o, QHash<QString, MaterialProps> *resolved = nullptr);

  bool operator==(const MaterialOverrides &o) const;

private:
  QHash<QString, MaterialProps> m_map;          // refKey(base) -> layer 4
  QHash<QString, QString> m_mapSpelling;        // refKey(base) -> original spelling
  QHash<QString, MaterialVariant> m_variants;   // refKey(full) -> layer 5
  QHash<QString, QString> m_ids;                // refKey(base) -> uuid
  QHash<QString, QString> m_idSpelling;         // refKey(base) -> original spelling
  quint64 m_version = 1;
};

// Stand-in entry for a local library texture so MaterialDefaults applies.
TextureEntry libraryStandIn(const QString &name, bool hasArm, bool hasRoughness, bool hasMetalness, bool hasAo,
                            bool hasEmission);

} // namespace eng
