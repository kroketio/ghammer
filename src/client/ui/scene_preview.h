#pragma once

// Small scenes drawn by their own engine in an RV.Viewport3D.

#include <vector>

#include <glm/glm.hpp>

#include "client/engine/brush.h"
#include "client/engine/entity.h"

class QObject;
class QQuickItem;

namespace vk {
class Engine3D;
}

namespace ui::preview {

// A lit preview engine with an empty scene.
vk::Engine3D *newEngine(QObject *parent);

void setScene(vk::Engine3D &engine, const std::vector<eng::ConvexBrush> &brushes,
              const std::vector<eng::Entity> &entities = {}, bool smooth = false);

// Unit vector from the target to the eye; pitch up is positive (degrees).
glm::vec3 orbitDir(float yaw, float pitch);
void orbit(vk::Engine3D &engine, const glm::vec3 &target, float yaw, float pitch, float distance);

// Gives `engine` to the RV.Viewport3D named `objectName` under `root`.
bool attach(QQuickItem *root, const char *objectName, vk::Engine3D *engine);

// A box with every face fitted to `repeat` x `repeat` tiles.
eng::ConvexBrush fittedBox(const glm::dvec3 &mn, const glm::dvec3 &mx, const eng::MaterialPtr &material,
                           int repeat = 1);
// A UV sphere: `repeatU` tiles around, `repeatV` pole to pole.
eng::ConvexBrush sphere(double radius, int segments, int rings, const eng::MaterialPtr &material, double repeatU,
                        double repeatV);

} // namespace ui::preview
