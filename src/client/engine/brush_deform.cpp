#include "brush_deform.h"

#include <algorithm>
#include <cmath>

#include "vertex_ops.h"

namespace eng::deform {

namespace {

constexpr double kPlanarTol = 1e-3;  // world units: a face this flat stays one face
constexpr double kConvexTol = 2e-3;  // a vertex this far in front of a face breaks convexity

glm::dvec3 newell(const std::vector<glm::dvec3> &poly) {
  glm::dvec3 n(0.0);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  const double l = glm::length(n);
  return l > 1e-14 ? n / l : glm::dvec3(0.0);
}

glm::dvec3 mean(const std::vector<glm::dvec3> &pts) {
  glm::dvec3 c(0.0);
  for (const auto &p: pts)
    c += p;
  return pts.empty() ? c : c / double(pts.size());
}

struct Candidate {
  glm::dvec3 normal;
  double distance;
  int src;                          // source face index
  std::vector<glm::dvec3> orig, mapped; // corners before / after (for the texture solve)
};

// Outward plane through `mapped` (away from the brush centroid).
bool planeOf(const std::vector<glm::dvec3> &mapped, const glm::dvec3 &centroid, glm::dvec3 &n, double &d) {
  n = newell(mapped);
  if (glm::length(n) < 0.5)
    return false;
  d = glm::dot(n, mean(mapped));
  if (glm::dot(n, centroid) - d > 0.0) {
    n = -n;
    d = -d;
  }
  return true;
}

bool behindAll(const glm::dvec3 &n, const double d, const std::vector<glm::dvec3> &pts) {
  for (const auto &p: pts)
    if (glm::dot(n, p) - d > kConvexTol)
      return false;
  return true;
}

// Marks the face a split just made (plane n·x = d) as a cut.
void markCut(ConvexBrush &b, const glm::dvec3 &n, const double d) {
  for (BrushFace &f: b.faces)
    if (glm::length(f.normal - n) < 1e-9 && std::fabs(f.distance - d) < 1e-9)
      f.cut = true;
}

} // namespace

double solveBasisFromCorners(FaceTexBasis &b, const std::vector<glm::dvec3> &pts, const std::vector<glm::dvec2> &uv,
                             const glm::dvec3 &n) {
  if (pts.size() < 3 || uv.size() != pts.size())
    return -1.0;
  glm::dvec3 c(0.0);
  for (const auto &p: pts)
    c += p;
  c /= double(pts.size());
  // In-plane frame from the longest spoke.
  glm::dvec3 e1(0.0);
  for (const auto &p: pts) {
    const glm::dvec3 d = (p - c) - n * glm::dot(p - c, n);
    if (glm::length(d) > glm::length(e1))
      e1 = d;
  }
  if (glm::length(e1) < 1e-9)
    return -1.0;
  e1 = glm::normalize(e1);
  const glm::dvec3 e2 = glm::normalize(glm::cross(n, e1));
  // Least squares for U = a·X + b·Y + k (and V): normal equations.
  glm::dmat3 m(0.0);
  glm::dvec3 ru(0.0), rv(0.0);
  for (size_t i = 0; i < pts.size(); ++i) {
    const glm::dvec3 w(glm::dot(pts[i] - c, e1), glm::dot(pts[i] - c, e2), 1.0);
    m += glm::outerProduct(w, w);
    ru += w * uv[i].x;
    rv += w * uv[i].y;
  }
  if (std::fabs(glm::determinant(m)) < 1e-12)
    return -1.0;
  const glm::dmat3 mi = glm::inverse(m);
  const glm::dvec3 cu = mi * ru, cv = mi * rv;
  const glm::dvec3 gu = e1 * cu.x + e2 * cu.y, gv = e1 * cv.x + e2 * cv.y;
  const double lu = glm::length(gu), lv = glm::length(gv);
  if (lu < 1e-12 || lv < 1e-12)
    return -1.0;
  // UV = g·(p - c) + k = g·p + (k - g·c); the offset wraps (textures repeat).
  auto wrap = [](const double o) { return o - std::floor(o); };
  FaceTexBasis out = b;
  out.uAxis = glm::vec3(gu / lu);
  out.vAxis = glm::vec3(gv / lv);
  out.tilesPerUnitU = float(lu);
  out.tilesPerUnitV = float(lv);
  out.offsetU = float(wrap(cu.z - glm::dot(gu, c)));
  out.offsetV = float(wrap(cv.z - glm::dot(gv, c)));
  out.valid = true;
  // The miss, measured before rounding the result to float.
  double miss = 0.0;
  for (size_t i = 0; i < pts.size(); ++i) {
    const glm::dvec2 got(glm::dot(gu, pts[i] - c) + cu.z, glm::dot(gv, pts[i] - c) + cv.z);
    miss = std::max(miss, std::max(std::fabs(got.x - uv[i].x) / lu, std::fabs(got.y - uv[i].y) / lv));
  }
  b = out;
  return miss;
}

std::vector<ConvexBrush> splitForTexture(const ConvexBrush &brush, const Map &map, const TexTarget &target,
                                         const double tol) {
  struct Step {
    ConvexBrush brush;
    int depth;
  };
  std::vector<ConvexBrush> done;
  std::vector<Step> todo {{brush, 0}};
  while (!todo.empty()) {
    Step st = std::move(todo.back());
    todo.pop_back();
    const ConvexBrush &b = st.brush;
    bool cut = false;
    for (size_t fi = 0; target && st.depth < 3 && fi < b.faces.size() && fi < b.polygons.size() && !cut; ++fi) {
      const BrushFace &face = b.faces[fi];
      const std::vector<glm::dvec3> &poly = b.polygons[fi];
      if (face.cut || !face.basis.valid || poly.size() < 4)
        continue;
      std::vector<glm::dvec3> mp;
      std::vector<glm::dvec2> uv;
      for (const auto &p: poly) {
        mp.push_back(map(p));
        uv.push_back(target(face, p));
      }
      const glm::dvec3 n = newell(mp);
      if (glm::length(n) < 0.5)
        continue;
      const double d = glm::dot(n, mean(mp));
      double dev = 0.0;
      for (const auto &q: mp)
        dev = std::max(dev, std::fabs(glm::dot(n, q) - d));
      if (dev > kPlanarTol)
        continue; // bends: the deformation splits it into triangles anyway
      FaceTexBasis fit = face.basis;
      if (solveBasisFromCorners(fit, mp, uv, n) <= tol)
        continue;
      // The shorter diagonal (ties: the one through the lowest corner).
      const int k = int(poly.size());
      int bi = -1, bj = -1;
      double best = 1e300;
      auto lower = [](const glm::dvec3 &a, const glm::dvec3 &c) {
        return a.x < c.x - 1e-9 || (std::fabs(a.x - c.x) <= 1e-9 && (a.y < c.y - 1e-9 || (std::fabs(a.y - c.y) <= 1e-9 && a.z < c.z)));
      };
      for (int i = 0; i < k; ++i)
        for (int j = i + 2; j < k; ++j) {
          if (i == 0 && j == k - 1)
            continue;
          const double l = glm::length(poly[size_t(j)] - poly[size_t(i)]);
          const glm::dvec3 lo = lower(poly[size_t(i)], poly[size_t(j)]) ? poly[size_t(i)] : poly[size_t(j)];
          const bool tieBetter = bi >= 0 && std::fabs(l - best) < 1e-9 * std::max(1.0, l)
                                 && lower(lo, lower(poly[size_t(bi)], poly[size_t(bj)]) ? poly[size_t(bi)] : poly[size_t(bj)]);
          if (l < best - 1e-9 * std::max(1.0, l) || tieBetter) {
            best = l;
            bi = i;
            bj = j;
          }
        }
      if (bi < 0)
        continue;
      const glm::dvec3 a = poly[size_t(bi)], c = poly[size_t(bj)];
      glm::dvec3 pn = glm::cross(c - a, face.normal);
      if (glm::length(pn) < 1e-9)
        continue;
      pn = glm::normalize(pn);
      const double pd = glm::dot(pn, a);
      std::optional<ConvexBrush> front, back;
      b.split(pn, pd, front, back);
      if (!front || !back || !front->valid() || !back->valid())
        continue;
      markCut(*back, pn, pd);
      markCut(*front, -pn, -pd);
      todo.push_back({std::move(*back), st.depth + 1});
      todo.push_back({std::move(*front), st.depth + 1});
      cut = true;
    }
    if (!cut)
      done.push_back(std::move(st.brush));
  }
  return done;
}

std::vector<ConvexBrush> slice(const ConvexBrush &brush, const glm::dvec3 &normal, std::vector<double> distances) {
  std::sort(distances.begin(), distances.end());
  std::vector<ConvexBrush> out;
  std::optional<ConvexBrush> rest = brush;
  const glm::dvec3 n = glm::normalize(normal);
  for (const double d: distances) {
    if (!rest)
      break;
    std::optional<ConvexBrush> front, back;
    rest->split(n, d, front, back);
    if (back) {
      markCut(*back, n, d);
      if (back->valid())
        out.push_back(std::move(*back));
    }
    if (front)
      markCut(*front, -n, -d);
    rest = std::move(front);
  }
  if (rest && rest->valid())
    out.push_back(std::move(*rest));
  return out;
}

Result deformBrush(const ConvexBrush &brush, const Map &map, const TexTarget &target) {
  Result res;
  const std::vector<glm::dvec3> verts = brush.vertices();
  std::vector<glm::dvec3> moved;
  moved.reserve(verts.size());
  for (const auto &v: verts)
    moved.push_back(map(v));
  const glm::dvec3 centroid = mean(moved);

  std::vector<Candidate> faces;
  bool ok = true;
  for (int fi = 0; fi < int(brush.faces.size()) && fi < int(brush.polygons.size()) && ok; ++fi) {
    const std::vector<glm::dvec3> &poly = brush.polygons[size_t(fi)];
    if (poly.size() < 3)
      continue;
    std::vector<glm::dvec3> mp;
    mp.reserve(poly.size());
    for (const auto &p: poly)
      mp.push_back(map(p));
    glm::dvec3 n;
    double d = 0.0;
    if (!planeOf(mp, centroid, n, d)) {
      ok = false;
      break;
    }
    double dev = 0.0;
    for (const auto &q: mp)
      dev = std::max(dev, std::fabs(glm::dot(n, q) - d));
    if (dev <= kPlanarTol && behindAll(n, d, moved)) {
      faces.push_back({n, d, fi, poly, mp});
      continue;
    }
    // Bent: a triangle fan from the first corner whose planes keep every vertex behind them.
    const int k = int(mp.size());
    bool found = false;
    for (int r = 0; r < k && !found; ++r) {
      std::vector<Candidate> tris;
      bool good = true;
      for (int i = 1; i + 1 < k && good; ++i) {
        const int a = r, b = (r + i) % k, c = (r + i + 1) % k;
        std::vector<glm::dvec3> tm {mp[size_t(a)], mp[size_t(b)], mp[size_t(c)]};
        glm::dvec3 tn;
        double td = 0.0;
        if (!planeOf(tm, centroid, tn, td)) {
          good = false;
          break;
        }
        good = behindAll(tn, td, moved);
        tris.push_back({tn, td, fi, {poly[size_t(a)], poly[size_t(b)], poly[size_t(c)]}, tm});
      }
      if (good) {
        faces.insert(faces.end(), tris.begin(), tris.end());
        res.triangulated = true;
        found = true;
      }
    }
    ok = found;
  }

  if (ok) {
    ConvexBrush out;
    for (const Candidate &c: faces) {
      const BrushFace &src = brush.faces[size_t(c.src)];
      BrushFace f;
      f.normal = c.normal;
      f.distance = c.distance;
      f.material = src.material;
      f.groups = src.groups;
      f.basis = src.basis;
      f.cut = src.cut;
      if (f.basis.valid) {
        if (target) {
          // Exact at the corners: the texture each corner should show.
          std::vector<glm::dvec2> uv;
          uv.reserve(c.orig.size());
          for (const auto &p: c.orig)
            uv.push_back(target(src, p));
          solveBasisFromCorners(f.basis, c.mapped, uv, c.normal);
        } else {
          transformFaceBasis(f.basis, glm::dmat4(1.0), src.normal, c.normal, false);
        }
      }
      out.faces.push_back(f);
    }
    if (out.build() && out.valid()) {
      res.brush = std::move(out);
      return res;
    }
  }

  // Fallback: the vertex tool's hull rebuild (also detects folding).
  const VertexEditResult v = moveBrushVertices(brush, moved, {}, bool(target));
  if (!v.brush || v.absorbed > 0 || !v.brush->valid()) {
    res.folded = true;
    return res;
  }
  res.brush = v.brush;
  res.triangulated = res.triangulated || v.retopologized;
  return res;
}

} // namespace eng::deform
