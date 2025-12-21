// Bevel mode (after HammerPro's bevel tool): the mode, picking, preview and commit; geometry is engine/brush_bevel.*.
// A hovered edge includes its collinear continuation on the mode's other brushes, so a corner bevels on both.

#include "vk_engine.h"

#include <cstring>

#include "vk_shared.h"
#include "client/engine/brush_bevel.h"
#include "client/engine/gameobject.h"
#include "client/engine/scene.h"
#include "client/engine/vertex_ops.h"

namespace vk {

namespace {

constexpr float kEdgePickPx = 8.0f;   // 2D views: an end-on edge within this
constexpr float kEdgeNearPx = 20.0f;  // 3D, off the brushes: the nearest edge within this
constexpr double kSourceUnit = 1.0 / double(Engine3D::kSourceUnitsPerWorldUnit); // world units
constexpr double kSameEps = 0.05 * kSourceUnit;

double pointSegmentPx(const glm::vec2 &p, const glm::vec2 &a, const glm::vec2 &b) {
  const glm::vec2 ab = b - a;
  const float len2 = glm::dot(ab, ab);
  const float t = len2 > 1e-9f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
  return glm::length(p - (a + ab * t));
}

template <class EdgeRef>
bool sameEdge(const EdgeRef &x, const EdgeRef &y) {
  return x.brush == y.brush
         && ((glm::length(x.a - y.a) < kSameEps && glm::length(x.b - y.b) < kSameEps)
             || (glm::length(x.a - y.b) < kSameEps && glm::length(x.b - y.a) < kSameEps));
}

bool samePlane(const eng::BrushFace &p, const eng::BrushFace &q) {
  return glm::dot(p.normal, q.normal) >= 1.0 - 1e-4 && std::fabs(p.distance - q.distance) <= 0.01 * kSourceUnit;
}

quint64 mixKey(quint64 h, const quint64 v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

quint64 mixDouble(const quint64 h, const double v) {
  quint64 bits;
  std::memcpy(&bits, &v, sizeof bits);
  return mixKey(h, bits);
}

} // namespace

bool Engine3D::enterBevelMode() {
  if (!m_scene)
    return false;
  std::vector<quint32> ids;
  std::vector<BevelEdgeRef> picked;
  if (m_editMode && !m_selectedVerts.empty()) {
    // Edit mode: edges with both corners selected.
    std::set<quint32> brushes;
    for (const VertexHandle &h: m_selectedVerts)
      brushes.insert(h.brush);
    for (const quint32 id: brushes)
      if (const auto o = m_scene->findById(id))
        if (const auto b = eng::ConvexBrush::fromObject(*o)) {
          ids.push_back(id);
          for (const auto &[a, e]: eng::brushEdges(*b))
            if (vertexSelected(id, glm::vec3(a)) && vertexSelected(id, glm::vec3(e)))
              picked.push_back({id, a, e});
        }
  } else {
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->isDerived() && !o->locked() && o->getMesh())
        ids.push_back(id);
  }
  if (ids.empty()) {
    emit toastRequested(tr("No brushes selected"));
    return false;
  }
  m_bevel.open = true;
  m_bevel.scopeIds = ids;
  m_bevel.scopeKey = ~0ull;
  m_bevel.picked = picked;
  m_bevel.hover.clear();
  m_bevel.previewKey = ~0ull;
  emit bevelChanged();
  requestRedraw();
  return true;
}

void Engine3D::leaveBevelMode() {
  if (!m_bevel.open)
    return;
  m_bevel.open = false;
  m_bevel.scopeIds.clear();
  m_bevel.scope.clear();
  m_bevel.scopeKey = ~0ull;
  m_bevel.picked.clear();
  m_bevel.hover.clear();
  m_bevel.preview.clear();
  m_bevel.previewKey = ~0ull;
  emit bevelChanged();
  requestRedraw();
}

const std::vector<Engine3D::BevelScopeBrush> &Engine3D::bevelScope() {
  quint64 key = mixKey(m_scene ? m_scene->structureVersion() : 0, eng::Mesh::globalGeometryEpoch());
  key = mixKey(key, eng::GameObject::globalTransformEpoch());
  if (key == m_bevel.scopeKey)
    return m_bevel.scope;
  m_bevel.scopeKey = key;
  m_bevel.scope.clear();
  if (!m_scene)
    return m_bevel.scope;
  for (const quint32 id: m_bevel.scopeIds) {
    const auto o = m_scene->findById(id);
    if (!o || o->hidden())
      continue;
    auto b = eng::ConvexBrush::fromObject(*o);
    if (!b)
      continue;
    BevelScopeBrush s;
    s.id = id;
    s.edges = eng::brushEdges(*b);
    s.brush = std::move(*b);
    m_bevel.scope.push_back(std::move(s));
  }
  return m_bevel.scope;
}

std::vector<Engine3D::BevelEdgeRef> Engine3D::bevelEdgeGroup(const BevelEdgeRef &edge) {
  std::vector<BevelEdgeRef> group {edge};
  const auto &scope = bevelScope();
  const BevelScopeBrush *own = nullptr;
  for (const auto &s: scope)
    if (s.id == edge.brush)
      own = &s;
  eng::BrushFace f0, f1;
  if (!own || !eng::bevel::edgeFaces(own->brush, {edge.a, edge.b}, f0, f1, kSourceUnit))
    return group;
  const glm::dvec3 d = glm::normalize(edge.b - edge.a);
  auto onLine = [&](const glm::dvec3 &p) {
    const glm::dvec3 w = p - edge.a;
    return glm::length(w - d * glm::dot(w, d)) < kSameEps;
  };
  // The corner continuing in another brush: a collinear edge between the same two planes, touching the group.
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &s: scope)
      for (const auto &[a, b]: s.edges) {
        const BevelEdgeRef cand {s.id, a, b};
        if (std::any_of(group.begin(), group.end(), [&](const BevelEdgeRef &g) { return sameEdge(g, cand); }))
          continue;
        if (std::fabs(glm::dot(glm::normalize(b - a), d)) < 1.0 - 1e-6 || !onLine(a) || !onLine(b))
          continue;
        // Overlapping or end to end with an edge of the group.
        const double ca = glm::dot(a - edge.a, d), cb = glm::dot(b - edge.a, d);
        bool touches = false;
        for (const BevelEdgeRef &g: group) {
          const double ga = glm::dot(g.a - edge.a, d), gb = glm::dot(g.b - edge.a, d);
          touches = touches
                    || (std::min(ca, cb) <= std::max(ga, gb) + kSameEps && std::max(ca, cb) >= std::min(ga, gb) - kSameEps);
        }
        if (!touches)
          continue;
        eng::BrushFace g0, g1;
        if (!eng::bevel::edgeFaces(s.brush, {a, b}, g0, g1, kSourceUnit))
          continue;
        if (!((samePlane(g0, f0) && samePlane(g1, f1)) || (samePlane(g0, f1) && samePlane(g1, f0))))
          continue;
        group.push_back(cand);
        grew = true;
      }
  }
  return group;
}

