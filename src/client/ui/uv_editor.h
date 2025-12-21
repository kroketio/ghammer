#pragma once

#include <QHash>
#include <QImage>
#include <QWidget>

#include <glm/glm.hpp>

class QLabel;

namespace vk {
class Engine3D;
}

namespace ui {

// The selected face in texture space over the tiled texture. Each drag is one undo step.
class UvCanvas final : public QWidget {
  Q_OBJECT

public:
  explicit UvCanvas(vk::Engine3D *engine, QWidget *parent = nullptr);
  void refresh();

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;
  void resizeEvent(QResizeEvent *) override;

private:
  [[nodiscard]] QPointF toScreen(const glm::dvec2 &uv) const;
  [[nodiscard]] glm::dvec2 toUv(const QPointF &px) const;
  void fit();
  QImage textureFor(const QString &name, const QString &path);

  vk::Engine3D *m_engine;
  glm::dvec2 m_center {0.5, 0.5};
  double m_zoom = 160.0; // pixels per tile
  quint64 m_fittedFor = 0;
  enum class Drag { None, Offset, Rotate, Scale, Pan } m_drag = Drag::None;
  QPointF m_press, m_last;
  glm::dvec2 m_pressUv {0.0};
  QHash<QString, QImage> m_images;
};

class UvEditor final : public QWidget {
  Q_OBJECT

public:
  explicit UvEditor(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();
  vk::Engine3D *m_engine;
  UvCanvas *m_canvas;
  QLabel *m_info;
};

} // namespace ui
