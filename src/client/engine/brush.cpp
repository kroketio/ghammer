#include "brush.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

#include "gameobject.h"

namespace eng {

namespace {

constexpr double kHuge = 1e5;

glm::dvec3 anyPerpendicular(const glm::dvec3 &n) {
  const glm::dvec3 a = std::fabs(n.x) < 0.6 ? glm::dvec3(1, 0, 0) : (std::fabs(n.y) < 0.6 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1));
  return glm::normalize(glm::cross(n, a));
}

// Keeps the part of `poly` with signedDistance <= eps (Sutherland-Hodgman).
std::vector<glm::dvec3> clipPolygon(const std::vector<glm::dvec3> &poly, const glm::dvec3 &n, double d) {
  std::vector<glm::dvec3> out;
  if (poly.empty())
    return out;
  out.reserve(poly.size() + 2);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 &a = poly[i];
    const glm::dvec3 &b = poly[(i + 1) % poly.size()];
    const double da = glm::dot(n, a) - d;
    const double db = glm::dot(n, b) - d;
    const bool ina = da <= ConvexBrush::kEpsilon;
    const bool inb = db <= ConvexBrush::kEpsilon;
    if (ina)
      out.push_back(a);
    if (ina != inb) {
      const double t = da / (da - db);
      out.push_back(a + (b - a) * t);
    }
  }
  return out;
}

void weld(std::vector<glm::dvec3> &poly) {
  std::vector<glm::dvec3> out;
  for (const auto &p: poly)
    if (out.empty() || glm::distance(out.back(), p) > ConvexBrush::kEpsilon * 4)
      out.push_back(p);
  while (out.size() > 1 && glm::distance(out.front(), out.back()) <= ConvexBrush::kEpsilon * 4)
    out.pop_back();
  // Snap to a fine grid so vertices shared by neighbouring faces agree exactly.
  for (auto &p: out)
    p = glm::round(p * 1e5) / 1e5;
  poly = std::move(out);
}

glm::dvec3 newellNormal(const std::vector<glm::dvec3> &poly) {
  glm::dvec3 n(0.0);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 &a = poly[i];
    const glm::dvec3 &b = poly[(i + 1) % poly.size()];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  const double len = glm::length(n);
  return len > 1e-14 ? n / len : glm::dvec3(0.0);
}

} // namespace

void defaultTexAxes(const glm::dvec3 &n, glm::vec3 &u, glm::vec3 &v) {
  const glm::dvec3 a = glm::abs(n);
  if (a.y >= a.x && a.y >= a.z) {
    // Floors and ceilings: seen from their outside with -Z up on screen.
    u = n.y > 0 ? glm::vec3(1, 0, 0) : glm::vec3(-1, 0, 0);
    v = {0, 0, 1};
  } else if (a.x >= a.z) {
    u = n.x > 0 ? glm::vec3(0, 0, -1) : glm::vec3(0, 0, 1);
    v = {0, -1, 0};
  } else {
    u = n.z > 0 ? glm::vec3(1, 0, 0) : glm::vec3(-1, 0, 0);
    v = {0, -1, 0};
  }
}

FaceTexBasis paraxialBasis(const glm::dvec3 &n, const double tilesPerUnit) {
  FaceTexBasis b;
  defaultTexAxes(n, b.uAxis, b.vAxis);
  b.tilesPerUnitU = float(tilesPerUnit);
  b.tilesPerUnitV = float(tilesPerUnit);
  b.valid = true;
  return b;
}

void unmirrorBasis(FaceTexBasis &b, const int axisMask) {
  const int count = (axisMask & 1) + ((axisMask >> 1) & 1) + ((axisMask >> 2) & 1);
  if (count % 2 == 0)
    return;
  // One axis: that one; all three: a turn about Y plus a mirror in Y.
  const int axis = count == 1 ? (axisMask & 1 ? 0 : axisMask & 2 ? 1 : 2) : 1;
  if (std::fabs(b.uAxis[axis]) >= std::fabs(b.vAxis[axis]) - 1e-4f) {
    b.uAxis = -b.uAxis;
    b.offsetU = -b.offsetU;
  } else {
    b.vAxis = -b.vAxis;
    b.offsetV = -b.offsetV;
  }
}

