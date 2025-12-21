#pragma once

// Export to Godot (4.8+):
//   textures/<pack>/<File>.png        full-res maps (downloaded by the caller)
//   textures/<pack>/<name>.tres       shared StandardMaterial3D
//   levels/<map>/<map>.tscn           the level scene (merged in place)
//   levels/<map>/*.glb                worldspawn, groups, stairs, brush entities
//   levels/<map>/materials/*.tres     materials this map overrides or varies
//   levels/<map>/<map>_io.gd          entity I/O (addons/ghammer)
//   textures/luts/<Name>.png          colour LUTs as Texture3D; shared by every map (same names overwrite)
// planLevel() is pure: partitions brushes, drops unseen faces (covered, or on the dev texture), builds the .glb
// bytes and lists materials / textures. The merge functions edit Godot files in place (godot_text.h).
// writeLevel() writes everything but the texture downloads, which the caller does first.

#include <QColor>
#include <QImage>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "shared/models/material_props.h"
#include "shared/models/texture_entry.h"
#include "entity.h"
#include "face_graph.h"

namespace eng {
class Scene;
struct Entity;
struct ColorLut;
}

namespace eng::godot {

// Project-relative paths ("textures/pack/X.png"); res://<rel> in Godot.
struct TextureJob {
  QString rel;
  // Server source (GET /api/2/source/<pack>/<name>/<mapKey>), or a local file.
  QString pack, name, mapKey, checksum;
  QString localSource;
  // Import hints for a new .import (Godot owns it afterwards).
  bool normal = false;
  bool normalDirectX = false;
  int roughnessChannel = -1;  // Godot's roughness/mode = channel + 2
  QString roughnessSrcNormal; // rel path of the normal map
  bool sky = false;           // a skybox panorama (.exr): HDR import, no mipmaps
};

// Albedo with the opacity map in its alpha (alpha-scissor materials only).
struct DerivedTexture {
  QString rel, colorRel, opacityRel;
};

struct SlotTexture {
  QString rel;
  int channel = 0;  // Godot TextureChannel: 0 R, 1 G, 2 B, 3 A, 4 gray
};

struct MaterialJob {
  QString ref;       // as the faces spell it
  QString gltfName;  // material name inside the .glb (key of the .import binding)
  QString tresRel;
  bool isShared = true;     // textures/<pack>/ (no map / variant layer)
  bool writeTres = true;  // false when the texture is unknown (offline): file kept as is
  MaterialProps props;    // effective
  std::array<std::optional<SlotTexture>, size_t(MaterialSlot::Count)> textures;
};

struct GlbJob {
  QString key;     // file base name and node name ("worldspawn", "group_hall", "func_door_12")
  QString rel;     // levels/<map>/<key>.glb
  quint32 entity = 0;  // brush entity id (func_door ...), 0 otherwise
  QByteArray bytes;
  QMap<QString, QString> materials;  // gltf material name -> .tres rel
  int faces = 0;
  int collisionTriangles = 0;  // of the merged <key>_collision-colonly mesh
};

// The strip as a png Godot imports as a Texture3D (the png is the resource, no .tres).
struct LutJob {
  QString rel;   // textures/luts/<Name>.png
  QString name;
  QImage strip;  // RGB888
};

// The skybox's .exr (a TextureJob) and a PanoramaSkyMaterial at textures/<pack>/<name>.tres; not used by the
// level scene yet (no WorldEnvironment export).
struct SkyJob {
  QString ref;      // "sourceskyboxes/SkyCloudy017"
  QString tresRel;  // textures/<pack>/<name>.tres
  QString texRel;   // textures/<pack>/<file>.exr
};

struct LevelPlan {
  QString mapName;  // file-safe ("box")
  QString levelRel; // "levels/box"
  std::vector<GlbJob> glbs;
  std::vector<MaterialJob> materials;
  std::vector<TextureJob> textures;
  std::vector<DerivedTexture> derived;
  std::vector<LutJob> luts;  // the level's colour LUT (at most one)
  std::optional<SkyJob> sky;
  int faces = 0;        // written
  int hiddenFaces = 0;  // removed: covered by other world brushes
  int devFaces = 0;     // removed: dev / default texture
  QStringList warnings;
};

struct LevelOptions {
  QString mapName = QStringLiteral("untitled");
  double unitScale = kMetersPerWorldUnit;  // world units -> metres
  QString devMaterialPref;   // Brush/DefaultMaterial
  // Catalogue entry of a base ref ("pack/Name"), or nullptr.
  std::function<const TextureEntry *(const QString &baseRef)> entryFor;
  QString skyRef;  // Engine3D::skybox(), empty = none
  QString lutRef;  // Engine3D::colorLut(), empty = none
  QImage lutStrip; // its strip (RGB888); null when not loaded
  // Export/Collision: false drops every static collision node and the clip brushes (rigid bodies keep hulls).
  bool exportCollision = true;
  // Export/DevMaterials: true keeps dev-textured faces, with the editor's built-in measurewall image.
  bool exportDevMaterials = false;
};

// World brush faces fully covered by other world brushes: the expensive part of planLevel. Inputs are gathered
// on the GUI thread; computeHiddenFaces is pure (worker thread), reusable while hiddenFaceKey is unchanged.
struct HiddenFaces {
  quint64 inputKey = 0;                    // hiddenFaceKey() of the inputs
  std::unordered_set<quint64> faces;       // (GameObject::stableId() << 32) | face index
  std::shared_ptr<const FaceGraph> graph;  // what EDITOR_GODOT_DROPDUMP reports from
};
std::vector<SolidInput> hiddenFaceInputs(const Scene &scene);
quint64 hiddenFaceKey(const std::vector<SolidInput> &solids);
HiddenFaces computeHiddenFaces(const std::vector<SolidInput> &solids);

// `hidden`: computeHiddenFaces(hiddenFaceInputs(scene)) by the caller; null computes it here.
LevelPlan planLevel(const Scene &scene, const LevelOptions &opt, const HiddenFaces *hidden = nullptr);
// textures/luts/<Name>.png, _2, _3 … for repeated names.
std::vector<LutJob> planLuts(const std::vector<ColorLut> &luts);

using UidFn = std::function<QString(const QString &rel)>;

// Only the keys the editor manages change.
QString mergeMaterialTres(const QString &existing, const MaterialJob &m, const UidFn &uidFor = {});
// <glb>.import: nodes/use_name_suffixes and the external material bindings.
QString mergeGlbImport(const QString &existing, const QMap<QString, QString> &materialRels);
// Never rewritten once it exists. A skybox: VRAM compressed HDR (BC6H / ASTC), no mipmaps, never seen as 3D.
QString newTextureImport(const TextureJob &t);
// `panorama` changes, the user's other keys stay; a file of another type is left alone.
QString mergeSkyTres(const QString &existing, const SkyJob &sky, const UidFn &uidFor = {});
// Godot's "3d_texture" importer cutting the strip into `slices` horizontal slices (default 8×8), lossless, no
// mipmaps. Keeps the uid and other keys; a 2D texture import is replaced.
QString mergeLutImport(const QString &existing, int slices);

struct SceneSun {
  bool present = false;
  glm::vec3 direction {0.0f, -1.0f, 0.0f};  // the way the light travels
  QColor color {Qt::white};
  float energy = 1.0f;  // Godot light_energy
  bool shadows = true;
};

struct SceneMerge {
  QString text;
  QHash<quint32, QString> entityNodes;  // entity id -> node path below the level root
  QStringList warnings;
};
SceneMerge mergeLevelScene(const QString &existing, const std::vector<Entity> &entities, const LevelPlan &plan,
                           const SceneSun &sun,
                           const QString &ioScriptRel, double unitScale, const UidFn &uidFor = {});

// Node basis looking along `forward` (Godot nodes look down -Z), +Y up.
void basisLookingAlong(const glm::vec3 &forward, glm::vec3 &x, glm::vec3 &y, glm::vec3 &z);

// Oldest Godot targeted. checkProject: empty when `root` holds a Godot 4.8+ project, else why not.
inline constexpr int kMinGodotMajor = 4;
inline constexpr int kMinGodotMinor = 8;
QString checkProject(const QString &root);

// textures/.level_editor_textures.json: rel -> server checksum.
QJsonObject readTextureManifest(const QString &root);
bool writeTextureManifest(const QString &root, const QJsonObject &manifest);
// The file exists and was downloaded with this checksum.
bool textureUpToDate(const QString &root, const TextureJob &t, const QJsonObject &manifest);

struct WriteOptions {
  QString root;
  QString sourcePath;  // the .ghmap (I/O script header)
  SceneSun sun;
  bool writeAddon = true;
  double unitScale = kMetersPerWorldUnit;
};

struct WriteResult {
  int written = 0;
  int luts = 0;  // LUT pngs exported
  QString skyTres;  // the skybox's PanoramaSkyMaterial (rel), empty = none
  int unchanged = 0;
  int removed = 0;
  int ioEntities = 0;
  QString scenePath;
  QStringList warnings;
  QStringList errors;
};

WriteResult writeLevel(const Scene &scene, const LevelPlan &plan, const WriteOptions &opt);

} // namespace eng::godot
