#include "directional_light.h"

#include "shared/lib/utils.h"

namespace eng {
  DirectionalLight::DirectionalLight(QObject * parent) : AbstractLight() {
    // Matches the renderer's default sun: azimuth 56, elevation 32.5.
    m_color = QVector3D(1.0f, 0.969f, 0.902f);
    m_direction = QVector3D(-0.471619f, -0.537300f, 0.699203f);
    m_intensity = 5.0f;
    setObjectName("Untitled Directional Light");
    setParent(parent);
  }

  DirectionalLight::DirectionalLight(QVector3D color, QVector3D direction, QObject * parent): AbstractLight(color) {
    m_direction = direction;
    setObjectName("Untitled Directional Light");
    setParent(parent);
  }

  DirectionalLight::DirectionalLight(const DirectionalLight & light): AbstractLight(light) {
    m_direction = light.m_direction;
    setObjectName(light.objectName());
  }

  DirectionalLight::~DirectionalLight() {}

  QVector3D DirectionalLight::direction() {
    return m_direction;
  }

  void DirectionalLight::setDirection(QVector3D direction) {
    if (!Utils::isEqual(m_direction, direction)) {
      m_direction = direction;
      directionChanged(m_direction);
    }
  }
}