bool ConvexBrush::build() {
  // Drop duplicate planes: both copies would survive clipping as a doubled face.
  std::vector<BrushFace> unique;
  for (auto f: faces) {
    const double len = glm::length(f.normal);
    if (len < 1e-12)
      continue;
    f.normal /= len;
    f.distance /= len;
    bool dup = false;
    for (const auto &u: unique)
      if (glm::dot(u.normal, f.normal) > 1.0 - 1e-9 && std::fabs(u.distance - f.distance) < kEpsilon)
        dup = true;
    if (!dup)
      unique.push_back(f);
  }

  std::vector<BrushFace> kept;
  std::vector<std::vector<glm::dvec3>> polys;
  for (size_t i = 0; i < unique.size(); ++i) {
    const glm::dvec3 n = unique[i].normal;
    const glm::dvec3 u = anyPerpendicular(n);
    const glm::dvec3 v = glm::cross(n, u);
    const glm::dvec3 c = n * unique[i].distance;
    // Counter-clockwise seen from outside (around +n).
    std::vector<glm::dvec3> poly = {c + (-u - v) * kHuge, c + (u - v) * kHuge, c + (u + v) * kHuge, c + (-u + v) * kHuge};
    for (size_t j = 0; j < unique.size() && poly.size() >= 3; ++j)
      if (j != i)
        poly = clipPolygon(poly, unique[j].normal, unique[j].distance);
    weld(poly);
    if (poly.size() < 3)
      continue;
    // A zero-area sliver is not a face.
    double area = 0.0;
    for (size_t k = 1; k + 1 < poly.size(); ++k)
      area += glm::length(glm::cross(poly[k] - poly[0], poly[k + 1] - poly[0])) * 0.5;
    if (area < 1e-7)
      continue;
    kept.push_back(unique[i]);
    polys.push_back(std::move(poly));
  }
  faces = std::move(kept);
  polygons = std::move(polys);
  // Unbounded planes leave polygons reaching out to kHuge.
  for (const auto &p: polygons)
    for (const auto &v: p)
      if (std::fabs(v.x) > kHuge * 0.5 || std::fabs(v.y) > kHuge * 0.5 || std::fabs(v.z) > kHuge * 0.5)
        return false;
  return valid();
}

double ConvexBrush::volume() const {
  if (polygons.size() < 4)
    return 0.0;
  const glm::dvec3 ref = centroid();
  double v = 0.0;
  for (const auto &poly: polygons)
    for (size_t k = 1; k + 1 < poly.size(); ++k)
      v += std::fabs(glm::dot(poly[0] - ref, glm::cross(poly[k] - ref, poly[k + 1] - ref))) / 6.0;
  return v;
}

glm::dvec3 ConvexBrush::centroid() const {
  glm::dvec3 c(0.0);
  size_t n = 0;
  for (const auto &poly: polygons)
    for (const auto &p: poly) {
      c += p;
      ++n;
    }
  return n ? c / double(n) : c;
}

void ConvexBrush::bounds(glm::dvec3 &mn, glm::dvec3 &mx) const {
  mn = glm::dvec3(1e300);
  mx = glm::dvec3(-1e300);
  for (const auto &poly: polygons)
    for (const auto &p: poly) {
      mn = glm::min(mn, p);
      mx = glm::max(mx, p);
    }
}

std::vector<glm::dvec3> ConvexBrush::vertices() const {
  std::vector<glm::dvec3> out;
  for (const auto &poly: polygons)
    for (const auto &p: poly) {
      bool dup = false;
      for (const auto &q: out)
        if (glm::distance(p, q) < kEpsilon * 10) {
          dup = true;
          break;
        }
      if (!dup)
        out.push_back(p);
    }
  return out;
}

