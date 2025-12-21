#pragma once

// Generated shapes fitted to an axis-aligned box along one axis: cylinder (solid, or hollow: a ring of pieces),
// capsule (a cylinder with rounded ends, one convex brush) and pyramid (an n-sided base to a point; many sides make a
// cone). Kept as ProcObjects "cylinder" / "capsule" / "pyramid" with these parameters.

#include <vector>

#include <QJsonObject>
#include <QString>

#include <glm/glm.hpp>

#include "brush.h"

namespace eng {

enum class ShapeKind { Cylinder = 0, Capsule, Pyramid };

struct ShapeParams {
  ShapeKind kind = ShapeKind::Cylinder;
  int sides = 16;        // around the axis
  int axis = 1;          // 0 X, 1 Y, 2 Z: the cylinder's / capsule's length, the pyramid's point
  double wall = 0.0;     // cylinder: 0 = solid, else the wall thickness of a hollow one (world units)
  int capSegments = 4;   // capsule: rings per rounded end
  bool smooth = true;    // the brushes shade as one smooth surface (eng::refreshSmoothUnits)
};

// "cylinder", "capsule", "pyramid".
[[nodiscard]] QString shapeTypeName(ShapeKind kind);
[[nodiscard]] bool shapeKindOf(const QString &type, ShapeKind &kind);

std::vector<ConvexBrush> buildShape(const glm::dvec3 &mn, const glm::dvec3 &mx, const ShapeParams &params,
                                    const MaterialPtr &material, double tilesPerUnit);

// JSON keys: "min", "max", "sides", "axis", "wall", "capSegments", "smooth" (lengths in world units); the kind is
// the ProcObject's type.
QJsonObject shapeToJson(const glm::dvec3 &mn, const glm::dvec3 &mx, const ShapeParams &params);
void shapeFromJson(const QJsonObject &o, glm::dvec3 &mn, glm::dvec3 &mx, ShapeParams &params);

} // namespace eng
