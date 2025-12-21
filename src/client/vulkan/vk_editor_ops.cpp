// Editor operations called by panels, menus and shortcuts (kept out of vk_engine.cpp).

#include <cfloat>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include "vk_engine.h"
#include "vk_undo.h"

#include "client/engine/scene.h"
#include "client/engine/entity.h"
#include "client/vulkan/vk_shared.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

std::vector<quint32> toVector(const std::set<quint32> &s) { return {s.begin(), s.end()}; }

} // namespace

QString Engine3D::entityDisplayName(const quint32 entityId) const {
  if (const eng::Entity *e = entityById(entityId)) {
    // "crate (func_rigidbody)"; unnamed, the classname alone.
    return e->name.isEmpty() ? e->classname : QStringLiteral("%1 (%2)").arg(e->name, e->classname);
  }
  return QStringLiteral("Entity %1").arg(entityId);
}

bool Engine3D::objectHidden(const quint32 id) const {
  const auto obj = m_scene ? m_scene->findById(id) : nullptr;
  return obj && obj->hiddenFlag();
}

bool Engine3D::objectLocked(const quint32 id) const {
  const auto obj = m_scene ? m_scene->findById(id) : nullptr;
  return obj && obj->locked();
}

void Engine3D::setObjectsHidden(const std::vector<quint32> &ids, const bool hidden) {
  if (!m_scene || ids.empty())
    return;
  std::vector<std::pair<quint32, bool>> before;
  for (const quint32 id: ids)
    if (const auto o = m_scene->findById(id); o && !o->isDerived()) // output follows its source
      before.emplace_back(id, o->hiddenFlag());
  auto apply = [this](const std::vector<std::pair<quint32, bool>> &states) {
    for (const auto &[id, h]: states)
      if (const auto o = m_scene ? m_scene->findById(id) : nullptr)
        o->setHidden(h);
    // Hidden objects cannot stay selected.
    std::vector<quint32> keep;
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->hidden())
        keep.push_back(id);
    if (keep.size() != m_selectedObjects.size())
      setSelected(keep, toVector(m_selectedFaces));
    invalidateDerivedCaches();
    emit objectStateChanged();
    emit sceneChanged();
  };
  std::vector<std::pair<quint32, bool>> after;
  for (const auto &[id, h]: before)
    after.emplace_back(id, hidden);
  apply(after);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        hidden ? QStringLiteral("Hide") : QStringLiteral("Show"), [apply, before] { apply(before); },
        [apply, after] { apply(after); }));
}

void Engine3D::setObjectsLocked(const std::vector<quint32> &ids, const bool locked) {
  if (!m_scene)
    return;
  for (const quint32 id: ids)
    if (const auto o = m_scene->findById(id); o && !o->isDerived()) // output follows its source
      o->setLocked(locked);
  if (locked) {
    std::vector<quint32> keep;
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->locked())
        keep.push_back(id);
    setSelected(keep, toVector(m_selectedFaces));
  }
  emit objectStateChanged();
}

void Engine3D::hideSelected() {
  setObjectsHidden(toVector(m_selectedObjects), true);
}

void Engine3D::isolateSelected() {
  if (!m_scene || m_selectedObjects.empty())
    return;
  std::vector<quint32> others;
  for (const auto &o: m_scene->gameObjects)
    if (!m_selectedObjects.count(o->stableId()) && !o->hidden())
      others.push_back(o->stableId());
  setObjectsHidden(others, true);
}

void Engine3D::showAll() {
  if (!m_scene)
    return;
  std::vector<quint32> hidden;
  for (const auto &o: m_scene->gameObjects)
    if (o->hiddenFlag())
      hidden.push_back(o->stableId());
  setObjectsHidden(hidden, false);
  for (auto &g: m_scene->groups)
    g.hidden = false;
  emit meshGroupsChanged();
}

void Engine3D::lockSelected() {
  setObjectsLocked(toVector(m_selectedObjects), true);
}

void Engine3D::unlockAll() {
  if (!m_scene)
    return;
  std::vector<quint32> all;
  for (const auto &o: m_scene->gameObjects)
    if (o->locked())
      all.push_back(o->stableId());
  setObjectsLocked(all, false);
  for (auto &g: m_scene->groups)
    g.locked = false;
  emit meshGroupsChanged();
}

int Engine3D::hiddenCount() const {
  int n = 0;
  if (m_scene)
    for (const auto &o: m_scene->gameObjects)
      n += o->hiddenFlag() ? 1 : 0;
  return n;
}

