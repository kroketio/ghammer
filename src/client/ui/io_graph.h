#pragma once

// The I/O Editor's flow graph: entities as node cards, connections as curves. Positions are saved with
// the map (Engine3D::setIOGraphPos).

#include <QGraphicsView>
#include <QHash>
#include <QSet>

#include <vector>

#include "client/ui/io_ui_common.h"

class QGraphicsPathItem;
class QGraphicsEllipseItem;

namespace vk {
class Engine3D;
}

namespace ui {

class IONodeItem;
class IOEdgeItem;

class IOGraphView final : public QGraphicsView {
  Q_OBJECT

public:
  explicit IOGraphView(vk::Engine3D *engine, QWidget *parent = nullptr);

  // Plus pseudo nodes for runtime, @external and unresolved targets. Keeps the view.
  void rebuild(const QSet<quint32> &entities);
  void setHighlight(quint32 entity, const QVector<iocommon::ConnRef> &conns);
  // Flow (layered, left to right) or map (top view).
  void autoLayout(bool mapLayout);
  void fitAll();
  void centerOnEntity(quint32 id, bool onlyIfHidden);
  void updateSimulation();
  void clearSimulation();

signals:
  void entityClicked(quint32 id, bool additive);
  void entityActivated(quint32 id); // double-click
  void connectionsClicked(const QVector<iocommon::ConnRef> &refs, bool additive);
  void backgroundClicked();
  // to == 0: dropped on empty space at scenePos. `output` is empty from the "+ connect" row.
  void connectRequested(quint32 from, const QString &output, quint32 to, const QPointF &scenePos,
                        const QPoint &globalPos);
  void entityMenuRequested(quint32 id, const QPoint &globalPos);
  void connectionMenuRequested(const iocommon::ConnRef &ref, const QPoint &globalPos);
  void backgroundMenuRequested(const QPointF &scenePos, const QPoint &globalPos);
  void deleteRequested();

protected:
  void drawBackground(QPainter *p, const QRectF &rect) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseDoubleClickEvent(QMouseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;
  void contextMenuEvent(QContextMenuEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;

private:
  IONodeItem *nodeAt(const QPoint &viewPos) const;
  IOEdgeItem *edgeAt(const QPoint &viewPos) const;
  void placeNewNodes(const std::vector<IONodeItem *> &unplaced);
  std::vector<QPointF> layeredPositions(const std::vector<IONodeItem *> &nodes) const;
  void updateSceneRect();

  vk::Engine3D *m_engine;
  QGraphicsScene *m_scene;
  QHash<QString, IONodeItem *> m_nodes; // "e<id>" for entities, "x:<target>" for pseudo targets
  std::vector<IOEdgeItem *> m_edges;
  std::vector<QGraphicsEllipseItem *> m_pulses;
  quint32 m_highlightEntity = 0;
  QVector<iocommon::ConnRef> m_highlightConns;

  enum class Drag { None, Pan, Move, Wire } m_drag = Drag::None;
  double m_uiScale = 1.0;  // already applied to the transform
  QPoint m_lastPos;
  QPointF m_grabOffset;
  bool m_moved = false;
  IONodeItem *m_dragNode = nullptr;
  QString m_wireOutput;
  QGraphicsPathItem *m_wire = nullptr;
  IONodeItem *m_wireHover = nullptr;
};

} // namespace ui
