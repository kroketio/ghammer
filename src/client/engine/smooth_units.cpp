#include "smooth_units.h"

#include <map>

#include "gameobject.h"
#include "mesh.h"
#include "scene.h"

namespace eng {

quint64 smoothUnitKey(const GameObject &o) {
  if (o.procObject())
    return (quint64(1) << 32) | o.procObject();
  if (o.brushEntity())
    return (quint64(2) << 32) | o.brushEntity();
  if (o.group() != kWorldspawnGroup)
    return (quint64(3) << 32) | o.group();
  return 0;
}

std::vector<std::vector<std::vector<glm::vec3>>>
smoothSolidsTogether(const std::vector<std::vector<std::vector<glm::dvec3>>> &polys,
                     const std::vector<std::vector<glm::dvec3>> &normals) {
  std::vector<std::vector<glm::dvec3>> allPolys;
  std::vector<glm::dvec3> allNormals;
  for (size_t s = 0; s < polys.size(); ++s)
    for (size_t f = 0; f < polys[s].size(); ++f) {
      allPolys.push_back(polys[s][f]);
      allNormals.push_back(f < normals[s].size() ? normals[s][f] : glm::dvec3(0.0, 1.0, 0.0));
    }
  const auto all = smoothPolygonNormals(allPolys, allNormals);
  std::vector<std::vector<std::vector<glm::vec3>>> out(polys.size());
  size_t i = 0;
  for (size_t s = 0; s < polys.size(); ++s)
    for (size_t f = 0; f < polys[s].size(); ++f)
      out[s].push_back(all[i++]);
  return out;
}

namespace {

quint64 mix(quint64 h, quint64 v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

quint64 signatureOf(const std::vector<GameObject *> &unit) {
  quint64 h = unit.size();
  for (const GameObject *o: unit)
    h = mix(mix(mix(h, o->stableId()), o->getMesh()->geometryVersion()), o->placementVersion());
  return h;
}

void smoothUnit(const std::vector<GameObject *> &unit) {
  std::vector<std::vector<std::vector<glm::dvec3>>> polys(unit.size());
  std::vector<std::vector<glm::dvec3>> normals(unit.size());
  std::vector<glm::vec3> local;
  for (size_t s = 0; s < unit.size(); ++s) {
    const Mesh &m = *unit[s]->getMesh();
    const glm::dmat4 model(unit[s]->getModelMatrix());
    const glm::dmat3 normalMat = glm::transpose(glm::inverse(glm::dmat3(model)));
    polys[s].resize(size_t(m.faceCount()));
    normals[s].assign(size_t(m.faceCount()), glm::dvec3(0.0, 1.0, 0.0));
    for (int f = 0; f < m.faceCount(); ++f) {
      if (m.getFacePolygon(f, local))
        for (const glm::vec3 &p: local)
          polys[s][size_t(f)].push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(p), 1.0)));
      glm::vec3 n;
      float d = 0.0f;
      if (m.getFacePlane(f, n, d))
        normals[s][size_t(f)] = glm::normalize(normalMat * glm::dvec3(n));
    }
  }
  const auto world = smoothSolidsTogether(polys, normals);
  for (size_t s = 0; s < unit.size(); ++s) {
    // Back to mesh space: the inverse of the normal matrix is the model's transpose.
    const glm::dmat3 toLocal = glm::transpose(glm::dmat3(glm::dmat4(unit[s]->getModelMatrix())));
    std::vector<std::vector<glm::vec3>> corners(world[s].size());
    for (size_t f = 0; f < world[s].size(); ++f)
      for (const glm::vec3 &n: world[s][f])
        corners[f].push_back(glm::vec3(glm::normalize(toLocal * glm::dvec3(n))));
    unit[s]->getMesh()->setCornerNormals(corners);
  }
}

} // namespace

void refreshSmoothUnits(Scene &scene, QHash<quint64, quint64> &signatures) {
  std::map<quint64, std::vector<GameObject *>> units;
  for (const auto &o: scene.gameObjects) {
    if (!o || o->isDerived() || !o->smoothShading() || !o->getMesh())
      continue;
    if (const quint64 key = smoothUnitKey(*o))
      units[key].push_back(o.get());
  }
  QHash<quint64, quint64> next;
  for (auto &[key, unit]: units) {
    const quint64 sig = signatureOf(unit);
    if (signatures.value(key) != sig) {
      if (unit.size() == 1)
        unit.front()->getMesh()->setSmoothShading(true);
      else
        smoothUnit(unit);
    }
    next.insert(key, signatureOf(unit));
  }
  signatures = std::move(next);
}

} // namespace eng
