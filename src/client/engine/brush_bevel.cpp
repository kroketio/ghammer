#include "brush_bevel.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <glm/gtc/quaternion.hpp>

namespace eng::bevel {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFlatEps = 0.02;     // radians: faces this close to flat have no corner
constexpr double kNormalMatch = 1e-4; // two planes are one when their normals are this close
// Source-unit sizes (scaled by Params::sourceUnit).
constexpr double kVertexEps = 0.05;
constexpr double kPlaneEps = 0.01;
constexpr double kProbe = 0.5;
constexpr double kMinRadius = 0.25;
constexpr double kMaxRadius = 16384.0;
constexpr double kMinChord = 0.5;
constexpr double kMinVolume = 1e-3;
// A face narrower than this along an edge is a sliver (an earlier tiny bevel): the corner is between the faces
// either side of it.
constexpr double kSliver = 1.0;

struct Corner {
  glm::dvec3 a {0.0};
  glm::dvec3 d {0.0}; // unit, a -> b
  double len = 0.0;
  int faces[2] = {-1, -1};
  glm::dvec3 n[2];
  glm::dvec3 t[2];    // in each face, away from the edge
  double reach[2] = {0.0, 0.0};
};

struct Fill {
  int covered = 0;    // the face lying against the neighbour
  int neighbour = -1;
  double reach = 0.0; // the neighbour's face, away from the edge
};

struct Plan {
  Corner corner;
  Kind kind = Kind::Round;
  std::optional<Fill> fill;
  glm::dvec3 u {0.0}, v {0.0};
  double limit = 0.0;
  [[nodiscard]] double angle() const { return std::acos(std::clamp(glm::dot(u, v), -1.0, 1.0)); }
};

struct Arc {
  glm::dvec3 center {0.0};
  std::vector<glm::dvec3> points;
};

int vertexIndex(const std::vector<glm::dvec3> &poly, const glm::dvec3 &p, const double eps) {
  for (size_t i = 0; i < poly.size(); ++i)
    if (glm::length(poly[i] - p) <= eps)
      return int(i);
  return -1;
}

glm::dvec3 polygonCentre(const std::vector<glm::dvec3> &poly) {
  glm::dvec3 c(0.0);
  for (const auto &p: poly)
    c += p;
  return poly.empty() ? c : c / double(poly.size());
}

double reachOf(const std::vector<glm::dvec3> &poly, const glm::dvec3 &a, const glm::dvec3 &t) {
  double r = 0.0;
  for (const auto &v: poly)
    r = std::max(r, glm::dot(v - a, t));
  return r;
}

bool samePlane(const BrushFace &p, const BrushFace &q, const double eps) {
  return glm::dot(p.normal, q.normal) >= 1.0 - kNormalMatch && std::fabs(p.distance - q.distance) <= eps;
}

BrushFace flipped(const BrushFace &f) {
  BrushFace o = f;
  o.normal = -f.normal;
  o.distance = -f.distance;
  return o;
}

Issue cornerOf(const ConvexBrush &brush, const Edge &edge, const double su, Corner &c) {
  const double eps = kVertexEps * su;
  std::vector<int> found;
  for (size_t i = 0; i < brush.polygons.size(); ++i) {
    const auto &poly = brush.polygons[i];
    const int ia = vertexIndex(poly, edge.a, eps), ib = vertexIndex(poly, edge.b, eps);
    if (ia < 0 || ib < 0 || ia == ib)
      continue;
    const int n = int(poly.size());
    if ((ia + 1) % n == ib || (ib + 1) % n == ia)
      found.push_back(int(i));
  }
  if (found.size() != 2)
    return Issue::NotAnEdge;
  const auto &p1 = brush.polygons[size_t(found[0])], &p2 = brush.polygons[size_t(found[1])];
  // The corners exactly as the brush has them.
  const glm::dvec3 a = p1[size_t(vertexIndex(p1, edge.a, eps))];
  const glm::dvec3 b = p1[size_t(vertexIndex(p1, edge.b, eps))];
  const double len = glm::length(b - a);
  if (!std::isfinite(len) || len < 0.01 * su)
    return Issue::NotAnEdge;
  c.a = a;
  c.len = len;
  c.d = (b - a) / len;
  for (int k = 0; k < 2; ++k) {
    const int f = found[size_t(k)];
    c.faces[k] = f;
    c.n[k] = brush.faces[size_t(f)].normal;
    glm::dvec3 t = glm::cross(c.n[k], c.d);
    if (glm::length(t) < 1e-9)
      return Issue::NotAnEdge;
    t = glm::normalize(t);
    const auto &poly = k == 0 ? p1 : p2;
    if (glm::dot(polygonCentre(poly) - a, t) < 0.0)
      t = -t;
    c.t[k] = t;
    c.reach[k] = reachOf(poly, a, t);
  }
  // A thin strip along the edge: step over it so the bevelled corner is the brush's real one.
  bool stepped = false;
  for (int k = 0; k < 2; ++k) {
    if (c.reach[k] >= kSliver * su)
      continue;
    const auto &poly = brush.polygons[size_t(c.faces[k])];
    int beyond = -1;
    for (size_t i = 0; i < poly.size() && beyond < 0; ++i) {
      const glm::dvec3 p = poly[i], q = poly[(i + 1) % poly.size()];
      const glm::dvec3 e = q - p;
      if (glm::length(e) < eps || std::fabs(glm::dot(glm::normalize(e), c.d)) < 1.0 - 1e-6)
        continue;
      const glm::dvec3 w = p - c.a;
      if (glm::length(w - c.d * glm::dot(w, c.d)) < eps)
        continue; // the edge itself
      // The strip's far edge: the face on its other side.
      for (size_t j = 0; j < brush.polygons.size() && beyond < 0; ++j) {
        if (int(j) == c.faces[0] || int(j) == c.faces[1])
          continue;
        const auto &other = brush.polygons[j];
        const int ip = vertexIndex(other, p, eps), iq = vertexIndex(other, q, eps);
        const int n = int(other.size());
        if (ip >= 0 && iq >= 0 && ip != iq && ((ip + 1) % n == iq || (iq + 1) % n == ip))
          beyond = int(j);
      }
    }
    if (beyond < 0)
      continue;
    // Only a strip that cuts a corner off: the faces either side meet at a real corner (not a thin plate's two
    // sides) and the strip faces out between them.
    const glm::dvec3 ns = brush.faces[size_t(c.faces[k])].normal;
    const glm::dvec3 nb = brush.faces[size_t(beyond)].normal, no = c.n[1 - k];
    if (std::fabs(glm::dot(nb, no)) > 1.0 - 1e-3 || glm::dot(ns, nb) <= 0.0 || glm::dot(ns, no) <= 0.0)
      continue;
    c.faces[k] = beyond;
    c.n[k] = brush.faces[size_t(beyond)].normal;
    stepped = true;
  }
  if (stepped) {
    // Where the two faces' planes meet, over the same span.
    const BrushFace &f0 = brush.faces[size_t(c.faces[0])], &f1 = brush.faces[size_t(c.faces[1])];
    glm::dvec3 dir = glm::cross(f0.normal, f1.normal);
    if (glm::length(dir) < 1e-9)
      return Issue::Flat;
    dir = glm::normalize(dir);
    if (glm::dot(dir, c.d) < 0.0)
      dir = -dir;
    const double g = glm::dot(f0.normal, f1.normal);
    const double den = std::max(1.0 - g * g, 1e-12);
    const glm::dvec3 x = ((f0.distance - f1.distance * g) * f0.normal + (f1.distance - f0.distance * g) * f1.normal) / den;
    const glm::dvec3 a2 = x + dir * glm::dot(a - x, dir), b2 = x + dir * glm::dot(b - x, dir);
    c.a = a2;
    c.len = glm::length(b2 - a2);
    c.d = dir;
    if (c.len < 0.01 * su)
      return Issue::NotAnEdge;
    for (int k = 0; k < 2; ++k) {
      const auto &poly = brush.polygons[size_t(c.faces[k])];
      glm::dvec3 t = glm::cross(c.n[k], c.d);
      if (glm::length(t) < 1e-9)
        return Issue::NotAnEdge;
      t = glm::normalize(t);
      if (glm::dot(polygonCentre(poly) - c.a, t) < 0.0)
        t = -t;
      c.t[k] = t;
      c.reach[k] = reachOf(poly, c.a, t);
    }
  }
  const double theta = std::acos(std::clamp(glm::dot(c.t[0], c.t[1]), -1.0, 1.0));
  if (theta < kFlatEps || theta > kPi - kFlatEps)
    return Issue::Flat;
  return Issue::None;
}

std::optional<Fill> findFill(const ConvexBrush &brush, const Corner &c, const std::vector<ConvexBrush> &neighbours,
                             const double su) {
  const glm::dvec3 mid = c.a + c.d * (c.len * 0.5);
  for (int covered = 0; covered < 2; ++covered) {
    const BrushFace plane = flipped(brush.faces[size_t(c.faces[covered])]);
    // Just outside the face, just past the edge: inside the neighbour when it fills the corner's other side.
    const glm::dvec3 probe = mid + c.n[covered] * (kProbe * su) - c.t[covered] * (kProbe * su);
    for (size_t k = 0; k < neighbours.size(); ++k) {
      const ConvexBrush &g = neighbours[k];
      int face = -1;
      for (size_t f = 0; f < g.faces.size() && face < 0; ++f)
        if (samePlane(g.faces[f], plane, kPlaneEps * su))
          face = int(f);
      if (face < 0)
        continue;
      bool inside = true;
      for (const BrushFace &f: g.faces)
        inside = inside && f.signedDistance(probe) < -kPlaneEps * su;
      if (inside) {
        Fill fill;
        fill.covered = covered;
        fill.neighbour = int(k);
        fill.reach = reachOf(g.polygons[size_t(face)], c.a, -c.t[covered]);
        return fill;
      }
    }
  }
  return std::nullopt;
}

bool onPlane(const BrushFace &plane, const glm::dvec3 &p, const double su) {
  return std::fabs(plane.signedDistance(p)) <= kPlaneEps * su;
}

Issue plansOf(const ConvexBrush &brush, const std::vector<Edge> &edges, const std::vector<ConvexBrush> &neighbours,
              const bool inverse, const double su, std::vector<Plan> &out) {
  out.clear();
  if (edges.empty())
    return Issue::NoEdge;
  if (!brush.valid())
    return Issue::Failed;
  std::vector<Corner> corners;
  for (const Edge &e: edges) {
    Corner c;
    if (const Issue i = cornerOf(brush, e, su, c); i != Issue::None)
      return i;
    const double eps = kVertexEps * su;
    bool dup = false;
    for (const Corner &o: corners) {
      const glm::dvec3 p = o.a, q = o.a + o.d * o.len, r = c.a, s = c.a + c.d * c.len;
      dup = dup || (glm::length(p - r) < eps && glm::length(q - s) < eps)
            || (glm::length(p - s) < eps && glm::length(q - r) < eps);
    }
    if (!dup)
      corners.push_back(c);
  }
  for (size_t i = 0; i < corners.size(); ++i) {
    const Corner &c = corners[i];
    double reach[2] = {c.reach[0], c.reach[1]};
    // Two picked parallel edges on one face: each gets half of it.
    for (int k = 0; k < 2; ++k) {
      const BrushFace &plane = brush.faces[size_t(c.faces[k])];
      for (size_t j = 0; j < corners.size(); ++j) {
        const Corner &o = corners[j];
        if (i == j || std::fabs(glm::dot(c.d, o.d)) < 1.0 - 1e-6)
          continue;
        if (!onPlane(plane, o.a, su) || !onPlane(plane, o.a + o.d * o.len, su))
          continue;
        const double gap = glm::dot(o.a - c.a, c.t[k]);
        if (gap > kPlaneEps * su)
          reach[k] = std::min(reach[k], gap * 0.5);
      }
    }
    Plan plan;
    plan.corner = c;
    const std::optional<Fill> fill = inverse ? findFill(brush, c, neighbours, su) : std::nullopt;
    if (fill) {
      const int open = 1 - fill->covered;
      plan.kind = Kind::Fill;
      plan.fill = fill;
      plan.u = c.t[open];
      plan.v = -c.t[fill->covered];
      const double half = std::tan(std::acos(std::clamp(glm::dot(plan.u, plan.v), -1.0, 1.0)) * 0.5);
      plan.limit = half * std::min(reach[open], fill->reach);
    } else {
      plan.kind = inverse ? Kind::Cove : Kind::Round;
      plan.u = c.t[0];
      plan.v = c.t[1];
      const double half = std::tan(std::acos(std::clamp(glm::dot(c.t[0], c.t[1]), -1.0, 1.0)) * 0.5);
      plan.limit = half * std::min(reach[0], reach[1]);
    }
    out.push_back(plan);
  }
  return Issue::None;
}

// A true circular arc tangent to the two directions from the apex: n + 1 points.
Arc fillet(const glm::dvec3 &apex, const glm::dvec3 &u, const glm::dvec3 &v, const glm::dvec3 &d, const double r,
           const int n) {
  const double phi = std::acos(std::clamp(glm::dot(u, v), -1.0, 1.0));
  const double s = r / std::tan(phi * 0.5);
  glm::dvec3 bis = u + v;
  bis = glm::length(bis) > 1e-12 ? glm::normalize(bis) : glm::dvec3(0.0);
  Arc arc;
  arc.center = apex + bis * (r / std::sin(phi * 0.5));
  const glm::dvec3 p0 = apex + u * s, p1 = apex + v * s;
  const glm::dvec3 x = p0 - arc.center, y = p1 - arc.center;
  const double total = std::atan2(glm::dot(d, glm::cross(x, y)), glm::dot(x, y));
  arc.points.push_back(p0);
  for (int k = 1; k < n; ++k) {
    const glm::dquat q = glm::angleAxis(total * double(k) / double(n), d);
    arc.points.push_back(arc.center + q * x);
  }
  arc.points.push_back(p1);
  return arc;
}

glm::dvec3 reflect(const glm::dvec3 &p, const glm::dvec3 &origin, const glm::dvec3 &dir) {
  const glm::dvec3 foot = origin + dir * glm::dot(p - origin, dir);
  return foot * 2.0 - p;
}

// The arc mirrored across its chord: it bulges toward the apex.
Arc cove(const Arc &arc) {
  Arc out;
  if (arc.points.size() < 2)
    return out;
  const glm::dvec3 first = arc.points.front(), last = arc.points.back();
  const glm::dvec3 dir = glm::normalize(last - first);
  const size_t n = arc.points.size();
  for (size_t k = 0; k < n; ++k)
    out.points.push_back(k == 0 || k + 1 == n ? arc.points[k] : reflect(arc.points[k], first, dir));
  out.center = reflect(arc.center, first, dir);
  return out;
}

// The plane through `p` spanned by `dir` and `d`, `keep` on its inner side (ConvexBrush keeps n·x <= d).
std::optional<BrushFace> sidePlane(const glm::dvec3 &p, const glm::dvec3 &dir, const glm::dvec3 &d,
                                   const glm::dvec3 &keep) {
  glm::dvec3 n = glm::cross(d, dir);
  if (glm::length(n) < 1e-12)
    return std::nullopt;
  n = glm::normalize(n);
  if (glm::dot(n, keep - p) > 0.0)
    n = -n;
  BrushFace f;
  f.normal = n;
  f.distance = glm::dot(n, p);
  return f;
}

template <class Keep>
std::optional<std::vector<BrushFace>> chordPlanes(const std::vector<glm::dvec3> &pts, const glm::dvec3 &d,
                                                  const Keep &keep) {
  std::vector<BrushFace> out;
  for (size_t k = 0; k + 1 < pts.size(); ++k) {
    const glm::dvec3 mid = (pts[k] + pts[k + 1]) * 0.5;
    const auto p = sidePlane(pts[k], pts[k + 1] - pts[k], d, keep(mid));
    if (!p)
      return std::nullopt;
    out.push_back(*p);
  }
  return out;
}

void oblique(const glm::dvec3 &p, const glm::dvec3 &apex, const glm::dvec3 &u, const glm::dvec3 &v, double &a,
             double &b) {
  const double g = glm::dot(u, v);
  const double den = std::max(1.0 - g * g, 1e-12);
  const double x = glm::dot(p - apex, u), y = glm::dot(p - apex, v);
  a = (x - g * y) / den;
  b = (y - g * x) / den;
}

// The convex cells a cove splits the brush into (each a set of planes).
std::optional<std::vector<std::vector<BrushFace>>> coveCells(const Arc &arc, const glm::dvec3 &u, const glm::dvec3 &v,
                                                             const glm::dvec3 &d) {
  const auto &pts = arc.points;
  if (pts.size() < 2)
    return std::nullopt;
  const size_t n = pts.size() - 1, m = n / 2;
  const glm::dvec3 t1 = pts[0], t2 = pts[n];
  const glm::dvec3 centre = arc.center;
  const auto chords = chordPlanes(pts, d, [&centre](const glm::dvec3 &mid) { return mid + (mid - centre); });
  const auto insideU = sidePlane(t1, v, d, t1 - u);
  const auto insideV = sidePlane(t2, u, d, t2 - v);
  if (!chords || !insideU || !insideV)
    return std::nullopt;
  std::vector<std::vector<BrushFace>> cells;
  auto need = [](const std::optional<BrushFace> &p, std::vector<BrushFace> &into) {
    if (!p)
      return false;
    into.push_back(*p);
    return true;
  };
  for (size_t k = 0; k < n; ++k) {
    std::vector<BrushFace> planes;
    if (k < m) {
      if (k > 0 && !need(sidePlane(pts[k], u, d, pts[k] + v), planes))
        return std::nullopt;
      if (!need(sidePlane(pts[k + 1], u, d, pts[k + 1] - v), planes))
        return std::nullopt;
      planes.push_back(*insideU);
    } else {
      if (k + 1 < n && !need(sidePlane(pts[k + 1], v, d, pts[k + 1] + u), planes))
        return std::nullopt;
      if (!need(sidePlane(pts[k], v, d, pts[k] - u), planes))
        return std::nullopt;
      if (m > 0 && !need(sidePlane(pts[m], u, d, pts[m] + v), planes))
        return std::nullopt;
      planes.push_back(*insideV);
    }
    planes.push_back((*chords)[k]);
    cells.push_back(std::move(planes));
  }
  if (m > 0) {
    std::vector<BrushFace> mid;
    if (!need(sidePlane(pts[m], v, d, pts[m] + u), mid) || !need(sidePlane(pts[m], u, d, pts[m] + v), mid))
      return std::nullopt;
    mid.push_back(*insideU);
    mid.push_back(*insideV);
    cells.push_back(std::move(mid));
  }
  {
    std::vector<BrushFace> rest;
    if (!need(sidePlane(t1, v, d, t1 + u), rest))
      return std::nullopt;
    rest.push_back(*insideV);
    cells.push_back(std::move(rest));
  }
  {
    std::vector<BrushFace> rest;
    if (!need(sidePlane(t2, u, d, t2 + v), rest))
      return std::nullopt;
    cells.push_back(std::move(rest));
  }
  return cells;
}

// The quads an inner fill is made of, between the arc and the two faces.
std::vector<std::vector<glm::dvec3>> fillRings(const Arc &arc, const glm::dvec3 &apex, const glm::dvec3 &u,
                                               const glm::dvec3 &v) {
  std::vector<std::vector<glm::dvec3>> rings;
  const auto &pts = arc.points;
  if (pts.size() < 2)
    return rings;
  const size_t n = pts.size() - 1, m = n / 2;
  std::vector<std::pair<double, double>> ab;
  for (const auto &p: pts) {
    double a = 0.0, b = 0.0;
    oblique(p, apex, u, v, a, b);
    ab.emplace_back(a, b);
  }
  auto at = [&](const double a, const double b) { return apex + u * a + v * b; };
  for (size_t k = 0; k < n; ++k) {
    std::vector<glm::dvec3> ring;
    if (k < m)
      ring = {at(0.0, ab[k].second), pts[k], pts[k + 1], at(0.0, ab[k + 1].second)};
    else
      ring = {at(ab[k + 1].first, ab[m].second), at(ab[k].first, ab[m].second), pts[k], pts[k + 1]};
    std::vector<glm::dvec3> clean;
    for (const auto &p: ring)
      if (clean.empty() || glm::length(clean.back() - p) > 1e-4)
        clean.push_back(p);
    while (clean.size() > 1 && glm::length(clean.front() - clean.back()) <= 1e-4)
      clean.pop_back();
    if (clean.size() >= 3)
      rings.push_back(std::move(clean));
  }
  return rings;
}

void counterClockwise(std::vector<glm::dvec3> &ring, const glm::dvec3 &normal) {
  const glm::dvec3 c = polygonCentre(ring);
  glm::dvec3 area(0.0);
  for (size_t i = 0; i < ring.size(); ++i)
    area += glm::cross(ring[i] - c, ring[(i + 1) % ring.size()] - c);
  if (glm::dot(area, normal) < 0.0)
    std::reverse(ring.begin(), ring.end());
}

// A ring (counter-clockwise around `d`) extruded along `d` from lo to hi.
std::optional<ConvexBrush> prism(const std::vector<glm::dvec3> &ring, const glm::dvec3 &d, const double lo,
                                 const double hi, const BrushFace &attrs) {
  ConvexBrush b;
  for (size_t i = 0; i < ring.size(); ++i) {
    const glm::dvec3 e = ring[(i + 1) % ring.size()] - ring[i];
    glm::dvec3 n = glm::cross(e, d);
    if (glm::length(n) < 1e-12)
      continue;
    n = glm::normalize(n);
    BrushFace f = attrs;
    f.normal = n;
    f.distance = glm::dot(n, ring[i]);
    b.faces.push_back(f);
  }
  const double base = glm::dot(d, ring.front());
  BrushFace top = attrs, bottom = attrs;
  top.normal = d;
  top.distance = base + hi;
  bottom.normal = -d;
  bottom.distance = -(base + lo);
  b.faces.push_back(top);
  b.faces.push_back(bottom);
  if (!b.build())
    return std::nullopt;
  return b;
}

// `src` (on a face with normal `srcN`) carried onto a face with normal `dstN` meeting it at `edgePoint`: the
// axes turn with the face, the offsets keep the texel at the shared edge.
FaceTexBasis wrapBasis(const FaceTexBasis &src, const glm::dvec3 &srcN, const glm::dvec3 &dstN,
                       const glm::dvec3 &edgePoint) {
  FaceTexBasis out = src;
  const glm::dvec3 axis = glm::cross(srcN, dstN);
  const double s = glm::length(axis), c = glm::dot(srcN, dstN);
  if (s < 1e-12)
    return out;
  const glm::dquat q = glm::angleAxis(std::atan2(s, c), axis / s);
  const glm::dvec3 u = q * glm::dvec3(src.uAxis), v = q * glm::dvec3(src.vAxis);
  out.uAxis = glm::vec3(u);
  out.vAxis = glm::vec3(v);
  out.offsetU = float(double(src.offsetU)
                      + double(src.tilesPerUnitU) * (glm::dot(edgePoint, glm::dvec3(src.uAxis)) - glm::dot(edgePoint, u)));
  out.offsetV = float(double(src.offsetV)
                      + double(src.tilesPerUnitV) * (glm::dot(edgePoint, glm::dvec3(src.vAxis)) - glm::dot(edgePoint, v)));
  out.valid = true;
  return out;
}

// The round's faces: the edge face's texture wrapped from chord to chord.
void chordDressing(const BrushFace &start, const std::vector<glm::dvec3> &points, std::vector<BrushFace> &planes,
                   const MaterialPtr &material) {
  FaceTexBasis prev = start.basis;
  glm::dvec3 prevN = start.normal;
  for (size_t k = 0; k < planes.size(); ++k) {
    BrushFace &p = planes[k];
    p.basis = start.basis.valid ? wrapBasis(prev, prevN, p.normal, points[k]) : start.basis;
    p.material = material ? material : start.material;
    p.groups = start.groups;
    p.cut = false;
    prev = p.basis;
    prevN = p.normal;
  }
}

// Dresses a new face like the plane it lies on (the round, or a surrounding brush's face), else like the
// brush's most parallel face.
void dressNew(BrushFace &face, const std::vector<BrushFace> &templates, const std::vector<BrushFace> &references,
              const ConvexBrush &fallback, const double su) {
  for (const auto *list: {&templates, &references})
    for (const BrushFace &t: *list)
      if (samePlane(t, face, kPlaneEps * su)) {
        face.material = t.material;
        face.basis = t.basis;
        face.groups = t.groups;
        face.cut = false;
        return;
      }
  const BrushFace *best = fallback.mostParallelFace(face.normal);
  if (!best)
    return;
  face.material = best->material;
  face.groups = best->groups;
  face.cut = false;
  if (glm::dot(best->normal, face.normal) > 1.0 - 1e-6 && best->basis.valid)
    face.basis = best->basis;
  else
    face.basis = paraxialBasis(face.normal, best->basis.valid ? best->basis.tilesPerUnitU : 1.0);
}

// The part of `piece` inside every plane; nothing when empty.
std::optional<ConvexBrush> clipKeep(const ConvexBrush &piece, const std::vector<BrushFace> &planes, const double su) {
  ConvexBrush b = piece;
  for (const BrushFace &p: planes)
    b.faces.push_back(p);
  if (!b.build() || b.volume() <= kMinVolume * su * su * su * 1e-3)
    return std::nullopt;
  return b;
}

bool sharesOpposite(const ConvexBrush &a, const ConvexBrush &b, const double su) {
  for (const BrushFace &p: a.faces)
    for (const BrushFace &q: b.faces)
      if (samePlane(p, flipped(q), kPlaneEps * su))
        return true;
  return false;
}

// Two convex pieces whose union is convex, as one brush.
std::optional<ConvexBrush> mergePair(const ConvexBrush &a, const ConvexBrush &b, const double su) {
  glm::dvec3 amn, amx, bmn, bmx;
  a.bounds(amn, amx);
  b.bounds(bmn, bmx);
  const double eps = kPlaneEps * su;
  if (glm::any(glm::lessThan(amx + eps, bmn)) || glm::any(glm::lessThan(bmx + eps, amn)))
    return std::nullopt;
  if (!sharesOpposite(a, b, su))
    return std::nullopt;
  ConvexBrush m;
  for (const auto *pair: {&a, &b}) {
    const ConvexBrush &self = *pair;
    const ConvexBrush &other = pair == &a ? b : a;
    for (const BrushFace &p: self.faces) {
      bool shared = false;
      for (const BrushFace &q: other.faces)
        shared = shared || samePlane(p, flipped(q), eps);
      if (shared)
        continue;
      bool dup = false;
      for (const BrushFace &q: m.faces)
        dup = dup || samePlane(p, q, eps);
      if (!dup)
        m.faces.push_back(p);
    }
  }
  if (!m.build())
    return std::nullopt;
  for (const auto *piece: {&a, &b})
    for (const glm::dvec3 &v: piece->vertices())
      for (const BrushFace &f: m.faces)
        if (f.signedDistance(v) > eps)
          return std::nullopt;
  const double va = a.volume(), vb = b.volume(), vm = m.volume();
  double area = 0.0;
  for (const auto &poly: m.polygons)
    for (size_t k = 1; k + 1 < poly.size(); ++k)
      area += glm::length(glm::cross(poly[k] - poly[0], poly[k + 1] - poly[0])) * 0.5;
  if (std::fabs(vm - va - vb) > eps * area + 1e-9 * vm)
    return std::nullopt;
  return m;
}

std::vector<ConvexBrush> mergePieces(std::vector<ConvexBrush> list, const double su) {
  for (;;) {
    bool merged = false;
    for (size_t i = 0; i < list.size() && !merged; ++i)
      for (size_t j = i + 1; j < list.size() && !merged; ++j)
        if (auto m = mergePair(list[i], list[j], su)) {
          list[i] = std::move(*m);
          list.erase(list.begin() + std::ptrdiff_t(j));
          merged = true;
        }
    if (!merged)
      return list;
  }
}

bool solidOk(const ConvexBrush &b, const double su) { return b.valid() && b.volume() > kMinVolume * su * su * su; }

} // namespace