std::vector<Engine3D::BevelEdgeRef> Engine3D::bevelEdgeGroupAt3D(const float px, const float py) {
  if (!m_scene || !m_camera)
    return {};
  glm::vec3 ro, rd;
  makeRay(px, py, std::max(1.0f, float(m_lastViewSize.width())), std::max(1.0f, float(m_lastViewSize.height())), ro,
          rd);
  float tHit = 1e30f;
  glm::vec3 n(0.0f);
  quint32 hitObj = 0;
  if (!raycastScene(ro, rd, tHit, n, &hitObj))
    tHit = 1e30f;
  const glm::vec2 cursor(px, py);
  const glm::vec3 eye = m_camera->getPosition();
  const glm::vec3 fwd = m_camera->getDirection();

  // On a face of the mode's brushes: that face's edge nearest the pointer on screen, however far.
  if (hitObj && tHit < 1e29f)
    for (const auto &s: bevelScope()) {
      if (s.id != hitObj)
        continue;
      const glm::dvec3 hit = glm::dvec3(ro) + glm::dvec3(rd) * double(tHit);
      int face = -1;
      double bestDist = 0.05;
      for (size_t i = 0; i < s.brush.faces.size(); ++i) {
        const double dist = std::fabs(s.brush.faces[i].signedDistance(hit));
        if (dist < bestDist) {
          bestDist = dist;
          face = int(i);
        }
      }
      if (face < 0)
        break;
      const auto &poly = s.brush.polygons[size_t(face)];
      std::optional<BevelEdgeRef> best;
      double bestPx = 1e30;
      for (size_t k = 0; k < poly.size(); ++k) {
        const glm::dvec3 a = poly[k], b = poly[(k + 1) % poly.size()];
        if (glm::length(b - a) < kSameEps)
          continue;
        if (!eng::bevel::opening(s.brush, {a, b}, {}, false, kSourceUnit))
          continue; // flat: nothing to bevel
        const double d = pointSegmentPx(cursor, projectToScreen(glm::vec3(a)), projectToScreen(glm::vec3(b)));
        if (d < bestPx) {
          bestPx = d;
          best = BevelEdgeRef {s.id, a, b};
        }
      }
      if (best)
        return bevelEdgeGroup(*best);
      break;
    }
  struct Cand {
    BevelEdgeRef edge;
    double px;
    double depth;
    bool facing;
  };
  std::vector<Cand> found;
  for (const auto &s: bevelScope())
    for (const auto &[a, b]: s.edges) {
      const glm::vec3 fa(a), fb(b);
      if (glm::dot(fa - eye, fwd) <= 0.01f || glm::dot(fb - eye, fwd) <= 0.01f)
        continue;
      const double d = pointSegmentPx(cursor, projectToScreen(fa), projectToScreen(fb));
      if (d > kEdgeNearPx)
        continue;
      // The edge's point nearest the ray must not be hidden behind a surface.
      const glm::dvec3 e = b - a;
      const glm::dvec3 r(rd), w0 = a - glm::dvec3(ro);
      const double aa = glm::dot(r, r), bb = glm::dot(r, e), cc = glm::dot(e, e), dd = glm::dot(r, w0),
                   ee = glm::dot(e, w0);
      const double den = aa * cc - bb * bb;
      const double t = std::clamp(den > 1e-12 ? (aa * ee - bb * dd) / den : 0.0, 0.0, 1.0);
      const double depth = glm::dot(a + e * t - glm::dvec3(ro), r) / aa;
      if (depth > double(tHit) * 1.01 + 0.4)
        continue;
      const auto open = eng::bevel::opening(s.brush, {a, b}, {}, false, kSourceUnit);
      if (!open)
        continue; // flat: nothing to bevel
      const bool facing = glm::dot(*open, glm::dvec3(eye) - (a + b) * 0.5) > 0.0;
      found.push_back({{s.id, a, b}, d, depth, facing});
    }
  if (found.empty())
    return {};
  double best = 1e30;
  for (const Cand &c: found)
    best = std::min(best, c.px);
  const Cand *pick = nullptr;
  for (const Cand &c: found) {
    if (c.px > best + 1.0)
      continue;
    if (!pick || (c.facing && !pick->facing) || (c.facing == pick->facing && c.depth < pick->depth))
      pick = &c;
  }
  return bevelEdgeGroup(pick->edge);
}

