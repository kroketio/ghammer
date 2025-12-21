#include "physics_world.h"

#include <algorithm>

#include <QElapsedTimer>
#include <QMutex>

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/constants.h>

#include "collision_hulls.h"

namespace eng::phys {
namespace {

// b3CreateWorld / b3DestroyWorld must never run on two threads at once.
QMutex &worldLock() {
  static QMutex m;
  return m;
}

quint64 mix(quint64 h, quint64 v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

b3Vec3 v3(const glm::vec3 &p) { return b3Vec3 {p.x, p.y, p.z}; }

} // namespace

Cache::~Cache() {
  for (auto &[k, e]: m_brushes)
    if (e.hull)
      b3DestroyHull(e.hull);
}

b3HullData *Cache::brushHull(const HullInput &in) {
  auto it = m_brushes.find(in.key);
  if (it == m_brushes.end()) {
    Entry e;
    if (in.points.size() >= 4) {
      std::vector<b3Vec3> pts;
      pts.reserve(in.points.size());
      for (const glm::vec3 &p: in.points)
        pts.push_back(v3(p));
      e.hull = b3CreateHull(pts.data(), int(pts.size()), B3_MAX_HULL_VERTICES);
    }
    it = m_brushes.emplace(in.key, e).first;
  }
  it->second.used = true;
  return it->second.hull;
}

const std::vector<std::vector<glm::vec3>> &Cache::bodyHulls(const BodyInput &in, int *skipped) {
  quint64 key = mix(0x5eed, quint64(in.maxHulls));
  std::vector<quint64> keys;
  for (const HullInput &b: in.brushes)
    keys.push_back(b.key);
  std::sort(keys.begin(), keys.end());
  for (const quint64 k: keys)
    key = mix(key, k);
  auto it = m_bodies.find(key);
  if (it == m_bodies.end()) {
    std::vector<std::vector<glm::vec3>> pts;
    pts.reserve(in.brushes.size());
    for (const HullInput &b: in.brushes)
      pts.push_back(b.points);
    BodyEntry e;
    hulls::Options opt;
    opt.maxHulls = in.maxHulls;
    e.hulls = hulls::collisionHulls(pts, opt, &e.skipped);
    it = m_bodies.emplace(key, std::move(e)).first;
  }
  it->second.used = true;
  if (skipped)
    *skipped = it->second.skipped;
  return it->second.hulls;
}

void Cache::sweep() {
  for (auto it = m_brushes.begin(); it != m_brushes.end();) {
    if (!it->second.used) {
      if (it->second.hull)
        b3DestroyHull(it->second.hull);
      it = m_brushes.erase(it);
    } else {
      it->second.used = false;
      ++it;
    }
  }
  for (auto it = m_bodies.begin(); it != m_bodies.end();) {
    if (!it->second.used) {
      it = m_bodies.erase(it);
    } else {
      it->second.used = false;
      ++it;
    }
  }
}

struct World::Impl {
  b3WorldId world = b3_nullWorldId;
  std::vector<std::pair<quint32, b3BodyId>> bodies;
};

World::World() : m(std::make_unique<Impl>()) {}

World::~World() {
  QMutexLocker lock(&worldLock());
  if (b3World_IsValid(m->world))
    b3DestroyWorld(m->world);
}

std::unique_ptr<World> World::build(const Snapshot &snapshot, Cache &cache) {
  QElapsedTimer timer;
  timer.start();
  std::unique_ptr<World> w(new World());
  {
    QMutexLocker lock(&worldLock());
    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = b3Vec3 {0.0f, -9.81f, 0.0f};
    w->m->world = b3CreateWorld(&def);
  }
  const b3WorldId world = w->m->world;

  {
    b3BodyDef bd = b3DefaultBodyDef();
    const b3BodyId ground = b3CreateBody(world, &bd);
    b3ShapeDef sd = b3DefaultShapeDef();
    for (const HullInput &in: snapshot.statics) {
      if (const b3HullData *hull = cache.brushHull(in)) {
        b3CreateHullShape(ground, &sd, hull);
        ++w->m_stats.staticHulls;
      } else {
        ++w->m_stats.failed;
      }
    }
  }

  // Rigid bodies: simplified hulls relative to the entity origin; density chosen so the body weighs its mass.
  for (const BodyInput &in: snapshot.bodies) {
    int skipped = 0;
    const auto &hulls = cache.bodyHulls(in, &skipped);
    w->m_stats.failed += skipped;
    if (hulls.empty())
      continue;
    std::vector<b3HullData *> made;
    std::vector<std::pair<glm::vec3, glm::vec3>> edges;
    double volume = 0.0;
    for (const auto &h: hulls) {
      std::vector<b3Vec3> pts;
      pts.reserve(h.size());
      for (const glm::vec3 &p: h)
        pts.push_back(v3(p - in.origin));
      if (b3HullData *hd = b3CreateHull(pts.data(), int(pts.size()), B3_MAX_HULL_VERTICES)) {
        made.push_back(hd);
        volume += double(hd->volume);
        const b3HullHalfEdge *he = b3GetHullEdges(hd);
        const b3Vec3 *hp = b3GetHullPoints(hd);
        for (int e = 0; he && e < hd->edgeCount; ++e)
          if (e < he[e].twin) {
            const b3Vec3 a = hp[he[e].origin], c = hp[he[he[e].twin].origin];
            edges.emplace_back(glm::vec3(a.x, a.y, a.z) + in.origin, glm::vec3(c.x, c.y, c.z) + in.origin);
          }
      } else {
        ++w->m_stats.failed;
      }
    }
    if (made.empty() || volume <= 0.0)
      continue;
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = b3Pos {in.origin.x, in.origin.y, in.origin.z};
    const b3BodyId body = b3CreateBody(world, &bd);
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = float(std::max(1e-6, double(in.massKg) / volume));
    for (b3HullData *hd: made) {
      b3CreateHullShape(body, &sd, hd);  // the world copies the hull
      b3DestroyHull(hd);
    }
    w->m->bodies.emplace_back(in.entity, body);
    BodyCollision col;
    col.entity = in.entity;
    col.brushes = int(in.brushes.size());
    col.hulls = hulls;
    col.edges = std::move(edges);
    w->m_collision.push_back(std::move(col));
    ++w->m_stats.bodies;
    w->m_stats.bodyHulls += int(made.size());
  }
  cache.sweep();
  w->m_stats.ms = double(timer.nsecsElapsed()) / 1e6;
  return w;
}

void World::step(const double seconds) {
  const int steps = std::max(1, int(seconds * 60.0 + 0.5));
  for (int i = 0; i < steps; ++i)
    b3World_Step(m->world, 1.0f / 60.0f, 4);
}

std::vector<BodyPose> World::bodyPoses() const {
  std::vector<BodyPose> out;
  out.reserve(m->bodies.size());
  for (const auto &[entity, body]: m->bodies) {
    const b3Pos p = b3Body_GetPosition(body);
    const b3Quat q = b3Body_GetRotation(body);
    BodyPose pose;
    pose.entity = entity;
    pose.position = glm::vec3(float(p.x), float(p.y), float(p.z));
    pose.rotation = glm::normalize(glm::quat(q.s, q.v.x, q.v.y, q.v.z));
    out.push_back(pose);
  }
  return out;
}

std::vector<std::pair<quint32, glm::vec3>> World::bodyPositions() const {
  std::vector<std::pair<quint32, glm::vec3>> out;
  for (const auto &[entity, body]: m->bodies) {
    const b3Pos p = b3Body_GetPosition(body);
    out.emplace_back(entity, glm::vec3(float(p.x), float(p.y), float(p.z)));
  }
  return out;
}

} // namespace eng::phys
