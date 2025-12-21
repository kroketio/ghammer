#include "brush_select.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>

#include "face_graph.h"

namespace eng::sel {

namespace {

constexpr double kTol = 0.1; // 1 Source unit
// Faces within this fraction of the brush's biggest face count as its largest (both sides of a wall).
constexpr double kMainFace = 0.98;

bool vertical(const glm::dvec3 &n) { return std::fabs(n.y) < 0.3; }
bool upward(const glm::dvec3 &n) { return n.y > 0.7; }
bool downward(const glm::dvec3 &n) { return n.y < -0.7; }

struct Brushes {
  const FaceGraph &g;
  std::unordered_map<quint32, int> solidOf;     // object id -> solid
  std::vector<std::vector<int>> faces;          // solid -> graph nodes
  std::vector<double> maxArea;                  // solid -> its biggest face
  mutable std::vector<QString> dominant;        // solid -> material, lazily
  mutable std::vector<char> dominantDone;

  explicit Brushes(const FaceGraph &graph) : g(graph) {
    const int n = g.solidCount();
    faces.resize(size_t(n));
    maxArea.assign(size_t(n), 0.0);
    dominant.resize(size_t(n));
    dominantDone.assign(size_t(n), 0);
    for (int s = 0; s < n; ++s)
      solidOf[g.solidObjId(s)] = s;
    for (int i = 0; i < int(g.nodes.size()); ++i) {
      const FaceNode &f = g.nodes[size_t(i)];
      if (f.solid < 0 || f.solid >= n)
        continue;
      faces[size_t(f.solid)].push_back(i);
      maxArea[size_t(f.solid)] = std::max(maxArea[size_t(f.solid)], f.area);
    }
  }

  // The material on most of the visible faces (all faces when every one is covered).
  const QString &dominantMaterial(int s) const {
    if (!dominantDone[size_t(s)]) {
      dominantDone[size_t(s)] = 1;
      std::map<QString, double> area, hiddenArea;
      for (const int i: faces[size_t(s)]) {
        const FaceNode &f = g.nodes[size_t(i)];
        (f.hidden ? hiddenArea : area)[f.material] += f.area;
      }
      const auto &use = area.empty() ? hiddenArea : area;
      double best = -1.0;
      for (const auto &[m, a]: use)
        if (a > best) {
          best = a;
          dominant[size_t(s)] = m;
        }
    }
    return dominant[size_t(s)];
  }