void Engine3D::toggleBevelPick(const std::vector<BevelEdgeRef> &group) {
  if (group.empty())
    return;
  auto isPicked = [this](const BevelEdgeRef &e) {
    return std::any_of(m_bevel.picked.begin(), m_bevel.picked.end(), [&](const BevelEdgeRef &p) { return sameEdge(p, e); });
  };
  // Whole corner picked: dropped; else every part of it picked.
  const bool all = std::all_of(group.begin(), group.end(), isPicked);
  if (all) {
    m_bevel.picked.erase(std::remove_if(m_bevel.picked.begin(), m_bevel.picked.end(),
                                        [&](const BevelEdgeRef &p) {
                                          return std::any_of(group.begin(), group.end(),
                                                             [&](const BevelEdgeRef &g) { return sameEdge(p, g); });
                                        }),
                         m_bevel.picked.end());
  } else {
    for (const BevelEdgeRef &g: group)
      if (!isPicked(g))
        m_bevel.picked.push_back(g);
  }
  m_bevel.previewKey = ~0ull;
  emit bevelChanged();
  requestRedraw();
}

void Engine3D::hoverBevel3D(const float px, const float py) {
  if (!m_bevel.open)
    return;
  std::vector<BevelEdgeRef> group;
  if (px >= 0.0f && py >= 0.0f)
    group = bevelEdgeGroupAt3D(px, py);
  bool same = group.size() == m_bevel.hover.size();
  for (size_t i = 0; same && i < group.size(); ++i)
    same = sameEdge(group[i], m_bevel.hover[i]);
  if (same)
    return;
  m_bevel.hover = std::move(group);
  emit bevelChanged();
  requestRedraw();
}

