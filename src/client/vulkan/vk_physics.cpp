// The map as a Box3D world (engine/physics_world.*), kept up to date in the background.
// Changes restart a 600 ms debounce; when it fires (no drag, gizmo edit or undo macro open) and the change-epoch key
// moved, the GUI thread snapshots per-brush world points (metres, cached per object) for one dedicated worker that
// owns the hull cache and builds the world. Results come back queued and are dropped when a newer build started.
// Read-only (overlay, stats, EDITOR_PHYSICSDUMP); the simulation (vk_physics_sim.cpp) builds its own.

#include "vk_engine.h"

#include <QHash>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <unordered_map>

#include "client/engine/entity.h"
#include "client/engine/gameobject.h"
#include "client/engine/mesh.h"
#include "client/engine/physics_world.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

quint64 mix(quint64 h, quint64 v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

} // namespace

struct Engine3D::PhysicsState {
  QThreadPool pool;  // one thread: builds are serialised, the cache is its alone
  std::shared_ptr<eng::phys::Cache> cache = std::make_shared<eng::phys::Cache>();
  std::shared_ptr<eng::phys::World> world;
  QTimer debounce;
  quint64 gen = 0;       // the newest build started
  quint64 key = 0;       // the scene key the newest build was started for
  int running = 0;
  bool enabled = true;
  // Per object: its world points (metres), valid for this signature.
  struct Points {
    quint64 sig = 0;
    std::vector<glm::vec3> points;
  };
  std::unordered_map<quint32, Points> points;
  std::vector<std::pair<double, std::function<void(const QString &)>>> checks;
};

void Engine3D::PhysicsStateDeleter::operator()(PhysicsState *s) const {
  if (!s)
    return;
  s->pool.waitForDone();
  s->world.reset();
  s->cache.reset();
  delete s;
}

const eng::phys::World *Engine3D::physicsWorld() const { return m_phys ? m_phys->world.get() : nullptr; }

bool Engine3D::physicsBusy() const { return m_phys && (m_phys->running > 0 || m_phys->debounce.isActive()); }

void Engine3D::setupPhysics() {
  m_phys.reset(new PhysicsState);
  m_phys->pool.setMaxThreadCount(1);
  m_phys->debounce.setSingleShot(true);
  m_phys->debounce.setInterval(600);
  connect(&m_phys->debounce, &QTimer::timeout, this, &Engine3D::startPhysicsBuild);
  for (auto sig: {&Engine3D::sceneChanged, &Engine3D::objectStateChanged, &Engine3D::entitiesChanged,
                  &Engine3D::modOutputsChanged})
    connect(this, sig, this, &Engine3D::schedulePhysics);
  // A simulation lasts while the document stays as it was (vk_physics_sim.cpp).
  connect(this, &Engine3D::undoRedoChanged, this, &Engine3D::checkPhysicsDocument);
  prefs::get().watch(QStringLiteral("Debug/Physics"), this, [this](const QVariant &v) {
    m_phys->enabled = v.toBool();
    if (!m_phys->enabled) {
      physicsStop();
      ++m_phys->gen;  // drop a build in flight
      m_phys->key = 0;
      m_phys->world.reset();
      emit physicsChanged();
    } else {
      schedulePhysics();
    }
  });
}

// Another level became active: rebuild from scratch (object ids of two levels can coincide). The hull cache is
// keyed by geometry and stays.
void Engine3D::resetPhysicsForLevel() {
  if (!m_phys)
    return;
  ++m_phys->gen;
  m_phys->key = 0;
  m_phys->world.reset();
  m_phys->points.clear();
  emit physicsChanged();
  schedulePhysics();
}

void Engine3D::schedulePhysics() {
  if (m_phys && m_phys->enabled && !m_preview)
    m_phys->debounce.start();
}

void Engine3D::startPhysicsBuild() {
  PhysicsState &ps = *m_phys;
  if (!ps.enabled || !m_scene)
    return;
  // Never mid-drag or inside a macro: the scene is in between and the work would be wasted.
  if (isGizmoDragging() || m_dragTarget != DragTarget::None || (m_undoStack && m_undoStack->inMacro())) {
    ps.debounce.start();
    return;
  }
  quint64 key = mix(quint64(reinterpret_cast<quintptr>(m_scene.data())), m_scene->structureVersion());
  key = mix(key, eng::Mesh::globalGeometryEpoch());
  key = mix(key, eng::GameObject::globalTransformEpoch());
  key = mix(key, eng::GameObject::visibilityEpoch());
  key = mix(key, m_entityEpoch);
  if (key == ps.key && (ps.world || ps.running)) {
    // Nothing changed since the last build.
    return;
  }
  ps.key = key;
  eng::phys::Snapshot snap = physicsSnapshot(nullptr);

  const quint64 gen = ++ps.gen;
  ++ps.running;
  QPointer<Engine3D> self(this);
  std::shared_ptr<eng::phys::Cache> cache = ps.cache;
  ps.pool.start([self, gen, cache, snap = std::move(snap)] {
    std::shared_ptr<eng::phys::World> world = eng::phys::World::build(snap, *cache);
    QMetaObject::invokeMethod(
        qApp,
        [self, gen, world]() mutable {
          if (!self || !self->m_phys)
            return;
          PhysicsState &st = *self->m_phys;
          --st.running;
          if (gen != st.gen || !st.enabled)
            return;  // a newer build is on its way (or physics was switched off)
          st.world = std::move(world);
          const eng::phys::Stats &s = st.world->stats();
          qInfo().noquote() << QStringLiteral("[physics] %1 static hulls, %2 bodies (%3 hulls), %4 failed, %5 ms")
                                   .arg(s.staticHulls)
                                   .arg(s.bodies)
                                   .arg(s.bodyHulls)
                                   .arg(s.failed)
                                   .arg(s.ms, 0, 'f', 1);
          emit self->physicsChanged();
          // EDITOR_PHYSICSDUMP waits for a finished build.
          if (!st.checks.empty() && st.running == 0 && !st.debounce.isActive()) {
            auto checks = std::move(st.checks);
            st.checks.clear();
            for (auto &[seconds, done]: checks)
              self->runPhysicsCheck(seconds, std::move(done));
          }
        },
        Qt::QueuedConnection);
  });
}

