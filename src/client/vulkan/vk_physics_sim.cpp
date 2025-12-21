// The editor's physics simulation: Play / Pause / Reset / Apply.
// Play builds its own Box3D world from physicsSnapshot (vk_physics.cpp) on the GUI thread and steps it at 60 Hz.
// While running, moving bodies are drawn through one rigid transform per brush (m_physPose, world units, after the
// brush's own model), read by the object blocks, face bases, culling, picking, the 2D wire and the shadow keys; the
// level is untouched and editing is off.
// Pause writes the bodies into the level (one undo step, "Simulate physics"). A body is "physics dirty" while its
// brushes are not its originals: Reset puts them back (one step), Apply keeps the new ones; undoing Pause cleans it.
// A hand edit while paused only makes the world stale: the next Play rebuilds it (bodies start at rest).

#include "vk_engine.h"

#include <QTimer>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "client/engine/entity.h"
#include "client/engine/gameobject.h"
#include "client/engine/physics_world.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

struct Engine3D::PhysicsSim {
  std::unique_ptr<eng::phys::Cache> cache; // before the world: it goes last
  std::unique_ptr<eng::phys::World> world;
  bool running = false;
  bool selectedOnly = false;
  bool worldStale = false; // the level changed since the world was built
  struct Body {
    quint32 entity = 0;
    std::vector<quint32> brushes; // the body's brushes in the level now
    // What Reset puts back: the brushes the body had before it was first
    // simulated (kept alive here; the Pause step holds them too).
    std::vector<std::shared_ptr<eng::GameObject>> originals;
    std::set<quint32> originalIds;
    glm::dvec3 baseOrigin {0.0}; // metres: the body origin in the level's pose
    glm::dquat baseRotation {1.0, 0.0, 0.0, 0.0};
    glm::dvec3 position {0.0}; // now (metres)
    glm::dquat rotation {1.0, 0.0, 0.0, 0.0};
    glm::dmat4 pose {1.0}; // level pose -> simulated pose (world units)
    bool moving = false;   // pose is not the identity
    std::vector<glm::dvec3> trail; // where its origin has been (world units)
  };
  std::vector<Body> bodies;
  QTimer timer;
  QElapsedTimer clock;
  double pending = 0.0; // seconds not stepped yet (whole 1/60 s steps)
  quint64 docRevision = 0;
};

void Engine3D::PhysicsSimDeleter::operator()(PhysicsSim *s) const { delete s; }

namespace {

constexpr double kStep = 1.0 / 60.0;
constexpr double kTrailSpacing = 0.4; // world units between trail dots (4 Source units)
constexpr size_t kTrailMax = 4000;

bool isRigidBody(const eng::Entity *e) { return e && e->classname == eng::kRigidBodyClass; }

// The rigid body entities a selection names (the entity itself or its brushes).
std::set<quint32> selectedRigidBodies(const Engine3D &e, const eng::Scene &scene) {
  std::set<quint32> out;
  for (const quint32 id: e.selectedEntities())
    if (isRigidBody(e.entityById(id)))
      out.insert(id);
  for (const quint32 id: e.selectedObjects())
    if (const auto o = scene.findById(id); o && o->brushEntity() && isRigidBody(e.entityById(o->brushEntity())))
      out.insert(o->brushEntity());
  return out;
}

} // namespace

Engine3D::PhysicsSimState Engine3D::physicsSimState() const {
  if (!m_physSim)
    return PhysicsSimState::None;
  return m_physSim->running ? PhysicsSimState::Running : PhysicsSimState::Paused;
}

bool Engine3D::physicsSimSelectedOnly() const { return m_physSim && m_physSim->selectedOnly; }

bool Engine3D::selectionHasRigidBody() const { return m_scene && !selectedRigidBodies(*this, *m_scene).empty(); }

bool Engine3D::levelHasRigidBody() const {
  if (m_scene)
    for (const auto &e: m_scene->entities)
      if (isRigidBody(&e))
        return true;
  return false;
}

bool Engine3D::physicsBodyDirty(const quint32 entity) const {
  if (!m_physSim || !m_scene)
    return false;
  for (const auto &b: m_physSim->bodies) {
    if (b.entity != entity)
      continue;
    if (b.moving)
      return true;
    if (b.originalIds.empty() || !entityById(entity))
      return false;
    const std::vector<quint32> now = brushesOfEntity(entity);
    return std::set<quint32>(now.begin(), now.end()) != b.originalIds;
  }
  return false;
}

