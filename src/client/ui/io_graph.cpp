#include "io_graph.h"
#include "client/ui/scale.h"

#include <QContextMenuEvent>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <cmath>
#include <map>

#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

using namespace iocommon;

namespace {

constexpr qreal kNodeW = 210.0;
constexpr qreal kHeaderH = 40.0;
constexpr qreal kRowH = 20.0;
constexpr qreal kFooterH = 18.0;
constexpr qreal kPortR = 4.5;
constexpr qreal kGapX = 120.0;
constexpr qreal kGapY = 28.0;
const QColor kCard(26, 32, 64);
const QColor kCardBorder(44, 53, 96);
const QColor kEdge(111, 159, 216);

QString keyFor(const quint32 id) { return QStringLiteral("e%1").arg(id); }

} // namespace

class IONodeItem final : public QGraphicsItem {
public:
  enum class Kind { Entity, Runtime, External, Broken };
  QString key;
  quint32 entity = 0;
  Kind kind = Kind::Entity;
  QString title, subtitle;
  QColor color {95, 211, 255};
  QPixmap icon;
  QStringList inputs, outputs;
  bool selected = false, flashing = false, problem = false, unnamed = false;
  QString state;
  std::vector<IOEdgeItem *> edges;

  void setSimState(bool flash, const QString &st) {
    if (flash == flashing && st == state)
      return;
    if (st.isEmpty() != state.isEmpty())
      prepareGeometryChange(); // the footer row comes or goes
    flashing = flash;
    state = st;
    update();
  }

  [[nodiscard]] bool pseudo() const { return kind != Kind::Entity; }
  [[nodiscard]] int bodyRows() const {
    return std::max<int>(int(inputs.size()), int(outputs.size()) + (pseudo() ? 0 : 1));
  }
  [[nodiscard]] qreal height() const {
    return kHeaderH + std::max(1, bodyRows()) * kRowH + 8.0 + (state.isEmpty() ? 0.0 : kFooterH);
  }
  QRectF boundingRect() const override { return QRectF(-8, -4, kNodeW + 16, height() + 8); }

  [[nodiscard]] QPointF inputPortLocal(const QString &name) const {
    const int i = std::max<int>(0, int(inputs.indexOf(name)));
    return {0.0, kHeaderH + 4.0 + i * kRowH + kRowH * 0.5};
  }
  [[nodiscard]] QPointF outputPortLocal(const QString &name) const {
    int i = int(outputs.indexOf(name));
    if (i < 0)
      i = int(outputs.size()); // the "+ connect" row
    return {kNodeW, kHeaderH + 4.0 + i * kRowH + kRowH * 0.5};
  }
  QPointF inputPort(const QString &n) const { return mapToScene(inputPortLocal(n)); }
  QPointF outputPort(const QString &n) const { return mapToScene(outputPortLocal(n)); }

  // Which output row (or "+ connect" = empty name with ok) a local point is on.
  bool outputAt(const QPointF &local, QString *name) const {
    if (pseudo() || local.x() < kNodeW * 0.45 || local.x() > kNodeW + 10)
      return false;
    const int row = int(std::floor((local.y() - kHeaderH - 4.0) / kRowH));
    if (row < 0 || row > outputs.size())
      return false;
    *name = row < outputs.size() ? outputs[row] : QString();
    return true;
  }

  void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r(0, 0, kNodeW, height());
    QColor border = selected ? kAccent : kCardBorder;
    if (flashing)
      border = QColor(255, 230, 90);
    QPen pen(border, selected || flashing ? 2.0 : 1.0);
    if (pseudo())
      pen.setStyle(Qt::DashLine);
    p->setPen(pen);
    p->setBrush(pseudo() ? QColor(20, 24, 46) : kCard);
    p->drawRoundedRect(r, 7, 7);

    // Header band in the class colour, clipped to the rounded corners.
    {
      QPainterPath clip;
      clip.addRoundedRect(r, 7, 7);
      p->save();
      p->setClipPath(clip);
      QColor hc = kind == Kind::Broken ? kBroken : kind == Kind::Entity ? color : kRuntime;
      hc.setAlphaF(0.28f);
      p->fillRect(QRectF(0, 0, kNodeW, kHeaderH), hc);
      p->restore();
    }
    qreal tx = 10;
    if (!icon.isNull()) {
      p->drawPixmap(QRectF(9, 10, 20, 20), icon, icon.rect());
      tx = 36;
    }
    // Scene units: fonts at normal-density sizes; the view transform carries the UI scale and zoom.
    QFont f = p->font();
    f.setPixelSize(12);
    f.setBold(true);
    p->setFont(f);
    p->setPen(kind == Kind::Broken ? kBroken : unnamed ? QColor(170, 180, 205) : QColor(232, 238, 250));
    const QFontMetrics fm(f);
    p->drawText(QRectF(tx, 4, kNodeW - tx - 20, 18), Qt::AlignVCenter | Qt::AlignLeft,
                fm.elidedText(title, Qt::ElideRight, int(kNodeW - tx - 20)));
    f.setBold(false);
    f.setPixelSize(10);
    p->setFont(f);
    p->setPen(kMuted);
    p->drawText(QRectF(tx, 20, kNodeW - tx - 8, 16), Qt::AlignVCenter | Qt::AlignLeft, subtitle);
    if (problem) {
      p->setPen(Qt::NoPen);
      p->setBrush(kBroken);
      p->drawEllipse(QPointF(kNodeW - 12, 12), 4, 4);
    }

