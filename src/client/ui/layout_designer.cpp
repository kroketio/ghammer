#include "layout_designer.h"
#include "client/ui/scale.h"

#include <climits>
#include <cmath>
#include <map>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#include "client/ctx.h"
#include "client/engine/camera.h"
#include "client/engine/scene.h"
#include "client/lib/globals.h"
#include "client/ui/action_manager.h"
#include "client/ui/layout_panel.h"
#include "client/vulkan/vk_engine.h"
#include "shared/lib/config.h"

namespace ui {

using namespace eng::layout;

namespace {
double segDistance(const glm::dvec2 &p, const glm::dvec2 &a, const glm::dvec2 &b) {
  const glm::dvec2 d = b - a;
  const double len2 = glm::dot(d, d);
  const double t = len2 > 1e-12 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
  return glm::length(p - (a + d * t));
}
glm::dvec2 su2(const QPoint &n) { return {double(n.x()), double(n.y())}; }
bool rectHas(const QRect &r, const glm::dvec2 &su) {
  return su.x >= r.x() && su.x < r.x() + r.width() && su.y >= r.y() && su.y < r.y() + r.height();
}
bool horizontal(const QPoint &a, const QPoint &b) { return a.y() == b.y() && a.x() != b.x(); }
bool vertical(const QPoint &a, const QPoint &b) { return a.x() == b.x() && a.y() != b.y(); }

template <typename T>
int common(const std::vector<T> &vals) {
  if (vals.empty())
    return -1;
  for (const T v: vals)
    if (v != vals.front())
      return INT_MIN;
  return vals.front();
}
} // namespace

const std::vector<int> &LayoutModel::corridorWidths() {
  static const std::vector<int> v {80, 96, 128, 144, 160, 176, 192, 224, 256};
  return v;
}
const std::vector<int> &LayoutModel::ventWidths() {
  static const std::vector<int> v {32, 48, 64, 96};
  return v;
}
const std::vector<int> &LayoutModel::gridSizes() {
  static const std::vector<int> v {8, 16, 32, 64, 128, 256};
  return v;
}

LayoutModel::LayoutModel(vk::Engine3D *engine, QObject *parent) : QObject(parent), m_engine(engine) {
  const int g = config()->getRaw(QStringLiteral("LayoutDesigner/Grid")).toInt();
  if (std::find(gridSizes().begin(), gridSizes().end(), g) != gridSizes().end())
    m_grid = g;
  refreshLayouts();
  newLayout();
  if (m_engine) {
    connect(m_engine, &vk::Engine3D::layoutsChanged, this, &LayoutModel::refreshLayouts);
    connect(m_engine, &vk::Engine3D::meshGroupsChanged, this, &LayoutModel::layoutsChanged);
    connect(m_engine, &vk::Engine3D::documentChanged, this, &LayoutModel::levelChanged);
  }
}

void LayoutModel::levelChanged() {
  refreshLayouts();
  m_previewing = false;
  m_doc.id = 0; // ids are minted per level
  // Live mode stays on, but the new layout generates nothing yet.
  const bool live = m_live;
  m_live = false;
  newLayout();
  m_live = live;
}

const Raster &LayoutModel::raster() const {
  if (m_rasterRev != m_revision) {
    m_raster = rasterize(m_doc);
    m_rasterRev = m_revision;
  }
  return m_raster;
}

void LayoutModel::setTool(int t) {
  t = std::clamp(t, 0, int(ToolErase));
  if (t == m_tool)
    return;
  m_tool = t;
  if (t != ToolSelect)
    clearSelection();
  emit toolChanged();
}

void LayoutModel::setGridSize(const int g) {
  int best = gridSizes().front();
  for (const int s: gridSizes())
    if (std::abs(s - g) < std::abs(best - g))
      best = s;
  if (best == m_grid)
    return;
  m_grid = best;
  config()->setRaw(QStringLiteral("LayoutDesigner/Grid"), m_grid);
  emit gridChanged();
}

void LayoutModel::stepGrid(const int dir) {
  const auto &s = gridSizes();
  int i = int(std::find(s.begin(), s.end(), m_grid) - s.begin());
  i = std::clamp(i + (dir > 0 ? 1 : -1), 0, int(s.size()) - 1);
  setGridSize(s[size_t(i)]);
}

void LayoutModel::setShowDimensions(const bool on) {
  if (on == m_showDims)
    return;
  m_showDims = on;
  emit toolChanged();
  emit changed();
}

void LayoutModel::setLive(const bool on) {
  if (on == m_live)
    return;
  m_live = on;
  emit toolChanged();
  if (on)
    generate();
}

void LayoutModel::setStatus(const QString &s) {
  if (s == m_status)
    return;
  m_status = s;
  emit statusChanged();
}

void LayoutModel::pushHistory() {
  if (m_histPos + 1 < int(m_history.size()))
    m_history.resize(size_t(m_histPos + 1));
  m_history.push_back(toJson(m_doc));
  if (m_history.size() > 200)
    m_history.erase(m_history.begin());
  m_histPos = int(m_history.size()) - 1;
}

void LayoutModel::restore(const int pos) {
  if (pos < 0 || pos >= int(m_history.size()))
    return;
  m_histPos = pos;
  // The placement is not an edit: undo keeps it.
  const glm::vec3 origin = m_doc.origin;
  m_doc = fromJson(m_history[size_t(pos)]);
  m_doc.origin = origin;
  m_selSegs.clear();
  m_selRooms.clear();
  touched(false);
}

void LayoutModel::touched(const bool history) {
  if (history)
    pushHistory();
  ++m_revision;
  emit changed();
  if (m_live && history)
    QTimer::singleShot(0, this, [this] { generate(); });
}

void LayoutModel::undo() {
  if (canUndo())
    restore(m_histPos - 1);
  if (m_live)
    generate();
}

void LayoutModel::redo() {
  if (canRedo())
    restore(m_histPos + 1);
  if (m_live)
    generate();
}

QVariant LayoutModel::cfg(const QString &key) const {
  if (key == QLatin1String("name"))
    return m_doc.name;
  if (key == QLatin1String("originX"))
    return int(std::lround(m_doc.origin.x * 10.0f));
  if (key == QLatin1String("originY"))
    return int(std::lround(m_doc.origin.y * 10.0f));
  if (key == QLatin1String("originZ"))
    return int(std::lround(m_doc.origin.z * 10.0f));
  return toJson(m_doc).value(QStringLiteral("config")).toObject().value(key).toVariant();
}

void LayoutModel::setCfg(const QString &key, const QVariant &value) {
  if (cfg(key) == value)
    return;
  if (key == QLatin1String("name")) {
    m_doc.name = value.toString();
  } else if (key == QLatin1String("originX")) {
    m_doc.origin.x = float(value.toInt()) / 10.0f;
  } else if (key == QLatin1String("originY")) {
    m_doc.origin.y = float(value.toInt()) / 10.0f;
  } else if (key == QLatin1String("originZ")) {
    m_doc.origin.z = float(value.toInt()) / 10.0f;
  } else {
    QJsonObject o = toJson(m_doc);
    QJsonObject c = o.value(QStringLiteral("config")).toObject();
    if (value.typeId() == QMetaType::QString)
      c[key] = value.toString();
    else
      c[key] = value.toInt();
    o[QStringLiteral("config")] = c;
    m_doc.config = fromJson(o).config;
  }
  touched();
}

int LayoutModel::drawWidth() const {
  return m_tool == ToolVent ? m_doc.config.ventWidth : m_doc.config.corridorWidth;
}

void LayoutModel::addPath(const Kind kind, const std::vector<QPoint> &nodes) {
  if (nodes.size() < 2)
    return;
  Path p;
  p.id = m_doc.nextShapeId++;
  p.kind = kind;
  p.nodes = nodes;
  const eng::layout::Config &c = m_doc.config;
  const Segment seg = kind == Vent ? Segment {c.ventWidth, c.ventHeight, c.ventElevation}
                                   : Segment {c.corridorWidth, c.corridorHeight, 0};
  p.segs.assign(nodes.size() - 1, seg);
  m_doc.paths.push_back(std::move(p));
  touched();
}

void LayoutModel::addRoomRect(const QRect &rect, const bool toSelected, const int contact) {
  const QRect r = rect.normalized();
  if (r.width() < 1 || r.height() < 1)
    return;
  if (toSelected && m_selRooms.size() == 1) {
    m_doc.rooms[size_t(m_selRooms.front())].rects.push_back(r);
  } else {
    RoomShape room;
    room.id = m_doc.nextShapeId++;
    room.height = m_doc.config.roomHeight;
    room.contact = std::clamp(contact, 0, 2);
    room.rects = {r};
    m_doc.rooms.push_back(room);
    if (m_tool == ToolSelect)
      m_selRooms = {int(m_doc.rooms.size()) - 1};
  }
  touched();
}

void LayoutModel::toggleSeal(const QRect &square) {
  const glm::dvec2 c(square.x() + square.width() * 0.5, square.y() + square.height() * 0.5);
  const auto before = m_doc.sealed.size();
  m_doc.sealed.erase(std::remove_if(m_doc.sealed.begin(), m_doc.sealed.end(),
                                    [&c](const QRect &s) { return rectHas(s, c); }),
                     m_doc.sealed.end());
  if (m_doc.sealed.size() == before)
    m_doc.sealed.push_back(square);
  touched();
}

LayoutModel::SegRef LayoutModel::segmentAt(const glm::dvec2 &su) const {
  SegRef best;
  double bestD = 1e18;
  for (size_t i = 0; i < m_doc.paths.size(); ++i) {
    const Path &p = m_doc.paths[i];
    for (size_t s = 0; s + 1 < p.nodes.size() && s < p.segs.size(); ++s) {
      const double d = segDistance(su, su2(p.nodes[s]), su2(p.nodes[s + 1]));
      // Vents are drawn on top, so they are picked first when they overlap.
      const double score = d - (p.kind == Vent ? 0.5 : 0.0);
      if (d <= p.segs[s].width * 0.5 + 1e-6 && score < bestD) {
        bestD = score;
        best = {int(i), int(s)};
      }
    }
  }
  return best;
}

int LayoutModel::roomAt(const glm::dvec2 &su) const {
  for (int i = int(m_doc.rooms.size()) - 1; i >= 0; --i)
    for (const QRect &r: m_doc.rooms[size_t(i)].rects)
      if (rectHas(r, su))
        return i;
  return -1;
}

bool LayoutModel::eraseAt(const glm::dvec2 &su) {
  const auto sealedBefore = m_doc.sealed.size();
  m_doc.sealed.erase(std::remove_if(m_doc.sealed.begin(), m_doc.sealed.end(),
                                    [&su](const QRect &s) { return rectHas(s, su); }),
                     m_doc.sealed.end());
  if (m_doc.sealed.size() != sealedBefore) {
    touched();
    return true;
  }
  const SegRef s = segmentAt(su);
  if (s.path >= 0) {
    Path &p = m_doc.paths[size_t(s.path)];
    // Removing a middle segment splits the path in two.
    Path tail;
    tail.id = m_doc.nextShapeId++;
    tail.kind = p.kind;
    tail.nodes.assign(p.nodes.begin() + s.seg + 1, p.nodes.end());
    tail.segs.assign(p.segs.begin() + s.seg + 1, p.segs.end());
    p.nodes.resize(size_t(s.seg + 1));
    p.segs.resize(size_t(s.seg));
    if (tail.nodes.size() >= 2)
      m_doc.paths.push_back(std::move(tail));
    m_doc.paths.erase(std::remove_if(m_doc.paths.begin(), m_doc.paths.end(),
                                     [](const Path &x) { return x.nodes.size() < 2; }),
                      m_doc.paths.end());
    m_selSegs.clear();
    touched();
    return true;
  }
  const int r = roomAt(su);
  if (r >= 0) {
    auto &rects = m_doc.rooms[size_t(r)].rects;
    for (size_t i = 0; i < rects.size(); ++i)
      if (rectHas(rects[i], su)) {
        rects.erase(rects.begin() + long(i));
        break;
      }
    if (rects.empty())
      m_doc.rooms.erase(m_doc.rooms.begin() + r);
    m_selRooms.clear();
    touched();
    return true;
  }
  return false;
}

void LayoutModel::clearSelection() {
  if (m_selSegs.empty() && m_selRooms.empty())
    return;
  m_selSegs.clear();
  m_selRooms.clear();
  ++m_revision;
  emit changed();
}

void LayoutModel::selectAt(const glm::dvec2 &su, const bool additive, const bool wholePath) {
  if (!additive) {
    m_selSegs.clear();
    m_selRooms.clear();
  }
  const SegRef s = segmentAt(su);
  if (s.path >= 0) {
    std::vector<SegRef> refs;
    if (wholePath)
      for (int i = 0; i < int(m_doc.paths[size_t(s.path)].segs.size()); ++i)
        refs.push_back({s.path, i});
    else
      refs.push_back(s);
    for (const SegRef &r: refs) {
      auto it = std::find(m_selSegs.begin(), m_selSegs.end(), r);
      if (it != m_selSegs.end() && additive && !wholePath)
        m_selSegs.erase(it);
      else if (it == m_selSegs.end())
        m_selSegs.push_back(r);
    }
  } else if (const int r = roomAt(su); r >= 0) {
    auto it = std::find(m_selRooms.begin(), m_selRooms.end(), r);
    if (it != m_selRooms.end() && additive)
      m_selRooms.erase(it);
    else if (it == m_selRooms.end())
      m_selRooms.push_back(r);
  }
  ++m_revision;
  emit changed();
}

bool LayoutModel::isSegSelected(const int path, const int seg) const {
  return std::find(m_selSegs.begin(), m_selSegs.end(), SegRef {path, seg}) != m_selSegs.end();
}

bool LayoutModel::isPathSelected(const int path) const {
  if (path < 0 || path >= int(m_doc.paths.size()))
    return false;
  for (int s = 0; s < int(m_doc.paths[size_t(path)].segs.size()); ++s)
    if (!isSegSelected(path, s))
      return false;
  return true;
}

int LayoutModel::selKind() const {
  if (!m_selSegs.empty())
    return m_doc.paths[size_t(m_selSegs.front().path)].kind == Vent ? 2 : 0;
  if (!m_selRooms.empty())
    return 1;
  return -1;
}

int LayoutModel::selWidth() const {
  std::vector<int> v;
  for (const SegRef &s: m_selSegs)
    v.push_back(m_doc.paths[size_t(s.path)].segs[size_t(s.seg)].width);
  return common(v);
}

int LayoutModel::selHeight() const {
  std::vector<int> v;
  for (const SegRef &s: m_selSegs)
    v.push_back(m_doc.paths[size_t(s.path)].segs[size_t(s.seg)].height);
  for (const int r: m_selRooms)
    v.push_back(m_doc.rooms[size_t(r)].height);
  return common(v);
}

int LayoutModel::selElevation() const {
  std::vector<int> v;
  for (const SegRef &s: m_selSegs)
    v.push_back(m_doc.paths[size_t(s.path)].segs[size_t(s.seg)].elevation);
  for (const int r: m_selRooms)
    v.push_back(m_doc.rooms[size_t(r)].elevation);
  return common(v);
}

int LayoutModel::selLength() const {
  std::vector<int> v;
  for (const SegRef &s: m_selSegs)
    v.push_back(segmentLength(m_doc.paths[size_t(s.path)], s.seg));
  return common(v);
}

QSize LayoutModel::selRoomSize() const {
  if (m_selRooms.size() != 1 || !m_selSegs.empty())
    return {-1, -1};
  const auto &rects = m_doc.rooms[size_t(m_selRooms.front())].rects;
  if (rects.size() != 1)
    return {-1, -1};
  return rects.front().size();
}

int LayoutModel::selContact() const {
  std::vector<int> v;
  for (const int r: m_selRooms)
    v.push_back(m_doc.rooms[size_t(r)].contact);
  return common(v);
}

void LayoutModel::setSelWidth(const int w) {
  if (m_selSegs.empty() || w < 8)
    return;
  for (const SegRef &s: m_selSegs)
    m_doc.paths[size_t(s.path)].segs[size_t(s.seg)].width = w;
  touched();
}

void LayoutModel::setSelHeight(const int h) {
  if (h < 8 || (m_selSegs.empty() && m_selRooms.empty()))
    return;
  for (const SegRef &s: m_selSegs)
    m_doc.paths[size_t(s.path)].segs[size_t(s.seg)].height = h;
  for (const int r: m_selRooms)
    m_doc.rooms[size_t(r)].height = h;
  touched();
}

void LayoutModel::setSelElevation(const int e) {
  if (m_selSegs.empty() && m_selRooms.empty())
    return;
  for (const SegRef &s: m_selSegs)
    m_doc.paths[size_t(s.path)].segs[size_t(s.seg)].elevation = e;
  for (const int r: m_selRooms)
    m_doc.rooms[size_t(r)].elevation = e;
  touched();
}

void LayoutModel::setSelLength(const int len) {
  if (m_selSegs.empty())
    return;
  // Each segment's far node moves along it and the rest follows; back to front so earlier shifts carry later ones.
  std::vector<SegRef> refs = m_selSegs;
  std::sort(refs.begin(), refs.end(), [](const SegRef &a, const SegRef &b) {
    return a.path != b.path ? a.path < b.path : a.seg > b.seg;
  });
  bool any = false;
  for (const SegRef &s: refs) {
    Path &p = m_doc.paths[size_t(s.path)];
    const QPoint a = p.nodes[size_t(s.seg)], b = p.nodes[size_t(s.seg) + 1];
    const glm::dvec2 d = su2(b) - su2(a);
    const double cur = glm::length(d);
    if (cur < 1e-9)
      continue;
    const double want = std::max(0, len - p.segs[size_t(s.seg)].width);
    const glm::dvec2 shift = d / cur * (want - cur);
    const QPoint off(int(std::lround(shift.x)), int(std::lround(shift.y)));
    if (off.isNull())
      continue;
    for (size_t k = size_t(s.seg) + 1; k < p.nodes.size(); ++k)
      p.nodes[k] += off;
    any = true;
  }
  if (!any)
    return;
  cleanPaths();
  touched();
}

void LayoutModel::setSelRoomSize(const int w, const int d) {
  if (selRoomSize().width() < 0 || w < 8 || d < 8)
    return;
  QRect &r = m_doc.rooms[size_t(m_selRooms.front())].rects.front();
  if (r.width() == w && r.height() == d)
    return;
  r.setWidth(w);
  r.setHeight(d);
  touched();
}

void LayoutModel::setSelContact(const int contact) {
  if (m_selRooms.empty())
    return;
  for (const int r: m_selRooms)
    m_doc.rooms[size_t(r)].contact = std::clamp(contact, 0, 2);
  touched();
}

void LayoutModel::deleteSelection() {
  if (m_selSegs.empty() && m_selRooms.empty())
    return;
  std::vector<int> rooms = m_selRooms;
  std::sort(rooms.rbegin(), rooms.rend());
  for (const int r: rooms)
    m_doc.rooms.erase(m_doc.rooms.begin() + r);
  std::vector<Path> out;
  for (size_t pi = 0; pi < m_doc.paths.size(); ++pi) {
    const Path &p = m_doc.paths[pi];
    Path cur;
    cur.kind = p.kind;
    for (size_t s = 0; s < p.segs.size(); ++s) {
      if (isSegSelected(int(pi), int(s))) {
        if (cur.nodes.size() >= 2) {
          cur.id = m_doc.nextShapeId++;
          out.push_back(cur);
        }
        cur = Path {};
        cur.kind = p.kind;
        continue;
      }
      if (cur.nodes.empty())
        cur.nodes.push_back(p.nodes[s]);
      cur.nodes.push_back(p.nodes[s + 1]);
      cur.segs.push_back(p.segs[s]);
    }
    if (cur.nodes.size() >= 2) {
      cur.id = cur.segs.size() == p.segs.size() ? p.id : m_doc.nextShapeId++;
      out.push_back(cur);
    }
  }
  m_doc.paths = std::move(out);
  m_selSegs.clear();
  m_selRooms.clear();
  touched();
}

void LayoutModel::cleanPaths() {
  bool removed = false;
  for (Path &p: m_doc.paths) {
    for (size_t k = 0; k + 1 < p.nodes.size();) {
      if (p.nodes[k] == p.nodes[k + 1]) {
        p.nodes.erase(p.nodes.begin() + long(k) + 1);
        if (k < p.segs.size())
          p.segs.erase(p.segs.begin() + long(std::min(k, p.segs.size() - 1)));
        removed = true;
      } else {
        ++k;
      }
    }
    p.segs.resize(p.nodes.empty() ? 0 : p.nodes.size() - 1);
  }
  const auto before = m_doc.paths.size();
  m_doc.paths.erase(std::remove_if(m_doc.paths.begin(), m_doc.paths.end(),
                                   [](const Path &x) { return x.nodes.size() < 2; }),
                    m_doc.paths.end());
  if (removed || m_doc.paths.size() != before)
    m_selSegs.clear();
}

void LayoutModel::beginPreview() {
  m_previewBase = m_doc;
  m_previewing = false;
}

void LayoutModel::setPreview(const Doc &d) {
  m_doc = d;
  m_previewing = toJson(m_doc) != toJson(m_previewBase);
  ++m_revision;
  emit changed();
}

void LayoutModel::commitPreview() {
  if (!m_previewing)
    return;
  m_previewing = false;
  cleanPaths();
  touched();
}

void LayoutModel::clearAll() {
  m_doc.paths.clear();
  m_doc.rooms.clear();
  m_doc.sealed.clear();
  m_selSegs.clear();
  m_selRooms.clear();
  touched();
}

void LayoutModel::refreshLayouts() {
  m_layouts = m_engine ? m_engine->layoutDocs() : QList<QJsonObject> {};
  emit layoutsChanged();
}

QStringList LayoutModel::layoutNames() const {
  QStringList out;
  for (const auto &d: m_layouts)
    out << QStringLiteral("%1  (#%2)").arg(d.value(QStringLiteral("name")).toString()).arg(d.value(QStringLiteral("id")).toInt());
  return out;
}

int LayoutModel::layoutIndex() const {
  for (int i = 0; i < m_layouts.size(); ++i)
    if (m_layouts[i].value(QStringLiteral("id")).toInt() == m_doc.id)
      return i;
  return -1;
}

QStringList LayoutModel::groupNames() const {
  QStringList out;
  if (m_engine)
    for (const auto &g: m_engine->meshGroups())
      out << g.name;
  return out;
}

QList<int> LayoutModel::groupIds() const {
  QList<int> out;
  if (m_engine)
    for (const auto &g: m_engine->meshGroups())
      out << int(g.id);
  return out;
}

void LayoutModel::newLayout() {
  Doc d;
  d.id = m_engine ? m_engine->mintLayoutId() : 1;
  // Unsaved layouts may collide; bump past the current one.
  if (d.id <= m_doc.id)
    d.id = m_doc.id + 1;
  d.name = QStringLiteral("Layout %1").arg(d.id);
  d.config = m_doc.config;
  d.config.groupName = QStringLiteral("layout_%1").arg(d.id);
  if (m_doc.paths.empty() && m_doc.rooms.empty() && m_history.empty())
    d.config = vk::Engine3D::layoutDefaults(); // the project's sizes
  m_doc = d;
  m_history.clear();
  m_histPos = -1;
  m_selSegs.clear();
  m_selRooms.clear();
  placeAtCamera();
  touched();
  emit layoutsChanged();
}

void LayoutModel::placeAtCamera() {
  if (!m_engine || !m_engine->camera() || layoutIndex() >= 0 || !m_engine->layoutDoc(m_doc.id).isEmpty())
    return;
  constexpr double kSnap = 32.0;
  const glm::vec3 eye = m_engine->camera()->getPosition();
  auto snap = [](float world) {
    return float(std::round(double(world) * 10.0 / kSnap) * kSnap / 10.0);
  };
  const glm::vec3 origin(snap(eye.x), snap(eye.y), snap(eye.z));
  if (origin == m_doc.origin)
    return;
  m_doc.origin = origin;
  ++m_revision;
  emit changed();
}

void LayoutModel::loadLayout(const int index) {
  if (index < 0 || index >= m_layouts.size())
    return;
  m_doc = fromJson(m_layouts[index]);
  m_history.clear();
  m_histPos = -1;
  m_selSegs.clear();
  m_selRooms.clear();
  touched();
  emit layoutsChanged();
}

void LayoutModel::loadLayoutById(const int id) {
  for (int i = 0; i < m_layouts.size(); ++i)
    if (m_layouts[i].value(QStringLiteral("id")).toInt() == id) {
      loadLayout(i);
      return;
    }
}

void LayoutModel::generate() {
  if (!m_engine)
    return;
  const int n = m_engine->generateLayout(toJson(m_doc));
  m_lastResult = tr("%n brush(es) generated", "", n);
  emit statusChanged();
  refreshLayouts();
  // A layout already in the map keeps its place.
  const QJsonObject stored = m_engine->layoutDoc(m_doc.id);
  if (!stored.isEmpty())
    m_doc.origin = fromJson(stored).origin;
}

void LayoutModel::removeFromMap() {
  if (m_engine)
    m_engine->removeLayoutBrushes(m_doc.id);
  m_lastResult = tr("brushes removed from the map");
  emit statusChanged();
}

void LayoutModel::useCurrentMaterial(const QString &slot) {
  if (!m_engine || m_engine->currentMaterial().isEmpty())
    return;
  setCfg(slot, m_engine->currentMaterial());
}

LayoutCanvas::LayoutCanvas(QQuickItem *parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
  setAcceptHoverEvents(true);
  setFlag(ItemAcceptsInputMethod, false);
  setFlag(ItemIsFocusScope, true);
  setActiveFocusOnTab(true);
  setAntialiasing(true);
  setFillColor(QColor(16, 20, 40));
}

void LayoutCanvas::setModel(QObject *m) {
  auto *lm = qobject_cast<LayoutModel *>(m);
  if (lm == m_model)
    return;
  if (m_model)
    disconnect(m_model, nullptr, this, nullptr);
  m_model = lm;
  if (m_model) {
    connect(m_model, &LayoutModel::changed, this, [this] { update(); });
    connect(m_model, &LayoutModel::gridChanged, this, [this] { update(); });
  }
  emit modelChanged();
  update();
}

glm::dvec2 LayoutCanvas::toPlan(const QPointF &px) const {
  return {m_center.x + (px.x() - width() * 0.5) / m_zoom, m_center.y + (px.y() - height() * 0.5) / m_zoom};
}

QPointF LayoutCanvas::toScreen(const glm::dvec2 &su) const {
  return {(su.x - m_center.x) * m_zoom + width() * 0.5, (su.y - m_center.y) * m_zoom + height() * 0.5};
}

QPoint LayoutCanvas::nearestNode(const glm::dvec2 &su) const {
  const int g = m_model ? m_model->gridSize() : 64;
  return {int(std::lround(su.x / g)) * g, int(std::lround(su.y / g)) * g};
}

QPoint LayoutCanvas::cellAt(const glm::dvec2 &su) const {
  const int g = m_model ? m_model->gridSize() : 64;
  return {int(std::floor(su.x / g)) * g, int(std::floor(su.y / g)) * g};
}

int LayoutCanvas::newRoomContact() const {
  const int def = m_model ? m_model->doc().config.roomContact : ContactOpen;
  if (!m_roomFlip)
    return def;
  return def == ContactSharedWall ? ContactOpen : ContactSharedWall;
}

void LayoutCanvas::fitAll() {
  if (!m_model)
    return;
  const Raster &r = m_model->raster();
  if (r.cells.isEmpty()) {
    m_center = {0, 0};
    m_zoom = 0.5;
    update();
    return;
  }
  QRect box;
  for (auto it = r.cells.constBegin(); it != r.cells.constEnd(); ++it)
    box = box.united(r.cellRect(it.key()));
  m_center = {box.x() + box.width() * 0.5, box.y() + box.height() * 0.5};
  m_zoom = std::clamp(std::min(width() / (box.width() + 256.0), height() / (box.height() + 256.0)), 0.02, 8.0);
  update();
}

void LayoutCanvas::drawLabel(QPainter *p, const QPointF &at, const QString &text, const QColor &color) const {
  const QFontMetricsF fm(p->font());
  const QRectF r = fm.boundingRect(text).adjusted(-4, -2, 4, 2).translated(at);
  p->setPen(Qt::NoPen);
  p->setBrush(QColor(10, 12, 26, 190));
  p->drawRoundedRect(r, 3, 3);
  p->setBrush(Qt::NoBrush);
  p->setPen(color);
  p->drawText(at, text);
}

void LayoutCanvas::paint(QPainter *p) {
  if (!m_model)
    return;
  const Doc &doc = m_model->doc();
  const int g = m_model->gridSize();
  p->setRenderHint(QPainter::Antialiasing, false);

  // Grid lines (when far enough apart), a major line every 8.
  const glm::dvec2 lo = toPlan({0, 0}), hi = toPlan({width(), height()});
  for (int pass = 0; pass < 2; ++pass) {
    const int step = pass == 0 ? g : g * 8;
    if (step * m_zoom < 6)
      continue;
    p->setPen(QPen(pass == 0 ? QColor(36, 44, 72) : QColor(58, 70, 110), 1));
    const int gx0 = int(std::floor(lo.x / step)), gx1 = int(std::ceil(hi.x / step));
    for (int gx = gx0; gx <= gx1; ++gx) {
      const double x = toScreen({double(gx) * step, 0}).x();
      p->drawLine(QPointF(x, 0), QPointF(x, height()));
    }
    const int gy0 = int(std::floor(lo.y / step)), gy1 = int(std::ceil(hi.y / step));
    for (int gy = gy0; gy <= gy1; ++gy) {
      const double y = toScreen({0, double(gy) * step}).y();
      p->drawLine(QPointF(0, y), QPointF(width(), y));
    }
  }
  p->setPen(QPen(QColor(120, 70, 80), 1));
  const QPointF o = toScreen({0, 0});
  p->drawLine(o - QPointF(10, 0), o + QPointF(10, 0));
  p->drawLine(o - QPointF(0, 10), o + QPointF(0, 10));

  // Generated cells: rooms green, corridors blue (lighter = wider), vents orange.
  const Raster &ras = m_model->raster();
  std::map<int, const Path *> pathById;
  for (const Path &path: doc.paths)
    pathById[path.id] = &path;
  for (auto it = ras.cells.constBegin(); it != ras.cells.constEnd(); ++it) {
    const CellInfo &ci = it.value();
    const QRect cr = ras.cellRect(it.key());
    const QPointF a = toScreen({double(cr.x()), double(cr.y())});
    const QPointF b = toScreen({double(cr.x() + cr.width()), double(cr.y() + cr.height())});
    const QRectF r(QPointF(std::floor(a.x()), std::floor(a.y())), QPointF(std::ceil(b.x()), std::ceil(b.y())));
    if (ci.kind == Room) {
      p->fillRect(r, ci.sealed ? QColor(140, 50, 50) : QColor(46, 120, 78));
    } else if (ci.kind == Corridor) {
      int w = 128;
      if (const auto pit = pathById.find(ci.shape); pit != pathById.end() && ci.seg >= 0
                                                      && ci.seg < int(pit->second->segs.size()))
        w = pit->second->segs[size_t(ci.seg)].width;
      const int light = std::clamp(70 + (w - 128) / 2, 40, 150);
      p->fillRect(r, ci.sealed ? QColor(140, 50, 50) : QColor(40, light, 170 + light / 4));
    }
    if (ci.vent)
      p->fillRect(r, ci.kind >= 0 ? QColor(230, 140, 40, 150) : QColor(200, 120, 30));
  }

  p->setRenderHint(QPainter::Antialiasing, true);
  const bool dims = m_model->showDimensions();
  // Room outlines; shared-wall rooms get a second, inner line.
  for (size_t i = 0; i < doc.rooms.size(); ++i) {
    const RoomShape &room = doc.rooms[i];
    const bool sel = std::find(m_model->selectedRooms().begin(), m_model->selectedRooms().end(), int(i))
                     != m_model->selectedRooms().end();
    const QColor col = sel ? QColor(255, 230, 120) : QColor(130, 220, 160);
    p->setBrush(Qt::NoBrush);
    for (const QRect &rc: room.rects) {
      const QRectF r(toScreen({double(rc.x()), double(rc.y())}),
                     toScreen({double(rc.x() + rc.width()), double(rc.y() + rc.height())}));
      p->setPen(QPen(col, sel ? 2.5 : 1.2));
      p->drawRect(r);
      if (room.contact == ContactSharedWall && r.width() > 8 && r.height() > 8) {
        p->setPen(QPen(col, 1.0, Qt::DotLine));
        p->drawRect(r.adjusted(3, 3, -3, -3));
      }
    }
    if (dims && !room.rects.empty()) {
      const QRect &rc = room.rects.front();
      QString t = QStringLiteral("%1 × %2 × %3").arg(rc.width()).arg(rc.height()).arg(room.height);
      if (room.contact == ContactSharedWall)
        t += tr("  · shared wall");
      else if (room.contact == ContactDoorway)
        t += tr("  · doorway");
      drawLabel(p, toScreen({double(rc.x()), double(rc.y())}) + QPointF(6, 16), t, QColor(200, 240, 210));
    }
  }
  for (size_t pi = 0; pi < doc.paths.size(); ++pi) {
    const Path &path = doc.paths[pi];
    bool pathSel = false;
    for (size_t s = 0; s + 1 < path.nodes.size() && s < path.segs.size(); ++s) {
      const QPointF a = toScreen(su2(path.nodes[s])), b = toScreen(su2(path.nodes[s + 1]));
      const bool sel = m_model->isSegSelected(int(pi), int(s));
      pathSel = pathSel || sel;
      p->setPen(QPen(sel ? QColor(255, 230, 120) : (path.kind == Vent ? QColor(255, 200, 120) : QColor(180, 210, 255)),
                     sel ? 2.5 : 1.0, sel ? Qt::SolidLine : Qt::DashLine));
      p->drawLine(a, b);
      const int w = path.segs[s].width;
      if (sel) {
        // Side handles change the width.
        const QPointF d = b - a;
        const double len = std::hypot(d.x(), d.y());
        if (len > 1e-6) {
          const QPointF n(-d.y() / len, d.x() / len);
          const double off = w * 0.5 * m_zoom;
          p->setPen(QPen(QColor(255, 230, 120), 2.0));
          p->drawLine(a + n * off, b + n * off);
          p->drawLine(a - n * off, b - n * off);
          p->setBrush(QColor(255, 230, 120));
          const QPointF m = (a + b) * 0.5;
          p->drawEllipse(m + n * off, 4.5, 4.5);
          p->drawEllipse(m - n * off, 4.5, 4.5);
          p->setBrush(Qt::NoBrush);
        }
      }
      if (dims)
        drawLabel(p, (a + b) * 0.5 + QPointF(8, -8),
                  QStringLiteral("%1 × %2").arg(w).arg(segmentLength(path, int(s))), QColor(235, 240, 255));
    }
    p->setPen(Qt::NoPen);
    p->setBrush(path.kind == Vent ? QColor(255, 200, 120) : QColor(200, 220, 255));
    for (const QPoint &n: path.nodes)
      p->drawEllipse(toScreen(su2(n)), 3.0, 3.0);
    if (pathSel) {
      // Node handles: an end changes the length, a corner moves.
      p->setPen(QPen(QColor(20, 24, 40), 1.0));
      p->setBrush(QColor(255, 230, 120));
      for (const QPoint &n: path.nodes)
        p->drawRect(QRectF(toScreen(su2(n)) - QPointF(5, 5), QSizeF(10, 10)));
    }
    p->setBrush(Qt::NoBrush);
  }
  // Sealed squares: a cross.
  p->setPen(QPen(QColor(255, 90, 90), 1.5));
  for (const QRect &s: doc.sealed) {
    const QPointF a = toScreen({double(s.x()), double(s.y())});
    const QPointF b = toScreen({double(s.x() + s.width()), double(s.y() + s.height())});
    p->drawLine(a, b);
    p->drawLine(QPointF(a.x(), b.y()), QPointF(b.x(), a.y()));
  }

  if (m_drag == Drag::Stroke && !m_stroke.empty()) {
    const bool vent = m_model->tool() == LayoutModel::ToolVent;
    const int w = m_model->drawWidth();
    const QColor ghost = vent ? QColor(255, 170, 60, 110) : QColor(110, 170, 255, 110);
    QPainterPath path;
    std::vector<QPoint> nodes = m_stroke;
    if (nodes.back() != m_strokeEnd)
      nodes.push_back(m_strokeEnd);
    for (size_t i = 0; i < nodes.size(); ++i) {
      const QPointF q = toScreen(su2(nodes[i]));
      i == 0 ? path.moveTo(q) : path.lineTo(q);
    }
    p->setPen(QPen(ghost, std::max(1.0, w * m_zoom), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p->drawPath(path);
    p->setPen(QPen(QColor(255, 255, 255), 1.5));
    p->drawPath(path);
    double total = 0.0;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
      const glm::dvec2 a = su2(nodes[i]), b = su2(nodes[i + 1]);
      const double len = glm::length(b - a);
      total += len;
      drawLabel(p, toScreen((a + b) * 0.5) + QPointF(8, -8),
                QStringLiteral("%1 × %2").arg(w).arg(int(std::lround(len)) + w), Qt::white);
    }
    if (nodes.size() > 2)
      drawLabel(p, toScreen(su2(m_strokeEnd)) + QPointF(12, 18), tr("total %1").arg(int(std::lround(total)) + w),
                QColor(255, 230, 120));
  }
  if (m_drag == Drag::Room) {
    const int w = std::abs(m_roomEnd.x() - m_roomStart.x()), h = std::abs(m_roomEnd.y() - m_roomStart.y());
    const QPointF a = toScreen({double(std::min(m_roomStart.x(), m_roomEnd.x())), double(std::min(m_roomStart.y(), m_roomEnd.y()))});
    const QPointF b = a + QPointF(w * m_zoom, h * m_zoom);
    p->fillRect(QRectF(a, b), QColor(80, 200, 120, 90));
    p->setPen(QPen(Qt::white, 1.5));
    p->drawRect(QRectF(a, b));
    QString t = QStringLiteral("%1 × %2").arg(w).arg(h);
    if (m_model->tool() == LayoutModel::ToolRoom && newRoomContact() == ContactSharedWall)
      t += tr("  · shared wall");
    drawLabel(p, b + QPointF(10, 0), t, Qt::white);
  }
}

void LayoutCanvas::updateStatus(const glm::dvec2 &su) {
  if (!m_model)
    return;
  m_model->setStatus(tr("%1, %2 u  ·  grid %3  ·  zoom %4%")
                         .arg(int(std::floor(su.x)))
                         .arg(int(std::floor(su.y)))
                         .arg(m_model->gridSize())
                         .arg(int(std::lround(m_zoom * 100))));
}

void LayoutCanvas::updateStroke(const glm::dvec2 &su, const bool diagonal) {
  const int g = m_model ? m_model->gridSize() : 64;
  const QPoint cur = nearestNode(su);
  const QPoint last = m_stroke.back();
  const QPoint d = cur - last;
  if (diagonal) {
    // 8 directions; length along the dominant component.
    const int ax = std::abs(d.x()), ay = std::abs(d.y());
    if (ax > 2 * ay)
      m_strokeEnd = {cur.x(), last.y()};
    else if (ay > 2 * ax)
      m_strokeEnd = {last.x(), cur.y()};
    else {
      const int len = std::max(ax, ay);
      m_strokeEnd = {last.x() + (d.x() >= 0 ? len : -len), last.y() + (d.y() >= 0 ? len : -len)};
    }
    m_strokeAxis = 2;
    return;
  }
  if (m_strokeAxis < 0 || m_strokeAxis == 2) {
    if (std::abs(d.x()) >= g || std::abs(d.y()) >= g)
      m_strokeAxis = std::abs(d.x()) >= std::abs(d.y()) ? 0 : 1;
  }
  // Leaving the current axis by two grid steps turns a corner.
  const int along = m_strokeAxis == 0 ? d.x() : d.y();
  const int perp = m_strokeAxis == 0 ? d.y() : d.x();
  if (m_strokeAxis >= 0 && std::abs(perp) >= 2 * g && std::abs(along) >= g) {
    const QPoint corner = m_strokeAxis == 0 ? QPoint(cur.x(), last.y()) : QPoint(last.x(), cur.y());
    m_stroke.push_back(corner);
    m_strokeAxis = 1 - m_strokeAxis;
    updateStroke(su, false);
    return;
  }
  m_strokeEnd = m_strokeAxis == 0 ? QPoint(cur.x(), last.y()) : m_strokeAxis == 1 ? QPoint(last.x(), cur.y()) : last;
}

void LayoutCanvas::dragNode(const glm::dvec2 &su, const bool free) {
  Doc d = m_model->previewBase();
  if (m_nodePath < 0 || m_nodePath >= int(d.paths.size()))
    return;
  const Path &base = m_model->previewBase().paths[size_t(m_nodePath)];
  Path &p = d.paths[size_t(m_nodePath)];
  const int n = int(p.nodes.size()), k = m_nodeIndex;
  if (k < 0 || k >= n)
    return;
  QPoint target = nearestNode(su);
  const QPoint orig = base.nodes[size_t(k)];
  if (!free && (k == 0 || k == n - 1) && n >= 2) {
    // An end slides along its segment: shorter or longer, never bent.
    const QPoint nb = base.nodes[size_t(k == 0 ? 1 : n - 2)];
    if (horizontal(orig, nb))
      target.setY(orig.y());
    else if (vertical(orig, nb))
      target.setX(orig.x());
  }
  p.nodes[size_t(k)] = target;
  if (!free) {
    // Neighbours follow on the shared axis so every segment stays straight.
    for (const int j: {k - 1, k + 1}) {
      if (j < 0 || j >= n)
        continue;
      const QPoint nb = base.nodes[size_t(j)];
      if (horizontal(orig, nb))
        p.nodes[size_t(j)].setY(target.y());
      else if (vertical(orig, nb))
        p.nodes[size_t(j)].setX(target.x());
    }
  }
  m_model->setPreview(d);
}

void LayoutCanvas::dragMove(const glm::dvec2 &su) {
  const QPoint delta = nearestNode(su) - nearestNode(m_dragStart);
  const Doc &base = m_model->previewBase();
  Doc d = base;
  for (const int r: m_model->selectedRooms())
    if (r >= 0 && r < int(d.rooms.size()))
      for (QRect &rc: d.rooms[size_t(r)].rects)
        rc.translate(delta);
  // A fully selected path moves whole; otherwise each segment moves across its axis and neighbours stretch.
  std::map<int, std::vector<int>> byPath;
  for (const auto &s: m_model->selectedSegs())
    byPath[s.path].push_back(s.seg);
  for (const auto &[pi, segs]: byPath) {
    if (pi < 0 || pi >= int(d.paths.size()))
      continue;
    Path &p = d.paths[size_t(pi)];
    const Path &bp = base.paths[size_t(pi)];
    if (m_model->isPathSelected(pi) || bp.segs.size() == 1) {
      for (QPoint &n: p.nodes)
        n += delta;
      continue;
    }
    std::vector<int> moveX(p.nodes.size(), 0), moveY(p.nodes.size(), 0);
    for (const int s: segs) {
      const QPoint a = bp.nodes[size_t(s)], b = bp.nodes[size_t(s) + 1];
      const bool h = horizontal(a, b), v = vertical(a, b);
      for (const int k: {s, s + 1}) {
        if (!h)
          moveX[size_t(k)] = 1;
        if (!v)
          moveY[size_t(k)] = 1;
      }
    }
    for (size_t k = 0; k < p.nodes.size(); ++k)
      p.nodes[k] += QPoint(moveX[k] ? delta.x() : 0, moveY[k] ? delta.y() : 0);
  }
  m_model->setPreview(d);
}

void LayoutCanvas::mousePressEvent(QMouseEvent *e) {
  forceActiveFocus();
  if (!m_model)
    return;
  const glm::dvec2 su = toPlan(e->position());
  m_lastPx = e->position();
  const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);
  const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);
  m_roomFlip = e->modifiers().testFlag(Qt::AltModifier);
  if (e->button() == Qt::MiddleButton) {
    m_drag = Drag::Pan;
    return;
  }
  if (e->button() == Qt::RightButton) {
    m_drag = Drag::Erase;
    m_model->eraseAt(su);
    return;
  }
  switch (m_model->tool()) {
    case LayoutModel::ToolCorridor:
    case LayoutModel::ToolVent:
      m_drag = Drag::Stroke;
      m_stroke = {nearestNode(su)};
      m_strokeEnd = m_stroke.back();
      m_strokeAxis = -1;
      break;
    case LayoutModel::ToolRoom:
      m_drag = Drag::Room;
      m_roomStart = nearestNode(su);
      m_roomEnd = m_roomStart;
      break;
    case LayoutModel::ToolSeal: {
      const int g = m_model->gridSize();
      m_model->toggleSeal(QRect(cellAt(su), QSize(g, g)));
      break;
    }
    case LayoutModel::ToolErase:
      m_drag = Drag::Erase;
      m_model->eraseAt(su);
      break;
    case LayoutModel::ToolSelect: {
      const Doc &doc = m_model->doc();
      constexpr double tolPx = 7.0;
      for (int pi = 0; pi < int(doc.paths.size()); ++pi) {
        bool any = false;
        for (int s = 0; s < int(doc.paths[size_t(pi)].segs.size()) && !any; ++s)
          any = m_model->isSegSelected(pi, s);
        if (!any)
          continue;
        const auto &nodes = doc.paths[size_t(pi)].nodes;
        for (int k = 0; k < int(nodes.size()); ++k) {
          const QPointF d = toScreen(su2(nodes[size_t(k)])) - e->position();
          if (std::max(std::fabs(d.x()), std::fabs(d.y())) <= tolPx) {
            m_drag = Drag::Node;
            m_nodePath = pi;
            m_nodeIndex = k;
            m_model->beginPreview();
            return;
          }
        }
      }
      for (const auto &ref: m_model->selectedSegs()) {
        const Path &path = doc.paths[size_t(ref.path)];
        const glm::dvec2 a = su2(path.nodes[size_t(ref.seg)]), b = su2(path.nodes[size_t(ref.seg) + 1]);
        if (std::fabs(segDistance(su, a, b) - path.segs[size_t(ref.seg)].width * 0.5) * m_zoom < tolPx) {
          m_drag = Drag::Width;
          m_widthSeg = ref;
          m_model->beginPreview();
          return;
        }
      }
      for (const int ri: m_model->selectedRooms()) {
        const auto &rects = doc.rooms[size_t(ri)].rects;
        for (size_t k = 0; k < rects.size(); ++k) {
          const QRect &rc = rects[k];
          const double l = rc.x(), t = rc.y(), r = rc.x() + rc.width(), b = rc.y() + rc.height();
          const double tol = tolPx / m_zoom;
          const bool inY = su.y > t - tol && su.y < b + tol, inX = su.x > l - tol && su.x < r + tol;
          int side = -1;
          if (inY && std::fabs(su.x - l) < tol)
            side = 0;
          else if (inY && std::fabs(su.x - r) < tol)
            side = 1;
          else if (inX && std::fabs(su.y - t) < tol)
            side = 2;
          else if (inX && std::fabs(su.y - b) < tol)
            side = 3;
          if (side >= 0) {
            m_drag = Drag::RoomEdge;
            m_edgeRoom = ri;
            m_edgeRect = int(k);
            m_edgeSide = side;
            m_model->beginPreview();
            return;
          }
        }
      }
      if (ctrl && m_model->selectedRooms().size() == 1) {
        // Ctrl+drag adds a rectangle to the selected room.
        m_drag = Drag::Room;
        m_roomStart = nearestNode(su);
        m_roomEnd = m_roomStart;
        return;
      }
      const LayoutModel::SegRef hit = m_model->segmentAt(su);
      const int room = hit.path < 0 ? m_model->roomAt(su) : -1;
      const bool onSelection =
          (hit.path >= 0 && m_model->isSegSelected(hit.path, hit.seg))
          || (room >= 0
              && std::find(m_model->selectedRooms().begin(), m_model->selectedRooms().end(), room)
                     != m_model->selectedRooms().end());
      if (!onSelection)
        m_model->selectAt(su, shift || ctrl, false);
      if (!shift && !ctrl && m_model->selCount() > 0 && (hit.path >= 0 || room >= 0)) {
        m_drag = Drag::Move;
        m_dragStart = su;
        m_model->beginPreview();
      } else {
        m_drag = Drag::Select;
      }
      break;
    }
    default:
      break;
  }
  update();
}

