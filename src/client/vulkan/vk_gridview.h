#pragma once

#include <set>

#include <QQuickRhiItem>
#include <QSharedPointer>
#include <QTimer>
#include <glm/glm.hpp>

#include "engine/scene.h"

namespace vk {

// One Hammer-style orthographic 2D view. The grid is procedural (fullscreen quad + fragment shader), so
// panning and zooming rebuild nothing.
class GridView : public QQuickRhiItem {
  Q_OBJECT

  Q_PROPERTY(Perspective perspective READ perspective WRITE setPerspective NOTIFY perspectiveChanged)
  Q_PROPERTY(bool showGrid READ showGrid WRITE setShowGrid NOTIFY showGridChanged)
  Q_PROPERTY(double pixelsPerSourceUnit READ pixelsPerSourceUnit NOTIFY pixelsPerSourceUnitChanged)
  Q_PROPERTY(double gridStepSource READ gridStepSource NOTIFY gridStepSourceChanged)
  Q_PROPERTY(int gridLevel READ gridLevel NOTIFY gridStepSourceChanged)
  // Qt::CursorShape for whatever the mouse is over; the QML MouseArea reads it back.
  Q_PROPERTY(int hoverCursor READ hoverCursor NOTIFY hoverHandleChanged)
  // The view's MouseArea, which gets the rotate pixmap cursor (QML's cursorShape only takes standard shapes).
  Q_PROPERTY(QQuickItem *cursorTarget READ cursorTarget WRITE setCursorTarget)

public:
  enum class Perspective {
    Top = 0,   // right +X, up +Y, depth -Z
    Front = 1, // right +X, up +Z, depth +Y
    Side = 2   // right +Y, up +Z, depth -X
  };
  Q_ENUM(Perspective)

  static constexpr double kUnitsPerWorldUnit = 10.0;
  // Source-unit spacing of the grid at level 0; every level doubles it.
  static constexpr double kBaseGridStepSource = 16.0;
  // A major line every 8 minor ones; snapping uses the major spacing.
  static constexpr double kMajorGridEvery = 8.0;
  // Resize handle size on screen.
  static constexpr double kHandleSizePx = 10.0;
  // Zoom of an empty view: a 32-unit cell is about 19 px.
  static constexpr double kDefaultPxPerWorldUnit = 6.0;

  explicit GridView(QQuickItem *parent = nullptr);
  ~GridView() override;
  QQuickRhiItemRenderer *createRenderer() override;

  [[nodiscard]] Perspective perspective() const { return m_perspective; }
  void setPerspective(Perspective perspective);

  [[nodiscard]] bool showGrid() const { return m_showGrid; }
  void setShowGrid(bool showGrid);

  [[nodiscard]] double pixelsPerSourceUnit() const { return m_pxPerWorldUnit / kUnitsPerWorldUnit; }
  [[nodiscard]] double gridStepSource() const;
  [[nodiscard]] int gridLevelOffset() const { return m_gridLevelOffset; }
  // 1-based level of the current step: 1 -> 1x1 source unit, 4 -> 8x8.
  [[nodiscard]] int gridLevel() const;

  Q_INVOKABLE void zoomAt(qreal mouseX, qreal mouseY, qreal factor);
  Q_INVOKABLE void panBy(qreal dxPixels, qreal dyPixels);
  // +1 coarser, -1 finer; the engine owns the level shared by all views.
  Q_INVOKABLE void stepGridLevel(int direction);
  Q_INVOKABLE void fitToScene();
  Q_INVOKABLE void resetView();

  // Saved/restored view placement: centre, zoom and grid visibility (the engine owns the grid level).
  [[nodiscard]] double viewCenterA() const { return m_center.x; }
  [[nodiscard]] double viewCenterB() const { return m_center.y; }
  [[nodiscard]] double pixelsPerWorldUnit() const { return m_pxPerWorldUnit; }
  Q_INVOKABLE void restoreView(double centerA, double centerB, double pxPerWorldUnit, bool showGrid);

  // Every live 2D view, in creation order.
  [[nodiscard]] static const QVector<GridView *> &instances();

  // Left-drag: decides marquee / slide / move here; snapping and applying live on Engine3D.
  // `additive` (Shift) adds to the selection.
  Q_INVOKABLE void beginLeftDrag(qreal x, qreal y, bool additive = false);
  // `duplicate` is Shift read live on every move: Shift arriving mid-drag starts the clone.
  Q_INVOKABLE void updateLeftDrag(qreal x, qreal y, bool duplicate = false);
  Q_INVOKABLE void endLeftDrag(qreal x, qreal y, bool additive = false);

