#pragma once

#include <array>

#include <glm/glm.hpp>

namespace vk {

// Gribb-Hartmann plane extraction for column-major view-projection matrices,
// with the classic p-vertex AABB test.
class Frustum {
public:
  std::array<glm::vec4, 6> planes {};

  void extract(const glm::mat4 &vp) {
    auto normalizePlane = [](glm::vec4 &p) {
      const float len = glm::length(glm::vec3(p));
      if (len > 1e-8f)
        p /= len;
    };

    planes[0] = glm::vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
    planes[1] = glm::vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
    planes[2] = glm::vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
    planes[3] = glm::vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
    planes[4] = glm::vec4(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2], vp[3][3] + vp[3][2]);
    planes[5] = glm::vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);

    for (auto &p: planes)
      normalizePlane(p);
  }

  [[nodiscard]] bool isBoxVisible(const glm::vec3 &min, const glm::vec3 &max) const {
    for (const auto &plane: planes) {
      const glm::vec3 n(plane);
      glm::vec3 p = min;
      if (n.x >= 0.0f)
        p.x = max.x;
      if (n.y >= 0.0f)
        p.y = max.y;
      if (n.z >= 0.0f)
        p.z = max.z;
      if (glm::dot(n, p) + plane.w < -1e-5f)
        return false;
    }
    return true;
  }
};

// Slab test; returns true when the ray hits the box and reports the entry
// distance in tEnter (which may be negative when the origin is inside).
inline bool rayAabb(const glm::vec3 &origin, const glm::vec3 &invDir,
                   const glm::vec3 &min, const glm::vec3 &max,
                   float &tEnter, float &tExit) {
  tEnter = 0.0f;
  tExit = 1e30f;
  for (int i = 0; i < 3; ++i) {
    float t1 = (min[i] - origin[i]) * invDir[i];
    float t2 = (max[i] - origin[i]) * invDir[i];
    if (t1 > t2)
      std::swap(t1, t2);
    tEnter = std::max(tEnter, t1);
    tExit = std::min(tExit, t2);
    if (tEnter > tExit)
      return false;
  }
  return true;
}

// Moller-Trumbore; returns the hit distance when the ray hits the triangle.
inline bool rayTriangle(const glm::vec3 &origin, const glm::vec3 &dir,
                       const glm::vec3 &v0, const glm::vec3 &v1, const glm::vec3 &v2,
                       float &t) {
  const glm::vec3 e1 = v1 - v0;
  const glm::vec3 e2 = v2 - v0;
  const glm::vec3 pvec = glm::cross(dir, e2);
  const float det = glm::dot(e1, pvec);
  if (std::fabs(det) < 1e-9f)
    return false;

  const float invDet = 1.0f / det;
  const glm::vec3 tvec = origin - v0;
  const float u = glm::dot(tvec, pvec) * invDet;
  if (u < -1e-6f || u > 1.0f + 1e-6f)
    return false;

  const glm::vec3 qvec = glm::cross(tvec, e1);
  const float v = glm::dot(dir, qvec) * invDet;
  if (v < -1e-6f || u + v > 1.0f + 1e-6f)
    return false;

  t = glm::dot(e2, qvec) * invDet;
  return t > 1e-6f;
}

} // namespace vk