std::optional<ConvexBrush> ConvexBrush::fromObject(const GameObject &object) {
  const auto mesh = object.getMesh();
  if (!mesh)
    return std::nullopt;
  const glm::dmat4 model(object.getModelMatrix());
  ConvexBrush b;
  std::vector<glm::vec3> local;
  std::vector<std::vector<glm::dvec3>> worldPolys;
  for (int f = 0; f < mesh->faceCount(); ++f) {
    if (!mesh->getFacePolygon(f, local))
      continue;
    std::vector<glm::dvec3> poly;
    poly.reserve(local.size());
    for (const auto &p: local)
      poly.push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(p), 1.0)));
    const glm::dvec3 n = newellNormal(poly);
    if (glm::length(n) < 0.5)
      continue;
    glm::dvec3 c(0.0);
    for (const auto &p: poly)
      c += p;
    c /= double(poly.size());
    BrushFace face;
    face.normal = n;
    face.distance = glm::dot(n, c);
    face.material = mesh->faceMaterialRef(f);
    face.basis = mesh->faceTexBasis(f);
    face.groups = mesh->faceGroups(f);
    b.faces.push_back(face);
    worldPolys.push_back(std::move(poly));
  }
  // Mesh windings may face in or out (Source normals point into the solid): orient every plane outwards.
  glm::dvec3 centre(0.0);
  size_t count = 0;
  for (const auto &poly: worldPolys)
    for (const auto &p: poly) {
      centre += p;
      ++count;
    }
  if (count == 0)
    return std::nullopt;
  centre /= double(count);
  for (auto &f: b.faces)
    if (f.signedDistance(centre) > 0.0) {
      f.normal = -f.normal;
      f.distance = -f.distance;
    }
  if (!b.build())
    return std::nullopt;
  return b;
}

std::optional<ConvexBrush> ConvexBrush::fromPlanes(std::vector<BrushFace> planes) {
  ConvexBrush b;
  b.faces = std::move(planes);
  if (!b.build())
    return std::nullopt;
  return b;
}

std::optional<ConvexBrush> ConvexBrush::fromPoints(const std::vector<glm::dvec3> &input, const MaterialPtr &material,
                                                   const double tilesPerUnit) {
  // Weld, then every plane through three points with all others behind it is a hull face (brute force; clouds
  // are small).
  std::vector<glm::dvec3> pts;
  for (const auto &p: input) {
    bool dup = false;
    for (const auto &q: pts)
      if (glm::distance(p, q) < kEpsilon * 10) {
        dup = true;
        break;
      }
    if (!dup)
      pts.push_back(p);
  }
  if (pts.size() < 4 || pts.size() > 400)
    return std::nullopt;
  glm::dvec3 centre(0.0);
  for (const auto &p: pts)
    centre += p;
  centre /= double(pts.size());

  ConvexBrush b;
  const size_t n = pts.size();
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j)
      for (size_t k = j + 1; k < n; ++k) {
        glm::dvec3 nrm = glm::cross(pts[j] - pts[i], pts[k] - pts[i]);
        const double len = glm::length(nrm);
        if (len < 1e-9)
          continue;
        nrm /= len;
        double d = glm::dot(nrm, pts[i]);
        if (glm::dot(nrm, centre) - d > 0.0) {
          nrm = -nrm;
          d = -d;
        }
        bool hull = true;
        for (size_t m = 0; m < n && hull; ++m)
          hull = glm::dot(nrm, pts[m]) - d <= kEpsilon * 10;
        if (!hull)
          continue;
        bool dup = false;
        for (const auto &f: b.faces)
          if (glm::dot(f.normal, nrm) > 1.0 - 1e-7 && std::fabs(f.distance - d) < kEpsilon * 10) {
            dup = true;
            break;
          }
        if (dup)
          continue;
        BrushFace face;
        face.normal = nrm;
        face.distance = d;
        face.material = material;
        face.basis = paraxialBasis(nrm, tilesPerUnit);
        b.faces.push_back(face);
      }
  if (!b.build())
    return std::nullopt;
  return b;
}

