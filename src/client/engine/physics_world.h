#pragma once

#include <QtGlobal>

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct b3HullData;

// The map as a Box3D world, built on one worker thread from a snapshot the GUI thread takes (vk_physics.cpp):
// brushes are hulls on one static body, each func_rigidbody a dynamic body of its simplified hulls (collision_hulls.h).
// Metres, Y up, gravity -9.81. The simulation (vk_physics_sim.cpp) builds its own world.
// Box3D: worlds must not be created / destroyed from two threads at once; World's ctor / dtor take a process-wide lock.
namespace eng::phys {

struct HullInput {
  quint64 key = 0;                // brush signature: unchanged brushes reuse their hull
  std::vector<glm::vec3> points;  // world space, metres
};

struct BodyInput {
  quint32 entity = 0;
  float massKg = 10.0f;
  int maxHulls = 4;
  glm::vec3 origin {0.0f};        // entity origin, metres
  std::vector<HullInput> brushes;
};

struct Snapshot {
  std::vector<HullInput> statics;
  std::vector<BodyInput> bodies;
};

// Origin (started at the BodyInput origin) and rotation since the build. Metres.
struct BodyPose {
  quint32 entity = 0;
  glm::vec3 position {0.0f};
  glm::quat rotation {1.0f, 0.0f, 0.0f, 0.0f};
};

struct Stats {
  int staticHulls = 0;
  int bodies = 0;
  int bodyHulls = 0;
  int failed = 0;   // brushes without a solid hull (flat) or over Box3D's hull limits
  double ms = 0.0;
};

// Hulls in world metres.
struct BodyCollision {
  quint32 entity = 0;
  int brushes = 0;
  std::vector<std::vector<glm::vec3>> hulls;
  std::vector<std::pair<glm::vec3, glm::vec3>> edges;  // world metres, for drawing
};

// Hulls kept between builds; only the build worker touches it.
class Cache {
public:
  Cache() = default;
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;
  ~Cache();

  // nullptr when the brush makes no hull.
  b3HullData *brushHull(const HullInput &in);
  // Simplified hulls (world metres), cached by the body's brushes.
  const std::vector<std::vector<glm::vec3>> &bodyHulls(const BodyInput &in, int *skipped);
  // Drops everything the latest snapshot did not use.
  void sweep();

private:
  struct Entry {
    b3HullData *hull = nullptr;
    bool used = false;
  };
  struct BodyEntry {
    std::vector<std::vector<glm::vec3>> hulls;
    int skipped = 0;
    bool used = false;
  };
  std::unordered_map<quint64, Entry> m_brushes;
  std::unordered_map<quint64, BodyEntry> m_bodies;
};

class World {
public:
  ~World();
  World(const World &) = delete;
  World &operator=(const World &) = delete;

  static std::unique_ptr<World> build(const Snapshot &snapshot, Cache &cache);

  [[nodiscard]] const Stats &stats() const { return m_stats; }
  [[nodiscard]] const std::vector<BodyCollision> &bodies() const { return m_collision; }

  // Whole 1/60 s steps (4 sub-steps each), at least one.
  void step(double seconds);
  [[nodiscard]] std::vector<std::pair<quint32, glm::vec3>> bodyPositions() const;
  [[nodiscard]] std::vector<BodyPose> bodyPoses() const;

private:
  World();
  struct Impl;
  std::unique_ptr<Impl> m;
  Stats m_stats;
  std::vector<BodyCollision> m_collision;
};

} // namespace eng::phys
