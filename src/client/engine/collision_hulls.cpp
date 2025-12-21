#include "collision_hulls.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/constants.h>

namespace eng::hulls {
namespace {

// Box3D's welding tolerances are tuned for ~1 m objects: points are mapped into [-1, 1] first.
struct Frame {
  glm::dvec3 centre {0.0};
  double scale = 1.0;  // input units per normalised unit
  [[nodiscard]] glm::vec3 in(const glm::vec3 &p) const { return glm::vec3((glm::dvec3(p) - centre) / scale); }
  [[nodiscard]] glm::vec3 out(const glm::vec3 &p) const { return glm::vec3(glm::dvec3(p) * scale + centre); }
};

std::vector<glm::vec3> hullPoints(const std::vector<glm::vec3> &points, const int maxVertices, double *volume) {
  if (volume)
    *volume = 0.0;
  if (points.size() < 4)
    return {};
  std::vector<b3Vec3> pts;
  pts.reserve(points.size());
  for (const glm::vec3 &p: points)
    pts.push_back(b3Vec3 {p.x, p.y, p.z});
  b3HullData *hull = b3CreateHull(pts.data(), int(pts.size()), std::clamp(maxVertices, 4, B3_MAX_HULL_VERTICES));
  if (!hull)
    return {};
  std::vector<glm::vec3> out;
  const b3Vec3 *hp = b3GetHullPoints(hull);
  out.reserve(size_t(hull->vertexCount));
  for (int i = 0; i < hull->vertexCount; ++i)
    out.emplace_back(hp[i].x, hp[i].y, hp[i].z);
  if (volume)
    *volume = double(hull->volume);
  b3DestroyHull(hull);
  return out;
}

struct Piece {
  std::vector<glm::vec3> points;  // hull vertices (normalised)
  double volume = 0.0;            // solid volume it stands for (its brushes)
  double hullVolume = 0.0;
  glm::vec3 mn {0.0f}, mx {0.0f};
  bool alive = true;
};

void bounds(Piece &p) {
  p.mn = glm::vec3(std::numeric_limits<float>::max());
  p.mx = glm::vec3(-std::numeric_limits<float>::max());
  for (const glm::vec3 &v: p.points) {
    p.mn = glm::min(p.mn, v);
    p.mx = glm::max(p.mx, v);
  }
}

float gap(const Piece &a, const Piece &b) {
  const glm::vec3 d = glm::max(glm::vec3(0.0f), glm::max(a.mn - b.mx, b.mn - a.mx));
  return glm::length(d);
}

} // namespace

std::vector<std::vector<glm::vec3>> collisionHulls(const std::vector<std::vector<glm::vec3>> &brushes,
                                                   const Options &opt, int *skipped) {
  if (skipped)
    *skipped = 0;
  glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
  for (const auto &b: brushes)
    for (const glm::vec3 &p: b) {
      mn = glm::min(mn, p);
      mx = glm::max(mx, p);
    }
  if (mn.x > mx.x)
    return {};
  Frame f;
  f.centre = (glm::dvec3(mn) + glm::dvec3(mx)) * 0.5;
  f.scale = std::max(1e-9, 0.5 * double(std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z})));

  std::vector<Piece> pieces;
  pieces.reserve(brushes.size());
  double total = 0.0;
  for (const auto &b: brushes) {
    std::vector<glm::vec3> local;
    local.reserve(b.size());
    for (const glm::vec3 &p: b)
      local.push_back(f.in(p));
    Piece pc;
    pc.points = hullPoints(local, B3_MAX_HULL_VERTICES, &pc.hullVolume);
    if (pc.points.empty() || pc.hullVolume <= 1e-9) {
      if (skipped)
        ++*skipped;
      continue;
    }
    pc.volume = pc.hullVolume;
    bounds(pc);
    total += pc.volume;
    pieces.push_back(std::move(pc));
  }
  if (pieces.empty())
    return {};

  // Greedy merging; pair costs are cached and recomputed only for pairs with the piece that grew.
  const int n = int(pieces.size());
  const int maxHulls = std::max(1, opt.maxHulls);
  const double budget = std::max(0.0, opt.tolerance) * total;
  constexpr float kNear = 0.1f;  // normalised units: 5 % of the group's size
  struct Pair {
    double cost = std::numeric_limits<double>::infinity();
    std::vector<glm::vec3> points;
    double hullVolume = 0.0;
    bool valid = false;
  };
  std::vector<Pair> cache(size_t(n) * size_t(n));
  const auto at = [n](int i, int j) { return size_t(std::min(i, j)) * size_t(n) + size_t(std::max(i, j)); };
  const auto evaluate = [&](int i, int j) -> Pair & {
    Pair &p = cache[at(i, j)];
    if (p.valid)
      return p;
    std::vector<glm::vec3> pts = pieces[size_t(i)].points;
    pts.insert(pts.end(), pieces[size_t(j)].points.begin(), pieces[size_t(j)].points.end());
    p.points = hullPoints(pts, B3_MAX_HULL_VERTICES, &p.hullVolume);
    p.cost = p.points.empty() ? std::numeric_limits<double>::infinity()
                              : p.hullVolume - (pieces[size_t(i)].volume + pieces[size_t(j)].volume);
    p.valid = true;
    return p;
  };
  int alive = n;
  while (alive > 1) {
    const bool forced = alive > maxHulls;
    int bi = -1, bj = -1;
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) {
      if (!pieces[size_t(i)].alive)
        continue;
      for (int j = i + 1; j < n; ++j) {
        if (!pieces[size_t(j)].alive)
          continue;
        if (!forced && gap(pieces[size_t(i)], pieces[size_t(j)]) > kNear)
          continue;
        const Pair &p = evaluate(i, j);
        if (p.cost < best) {
          best = p.cost;
          bi = i;
          bj = j;
        }
      }
    }
    if (bi < 0 || (!forced && best > budget))
      break;
    Pair merged = std::move(cache[at(bi, bj)]);
    Piece &a = pieces[size_t(bi)];
    a.points = std::move(merged.points);
    a.hullVolume = merged.hullVolume;
    a.volume += pieces[size_t(bj)].volume;
    bounds(a);
    pieces[size_t(bj)].alive = false;
    --alive;
    for (int k = 0; k < n; ++k) {
      cache[at(bi, k)].valid = false;
      cache[at(bj, k)].valid = false;
    }
  }

  std::vector<std::vector<glm::vec3>> out;
  for (const Piece &p: pieces) {
    if (!p.alive)
      continue;
    std::vector<glm::vec3> hull = hullPoints(p.points, opt.maxVertices, nullptr);
    if (hull.empty())
      hull = p.points;
    for (glm::vec3 &v: hull)
      v = f.out(v);
    out.push_back(std::move(hull));
  }
  return out;
}

} // namespace eng::hulls
