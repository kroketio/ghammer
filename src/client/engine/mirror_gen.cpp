#include "mirror_gen.h"

#include <algorithm>

namespace eng::mirror {

glm::dmat4 matrix(const Plane &p) {
  glm::dmat4 m(1.0);
  m[p.axis][p.axis] = -1.0;
  m[3][p.axis] = 2.0 * p.d;
  return m;
}

void box(const glm::dmat4 &m, glm::dvec3 &mn, glm::dvec3 &mx) {
  const glm::dvec3 a = glm::dvec3(m * glm::dvec4(mn, 1.0));
  const glm::dvec3 b = glm::dvec3(m * glm::dvec4(mx, 1.0));
  mn = glm::min(a, b);
  mx = glm::max(a, b);
}

double overlap(const glm::dvec3 &amn, const glm::dvec3 &amx, const glm::dvec3 &bmn, const glm::dvec3 &bmx) {
  const glm::dvec3 imn = glm::max(amn, bmn);
  const glm::dvec3 imx = glm::min(amx, bmx);
  const glm::dvec3 ea = amx - amn, eb = bmx - bmn, ei = imx - imn;
  if (ei.x < 0.0 || ei.y < 0.0 || ei.z < 0.0)
    return 0.0;
  double num = 1.0, den = 1.0;
  for (int i = 0; i < 3; ++i) {
    const double ext = std::min(ea[i], eb[i]);
    if (ext <= 1e-9) // flat on this axis: touching counts as sharing it
      continue;
    num *= ei[i];
    den *= ext;
  }
  return den > 0.0 ? num / den : 1.0;
}

bool selfOverlapping(const Plane &p, const glm::dvec3 &mn, const glm::dvec3 &mx) {
  glm::dvec3 cmn = mn, cmx = mx;
  box(matrix(p), cmn, cmx);
  return overlap(cmn, cmx, mn, mx) >= 0.5;
}

} // namespace eng::mirror