    const QFontMetrics sm(f);
    for (int i = 0; i < inputs.size(); ++i) {
      const qreal y = kHeaderH + 4.0 + i * kRowH;
      p->setPen(QColor(255, 196, 140));
      p->drawText(QRectF(12, y, kNodeW * 0.5 - 14, kRowH), Qt::AlignVCenter | Qt::AlignLeft,
                  sm.elidedText(inputs[i], Qt::ElideRight, int(kNodeW * 0.5 - 14)));
      p->setPen(QPen(kIncoming.darker(140), 1));
      p->setBrush(kIncoming);
      p->drawEllipse(QPointF(0, y + kRowH * 0.5), kPortR, kPortR);
    }
    if (!pseudo()) {
      for (int i = 0; i <= outputs.size(); ++i) {
        const qreal y = kHeaderH + 4.0 + i * kRowH;
        const bool plus = i == outputs.size();
        p->setPen(plus ? kMuted : QColor(150, 220, 255));
        p->drawText(QRectF(kNodeW * 0.4, y, kNodeW * 0.6 - 12, kRowH), Qt::AlignVCenter | Qt::AlignRight,
                    plus ? QStringLiteral("+ connect") : sm.elidedText(outputs[i], Qt::ElideLeft, int(kNodeW * 0.6 - 12)));
        p->setPen(QPen(plus ? kMuted : kAccent.darker(140), 1));
        p->setBrush(plus ? Qt::NoBrush : QBrush(kAccent));
        p->drawEllipse(QPointF(kNodeW, y + kRowH * 0.5), kPortR, kPortR);
      }
    }
    if (!state.isEmpty()) {
      p->setPen(QColor(255, 230, 120));
      p->drawText(QRectF(10, height() - kFooterH - 2, kNodeW - 20, kFooterH), Qt::AlignVCenter | Qt::AlignLeft,
                  sm.elidedText(QStringLiteral("● ") + state, Qt::ElideRight, int(kNodeW - 20)));
    }
  }
};

class IOEdgeItem final : public QGraphicsItem {
public:
  IONodeItem *from = nullptr, *to = nullptr;
  QString output, input, label;
  ConnRef ref;
  QColor status = kOk;
  bool delayed = false, disabled = false, selected = false;
  int attached = 0; // 1 = leaves the highlighted entity, 2 = arrives at it
  QPainterPath path;

  void updatePath() {
    prepareGeometryChange();
    const QPointF a = from->outputPort(output);
    const QPointF b = to->inputPort(input);
    const qreal dx = b.x() > a.x() + 40 ? std::max(50.0, (b.x() - a.x()) * 0.5) : 140.0;
    path = QPainterPath(a);
    path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
  }
  QRectF boundingRect() const override { return path.boundingRect().adjusted(-40, -20, 40, 20); }
  QPainterPath shape() const override {
    QPainterPathStroker s;
    s.setWidth(12);
    return s.createStroke(path);
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
    p->setRenderHint(QPainter::Antialiasing);
    QColor c = kEdge;
    if (attached == 1)
      c = kAccent;
    else if (attached == 2)
      c = kIncoming;
    if (status == kBroken || status == kWarn)
      c = status;
    if (disabled)
      c = kDisabled;
    if (selected)
      c = QColor(255, 255, 255);
    QPen pen(c, selected ? 3.0 : (attached ? 2.2 : 1.6));
    if (disabled)
      pen.setStyle(Qt::DotLine);
    else if (delayed)
      pen.setDashPattern({6, 4});
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);
    // Arrowhead into the input port.
    const QPointF b = path.pointAtPercent(1.0);
    const QPointF dir = b - path.pointAtPercent(0.97);
    const qreal len = std::hypot(dir.x(), dir.y());
    if (len > 1e-3) {
      const QPointF d = dir / len, n(-d.y(), d.x());
      const QPointF tip = b - d * (kPortR + 1);
      QPolygonF tri {tip, tip - d * 9 + n * 4.5, tip - d * 9 - n * 4.5};
      p->setPen(Qt::NoPen);
      p->setBrush(c);
      p->drawPolygon(tri);
    }
    if (!label.isEmpty()) {
      QFont f = p->font();
      f.setPixelSize(10);
      p->setFont(f);
      const QFontMetrics fm(f);
      const QPointF m = path.pointAtPercent(0.5);
      const QRectF box(m.x() - fm.horizontalAdvance(label) * 0.5 - 6, m.y() - 9, fm.horizontalAdvance(label) + 12, 18);
      p->setPen(QPen(c, 1));
      p->setBrush(QColor(14, 18, 38));
      p->drawRoundedRect(box, 9, 9);
      p->setPen(QColor(220, 228, 245));
      p->drawText(box, Qt::AlignCenter, label);
    }
  }
};

