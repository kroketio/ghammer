#pragma once

#include <vector>

#include <QJsonObject>

#include <glm/glm.hpp>

#include "brush.h"

namespace eng {

// Arches fitted to an axis-aligned box: profile in the (spanAxis, Y) plane, extruded along the third axis.
// All pieces are convex and none overlap.
enum class ArchProfile { Round = 0, Segmental, Pointed, Elliptical, Tudor };
enum class ArchMode {
  Ring = 0,    // the ring (voussoirs) plus jamb posts below the spring line
  Archway = 1, // wall with an arched opening: 2 jambs + one spandrel per segment
  Opening = 2, // the opening volume, for carving
};

struct ArchParams {
  ArchProfile profile = ArchProfile::Round;
  ArchMode mode = ArchMode::Archway;
  int segments = 8;
  double thickness = 1.6; // ring depth / jamb width / crown height, world units (Archway: 0 = no jambs)
  double spring = 0.0;    // straight jamb height below the arc, world units
  double rise = 0.5;      // Segmental: rise as a fraction of the available height (0.05..1)
  bool smooth = true;     // the brushes shade as one smooth surface (eng::refreshSmoothUnits)
};

// The intrados, left springing to right, in profile space (x in [x0, x1], y in [base, top]).
// segments + 1 points; Pointed / Tudor always have a vertex on the apex.
std::vector<glm::dvec2> archCurve(ArchProfile profile, double x0, double x1, double base, double top, int segments,
                                  double rise);

// spanAxis: 0 = X, 2 = Z.
std::vector<ConvexBrush> buildArch(const glm::dvec3 &mn, const glm::dvec3 &mx, int spanAxis, const ArchParams &params,
                                   const MaterialPtr &material, double tilesPerUnit);

// A generated arch (ProcObject "arch"). JSON keys: "min", "max", "spanAxis", "mode", "profile",
// "segments", "thickness", "spring", "rise", "smooth"; lengths in world units.
struct ArchPlacement {
  glm::dvec3 mn {0.0}, mx {0.0};
  int spanAxis = 0;
};
QJsonObject archToJson(const ArchPlacement &place, const ArchParams &params);
void archFromJson(const QJsonObject &o, ArchPlacement &place, ArchParams &params);

} // namespace eng
