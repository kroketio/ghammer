// Texture tools on world-space faces: justify, align to edge, reset, copy / paste / wrap, the UV editor's session.

#include <cfloat>
#include <cmath>

#include <glm/gtx/rotate_vector.hpp>

#include "vk_engine.h"
#include "vk_shared.h"

#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

// The face polygon in world space and its outward normal.
bool worldFace(const eng::GameObject &obj, int f, std::vector<glm::vec3> &poly, glm::vec3 &n) {
  std::vector<glm::vec3> local;
  const auto mesh = obj.getMesh();
  if (!mesh || !mesh->getFacePolygon(f, local) || local.size() < 3)
    return false;
  const glm::mat4 m = obj.getModelMatrix();
  poly.clear();
  for (const auto &p: local)
    poly.push_back(glm::vec3(m * glm::vec4(p, 1.0f)));
  glm::vec3 nn(0.0f);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::vec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
    nn += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
  }
  if (glm::length(nn) < 1e-9f)
    return false;
  n = glm::normalize(nn);
  // Mesh windings may face in or out: orient away from the object's centre.
  const auto [lo, hi] = obj.getWorldAABB();
  glm::vec3 c(0.0f);
  for (const auto &p: poly)
    c += p;
  c /= float(poly.size());
  if (glm::dot(n, c - (lo + hi) * 0.5f) < 0.0f)
    n = -n;
  return true;
}

namespace {

glm::vec3 centroid(const std::vector<glm::vec3> &poly) {
  glm::vec3 c(0.0f);
  for (const auto &p: poly)
    c += p;
  return poly.empty() ? c : c / float(poly.size());
}

float wrap01(float v) { return v - std::floor(v); }

} // namespace

void Engine3D::editSelectedFacesWorld(const QString &label,
                                      const std::function<void(const std::vector<glm::vec3> &, const glm::vec3 &,
                                                               eng::FaceTexBasis &)> &edit) {
  editSelectedFaces(label, [this, &edit](eng::Mesh &mesh, int f, eng::FaceTexBasis &b) {
    // Find the object owning this mesh (refs are by object, the callback gets the mesh).
    for (const auto &[sid, face]: selectedFaceRefs()) {
      const auto o = m_scene->findById(sid);
      if (!o || o->getMesh().get() != &mesh || face != f)
        continue;
      std::vector<glm::vec3> poly;
      glm::vec3 n;
      if (worldFace(*o, f, poly, n))
        edit(poly, n, b);
      return;
    }
  });
}

void Engine3D::justifyFaceTextures(const int mode) {
  static const char *names[] = {"left", "right", "top", "bottom", "centre"};
  editSelectedFacesWorld(QStringLiteral("Justify texture %1").arg(QLatin1String(names[std::clamp(mode, 0, 4)])),
                         [mode](const std::vector<glm::vec3> &poly, const glm::vec3 &, eng::FaceTexBasis &b) {
                           float u0 = FLT_MAX, u1 = -FLT_MAX, v0 = FLT_MAX, v1 = -FLT_MAX;
                           for (const auto &p: poly) {
                             u0 = std::min(u0, glm::dot(p, b.uAxis) * b.tilesPerUnitU);
                             u1 = std::max(u1, glm::dot(p, b.uAxis) * b.tilesPerUnitU);
                             v0 = std::min(v0, glm::dot(p, b.vAxis) * b.tilesPerUnitV);
                             v1 = std::max(v1, glm::dot(p, b.vAxis) * b.tilesPerUnitV);
                           }
                           switch (mode) {
                             case 0: b.offsetU = wrap01(-u0); break;
                             case 1: b.offsetU = wrap01(-u1); break;
                             case 2: b.offsetV = wrap01(-v0); break;
                             case 3: b.offsetV = wrap01(-v1); break;
                             default:
                               b.offsetU = wrap01(0.5f - 0.5f * (u0 + u1));
                               b.offsetV = wrap01(0.5f - 0.5f * (v0 + v1));
                               break;
                           }
                           b.valid = true;
                         });
}

void Engine3D::alignFaceTexturesToEdge() {
  const int k = m_alignEdgeCycle++;
  editSelectedFacesWorld(QStringLiteral("Align texture to edge"),
                         [k](const std::vector<glm::vec3> &poly, const glm::vec3 &n, eng::FaceTexBasis &b) {
                           const size_t i = size_t(k) % poly.size();
                           glm::vec3 e = poly[(i + 1) % poly.size()] - poly[i];
                           e -= n * glm::dot(e, n);
                           if (glm::length(e) < 1e-6f)
                             return;
                           e = glm::normalize(e);
                           // Keep the texture's handedness: V stays on the same side of U.
                           const float hand = glm::dot(glm::cross(b.uAxis, b.vAxis), n) >= 0.0f ? 1.0f : -1.0f;
                           b.uAxis = e;
                           b.vAxis = glm::normalize(glm::cross(n, e)) * hand;
                           // Start the texture on that edge.
                           b.offsetU = wrap01(-glm::dot(poly[i], b.uAxis) * b.tilesPerUnitU);
                           b.offsetV = wrap01(-glm::dot(poly[i], b.vAxis) * b.tilesPerUnitV);
                           b.valid = true;
                         });
}