IOGraphView::IOGraphView(vk::Engine3D *engine, QWidget *parent) : QGraphicsView(parent), m_engine(engine) {
  m_scene = new QGraphicsScene(this);
  setScene(m_scene);
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setFrameShape(QFrame::NoFrame);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  // The UI text scale is part of the view transform.
  scale::bind(this, [this] {
    const double k = scale::textFactor() / m_uiScale;
    m_uiScale = scale::textFactor();
    QGraphicsView::scale(k, k);
  });
}

void IOGraphView::drawBackground(QPainter *p, const QRectF &rect) {
  p->fillRect(rect, QColor(14, 18, 38));
  const qreal minor = 20.0, major = 100.0;
  const qreal scale = transform().m11();
  auto grid = [&](qreal step, const QColor &c) {
    if (step * scale < 6)
      return;
    p->setPen(QPen(c, 0));
    for (qreal x = std::floor(rect.left() / step) * step; x < rect.right(); x += step)
      p->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    for (qreal y = std::floor(rect.top() / step) * step; y < rect.bottom(); y += step)
      p->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
  };
  grid(minor, QColor(22, 27, 52));
  grid(major, QColor(30, 37, 70));
}

void IOGraphView::rebuild(const QSet<quint32> &entityIds) {
  const QPointF centre = mapToScene(viewport()->rect().center());
  m_wire = nullptr;
  m_wireHover = nullptr;
  m_dragNode = nullptr;
  m_drag = Drag::None;
  m_scene->clear();
  m_nodes.clear();
  m_edges.clear();
  m_pulses.clear();
  if (!m_engine)
    return;
  const auto &ents = entitiesOf(m_engine);

  auto entityNode = [&](const eng::Entity &e) {
    auto *n = new IONodeItem();
    n->key = keyFor(e.id);
    n->entity = e.id;
    const eng::EntityClass *c = e.cls();
    n->title = e.name.isEmpty() ? (c ? c->label : e.classname) : e.name;
    n->unnamed = e.name.isEmpty();
    n->subtitle = e.name.isEmpty() ? QStringLiteral("unnamed %1").arg(e.classname) : e.classname;
    n->color = c ? c->color : QColor(95, 211, 255);
    if (c)
      n->icon = ActionManager::icon(c->icon).pixmap(scale::px(40), scale::px(40));
    n->setZValue(1);
    m_scene->addItem(n);
    m_nodes.insert(n->key, n);
    return n;
  };
  for (const auto &e: ents)
    if (entityIds.contains(e.id))
      entityNode(e);

  auto pseudoNode = [&](const QString &target, IONodeItem::Kind kind) {
    const QString key = (kind == IONodeItem::Kind::Broken ? QStringLiteral("?:") : QStringLiteral("x:")) + target.toLower();
    if (IONodeItem *n = m_nodes.value(key))
      return n;
    auto *n = new IONodeItem();
    n->key = key;
    n->kind = kind;
    n->title = target;
    n->subtitle = kind == IONodeItem::Kind::Broken   ? QStringLiteral("matches no entity")
                  : kind == IONodeItem::Kind::External ? QStringLiteral("Godot group (outside the map)")
                                                       : QStringLiteral("known while the game runs");
    n->setZValue(1);
    m_scene->addItem(n);
    m_nodes.insert(key, n);
    return n;
  };

  // A node's rows come from the edges that touch it.
  struct Pending {
    IONodeItem *from, *to;
    QString output, input, label;
    ConnRef ref;
    QColor status;
    bool delayed, disabled;
  };
  std::vector<Pending> pending;
  for (const auto &e: ents) {
    IONodeItem *from = m_nodes.value(keyFor(e.id));
    if (!from)
      continue;
    for (int i = 0; i < e.outputs.size(); ++i) {
      const auto &c = e.outputs[i];
      const QColor st = statusColor(ents, e, i);
      QStringList bits;
      if (!c.param.isEmpty())
        bits << QStringLiteral("\"%1\"").arg(c.param);
      if (c.delay > 0.0f)
        bits << delayText(c.delay);
      if (c.times == 1)
        bits << QStringLiteral("once");
      else if (c.times > 1)
        bits << QStringLiteral("×%1").arg(c.times);
      const QString label = bits.join(QStringLiteral(" · "));
      const auto r = eng::io::resolve(ents, c.target, e.id);
      std::vector<IONodeItem *> targets;
      if (r.kind == eng::io::TargetKind::Resolved) {
        for (const quint32 t: r.ids)
          if (IONodeItem *tn = m_nodes.value(keyFor(t)))
            targets.push_back(tn);
      } else if (r.kind == eng::io::TargetKind::Unresolved) {
        targets.push_back(pseudoNode(c.target.isEmpty() ? QStringLiteral("(no target)") : c.target,
                                     IONodeItem::Kind::Broken));
      } else {
        targets.push_back(pseudoNode(c.target, r.kind == eng::io::TargetKind::External ? IONodeItem::Kind::External
                                                                                        : IONodeItem::Kind::Runtime));
      }
      for (IONodeItem *t: targets)
        pending.push_back({from, t, c.output, c.input.isEmpty() ? QStringLiteral("?") : c.input, label, {e.id, i}, st,
                           c.delay > 0.0f, c.disabled});
    }
  }
  for (const auto &pe: pending) {
    if (!pe.from->outputs.contains(pe.output))
      pe.from->outputs << pe.output;
    if (!pe.to->inputs.contains(pe.input))
      pe.to->inputs << pe.input;
    if (pe.status == kBroken || pe.status == kWarn)
      pe.from->problem = true;
  }
  for (const auto &pe: pending) {
    auto *ed = new IOEdgeItem();
    ed->from = pe.from;
    ed->to = pe.to;
    ed->output = pe.output;
    ed->input = pe.input;
    ed->label = pe.label;
    ed->ref = pe.ref;
    ed->status = pe.status;
    ed->delayed = pe.delayed;
    ed->disabled = pe.disabled;
    ed->setZValue(0);
    pe.from->edges.push_back(ed);
    if (pe.to != pe.from)
      pe.to->edges.push_back(ed);
    m_scene->addItem(ed);
    m_edges.push_back(ed);
  }

  std::vector<IONodeItem *> unplaced;
  for (IONodeItem *n: std::as_const(m_nodes)) {
    glm::vec2 pos;
    if (!n->pseudo() && m_engine->ioGraphPos(n->entity, &pos))
      n->setPos(pos.x, pos.y);
    else
      unplaced.push_back(n);
  }
  if (!unplaced.empty())
    placeNewNodes(unplaced);
  for (IOEdgeItem *ed: m_edges)
    ed->updatePath();
  setHighlight(m_highlightEntity, m_highlightConns);
  updateSceneRect();
  centerOn(centre);
}