bool edgeFaces(const ConvexBrush &brush, const Edge &edge, BrushFace &f0, BrushFace &f1, const double sourceUnit) {
  Corner c;
  const Issue i = cornerOf(brush, edge, sourceUnit, c);
  if (i != Issue::None && i != Issue::Flat)
    return false;
  if (c.faces[0] < 0 || c.faces[1] < 0)
    return false;
  f0 = brush.faces[size_t(c.faces[0])];
  f1 = brush.faces[size_t(c.faces[1])];
  return true;
}

QString issueText(const Issue issue) {
  switch (issue) {
    case Issue::None: return {};
    case Issue::NoEdge: return QCoreApplication::translate("Bevel", "No edge to bevel");
    case Issue::NotAnEdge: return QCoreApplication::translate("Bevel", "Not an edge of the brush");
    case Issue::NoRadius: return QCoreApplication::translate("Bevel", "Radius too small");
    case Issue::Flat: return QCoreApplication::translate("Bevel", "Flat edge: no corner to bevel");
    case Issue::NoRoom: return QCoreApplication::translate("Bevel", "No room for a bevel here");
    case Issue::Failed: return QCoreApplication::translate("Bevel", "The bevel cannot be made");
  }
  return {};
}

QString kindText(const Kind kind, const int segments) {
  switch (kind) {
    case Kind::Round:
      return segments <= 1 ? QCoreApplication::translate("Bevel", "Chamfer")
                           : QCoreApplication::translate("Bevel", "Round");
    case Kind::Cove: return QCoreApplication::translate("Bevel", "Cove");
    case Kind::Fill: return QCoreApplication::translate("Bevel", "Inner fill");
  }
  return {};
}