ConvexBrush ConvexBrush::box(const glm::dvec3 &mn, const glm::dvec3 &mx, const MaterialPtr &material,
                             const double tilesPerUnit) {
  ConvexBrush b;
  for (int axis = 0; axis < 3; ++axis)
    for (int s: {1, -1}) {
      BrushFace f;
      f.normal = glm::dvec3(0.0);
      f.normal[axis] = s;
      f.distance = s > 0 ? mx[axis] : -mn[axis];
      f.material = material;
      f.basis = paraxialBasis(f.normal, tilesPerUnit);
      b.faces.push_back(f);
    }
  b.build();
  return b;
}

std::shared_ptr<Mesh> ConvexBrush::toMesh() const {
  // Faces wound clockwise from outside: meshes (and their back-face culling) use Source's inward winding.
  std::vector<std::vector<glm::vec3>> polys;
  polys.reserve(polygons.size());
  for (const auto &poly: polygons) {
    std::vector<glm::vec3> p;
    p.reserve(poly.size());
    for (auto it = poly.rbegin(); it != poly.rend(); ++it)
      p.push_back(glm::vec3(*it));
    polys.push_back(std::move(p));
  }
  auto mesh = Mesh::createFromPolygons(polys);
  for (size_t f = 0; f < faces.size(); ++f) {
    mesh->setFaceMaterial(int(f), faces[f].material);
    FaceTexBasis basis = faces[f].basis;
    if (!basis.valid)
      basis = paraxialBasis(faces[f].normal, 0.0390625);
    mesh->setFaceTexBasis(int(f), basis);
    if (!faces[f].groups.empty())
      mesh->setFaceGroups(int(f), faces[f].groups);
  }
  mesh->refreshTangentFrame();
  return mesh;
}

const BrushFace *ConvexBrush::mostParallelFace(const glm::dvec3 &n) const {
  const BrushFace *best = nullptr;
  double bestDot = -2.0;
  for (const auto &f: faces) {
    const double d = glm::dot(f.normal, n);
    if (d > bestDot) {
      bestDot = d;
      best = &f;
    }
  }
  return best;
}

bool ConvexBrush::clip(const BrushFace &plane) {
  faces.push_back(plane);
  return build();
}

void ConvexBrush::split(const glm::dvec3 &normal, const double distance, std::optional<ConvexBrush> &front,
                        std::optional<ConvexBrush> &back) const {
  front.reset();
  back.reset();
  const glm::dvec3 n = glm::normalize(normal);
  // Entirely on one side: no cut.
  double lo = 1e300, hi = -1e300;
  for (const auto &poly: polygons)
    for (const auto &p: poly) {
      const double s = glm::dot(n, p) - distance;
      lo = std::min(lo, s);
      hi = std::max(hi, s);
    }
  if (hi <= kEpsilon) {
    back = *this;
    return;
  }
  if (lo >= -kEpsilon) {
    front = *this;
    return;
  }
  // Back piece keeps n·x <= d; its cap faces +n and inherits from the face most aligned with +n.
  {
    ConvexBrush b = *this;
    BrushFace cap;
    cap.normal = n;
    cap.distance = distance;
    if (const BrushFace *src = mostParallelFace(n)) {
      cap.material = src->material;
      cap.basis = src->basis;
      cap.groups = src->groups;
    }
    if (b.clip(cap))
      back = std::move(b);
  }
  {
    ConvexBrush f = *this;
    BrushFace cap;
    cap.normal = -n;
    cap.distance = -distance;
    if (const BrushFace *src = mostParallelFace(-n)) {
      cap.material = src->material;
      cap.basis = src->basis;
      cap.groups = src->groups;
    }
    if (f.clip(cap))
      front = std::move(f);
  }
}