void IOGraphView::updateSceneRect() {
  const QRectF r = m_scene->itemsBoundingRect().adjusted(-2000, -2000, 2000, 2000);
  m_scene->setSceneRect(r.united(QRectF(-2000, -2000, 4000, 4000)));
}

void IOGraphView::setHighlight(const quint32 entity, const QVector<ConnRef> &conns) {
  m_highlightEntity = entity;
  m_highlightConns = conns;
  for (IONodeItem *n: std::as_const(m_nodes)) {
    const bool sel = !n->pseudo() && n->entity == entity && entity != 0;
    if (sel != n->selected) {
      n->selected = sel;
      n->update();
    }
  }
  for (IOEdgeItem *ed: m_edges) {
    ed->selected = conns.contains(ed->ref);
    ed->attached = ed->from->entity == entity && entity ? 1 : (!ed->to->pseudo() && ed->to->entity == entity && entity ? 2 : 0);
    ed->setZValue(ed->selected ? 0.6 : ed->attached ? 0.3 : 0.0);
    ed->update();
  }
}

// Longest-path layering per connected component, barycentre ordering, then the
// components stacked top to bottom (single nodes gathered in a grid at the end).
std::vector<QPointF> IOGraphView::layeredPositions(const std::vector<IONodeItem *> &nodes) const {
  const int n = int(nodes.size());
  std::vector<QPointF> out(static_cast<size_t>(n));
  QHash<IONodeItem *, int> idx;
  for (int i = 0; i < n; ++i)
    idx.insert(nodes[size_t(i)], i);
  const size_t count = size_t(n);
  std::vector<std::vector<int>> succ(count), pred(count), undirected(count);
  for (IOEdgeItem *ed: m_edges) {
    const int a = idx.value(ed->from, -1), b = idx.value(ed->to, -1);
    if (a < 0 || b < 0 || a == b)
      continue;
    if (std::find(succ[size_t(a)].begin(), succ[size_t(a)].end(), b) == succ[size_t(a)].end()) {
      succ[size_t(a)].push_back(b);
      pred[size_t(b)].push_back(a);
      undirected[size_t(a)].push_back(b);
      undirected[size_t(b)].push_back(a);
    }
  }
  std::vector<int> comp(count, -1);
  int comps = 0;
  for (int s = 0; s < n; ++s) {
    if (comp[size_t(s)] >= 0)
      continue;
    std::vector<int> st {s};
    comp[size_t(s)] = comps;
    while (!st.empty()) {
      const int v = st.back();
      st.pop_back();
      for (const int w: undirected[size_t(v)])
        if (comp[size_t(w)] < 0) {
          comp[size_t(w)] = comps;
          st.push_back(w);
        }
    }
    ++comps;
  }
  // Cycle breaking: DFS back edges are ignored for layering.
  std::vector<int> layer(count, 0), state(count, 0);
  std::vector<std::vector<int>> dag(count);
  std::function<void(int)> dfs = [&](int v) {
    state[size_t(v)] = 1;
    for (const int w: succ[size_t(v)]) {
      if (state[size_t(w)] == 1)
        continue; // back edge
      dag[size_t(v)].push_back(w);
      if (state[size_t(w)] == 0)
        dfs(w);
    }
    state[size_t(v)] = 2;
  };
  // Start from sources so chains read left to right.
  for (int v = 0; v < n; ++v)
    if (pred[size_t(v)].empty() && state[size_t(v)] == 0)
      dfs(v);
  for (int v = 0; v < n; ++v)
    if (state[size_t(v)] == 0)
      dfs(v);
  // Longest path: relax in topological order (Kahn over the DAG).
  std::vector<int> indeg(count, 0);
  for (int v = 0; v < n; ++v)
    for (const int w: dag[size_t(v)])
      ++indeg[size_t(w)];
  std::vector<int> queue;
  for (int v = 0; v < n; ++v)
    if (!indeg[size_t(v)])
      queue.push_back(v);
  for (size_t q = 0; q < queue.size(); ++q) {
    const int v = queue[q];
    for (const int w: dag[size_t(v)]) {
      layer[size_t(w)] = std::max(layer[size_t(w)], layer[size_t(v)] + 1);
      if (--indeg[size_t(w)] == 0)
        queue.push_back(w);
    }
  }

  qreal y0 = 0.0;
  std::vector<int> singles;
  for (int c = 0; c < comps; ++c) {
    std::vector<int> members;
    for (int v = 0; v < n; ++v)
      if (comp[size_t(v)] == c)
        members.push_back(v);
    if (members.size() == 1 && succ[size_t(members[0])].empty() && pred[size_t(members[0])].empty()) {
      singles.push_back(members[0]);
      continue;
    }
    std::map<int, std::vector<int>> layers;
    for (const int v: members)
      layers[layer[size_t(v)]].push_back(v);
    std::vector<double> order(count, 0.0);
    for (auto &[l, vs]: layers) {
      std::sort(vs.begin(), vs.end(), [&](int a, int b) { return nodes[size_t(a)]->title < nodes[size_t(b)]->title; });
      for (size_t i = 0; i < vs.size(); ++i)
        order[size_t(vs[i])] = double(i);
    }
    for (int sweep = 0; sweep < 4; ++sweep) {
      const bool down = sweep % 2 == 0;
      auto apply = [&](std::vector<int> &vs) {
        for (const int v: vs) {
          const auto &nb = down ? pred[size_t(v)] : succ[size_t(v)];
          if (nb.empty())
            continue;
          double sum = 0;
          for (const int w: nb)
            sum += order[size_t(w)];
          order[size_t(v)] = sum / double(nb.size()) + double(v) * 1e-6;
        }
        std::sort(vs.begin(), vs.end(), [&](int a, int b) { return order[size_t(a)] < order[size_t(b)]; });
        for (size_t i = 0; i < vs.size(); ++i)
          order[size_t(vs[i])] = double(i);
      };
      if (down)
        for (auto &[l, vs]: layers)
          apply(vs);
      else
        for (auto it = layers.rbegin(); it != layers.rend(); ++it)
          apply(it->second);
    }
    qreal compH = 0.0;
    for (auto &[l, vs]: layers) {
      qreal y = y0;
      for (const int v: vs) {
        out[size_t(v)] = QPointF(l * (kNodeW + kGapX), y);
        y += nodes[size_t(v)]->height() + kGapY;
      }
      compH = std::max(compH, y - y0);
    }
    y0 += compH + 50.0;
  }
  const int cols = std::max(1, int(std::ceil(std::sqrt(double(singles.size())))));
  for (size_t i = 0; i < singles.size(); ++i)
    out[size_t(singles[i])] = QPointF(int(i % size_t(cols)) * (kNodeW + 40.0), y0 + int(i / size_t(cols)) * 110.0);
  return out;
}

