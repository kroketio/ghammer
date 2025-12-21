#include "gameobject.h"

namespace eng {

GameObject::GameObject(const std::shared_ptr<Mesh> &mesh, quint32 stableId) :
    m_mesh(mesh),
    m_stableId(stableId != 0 ? stableId : s_nextStableId.fetch_add(1) + 1),
    m_position(0.0f),
    m_rotation(0.0f),
    m_scale(1.0f) {
  if (stableId != 0)
    reserveIdsAbove(stableId);
}

void GameObject::reserveIdsAbove(const quint32 id) {
  // Only moves forward: racing loaders or a map loaded over higher ids must never reuse an id in use.
  quint32 cur = s_nextStableId.load();
  while (cur <= id && !s_nextStableId.compare_exchange_weak(cur, id))
    ;
}

void GameObject::invalidateTransform(const bool moved) {
  m_transformDirty = true;
  m_aabbDirty = true;
  ++m_transformVersion;
  if (moved)
    ++m_placementVersion;
  ++s_transformEpoch;
}

glm::mat4 GameObject::getModelMatrix() const {
  if (!m_transformDirty)
    return m_modelMatrix;

  glm::mat4 model(1.0f);
  model = glm::translate(model, m_position);
  model = glm::rotate(model, glm::radians(m_rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
  model = glm::rotate(model, glm::radians(m_rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
  model = glm::rotate(model, glm::radians(m_rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
  model = glm::scale(model, m_scale);

  m_modelMatrix = model;
  m_transformDirty = false;
  return m_modelMatrix;
}

std::pair<glm::vec3, glm::vec3> GameObject::computeAABB() const {
  if (!m_mesh)
    return {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};

  const auto local = m_mesh->getAABB();
  const glm::mat4 model = getModelMatrix();

  glm::vec3 minV = glm::vec3(model * glm::vec4(local.first, 1.0f));
  glm::vec3 maxV = minV;
  for (int i = 0; i < 8; ++i) {
    glm::vec3 corner = local.first;
    if (i & 1)
      corner.x = local.second.x;
    if (i & 2)
      corner.y = local.second.y;
    if (i & 4)
      corner.z = local.second.z;
    const glm::vec3 worldCorner = glm::vec3(model * glm::vec4(corner, 1.0f));
    minV = glm::min(minV, worldCorner);
    maxV = glm::max(maxV, worldCorner);
  }
  return {minV, maxV};
}

std::pair<glm::vec3, glm::vec3> GameObject::getWorldAABB() const {
  if (m_aabbDirty) {
    m_cachedWorldAABB = computeAABB();
    m_aabbDirty = false;
  }
  return m_cachedWorldAABB;
}

} // namespace eng
