#pragma once

#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace eng {

class Camera {
public:
  explicit Camera(glm::vec3 position = glm::vec3(0.0f, 3.0f, 0.0f));

  void moveForward(float delta, float speed);
  void moveBackward(float delta, float speed);
  void moveLeft(float delta, float speed);
  void moveRight(float delta, float speed);
  void moveUp(float delta, float speed);
  void moveDown(float delta, float speed);

  void rotate(float deltaYaw, float deltaPitch);

  glm::mat4 getViewMatrix() const;
  glm::mat4 getProjectionMatrix(float aspectRatio) const;

  glm::vec3 getPosition() const { return m_position; }
  glm::vec3 getDirection() const;
  glm::vec3 getRight() const;
  glm::vec3 getUp() const;

  void setFOV(const float fov) { m_fov = fov; }
  [[nodiscard]] float getFov() const { return m_fov; }
  [[nodiscard]] float getYaw() const { return m_yaw; }
  [[nodiscard]] float getPitch() const { return m_pitch; }
  void setPose(const glm::vec3 &position, const float yaw, const float pitch) {
    m_position = position;
    m_yaw = yaw;
    m_pitch = pitch;
  }
  [[nodiscard]] float getFar() const { return m_far; }
  [[nodiscard]] float getNear() const { return m_near; }
  void setClipPlanes(float nearPlane, float farPlane) {
    m_near = std::max(0.001f, nearPlane);
    m_far = std::max(m_near * 10.0f, farPlane);
  }
  void setMouseSensitivity(float s) { m_mouseSensitivity = s; }
  void setInvertY(bool on) { m_invertY = on; }

private:
  glm::vec3 m_position;
  float m_yaw;   // Rotation around Y axis (left/right)
  float m_pitch; // Rotation around X axis (up/down)

  float m_mouseSensitivity = 0.1f;
  bool m_invertY = false;
  float m_fov = 70.0f;
  // Fitted to the render distance by the engine, for depth precision.
  float m_near = 0.1f;
  float m_far = 1000.0f;
};

} // namespace eng