void LayoutCanvas::mouseDoubleClickEvent(QMouseEvent *e) {
  if (m_model && m_model->tool() == LayoutModel::ToolSelect && e->button() == Qt::LeftButton)
    m_model->selectAt(toPlan(e->position()), e->modifiers().testFlag(Qt::ShiftModifier), true);
}

void LayoutCanvas::mouseMoveEvent(QMouseEvent *e) {
  if (!m_model)
    return;
  const glm::dvec2 su = toPlan(e->position());
  updateStatus(su);
  switch (m_drag) {
    case Drag::Pan: {
      const QPointF d = e->position() - m_lastPx;
      m_center -= glm::dvec2(d.x(), d.y()) / m_zoom;
      break;
    }
    case Drag::Stroke:
      updateStroke(su, e->modifiers().testFlag(Qt::ShiftModifier));
      break;
    case Drag::Room:
      m_roomEnd = nearestNode(su);
      m_roomFlip = e->modifiers().testFlag(Qt::AltModifier);
      break;
    case Drag::Erase:
      m_model->eraseAt(su);
      break;
    case Drag::Width: {
      const Doc &base = m_model->previewBase();
      if (m_widthSeg.path < 0 || m_widthSeg.path >= int(base.paths.size()))
        break;
      const Path &path = base.paths[size_t(m_widthSeg.path)];
      const glm::dvec2 a = su2(path.nodes[size_t(m_widthSeg.seg)]), b = su2(path.nodes[size_t(m_widthSeg.seg) + 1]);
      const int step = path.kind == Vent ? 8 : 16;
      const int w = std::max(step, int(std::lround(2.0 * segDistance(su, a, b) / step)) * step);
      Doc d = base;
      for (const auto &s: m_model->selectedSegs())
        d.paths[size_t(s.path)].segs[size_t(s.seg)].width = w;
      d.paths[size_t(m_widthSeg.path)].segs[size_t(m_widthSeg.seg)].width = w;
      m_model->setPreview(d);
      break;
    }
    case Drag::RoomEdge: {
      Doc d = m_model->previewBase();
      if (m_edgeRoom < 0 || m_edgeRoom >= int(d.rooms.size()) || m_edgeRect < 0
          || m_edgeRect >= int(d.rooms[size_t(m_edgeRoom)].rects.size()))
        break;
      QRect &rc = d.rooms[size_t(m_edgeRoom)].rects[size_t(m_edgeRect)];
      int l = rc.x(), t = rc.y(), r = rc.x() + rc.width(), b = rc.y() + rc.height();
      const QPoint n = nearestNode(su);
      constexpr int kMin = 8;
      if (m_edgeSide == 0)
        l = std::min(n.x(), r - kMin);
      else if (m_edgeSide == 1)
        r = std::max(n.x(), l + kMin);
      else if (m_edgeSide == 2)
        t = std::min(n.y(), b - kMin);
      else
        b = std::max(n.y(), t + kMin);
      rc = QRect(l, t, r - l, b - t);
      m_model->setPreview(d);
      break;
    }
    case Drag::Node:
      dragNode(su, e->modifiers().testFlag(Qt::ShiftModifier));
      break;
    case Drag::Move:
      dragMove(su);
      break;
    default:
      break;
  }
  m_lastPx = e->position();
  update();
}

