#pragma once

#include <atomic>
#include <memory>

#include <QString>

#include <glm/gtc/quaternion.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "mesh.h"

namespace eng {

class GameObject {
public:
  // `stableId` 0 mints a fresh one; loaders pass the saved id so everything keyed by it still matches.
  explicit GameObject(const std::shared_ptr<Mesh> &mesh, quint32 stableId = 0);

  // Identity for this object's lifetime, never reused. Everything derived (picking ids, GPU records, wire slots,
  // selection) is keyed by it, not by vector position.
  [[nodiscard]] quint32 stableId() const { return m_stableId; }
  // Pulls the mint past an id read from disk, so a reloaded map never reuses a file's ids.
  static void reserveIdsAbove(quint32 id);

  void setPosition(glm::vec3 pos) {
    m_position = pos;
    invalidateTransform();
  }

  void setRotation(glm::vec3 rot) {
    m_rotation = rot;
    invalidateTransform();
  }

  void setScale(glm::vec3 scale) {
    m_scale = scale;
    invalidateTransform();
  }

  // Hidden objects are neither drawn nor pickable; locked ones draw but cannot be selected.
  [[nodiscard]] const QString &name() const { return m_name; }
  void setName(const QString &name) { m_name = name; }
  // User-hidden, a mirror copy whose set hides copies (MirrorSet::hideMirrored), or a modifier source replaced
  // by its output (and output of a hidden source).
  [[nodiscard]] bool hidden() const { return m_hidden || m_mirrorHidden || m_modHidden; }
  // The user's own flag: what is saved and undone.
  [[nodiscard]] bool hiddenFlag() const { return m_hidden; }
  void setHidden(bool h) {
    if (m_hidden == h)
      return;
    m_hidden = h;
    ++s_visibilityEpoch;
    invalidateTransform(false);
  }
  // Derived from the mirror set, never saved (Engine3D::refreshMirrorVisibility).
  [[nodiscard]] bool mirrorHidden() const { return m_mirrorHidden; }
  void setMirrorHidden(bool h) {
    if (m_mirrorHidden == h)
      return;
    m_mirrorHidden = h;
    ++s_visibilityEpoch;
    invalidateTransform(false);
  }
  // Derived by the modifier system, never saved (Engine3D::refreshModVisibility).
  [[nodiscard]] bool modHidden() const { return m_modHidden; }
  void setModHidden(bool h) {
    if (m_modHidden == h)
      return;
    m_modHidden = h;
    ++s_visibilityEpoch;
    invalidateTransform(false);
  }
  // A modifier source whose host has output: exporters skip it. Derived, never saved.
  [[nodiscard]] bool modReplaced() const { return m_modReplaced; }
  void setModReplaced(bool r) { m_modReplaced = r; }
  // Modifier host this brush is a SOURCE of, 0 for none. Saved; edits and Edit Source duplicates keep it.
  [[nodiscard]] quint32 modHost() const { return m_modHost; }
  void setModHost(quint32 id) { m_modHost = id; }
  // Non-zero for modifier OUTPUT of that host: derived, never saved, never in undo, never selected as a brush.
  [[nodiscard]] quint32 modOutput() const { return m_modOutput; }
  void setModOutput(quint32 host) { m_modOutput = host; }
  // A model entity's render object or modifier output: editing loops skip these.
  [[nodiscard]] bool isDerived() const { return m_ownerEntity != 0 || m_modOutput != 0; }
  [[nodiscard]] bool locked() const { return m_locked; }
  void setLocked(bool l) { m_locked = l; }
  // Smooth shading (saved): normals blend across faces meeting under kSmoothAngleDeg, sharp elsewhere.
  [[nodiscard]] bool smoothShading() const { return m_smooth; }
  void setSmoothShading(bool on) {
    m_smooth = on;
    if (m_mesh)
      m_mesh->setSmoothShading(on);
  }
  // 0 = default layer, no group.
  [[nodiscard]] quint32 layer() const { return m_layer; }
  void setLayer(quint32 layer) { m_layer = layer; }
  [[nodiscard]] quint32 group() const { return m_group; }
  void setGroup(quint32 group) { m_group = group; }
  [[nodiscard]] static quint64 visibilityEpoch() { return s_visibilityEpoch; }
  // Non-zero for a model entity's render object (prop_static): drawn and shadowed like a brush, but not saved,
  // listed or edited; picking selects the entity.
  [[nodiscard]] quint32 ownerEntity() const { return m_ownerEntity; }
  // Non-zero for a brush entity's brush (func_detail, trigger_*...); unlike ownerEntity, ordinary saved geometry.
  [[nodiscard]] quint32 brushEntity() const { return m_brushEntity; }
  void setBrushEntity(quint32 id) { m_brushEntity = id; }
  // Generated object (stairs, ...) this brush is part of, 0 for none; copies made by edits do not inherit it.
  [[nodiscard]] quint32 procObject() const { return m_procObject; }
  void setProcObject(quint32 id) { m_procObject = id; }
  // Mirror link (Scene::mirrorLinks), 0 for none: both sides stay reflections (Engine3D::syncMirrors).
  // Edits copy it; duplicates do not.
  // Orientation: geometry is baked, so this records how far the brush has been turned (rotations and mirror flips
  // carry it, Engine3D::carryOrientation).
  [[nodiscard]] const glm::dquat &orientation() const { return m_orientation; }
  void setOrientation(const glm::dquat &q) { m_orientation = q; }
  [[nodiscard]] quint32 mirrorLink() const { return m_mirrorLink; }
  [[nodiscard]] int mirrorSide() const { return m_mirrorSide; }
  void setMirrorLink(quint32 link, int side) {
    m_mirrorLink = link;
    m_mirrorSide = link ? side : 0;
  }
  void setOwnerEntity(quint32 id) { m_ownerEntity = id; }