void IOGraphView::placeNewNodes(const std::vector<IONodeItem *> &unplaced) {
  // New nodes go next to a placed neighbour, else into a flow layout below the rest.
  QSet<IONodeItem *> todo(unplaced.begin(), unplaced.end());
  QRectF placed;
  for (IONodeItem *n: std::as_const(m_nodes))
    if (!todo.contains(n))
      placed = placed.united(n->sceneBoundingRect());
  for (int pass = 0; pass < 3 && !todo.isEmpty(); ++pass)
    for (IOEdgeItem *ed: m_edges) {
      IONodeItem *anchor = nullptr, *mover = nullptr;
      bool right = true;
      if (todo.contains(ed->to) && !todo.contains(ed->from)) {
        anchor = ed->from;
        mover = ed->to;
      } else if (todo.contains(ed->from) && !todo.contains(ed->to)) {
        anchor = ed->to;
        mover = ed->from;
        right = false;
      }
      if (!anchor)
        continue;
      QPointF p = anchor->pos() + QPointF(right ? kNodeW + kGapX : -(kNodeW + kGapX), 0);
      for (int guard = 0; guard < 50; ++guard) {
        bool hit = false;
        for (IONodeItem *o: std::as_const(m_nodes))
          if (o != mover && !todo.contains(o) && QRectF(p, QSizeF(kNodeW, mover->height())).intersects(o->sceneBoundingRect()))
            hit = true;
        if (!hit)
          break;
        p.ry() += 60;
      }
      mover->setPos(p);
      todo.remove(mover);
      if (!mover->pseudo() && m_engine)
        m_engine->setIOGraphPos(mover->entity, glm::vec2(float(p.x()), float(p.y())));
    }
  if (todo.isEmpty())
    return;
  std::vector<IONodeItem *> rest(todo.begin(), todo.end());
  std::sort(rest.begin(), rest.end(), [](IONodeItem *a, IONodeItem *b) { return a->key < b->key; });
  const auto pos = layeredPositions(rest);
  const QPointF off = placed.isNull() ? QPointF(0, 0) : QPointF(placed.left(), placed.bottom() + 80);
  for (size_t i = 0; i < rest.size(); ++i) {
    rest[i]->setPos(pos[i] + off);
    if (!rest[i]->pseudo() && m_engine)
      m_engine->setIOGraphPos(rest[i]->entity, glm::vec2(float(rest[i]->x()), float(rest[i]->y())));
  }
}