void Engine3D::resetFaceTextures() {
  // Every field back to the face's own default (its texture's size, not the
  // current material's), with the same paraxial axes as Align to World.
  resetFaceTextureFields(31);
}

void Engine3D::shiftFaceTexturesSource(const double du, const double dv) {
  const float k = 1.0f / kSourceUnitsPerWorldUnit;
  editSelectedFaces(QStringLiteral("Shift texture"), [du, dv, k](eng::Mesh &, int, eng::FaceTexBasis &b) {
    b.offsetU = wrap01(b.offsetU + float(du) * k * b.tilesPerUnitU);
    b.offsetV = wrap01(b.offsetV + float(dv) * k * b.tilesPerUnitV);
  });
}

bool Engine3D::copyFaceAttributes() {
  const auto refs = selectedFaceRefs();
  if (refs.empty() || !m_scene)
    return false;
  const auto o = m_scene->findById(refs.front().first);
  if (!o)
    return false;
  std::vector<glm::vec3> poly;
  glm::vec3 n;
  if (!worldFace(*o, refs.front().second, poly, n))
    return false;
  const auto mat = o->getMesh()->faceMaterialRef(refs.front().second);
  m_faceClip.valid = true;
  m_faceClip.material = mat ? mat->name : QString();
  m_faceClip.basis = o->getMesh()->faceTexBasis(refs.front().second);
  m_faceClip.normal = n;
  m_faceClip.point = centroid(poly);
  emit uvTransformChanged();
  return true;
}

namespace {
// The clipboard's mapping moved onto another face. Projection keeps the axes;
// wrap rotates them about the line the two planes share and keeps the UV of
// that line, so a texture folds around a corner without a seam.
eng::FaceTexBasis transferBasis(const Engine3D::FaceClip &clip, const std::vector<glm::vec3> &poly, const glm::vec3 &n,
                                bool wrap) {
  eng::FaceTexBasis b = clip.basis;
  b.valid = true;
  if (!wrap)
    return b;
  const glm::vec3 ns = glm::normalize(clip.normal);
  const glm::vec3 axis = glm::cross(ns, n);
  const float s = glm::length(axis);
  if (s < 1e-5f)
    return b; // parallel faces: projection is already seamless
  const float ang = std::atan2(s, glm::dot(ns, n));
  const glm::vec3 a = axis / s;
  // A point on both planes: the shared edge's line.
  const float ds = glm::dot(ns, clip.point), dt = glm::dot(n, centroid(poly));
  const glm::vec3 L = (glm::cross(n, axis) * ds + glm::cross(axis, ns) * dt) / (s * s);
  const glm::vec2 before = clip.basis.uvAt(L);
  b.uAxis = glm::normalize(glm::rotate(clip.basis.uAxis, ang, a));
  b.vAxis = glm::normalize(glm::rotate(clip.basis.vAxis, ang, a));
  b.offsetU = 0.0f;
  b.offsetV = 0.0f;
  const glm::vec2 after = b.uvAt(L);
  b.offsetU = wrap01(before.x - after.x);
  b.offsetV = wrap01(before.y - after.y);
  return b;
}
} // namespace

void Engine3D::pasteFaceAttributes(const int mode) {
  if (!m_faceClip.valid)
    return;
  const auto mat = m_faceClip.material.isEmpty() ? eng::MaterialPtr()
                                                 : TextureManager::instance().materialFor(m_faceClip.material);
  const FaceClip clip = m_faceClip;
  const auto refs = selectedFaceRefs();
  static const char *labels[] = {"Paste material", "Paste texture", "Wrap texture"};
  // The material and the mapping change in one step (the undo snapshot holds both).
  editSelectedFaces(QLatin1String(labels[std::clamp(mode, 0, 2)]), [&](eng::Mesh &mesh, int f, eng::FaceTexBasis &b) {
    if (mat)
      mesh.setFaceMaterial(f, mat);
    if (mode < 1)
      return;
    for (const auto &[sid, face]: refs) {
      const auto o = m_scene->findById(sid);
      if (!o || o->getMesh().get() != &mesh || face != f)
        continue;
      std::vector<glm::vec3> poly;
      glm::vec3 n;
      if (worldFace(*o, f, poly, n))
        b = transferBasis(clip, poly, n, mode == 2);
      return;
    }
  });
  invalidateDerivedCaches();
}

