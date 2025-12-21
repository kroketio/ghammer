// Face tools: extrude (move a face plane) and shear.

#include <glm/gtc/matrix_transform.hpp>

#include "vk_engine.h"

#include "client/engine/brush.h"
#include "client/engine/scene.h"

namespace vk {

std::map<quint32, std::vector<int>> Engine3D::selectedBrushFaces() const {
  std::map<quint32, std::vector<int>> out;
  if (!m_scene)
    return out;
  // Material mode: picked faces, matched to brush faces by world plane.
  for (const quint32 pid: m_selectedFaces) {
    const quint32 oid = eng::stableIdFromPicking(pid);
    const int f = int(eng::faceFromPicking(pid));
    const auto obj = m_scene->findById(oid);
    if (!obj || obj->isDerived())
      continue;
    const auto b = eng::ConvexBrush::fromObject(*obj);
    std::vector<glm::vec3> local;
    if (!b || !obj->getMesh()->getFacePolygon(f, local) || local.size() < 3)
      continue;
    const glm::dmat4 model(obj->getModelMatrix());
    glm::dvec3 c(0.0);
    std::vector<glm::dvec3> poly;
    for (const auto &p: local) {
      poly.push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(p), 1.0)));
      c += poly.back();
    }
    c /= double(poly.size());
    int best = -1;
    double bestD = 1e-2;
    for (size_t i = 0; i < b->faces.size(); ++i) {
      const double d = std::fabs(b->faces[i].signedDistance(c));
      bool inPoly = true;
      for (const auto &q: poly)
        inPoly = inPoly && std::fabs(b->faces[i].signedDistance(q)) < 1e-2;
      if (inPoly && d < bestD) {
        bestD = d;
        best = int(i);
      }
    }
    if (best >= 0 && std::find(out[oid].begin(), out[oid].end(), best) == out[oid].end())
      out[oid].push_back(best);
  }
  // Vertex tool: faces whose corners are all selected.
  if (m_editMode && !m_selectedVerts.empty()) {
    for (const quint32 id: m_selectedObjects) {
      const auto obj = m_scene->findById(id);
      const auto b = obj ? eng::ConvexBrush::fromObject(*obj) : std::nullopt;
      if (!b)
        continue;
      for (size_t i = 0; i < b->polygons.size(); ++i) {
        bool all = !b->polygons[i].empty();
        for (const auto &q: b->polygons[i])
          all = all && vertexSelected(id, glm::vec3(q));
        if (all && std::find(out[id].begin(), out[id].end(), int(i)) == out[id].end())
          out[id].push_back(int(i));
      }
    }
  }
  for (auto it = out.begin(); it != out.end();)
    it = it->second.empty() ? out.erase(it) : std::next(it);
  return out;
}

int Engine3D::extrudeSelectedFaces(const double distance, const bool newBrush) {
  if (!m_scene || std::fabs(distance) < 1e-9)
    return 0;
  const auto faces = selectedBrushFaces();
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  std::vector<quint32> keepFaces;
  for (const auto &[id, list]: faces) {
    const auto obj = m_scene->findById(id);
    auto b = obj ? eng::ConvexBrush::fromObject(*obj) : std::nullopt;
    if (!b || obj->locked())
      continue;
    if (newBrush) {
      // A new brush per face: the volume it sweeps through.
      for (const int fi: list) {
        const eng::BrushFace face = b->faces[size_t(fi)];
        eng::ConvexBrush grown = *b;
        grown.faces[size_t(fi)].distance += std::fabs(distance);
        eng::BrushFace cap = face;
        cap.normal = -face.normal;
        cap.distance = -face.distance;
        if (!grown.build() || !grown.clip(cap) || !grown.valid())
          continue;
        if (auto o = objectFromBrush(grown, obj.get()))
          add.push_back(o);
      }
      continue;
    }
    eng::ConvexBrush moved = *b;
    for (const int fi: list)
      moved.faces[size_t(fi)].distance += distance;
    if (!moved.build() || !moved.valid())
      continue; // pushed through the brush: leave it alone
    if (auto o = objectFromBrush(moved, obj.get())) {
      remove.push_back(id);
      add.push_back(o);
      // Brush face order is mesh face order: the same indices pick the moved faces.
      for (const int fi: list)
        if (fi < o->getMesh()->faceCount())
          keepFaces.push_back(eng::makePickingId(o->stableId(), uint32_t(fi)));
    }
  }
  if (add.empty())
    return 0;
  const bool faceMode = !m_selectedFaces.empty();
  const QString label = newBrush ? QStringLiteral("Extrude to new brush") : QStringLiteral("Extrude faces");
  if (newBrush) {
    // New brushes from unlinked faces get a mirror copy (linked ones are synced already).
    withMirror(label, [&] {
      replaceObjects(remove, add, label);
      return true;
    });
  } else {
    replaceObjects(remove, add, label);
  }
  // Re-pick the same faces on the new objects so the next extrude continues.
  if (faceMode && !newBrush)
    setSelected({}, keepFaces);
  if (m_editMode)
    remapSelectedVertices();
  return int(add.size());
}

bool Engine3D::shearSelection(const int axis, const int byAxis, const double factor) {
  if (axis < 0 || axis > 2 || byAxis < 0 || byAxis > 2 || axis == byAxis)
    return false;
  glm::vec3 mn, mx;
  if (!selectionBounds(mn, mx))
    return false;
  glm::dmat4 shear(1.0);
  shear[byAxis][axis] = factor; // column byAxis, row axis: p[axis] += factor * p[byAxis]
  const glm::dvec3 pivot(mn);
  const glm::dmat4 m = glm::translate(glm::dmat4(1.0), pivot) * shear * glm::translate(glm::dmat4(1.0), -pivot);
  return transformSelectionBrushes(m, QStringLiteral("Shear"));
}

} // namespace vk