void Engine3D::selectAll() {
  if (!m_scene)
    return;
  if (m_materialMode) {
    std::vector<quint32> faces;
    for (const auto &o: m_scene->gameObjects)
      if (const auto mesh = o->getMesh(); mesh && !o->hidden())
        for (int f = 0; f < mesh->faceCount(); ++f)
          faces.push_back(mesh->getFacePickingID(f));
    setSelected({}, faces);
    return;
  }
  std::vector<quint32> ids;
  ids.reserve(m_scene->gameObjects.size());
  for (const auto &o: m_scene->gameObjects)
    if (!o->hidden() && !o->locked() && !o->isDerived())
      ids.push_back(o->stableId());
  setSelected(ids, {});
  std::vector<quint32> ents;
  for (const auto &e: m_scene->entities)
    if (!e.hidden)
      ents.push_back(e.id);
  setSelectedEntities(ents);
}

void Engine3D::invertSelection() {
  if (!m_scene)
    return;
  if (m_materialMode) {
    std::vector<quint32> faces;
    for (const auto &o: m_scene->gameObjects)
      if (const auto mesh = o->getMesh(); mesh && !o->hidden())
        for (int f = 0; f < mesh->faceCount(); ++f)
          if (const quint32 pid = mesh->getFacePickingID(f); pid && !m_selectedFaces.count(pid))
            faces.push_back(pid);
    setSelected({}, faces);
    return;
  }
  std::vector<quint32> ids;
  for (const auto &o: m_scene->gameObjects)
    if (!o->hidden() && !o->locked() && !o->isDerived() && !m_selectedObjects.count(o->stableId()))
      ids.push_back(o->stableId());
  setSelected(ids, {});
}

void Engine3D::selectByMaterial() {
  if (!m_scene)
    return;
  if (m_materialMode) {
    selectSameMaterialFaces(false); // Auto-select's Same material
    return;
  }
  QSet<QString> names;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && o->getMesh())
      for (int f = 0; f < o->getMesh()->faceCount(); ++f)
        if (const auto m = o->getMesh()->faceMaterialRef(f))
          names.insert(m->name);
  for (const quint32 pid: m_selectedFaces)
    if (const auto o = m_scene->findById(eng::stableIdFromPicking(pid)); o && o->getMesh()) {
      const int f = faceIndexOfPicking(o->getMesh(), pid);
      if (f >= 0)
        if (const auto m = o->getMesh()->faceMaterialRef(f))
          names.insert(m->name);
    }
  if (names.isEmpty())
    return;
  std::vector<quint32> ids;
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o->getMesh();
    if (!mesh || o->hidden() || o->locked())
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (const auto m = mesh->faceMaterialRef(f); m && names.contains(m->name)) {
        ids.push_back(o->stableId());
        break;
      }
  }
  setSelected(ids, {});
}

void Engine3D::selectFacesWithMaterial(const QString &name) {
  if (!m_scene)
    return;
  const QString key = name.toLower();
  std::vector<quint32> faces, objects;
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o->getMesh();
    if (!mesh || o->isDerived())
      continue;
    bool any = false;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (const auto m = mesh->faceMaterialRef(f); m && m->name == key) {
        faces.push_back(mesh->getFacePickingID(f));
        any = true;
      }
    // Modifier sources' faces count; the brushes only when shown (else Object mode picks their modified object).
    if (any && !o->locked() && !o->hidden())
      objects.push_back(o->stableId());
  }
  if (m_materialMode)
    setSelected({}, faces);
  else
    setSelected(objects, {});
}

bool Engine3D::selectionBounds(glm::vec3 &mn, glm::vec3 &mx) const {
  glm::dvec3 a, b;
  bool any = false;
  if (selectionWorldAABB(a, b)) {
    mn = glm::vec3(a);
    mx = glm::vec3(b);
    any = true;
  }
  // Faces count by their brush; the entity by its marker box.
  if (!m_selectedFaces.empty() && m_scene) {
    for (const quint32 pid: m_selectedFaces) {
      const auto o = m_scene->findById(eng::stableIdFromPicking(pid));
      if (!o)
        continue;
      const auto [lo, hi] = o->getWorldAABB();
      mn = any ? glm::min(mn, lo) : lo;
      mx = any ? glm::max(mx, hi) : hi;
      any = true;
    }
  }
  for (const auto &[id, o]: selectedEntityOrigins()) {
    const glm::vec3 h(0.8f);
    mn = any ? glm::min(mn, o - h) : o - h;
    mx = any ? glm::max(mx, o + h) : o + h;
    any = true;
  }
  return any;
}