  [[nodiscard]] glm::dvec3 extent(int s) const { return g.solidMax(s) - g.solidMin(s); }
  [[nodiscard]] glm::dvec3 centre(int s) const { return (g.solidMax(s) + g.solidMin(s)) * 0.5; }
};

bool sameSize(const glm::dvec3 &a, const glm::dvec3 &b) {
  // Height must match; the horizontal sides may be swapped (turned 90 degrees).
  if (std::fabs(a.y - b.y) > kTol)
    return false;
  const double a0 = std::min(a.x, a.z), a1 = std::max(a.x, a.z);
  const double b0 = std::min(b.x, b.z), b1 = std::max(b.x, b.z);
  return std::fabs(a0 - b0) <= kTol && std::fabs(a1 - b1) <= kTol;
}

} // namespace

BrushResult selectBrushes(const FaceGraph &g, const BrushQuery &q, ExposureCache &cache) {
  BrushResult r;
  const int n = g.solidCount();
  if (n == 0) {
    r.note = QStringLiteral("nothing matches");
    return r;
  }
  const Brushes b(g);

  std::vector<int> seeds;
  for (const quint32 id: q.seeds)
    if (const auto it = b.solidOf.find(id); it != b.solidOf.end())
      seeds.push_back(it->second);
  if (seeds.empty()) {
    r.note = QStringLiteral("pick a brush first");
    return r;
  }
  std::vector<QString> seedMaterials;
  for (const int s: seeds)
    seedMaterials.push_back(b.dominantMaterial(s));

  // Filters shared by every kind.
  auto usable = [&](int s) {
    if (!g.solidOccluder(s))
      return false;
    if (q.sameMaterial
        && std::find(seedMaterials.begin(), seedMaterials.end(), b.dominantMaterial(s)) == seedMaterials.end())
      return false;
    if (!q.hasRegion)
      return true;
    const glm::dvec3 c = b.centre(s);
    return glm::all(glm::greaterThanEqual(c, q.regionMin - kTol)) && glm::all(glm::lessThanEqual(c, q.regionMax + kTol));
  };

  std::vector<int> picked;
  switch (q.intent) {
    case BrushIntent::Wall:
    case BrushIntent::Floor:
    case BrushIntent::Ceiling: {
      auto orientationOk = [&](const glm::dvec3 &nrm) {
        return q.intent == BrushIntent::Wall ? vertical(nrm) : q.intent == BrushIntent::Floor ? upward(nrm) : downward(nrm);
      };
      Query fq;
      fq.intent = q.intent == BrushIntent::Wall ? Intent::Wall : q.intent == BrushIntent::Floor ? Intent::Floor : Intent::Ceiling;
      fq.exposure = q.exposure;
      fq.contiguous = q.contiguous;
      fq.sameMaterial = q.sameMaterial;
      fq.hasRegion = q.hasRegion;
      fq.regionMin = q.regionMin;
      fq.regionMax = q.regionMax;
      // Seed with the picked brushes' faces of that kind; the face rules drop those that do not qualify.
      for (const int s: seeds)
        for (const int i: b.faces[size_t(s)]) {
          const FaceNode &f = g.nodes[size_t(i)];
          if (!f.hidden && orientationOk(f.normal))
            fq.seeds.push_back(i);
        }
      if (fq.seeds.empty()) {
        r.note = QStringLiteral("the picked brush has no face of that kind");
        break;
      }
      const Result faces = select(g, fq, cache);
      // Only when such a face is one of the brush's largest: a wall's small top flush with a floor is not a floor.
      std::vector<char> in(size_t(n), 0);
      for (const int i: faces.faces) {
        const FaceNode &f = g.nodes[size_t(i)];
        if (f.solid >= 0 && f.area >= b.maxArea[size_t(f.solid)] * kMainFace)
          in[size_t(f.solid)] = 1;
      }
      for (int s = 0; s < n; ++s)
        if (in[size_t(s)] && usable(s))
          picked.push_back(s);
      if (picked.empty())
        r.note = faces.note.isEmpty() ? QStringLiteral("no brush of that kind") : faces.note;
      break;
    }
    case BrushIntent::Facade: {
      // The seed is the picked brush's biggest outdoor wall face.
      int seed = -1;
      double best = -1.0;
      for (const int pass: {0, 1}) {
        for (const int s: seeds)
          for (const int i: b.faces[size_t(s)]) {
            const FaceNode &f = g.nodes[size_t(i)];
            if (f.hidden || !vertical(f.normal) || f.area <= best)
              continue;
            if (pass == 0 && !isOutdoor(g, i, cache))
              continue;
            best = f.area;
            seed = i;
          }
        if (seed >= 0)
          break;
      }
      if (seed < 0) {
        r.note = QStringLiteral("pick a wall brush of the facade first");
        break;
      }
      Query fq;
      fq.intent = Intent::Facade;
      fq.allSides = q.allSides;
      fq.contiguous = q.contiguous;
      fq.sameMaterial = q.sameMaterial;
      fq.hasRegion = q.hasRegion;
      fq.regionMin = q.regionMin;
      fq.regionMax = q.regionMax;
      fq.seeds = {seed};
      const Result faces = select(g, fq, cache);
      // Each brush takes the kind of most of its facade area.
      std::vector<std::array<double, 6>> area(size_t(n), std::array<double, 6> {});
      std::vector<char> in(size_t(n), 0);
      for (size_t k = 0; k < faces.faces.size() && k < faces.types.size(); ++k) {
        const FaceNode &f = g.nodes[size_t(faces.faces[k])];
        if (f.solid < 0)
          continue;
        in[size_t(f.solid)] = 1;
        area[size_t(f.solid)][size_t(faces.types[k])] += f.area;
      }
      for (int s = 0; s < n; ++s) {
        if (!in[size_t(s)] || !usable(s))
          continue;
        const auto &a = area[size_t(s)];
        const int type = int(std::max_element(a.begin(), a.end()) - a.begin());
        ++r.typeCounts[size_t(type)];
        if (q.facadeTypes[size_t(type)])
          picked.push_back(s);
      }
      if (picked.empty())
        r.note = faces.note.isEmpty() ? QStringLiteral("nothing matches") : faces.note;
      break;
    }
    case BrushIntent::SameSize:
    case BrushIntent::SameMaterial:
    case BrushIntent::Touching: {
      std::vector<glm::dvec3> sizes;
      for (const int s: seeds)
        sizes.push_back(b.extent(s));
      auto accept = [&](int s) {
        if (!usable(s))
          return false;
        switch (q.intent) {
          case BrushIntent::SameSize:
            return std::any_of(sizes.begin(), sizes.end(), [&](const glm::dvec3 &e) { return sameSize(e, b.extent(s)); });
          case BrushIntent::SameMaterial:
            return std::find(seedMaterials.begin(), seedMaterials.end(), b.dominantMaterial(s)) != seedMaterials.end();
          default: return true;
        }
      };
      // Touching always grows from the picked brushes; the others only when Contiguous.
      if (q.contiguous || q.intent == BrushIntent::Touching) {
        std::vector<char> seen(size_t(n), 0);
        std::deque<int> todo;
        for (const int s: seeds)
          if (!seen[size_t(s)] && accept(s)) {
            seen[size_t(s)] = 1;
            todo.push_back(s);
          }
        while (!todo.empty()) {
          const int s = todo.front();
          todo.pop_front();
          picked.push_back(s);
          for (const int o: g.touchingSolids(s))
            if (!seen[size_t(o)] && accept(o)) {
              seen[size_t(o)] = 1;
              todo.push_back(o);
            }
        }
      } else {
        for (int s = 0; s < n; ++s)
          if (accept(s))
            picked.push_back(s);
      }
      if (picked.empty())
        r.note = QStringLiteral("nothing matches");
      break;
    }
    case BrushIntent::Count: break;
  }

  r.mn = glm::dvec3(1e300);
  r.mx = glm::dvec3(-1e300);
  for (const int s: picked) {
    r.objects.push_back(g.solidObjId(s));
    r.mn = glm::min(r.mn, g.solidMin(s));
    r.mx = glm::max(r.mx, g.solidMax(s));
  }
  std::sort(r.objects.begin(), r.objects.end());
  r.objects.erase(std::unique(r.objects.begin(), r.objects.end()), r.objects.end());
  if (r.objects.empty()) {
    r.mn = r.mx = glm::dvec3(0.0);
    if (r.note.isEmpty())
      r.note = QStringLiteral("nothing matches");
  }
  return r;
}

} // namespace eng::sel
