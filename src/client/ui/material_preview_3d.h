#pragma once

#include <QQuickWidget>

#include "client/engine/material.h"

#include "shared/models/material_props.h"
#include "shared/models/texture_entry.h"

namespace vk {
class Engine3D;
}

namespace ui {

// A texture on a lit cube, sphere or plane (qml/MaterialPreview3D.qml). Loaded on first show.
class MaterialPreview3D final : public QQuickWidget {
  Q_OBJECT

public:
  enum class Shape { Cube, Sphere, Plane };
  enum class Lighting { Sun, Omni };

  explicit MaterialPreview3D(QWidget *parent = nullptr);

  // `props`: resolved, every key set.
  void showTexture(const TextureEntry &entry, const MaterialProps &props);
  void clear();

  void setShape(Shape s);
  void setLighting(Lighting l);
  [[nodiscard]] Shape shape() const { return m_shape; }
  [[nodiscard]] Lighting lighting() const { return m_lighting; }

protected:
  void showEvent(QShowEvent *e) override;

private slots:
  void updateView();

private:
  void ensureLoaded();
  void rebuildScene();
  void updatePending();

  TextureEntry m_entry;
  MaterialProps m_props;
  eng::MaterialPtr m_material;
  vk::Engine3D *m_engine = nullptr;
  class QTimer *m_pendingPoll = nullptr;
  Shape m_shape = Shape::Cube;
  Lighting m_lighting = Lighting::Sun;
  float m_sunIntensity = 1.0f;
  bool m_loaded = false;
};

} // namespace ui
