// Zen mode: the 2D views show only what touches a box around the 3D camera.

#include "vk_engine.h"

#include "client/engine/gameobject.h"
#include "client/engine/scene.h"
#include "client/vulkan/vk_gridview.h"

namespace vk {

void Engine3D::setZenMode(const bool on) {
  if (on == m_zenOn)
    return;
  m_zenOn = on;
  updateZenBox(true);
  emit zenModeChanged();
}

void Engine3D::setZenSizeSource(const glm::vec3 &size) {
  const glm::vec3 s = glm::max(size, glm::vec3(16.0f));
  if (s == m_zenSizeSource)
    return;
  m_zenSizeSource = s;
  updateZenBox(true);
  emit zenModeChanged();
}

bool Engine3D::zenBox(glm::vec3 &mn, glm::vec3 &mx) const {
  if (!m_zenOn)
    return false;
  // Source x / y lie across (our x / z), Source z is up (our y).
  const float k = float(GridView::kUnitsPerWorldUnit);
  const glm::vec3 half(m_zenSizeSource.x / k * 0.5f, m_zenSizeSource.z / k * 0.5f, m_zenSizeSource.y / k * 0.5f);
  mn = m_zenCenter - half;
  mx = m_zenCenter + half;
  return true;
}

bool Engine3D::zenHides(const eng::GameObject &o) const {
  glm::vec3 mn, mx;
  if (!zenBox(mn, mx) || m_selectedObjects.count(o.stableId()))
    return false;
  const auto [lo, hi] = o.getWorldAABB();
  return hi.x < mn.x || lo.x > mx.x || hi.y < mn.y || lo.y > mx.y || hi.z < mn.z || lo.z > mx.z;
}

bool Engine3D::zenHidesPoint(const glm::vec3 &p) const {
  glm::vec3 mn, mx;
  if (!zenBox(mn, mx))
    return false;
  return glm::any(glm::lessThan(p, mn)) || glm::any(glm::greaterThan(p, mx));
}

void Engine3D::updateZenBox(const bool force) {
  if (!m_zenOn && !force)
    return;
  const glm::vec3 c = m_camera ? m_camera->getPosition() : glm::vec3(0.0f);
  if (!force && c == m_zenCenter)
    return;
  m_zenCenter = c;
  ++m_zenEpoch;
  emit zenChanged();
}

} // namespace vk