bool Engine3D::selectionInPhysicsSimulation() const {
  if (!m_physSim || !m_scene)
    return false;
  for (const quint32 id: selectedRigidBodies(*this, *m_scene))
    for (const auto &b: m_physSim->bodies)
      if (b.entity == id)
        return true;
  return false;
}

bool Engine3D::selectionPhysicsDirty() const {
  if (!m_physSim || !m_scene)
    return false;
  for (const quint32 id: selectedRigidBodies(*this, *m_scene))
    if (physicsBodyDirty(id))
      return true;
  return false;
}

int Engine3D::physicsDirtyCount() const {
  int n = 0;
  if (m_physSim)
    for (const auto &b: m_physSim->bodies)
      n += physicsBodyDirty(b.entity) ? 1 : 0;
  return n;
}

std::vector<quint32> Engine3D::physicsBodyBrushes() const {
  std::vector<quint32> out;
  if (m_physSim)
    for (const auto &b: m_physSim->bodies)
      out.insert(out.end(), b.brushes.begin(), b.brushes.end());
  return out;
}

std::vector<std::array<glm::dvec3, 8>> Engine3D::physicsDirtyBoxes() const {
  std::vector<std::array<glm::dvec3, 8>> out;
  if (!m_physSim || !m_scene)
    return out;
  for (const auto &b: m_physSim->bodies) {
    if (!physicsBodyDirty(b.entity))
      continue;
    glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
    for (const quint32 id: b.brushes)
      if (const auto o = m_scene->findById(id); o && !o->hidden()) {
        const auto [olo, ohi] = o->getWorldAABB();
        lo = glm::min(lo, glm::dvec3(olo));
        hi = glm::max(hi, glm::dvec3(ohi));
      }
    if (lo.x > hi.x)
      continue;
    // A hair outside the brushes, so the outline never fights their edges.
    lo -= glm::dvec3(0.05);
    hi += glm::dvec3(0.05);
    std::array<glm::dvec3, 8> c;
    for (int i = 0; i < 8; ++i) {
      // Bottom ring 0-3, top ring 4-7, each going round.
      const int r = i & 3;
      const glm::dvec3 p((r == 1 || r == 2) ? hi.x : lo.x, i >= 4 ? hi.y : lo.y, (r >= 2) ? hi.z : lo.z);
      c[size_t(i)] = glm::dvec3(b.pose * glm::dvec4(p, 1.0));
    }
    out.push_back(c);
  }
  return out;
}

std::vector<std::vector<glm::dvec3>> Engine3D::physicsTrails() const {
  std::vector<std::vector<glm::dvec3>> out;
  if (m_physSim)
    for (const auto &b: m_physSim->bodies)
      if (b.trail.size() > 1 && physicsBodyDirty(b.entity))
        out.push_back(b.trail);
  return out;
}

const glm::dmat4 *Engine3D::physicsBodyPose(const quint32 entity) const {
  if (m_physSim)
    for (const auto &b: m_physSim->bodies)
      if (b.entity == entity)
        return b.moving ? &b.pose : nullptr;
  return nullptr;
}

std::pair<glm::vec3, glm::vec3> Engine3D::drawnAABB(const eng::GameObject &o) const {
  const auto box = o.getWorldAABB();
  const glm::mat4 *pose = physicsPose(o.stableId());
  if (!pose)
    return box;
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 c((i & 1) ? box.second.x : box.first.x, (i & 2) ? box.second.y : box.first.y,
                      (i & 4) ? box.second.z : box.first.z);
    const glm::vec3 p = glm::vec3(*pose * glm::vec4(c, 1.0f));
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  return {lo, hi};
}