bool Engine3D::pressBevel3D(const float px, const float py) {
  if (!m_bevel.open)
    return false;
  toggleBevelPick(bevelEdgeGroupAt3D(px, py));
  return true;
}

bool Engine3D::pickBevelEdge2D(const int plane, const glm::dvec2 &p, const double pxPerWorldUnit) {
  if (!m_bevel.open)
    return false;
  const ViewBasis bs = viewBasisFor(plane);
  const double tol = kEdgePickPx / std::max(1e-9, pxPerWorldUnit);
  std::optional<BevelEdgeRef> best;
  double bestScore = 1e300;
  for (const auto &s: bevelScope())
    for (const auto &[a, b]: s.edges) {
      if (std::fabs(glm::dot(glm::normalize(b - a), bs.depth)) < 0.999)
        continue; // only an edge seen end-on is a point here
      const double dist = glm::length(glm::dvec2(glm::dot(a, bs.right), glm::dot(a, bs.up)) - p);
      if (dist > tol)
        continue;
      // The nearest, then the one nearest the viewer.
      const double score = dist - std::max(glm::dot(a, bs.depth), glm::dot(b, bs.depth)) * 1e-6;
      if (score < bestScore) {
        bestScore = score;
        best = BevelEdgeRef {s.id, a, b};
      }
    }
  if (best)
    toggleBevelPick(bevelEdgeGroup(*best));
  return true;
}

void Engine3D::clearBevelEdges() {
  if (m_bevel.picked.empty())
    return;
  m_bevel.picked.clear();
  m_bevel.previewKey = ~0ull;
  emit bevelChanged();
  requestRedraw();
}

void Engine3D::toggleBevelInverse() { setBevelInverse(!m_bevel.inverse); }

void Engine3D::setBevelRadius(const double radius) {
  const double r = std::clamp(radius, 0.25 * kSourceUnit, 16384.0 * kSourceUnit);
  if (r == m_bevel.radius)
    return;
  m_bevel.radius = r;
  emit bevelChanged();
  requestRedraw();
}

void Engine3D::setBevelSegments(const int segments) {
  const int s = std::clamp(segments, 1, eng::bevel::kMaxSegments);
  if (s == m_bevel.segments)
    return;
  m_bevel.segments = s;
  emit bevelChanged();
  requestRedraw();
}

void Engine3D::setBevelInverse(const bool inverse) {
  if (inverse == m_bevel.inverse)
    return;
  m_bevel.inverse = inverse;
  emit bevelChanged();
  requestRedraw();
}