  Q_INVOKABLE void hoverAt(qreal x, qreal y);
  Q_INVOKABLE void endHover();
  // Finishes the path being drawn with the pen.
  Q_INVOKABLE void doubleClickAt(qreal x, qreal y);
  [[nodiscard]] int hoverCursor() const;
  [[nodiscard]] QQuickItem *cursorTarget() const { return m_cursorTarget; }
  void setCursorTarget(QQuickItem *item) { m_cursorTarget = item; }
  // Rotate mode applies here (gizmo Rotate, Object mode, no marquee, no vertex tool).
  [[nodiscard]] bool rotateHandles() const;

protected:
  void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
  void itemChange(ItemChange change, const ItemChangeData &value) override;

signals:
  void perspectiveChanged(Perspective perspective);
  void showGridChanged(bool showGrid);
  void pixelsPerSourceUnitChanged(double pixelsPerSourceUnit);
  void gridStepSourceChanged(double gridStepSource);
  void hoverHandleChanged();

private slots:
  void pollScene();

private:
  friend class GridViewRenderer;

  [[nodiscard]] QSharedPointer<eng::Scene> resolveScene() const;
  [[nodiscard]] std::set<uint32_t> resolveSelection() const;
  [[nodiscard]] glm::dvec2 planeAt(qreal x, qreal y) const;
  [[nodiscard]] bool insideMarquee(const glm::dvec2 &p) const;
  [[nodiscard]] bool insideSelected(const glm::dvec2 &p) const;
  [[nodiscard]] bool selectionBox(glm::dvec4 &out) const;
  // The box the resize handles act on: the marquee, else the selected meshes' bounds (entities excluded).
  // False when there is nothing resizable.
  [[nodiscard]] bool resizeBox(glm::dvec4 &out) const;
  [[nodiscard]] double handleSizeWorld() const;
  // vk::SelectionHandle under this plane point, or -1.
  [[nodiscard]] int hitTestHandle(const glm::dvec2 &p) const;
  // Measured against the rect the renderer draws (markerRectOnPlane()).
  [[nodiscard]] bool entityMarkerHit(const glm::dvec2 &p) const;
  mutable quint32 m_lastMarkerHit = 0;
  // Selected oriented entities show a rotate knob (Object mode, no tool owns the button).
  [[nodiscard]] bool entityKnobs() const;
  // The anchor handle under this plane point (Engine3D::anchorHandles2D), or 0.
  [[nodiscard]] quint32 anchorHandleHit(const glm::dvec2 &p) const;
  // The knob under this plane point, with its entity's centre and arrow direction in the plane.
  [[nodiscard]] bool entityKnobHit(const glm::dvec2 &p, quint32 &id, glm::dvec2 &centre, glm::dvec2 &dir) const;
  void pickAt(const glm::dvec2 &p, bool additive);
  void sceneDepthRange(double &d0, double &d1) const;
  void applyZoom(double pxPerWorldUnit);
  void applyGridLevel(int offset);
  void forceRepaints();

  Perspective m_perspective {Perspective::Top};
  glm::dvec2 m_center {0.0, 0.0};
  double m_pxPerWorldUnit {kDefaultPxPerWorldUnit};
  int m_gridLevelOffset {0};
  bool m_showGrid {true};
  bool m_fitted {false};
  bool m_userAdjusted {false};

  QSharedPointer<eng::Scene> m_scene;
  std::set<uint32_t> m_selection;
  int m_hoverHandle {-1};
  bool m_cutHoleHover {false};
  bool m_mirrorLineHover {false}; // pointer on the mirror line (it can be dragged)
  bool m_pivotDrag {false};
  // 2D rotate: the drag, and the box the handles keep meanwhile.
  bool m_rotating {false};
  glm::dvec4 m_rotateBox {0.0};
  // Turning entities by their knob, and the pointer resting on a knob.
  bool m_entityRotating {false};
  bool m_knobHover {false};
  // Dragging an anchor handle, and the pointer resting on one.
  bool m_anchorDragging {false};
  bool m_anchorHover {false};
  // An unmoved press inside the selection: released in place it toggles rotate mode.
  bool m_clickToggle {false};
  // An unmoved Shift press on a selected entity: released in place it deselects that entity.
  quint32 m_markerToggle {0};
  QPointF m_pressPx;
  QQuickItem *m_cursorTarget {nullptr};
  void applyCursor();
  bool m_vertexBox {false};
  quint64 m_pollVertexEpoch = 0;
  quint64 m_pollXforms = 0;
  quint64 m_pollGeom = 0;
  QTimer m_pollTimer;
};

} // namespace vk