bool ConvexBrush::overlaps(const ConvexBrush &other) const {
  glm::dvec3 a0, a1, b0, b1;
  bounds(a0, a1);
  other.bounds(b0, b1);
  for (int i = 0; i < 3; ++i)
    if (a1[i] <= b0[i] + kEpsilon || b1[i] <= a0[i] + kEpsilon)
      return false;
  return intersect(other).has_value();
}

std::optional<ConvexBrush> ConvexBrush::intersect(const ConvexBrush &other) const {
  ConvexBrush r = *this;
  for (const auto &f: other.faces)
    r.faces.push_back(f);
  if (!r.build())
    return std::nullopt;
  return r;
}

std::vector<ConvexBrush> ConvexBrush::subtract(const ConvexBrush &cutter) const {
  if (!overlaps(cutter))
    return {*this};
  // Peel off what is in front of each cutter plane; what is behind all of them is discarded. Axis-aligned
  // planes first, for clean box-shaped fragments.
  std::vector<const BrushFace *> order;
  for (const auto &f: cutter.faces)
    order.push_back(&f);
  std::stable_sort(order.begin(), order.end(), [](const BrushFace *a, const BrushFace *b) {
    const auto axisness = [](const glm::dvec3 &n) { return std::max({std::fabs(n.x), std::fabs(n.y), std::fabs(n.z)}); };
    return axisness(a->normal) > axisness(b->normal) + 1e-9;
  });

  std::vector<ConvexBrush> out;
  std::optional<ConvexBrush> rest = *this;
  for (const BrushFace *p: order) {
    if (!rest)
      break;
    std::optional<ConvexBrush> front, back;
    rest->split(p->normal, p->distance, front, back);
    if (front) {
      // The cap against the hole shows the cutter's surface.
      for (auto &f: front->faces) {
        if (glm::dot(f.normal, -p->normal) <= 1.0 - 1e-6 || std::fabs(f.distance + p->distance) >= kEpsilon * 10)
          continue;
        if (p->material) {
          f.material = p->material;
          f.basis = p->basis;
        }
        if (!p->groups.empty())
          f.groups = p->groups;
      }
      out.push_back(std::move(*front));
    }
    rest = std::move(back);
  }
  return out;
}

std::vector<ConvexBrush> ConvexBrush::hollow(const double thickness) const {
  ConvexBrush inner = *this;
  for (auto &f: inner.faces)
    f.distance -= thickness;
  if (!inner.build())
    return {*this}; // too thin to hollow: leave it solid
  return subtract(inner);
}

