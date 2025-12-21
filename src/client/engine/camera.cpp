#include "camera.h"
#include <glm/gtx/euler_angles.hpp>
#include <cmath>

namespace eng {

  // Godot's world: +Y up, right-handed. Yaw -90 looks down +Z.
  static const glm::vec3 kWorldUp(0.0f, 1.0f, 0.0f);

  Camera::Camera(glm::vec3 position) :
    m_position(position), m_yaw(-90.0f), m_pitch(0.0f) {
  }

  void Camera::moveForward(float delta, float speed) {
    m_position += getDirection() * speed * delta;
  }

  void Camera::moveBackward(float delta, float speed) {
    m_position -= getDirection() * speed * delta;
  }

  void Camera::moveLeft(float delta, float speed) {
    m_position -= getRight() * speed * delta;
  }

  void Camera::moveRight(float delta, float speed) {
    m_position += getRight() * speed * delta;
  }

  void Camera::moveUp(float delta, float speed) {
    m_position += kWorldUp * speed * delta;
  }

  void Camera::moveDown(float delta, float speed) {
    m_position -= kWorldUp * speed * delta;
  }

  void Camera::rotate(float deltaYaw, float deltaPitch) {
    m_yaw   += deltaYaw * m_mouseSensitivity;

    m_pitch += (m_invertY ? -deltaPitch : deltaPitch) * m_mouseSensitivity;

    if (m_pitch > 89.0f) m_pitch = 89.0f;
    if (m_pitch < -89.0f) m_pitch = -89.0f;
  }

  glm::vec3 Camera::getDirection() const {
    float yawRad = glm::radians(m_yaw);
    float pitchRad = glm::radians(m_pitch);
    glm::vec3 dir;
    dir.x = std::cos(yawRad) * std::cos(pitchRad);
    dir.y = std::sin(pitchRad);
    dir.z = -std::sin(yawRad) * std::cos(pitchRad);
    return glm::normalize(dir);
  }

  glm::vec3 Camera::getRight() const {
    glm::vec3 dir = getDirection();
    dir.y = 0.0f;                 // flatten direction to the XZ plane
    if (glm::length(dir) < 0.001f) dir = glm::vec3(1, 0, 0);
    return glm::normalize(glm::cross(dir, kWorldUp));
  }

  glm::vec3 Camera::getUp() const {
    return glm::normalize(glm::cross(getRight(), getDirection()));
  }

  glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(m_position, m_position + getDirection(), kWorldUp);
  }

  glm::mat4 Camera::getProjectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(m_fov), aspectRatio, m_near, m_far);
  }

} // namespace eng