quint32 Engine3D::facePickAt(const float x, const float y) const {
  if (!m_scene)
    return 0;
  glm::vec3 origin, dir;
  makeRay(x, y, float(m_lastViewSize.width()), float(m_lastViewSize.height()), origin, dir);
  float bestT = 1e30f;
  quint32 best = 0;
  for (const auto &obj: m_scene->gameObjects) {
    const auto mesh = obj->getMesh();
    if (!mesh || obj->hidden())
      continue;
    const glm::mat4 model = obj->getModelMatrix();
    const auto &indices = mesh->indexData();
    const auto &vertices = mesh->vertexData();
    for (int face = 0; face < mesh->faceCount(); ++face) {
      const quint32 pid = mesh->getFacePickingID(face);
      const size_t base = mesh->faceIndexStart(face), count = mesh->faceIndexCount(face);
      if (!pid || base + count > indices.size())
        continue;
      for (size_t t = 0; t < count / 3; ++t) {
        const glm::vec3 v0 = glm::vec3(model * glm::vec4(vertices[indices[base + t * 3]].position, 1.0f));
        const glm::vec3 v1 = glm::vec3(model * glm::vec4(vertices[indices[base + t * 3 + 1]].position, 1.0f));
        const glm::vec3 v2 = glm::vec3(model * glm::vec4(vertices[indices[base + t * 3 + 2]].position, 1.0f));
        // Moller-Trumbore
        const glm::vec3 e1 = v1 - v0, e2 = v2 - v0, pv = glm::cross(dir, e2);
        const float det = glm::dot(e1, pv);
        if (std::fabs(det) < 1e-9f)
          continue;
        const float inv = 1.0f / det;
        const glm::vec3 tv = origin - v0;
        const float u = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f)
          continue;
        const glm::vec3 qv = glm::cross(tv, e1);
        const float v = glm::dot(dir, qv) * inv;
        if (v < 0.0f || u + v > 1.0f)
          continue;
        const float hit = glm::dot(e2, qv) * inv;
        if (hit > 1e-4f && hit < bestT) {
          bestT = hit;
          best = pid;
        }
      }
    }
  }
  return best;
}

bool Engine3D::pasteFaceAttributesAt(const float x, const float y, const int mode) {
  if (!m_faceClip.valid)
    return false;
  const quint32 pid = facePickAt(x, y);
  if (!pid)
    return false;
  // Paste onto that one face, then put the selection back.
  const auto faces = m_selectedFaces;
  const auto objs = m_selectedObjects;
  m_selectedFaces = {pid};
  m_selectedObjects.clear();
  pasteFaceAttributes(mode);
  m_selectedFaces = faces;
  m_selectedObjects = objs;
  requestRedraw();
  return true;
}

Engine3D::FaceView Engine3D::primaryFace() const {
  FaceView v;
  const auto refs = selectedFaceRefs();
  if (refs.empty() || !m_scene)
    return v;
  for (size_t i = 0; i < refs.size() && i < 64; ++i) {
    const auto o = m_scene->findById(refs[i].first);
    if (!o)
      continue;
    std::vector<glm::vec3> poly;
    glm::vec3 n;
    if (!worldFace(*o, refs[i].second, poly, n))
      continue;
    if (!v.valid) {
      v.valid = true;
      v.object = refs[i].first;
      v.face = refs[i].second;
      v.poly = poly;
      v.normal = n;
      v.basis = o->getMesh()->faceTexBasis(refs[i].second);
      v.material = o->getMesh()->faceMaterialRef(refs[i].second);
    } else {
      v.others.push_back(poly);
    }
  }
  return v;
}

void Engine3D::beginFaceEdit() {
  m_faceEdit = {};
  m_faceEdit.refs = selectedFaceRefs();
  if (m_faceEdit.refs.empty() || !m_scene)
    return;
  std::set<quint32> owners;
  for (const auto &r: m_faceEdit.refs)
    owners.insert(r.first);
  for (const quint32 id: owners)
    m_faceEdit.before.push_back(captureObjectState(id, true));
  for (const auto &[sid, f]: m_faceEdit.refs) {
    const auto o = m_scene->findById(sid);
    m_faceEdit.base.push_back(o ? o->getMesh()->faceTexBasis(f) : eng::FaceTexBasis {});
  }
  m_faceEdit.active = true;
}

void Engine3D::updateFaceEdit(const std::function<void(const std::vector<glm::vec3> &, const glm::vec3 &,
                                                       eng::FaceTexBasis &, eng::FaceTexBasis &)> &edit) {
  if (!m_faceEdit.active || !m_scene)
    return;
  for (size_t i = 0; i < m_faceEdit.refs.size(); ++i) {
    const auto &[sid, f] = m_faceEdit.refs[i];
    const auto o = m_scene->findById(sid);
    if (!o)
      continue;
    std::vector<glm::vec3> poly;
    glm::vec3 n;
    if (!worldFace(*o, f, poly, n))
      continue;
    eng::FaceTexBasis base = m_faceEdit.base[i];
    eng::FaceTexBasis out = base;
    edit(poly, n, base, out);
    out.valid = true;
    o->getMesh()->setFaceTexBasis(f, out);
  }
  emit uvTransformChanged();
  emit objectStateChanged();
  requestRedraw();
}

void Engine3D::endFaceEdit(const QString &label) {
  if (!m_faceEdit.active)
    return;
  m_faceEdit.active = false;
  std::set<quint32> owners;
  for (const auto &r: m_faceEdit.refs)
    owners.insert(r.first);
  std::vector<ObjectState> after;
  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, std::move(m_faceEdit.before), std::move(after)));
  m_faceEdit = {};
  emit uvTransformChanged();
}

} // namespace vk