void fitBasisToFace(FaceTexBasis &b, const std::vector<glm::vec3> &poly, const glm::vec3 &normal, const int repeatU,
                    const int repeatV) {
  if (poly.size() < 3 || glm::length(normal) < 1e-9f)
    return;
  const glm::dvec3 n = glm::normalize(glm::dvec3(normal));
  auto flat = [&n](const glm::dvec3 &d) { return d - n * glm::dot(d, n); };
  // The direction U should keep: the current U in the face plane, else the default U.
  glm::dvec3 ref = flat(glm::dvec3(b.uAxis));
  if (!b.valid || glm::length(ref) < 1e-6) {
    glm::vec3 du, dv;
    defaultTexAxes(n, du, dv);
    ref = flat(glm::dvec3(du));
  }
  if (glm::length(ref) < 1e-6)
    ref = flat(std::fabs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
  ref = glm::normalize(ref);
  // V's side of U: cross(u, v) against n (−1 for the default axes).
  const double hand = b.valid && glm::dot(glm::cross(glm::dvec3(b.uAxis), glm::dvec3(b.vAxis)), n) > 0.0 ? 1.0 : -1.0;

  // The edge closest to that direction (tie: the longer edge).
  glm::dvec3 best(0.0);
  double bestAlign = -1.0, bestLen = 0.0;
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 e = flat(glm::dvec3(poly[(i + 1) % poly.size()]) - glm::dvec3(poly[i]));
    const double len = glm::length(e);
    if (len < 1e-5)
      continue;
    const double align = std::fabs(glm::dot(e / len, ref));
    if (align > bestAlign + 1e-6 || (align > bestAlign - 1e-6 && len > bestLen)) {
      best = e / len;
      bestAlign = align;
      bestLen = len;
    }
  }
  if (bestAlign >= 0.0) {
    const glm::dvec3 u = glm::dot(best, ref) < 0.0 ? -best : best;
    // cross(u, cross(n, u)) = n: cross(n, u) is V's side for hand +1.
    b.uAxis = glm::vec3(u);
    b.vAxis = glm::vec3(glm::normalize(glm::cross(n, u)) * hand);
  }

  // The extents along the new axes.
  double u0 = DBL_MAX, u1 = -DBL_MAX, v0 = DBL_MAX, v1 = -DBL_MAX;
  for (const glm::vec3 &p: poly) {
    const glm::dvec3 q(p);
    u0 = std::min(u0, glm::dot(q, glm::dvec3(b.uAxis)));
    u1 = std::max(u1, glm::dot(q, glm::dvec3(b.uAxis)));
    v0 = std::min(v0, glm::dot(q, glm::dvec3(b.vAxis)));
    v1 = std::max(v1, glm::dot(q, glm::dvec3(b.vAxis)));
  }
  if (repeatU > 0 && u1 - u0 > 1e-5) {
    b.tilesPerUnitU = float(double(repeatU) / (u1 - u0));
    b.offsetU = float(-u0 * double(b.tilesPerUnitU));
  }
  if (repeatV > 0 && v1 - v0 > 1e-5) {
    b.tilesPerUnitV = float(double(repeatV) / (v1 - v0));
    b.offsetV = float(-v0 * double(b.tilesPerUnitV));
  }
  b.valid = true;
}

void transformFaceBasis(FaceTexBasis &b, const glm::dmat4 &m, const glm::dvec3 &oldNormal,
                        const glm::dvec3 &newNormal, const bool textureLock) {
  if (!b.valid)
    return;
  if (textureLock) {
    // UV(x) = dot(x, u) * k + o. For x' = A x + t with A orthogonal the locked axes are u' = A u and
    // o' = o - dot(t, u') * k.
    const glm::dmat3 a(m);
    const glm::dvec3 t(m[3]);
    glm::dvec3 u = a * glm::dvec3(b.uAxis);
    glm::dvec3 v = a * glm::dvec3(b.vAxis);
    const double su = glm::length(u), sv = glm::length(v);
    if (su > 1e-12 && sv > 1e-12) {
      // Scale shows up as axis length: fold it into the repeat so the texture stretches with the brush.
      u /= su;
      v /= sv;
      b.tilesPerUnitU = float(b.tilesPerUnitU / su);
      b.tilesPerUnitV = float(b.tilesPerUnitV / sv);
      // dot(A x, u/su) = dot(x, A^T u)/su; for orthogonal A this is exact.
      b.uAxis = glm::vec3(u);
      b.vAxis = glm::vec3(v);
      b.offsetU = float(b.offsetU - glm::dot(t, u) * b.tilesPerUnitU);
      b.offsetV = float(b.offsetV - glm::dot(t, v) * b.tilesPerUnitV);
    }
    return;
  }
  // World grid: Q = F_new * F_old^T, F = (u, v, n) of the default frames. Both have cross(u, v) == -n, so Q is
  // a proper rotation (identity while the normal keeps its dominant axis).
  if (glm::length(oldNormal) < 0.5 || glm::length(newNormal) < 0.5)
    return;
  glm::vec3 ou, ov, nu, nv;
  defaultTexAxes(oldNormal, ou, ov);
  defaultTexAxes(newNormal, nu, nv);
  if (ou == nu && ov == nv)
    return;
  auto frameNormal = [](const glm::vec3 &u, const glm::vec3 &v) { return -glm::cross(glm::dvec3(u), glm::dvec3(v)); };
  const glm::dmat3 fOld(glm::dvec3(ou), glm::dvec3(ov), frameNormal(ou, ov));
  const glm::dmat3 fNew(glm::dvec3(nu), glm::dvec3(nv), frameNormal(nu, nv));
  const glm::dmat3 q = fNew * glm::transpose(fOld);
  b.uAxis = glm::vec3(q * glm::dvec3(b.uAxis));
  b.vAxis = glm::vec3(q * glm::dvec3(b.vAxis));
}