void Engine3D::focusSelection() {
  glm::vec3 mn, mx;
  if (!selectionBounds(mn, mx)) {
    if (!m_scene || m_scene->gameObjects.empty())
      return;
    mn = glm::vec3(FLT_MAX);
    mx = glm::vec3(-FLT_MAX);
    for (const auto &o: m_scene->gameObjects) {
      if (o->hidden())
        continue;
      const auto [lo, hi] = o->getWorldAABB();
      mn = glm::min(mn, lo);
      mx = glm::max(mx, hi);
    }
    if (mn.x > mx.x)
      return;
  }
  const glm::vec3 center = (mn + mx) * 0.5f;
  const float radius = std::max(0.5f, glm::length(mx - mn) * 0.5f);
  const float halfFov = glm::radians(m_camera->getFov()) * 0.5f;
  const float dist = radius / std::sin(std::max(0.1f, halfFov)) * 1.1f;
  const glm::vec3 dir = m_camera->getDirection();
  m_camera->setPose(center - dir * dist, m_camera->getYaw(), m_camera->getPitch());
  m_orbitPivot = center;
  requestRedraw();
}

void Engine3D::translateSelection(const glm::vec3 &delta, const QString &label) {
  if (!m_scene || glm::length(delta) == 0.0f)
    return;
  // Selected modified objects move through their sources.
  std::vector<quint32> ids = moveTargets();
  if (const QString why = modMoveBlocked(); !why.isEmpty())
    emit toastRequested(why);
  // (selected entities are moved below)
  if (ids.empty() && !m_selectedFaces.empty()) {
    std::set<quint32> owners;
    for (const quint32 pid: m_selectedFaces)
      owners.insert(eng::stableIdFromPicking(pid));
    ids = toVector(owners);
  }
  ids.erase(std::remove_if(ids.begin(), ids.end(), [this](quint32 id) { return objectLocked(id); }), ids.end());

  std::vector<glm::vec3> basePos, baseScale;
  std::vector<DragFaceUv> baseUv;
  for (const quint32 id: ids) {
    const auto o = m_scene->findById(id);
    basePos.push_back(o ? o->getPosition() : glm::vec3(0.0f));
    baseScale.push_back(o ? o->getScale() : glm::vec3(1.0f));
  }
  captureFaceUv(ids, baseUv);
  for (size_t k = 0; k < ids.size(); ++k)
    if (const auto o = m_scene->findById(ids[k]))
      o->setPosition(basePos[k] + delta);
  applyTextureLockMove(ids, baseUv, delta);

  const auto entBase = selectedEntityOrigins();
  moveSelectedEntities(entBase, delta);
  recordEntityMoves(entBase, label);
  recordTransformEdit(label, ids, basePos, baseScale, baseUv);
  emit sceneChanged();
  emit objectStateChanged();
}

void Engine3D::nudgeSelection(const int axis, const int direction) {
  if (axis < 0 || axis > 2 || direction == 0)
    return;
  glm::vec3 d(0.0f);
  d[axis] = float(direction) * float(gizmoSnapStep());
  if (m_editMode && !m_selectedVerts.empty()) {
    nudgeVertices(d);
    return;
  }
  // Material mode: move the picked faces' texture one grid step, or rotate it by 15 degrees.
  if (m_materialMode && !m_selectedFaces.empty()) {
    const double step = gizmoSnapStep() * kSourceUnitsPerWorldUnit * direction;
    if (axis == 0)
      shiftFaceTexturesSource(-step, 0.0);
    else if (axis == 2)
      shiftFaceTexturesSource(0.0, -step);
    else
      rotateFaceTextures(15.0 * direction);
    return;
  }
  translateSelection(d, QStringLiteral("Nudge"));
}