  [[nodiscard]] glm::vec3 getPosition() const { return m_position; }
  [[nodiscard]] glm::vec3 getRotation() const { return m_rotation; }
  [[nodiscard]] glm::vec3 getScale() const { return m_scale; }

  [[nodiscard]] glm::mat4 getModelMatrix() const;

  std::shared_ptr<Mesh> getMesh() { return m_mesh; }
  // Swaps the geometry (vertex-edit preview); bumps the transform so caches refresh.
  void setMesh(const std::shared_ptr<Mesh> &mesh) {
    m_mesh = mesh;
    if (m_smooth && m_mesh)
      m_mesh->setSmoothShading(true);
    invalidateTransform();
  }
  [[nodiscard]] std::shared_ptr<const Mesh> getMesh() const { return m_mesh; }
  // No refcount traffic; per-frame sweeps only read.
  [[nodiscard]] const std::shared_ptr<Mesh> &meshRef() const { return m_mesh; }

  // Bumped by every transform edit: the GPU block is rewritten only for objects that changed.
  [[nodiscard]] quint64 transformVersion() const { return m_transformVersion; }
  // Bumped only on placement or mesh swap, not hide / show: what geometry caches key on.
  [[nodiscard]] quint64 placementVersion() const { return m_placementVersion; }

  // Process-wide: lets consumers skip whole-scene sweeps on frames where nothing moved.
  [[nodiscard]] static quint64 globalTransformEpoch() { return s_transformEpoch; }

  // World-space AABB, recomputed lazily.
  [[nodiscard]] std::pair<glm::vec3, glm::vec3> getWorldAABB() const;
  void computeWorldAABB() const {
    m_cachedWorldAABB = computeAABB();
    m_aabbDirty = false;
  }

private:
  // moved = false: only the GPU block changes (visibility), not the placement.
  void invalidateTransform(bool moved = true);
  [[nodiscard]] std::pair<glm::vec3, glm::vec3> computeAABB() const;

  std::shared_ptr<Mesh> m_mesh;
  quint32 m_stableId = 0;
  QString m_name;
  bool m_hidden = false;
  bool m_locked = false;
  bool m_smooth = false;
  quint32 m_layer = 0;
  quint32 m_group = 0;
  quint32 m_ownerEntity = 0;
  quint32 m_brushEntity = 0;
  quint32 m_procObject = 0;
  bool m_mirrorHidden = false;
  bool m_modHidden = false;
  bool m_modReplaced = false;
  quint32 m_modHost = 0;
  quint32 m_modOutput = 0;
  glm::dquat m_orientation {1.0, 0.0, 0.0, 0.0};
  quint32 m_mirrorLink = 0;
  int m_mirrorSide = 0;
  inline static quint64 s_visibilityEpoch = 0;
  glm::vec3 m_position;
  glm::vec3 m_rotation; // Euler angles in degrees
  glm::vec3 m_scale;

  mutable bool m_transformDirty = true;
  mutable glm::mat4 m_modelMatrix { 1.0f };
  quint64 m_transformVersion = 1;
  quint64 m_placementVersion = 1;

  inline static quint64 s_transformEpoch = 0;
  // Ids start at 1; 0 means "no identity".
  inline static std::atomic<quint32> s_nextStableId { 0 };

  mutable bool m_aabbDirty = true;
  mutable std::pair<glm::vec3, glm::vec3> m_cachedWorldAABB;
};

} // namespace eng
