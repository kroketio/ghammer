#pragma once

// Smooth shading across brushes. The smooth brushes of one unit - a generated object (arch, stairs), a brush entity
// (func_detail, ...) or a group (Ctrl+G) - blend their normals together, so a curve built from several convex
// brushes shades as one surface. A smooth brush in none of these smooths on its own (Mesh::setSmoothShading).

#include <QHash>
#include <QtGlobal>

#include <vector>

#include <glm/glm.hpp>

namespace eng {

class GameObject;
class Scene;

// The unit a brush smooths with; 0 = on its own.
[[nodiscard]] quint64 smoothUnitKey(const GameObject &o);

// Corner normals of several solids' faces smoothed together (kSmoothAngleDeg): polys[s][f] is solid s's face f with
// its normal normals[s][f]; the result has the same shape.
std::vector<std::vector<std::vector<glm::vec3>>>
smoothSolidsTogether(const std::vector<std::vector<std::vector<glm::dvec3>>> &polys,
                     const std::vector<std::vector<glm::dvec3>> &normals);

// Re-smooths the vertex normals of every unit whose brushes changed since the last call (`signatures` keeps each
// unit's state between calls). Cheap when nothing changed.
void refreshSmoothUnits(Scene &scene, QHash<quint64, quint64> &signatures);

} // namespace eng