void Engine3D::setSelectionBounds(const glm::vec3 &newMin, const glm::vec3 &newMax) {
  glm::vec3 mn, mx;
  if (!m_scene || !selectionBounds(mn, mx))
    return;
  // A generated object is rebuilt in the new box instead of being stretched.
  if (const quint32 proc = selectedProcObject(); proc && procMembers(proc).size() == m_selectedObjects.size()) {
    const glm::vec3 size = newMax - newMin;
    if (glm::any(glm::greaterThan(glm::abs(size - (mx - mn)), glm::vec3(1e-4f)))) {
      setProcParams(proc, procParamsForBounds(proc, glm::dvec3(newMin), glm::dvec3(newMax)),
                    QStringLiteral("Resize %1").arg(procName(proc)));
      return;
    }
  }
  const glm::vec3 oldSize = mx - mn;
  const glm::vec3 newSize = glm::max(newMax - newMin, glm::vec3(0.001f));
  glm::vec3 factor(1.0f);
  for (int a = 0; a < 3; ++a)
    factor[a] = oldSize[a] > 1e-5f ? newSize[a] / oldSize[a] : 1.0f;

  if (glm::all(glm::lessThan(glm::abs(factor - glm::vec3(1.0f)), glm::vec3(1e-5f)))) {
    translateSelection(newMin - mn, QStringLiteral("Move"));
    return;
  }

  std::vector<quint32> ids = toVector(m_selectedObjects);
  ids.erase(std::remove_if(ids.begin(), ids.end(), [this](quint32 id) { return objectLocked(id); }), ids.end());
  std::vector<glm::vec3> basePos, baseScale;
  std::vector<DragFaceUv> baseUv;
  for (const quint32 id: ids) {
    const auto o = m_scene->findById(id);
    basePos.push_back(o ? o->getPosition() : glm::vec3(0.0f));
    baseScale.push_back(o ? o->getScale() : glm::vec3(1.0f));
  }
  captureFaceUv(ids, baseUv);
  for (size_t k = 0; k < ids.size(); ++k) {
    const auto o = m_scene->findById(ids[k]);
    if (!o)
      continue;
    // Position is the object origin; map it through the box remap.
    o->setPosition(newMin + (basePos[k] - mn) * factor);
    o->setScale(baseScale[k] * factor);
  }
  // Scale lock works per axis pair; run it for the two largest changes.
  int axes[3] = {0, 1, 2};
  std::sort(std::begin(axes), std::end(axes), [&factor](int a, int b) {
    return std::fabs(factor[a] - 1.0f) > std::fabs(factor[b] - 1.0f);
  });
  glm::vec3 anchor = newMin - factor * mn;
  applyTextureScaleLock(ids, baseUv, axes[0], factor[axes[0]], axes[1], factor[axes[1]], anchor);
  recordTransformEdit(QStringLiteral("Resize"), ids, basePos, baseScale, baseUv);
  emit sceneChanged();
  emit objectStateChanged();
}

void Engine3D::duplicateSelection() {
  if (!m_scene || (m_selectedObjects.empty() && m_selectedMods.empty() && m_selectedEntities.empty()))
    return;
  // Selected modified objects duplicate through their sources.
  std::vector<quint32> ids = moveTargets();
  if (ids.empty() && (!m_selectedObjects.empty() || !m_selectedMods.empty()))
    if (const QString why = modMoveBlocked(); !why.isEmpty())
      emit toastRequested(why);
  std::map<quint32, glm::vec3> entBase = selectedEntityOrigins();
  std::vector<glm::vec3> basePos;
  std::vector<DragFaceUv> baseUv;
  for (const quint32 id: ids) {
    const auto o = m_scene->findById(id);
    basePos.push_back(o ? o->getPosition() : glm::vec3(0.0f));
  }
  captureFaceUv(ids, baseUv);
  DuplicateSet set;
  if (!ids.empty())
    spawnDuplicates(ids, basePos, baseUv, set);
  spawnEntityDuplicates(entBase, set);
  if (!set.triggered)
    return;
  // Offset the copies one grid step along X and Z.
  const float step = float(gizmoSnapStep());
  const glm::vec3 offset(step, 0.0f, step);
  if (!set.clones.empty()) {
    for (size_t k = 0; k < ids.size(); ++k)
      if (const auto o = m_scene->findById(ids[k]))
        o->setPosition(basePos[k] + offset);
    applyTextureLockMove(ids, baseUv, offset);
  }
  if (!set.entityClones.empty())
    moveSelectedEntities(entBase, offset);
  pushDuplicateUndoMirrored(set, QStringLiteral("Duplicate"));
  emit sceneChanged();
}

namespace {

glm::vec3 faceNormalOf(const eng::Mesh &mesh, int face) {
  glm::vec3 n;
  float d;
  return mesh.getFacePlane(face, n, d) ? n : glm::vec3(0.0f, 1.0f, 0.0f);
}

// The default (paraxial, oriented) axes, as new brushes get.
void paraxialAxes(const glm::vec3 &n, glm::vec3 &u, glm::vec3 &v) { eng::defaultTexAxes(glm::dvec3(n), u, v); }

} // namespace

