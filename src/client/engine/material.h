#pragma once

#include <QSharedPointer>
#include <QString>

#include <glm/glm.hpp>

#include "shared/models/material_props.h"
#include "shared/models/texture_entry.h"

class QRhi;
class QRhiTexture;
class QRhiSampler;
class QRhiShaderResourceBindings;

namespace eng {

// CPU side of one material. GPU handles belong to the texture manager, valid only while their QRhi lives.
// `ref`: "pack/Name" (server), "pack/Name#variant" (map-local variant) or a bare name (local library, old maps);
// `name` is it lower-cased, the lookup key. Anything can change at any time: `version` bumps when the bound textures
// change (SRBs rebuilt); propsChanged() only bumps the global epoch (per-face blocks rewritten).
class Material {
public:
  Material() = default;
  explicit Material(const QString &name);

  enum class Kind : quint8 { Library, Server, File };
  enum class State : quint8 { Pending, Ready, Error };

  QString name;  // lower-case key
  QString ref;   // spelling saved in maps
  Kind kind = Kind::Library;
  State state = State::Pending;

  // Server textures: the catalogue entry.
  TextureEntry entry;
  [[nodiscard]] bool hasEntry() const { return entry.isValid(); }

  // Variants draw with their base's textures unless their texture channels differ.
  QSharedPointer<Material> gpuSource;
  quint64 gpuSourceVersion = 0;

  QRhiTexture *albedo = nullptr;
  QRhiTexture *normal = nullptr;
  QRhiTexture *metallicRoughness = nullptr;  // ORM: R = AO, G = roughness, B = metal
  QRhiTexture *emission = nullptr;

  // The above may be shared fallbacks; these are the ones this material created and must release.
  QRhiTexture *ownedAlbedo = nullptr;
  QRhiTexture *ownedNormal = nullptr;
  QRhiTexture *ownedMr = nullptr;
  QRhiTexture *ownedEmission = nullptr;
  bool ownsTextures = false;
  // The QRhi the textures above belong to, and the current load.
  QRhi *gpuRhi = nullptr;
  quint64 loadTicket = 0;

  // Maps that really exist (not fallbacks).
  bool realNormal = false;
  bool realOrm = false;
  bool realEmission = false;

  // Local texture library maps (non-server materials).
  QString libAlbedo, libNormal, libRoughness, libMetallic, libAo, libArm, libEmissive;
  bool libNormalDirectX = false;

  // Native diffuse width in pixels; the VMF texture scale is relative to it (world tile = texWidth * scale Source
  // units). Server textures: the *source* size (usually 1024), not the 512 px editor copy, so UVs match Godot.
  int texWidth = 512;
  int texHeight = 512;

  [[nodiscard]] bool hasNormalMap() const { return realNormal; }
  [[nodiscard]] bool hasMetallicRoughnessMap() const { return realOrm; }
  [[nodiscard]] bool hasEmissionMap() const { return realEmission; }

  // Fully resolved properties (all five layers, see material_overrides.h).
  MaterialProps props = MaterialProps::godotDefaults();
  // Channels the ORM image was composed with (a change re-composes).
  matprops::Channel loadedAoChannel = matprops::Channel::Red;
  matprops::Channel loadedRoughnessChannel = matprops::Channel::Green;
  matprops::Channel loadedMetallicChannel = matprops::Channel::Blue;

  glm::vec3 albedoTint { 1.0f, 1.0f, 1.0f };  // tool overlays (collision, triggers)

  quint64 version = 1;
  void touch() {
    ++version;
    ++s_globalEpoch;
  }
  static void propsChanged() { ++s_globalEpoch; }

  // A baked material buffer may skip its rewrite only when no material changed, or a lazily loaded normal map
  // bakes out of existence.
  [[nodiscard]] static quint64 globalEpoch() { return s_globalEpoch; }

  inline static quint64 s_globalEpoch = 0;

  // One SRB per material: only the texture slots differ.
  QRhiShaderResourceBindings *srb = nullptr;
  quint64 srbVersion = 0;
  // The object / face UBOs grow in place; a buffer swap invalidates every SRB pointing at the old one.
  quint64 srbBufferGen = 0;
  QRhiShaderResourceBindings *srbShadow = nullptr;
  const void *srbOwner = nullptr;  // the engine that built srb / srbShadow
};

using MaterialPtr = QSharedPointer<Material>;

// The dev texture new brushes get (devtexture01, dev/dev_measurewall01a / 01d, or `defaultPref`). The 3D view
// draws its built-in image; the Godot export leaves such faces out unless Export/DevMaterials is on.
bool isDevMaterial(const QString &name, const QString &defaultPref = {});

} // namespace eng