void LayoutCanvas::mouseReleaseEvent(QMouseEvent *) {
  if (!m_model) {
    m_drag = Drag::None;
    return;
  }
  switch (m_drag) {
    case Drag::Stroke: {
      std::vector<QPoint> nodes = m_stroke;
      if (m_strokeEnd != nodes.back())
        nodes.push_back(m_strokeEnd);
      m_drag = Drag::None;
      m_model->addPath(m_model->tool() == LayoutModel::ToolVent ? Vent : Corridor, nodes);
      break;
    }
    case Drag::Room: {
      m_drag = Drag::None;
      const int x0 = std::min(m_roomStart.x(), m_roomEnd.x()), x1 = std::max(m_roomStart.x(), m_roomEnd.x());
      const int y0 = std::min(m_roomStart.y(), m_roomEnd.y()), y1 = std::max(m_roomStart.y(), m_roomEnd.y());
      m_model->addRoomRect(QRect(x0, y0, x1 - x0, y1 - y0), m_model->tool() == LayoutModel::ToolSelect,
                           newRoomContact());
      break;
    }
    case Drag::Width:
    case Drag::RoomEdge:
    case Drag::Node:
    case Drag::Move:
      m_drag = Drag::None;
      m_model->commitPreview();
      break;
    default:
      break;
  }
  m_drag = Drag::None;
  update();
}

