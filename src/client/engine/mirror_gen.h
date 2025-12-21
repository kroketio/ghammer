#pragma once

#include <glm/glm.hpp>

namespace eng::mirror {

// A mirror plane: p[axis] = d (axis 0 = X, 1 = Y, 2 = Z, world units).
struct Plane {
  int axis = 0;
  double d = 0.0;
  bool operator==(const Plane &o) const { return axis == o.axis && d == o.d; }
};

[[nodiscard]] glm::dmat4 matrix(const Plane &p);
// The axis-aligned box `mn`..`mx` through a reflection `m`.
void box(const glm::dmat4 &m, glm::dvec3 &mn, glm::dvec3 &mx);
// Shared fraction per axis (against the shorter extent), multiplied: 1 = same box, 0 = apart; flat axes ignored.
[[nodiscard]] double overlap(const glm::dvec3 &amn, const glm::dvec3 &amx, const glm::dvec3 &bmn,
                             const glm::dvec3 &bmx);
// True when the reflection of `mn`..`mx` overlaps itself (a brush across the plane): no copy is made.
[[nodiscard]] bool selfOverlapping(const Plane &p, const glm::dvec3 &mn, const glm::dvec3 &mx);

} // namespace eng::mirror