void Engine3D::setBevelSmooth(const bool on) {
  if (on == m_bevel.smooth)
    return;
  m_bevel.smooth = on;
  emit bevelChanged();
}

void Engine3D::setBevelCurrentMaterial(const bool on) {
  if (on == m_bevel.currentMaterial)
    return;
  m_bevel.currentMaterial = on;
  emit bevelChanged();
  requestRedraw();
}

std::vector<eng::ConvexBrush> Engine3D::bevelNeighbours(const quint32 brush, const eng::ConvexBrush &b) const {
  std::vector<eng::ConvexBrush> out;
  if (!m_scene)
    return out;
  glm::dvec3 mn, mx;
  b.bounds(mn, mx);
  mn -= glm::dvec3(kSourceUnit);
  mx += glm::dvec3(kSourceUnit);
  for (const auto &o: m_scene->gameObjects) {
    if (!o || o->stableId() == brush || o->isDerived() || o->hidden() || !o->getMesh())
      continue;
    const auto [lo, hi] = o->getWorldAABB();
    if (glm::any(glm::lessThan(glm::dvec3(hi), mn)) || glm::any(glm::greaterThan(glm::dvec3(lo), mx)))
      continue;
    if (auto nb = eng::ConvexBrush::fromObject(*o))
      out.push_back(std::move(*nb));
  }
  return out;
}

double Engine3D::bevelLimit() {
  refreshBevelPreview();
  return m_bevel.limit;
}

QString Engine3D::bevelStatus(bool *error) {
  refreshBevelPreview();
  if (error)
    *error = m_bevel.statusError;
  return m_bevel.status;
}

quint64 Engine3D::bevelKey(const std::vector<BevelEdgeRef> &edges) const {
  quint64 key = mixKey(0x5bd1e995ull, m_scene ? m_scene->structureVersion() : 0);
  key = mixKey(key, eng::Mesh::globalGeometryEpoch());
  key = mixKey(key, eng::GameObject::globalTransformEpoch());
  key = mixDouble(key, m_bevel.radius);
  key = mixKey(key, quint64(m_bevel.segments) << 1 | quint64(m_bevel.inverse));
  key = mixKey(key, quint64(m_bevel.currentMaterial));
  key = mixKey(key, qHash(m_currentMaterial));
  for (const BevelEdgeRef &e: edges) {
    key = mixKey(key, e.brush);
    key = mixDouble(mixDouble(mixDouble(key, e.a.x), e.a.y), e.a.z);
    key = mixDouble(mixDouble(mixDouble(key, e.b.x), e.b.y), e.b.z);
  }
  return key;
}

void Engine3D::refreshBevelPreview() {
  const quint64 key = bevelKey(m_bevel.picked);
  if (key == m_bevel.previewKey)
    return;
  m_bevel.previewKey = key;
  m_bevel.preview.clear();
  m_bevel.status.clear();
  m_bevel.statusError = false;
  m_bevel.limit = 0.0;
  if (m_bevel.picked.empty() || !m_scene)
    return;
  m_bevel.statusError = !computeBevel(m_bevel.picked, m_bevel.preview, m_bevel.status, m_bevel.limit,
                                      m_bevel.previewRadius, m_bevel.previewSegments);
}

