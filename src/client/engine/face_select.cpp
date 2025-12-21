#include "face_select.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

#include "face_graph.h"

namespace eng::sel {

namespace {

constexpr double kTol = 0.1;       // 1 Source unit
constexpr double kHeightTol = 0.05;

bool vertical(const glm::dvec3 &n) { return std::fabs(n.y) < 0.3; }
bool upward(const glm::dvec3 &n) { return n.y > 0.7; }
bool downward(const glm::dvec3 &n) { return n.y < -0.7; }

glm::dvec3 horizontal(const glm::dvec3 &n) {
  glm::dvec3 h(n.x, 0.0, n.z);
  const double l = glm::length(h);
  return l > 1e-9 ? h / l : glm::dvec3(1.0, 0.0, 0.0);
}

bool coplanar(const FaceNode &a, const FaceNode &b) {
  return glm::dot(a.normal, b.normal) > 0.999 && std::fabs(a.dist - b.dist) < kHeightTol;
}

void projectRange(const FaceNode &n, const glm::dvec3 &axis, double &lo, double &hi) {
  lo = 1e300;
  hi = -1e300;
  for (const auto &p: n.poly) {
    const double d = glm::dot(axis, p);
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
}

struct Ctx {
  const FaceGraph &g;
  const Query &q;
  ExposureCache &cache;
  std::vector<QString> materials; // seed materials when q.sameMaterial

  [[nodiscard]] bool usable(int i) const {
    const FaceNode &n = g.nodes[size_t(i)];
    if (n.hidden || !n.selectable)
      return false;
    if (q.sameMaterial && !materials.empty()
        && std::find(materials.begin(), materials.end(), n.material) == materials.end())
      return false;
    if (!q.hasRegion)
      return true;
    const glm::dvec3 &c = n.centroid;
    return glm::all(glm::greaterThanEqual(c, q.regionMin - kTol)) && glm::all(glm::lessThanEqual(c, q.regionMax + kTol));
  }

  [[nodiscard]] bool exposureOk(int i) const {
    if (q.exposure == Exposure::Any)
      return true;
    return isOutdoor(g, i, cache) == (q.exposure == Exposure::Outdoor);
  }

  // A doorway / window: the jamb we would enter meets a horizontal face (head or sill) that turns back onto our
  // plane (lintel or wall under the sill).
  [[nodiscard]] bool isOpening(int from, int into) const {
    const FaceNode &A = g.nodes[size_t(from)];
    for (const FaceLink &l: g.links[size_t(into)]) {
      if (l.kind != LinkKind::Concave)
        continue;
      const FaceNode &D = g.nodes[size_t(l.other)];
      if (vertical(D.normal))
        continue;
      for (const FaceLink &m: g.links[size_t(l.other)])
        if (m.other != into && m.kind == LinkKind::Convex && coplanar(g.nodes[size_t(m.other)], A))
          return true;
    }
    return false;
  }
};

template <typename Accept, typename Cross>
std::vector<int> flood(const FaceGraph &g, const std::vector<int> &seeds, Accept &&accept, Cross &&cross) {
  std::vector<char> seen(g.nodes.size(), 0);
  std::deque<int> todo;
  std::vector<int> out;
  for (const int s: seeds)
    if (s >= 0 && s < int(g.nodes.size()) && !seen[size_t(s)] && accept(s)) {
      seen[size_t(s)] = 1;
      todo.push_back(s);
    }
  while (!todo.empty()) {
    const int a = todo.front();
    todo.pop_front();
    out.push_back(a);
    for (const FaceLink &l: g.links[size_t(a)]) {
      const int b = l.other;
      if (seen[size_t(b)] || !accept(b) || !cross(a, b, l))
        continue;
      seen[size_t(b)] = 1;
      todo.push_back(b);
    }
  }
  return out;
}

void facadeSide(const Ctx &c, int seed, std::vector<int> &faces, std::vector<FacadeType> &types,
                std::vector<char> &assigned, std::vector<int> *nextSeeds) {
  const FaceGraph &g = c.g;
  const FaceNode &S = g.nodes[size_t(seed)];
  const glm::dvec3 nS = horizontal(S.normal);
  const double T = std::max(0.8, g.solidDepth(S.solid, nS));
  const double plane0 = glm::dot(nS, S.centroid);
  const double protrude = std::max(2.0 * T, 4.8);

  auto accept = [&](int i) {
    if (assigned[size_t(i)] || !c.usable(i))
      return false;
    const FaceNode &F = g.nodes[size_t(i)];
    if (glm::dot(F.normal, nS) < -0.3)
      return false;
    double lo, hi;
    projectRange(F, nS, lo, hi);
    return lo >= plane0 - T - kTol && hi <= plane0 + protrude + kTol;
  };
  std::vector<int> side;
  if (c.q.contiguous) {
    side = flood(g, {seed}, accept, [](int, int, const FaceLink &) { return true; });
  } else {
    for (int i = 0; i < int(g.nodes.size()); ++i)
      if (accept(i))
        side.push_back(i);
  }
  if (side.empty())
    return;

  std::map<long long, double> planes;
  for (const int i: side) {
    const FaceNode &F = g.nodes[size_t(i)];
    if (glm::dot(F.normal, nS) > 0.7)
      planes[std::llround(glm::dot(nS, F.centroid) / kHeightTol)] += F.area;
  }
  double mainPlane = plane0, bestArea = -1.0;
  for (const auto &[key, area]: planes)
    if (area > bestArea) {
      bestArea = area;
      mainPlane = double(key) * kHeightTol;
    }

  for (const int i: side) {
    const FaceNode &F = g.nodes[size_t(i)];
    assigned[size_t(i)] = 1;
    const double d = glm::dot(nS, F.centroid) - mainPlane;
    FacadeType t;
    if (glm::dot(F.normal, nS) > 0.7)
      t = d > kTol ? FacadeType::Trim : d < -kTol ? FacadeType::Recessed : FacadeType::Wall;
    else if (d < -kTol)
      t = FacadeType::Reveal;
    else if (upward(F.normal))
      t = FacadeType::Ledge;
    else if (downward(F.normal))
      t = FacadeType::Soffit;
    else
      t = FacadeType::Trim;
    faces.push_back(i);
    types.push_back(t);
  }

  if (!nextSeeds)
    return;
  for (const int i: side)
    for (const FaceLink &l: g.links[size_t(i)]) {
      const int b = l.other;
      if (assigned[size_t(b)] || l.kind != LinkKind::Convex || !c.usable(b))
        continue;
      const FaceNode &F = g.nodes[size_t(b)];
      if (!vertical(F.normal) || std::fabs(glm::dot(horizontal(F.normal), nS)) > 0.7)
        continue;
      if (isOutdoor(g, b, c.cache))
        nextSeeds->push_back(b);
    }
}

} // namespace

bool isOutdoor(const FaceGraph &g, const int node, ExposureCache &cache) {
  if (node < 0 || node >= int(g.nodes.size()))
    return false;
  if (cache.v.size() != g.nodes.size())
    cache.v.assign(g.nodes.size(), -1);
  int8_t &slot = cache.v[size_t(node)];
  if (slot >= 0)
    return slot == 1;
  const FaceNode &n = g.nodes[size_t(node)];
  const glm::dvec3 p = n.centroid + n.normal * (FaceGraph::kEps * 5.0);
  const double maxT = glm::length(g.mx - g.mn) * 2.0 + 10.0;
  const glm::dvec3 up(0.0, 1.0, 0.0);
  bool outdoor;
  if (upward(n.normal)) {
    outdoor = !g.raycast(p, up, maxT);
  } else if (downward(n.normal)) {
    outdoor = !g.raycast(p, -up, maxT);
  } else if (!g.raycast(p, up, maxT)) {
    outdoor = true;
  } else {
    const glm::dvec3 h = horizontal(n.normal);
    int escapes = 0;
    for (const double deg: {0.0, 35.0, -35.0}) {
      const double r = glm::radians(deg);
      const glm::dvec3 d(h.x * std::cos(r) - h.z * std::sin(r), 0.0, h.x * std::sin(r) + h.z * std::cos(r));
      escapes += g.raycast(p, d, maxT) ? 0 : 1;
    }
    outdoor = escapes >= 2;
  }
  slot = outdoor ? 1 : 0;
  return outdoor;
}

QString facadeTypeName(const FacadeType t) {
  switch (t) {
    case FacadeType::Wall: return QStringLiteral("Wall");
    case FacadeType::Recessed: return QStringLiteral("Recessed");
    case FacadeType::Reveal: return QStringLiteral("Reveals");
    case FacadeType::Trim: return QStringLiteral("Trim");
    case FacadeType::Ledge: return QStringLiteral("Ledges");
    case FacadeType::Soffit: return QStringLiteral("Soffits");
    default: return {};
  }
}

Result select(const FaceGraph &g, const Query &q, ExposureCache &cache) {
  Result r;
  if (g.nodes.empty())
    return r;
  if (cache.v.size() != g.nodes.size())
    cache.v.assign(g.nodes.size(), -1);
  Ctx c {g, q, cache, {}};
  std::vector<int> seeds;
  for (const int s: q.seeds)
    if (s >= 0 && s < int(g.nodes.size())) {
      seeds.push_back(s);
      const QString &m = g.nodes[size_t(s)].material;
      if (std::find(c.materials.begin(), c.materials.end(), m) == c.materials.end())
        c.materials.push_back(m);
    }
  const bool walk = q.contiguous && !seeds.empty();

  auto everyone = [&](auto &&accept) {
    std::vector<int> out;
    for (int i = 0; i < int(g.nodes.size()); ++i)
      if (accept(i))
        out.push_back(i);
    return out;
  };

  switch (q.intent) {
    case Intent::Wall: {
      auto accept = [&](int i) { return c.usable(i) && vertical(g.nodes[size_t(i)].normal) && c.exposureOk(i); };
      if (walk) {
        r.faces = flood(g, seeds, accept, [&](int a, int b, const FaceLink &l) {
          return q.exposure != Exposure::Indoor || l.kind != LinkKind::Convex || !c.isOpening(a, b);
        });
        if (r.faces.empty())
          r.note = q.exposure == Exposure::Outdoor ? QStringLiteral("the picked face is not an outdoor wall")
                                                   : QStringLiteral("the picked face is not an indoor wall");
      } else {
        r.faces = everyone(accept);
      }
      break;
    }
    case Intent::Floor:
    case Intent::Ceiling: {
      const bool floor = q.intent == Intent::Floor;
      auto accept = [&](int i) {
        const glm::dvec3 &n = g.nodes[size_t(i)].normal;
        return c.usable(i) && (floor ? upward(n) : downward(n)) && (!floor || c.exposureOk(i));
      };
      if (walk) {
        r.faces = flood(g, seeds, accept, [&](int a, int b, const FaceLink &) {
          return glm::dot(g.nodes[size_t(a)].normal, g.nodes[size_t(b)].normal) > 0.95;
        });
        if (r.faces.empty())
          r.note = floor ? QStringLiteral("the picked face is not a floor of that kind")
                         : QStringLiteral("the picked face is not a ceiling");
      } else if (!seeds.empty()) {
        std::vector<double> heights;
        for (const int s: seeds)
          heights.push_back(g.nodes[size_t(s)].centroid.y);
        r.faces = everyone([&](int i) {
          if (!accept(i))
            return false;
          const double y = g.nodes[size_t(i)].centroid.y;
          return std::any_of(heights.begin(), heights.end(), [y](double h) { return std::fabs(h - y) < kHeightTol; });
        });
      } else {
        r.faces = everyone(accept);
      }
      break;
    }
    case Intent::Group: {
      auto accept = [&](int i) { return c.usable(i) && facegroup::contains(g.nodes[size_t(i)].groups, q.groupId); };
      if (q.groupId == 0)
        r.note = QStringLiteral("pick a group");
      else if (walk)
        r.faces = flood(g, seeds, accept, [](int, int, const FaceLink &) { return true; });
      else
        r.faces = everyone(accept);
      break;
    }
    case Intent::Facade: {
      int seed = -1;
      for (const int s: seeds)
        if (c.usable(s) && vertical(g.nodes[size_t(s)].normal)) {
          seed = s;
          break;
        }
      if (seed < 0) {
        r.note = QStringLiteral("pick a wall face of the facade first");
        break;
      }
      std::vector<char> assigned(g.nodes.size(), 0);
      std::vector<int> next;
      facadeSide(c, seed, r.faces, r.types, assigned, q.allSides ? &next : nullptr);
      for (int sides = 1; q.allSides && !next.empty() && sides < 64;) {
        const int s = next.back();
        next.pop_back();
        if (assigned[size_t(s)])
          continue;
        facadeSide(c, s, r.faces, r.types, assigned, &next);
        ++sides;
      }
      break;
    }
  }

  if (q.intent != Intent::Facade)
    std::sort(r.faces.begin(), r.faces.end());
  r.mn = glm::dvec3(1e300);
  r.mx = glm::dvec3(-1e300);
  for (const int i: r.faces) {
    r.mn = glm::min(r.mn, g.nodes[size_t(i)].mn);
    r.mx = glm::max(r.mx, g.nodes[size_t(i)].mx);
  }
  if (r.faces.empty()) {
    r.mn = r.mx = glm::dvec3(0.0);
    if (r.note.isEmpty())
      r.note = QStringLiteral("nothing matches");
  }
  return r;
}

} // namespace eng::sel
