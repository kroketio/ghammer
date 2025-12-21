#pragma once

#include "material_overrides.h"
#include "modifiers.h"
#include "path_curve.h"

#include <filesystem>
#include <memory>
#include <set>
#include <vector>

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <QColor>

#include "directional_light.h"
#include "face_groups.h"
#include "gameobject.h"
#include "entity.h"
#include "color_lut.h"
#include "mirror_gen.h"

namespace eng {

// Every brush belongs to exactly one group; id 0 is worldspawn, which always exists. Hidden / locked are applied
// to the members when toggled, so rendering and picking only look at the objects.
struct MeshGroup {
  quint32 id = 0;
  QString name;
  QColor color {120, 160, 220};
  bool hidden = false;
  bool locked = false;
};
inline constexpr quint32 kWorldspawnGroup = 0;

// Picking ids come from stable identity, never a slot, so removing an object renumbers nothing:
//   pickingId = stableId * kPickingFacesPerObject + face + 1
// 0 = nothing hit. 128 faces per brush is a ceiling; with 32-bit ids that leaves room for 33 million objects.
inline constexpr uint32_t kPickingFacesPerObject = 128;

[[nodiscard]] inline constexpr uint32_t makePickingId(const uint32_t stableId, const uint32_t face) {
  return stableId * kPickingFacesPerObject + face + 1u;
}
// Only valid for a non-zero picking id.
[[nodiscard]] inline constexpr uint32_t stableIdFromPicking(const uint32_t pickingId) {
  return (pickingId - 1u) / kPickingFacesPerObject;
}
[[nodiscard]] inline constexpr uint32_t faceFromPicking(const uint32_t pickingId) {
  return (pickingId - 1u) % kPickingFacesPerObject;
}

class Scene final : public QObject {
Q_OBJECT
public:
  explicit Scene(QObject *parent = nullptr);

  static QSharedPointer<Scene> VMF(const std::filesystem::path &path);

  const QVector<DirectionalLight *> &directionalLights() const { return m_directionalLights; }

  std::vector<std::shared_ptr<GameObject>> gameObjects;

  // Point entities; owned by the scene so loaders fill them like the brushes.
  std::vector<Entity> entities;
  quint32 nextEntityId = kEntityIdBase + 1;

  [[nodiscard]] Entity *findEntity(quint32 id) {
    for (auto &e: entities)
      if (e.id == id)
        return &e;
    return nullptr;
  }
  quint32 mintEntityId() { return nextEntityId++; }

  // groups[0] is always worldspawn. New brushes go into the active group.
  std::vector<MeshGroup> groups {MeshGroup {kWorldspawnGroup, QStringLiteral("worldspawn"), QColor(120, 160, 220)}};
  quint32 activeGroup = kWorldspawnGroup;
  // Material overrides (map layer, variants, project pack): the MATL chunk. See material_overrides.h.
  MaterialOverrides materialOverrides;
  // Face group names (Floor, Wall, Ceiling, Vent plus user groups); faces store ids into this table.
  FaceGroupTable faceGroups;
  // Level-scope settings (prefs::Scope::Level): only keys off their defaults. LCFG chunk; edited through undoable
  // steps (Engine3D::setLevelSetting).
  QJsonObject levelSettings;

  // Brushes the editor rebuilds from parameters (stairs, arches). `type` picks the generator, `params` its JSON.
  struct ProcObject {
    quint32 id = 0;
    QString type;
    QString name;
    QJsonObject params;
  };
  std::vector<ProcObject> procObjects;
  [[nodiscard]] ProcObject *findProc(quint32 id) {
    for (auto &p: procObjects)
      if (p.id == id)
        return &p;
    return nullptr;
  }
  [[nodiscard]] const ProcObject *findProc(quint32 id) const {
    for (const auto &p: procObjects)
      if (p.id == id)
        return &p;
    return nullptr;
  }
  [[nodiscard]] quint32 mintProcId() const {
    quint32 next = 1;
    for (const auto &p: procObjects)
      next = std::max(next, p.id + 1);
    return next;
  }
  // Brushes tagged with a link id (GameObject::mirrorLink) on two sides of the link's own plane, kept reflections
  // of each other.
  struct MirrorLink {
    quint32 id = 0;
    mirror::Plane plane;
    bool mirrorTextures = false; // the copy shows the mirrored texture
    quint32 set = 0;             // the mirror set (collection)
  };
  std::vector<MirrorLink> mirrorLinks;
  [[nodiscard]] const MirrorLink *findMirrorLink(quint32 id) const {
    for (const auto &l: mirrorLinks)
      if (l.id == id)
        return &l;
    return nullptr;
  }
  [[nodiscard]] MirrorLink *findMirrorLink(quint32 id) {
    for (auto &l: mirrorLinks)
      if (l.id == id)
        return &l;
    return nullptr;
  }
  [[nodiscard]] quint32 mintMirrorLinkId() const {
    quint32 next = 1;
    for (const auto &l: mirrorLinks)
      next = std::max(next, l.id + 1);
    return next;
  }
  // What the user sees as one mirror ("Mirror 1"): every link made during one line placement (a mirrored
  // corridor is many links, one set). A duplicated set becomes a new set.
  struct MirrorSet {
    quint32 id = 0;
    QString name;
    bool hideMirrored = false; // side 1 (the copies) hidden in the views
  };
  std::vector<MirrorSet> mirrorSets;
  [[nodiscard]] const MirrorSet *findMirrorSet(quint32 id) const {
    for (const auto &s: mirrorSets)
      if (s.id == id)
        return &s;
    return nullptr;
  }
  [[nodiscard]] MirrorSet *findMirrorSet(quint32 id) {
    for (auto &s: mirrorSets)
      if (s.id == id)
        return &s;
    return nullptr;
  }
  [[nodiscard]] quint32 mintMirrorSetId() const {
    quint32 next = 1;
    for (const auto &s: mirrorSets)
      next = std::max(next, s.id + 1);
    return next;
  }
  // Mirror mode's line (what new links copy), saved with the map.
  bool mirrorLineValid = false;
  mirror::Plane mirrorLine;
  // The set new links on the current line join; 0: a new one on the next link.
  quint32 mirrorActiveSet = 0;