std::vector<std::pair<quint32, int>> Engine3D::selectedFaceRefs() const {
  std::vector<std::pair<quint32, int>> out;
  if (!m_scene)
    return out;
  if (!m_selectedFaces.empty()) {
    for (const quint32 pid: m_selectedFaces) {
      const quint32 sid = eng::stableIdFromPicking(pid);
      const auto o = m_scene->findById(sid);
      if (!o)
        continue;
      const int f = faceIndexOfPicking(o->getMesh(), pid);
      if (f >= 0)
        out.emplace_back(sid, f);
    }
  } else {
    for (const quint32 sid: m_selectedObjects) {
      const auto o = m_scene->findById(sid);
      if (!o || !o->getMesh())
        continue;
      for (int f = 0; f < o->getMesh()->faceCount(); ++f)
        out.emplace_back(sid, f);
    }
  }
  return out;
}

void Engine3D::editSelectedFaces(const QString &label,
                                 const std::function<void(eng::Mesh &, int, eng::FaceTexBasis &)> &edit) {
  const auto refs = selectedFaceRefs();
  if (refs.empty())
    return;
  std::set<quint32> owners;
  for (const auto &r: refs)
    owners.insert(r.first);
  std::vector<ObjectState> before, after;
  for (const quint32 id: owners)
    before.push_back(captureObjectState(id, true));
  for (const auto &[sid, f]: refs) {
    const auto o = m_scene->findById(sid);
    if (!o || !o->getMesh())
      continue;
    auto &mesh = *o->getMesh();
    eng::FaceTexBasis b = mesh.faceTexBasis(f);
    edit(mesh, f, b);
    mesh.setFaceTexBasis(f, b);
  }
  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, std::move(before), std::move(after)));
  emit uvTransformChanged();
  emit objectStateChanged();
}

void Engine3D::rotateFaceTextures(const double degrees) {
  editSelectedFacesWorld(QStringLiteral("Rotate texture"),
                         [degrees](const std::vector<glm::vec3> &, const glm::vec3 &n, eng::FaceTexBasis &b) {
                           const float r = glm::radians(float(degrees));
                           b.uAxis = glm::normalize(glm::rotate(b.uAxis, r, n));
                           b.vAxis = glm::normalize(glm::rotate(b.vAxis, r, n));
                           b.valid = true;
                         });
}

void Engine3D::flipFaceTextures(const bool horizontal) {
  editSelectedFaces(QStringLiteral("Flip texture"), [horizontal](eng::Mesh &, int, eng::FaceTexBasis &b) {
    if (horizontal) {
      b.uAxis = -b.uAxis;
      b.offsetU = -b.offsetU;
    } else {
      b.vAxis = -b.vAxis;
      b.offsetV = -b.offsetV;
    }
  });
}

void Engine3D::alignFaceTextures(const int mode) {
  // 0 = world (paraxial), 1 = face (axes in the face plane).
  editSelectedFacesWorld(mode == 0 ? QStringLiteral("Align texture to world") : QStringLiteral("Align texture to face"),
                    [mode](const std::vector<glm::vec3> &, const glm::vec3 &n, eng::FaceTexBasis &b) {
                      glm::vec3 u, v;
                      paraxialAxes(n, u, v);
                      if (mode == 1) {
                        // Project the paraxial axes into the plane.
                        u = u - n * glm::dot(u, n);
                        if (glm::length(u) < 1e-4f)
                          u = glm::cross(n, glm::vec3(0, 1, 0));
                        u = glm::normalize(u);
                        // Same handedness as defaultTexAxes: cross(u, v) == -n.
                        v = glm::normalize(glm::cross(u, n));
                      }
                      b.uAxis = u;
                      b.vAxis = v;
                      b.valid = true;
                    });
}

void Engine3D::fitFaceTextures(const int repeatU, const int repeatV) {
  // World-space polygon (the basis maps world points); fitBasisToFace turns U onto a face edge first.
  editSelectedFacesWorld(QStringLiteral("Fit texture"), [repeatU, repeatV](const std::vector<glm::vec3> &q,
                                                                           const glm::vec3 &n, eng::FaceTexBasis &b) {
    eng::fitBasisToFace(b, q, n, repeatU, repeatV);
  });
}