void ConvexBrush::transform(const glm::dmat4 &m, const bool textureLock) {
  const glm::dmat3 a(m);
  const glm::dvec3 t(m[3]);
  const glm::dmat3 normalMat = glm::inverseTranspose(a);
  for (auto &f: faces) {
    const glm::dvec3 p = f.normal * f.distance; // a point on the plane
    const glm::dvec3 p2 = a * p + t;
    const glm::dvec3 oldNormal = f.normal;
    f.normal = glm::normalize(normalMat * f.normal);
    f.distance = glm::dot(f.normal, p2);
    transformFaceBasis(f.basis, m, oldNormal, f.normal, textureLock);
  }
  build();
}

namespace {

// Maps a shape built with Y up in [0,1]^3 into the box with the requested up axis.
glm::dvec3 place(const glm::dvec3 &unit, const glm::dvec3 &mn, const glm::dvec3 &mx, int up) {
  glm::dvec3 q;
  if (up == 1)
    q = unit;
  else if (up == 0)
    q = {unit.y, unit.x, unit.z}; // up along X
  else
    q = {unit.x, unit.z, unit.y}; // up along Z
  return mn + q * (mx - mn);
}

std::optional<ConvexBrush> hullOf(const std::vector<glm::dvec3> &unitPts, const glm::dvec3 &mn, const glm::dvec3 &mx,
                                  int up, const MaterialPtr &mat, double tpu) {
  std::vector<glm::dvec3> pts;
  pts.reserve(unitPts.size());
  for (const auto &p: unitPts)
    pts.push_back(place(p, mn, mx, up));
  return ConvexBrush::fromPoints(pts, mat, tpu);
}

} // namespace

