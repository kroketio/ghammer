#pragma once

#include "abstract_light.h"

namespace eng {
class DirectionalLight: public AbstractLight {
  Q_OBJECT

public:
  DirectionalLight(QObject* parent = 0);
  DirectionalLight(QVector3D color, QVector3D direction, QObject* parent = 0);
  DirectionalLight(const DirectionalLight& light);
  ~DirectionalLight();

  QVector3D direction();

public slots:
  void setDirection(QVector3D direction);

signals:
  void directionChanged(QVector3D direction);

protected:
  QVector3D m_direction;
};
}