void Engine3D::shiftFaceTextures(const double du, const double dv) {
  editSelectedFaces(QStringLiteral("Shift texture"), [du, dv](eng::Mesh &, int, eng::FaceTexBasis &b) {
    b.offsetU = float(std::fmod(b.offsetU + du, 1.0));
    b.offsetV = float(std::fmod(b.offsetV + dv, 1.0));
  });
}

void Engine3D::scaleFaceTextures(const double fu, const double fv) {
  editSelectedFaces(QStringLiteral("Scale texture"), [fu, fv](eng::Mesh &, int, eng::FaceTexBasis &b) {
    if (fu > 0.0)
      b.tilesPerUnitU = float(b.tilesPerUnitU / fu);
    if (fv > 0.0)
      b.tilesPerUnitV = float(b.tilesPerUnitV / fv);
  });
}

void Engine3D::setFaceTextureBasis(const double tileSourceU, const double tileSourceV, const double offU,
                                   const double offV, const double rotationDeg, const int which) {
  // `which`: bitmask of fields to write (1 scaleU, 2 scaleV, 4 offU, 8 offV, 16 rotation).
  editSelectedFaces(QStringLiteral("Edit texture"), [=](eng::Mesh &mesh, int f, eng::FaceTexBasis &b) {
    if ((which & 1) && tileSourceU > 1e-6)
      b.tilesPerUnitU = float(kSourceUnitsPerWorldUnit / tileSourceU);
    if ((which & 2) && tileSourceV > 1e-6)
      b.tilesPerUnitV = float(kSourceUnitsPerWorldUnit / tileSourceV);
    const auto mat = mesh.faceMaterialRef(f);
    const double tw = (which & kOffsetsInTexels) ? double(mat && mat->texWidth > 0 ? mat->texWidth : 512) : 1.0;
    const double th = (which & kOffsetsInTexels) ? double(mat && mat->texHeight > 0 ? mat->texHeight : tw) : 1.0;
    if (which & 4)
      b.offsetU = float(offU / tw);
    if (which & 8)
      b.offsetV = float(offV / th);
    if (which & 16) {
      const glm::vec3 n = faceNormalOf(mesh, f);
      glm::vec3 u, v;
      paraxialAxes(n, u, v);
      const float cur = faceTextureRotationOf(n, b);
      const float r = glm::radians(float(rotationDeg) - cur);
      b.uAxis = glm::normalize(glm::rotate(b.uAxis, r, n));
      b.vAxis = glm::normalize(glm::rotate(b.vAxis, r, n));
    }
    b.valid = true;
  });
}

namespace {
// A face's default tile size (Source units) from its own texture.
void defaultTileOf(const eng::MaterialPtr &mat, double &u, double &v) {
  const double scale = prefs::d("Brush/DefaultTextureScale");
  const double w = mat && mat->texWidth > 0 ? mat->texWidth : 512.0;
  const double h = mat && mat->texHeight > 0 ? mat->texHeight : w;
  u = w * scale;
  v = h * scale;
}
} // namespace

void Engine3D::resetFaceTextureFields(const int which) {
  const bool all = (which & 31) == 31;
  editSelectedFaces(all ? QStringLiteral("Reset texture") : QStringLiteral("Reset texture field"),
                    [which, all](eng::Mesh &mesh, int f, eng::FaceTexBasis &b) {
                      double tu, tv;
                      defaultTileOf(mesh.faceMaterialRef(f), tu, tv);
                      const glm::vec3 n = faceNormalOf(mesh, f);
                      if (all) {
                        paraxialAxes(n, b.uAxis, b.vAxis);
                      } else if (which & 16) {
                        const float r = glm::radians(-faceTextureRotationOf(n, b));
                        b.uAxis = glm::normalize(glm::rotate(b.uAxis, r, n));
                        b.vAxis = glm::normalize(glm::rotate(b.vAxis, r, n));
                      }
                      if (which & 1)
                        b.tilesPerUnitU = float(kSourceUnitsPerWorldUnit / tu);
                      if (which & 2)
                        b.tilesPerUnitV = float(kSourceUnitsPerWorldUnit / tv);
                      if (which & 4)
                        b.offsetU = 0.0f;
                      if (which & 8)
                        b.offsetV = 0.0f;
                      b.valid = true;
                    });
}

