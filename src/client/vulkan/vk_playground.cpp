// File › New › Playground Level: a walled floor, a tilted rigid-body crate floating above it, an omni light.

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "vk_engine.h"

#include "client/engine/brush.h"
#include "client/engine/scene.h"

namespace vk {

void Engine3D::buildPlayground() {
  if (!m_scene)
    return;
  double tpu = 0.0;
  const auto mat = newBrushMaterial(tpu);
  const double u = 1.0 / kSourceUnitsPerWorldUnit;  // Source units -> world units
  const auto box = [&](const glm::dvec3 &a, const glm::dvec3 &b) { return eng::ConvexBrush::box(a * u, b * u, mat, tpu); };

  // A 512 x 512 floor, walls 16 thick standing round its edge: two 128 high, two 64.
  std::vector<std::shared_ptr<eng::GameObject>> world;
  for (const eng::ConvexBrush &b: {box({-256, -16, -256}, {256, 0, 256}),     // floor
                                   box({-272, -16, 256}, {272, 128, 272}),    // +Z
                                   box({-272, -16, -272}, {272, 64, -256}),   // -Z
                                   box({256, -16, -256}, {272, 64, 256}),     // +X
                                   box({-272, -16, -256}, {-256, 128, 256})}) // -X
    if (auto o = objectFromBrush(b))
      world.push_back(o);
  replaceObjects({}, world, QStringLiteral("Playground"));

  // A 128 crate 192 above the floor, turned about two axes, each face's texture fitted to it; a rigid body.
  eng::ConvexBrush crate = box({-64, 192, -64}, {64, 320, 64});
  const glm::dvec3 centre = glm::dvec3(0.0, 256.0, 0.0) * u;
  const glm::dquat turn = glm::angleAxis(glm::radians(30.0), glm::dvec3(0, 1, 0))
                          * glm::angleAxis(glm::radians(20.0), glm::dvec3(1, 0, 0));
  crate.transform(glm::translate(glm::dmat4(1.0), centre) * glm::mat4_cast(turn) * glm::translate(glm::dmat4(1.0), -centre),
                  true);
  for (size_t i = 0; i < crate.faces.size() && i < crate.polygons.size(); ++i) {
    std::vector<glm::vec3> poly;
    for (const glm::dvec3 &p: crate.polygons[i])
      poly.push_back(glm::vec3(p));
    eng::fitBasisToFace(crate.faces[i].basis, poly, glm::vec3(crate.faces[i].normal), 1, 1);
  }
  if (auto o = objectFromBrush(crate)) {
    o->setOrientation(turn);
    replaceObjects({}, {o}, QStringLiteral("Playground"));
    makeBrushEntity(QStringLiteral("func_rigidbody"));
  }

  addEntity(QStringLiteral("light"), glm::vec3(glm::dvec3(160, 96, -160) * u));
  setSelected({}, {});
  // A new level: nothing to undo, nothing unsaved.
  if (m_undoStack) {
    m_undoStack->clear();
    m_undoStack->markClean();
  }
}

} // namespace vk