void IOGraphView::autoLayout(const bool mapLayout) {
  std::vector<IONodeItem *> all;
  for (IONodeItem *n: std::as_const(m_nodes))
    all.push_back(n);
  std::sort(all.begin(), all.end(), [](IONodeItem *a, IONodeItem *b) { return a->key < b->key; });
  if (all.empty())
    return;
  if (!mapLayout) {
    const auto pos = layeredPositions(all);
    for (size_t i = 0; i < all.size(); ++i)
      all[i]->setPos(pos[i]);
  } else {
    // Top view (X right, Z down), scaled, then pushed apart. Pseudo nodes sit right of their first source.
    std::vector<IONodeItem *> ents;
    glm::vec2 mn(1e30f), mx(-1e30f);
    for (IONodeItem *n: all)
      if (const eng::Entity *e = n->pseudo() ? nullptr : m_engine->entityById(n->entity)) {
        ents.push_back(n);
        mn = glm::min(mn, glm::vec2(e->origin.x, e->origin.z));
        mx = glm::max(mx, glm::vec2(e->origin.x, e->origin.z));
      }
    const glm::vec2 ext = glm::max(mx - mn, glm::vec2(1.0f));
    const qreal span = std::max(900.0, std::sqrt(double(ents.size())) * (kNodeW + 80.0));
    const qreal k = span / std::max(ext.x, ext.y);
    for (IONodeItem *n: ents) {
      const eng::Entity *e = m_engine->entityById(n->entity);
      n->setPos((e->origin.x - mn.x) * k, (e->origin.z - mn.y) * k);
    }
    for (int iter = 0; iter < 60; ++iter) {
      bool moved = false;
      for (size_t i = 0; i < ents.size(); ++i)
        for (size_t j = i + 1; j < ents.size(); ++j) {
          QRectF a = ents[i]->sceneBoundingRect().adjusted(-12, -12, 12, 12);
          QRectF b = ents[j]->sceneBoundingRect();
          if (!a.intersects(b))
            continue;
          QPointF d = b.center() - a.center();
          if (std::hypot(d.x(), d.y()) < 1e-3)
            d = QPointF(1, 0.5);
          d /= std::hypot(d.x(), d.y());
          ents[i]->moveBy(-d.x() * 14, -d.y() * 14);
          ents[j]->moveBy(d.x() * 14, d.y() * 14);
          moved = true;
        }
      if (!moved)
        break;
    }
    for (IONodeItem *n: all)
      if (n->pseudo())
        for (IOEdgeItem *ed: n->edges)
          if (ed->to == n) {
            n->setPos(ed->from->pos() + QPointF(kNodeW + kGapX, 30));
            break;
          }
  }
  for (IONodeItem *n: all)
    if (!n->pseudo())
      m_engine->setIOGraphPos(n->entity, glm::vec2(float(n->x()), float(n->y())));
  for (IOEdgeItem *ed: m_edges)
    ed->updatePath();
  updateSceneRect();
  fitAll();
}