float Engine3D::faceTextureRotationOf(const glm::vec3 &n, const eng::FaceTexBasis &b) {
  glm::vec3 u, v;
  paraxialAxes(n, u, v);
  // Angle of the current U axis from the paraxial U, around the normal.
  const glm::vec3 uProj = glm::normalize(b.uAxis - n * glm::dot(b.uAxis, n) + glm::vec3(1e-9f));
  const glm::vec3 refU = glm::normalize(u - n * glm::dot(u, n) + glm::vec3(1e-9f));
  float ang = std::atan2(glm::dot(glm::cross(refU, uProj), n), glm::dot(refU, uProj));
  float deg = glm::degrees(ang);
  if (std::fabs(deg) < 0.01f)
    deg = 0.0f;
  return deg;
}

Engine3D::FaceSummary Engine3D::faceSummary() const {
  FaceSummary s;
  const auto refs = selectedFaceRefs();
  s.count = int(refs.size());
  bool first = true;
  for (const auto &[sid, f]: refs) {
    const auto o = m_scene->findById(sid);
    if (!o || !o->getMesh())
      continue;
    const auto &mesh = *o->getMesh();
    const eng::FaceTexBasis &b = mesh.faceTexBasis(f);
    const auto mat = mesh.faceMaterialRef(f);
    const QString name = mat ? mat->name : QString();
    const double su = b.tilesPerUnitU > 1e-9f ? kSourceUnitsPerWorldUnit / b.tilesPerUnitU : 0.0;
    const double sv = b.tilesPerUnitV > 1e-9f ? kSourceUnitsPerWorldUnit / b.tilesPerUnitV : 0.0;
    const double rot = faceTextureRotationOf(faceNormalOf(mesh, f), b);
    const double tw = mat && mat->texWidth > 0 ? mat->texWidth : 512.0;
    const double th = mat && mat->texHeight > 0 ? mat->texHeight : tw;
    auto wrapTex = [](double tiles, double size) {
      double t = tiles - std::floor(tiles);
      if (t > 1.0 - 1e-6)
        t = 0.0;
      return t * size;
    };
    const double otu = wrapTex(b.offsetU, tw), otv = wrapTex(b.offsetV, th);
    double du, dv;
    defaultTileOf(mat, du, dv);
    s.changedScaleU |= std::fabs(su - du) > 1e-3;
    s.changedScaleV |= std::fabs(sv - dv) > 1e-3;
    s.changedOffsetU |= otu > 1e-3;
    s.changedOffsetV |= otv > 1e-3;
    s.changedRotation |= std::fabs(rot) > 1e-2;
    if (first) {
      s.offsetTexU = otu;
      s.offsetTexV = otv;
      s.material = name;
      s.tileSourceU = su;
      s.tileSourceV = sv;
      s.offsetU = b.offsetU;
      s.offsetV = b.offsetV;
      s.rotation = rot;
      s.texWidth = mat ? mat->texWidth : 0;
      s.texHeight = mat ? mat->texHeight : 0;
      first = false;
      continue;
    }
    s.mixedMaterial |= name != s.material;
    s.mixedScaleU |= std::fabs(su - s.tileSourceU) > 1e-3;
    s.mixedScaleV |= std::fabs(sv - s.tileSourceV) > 1e-3;
    s.mixedOffsetU |= std::fabs(b.offsetU - s.offsetU) > 1e-4;
    s.mixedOffsetV |= std::fabs(b.offsetV - s.offsetV) > 1e-4;
    s.mixedRotation |= std::fabs(rot - s.rotation) > 1e-2;
    s.mixedOffsetTexU |= std::fabs(otu - s.offsetTexU) > 1e-3;
    s.mixedOffsetTexV |= std::fabs(otv - s.offsetTexV) > 1e-3;
  }
  return s;
}

void Engine3D::applyMaterialToSelection(const QString &materialName) {
  const auto refs = selectedFaceRefs();
  if (refs.empty() || materialName.isEmpty())
    return;
  const auto material = TextureManager::instance().materialFor(materialName);
  if (!material)
    return;
  std::set<quint32> owners;
  for (const auto &r: refs)
    owners.insert(r.first);
  std::vector<ObjectState> before, after;
  for (const quint32 id: owners)
    before.push_back(captureObjectState(id, true));
  for (const auto &[sid, f]: refs)
    if (const auto o = m_scene->findById(sid); o && o->getMesh())
      o->getMesh()->setFaceMaterial(f, material);
  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, QStringLiteral("Apply material"),
                                                            std::move(before), std::move(after)));
  m_currentMaterial = materialName;
  emit uvTransformChanged();
  emit objectStateChanged();
  emit sceneChanged();
}

QString Engine3D::pickMaterialFromSelection() const {
  const auto s = faceSummary();
  return s.material;
}