void LayoutCanvas::hoverMoveEvent(QHoverEvent *e) { updateStatus(toPlan(e->position())); }

void LayoutCanvas::wheelEvent(QWheelEvent *e) {
  const glm::dvec2 before = toPlan(e->position());
  const double f = std::pow(1.0015, e->angleDelta().y());
  m_zoom = std::clamp(m_zoom * f, 0.02, 8.0);
  const glm::dvec2 after = toPlan(e->position());
  m_center += before - after;
  updateStatus(after);
  update();
}

void LayoutCanvas::keyPressEvent(QKeyEvent *e) {
  if (!m_model)
    return;
  switch (e->key()) {
    case Qt::Key_BracketLeft:
      m_model->stepGrid(-1);
      break;
    case Qt::Key_BracketRight:
      m_model->stepGrid(1);
      break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      m_model->deleteSelection();
      break;
    case Qt::Key_Escape:
      if (m_drag == Drag::Node || m_drag == Drag::Move || m_drag == Drag::Width || m_drag == Drag::RoomEdge)
        m_model->setPreview(m_model->previewBase());
      if (m_drag != Drag::None)
        m_drag = Drag::None;
      else
        m_model->clearSelection();
      break;
    case Qt::Key_F:
      fitAll();
      break;
    case Qt::Key_1: m_model->setTool(LayoutModel::ToolSelect); break;
    case Qt::Key_2: m_model->setTool(LayoutModel::ToolCorridor); break;
    case Qt::Key_3: m_model->setTool(LayoutModel::ToolRoom); break;
    case Qt::Key_4: m_model->setTool(LayoutModel::ToolVent); break;
    case Qt::Key_5: m_model->setTool(LayoutModel::ToolSeal); break;
    case Qt::Key_6: m_model->setTool(LayoutModel::ToolErase); break;
    default:
      QQuickPaintedItem::keyPressEvent(e);
      return;
  }
  update();
}