bool Engine3D::computeBevel(const std::vector<BevelEdgeRef> &edges, std::vector<BevelOut> &out, QString &status,
                            double &limit, double &radius, int &segments) {
  out.clear();
  status.clear();
  limit = 0.0;
  if (edges.empty() || !m_scene)
    return false;
  std::map<quint32, std::vector<eng::bevel::Edge>> perBrush;
  for (const BevelEdgeRef &e: edges)
    perBrush[e.brush].push_back({e.a, e.b});
  struct Job {
    quint32 id;
    eng::ConvexBrush brush;
    std::vector<eng::bevel::Edge> edges;
    std::vector<eng::ConvexBrush> neighbours;
  };
  std::vector<Job> jobs;
  double lim = 1e300;
  for (auto &[id, list]: perBrush) {
    const auto o = m_scene->findById(id);
    const auto brush = o ? eng::ConvexBrush::fromObject(*o) : std::nullopt;
    if (!brush) {
      status = eng::bevel::issueText(eng::bevel::Issue::NotAnEdge);
      return false;
    }
    Job job {id, *brush, list, m_bevel.inverse ? bevelNeighbours(id, *brush) : std::vector<eng::ConvexBrush> {}};
    eng::bevel::Issue i = eng::bevel::Issue::None;
    lim = std::min(lim, eng::bevel::limit(job.brush, job.edges, job.neighbours, m_bevel.inverse, kSourceUnit, &i));
    if (i != eng::bevel::Issue::None) {
      status = eng::bevel::issueText(i);
      return false;
    }
    jobs.push_back(std::move(job));
  }
  limit = lim;
  eng::bevel::Params params;
  params.radius = std::min(m_bevel.radius, lim);
  params.segments = m_bevel.segments;
  params.inverse = m_bevel.inverse;
  params.sourceUnit = kSourceUnit;
  if (m_bevel.currentMaterial && !m_currentMaterial.isEmpty())
    params.material = TextureManager::instance().materialFor(m_currentMaterial);
  // One radius / segment count for every brush (the smallest allowed) so a corner across brushes stays one round.
  std::vector<eng::bevel::Result> results;
  for (int pass = 0; pass < 2; ++pass) {
    results.clear();
    int minSeg = params.segments;
    for (const Job &j: jobs) {
      results.push_back(eng::bevel::bevel(j.brush, j.edges, j.neighbours, params));
      if (!results.back().ok()) {
        status = eng::bevel::issueText(results.back().issue);
        return false;
      }
      minSeg = std::min(minSeg, results.back().segments);
    }
    if (minSeg == params.segments)
      break;
    params.segments = minSeg;
  }
  QStringList kinds;
  std::vector<eng::bevel::Kind> all;
  for (size_t i = 0; i < jobs.size(); ++i) {
    const bool changes = std::any_of(results[i].kinds.begin(), results[i].kinds.end(),
                                     [](eng::bevel::Kind k) { return k != eng::bevel::Kind::Fill; });
    out.push_back({jobs[i].id, results[i].pieces, results[i].added, changes});
    for (const eng::bevel::Kind k: results[i].kinds)
      if (std::find(all.begin(), all.end(), k) == all.end()) {
        all.push_back(k);
        kinds << eng::bevel::kindText(k, results[i].segments);
      }
  }
  radius = results.empty() ? 0.0 : results.front().radius;
  segments = params.segments;
  status = tr("%1 · R %2 · %n segment(s)", "", segments)
               .arg(kinds.join(QStringLiteral(" / ")))
               .arg(radius * double(kSourceUnitsPerWorldUnit), 0, 'g', 4);
  return true;
}

bool Engine3D::applyBevel() {
  if (!m_bevel.open || !m_scene)
    return false;
  if (m_bevel.picked.empty()) {
    emit toastRequested(tr("No edges picked"));
    return false;
  }
  refreshBevelPreview();
  if (m_bevel.statusError || m_bevel.preview.empty()) {
    emit toastRequested(m_bevel.status.isEmpty() ? eng::bevel::issueText(eng::bevel::Issue::Failed) : m_bevel.status);
    return false;
  }
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  for (const BevelOut &out: m_bevel.preview) {
    const auto src = m_scene->findById(out.brush);
    if (!src)
      continue;
    // A fill leaves the brush itself as it was.
    if (out.changesBrush) {
      remove.push_back(out.brush);
      for (const eng::ConvexBrush &p: out.pieces)
        if (auto o = objectFromBrush(p, src.get())) {
          if (m_bevel.smooth)
            o->setSmoothShading(true);
          add.push_back(o);
        }
    }
    for (const eng::ConvexBrush &a: out.added)
      if (auto o = objectFromBrush(a, src.get())) {
        if (m_bevel.smooth)
          o->setSmoothShading(true);
        add.push_back(o);
      }
  }
  if (add.empty() && remove.empty()) {
    emit toastRequested(tr("Nothing to bevel"));
    return false;
  }
  const QString label = tr("Bevel edges");
  const QString status = m_bevel.status;
  m_undoStack->beginMacro(label);
  if (m_editMode)
    clearVertexSelection();
  replaceObjects(remove, add, label); // the results are selected
  m_undoStack->endMacro();
  qInfo().noquote() << QStringLiteral("[bevel] %1 brush(es) -> %2 (%3)").arg(remove.size()).arg(add.size()).arg(status);
  leaveBevelMode();
  return true;
}