// A Box3D world of the level as it is now, for the given bodies. Bodies keep
// their originals and trails; their brushes and baselines follow the level.
bool Engine3D::buildPhysicsWorld(const std::set<quint32> &entities) {
  PhysicsSim &sim = *m_physSim;
  QElapsedTimer t;
  t.start();
  syncBrushEntityOrigins(); // body origins at the brushes' centre as they are now
  const eng::phys::Snapshot snap = physicsSnapshot(&entities);
  sim.world.reset();
  sim.cache = std::make_unique<eng::phys::Cache>();
  sim.world = eng::phys::World::build(snap, *sim.cache);
  std::vector<PhysicsSim::Body> bodies;
  for (const auto &col: sim.world->bodies()) {
    PhysicsSim::Body b;
    for (auto &old: sim.bodies)
      if (old.entity == col.entity) {
        b = std::move(old);
        old.entity = 0;
      }
    b.entity = col.entity;
    for (const auto &in: snap.bodies)
      if (in.entity == col.entity)
        b.baseOrigin = glm::dvec3(in.origin);
    b.baseRotation = glm::dquat(1.0, 0.0, 0.0, 0.0);
    b.position = b.baseOrigin;
    b.rotation = b.baseRotation;
    b.pose = glm::dmat4(1.0);
    b.moving = false;
    b.brushes.clear();
    std::vector<std::shared_ptr<eng::GameObject>> objects;
    for (const quint32 id: brushesOfEntity(col.entity)) {
      const auto o = m_scene->findById(id);
      if (!o || o->hiddenFlag() || o->modReplaced())
        continue;
      b.brushes.push_back(id);
      objects.push_back(o);
    }
    if (b.brushes.empty())
      continue;
    // First time simulated: these are what Reset brings back.
    if (b.originalIds.empty()) {
      b.originals = objects;
      b.originalIds = std::set<quint32>(b.brushes.begin(), b.brushes.end());
    }
    const glm::dvec3 at = b.baseOrigin / eng::kMetersPerWorldUnit;
    if (b.trail.empty() || glm::length(b.trail.back() - at) > kTrailSpacing)
      b.trail.push_back(at);
    bodies.push_back(std::move(b));
  }
  // Bodies the world could not take now keep their originals for Reset.
  for (auto &old: sim.bodies)
    if (old.entity && !old.originalIds.empty()) {
      old.brushes = brushesOfEntity(old.entity);
      old.pose = glm::dmat4(1.0);
      old.moving = false;
      bodies.push_back(std::move(old));
    }
  sim.bodies = std::move(bodies);
  sim.worldStale = false;
  sim.docRevision = m_undoStack ? m_undoStack->documentRevision() : 0;
  qInfo().noquote() << QStringLiteral("[physics] simulation world: %1 bodies, %2 static hulls, built in %3 ms")
                           .arg(sim.world->stats().bodies)
                           .arg(sim.world->stats().staticHulls)
                           .arg(double(t.nsecsElapsed()) / 1e6, 0, 'f', 1);
  return sim.world->stats().bodies > 0;
}

bool Engine3D::physicsPlay(const bool selectedOnly) {
  if (!m_scene || !m_phys || !prefs::b("Debug/Physics"))
    return false;
  if (m_physSim && m_physSim->running)
    return true;
  if (!m_physSim) {
    std::set<quint32> simulated;
    if (selectedOnly) {
      simulated = selectedRigidBodies(*this, *m_scene);
    } else {
      for (const auto &e: m_scene->entities)
        if (isRigidBody(&e))
          simulated.insert(e.id);
    }
    if (simulated.empty()) {
      emit toastRequested(tr("No rigid body to simulate"));
      return false;
    }
    m_physSim.reset(new PhysicsSim);
    m_physSim->selectedOnly = selectedOnly;
    m_physSim->timer.setInterval(16);
    connect(&m_physSim->timer, &QTimer::timeout, this, &Engine3D::physicsSimTick);
    if (!buildPhysicsWorld(simulated)) {
      m_physSim.reset();
      emit toastRequested(tr("No rigid body with a solid collision shape"));
      emit physicsSimChanged();
      return false;
    }
  } else if (m_physSim->worldStale || !m_physSim->world) {
    // The level changed while paused: a new world from it, the same bodies.
    std::set<quint32> simulated;
    for (const auto &b: m_physSim->bodies)
      if (entityById(b.entity))
        simulated.insert(b.entity);
    if (simulated.empty() || !buildPhysicsWorld(simulated)) {
      emit toastRequested(tr("No rigid body with a solid collision shape"));
      return false;
    }
  }
  m_physSim->running = true;
  m_physSim->pending = 0.0;
  m_physSim->clock.restart();
  m_physSim->timer.start();
  ++m_physPoseEpoch; // the views mark the bodies
  updatePhysicsPoses();
  emit physicsSimChanged();
  requestRedraw();
  return true;
}

