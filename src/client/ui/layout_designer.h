#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QStringList>
#include <QVariant>

#include <vector>

#include "client/engine/layout_gen.h"

class QQuickWidget;
class QSplitter;
class QLabel;
class Ctx;

namespace vk {
class Engine3D;
}

namespace ui {

class LayoutPanel;

// The Layout Designer's document and tools. Edits are snapshots on the designer's own undo stack;
// Generate makes the brushes as one step of the map's undo history.
class LayoutModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
  Q_PROPERTY(int gridSize READ gridSize WRITE setGridSize NOTIFY gridChanged)
  Q_PROPERTY(bool showDimensions READ showDimensions WRITE setShowDimensions NOTIFY toolChanged)
  Q_PROPERTY(bool live READ live WRITE setLive NOTIFY toolChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)

public:
  enum Tool { ToolSelect = 0, ToolCorridor, ToolRoom, ToolVent, ToolSeal, ToolErase };
  Q_ENUM(Tool)

  static const std::vector<int> &corridorWidths();
  static const std::vector<int> &ventWidths();
  static const std::vector<int> &gridSizes();

  explicit LayoutModel(vk::Engine3D *engine, QObject *parent = nullptr);

  [[nodiscard]] const eng::layout::Doc &doc() const { return m_doc; }
  [[nodiscard]] const eng::layout::Raster &raster() const;
  [[nodiscard]] int revision() const { return m_revision; }
  [[nodiscard]] int tool() const { return m_tool; }
  void setTool(int t);
  [[nodiscard]] int gridSize() const { return m_grid; }
  void setGridSize(int g);
  Q_INVOKABLE void stepGrid(int dir);
  [[nodiscard]] bool showDimensions() const { return m_showDims; }
  void setShowDimensions(bool on);
  [[nodiscard]] bool live() const { return m_live; }
  void setLive(bool on);
  [[nodiscard]] QString status() const { return m_status; }
  void setStatus(const QString &s);
  [[nodiscard]] QString lastResult() const { return m_lastResult; }
  [[nodiscard]] bool canUndo() const { return m_histPos > 0; }
  [[nodiscard]] bool canRedo() const { return m_histPos + 1 < int(m_history.size()); }

  // Selection: path segments (path index, segment index) or rooms.
  struct SegRef {
    int path = -1, seg = -1;
    bool operator==(const SegRef &o) const { return path == o.path && seg == o.seg; }
  };
  [[nodiscard]] const std::vector<SegRef> &selectedSegs() const { return m_selSegs; }
  [[nodiscard]] const std::vector<int> &selectedRooms() const { return m_selRooms; }
  [[nodiscard]] bool isSegSelected(int path, int seg) const;
  [[nodiscard]] bool isPathSelected(int path) const;
  // -1 none, 0 corridor segments, 1 rooms, 2 vent segments.
  [[nodiscard]] int selKind() const;
  [[nodiscard]] int selCount() const { return int(m_selSegs.size() + m_selRooms.size()); }
  // INT_MIN = mixed, -1 = nothing to show.
  [[nodiscard]] int selWidth() const;
  [[nodiscard]] int selHeight() const;
  [[nodiscard]] int selElevation() const;
  [[nodiscard]] int selLength() const;
  [[nodiscard]] QSize selRoomSize() const;
  [[nodiscard]] int selContact() const;

  [[nodiscard]] QStringList layoutNames() const;
  [[nodiscard]] int layoutIndex() const;
  [[nodiscard]] QStringList groupNames() const;
  [[nodiscard]] QList<int> groupIds() const;

  [[nodiscard]] QVariant cfg(const QString &key) const;
  void setCfg(const QString &key, const QVariant &value);
  void setSelWidth(int w);
  void setSelHeight(int h);
  void setSelElevation(int e);
  void setSelLength(int len);
  void setSelRoomSize(int w, int d);
  void setSelContact(int contact);
  void deleteSelection();
  void undo();
  void redo();
  void clearAll();
  void newLayout();
  // Another level: list its layouts and start a new one.
  void levelChanged();
  void loadLayout(int index);
  void loadLayoutById(int id);
  void generate();
  // A layout not in the map yet goes to the 3D camera, snapped to 32 units.
  void placeAtCamera();
  void removeFromMap();
  void useCurrentMaterial(const QString &slot);
  [[nodiscard]] int drawWidth() const;