  // Modifier hosts: each owns source brushes (GameObject::modHost) and a stack; the output is derived
  // (GameObject::modOutput) and never stored. modEpoch moves on every change (undo included).
  std::vector<mods::Host> modHosts;
  quint64 modEpoch = 0;
  [[nodiscard]] const mods::Host *findModHost(quint32 id) const {
    for (const auto &h: modHosts)
      if (h.id == id)
        return &h;
    return nullptr;
  }
  [[nodiscard]] mods::Host *findModHost(quint32 id) {
    for (auto &h: modHosts)
      if (h.id == id)
        return &h;
    return nullptr;
  }
  [[nodiscard]] quint32 mintModHostId() const {
    quint32 next = 1;
    for (const auto &h: modHosts)
      next = std::max(next, h.id + 1);
    return next;
  }

  // Drawn bezier curves or Connect paths that Curve modifiers follow. pathEpoch moves on every change.
  std::vector<path::Path> paths;
  quint64 pathEpoch = 0;
  [[nodiscard]] const path::Path *findPath(quint32 id) const {
    for (const auto &p: paths)
      if (p.id == id)
        return &p;
    return nullptr;
  }
  [[nodiscard]] path::Path *findPath(quint32 id) {
    for (auto &p: paths)
      if (p.id == id)
        return &p;
    return nullptr;
  }
  [[nodiscard]] quint32 mintPathId() const {
    quint32 next = 1;
    for (const auto &p: paths)
      next = std::max(next, p.id + 1);
    return next;
  }

  [[nodiscard]] MeshGroup *findGroup(quint32 id) {
    for (auto &g: groups)
      if (g.id == id)
        return &g;
    return nullptr;
  }
  [[nodiscard]] const MeshGroup *findGroup(quint32 id) const {
    for (const auto &g: groups)
      if (g.id == id)
        return &g;
    return nullptr;
  }
  [[nodiscard]] quint32 mintGroupId() const {
    quint32 next = 1;
    for (const auto &g: groups)
      next = std::max(next, g.id + 1);
    return next;
  }
  // Worldspawn first, and every object's group exists (old maps reference bare ids).
  void normalizeGroups();

  // Bumped when the object list changes shape (cached draw lists are stale).
  quint64 structureVersion() const { return m_structureVersion; }
  void addObject(const std::shared_ptr<GameObject> &object);
  void addObjects(const std::vector<std::shared_ptr<GameObject>> &objects);

  // One pass, one version bump, one signal. Stable ids name the same objects before and after.
  void removeObjects(const std::set<quint32> &stableIds);

  // Puts objects back at their old slots, one version bump. `slotList` must be ascending.
  void insertObjects(const std::vector<std::shared_ptr<GameObject>> &objects,
                     const std::vector<int> &slotList);

  // -1 when gone. The map is rebuilt lazily after a structure change.
  [[nodiscard]] int indexOf(quint32 stableId) const;
  [[nodiscard]] std::shared_ptr<GameObject> findById(quint32 stableId) const;
  // Null when out of range.
  [[nodiscard]] std::shared_ptr<GameObject> at(size_t slot) const {
    return slot < gameObjects.size() ? gameObjects[slot] : nullptr;
  }

signals:
  void structureChanged();

private:
  void rebuildIdIndex() const;

  QVector<DirectionalLight *> m_directionalLights;
  quint64 m_structureVersion = 0;

  mutable quint64 m_idIndexVersion = ~0ull;
  mutable QHash<quint32, int> m_idToSlot;
};

} // namespace eng
