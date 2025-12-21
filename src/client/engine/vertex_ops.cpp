#include "vertex_ops.h"

#include <cmath>

namespace eng {

namespace {
constexpr double kSame = 1e-5;

bool same(const glm::dvec3 &a, const glm::dvec3 &b) { return glm::length(a - b) < kSame * 10.0; }

int indexOf(const std::vector<glm::dvec3> &pts, const glm::dvec3 &p) {
  for (size_t i = 0; i < pts.size(); ++i)
    if (same(pts[i], p))
      return int(i);
  return -1;
}

// Affine map taking three old points to three new ones (plane normal to the new normal). False when degenerate.
bool affineFrom3(const glm::dvec3 o[3], const glm::dvec3 n[3], glm::dmat3 &A, glm::dvec3 &t) {
  const glm::dvec3 e1 = o[1] - o[0], e2 = o[2] - o[0];
  const glm::dvec3 f1 = n[1] - n[0], f2 = n[2] - n[0];
  const glm::dvec3 en = glm::cross(e1, e2), fn = glm::cross(f1, f2);
  if (glm::length(en) < 1e-9 || glm::length(fn) < 1e-9)
    return false;
  const glm::dmat3 E(e1, e2, glm::normalize(en));
  const glm::dmat3 F(f1, f2, glm::normalize(fn));
  if (std::fabs(glm::determinant(E)) < 1e-12)
    return false;
  A = F * glm::inverse(E);
  t = n[0] - A * o[0];
  return true;
}

} // namespace

void lockBasisAffine(FaceTexBasis &b, const glm::dmat3 &A, const glm::dvec3 &t) {
  if (!b.valid || std::fabs(glm::determinant(A)) < 1e-12)
    return;
  const glm::dmat3 Ainv = glm::inverse(A);
  const glm::dmat3 AinvT = glm::transpose(Ainv);
  const glm::dvec3 tl = Ainv * t;
  auto solve = [&](glm::vec3 &axis, float &tpu, float &off) {
    const glm::dvec3 u(axis);
    const glm::dvec3 nu = AinvT * u;
    const double len = glm::length(nu);
    if (len < 1e-12)
      return;
    const double newOff = double(off) - double(tpu) * glm::dot(tl, u);
    axis = glm::vec3(nu / len);
    tpu = float(double(tpu) * len);
    off = float(newOff - std::floor(newOff));
  };
  solve(b.uAxis, b.tilesPerUnitU, b.offsetU);
  solve(b.vAxis, b.tilesPerUnitV, b.offsetV);
}

std::vector<std::pair<glm::dvec3, glm::dvec3>> brushEdges(const ConvexBrush &brush) {
  std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
  for (const auto &poly: brush.polygons) {
    for (size_t i = 0; i < poly.size(); ++i) {
      const glm::dvec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
      bool dup = false;
      for (const auto &e: out)
        if ((same(e.first, a) && same(e.second, b)) || (same(e.first, b) && same(e.second, a))) {
          dup = true;
          break;
        }
      if (!dup)
        out.emplace_back(a, b);
    }
  }
  return out;
}

VertexEditResult moveBrushVertices(const ConvexBrush &orig, const std::vector<glm::dvec3> &newPositions,
                                   const std::vector<glm::dvec3> &extra, const bool textureLock) {
  VertexEditResult res;
  const std::vector<glm::dvec3> oldV = orig.vertices();
  if (newPositions.size() != oldV.size())
    return res;
  std::vector<glm::dvec3> pts = newPositions;
  pts.insert(pts.end(), extra.begin(), extra.end());
  const MaterialPtr mat0 = orig.faces.empty() ? MaterialPtr() : orig.faces.front().material;
  auto hull = ConvexBrush::fromPoints(pts, mat0, 0.04);
  if (!hull || !hull->valid())
    return res;

  const std::vector<glm::dvec3> hullV = hull->vertices();
  for (size_t i = 0; i < oldV.size(); ++i)
    if (!same(oldV[i], newPositions[i]) && indexOf(hullV, newPositions[i]) < 0)
      ++res.absorbed;
  for (const auto &p: extra)
    if (indexOf(hullV, p) < 0)
      ++res.absorbed;
  res.retopologized = hull->faces.size() != orig.faces.size();

  auto mapOld = [&](const glm::dvec3 &q) -> glm::dvec3 {
    const int i = indexOf(oldV, q);
    return i >= 0 ? newPositions[size_t(i)] : q;
  };

  for (size_t fi = 0; fi < hull->faces.size(); ++fi) {
    BrushFace &f = hull->faces[fi];
    const auto &poly = hull->polygons[fi];
    int best = -1;
    double bestScore = -1.0;
    for (size_t oi = 0; oi < orig.faces.size(); ++oi) {
      const BrushFace &of = orig.faces[oi];
      double score = 0.0;
      for (const auto &q: orig.polygons[oi])
        if (indexOf(poly, mapOld(q)) >= 0)
          score += 1.0;
      const double par = glm::dot(of.normal, f.normal);
      if (par > 1.0 - 1e-9 && std::fabs(of.distance - f.distance) < 1e-4)
        score += 100.0;
      score += 0.01 * par; // tie-break: the more parallel face
      if (score > bestScore) {
        bestScore = score;
        best = int(oi);
      }
    }
    if (best < 0)
      continue;
    const BrushFace &src = orig.faces[size_t(best)];
    f.material = src.material;
    f.basis = src.basis;
    f.groups = src.groups;
    if (!textureLock || !src.basis.valid)
      continue;
    // Reference vertices of the source face: unmoved ones first.
    std::vector<std::pair<glm::dvec3, glm::dvec3>> still, moved;
    for (const auto &q: orig.polygons[size_t(best)]) {
      const glm::dvec3 nq = mapOld(q);
      (same(q, nq) ? still : moved).emplace_back(q, nq);
    }
    if (still.size() >= 3)
      continue; // the plane is pinned by three corners
    std::vector<std::pair<glm::dvec3, glm::dvec3>> refs = still;
    refs.insert(refs.end(), moved.begin(), moved.end());
    bool solved = false;
    for (size_t a = 0; a < refs.size() && !solved; ++a)
      for (size_t b = a + 1; b < refs.size() && !solved; ++b)
        for (size_t c = b + 1; c < refs.size() && !solved; ++c) {
          const glm::dvec3 o[3] = {refs[a].first, refs[b].first, refs[c].first};
          const glm::dvec3 n[3] = {refs[a].second, refs[b].second, refs[c].second};
          glm::dmat3 A;
          glm::dvec3 t;
          if (affineFrom3(o, n, A, t)) {
            lockBasisAffine(f.basis, A, t);
            solved = true;
          }
        }
  }
  res.brush = std::move(*hull);
  return res;
}

} // namespace eng