QStringList Engine3D::materialsInUse() const {
  QSet<QString> names;
  if (m_scene)
    for (const auto &o: m_scene->gameObjects)
      if (const auto mesh = o->getMesh())
        for (int f = 0; f < mesh->faceCount(); ++f)
          if (const auto m = mesh->faceMaterialRef(f))
            names.insert(m->name);
  QStringList out(names.begin(), names.end());
  out.sort();
  return out;
}

} // namespace vk

namespace vk {

void Engine3D::beginOrbit(const float px, const float py) {
  glm::vec3 mn, mx;
  if (prefs::b("Viewport/OrbitAroundSelection") && selectionBounds(mn, mx)) {
    m_orbitPivot = (mn + mx) * 0.5f;
  } else {
    // Whatever is under the cursor, else a point ahead of the camera.
    glm::vec3 ro, rd;
    makeRay(px, py, std::max(1.0f, float(m_lastViewSize.width())), std::max(1.0f, float(m_lastViewSize.height())),
            ro, rd);
    float t;
    glm::vec3 n;
    m_orbitPivot = raycastScene(ro, rd, t, n) ? ro + rd * t : ro + rd * 30.0f;
  }
}

void Engine3D::orbit(const float dx, const float dy) {
  const glm::vec3 cam = m_camera->getPosition();
  const float dist = std::max(0.5f, glm::length(cam - m_orbitPivot));
  const float sens = float(prefs::d("Viewport/MouseSensitivity")) * 2.5f;
  float yaw = m_camera->getYaw() - dx * sens;
  float pitch = std::clamp(m_camera->getPitch() + (prefs::b("Viewport/InvertY") ? dy : -dy) * sens, -89.0f, 89.0f);
  const float yr = glm::radians(yaw), pr = glm::radians(pitch);
  const glm::vec3 dir(std::cos(yr) * std::cos(pr), std::sin(pr), -std::sin(yr) * std::cos(pr));
  m_camera->setPose(m_orbitPivot - dir * dist, yaw, pitch);
  requestRedraw();
}

void Engine3D::pan(const float dx, const float dy) {
  const glm::vec3 cam = m_camera->getPosition();
  const float dist = std::max(1.0f, glm::length(cam - m_orbitPivot));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  const float perPixel = 2.0f * dist * std::tan(glm::radians(m_camera->getFov()) * 0.5f) / viewH;
  const glm::vec3 move = (-m_camera->getRight() * dx + m_camera->getUp() * dy) * perPixel;
  m_camera->setPose(cam + move, m_camera->getYaw(), m_camera->getPitch());
  m_orbitPivot += move;
  requestRedraw();
}

void Engine3D::dolly(const float steps) {
  // Alt+wheel: move towards the orbit pivot by a fraction of the distance.
  const glm::vec3 cam = m_camera->getPosition();
  const glm::vec3 to = m_orbitPivot - cam;
  const float dist = glm::length(to);
  if (dist < 1e-3f)
    return;
  const float f = std::clamp(steps * 0.12f, -0.9f, 0.9f);
  m_camera->setPose(cam + to * f, m_camera->getYaw(), m_camera->getPitch());
  requestRedraw();
}

} // namespace vk


namespace vk {

void Engine3D::lookAlongAxis(const int axis, const int sign) {
  // Keep the point in front of the camera (the orbit pivot or 30 units ahead).
  glm::vec3 mn, mx;
  const glm::vec3 target = selectionBounds(mn, mx) ? (mn + mx) * 0.5f
                                                    : m_camera->getPosition() + m_camera->getDirection() * 30.0f;
  const float dist = std::max(5.0f, glm::length(target - m_camera->getPosition()));
  // Direction the camera should look along.
  glm::vec3 dir(0.0f);
  dir[std::clamp(axis, 0, 2)] = sign < 0 ? -1.0f : 1.0f;
  // getDirection: (cos y cos p, sin p, -sin y cos p)
  const float pitch = glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f))) * 0.999f;
  float yaw = m_camera->getYaw();
  if (std::fabs(dir.y) < 0.5f)
    yaw = glm::degrees(std::atan2(-dir.z, dir.x));
  const float yr = glm::radians(yaw), pr = glm::radians(pitch);
  const glm::vec3 d(std::cos(yr) * std::cos(pr), std::sin(pr), -std::sin(yr) * std::cos(pr));
  m_camera->setPose(target - d * dist, yaw, pitch);
  m_orbitPivot = target;
  requestRedraw();
}

} // namespace vk