void IOGraphView::fitAll() {
  QRectF r;
  for (IONodeItem *n: std::as_const(m_nodes))
    r = r.united(n->sceneBoundingRect());
  if (r.isNull())
    return;
  fitInView(r.adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
  if (transform().m11() > 1.4 * m_uiScale)
    setTransform(QTransform::fromScale(1.4 * m_uiScale, 1.4 * m_uiScale));
}

void IOGraphView::centerOnEntity(const quint32 id, const bool onlyIfHidden) {
  IONodeItem *n = m_nodes.value(keyFor(id));
  if (!n)
    return;
  const QRectF vis = mapToScene(viewport()->rect()).boundingRect();
  if (onlyIfHidden && vis.contains(n->sceneBoundingRect()))
    return;
  centerOn(n->sceneBoundingRect().center());
}

void IOGraphView::updateSimulation() {
  if (!m_engine)
    return;
  const bool running = m_engine->ioSimRunning();
  for (IONodeItem *n: std::as_const(m_nodes)) {
    if (n->pseudo())
      continue;
    const bool flash = running && m_engine->ioSimFlashing(n->entity);
    QString st = running ? m_engine->ioSimState(n->entity) : QString();
    if (running && m_engine->ioSimKilled(n->entity))
      st = QStringLiteral("killed");
    n->setSimState(flash, st);
  }
  const auto pulses = running ? m_engine->ioPulseLinks() : std::vector<vk::Engine3D::IOPulseLink>();
  size_t used = 0;
  for (const auto &pl: pulses)
    for (IOEdgeItem *ed: m_edges) {
      if (ed->from->entity != pl.from || ed->to->pseudo() || ed->to->entity != pl.to)
        continue;
      if (used >= m_pulses.size()) {
        auto *dot = m_scene->addEllipse(-5, -5, 10, 10, QPen(QColor(255, 255, 255), 1.5), QBrush(QColor(255, 225, 90)));
        dot->setZValue(5);
        m_pulses.push_back(dot);
      }
      m_pulses[used]->setPos(ed->path.pointAtPercent(std::clamp<qreal>(pl.t, 0.0, 1.0)));
      m_pulses[used]->setVisible(true);
      ++used;
      break;
    }
  for (size_t i = used; i < m_pulses.size(); ++i)
    m_pulses[i]->setVisible(false);
}

void IOGraphView::clearSimulation() {
  for (auto *d: m_pulses)
    d->setVisible(false);
  updateSimulation();
}

IONodeItem *IOGraphView::nodeAt(const QPoint &viewPos) const {
  for (QGraphicsItem *it: items(viewPos))
    if (auto *n = dynamic_cast<IONodeItem *>(it))
      return n;
  return nullptr;
}

IOEdgeItem *IOGraphView::edgeAt(const QPoint &viewPos) const {
  for (QGraphicsItem *it: items(viewPos))
    if (auto *ed = dynamic_cast<IOEdgeItem *>(it))
      return ed;
  return nullptr;
}

void IOGraphView::mousePressEvent(QMouseEvent *e) {
  setFocus();
  m_lastPos = e->pos();
  m_moved = false;
  const bool additive = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
  if (e->button() == Qt::MiddleButton) {
    m_drag = Drag::Pan;
    viewport()->setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (e->button() != Qt::LeftButton) {
    QGraphicsView::mousePressEvent(e);
    return;
  }
  if (IONodeItem *n = nodeAt(e->pos())) {
    QString out;
    if (n->outputAt(n->mapFromScene(mapToScene(e->pos())), &out)) {
      m_drag = Drag::Wire;
      m_dragNode = n;
      m_wireOutput = out;
      m_wire = m_scene->addPath(QPainterPath(), QPen(kAccent, 2.0, Qt::DashLine));
      m_wire->setZValue(4);
      viewport()->setCursor(Qt::CrossCursor);
      return;
    }
    // The click can rebuild the scene and delete `n`: look it up again by key.
    const QString key = n->key;
    const QPointF grab = mapToScene(e->pos()) - n->pos();
    if (!n->pseudo())
      emit entityClicked(n->entity, additive);
    if (IONodeItem *alive = m_nodes.value(key)) {
      m_drag = Drag::Move;
      m_dragNode = alive;
      m_grabOffset = grab;
    }
    return;
  }
  if (IOEdgeItem *ed = edgeAt(e->pos())) {
    emit connectionsClicked({ed->ref}, additive);
    return;
  }
  emit backgroundClicked();
  m_drag = Drag::Pan;
  viewport()->setCursor(Qt::ClosedHandCursor);
}

void IOGraphView::mouseMoveEvent(QMouseEvent *e) {
  const QPoint delta = e->pos() - m_lastPos;
  if ((e->pos() - m_lastPos).manhattanLength() > 2)
    m_moved = true;
  switch (m_drag) {
    case Drag::Pan:
      horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
      verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
      m_lastPos = e->pos();
      return;
    case Drag::Move: {
      if (!m_moved)
        return;
      QPointF p = mapToScene(e->pos()) - m_grabOffset;
      p = QPointF(std::round(p.x() / 10.0) * 10.0, std::round(p.y() / 10.0) * 10.0);
      m_dragNode->setPos(p);
      for (IOEdgeItem *ed: m_dragNode->edges)
        ed->updatePath();
      return;
    }
    case Drag::Wire: {
      const QPointF a = m_dragNode->outputPort(m_wireOutput);
      QPointF b = mapToScene(e->pos());
      IONodeItem *hover = nodeAt(e->pos());
      if (hover && hover->pseudo())
        hover = nullptr;
      if (hover != m_wireHover) {
        if (m_wireHover) {
          m_wireHover->flashing = false;
          m_wireHover->update();
        }
        m_wireHover = hover;
        if (hover) {
          hover->flashing = true;
          hover->update();
        }
      }
      if (hover)
        b = hover->mapToScene(QPointF(0, kHeaderH * 0.5));
      const qreal dx = std::max(50.0, std::abs(b.x() - a.x()) * 0.5);
      QPainterPath path(a);
      path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
      m_wire->setPath(path);
      return;
    }
    case Drag::None: {
      // Crosshair over output ports.
      IONodeItem *n = nodeAt(e->pos());
      QString out;
      viewport()->setCursor(n && n->outputAt(n->mapFromScene(mapToScene(e->pos())), &out) ? Qt::CrossCursor
                            : n                                                         ? Qt::OpenHandCursor
                                                                                        : Qt::ArrowCursor);
      QGraphicsView::mouseMoveEvent(e);
      return;
    }
  }
}

void IOGraphView::mouseReleaseEvent(QMouseEvent *e) {
  const Drag drag = m_drag;
  m_drag = Drag::None;
  viewport()->setCursor(Qt::ArrowCursor);
  if (drag == Drag::Move && m_dragNode && m_moved) {
    if (!m_dragNode->pseudo() && m_engine)
      m_engine->setIOGraphPos(m_dragNode->entity, glm::vec2(float(m_dragNode->x()), float(m_dragNode->y())));
    updateSceneRect();
  }
  if (drag == Drag::Wire) {
    if (m_wireHover) {
      m_wireHover->flashing = false;
      m_wireHover->update();
    }
    IONodeItem *target = m_wireHover;
    m_wireHover = nullptr;
    delete m_wire;
    m_wire = nullptr;
    const quint32 from = m_dragNode ? m_dragNode->entity : 0;
    m_dragNode = nullptr;
    if (from && m_moved)
      emit connectRequested(from, m_wireOutput, target ? target->entity : 0, mapToScene(e->pos()),
                            e->globalPosition().toPoint());
    else if (from && !m_moved)
      emit entityClicked(from, false);
    return;
  }
  m_dragNode = nullptr;
  QGraphicsView::mouseReleaseEvent(e);
}

void IOGraphView::mouseDoubleClickEvent(QMouseEvent *e) {
  if (IONodeItem *n = nodeAt(e->pos()); n && !n->pseudo())
    emit entityActivated(n->entity);
}

void IOGraphView::wheelEvent(QWheelEvent *e) {
  const qreal steps = e->angleDelta().y() / 120.0;
  const qreal factor = std::pow(1.15, steps);
  const qreal next = transform().m11() * factor / m_uiScale;
  if (next < 0.15 || next > 3.0)
    return;
  scale(factor, factor);
}

void IOGraphView::contextMenuEvent(QContextMenuEvent *e) {
  if (IONodeItem *n = nodeAt(e->pos())) {
    if (!n->pseudo())
      emit entityMenuRequested(n->entity, e->globalPos());
    return;
  }
  if (IOEdgeItem *ed = edgeAt(e->pos())) {
    emit connectionMenuRequested(ed->ref, e->globalPos());
    return;
  }
  emit backgroundMenuRequested(mapToScene(e->pos()), e->globalPos());
}

void IOGraphView::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
    emit deleteRequested();
    return;
  }
  if (e->key() == Qt::Key_F && e->modifiers() == Qt::NoModifier) {
    fitAll();
    return;
  }
  QGraphicsView::keyPressEvent(e);
}

} // namespace ui