void Engine3D::physicsPause() {
  PhysicsSim *sim = m_physSim.get();
  if (!sim || !sim->running)
    return;
  sim->running = false;
  sim->timer.stop();
  // The bodies where they are, in the level: one undo step.
  std::vector<PhysicsSim::Body *> moved;
  for (auto &b: sim->bodies)
    if (b.moving && !b.brushes.empty())
      moved.push_back(&b);
  if (!moved.empty() && m_scene && m_undoStack) {
    const auto prevEntities = m_selectedEntities;
    const auto prevObjects = m_selectedObjects;
    const auto prevMods = m_selectedMods;
    const bool lock = m_textureLock;
    m_physApplying = true;
    m_undoStack->beginMacro(tr("Simulate physics"));
    std::set<quint32> replaced, reselect;
    for (PhysicsSim::Body *b: moved) {
      for (const quint32 id: b->brushes)
        m_physPose.remove(id);
      bool wasSelected = false;
      for (const quint32 id: b->brushes)
        wasSelected = wasSelected || prevObjects.count(id);
      // The brushes alone, so nothing else selected moves with them; the
      // texture travels with the body.
      m_selectedEntities.clear();
      m_selectedMods.clear();
      m_selectedFaces.clear();
      m_selectedObjects = std::set<quint32>(b->brushes.begin(), b->brushes.end());
      m_textureLock = true;
      transformSelectionBrushes(b->pose, tr("Simulate physics"));
      m_textureLock = lock;
      replaced.insert(b->brushes.begin(), b->brushes.end());
      b->brushes.assign(m_selectedObjects.begin(), m_selectedObjects.end());
      if (wasSelected)
        reselect.insert(b->brushes.begin(), b->brushes.end());
      // The level is at the simulated pose now: that is the new baseline.
      b->baseOrigin = b->position;
      b->baseRotation = b->rotation;
      b->pose = glm::dmat4(1.0);
      b->moving = false;
    }
    m_undoStack->endMacro();
    // The selection as it was, with the new brushes in place of the old ones.
    m_selectedObjects.clear();
    for (const quint32 id: prevObjects)
      if (!replaced.count(id) && m_scene->findById(id))
        m_selectedObjects.insert(id);
    m_selectedObjects.insert(reselect.begin(), reselect.end());
    m_selectedEntities = prevEntities;
    m_selectedMods = prevMods;
    refreshDrawSelection();
    m_physApplying = false;
    sim->docRevision = m_undoStack->documentRevision();
    emit selectionChanged();
  }
  m_physPose.clear();
  ++m_physPoseEpoch;
  ++m_viewEpoch;
  emit physicsSimChanged();
  requestRedraw();
}

void Engine3D::physicsStop() {
  if (!m_physSim && m_physPose.isEmpty())
    return;
  m_physSim.reset();
  m_physPose.clear();
  ++m_physPoseEpoch;
  ++m_viewEpoch;
  emit physicsSimChanged();
  requestRedraw();
}

void Engine3D::physicsReset() {
  PhysicsSim *sim = m_physSim.get();
  if (!sim || !m_scene) {
    physicsStop();
    return;
  }
  // A running world's motion was never written: it simply stops being drawn.
  sim->running = false;
  sim->timer.stop();
  m_physPose.clear();
  // Every dirty body gets the brushes it started with, one undo step.
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  std::set<quint32> reselect;
  for (const auto &b: sim->bodies) {
    if (b.originalIds.empty() || !entityById(b.entity))
      continue;
    const std::vector<quint32> now = brushesOfEntity(b.entity);
    if (std::set<quint32>(now.begin(), now.end()) == b.originalIds)
      continue;
    bool wasSelected = m_selectedEntities.count(b.entity) > 0;
    for (const quint32 id: now)
      wasSelected = wasSelected || m_selectedObjects.count(id);
    remove.insert(remove.end(), now.begin(), now.end());
    for (const auto &o: b.originals)
      if (!m_scene->findById(o->stableId())) {
        add.push_back(o);
        if (wasSelected)
          reselect.insert(o->stableId());
      }
  }
  if ((!remove.empty() || !add.empty()) && m_undoStack) {
    const auto prevEntities = m_selectedEntities;
    const auto prevObjects = m_selectedObjects;
    m_physApplying = true;
    replaceObjects(remove, add, tr("Reset physics"));
    m_physApplying = false;
    // The selection as it was, the bodies with their original brushes.
    m_selectedObjects.clear();
    for (const quint32 id: prevObjects)
      if (m_scene->findById(id))
        m_selectedObjects.insert(id);
    m_selectedObjects.insert(reselect.begin(), reselect.end());
    m_selectedEntities = prevEntities;
    refreshDrawSelection();
    emit selectionChanged();
  }
  physicsStop();
}