int cappedSegments(const double radius, const int segments, const double cornerAngle, const double sourceUnit) {
  const int want = std::clamp(segments, 1, kMaxSegments);
  const double ratio = std::min(kMinChord * sourceUnit / (2.0 * radius), 1.0);
  const double step = 2.0 * std::asin(ratio);
  if (!(step > 0.0) || !std::isfinite(step))
    return want;
  const double n = std::floor((kPi - cornerAngle) / step);
  const int cap = std::isfinite(n) ? std::clamp(int(n), 1, kMaxSegments) : 1;
  return std::min(want, cap);
}

double limit(const ConvexBrush &brush, const std::vector<Edge> &edges, const std::vector<ConvexBrush> &neighbours,
             const bool inverse, const double sourceUnit, Issue *issue) {
  std::vector<Plan> plans;
  const Issue i = plansOf(brush, edges, inverse ? neighbours : std::vector<ConvexBrush> {}, inverse, sourceUnit, plans);
  if (issue)
    *issue = i;
  if (i != Issue::None)
    return 0.0;
  double l = kMaxRadius * sourceUnit;
  for (const Plan &p: plans)
    l = std::min(l, p.limit);
  return l;
}

std::vector<Kind> kinds(const ConvexBrush &brush, const std::vector<Edge> &edges,
                        const std::vector<ConvexBrush> &neighbours, const bool inverse, const double sourceUnit) {
  std::vector<Plan> plans;
  std::vector<Kind> out;
  if (plansOf(brush, edges, inverse ? neighbours : std::vector<ConvexBrush> {}, inverse, sourceUnit, plans)
      == Issue::None)
    for (const Plan &p: plans)
      out.push_back(p.kind);
  return out;
}