std::vector<ConvexBrush> buildPrimitive(const PrimitiveKind kind, const glm::dvec3 &mn, const glm::dvec3 &mx,
                                        const PrimitiveParams &prm, const MaterialPtr &mat, const double tpu) {
  std::vector<ConvexBrush> out;
  const int up = std::clamp(prm.up, 0, 2);
  auto push = [&out](std::optional<ConvexBrush> b) {
    if (b)
      out.push_back(std::move(*b));
  };
  const double pi = 3.14159265358979323846;
  switch (kind) {
    case PrimitiveKind::Box:
      out.push_back(ConvexBrush::box(mn, mx, mat, tpu));
      break;
    case PrimitiveKind::Wedge:
      push(hullOf({{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 0}, {1, 1, 0}}, mn, mx, up, mat, tpu));
      break;
    case PrimitiveKind::Pyramid:
      push(hullOf({{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}, {0.5, 1, 0.5}}, mn, mx, up, mat, tpu));
      break;
    case PrimitiveKind::Cylinder:
    case PrimitiveKind::Cone:
    case PrimitiveKind::Spike: {
      const int n = std::clamp(prm.sides, 3, 64);
      std::vector<glm::dvec3> pts;
      for (int i = 0; i < n; ++i) {
        const double a = 2.0 * pi * (i + 0.5) / n;
        const glm::dvec3 ring(0.5 + 0.5 * std::cos(a), 0.0, 0.5 + 0.5 * std::sin(a));
        pts.push_back(ring);
        if (kind == PrimitiveKind::Cylinder)
          pts.push_back(ring + glm::dvec3(0, 1, 0));
      }
      if (kind != PrimitiveKind::Cylinder)
        pts.push_back({0.5, 1.0, 0.5});
      push(hullOf(pts, mn, mx, up, mat, tpu));
      break;
    }
    case PrimitiveKind::Sphere: {
      const int n = std::clamp(prm.sides, 4, 24);
      const int rings = std::max(2, n / 2);
      std::vector<glm::dvec3> pts;
      pts.push_back({0.5, 0.0, 0.5});
      pts.push_back({0.5, 1.0, 0.5});
      for (int r = 1; r < rings; ++r) {
        const double phi = pi * r / rings;
        for (int i = 0; i < n; ++i) {
          const double th = 2.0 * pi * (i + 0.5 * (r & 1)) / n;
          pts.push_back({0.5 + 0.5 * std::sin(phi) * std::cos(th), 0.5 - 0.5 * std::cos(phi),
                         0.5 + 0.5 * std::sin(phi) * std::sin(th)});
        }
      }
      push(hullOf(pts, mn, mx, up, mat, tpu));
      break;
    }
    case PrimitiveKind::Stairs: {
      // One brush per step, no overlaps: step s spans depth [s/n, (s+1)/n] of the climb (n boxes, n + 1 with the support).
      const int n = std::clamp(prm.steps, 1, 128);
      // Unit-space (climb, height, across) -> unit cube point for the direction.
      const int dir = std::clamp(prm.stairDirection, 0, 3);
      auto unitPt = [dir](double climb, double h, double across) -> glm::dvec3 {
        switch (dir) {
          case 0: return {climb, h, across};       // +X
          case 1: return {1.0 - climb, h, across}; // -X
          case 3: return {across, h, 1.0 - climb}; // -Z
          default: return {across, h, climb};      // +Z
        }
      };
      auto boxOf = [&](double c0, double c1, double h0, double h1) {
        const glm::dvec3 a = place(unitPt(c0, h0, 0.0), mn, mx, up);
        const glm::dvec3 b = place(unitPt(c1, h1, 1.0), mn, mx, up);
        out.push_back(ConvexBrush::box(glm::min(a, b), glm::max(a, b), mat, tpu));
      };
      for (int s = 0; s < n; ++s) {
        const double c0 = double(s) / n, c1 = double(s + 1) / n;
        const double top = double(s + 1) / n;
        const double bottom = prm.stairStyle == 0 ? 0.0 : double(s) / n;
        boxOf(c0, c1, bottom, top);
      }
      if (prm.stairStyle == 2 && n >= 2) {
        // Triangular support: slope from the floor at the back of the first step to the underside of the last.
        const double c0 = 1.0 / n;
        const double hTop = double(n - 1) / n;
        std::vector<glm::dvec3> pts;
        for (const double across: {0.0, 1.0}) {
          pts.push_back(unitPt(c0, 0.0, across));
          pts.push_back(unitPt(1.0, 0.0, across));
          pts.push_back(unitPt(1.0, hTop, across));
        }
        push(hullOf(pts, mn, mx, up, mat, tpu));
      }
      break;
    }
    case PrimitiveKind::Arch: {
      // A half-ring in the local XY plane, extruded along Z.
      const int n = std::clamp(prm.segments, 2, 64);
      const double t = std::clamp(prm.archThickness, 0.02, 0.5);
      const double span = std::clamp(prm.archDegrees, 10.0, 360.0) * pi / 180.0;
      const double start = pi * 0.5 - span * 0.5;
      for (int i = 0; i < n; ++i) {
        const double a0 = start + span * i / n;
        const double a1 = start + span * (i + 1) / n;
        std::vector<glm::dvec3> pts;
        for (const double a: {a0, a1})
          for (const double r: {0.5, 0.5 - t * 0.5})
            for (const double z: {0.0, 1.0})
              // Up to a half circle the arch fills the box height; wider arcs are centred to stay inside it.
              pts.push_back({0.5 + r * std::cos(a), span <= pi + 1e-9 ? 2.0 * r * std::sin(a) : 0.5 + r * std::sin(a), z});
        push(hullOf(pts, mn, mx, up, mat, tpu));
      }
      break;
    }
  }
  return out;
}

} // namespace eng