bool Engine3D::physicsApply() {
  PhysicsSim *sim = m_physSim.get();
  if (!sim || !m_scene)
    return false;
  if (sim->running)
    physicsPause(); // writes them where they are
  const std::set<quint32> sel = selectedRigidBodies(*this, *m_scene);
  int kept = 0;
  for (auto &b: sim->bodies) {
    if (!sel.count(b.entity) || !physicsBodyDirty(b.entity))
      continue;
    // Their brushes now are theirs: Reset leaves them where they are.
    const std::vector<quint32> now = brushesOfEntity(b.entity);
    b.originals.clear();
    for (const quint32 id: now)
      if (const auto o = m_scene->findById(id))
        b.originals.push_back(o);
    b.originalIds = std::set<quint32>(now.begin(), now.end());
    b.trail.clear();
    b.trail.push_back(b.position / eng::kMetersPerWorldUnit);
    ++kept;
  }
  if (kept == 0)
    return false;
  ++m_physPoseEpoch;
  ++m_viewEpoch;
  emit physicsSimChanged();
  requestRedraw();
  qInfo().noquote() << QStringLiteral("[physics] kept the simulated pose of %1 bodies").arg(kept);
  return true;
}

void Engine3D::checkPhysicsDocument() {
  if (!m_physSim || m_physApplying || !m_undoStack)
    return;
  if (m_undoStack->documentRevision() == m_physSim->docRevision)
    return;
  m_physSim->docRevision = m_undoStack->documentRevision();
  m_physSim->worldStale = true;
  if (m_physSim->running) {
    // An edit while running (editing is off, but a step may slip through):
    // the motion so far is dropped, the bodies are where the level has them.
    m_physSim->running = false;
    m_physSim->timer.stop();
    m_physPose.clear();
    for (auto &b: m_physSim->bodies) {
      b.pose = glm::dmat4(1.0);
      b.moving = false;
    }
    ++m_physPoseEpoch;
    ++m_viewEpoch;
    requestRedraw();
  }
  // The bodies' brushes follow the level (an undo may have swapped them).
  for (auto &b: m_physSim->bodies)
    b.brushes = brushesOfEntity(b.entity);
  ++m_physPoseEpoch;
  emit physicsSimChanged();
}

void Engine3D::physicsSimTick() {
  PhysicsSim *sim = m_physSim.get();
  if (!sim || !sim->running || !sim->world)
    return;
  // Never more than a quarter second at once (a stall, a debugger).
  sim->pending += std::clamp(double(sim->clock.restart()) / 1000.0, 0.0, 0.25);
  const int steps = int(sim->pending / kStep);
  if (steps <= 0)
    return;
  sim->pending -= steps * kStep;
  sim->world->step(steps * kStep);
  updatePhysicsPoses();
}

void Engine3D::updatePhysicsPoses() {
  PhysicsSim *sim = m_physSim.get();
  if (!sim || !sim->world)
    return;
  const double toWorld = 1.0 / eng::kMetersPerWorldUnit;
  bool dirtyChanged = false, posesChanged = false;
  const auto poses = sim->world->bodyPoses();
  for (auto &b: sim->bodies) {
    if (b.brushes.empty())
      continue;
    bool found = false;
    for (const auto &p: poses) {
      if (p.entity != b.entity)
        continue;
      b.position = glm::dvec3(p.position);
      b.rotation = glm::normalize(glm::dquat(p.rotation));
      found = true;
      break;
    }
    if (!found)
      continue; // not in this world (no solid hull)
    // From the level's pose to the simulated one, in world units:
    // p' = position + R·R_base⁻¹·(p − baseOrigin).
    const glm::dquat turn = b.rotation * glm::inverse(b.baseRotation);
    const glm::dmat4 before = b.pose;
    b.pose = glm::translate(glm::dmat4(1.0), b.position * toWorld) * glm::mat4_cast(turn)
             * glm::translate(glm::dmat4(1.0), -b.baseOrigin * toWorld);
    const bool moving = glm::length(b.position - b.baseOrigin) * toWorld > 1e-3
                        || std::fabs(std::fabs(turn.w) - 1.0) > 1e-7;
    if (moving != b.moving) {
      b.moving = moving;
      dirtyChanged = true;
    }
    posesChanged = posesChanged || dirtyChanged || before != b.pose;
    const glm::mat4 pose(b.pose);
    for (const quint32 id: b.brushes) {
      if (b.moving)
        m_physPose.insert(id, pose);
      else
        m_physPose.remove(id);
    }
    // The trail: a dot every few units the origin travels.
    const glm::dvec3 at = b.position * toWorld;
    if (b.trail.empty() || glm::length(at - b.trail.back()) >= kTrailSpacing) {
      if (b.trail.size() >= kTrailMax)
        b.trail.erase(b.trail.begin());
      b.trail.push_back(at);
    }
  }
  // Bodies at rest (asleep) change nothing: no uploads, no shadow passes.
  if (posesChanged) {
    ++m_physPoseEpoch;
    ++m_viewEpoch;
    requestRedraw();
  }
  if (dirtyChanged)
    emit physicsSimChanged();
}

} // namespace vk