eng::phys::Snapshot Engine3D::physicsSnapshot(const std::set<quint32> *simulated) {
  PhysicsState &ps = *m_phys;
  // The snapshot (GUI thread): skip what the export skips or what does not
  // collide; rigid bodies grouped per entity (with `simulated`, only those
  // entities: every other rigid body is solid like the rest of the map).
  const float m = float(eng::kMetersPerWorldUnit);
  eng::phys::Snapshot snap;
  std::unordered_map<quint32, size_t> bodyIndex;
  std::unordered_map<quint32, PhysicsState::Points> keep;
  for (const auto &o: m_scene->gameObjects) {
    if (!o || o->hiddenFlag() || o->modReplaced() || o->ownerEntity())
      continue;
    const auto mesh = o->getMesh();
    if (!mesh || mesh->faceCount() == 0)
      continue;
    const eng::Entity *ent = o->brushEntity() ? entityById(o->brushEntity()) : nullptr;
    const QString cls = ent ? ent->classname : QString();
    if (cls.startsWith(QLatin1String("trigger_")) || cls == QLatin1String("func_illusionary"))
      continue;

    quint64 sig = mix(o->stableId(), quint64(reinterpret_cast<quintptr>(mesh.get())));
    sig = mix(sig, mesh->geometryVersion());
    sig = mix(sig, o->placementVersion());
    PhysicsState::Points pts;
    if (auto it = ps.points.find(o->stableId()); it != ps.points.end() && it->second.sig == sig) {
      pts = std::move(it->second);
    } else {
      pts.sig = sig;
      const glm::mat4 model = o->getModelMatrix();
      QHash<quint64, int> seen;
      for (const eng::Vertex &v: mesh->vertexData()) {
        const glm::vec3 w = glm::vec3(model * glm::vec4(v.position, 1.0f)) * m;
        // Unique within 0.1 mm.
        const auto q = [](float c) { return quint64(qint64(std::llround(double(c) * 1e4)) & 0x1fffff); };
        const quint64 h = q(w.x) | (q(w.y) << 21) | (q(w.z) << 42);
        if (seen.contains(h))
          continue;
        seen.insert(h, 1);
        pts.points.push_back(w);
      }
    }
    eng::phys::HullInput in;
    in.key = sig;
    in.points = pts.points;
    keep.emplace(o->stableId(), std::move(pts));
    if (ent && cls == eng::kRigidBodyClass && (!simulated || simulated->count(ent->id))) {
      auto [it, fresh] = bodyIndex.emplace(ent->id, snap.bodies.size());
      if (fresh) {
        eng::phys::BodyInput b;
        b.entity = ent->id;
        b.massKg = float(std::max(0.01, ent->num(QStringLiteral("mass"), 10.0)));
        b.maxHulls = std::clamp(int(std::lround(ent->num(QStringLiteral("max_hulls"), 4.0))), 1, 16);
        b.origin = ent->origin * m;
        snap.bodies.push_back(std::move(b));
      }
      snap.bodies[it->second].brushes.push_back(std::move(in));
    } else {
      snap.statics.push_back(std::move(in));
    }
  }
  ps.points = std::move(keep);
  return snap;
}

void Engine3D::runPhysicsCheck(const double seconds, std::function<void(const QString &)> done) {
  PhysicsState &ps = *m_phys;
  if (!ps.world || ps.running > 0 || ps.debounce.isActive()) {
    // Not built yet: run once the next build lands.
    ps.checks.emplace_back(seconds, std::move(done));
    if (!ps.debounce.isActive() && ps.running == 0)
      startPhysicsBuild();
    return;
  }
  std::shared_ptr<eng::phys::World> world = ps.world;
  ps.pool.start([world, seconds, done = std::move(done)] {
    const auto before = world->bodyPositions();
    world->step(seconds);
    const auto after = world->bodyPositions();
    QStringList lines;
    const eng::phys::Stats &s = world->stats();
    lines << QStringLiteral("[physics] check: %1 static hulls, %2 bodies, %3 s").arg(s.staticHulls).arg(s.bodies).arg(seconds);
    for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
      int hullCount = 0, brushes = 0;
      for (const auto &c: world->bodies())
        if (c.entity == before[i].first) {
          hullCount = int(c.hulls.size());
          brushes = c.brushes;
        }
      const glm::vec3 a = before[i].second, b = after[i].second;
      lines << QStringLiteral("[physics]   body %1: %2 brushes -> %3 hulls, (%4 %5 %6) m -> (%7 %8 %9) m")
                   .arg(before[i].first)
                   .arg(brushes)
                   .arg(hullCount)
                   .arg(a.x, 0, 'f', 2).arg(a.y, 0, 'f', 2).arg(a.z, 0, 'f', 2)
                   .arg(b.x, 0, 'f', 2).arg(b.y, 0, 'f', 2).arg(b.z, 0, 'f', 2);
    }
    const QString report = lines.join(QLatin1Char('\n'));
    QMetaObject::invokeMethod(qApp, [done, report] { done(report); }, Qt::QueuedConnection);
  });
}

} // namespace vk