  // Edits used by the canvas. Coordinates are Source units.
  void addPath(eng::layout::Kind kind, const std::vector<QPoint> &nodes);
  void addRoomRect(const QRect &rect, bool toSelected, int contact);
  void toggleSeal(const QRect &square);
  bool eraseAt(const glm::dvec2 &su);
  void selectAt(const glm::dvec2 &su, bool additive, bool wholePath);
  void clearSelection();
  [[nodiscard]] SegRef segmentAt(const glm::dvec2 &su) const;
  [[nodiscard]] int roomAt(const glm::dvec2 &su) const;
  // Live drags edit a copy of the drag-start document, committed as one entry.
  void beginPreview();
  [[nodiscard]] const eng::layout::Doc &previewBase() const { return m_previewBase; }
  void setPreview(const eng::layout::Doc &d);
  void commitPreview();
  void refreshLayouts();

signals:
  void changed();
  void toolChanged();
  void gridChanged();
  void statusChanged();
  void layoutsChanged();

private:
  void pushHistory();
  void restore(int pos);
  void touched(bool history = true);
  void cleanPaths();

  vk::Engine3D *m_engine;
  eng::layout::Doc m_doc;
  eng::layout::Doc m_previewBase;
  mutable eng::layout::Raster m_raster;
  mutable int m_rasterRev = -1;
  int m_revision = 0;
  int m_tool = ToolCorridor;
  int m_grid = 64;
  bool m_showDims = true;
  bool m_live = false;
  QString m_status;
  QString m_lastResult;
  std::vector<QJsonObject> m_history;
  int m_histPos = -1;
  std::vector<SegRef> m_selSegs;
  std::vector<int> m_selRooms;
  QList<QJsonObject> m_layouts;
  bool m_previewing = false;
};

// The top-down grid the layout is drawn on. Plan coordinates are Source units
// (x right, y down = world +Z).
class LayoutCanvas : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QObject *model READ model WRITE setModel NOTIFY modelChanged)

public:
  explicit LayoutCanvas(QQuickItem *parent = nullptr);
  [[nodiscard]] QObject *model() const { return m_model; }
  void setModel(QObject *m);
  void paint(QPainter *p) override;
  Q_INVOKABLE void fitAll();

signals:
  void modelChanged();

protected:
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseDoubleClickEvent(QMouseEvent *e) override;
  void hoverMoveEvent(QHoverEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;

private:
  [[nodiscard]] glm::dvec2 toPlan(const QPointF &px) const;
  [[nodiscard]] QPointF toScreen(const glm::dvec2 &su) const;
  [[nodiscard]] QPoint nearestNode(const glm::dvec2 &su) const;
  [[nodiscard]] QPoint cellAt(const glm::dvec2 &su) const;
  void updateStroke(const glm::dvec2 &su, bool diagonal);
  void updateStatus(const glm::dvec2 &su);
  void dragNode(const glm::dvec2 &su, bool free);
  void dragMove(const glm::dvec2 &su);
  void drawLabel(QPainter *p, const QPointF &at, const QString &text, const QColor &color) const;
  [[nodiscard]] int newRoomContact() const;

  QPointer<LayoutModel> m_model;
  glm::dvec2 m_center {0.0, 0.0};
  double m_zoom = 0.5; // pixels per Source unit
  enum class Drag { None, Pan, Stroke, Room, Erase, Width, RoomEdge, Node, Move, Select };
  Drag m_drag = Drag::None;
  QPointF m_lastPx;
  glm::dvec2 m_dragStart {0.0};
  std::vector<QPoint> m_stroke; // committed nodes of the stroke being drawn
  QPoint m_strokeEnd;
  int m_strokeAxis = -1; // 0 = x, 1 = y, 2 = diagonal
  QPoint m_roomStart, m_roomEnd;
  bool m_roomFlip = false;
  LayoutModel::SegRef m_widthSeg;
  int m_nodePath = -1, m_nodeIndex = -1;
  int m_edgeRoom = -1, m_edgeRect = -1, m_edgeSide = -1;
};

// A standalone top-level window (no Qt parent); one instance, reused by open().
class LayoutDesigner final : public QMainWindow {
  Q_OBJECT

public:
  static LayoutDesigner *open(vk::Engine3D *engine, Ctx *ctx, QWidget *owner, int layoutId = 0);
  static void closeIfOpen();
  [[nodiscard]] LayoutModel *model() const { return m_model; }

protected:
  void closeEvent(QCloseEvent *e) override;

private:
  LayoutDesigner(vk::Engine3D *engine, Ctx *ctx, QWidget *owner);
  void fitView();
  void saveWindowState();
  LayoutModel *m_model = nullptr;
  QQuickWidget *m_view = nullptr;
  QSplitter *m_split = nullptr;
  LayoutPanel *m_panel = nullptr;
  QLabel *m_statusText = nullptr;
  QLabel *m_resultText = nullptr;
};

} // namespace ui