std::optional<glm::dvec3> opening(const ConvexBrush &brush, const Edge &edge,
                                  const std::vector<ConvexBrush> &neighbours, const bool inverse,
                                  const double sourceUnit) {
  std::vector<Plan> plans;
  if (plansOf(brush, {edge}, inverse ? neighbours : std::vector<ConvexBrush> {}, inverse, sourceUnit, plans)
          != Issue::None
      || plans.empty())
    return std::nullopt;
  const Plan &p = plans.front();
  glm::dvec3 dir = p.kind == Kind::Fill ? p.u + p.v : -(p.u + p.v);
  if (glm::length(dir) < 1e-9)
    return std::nullopt;
  return glm::normalize(dir);
}

Result bevel(const ConvexBrush &brush, const std::vector<Edge> &edges, const std::vector<ConvexBrush> &neighbours,
             const Params &params) {
  Result res;
  const double su = params.sourceUnit;
  if (!std::isfinite(params.radius) || params.radius < kMinRadius * su) {
    res.issue = Issue::NoRadius;
    return res;
  }
  const std::vector<ConvexBrush> none;
  const std::vector<ConvexBrush> &nb = params.inverse ? neighbours : none;
  std::vector<Plan> plans;
  if (const Issue i = plansOf(brush, edges, nb, params.inverse, su, plans); i != Issue::None) {
    res.issue = i;
    return res;
  }
  double lim = kMaxRadius * su;
  for (const Plan &p: plans)
    lim = std::min(lim, p.limit);
  if (lim < kMinRadius * su) {
    res.issue = Issue::NoRoom;
    return res;
  }
  const double radius = std::min({params.radius, lim, kMaxRadius * su});
  int segments = std::clamp(params.segments, 1, kMaxSegments);
  for (const Plan &p: plans)
    segments = std::min(segments, cappedSegments(radius, kMaxSegments, p.angle(), su));

  std::vector<BrushFace> templates, references;
  std::vector<ConvexBrush> pieces {brush};
  std::vector<ConvexBrush> added;
  // A face a clip adds, dressed at once (the original faces keep theirs).
  auto dressed = [&](std::vector<BrushFace> planes) {
    for (BrushFace &p: planes)
      dressNew(p, templates, {}, brush, su);
    return planes;
  };
  for (const Plan &plan: plans) {
    const Corner &c = plan.corner;
    const Arc arc = fillet(c.a, plan.u, plan.v, c.d, radius, segments);
    switch (plan.kind) {
      case Kind::Round: {
        const glm::dvec3 centre = arc.center;
        auto planes = chordPlanes(arc.points, c.d, [&centre](const glm::dvec3 &) { return centre; });
        if (!planes) {
          res.issue = Issue::Failed;
          return res;
        }
        chordDressing(brush.faces[size_t(c.faces[0])], arc.points, *planes, params.material);
        templates.insert(templates.end(), planes->begin(), planes->end());
        std::vector<ConvexBrush> next;
        for (const ConvexBrush &p: pieces)
          if (auto q = clipKeep(p, *planes, su))
            next.push_back(std::move(*q));
        pieces = std::move(next);
        break;
      }
      case Kind::Cove: {
        const Arc cv = cove(arc);
        const glm::dvec3 centre = cv.center;
        auto planes = chordPlanes(cv.points, c.d, [&centre](const glm::dvec3 &mid) { return mid + (mid - centre); });
        const auto cells = coveCells(cv, plan.u, plan.v, c.d);
        if (!planes || !cells) {
          res.issue = Issue::Failed;
          return res;
        }
        chordDressing(brush.faces[size_t(c.faces[0])], cv.points, *planes, params.material);
        templates.insert(templates.end(), planes->begin(), planes->end());
        std::vector<ConvexBrush> next;
        for (const ConvexBrush &p: pieces)
          for (const auto &cell: *cells)
            if (auto q = clipKeep(p, dressed(cell), su))
              next.push_back(std::move(*q));
        pieces = std::move(next);
        break;
      }
      case Kind::Fill: {
        if (!plan.fill || plan.fill->neighbour < 0 || size_t(plan.fill->neighbour) >= nb.size()) {
          res.issue = Issue::Failed;
          return res;
        }
        const ConvexBrush &other = nb[size_t(plan.fill->neighbour)];
        const int open = 1 - plan.fill->covered;
        const glm::dvec3 apex = c.a;
        auto planes = chordPlanes(arc.points, c.d, [&apex](const glm::dvec3 &) { return apex; });
        if (!planes) {
          res.issue = Issue::Failed;
          return res;
        }
        chordDressing(brush.faces[size_t(c.faces[open])], arc.points, *planes, params.material);
        templates.insert(templates.end(), planes->begin(), planes->end());
        references.insert(references.end(), brush.faces.begin(), brush.faces.end());
        references.insert(references.end(), other.faces.begin(), other.faces.end());
        // The fill ends where the two brushes end along the edge.
        const glm::dvec3 end = c.a + c.d * c.len;
        std::vector<BrushFace> ends;
        for (size_t i = 0; i < brush.faces.size(); ++i) {
          const BrushFace &g = brush.faces[i];
          if (int(i) == c.faces[0] || int(i) == c.faces[1] || std::fabs(glm::dot(g.normal, c.d)) <= 1e-3)
            continue;
          if (onPlane(g, c.a, su) || onPlane(g, end, su))
            ends.push_back(g);
        }
        for (const BrushFace &g: other.faces)
          if (std::fabs(glm::dot(g.normal, c.d)) > 0.5)
            ends.push_back(g);
        const double margin = radius * 4.0 + 64.0 * su;
        const BrushFace &attrs = brush.faces[size_t(c.faces[open])];
        std::vector<ConvexBrush> made;
        for (auto ring: fillRings(arc, apex, plan.u, plan.v)) {
          counterClockwise(ring, c.d);
          const auto p = prism(ring, c.d, -margin, c.len + margin, attrs);
          if (!p)
            continue;
          if (auto q = clipKeep(*p, ends, su))
            made.push_back(std::move(*q));
        }
        if (made.empty()) {
          res.issue = Issue::Failed;
          return res;
        }
        for (ConvexBrush &m: mergePieces(std::move(made), su))
          added.push_back(std::move(m));
        break;
      }
    }
  }
  if (pieces.empty()) {
    res.issue = Issue::Failed;
    return res;
  }
  if (pieces.size() > 1)
    pieces = mergePieces(std::move(pieces), su);
  // Every face of a fill brush is new: the round's, or a wall's it lies on.
  for (ConvexBrush &a: added) {
    for (BrushFace &f: a.faces)
      dressNew(f, templates, references, brush, su);
    a.build();
  }
  for (const ConvexBrush &p: pieces)
    if (!solidOk(p, su)) {
      res.issue = Issue::Failed;
      return res;
    }
  for (const ConvexBrush &a: added)
    if (!solidOk(a, su)) {
      res.issue = Issue::Failed;
      return res;
    }
  res.pieces = std::move(pieces);
  res.added = std::move(added);
  res.radius = radius;
  res.segments = segments;
  for (const Plan &p: plans)
    res.kinds.push_back(p.kind);
  return res;
}

} // namespace eng::bevel