void Engine3D::appendBevelOverlay(const std::function<void(const glm::vec3 &, const glm::vec3 &)> &edge,
                                  std::vector<glm::vec3> &fill) {
  if (!m_bevel.open)
    return;
  // The faces a result adds, outlined, and filled when `filled`.
  auto corner = [&](const std::vector<BevelOut> &outs, const bool filled) {
    for (const BevelOut &out: outs) {
      const eng::ConvexBrush *orig = nullptr;
      for (const auto &sb: bevelScope())
        if (sb.id == out.brush)
          orig = &sb.brush;
      auto isNew = [&](const eng::BrushFace &f) {
        return !orig || std::none_of(orig->faces.begin(), orig->faces.end(),
                                     [&](const eng::BrushFace &g) { return samePlane(g, f); });
      };
      for (const auto *list: {&out.pieces, &out.added}) {
        if (list == &out.pieces && !out.changesBrush)
          continue;
        for (const eng::ConvexBrush &b: *list)
          for (size_t i = 0; i < b.polygons.size(); ++i) {
            if (list == &out.pieces && !isNew(b.faces[i]))
              continue;
            const auto &poly = b.polygons[i];
            for (size_t k = 0; k < poly.size(); ++k)
              edge(glm::vec3(poly[k]), glm::vec3(poly[(k + 1) % poly.size()]));
            if (filled)
              for (size_t k = 1; k + 1 < poly.size(); ++k) {
                fill.push_back(glm::vec3(poly[0]));
                fill.push_back(glm::vec3(poly[k]));
                fill.push_back(glm::vec3(poly[k + 1]));
              }
          }
      }
    }
  };
  // Picked edges: their round filled (the edges alone when it cannot be made).
  refreshBevelPreview();
  if (!m_bevel.preview.empty())
    corner(m_bevel.preview, true);
  else
    for (const BevelEdgeRef &e: m_bevel.picked)
      edge(glm::vec3(e.a), glm::vec3(e.b));
  // Hovered unpicked edge: its round, outlined.
  auto isPicked = [this](const BevelEdgeRef &e) {
    return std::any_of(m_bevel.picked.begin(), m_bevel.picked.end(), [&](const BevelEdgeRef &p) { return sameEdge(p, e); });
  };
  if (m_bevel.hover.empty() || std::all_of(m_bevel.hover.begin(), m_bevel.hover.end(), isPicked))
    return;
  const quint64 key = bevelKey(m_bevel.hover);
  if (key != m_bevel.hoverKey) {
    m_bevel.hoverKey = key;
    QString status;
    double limit = 0.0, radius = 0.0;
    int segments = 0;
    if (!computeBevel(m_bevel.hover, m_bevel.hoverPreview, status, limit, radius, segments))
      m_bevel.hoverPreview.clear();
  }
  if (!m_bevel.hoverPreview.empty())
    corner(m_bevel.hoverPreview, false);
  else
    for (const BevelEdgeRef &e: m_bevel.hover)
      edge(glm::vec3(e.a), glm::vec3(e.b));
}

} // namespace vk
