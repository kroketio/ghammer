// Cut mode in the 3D view: hover places a grid-snapped cut across the face, click applies; it is the same
// CutLine the 2D views draw.

#include <cmath>

#include "vk_engine.h"

#include "client/engine/scene.h"

namespace vk {

namespace {
int dominantAxis(const glm::dvec3 &v) {
  int a = 0;
  if (std::fabs(v.y) > std::fabs(v[a]))
    a = 1;
  if (std::fabs(v.z) > std::fabs(v[a]))
    a = 2;
  return a;
}
} // namespace

bool Engine3D::cutHit3D(const float px, const float py, glm::dvec3 &hit, int &faceAxis) const {
  if (!m_scene || m_selectedObjects.empty())
    return false;
  glm::vec3 ro, rd;
  makeRay(px, py, std::max(1.0f, float(m_lastViewSize.width())), std::max(1.0f, float(m_lastViewSize.height())), ro,
          rd);
  float t = 0.0f;
  glm::vec3 n(0.0f);
  quint32 obj = 0;
  if (raycastScene(ro, rd, t, n, &obj) && m_selectedObjects.count(obj)) {
    hit = glm::dvec3(ro + rd * t);
    faceAxis = dominantAxis(glm::dvec3(n));
    return true;
  }
  // Not on a selected brush: use the selection's box, so a hidden selection can still be aimed at.
  glm::dvec3 mn, mx;
  if (!selectionWorldAABB(mn, mx))
    return false;
  double t0 = 0.0, t1 = 1e30;
  int enterAxis = -1;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(rd[a]) < 1e-9) {
      if (ro[a] < mn[a] || ro[a] > mx[a])
        return false;
      continue;
    }
    double ta = (mn[a] - ro[a]) / rd[a], tb = (mx[a] - ro[a]) / rd[a];
    if (ta > tb)
      std::swap(ta, tb);
    if (ta > t0) {
      t0 = ta;
      enterAxis = a;
    }
    t1 = std::min(t1, tb);
    if (t0 > t1)
      return false;
  }
  if (enterAxis < 0)
    return false;
  hit = glm::dvec3(ro) + glm::dvec3(rd) * t0;
  faceAxis = enterAxis;
  return true;
}

void Engine3D::turnCut3D() {
  m_cutTurned = !m_cutTurned;
  if (m_cutHoverValid && !isCutout()) {
    // A wheel turn is a deliberate 3D aim: it takes over from a 2D line.
    m_cutLineFrom2D = false;
    hoverCut3D(m_cutHoverPx.x, m_cutHoverPx.y, m_cutHoverShift);
  }
}

void Engine3D::hoverCut3D(const float px, const float py, const bool rotate) {
  if (m_cutMode == CutMode::Off)
    return;
  m_cutHoverValid = true;
  m_cutHoverPx = {px, py};
  m_cutHoverShift = rotate;
  glm::dvec3 hit;
  int faceAxis = 1;
  if (!cutHit3D(px, py, hit, faceAxis))
    return;
  const double step = std::max(1e-3, gizmoSnapStep());

  if (isCutout()) {
    // Aim from the 2D view that looks straight at the face under the pointer.
    for (int plane = 0; plane < 3; ++plane) {
      const ViewBasis bs = viewBasisFor(plane);
      if (std::fabs(bs.depth[faceAxis]) < 0.5)
        continue;
      if (cutoutPlaced())
        return;
      trackCutHole(plane, {glm::dot(hit, bs.right), glm::dot(hit, bs.up)}, step);
      return;
    }
    return;
  }

  if (m_cutLineFrom2D && m_cutLine.valid)
    return;

  // The cut normal is the face's in-plane axis that makes the line upright on screen (Shift: the other).
  const int u = (faceAxis + 1) % 3, v = (faceAxis + 2) % 3;
  int axis;
  if (faceAxis != 1) {
    // A wall: cutting across its horizontal axis gives an upright line.
    axis = u == 1 ? v : u;
  } else {
    const glm::dvec3 right(m_camera->getRight());
    axis = std::fabs(right[u]) >= std::fabs(right[v]) ? u : v;
  }
  if (rotate != m_cutTurned)
    axis = axis == u ? v : u;

  // Express the plane as a line in a 2D view whose right or up is that axis, as if drawn there.
  for (int plane = 0; plane < 3; ++plane) {
    const ViewBasis bs = viewBasisFor(plane);
    for (const bool alongRight: {true, false}) {
      const glm::dvec3 n = alongRight ? bs.up : bs.right;
      if (std::fabs(n[axis]) < 0.5)
        continue;
      const double world = std::round(hit[axis] / step) * step;
      const double k = world * n[axis];
      if (m_cutLine.valid && !m_cutLine.angled && m_cutLine.plane == plane && m_cutLine.alongRight == alongRight
          && std::fabs(m_cutLine.k - k) < 1e-9)
        return;
      m_cutLine = {};
      m_cutLine.valid = true;
      m_cutLine.plane = plane;
      m_cutLine.alongRight = alongRight;
      m_cutLine.k = k;
      emit cutLineChanged();
      requestRedraw();
      return;
    }
  }
}

bool Engine3D::clickCut3D(const float px, const float py, const bool rotate) {
  if (m_cutMode == CutMode::Off)
    return false;
  if (isCutout()) {
    if (!cutoutPlaced())
      hoverCut3D(px, py, rotate);
    if (m_cutHoles.empty())
      return true;
    placeCutHole();
    applyCut();
    return true;
  }
  m_cutLineFrom2D = false;
  hoverCut3D(px, py, rotate);
  if (m_cutLine.valid)
    applyCut();
  return true;
}

} // namespace vk