namespace {
QPointer<LayoutDesigner> s_window;
}

// No Qt parent: its own taskbar entry, not kept above the main window. The main window's
// closeEvent closes it via closeIfOpen().
LayoutDesigner::LayoutDesigner(vk::Engine3D *engine, Ctx *ctx, QWidget *owner) : QMainWindow(nullptr, Qt::Window) {
  setObjectName(QStringLiteral("LayoutDesigner"));
  setWindowTitle(tr("Layout Designer"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setAttribute(Qt::WA_QuitOnClose, false);
  if (owner)
    connect(owner, &QObject::destroyed, this, &QObject::deleteLater);
  resize(scale::initialSize(this, {1500, 950}));
  static bool registered = false;
  if (!registered) {
    qmlRegisterType<LayoutCanvas>("LayoutDesigner", 1, 0, "LayoutCanvas");
    registered = true;
  }
  m_model = new LayoutModel(engine, this);
  m_view = new QQuickWidget(this);
  m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
  m_view->rootContext()->setContextProperty(QStringLiteral("layoutModel"), m_model);
  m_view->rootContext()->setContextProperty(QStringLiteral("ctx"), ctx);
  m_view->setSource(QUrl(QStringLiteral("qrc:/Main/qml/LayoutDesigner.qml")));

  m_panel = new LayoutPanel(m_model, this);
  m_split = new QSplitter(Qt::Horizontal, this);
  m_split->addWidget(m_view);
  m_split->addWidget(m_panel);
  m_split->setStretchFactor(0, 1);
  m_split->setStretchFactor(1, 0);
  m_split->setCollapsible(0, false);
  m_split->setSizes({scale::px(1150), scale::px(350)});
  setCentralWidget(m_split);
  connect(m_panel, &LayoutPanel::fitRequested, this, &LayoutDesigner::fitView);

  auto *bar = addToolBar(tr("Layout"));
  bar->setObjectName(QStringLiteral("layoutDesigner.toolbar"));
  bar->setMovable(false);
  bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto add = [this, bar](const QString &text, const char *icon, const QKeySequence &key, auto fn) {
    QAction *a = bar->addAction(ActionManager::icon(QString::fromLatin1(icon)), text);
    if (!key.isEmpty()) {
      a->setShortcut(key);
      a->setShortcutContext(Qt::WindowShortcut);
      a->setToolTip(QStringLiteral("%1  (%2)").arg(text, key.toString(QKeySequence::NativeText)));
    }
    connect(a, &QAction::triggered, this, fn);
    return a;
  };
  QAction *undo = add(tr("Undo"), "loop_back", QKeySequence::Undo, [this] { m_model->undo(); });
  QAction *redo = add(tr("Redo"), "loop_forwards", QKeySequence(QStringLiteral("Ctrl+Shift+Z")), [this] { m_model->redo(); });
  auto *redoAlt = new QAction(this);
  redoAlt->setShortcut(QKeySequence(QStringLiteral("Ctrl+Y")));
  redoAlt->setShortcutContext(Qt::WindowShortcut);
  connect(redoAlt, &QAction::triggered, this, [this] { m_model->redo(); });
  addAction(redoAlt);
  bar->addSeparator();
  add(tr("Fit"), "zoom_all", QKeySequence(), [this] { fitView(); });
  bar->addSeparator();
  add(tr("Clear all"), "trash", QKeySequence(), [this] { m_model->clearAll(); });

  auto *spacer = new QWidget(bar);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  bar->addWidget(spacer);
  bar->addWidget(new QLabel(tr("Brushes go to "), bar));
  auto *target = new QComboBox(bar);
  target->addItems({tr("Worldspawn"), tr("New mesh group"), tr("Existing mesh group")});
  bar->addWidget(target);
  auto *groupName = new QLineEdit(bar);
  groupName->setPlaceholderText(tr("group name"));
  scale::bind(groupName, [=, this] { groupName->setMinimumWidth(scale::tpx(140)); });
  QAction *groupNameAct = bar->addWidget(groupName);
  auto *group = new QComboBox(bar);
  scale::bind(group, [=, this] { group->setMinimumWidth(scale::tpx(140)); });
  QAction *groupAct = bar->addWidget(group);
  auto *live = new QCheckBox(tr("Live"), bar);
  live->setToolTip(tr("Regenerate the brushes on every edit"));
  bar->addWidget(live);
  bar->addSeparator();
  add(tr("Remove brushes"), "x", QKeySequence(), [this] { m_model->removeFromMap(); })
      ->setToolTip(tr("Remove this layout's brushes from the map"));
  auto *gen = new QPushButton(ActionManager::icon(QStringLiteral("mod_build")), tr("Generate brushes"), bar);
  gen->setObjectName(QStringLiteral("layoutGenerate"));
  gen->setStyleSheet(QStringLiteral("#layoutGenerate { background:#3b7a5c; color:white; font-weight:bold; "
                                    "padding:5px 14px; border:1px solid #4f9a76; border-radius:4px; }"
                                    "#layoutGenerate:hover { background:#46906c; }"));
  gen->setToolTip(tr("Build the layout's brushes in the map at the 3D camera (snapped to 32 units). "
                     "Regenerating replaces them."));
  bar->addWidget(gen);
  connect(gen, &QPushButton::clicked, this, [this] { m_model->generate(); });
  connect(target, &QComboBox::activated, this, [this](int i) { m_model->setCfg(QStringLiteral("target"), i); });
  connect(groupName, &QLineEdit::editingFinished, this,
          [this, groupName] { m_model->setCfg(QStringLiteral("groupName"), groupName->text()); });
  connect(group, &QComboBox::activated, this,
          [this, group] { m_model->setCfg(QStringLiteral("groupId"), group->currentData().toInt()); });
  connect(live, &QCheckBox::toggled, this, [this](bool on) { m_model->setLive(on); });
  auto syncOutput = [this, target, groupName, groupNameAct, group, groupAct, live] {
    const eng::layout::Config &c = m_model->doc().config;
    {
      QSignalBlocker b(target);
      target->setCurrentIndex(std::clamp(c.target, 0, 2));
    }
    groupNameAct->setVisible(c.target == 1);
    groupAct->setVisible(c.target == 2);
    if (!groupName->hasFocus() && groupName->text() != c.groupName) {
      QSignalBlocker b(groupName);
      groupName->setText(c.groupName);
    }
    {
      QSignalBlocker b(group);
      const QStringList names = m_model->groupNames();
      const QList<int> ids = m_model->groupIds();
      bool same = group->count() == names.size();
      for (int i = 0; same && i < names.size(); ++i)
        same = group->itemText(i) == names[i] && group->itemData(i).toInt() == ids[i];
      if (!same) {
        group->clear();
        for (int i = 0; i < names.size(); ++i)
          group->addItem(names[i], ids[i]);
      }
      group->setCurrentIndex(group->findData(c.groupId));
    }
    {
      QSignalBlocker b(live);
      live->setChecked(m_model->live());
    }
  };
  connect(m_model, &LayoutModel::changed, this, syncOutput);
  connect(m_model, &LayoutModel::toolChanged, this, syncOutput);
  connect(m_model, &LayoutModel::layoutsChanged, this, syncOutput);
  syncOutput();
  auto syncUndo = [this, undo, redo] {
    undo->setEnabled(m_model->canUndo());
    redo->setEnabled(m_model->canRedo());
  };
  connect(m_model, &LayoutModel::changed, this, syncUndo);
  syncUndo();

  m_statusText = new QLabel(this);
  m_resultText = new QLabel(this);
  m_resultText->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  statusBar()->addWidget(m_statusText, 1);
  statusBar()->addPermanentWidget(m_resultText);
  auto syncStatus = [this] {
    m_statusText->setText(m_model->status());
    m_resultText->setText(m_model->lastResult());
  };
  connect(m_model, &LayoutModel::statusChanged, this, syncStatus);
  syncStatus();

  const QByteArray geo = config()->getRaw(QStringLiteral("LayoutDesigner/Geometry")).toByteArray();
  if (!geo.isEmpty())
    restoreGeometry(QByteArray::fromBase64(geo));
  const QByteArray split = config()->getRaw(QStringLiteral("LayoutDesigner/Splitter")).toByteArray();
  if (!split.isEmpty())
    m_split->restoreState(QByteArray::fromBase64(split));
}

void LayoutDesigner::fitView() {
  if (QQuickItem *root = m_view ? m_view->rootObject() : nullptr)
    if (auto *canvas = root->findChild<LayoutCanvas *>())
      canvas->fitAll();
}

void LayoutDesigner::saveWindowState() {
  config()->setRaw(QStringLiteral("LayoutDesigner/Geometry"), QString::fromLatin1(saveGeometry().toBase64()));
  config()->setRaw(QStringLiteral("LayoutDesigner/Splitter"), QString::fromLatin1(m_split->saveState().toBase64()));
}

void LayoutDesigner::closeEvent(QCloseEvent *e) {
  saveWindowState();
  QMainWindow::closeEvent(e);
}

LayoutDesigner *LayoutDesigner::open(vk::Engine3D *engine, Ctx *ctx, QWidget *owner, const int layoutId) {
  if (!s_window)
    s_window = new LayoutDesigner(engine, ctx, owner);
  if (layoutId > 0)
    s_window->m_model->loadLayoutById(layoutId);
  else
    s_window->m_model->placeAtCamera();
  if (s_window->isMinimized())
    s_window->showNormal();
  s_window->show();
  s_window->raise();
  s_window->activateWindow();
  return s_window;
}

void LayoutDesigner::closeIfOpen() {
  if (s_window)
    s_window->close();
}

} // namespace ui
