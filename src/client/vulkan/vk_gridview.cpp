#include <QGuiApplication>
#include "vk_gridview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

#include <QElapsedTimer>
#include <QIcon>
#include <QLineF>
#include <QPainter>
#include <QPixmap>
#include <QLoggingCategory>
#include <QQuickWindow>

#include <glm/gtc/matrix_transform.hpp>

#include <rhi/qrhi.h>

#include "client/lib/globals.h"
#include "client/vulkan/vk_engine.h"
#include "vk_common.h"
#include "vk_shared.h"
#include "vk_text.h"

namespace vk {

static QLoggingCategory gridLog("vk.grid", QtWarningMsg);

// VK_PERF=1 report for the 2D views; all grid views share one counter set.
struct GridPerf {
  qint64 renders = 0;
  qint64 wireFull = 0;
  qint64 wireFullVerts = 0;
  qint64 wirePatched = 0;
  qint64 wirePatchVerts = 0;
  qint64 whyScene = 0;
  qint64 whyStruct = 0;
  qint64 whyPersp = 0;
  qint64 whySlots = 0;
  qint64 whyDead = 0;
  qint64 pollCalls = 0;
  qint64 pollRepaints = 0;
  qint64 forceRepaints = 0;
  double msRender = 0.0;
  double msWire = 0.0;
  double msSync = 0.0;
  double msEnsure = 0.0;
  double msUniforms = 0.0;
  double msSelBox = 0.0;
  double msMarkers = 0.0;
  double msPasses = 0.0;
};

static GridPerf &gperf() {
  static GridPerf p;
  return p;
}

static QVector<GridView *> g_gridViews;

struct ScopedGridPerf {
  double &bucket;
  QElapsedTimer timer;
  explicit ScopedGridPerf(double &b) : bucket(b) {
    if (perfOn())
      timer.start();
  }
  ~ScopedGridPerf() {
    if (perfOn())
      bucket += double(timer.nsecsElapsed()) * 1e-6;
  }
};

static void gridPerfReport() {
  if (!perfOn())
    return;
  static QElapsedTimer clock;
  static qint64 windowMs = 0;
  if (!clock.isValid()) {
    clock.start();
    return;
  }
  const qint64 now = clock.elapsed();
  const qint64 win = now - windowMs;
  if (win < 1000)
    return;
  windowMs = now;
  const double sec = win * 1e-3;
  const auto div = [](const double v, const qint64 n) { return v / double(std::max<qint64>(1, n)); };
  GridPerf &p = gperf();
  qInfo(perfLog(),
        "grid renders=%5.1f/s render=%6.2fms | wire=%6.2fms full=%4.1f/s fullVerts=%8lld "
        "patched=%5.1f/s patchVerts=%7lld why(s=%2.1f st=%2.1f p=%2.1f sl=%2.1f d=%2.1f) | "
        "sync=%5.2f ensure=%5.2f unif=%5.2f selbox=%5.2f markers=%5.2f passes=%5.2f | "
        "poll=%4.1f/s repaints=%4.1f/s forced=%4.1f/s",
        double(p.renders) / sec, div(p.msRender, p.renders), div(p.msWire, p.renders),
        double(p.wireFull) / sec, p.wireFullVerts, double(p.wirePatched) / sec, p.wirePatchVerts,
        double(p.whyScene) / sec, double(p.whyStruct) / sec, double(p.whyPersp) / sec,
        double(p.whySlots) / sec, double(p.whyDead) / sec,
        div(p.msSync, p.renders),
        div(p.msEnsure, p.renders), div(p.msUniforms, p.renders), div(p.msSelBox, p.renders),
        div(p.msMarkers, p.renders), div(p.msPasses, p.renders), double(p.pollCalls) / sec,
        double(p.pollRepaints) / sec,
        double(p.forceRepaints) / sec);
  p = GridPerf();
}

namespace {

struct PlaneBasis {
  glm::vec3 right;
  glm::vec3 up;
  glm::vec3 depth;
};

PlaneBasis basisFor(const GridView::Perspective perspective) {
  switch (perspective) {
    case GridView::Perspective::Front:
      return {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, -1)};
    case GridView::Perspective::Side:
      return {glm::vec3(0, 0, -1), glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0)};
    case GridView::Perspective::Top:
    default:
      return {glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)};
  }
}

// world -> plane(a, b, depth): rows of the matrix are the basis vectors.
glm::mat4 basisMatrix(const PlaneBasis &b) {
  return glm::mat4(
      b.right.x, b.up.x, b.depth.x, 0.0f,
      b.right.y, b.up.y, b.depth.y, 0.0f,
      b.right.z, b.up.z, b.depth.z, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f);
}

// Undirected, quantised identity of a plane-space segment, so a mesh contributes each line once.
struct LineKey {
  std::int32_t ax, ay, bx, by;

  bool operator<(const LineKey &o) const {
    if (ax != o.ax)
      return ax < o.ax;
    if (ay != o.ay)
      return ay < o.ay;
    if (bx != o.bx)
      return bx < o.bx;
    return by < o.by;
  }
};

inline LineKey makeLineKey(const glm::vec2 &a, const glm::vec2 &b) {
  const auto q = [](const float v) { return static_cast<std::int32_t>(std::lround(v * 1024.0f)); };
  const LineKey k {q(a.x), q(a.y), q(b.x), q(b.y)};
  const LineKey r {k.bx, k.by, k.ax, k.ay};
  return r < k ? r : k;
}

// Thick segment as 6 verts: xy = centerline position, zw = unit perpendicular.
inline void appendSegment(QVector<float> &v, const glm::vec2 &p0, const glm::vec2 &p1,
                         const glm::vec2 &n) {
  v << p0.x << p0.y << n.x << n.y
    << p0.x << p0.y << -n.x << -n.y
    << p1.x << p1.y << -n.x << -n.y
    << p0.x << p0.y << n.x << n.y
    << p1.x << p1.y << -n.x << -n.y
    << p1.x << p1.y << n.x << n.y;
}

// The wire buffer is one triangle list: a triangle straddling a live and a zeroed range rasterises a sliver from
// the origin. Segments are 6 vertices, so slot windows must stay on the triangle grid too.
static constexpr quint32 kWireVertsPerTriangle = 3;

[[nodiscard]] constexpr quint32 alignWireVerts(const quint32 verts) {
  return (verts + kWireVertsPerTriangle - 1) / kWireVertsPerTriangle * kWireVertsPerTriangle;
}

// One object's perimeter edges on the view plane, deduped. Per object, so moving one mesh rewrites one slot.
inline void buildObjectWire(const eng::Mesh &mesh, const glm::mat4 &model, const PlaneBasis &basis,
                          QVector<float> &out) {
  std::set<LineKey> seen;

  std::vector<glm::vec3> poly;
  std::vector<glm::vec2> p;
  for (int face = 0; face < mesh.faceCount(); ++face) {
    if (!mesh.getFacePolygon(face, poly))
      continue;

    p.resize(poly.size());
    for (size_t k = 0; k < poly.size(); ++k) {
      const glm::vec3 w = glm::vec3(model * glm::vec4(poly[k], 1.0f));
      p[k] = glm::vec2(glm::dot(basis.right, w), glm::dot(basis.up, w));
    }

    for (size_t k = 0; k < p.size(); ++k) {
      const glm::vec2 &a = p[k];
      const glm::vec2 &b = p[(k + 1) % p.size()];
      const glm::vec2 d(b.x - a.x, b.y - a.y);
      const float len = glm::length(d);
      if (len < 1e-6f)
        continue;
      if (!seen.insert(makeLineKey(a, b)).second)
        continue;
      appendSegment(out, a, b, glm::vec2(-d.y / len, d.x / len));
    }
  }
}

// Everything a wire slot reads; a slot may be skipped only when this is unchanged.
inline quint64 wireKeyFor(const eng::GameObject &obj) {
  const std::shared_ptr<eng::Mesh> &mesh = obj.meshRef();
  auto mix = [](quint64 seed, const quint64 value) {
    return seed * 0x9E3779B97F4A7C15ull + value + 0x165667B19E3779F9ull;
  };
  quint64 k = mix(0x2545F4914F6CDD1Dull, obj.transformVersion());
  k = mix(k, obj.hidden() ? 1ull : 0ull);
  k = mix(k, static_cast<quint64>(reinterpret_cast<uintptr_t>(mesh.get())) >> 4);
  k = mix(k, mesh ? mesh->geometryVersion() : 0ull);
  return k;
}

// Perimeter of a rect, chopped into dashes of a fixed screen size.
inline void dashRect(QVector<float> &out, const glm::vec2 &c0, const glm::vec2 &c1,
                    const float dash, const float gap) {
  const glm::vec2 c[4] = {c0, {c1.x, c0.y}, c1, {c0.x, c1.y}};
  for (int k = 0; k < 4; ++k) {
    const glm::vec2 a = c[k];
    const glm::vec2 b = c[(k + 1) & 3];
    const glm::vec2 d = b - a;
    const float total = glm::length(d);
    if (total < 1e-6f)
      continue;
    const glm::vec2 dir = d / total;
    const glm::vec2 n(-dir.y, dir.x);
    for (float t = 0.0f; t < total - 1e-4f; t += dash + gap)
      appendSegment(out, a + dir * t, a + dir * std::min(t + dash, total), n);
  }
}

// Floats per vertex (must match the pipeline layouts): wire (vec2 pos, vec2 normal), selbox (vec4 plane, vec4 colour).
constexpr int kWireFloats = 4;
constexpr int kBoxFloats = 8;

// Solid quad in plane space: (vec4 plane, vec4 color) per vertex.
inline void pushBoxVertex(QVector<float> &v, const float x, const float y, const glm::vec4 &c) {
  v << x << y << 0.0f << 0.0f << c.x << c.y << c.z << c.w;
}

inline void appendBoxQuad(QVector<float> &v, const glm::dvec4 &r, const glm::vec4 &c) {
  const float x0 = float(r.x);
  const float y0 = float(r.y);
  const float x1 = float(r.z);
  const float y1 = float(r.w);
  pushBoxVertex(v, x0, y0, c);
  pushBoxVertex(v, x1, y0, c);
  pushBoxVertex(v, x1, y1, c);
  pushBoxVertex(v, x0, y0, c);
  pushBoxVertex(v, x1, y1, c);
  pushBoxVertex(v, x0, y1, c);
}

// Selection fill and handles share one buffer and pipeline. Green = selection, yellow = marquee being dragged.
const glm::vec4 kSelectionFill(0.15f, 0.75f, 0.55f, 0.2f);
const glm::vec4 kMarqueeFill(0.95f, 0.85f, 0.20f, 0.18f);
const glm::vec4 kHandleColor(1.0f, 1.0f, 1.0f, 1.0f);
// The cut line and the doomed half of the selection.
const glm::vec4 kCutLineColor(0.95f, 0.22f, 0.18f, 1.0f);
const glm::vec4 kCutFill(0.90f, 0.18f, 0.15f, 0.25f);

// Entity marker box size on screen, scaled a little with the zoom (markerScale).
constexpr double kMarkerPx = 24.0;
constexpr double kMarkerGrabPx = 5.0;

// Marker size factor at `pxPerUnit` px per Source unit: 0.2 and below -20 %, 0.4 -10 %, 0.6 +20 %, 1.0 and up
// +50 %, linear in between.
inline double markerScale(const double pxPerUnit) {
  static constexpr double kAt[4] = {0.2, 0.4, 0.6, 1.0};
  static constexpr double kScale[4] = {0.8, 0.9, 1.2, 1.5};
  if (pxPerUnit <= kAt[0])
    return kScale[0];
  for (int i = 1; i < 4; ++i)
    if (pxPerUnit <= kAt[i]) {
      const double t = (pxPerUnit - kAt[i - 1]) / (kAt[i] - kAt[i - 1]);
      return kScale[i - 1] + (kScale[i] - kScale[i - 1]) * t;
    }
  return kScale[3];
}
// Entity marker lines are 2.5 px wide (half below).
constexpr double kMarkerLineHalfPx = 1.25;
constexpr double kMarkerTextPx = 26.0;
// The facing arrow and, past its tip, the rotate knob.
constexpr double kMarkerArrowPx = 56.0;
constexpr double kMarkerArrowHeadPx = 16.0;
constexpr double kKnobRadiusPx = 6.0;
constexpr double kKnobGapPx = 14.0;

// A wireframe segment as one thin quad through the selbox pipeline (per-vertex colour).
inline void appendWireQuad(QVector<float> &v, const glm::vec2 &p0, const glm::vec2 &p1,
                          const float halfThick, const glm::vec4 &c) {
  const glm::vec2 d = p1 - p0;
  const float len = glm::length(d);
  if (len < 1e-6f)
    return;
  const glm::vec2 n = glm::vec2(-d.y, d.x) * (halfThick / len);
  const glm::vec2 a = p0 + n;
  const glm::vec2 b = p1 + n;
  const glm::vec2 e = p1 - n;
  const glm::vec2 f = p0 - n;
  pushBoxVertex(v, a.x, a.y, c);
  pushBoxVertex(v, b.x, b.y, c);
  pushBoxVertex(v, e.x, e.y, c);
  pushBoxVertex(v, a.x, a.y, c);
  pushBoxVertex(v, e.x, e.y, c);
  pushBoxVertex(v, f.x, f.y, c);
}

// Wireframe circle (a range sphere's projection in every view).
inline void appendCircleWire(QVector<float> &v, const glm::vec2 &center, const float radius,
                            const float halfThick, const glm::vec4 &c) {
  if (radius <= 0.0f)
    return;
  const int segments = 64;
  const float step = 2.0f * 3.14159265358979f / float(segments);
  glm::vec2 p = center + glm::vec2(radius, 0.0f);
  for (int k = 0; k < segments; ++k) {
    const float a = step * float(k + 1);
    const glm::vec2 q = center + glm::vec2(std::cos(a), std::sin(a)) * radius;
    appendWireQuad(v, p, q, halfThick, c);
    p = q;
  }
}

// A filled disc (triangle fan through the selbox pipeline).
inline void appendDisc(QVector<float> &v, const glm::vec2 &center, const float radius, const glm::vec4 &c) {
  const int segments = 20;
  const float step = 2.0f * 3.14159265358979f / float(segments);
  for (int k = 0; k < segments; ++k) {
    const glm::vec2 a = center + glm::vec2(std::cos(step * float(k)), std::sin(step * float(k))) * radius;
    const glm::vec2 b = center + glm::vec2(std::cos(step * float(k + 1)), std::sin(step * float(k + 1))) * radius;
    pushBoxVertex(v, center.x, center.y, c);
    pushBoxVertex(v, a.x, a.y, c);
    pushBoxVertex(v, b.x, b.y, c);
  }
}

// Zen mode leaves out markers outside its box (selected ones stay).
inline QVector<EntityMarker> markers2D() {
  QVector<EntityMarker> out = g::engine3D ? g::engine3D->entityMarkers() : QVector<EntityMarker>();
  if (g::engine3D && g::engine3D->zenMode())
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const EntityMarker &m) { return !m.selected && g::engine3D->zenHidesPoint(m.position); }),
              out.end());
  return out;
}

// The marker's rect in this view's plane (`zoom` = device px per world unit); shared by draw and hit test.
inline glm::dvec4 markerRectOnPlane(const EntityMarker &m, const GridView::Perspective perspective,
                                   const double zoom, const double scale = 1.0) {
  const PlaneBasis basis = basisFor(perspective);
  const double ca = glm::dot(basis.right, m.position);
  const double cb = glm::dot(basis.up, m.position);
  const double half = zoom > 1e-9 ? kMarkerPx * scale / (2.0 * zoom) : 0.0;
  return glm::dvec4(ca - half, cb - half, ca + half, cb + half);
}

// A marker's facing on the plane (none along the view's depth: the knob sits to the right); shared by draw
// and knob hit test.
struct MarkerArrow {
  glm::dvec2 centre {0.0};
  glm::dvec2 dir {1.0, 0.0};
  bool flat = true;
  [[nodiscard]] glm::dvec2 tip(const double zoom) const { return centre + dir * (kMarkerArrowPx / zoom); }
  [[nodiscard]] glm::dvec2 knob(const double zoom) const {
    return centre + dir * ((kMarkerArrowPx + kKnobGapPx) / zoom);
  }
};

inline MarkerArrow markerArrow(const EntityMarker &m, const GridView::Perspective perspective, const double zoom) {
  const glm::dvec4 r = markerRectOnPlane(m, perspective, zoom);
  const PlaneBasis basis = basisFor(perspective);
  MarkerArrow a;
  a.centre = {(r.x + r.z) * 0.5, (r.y + r.w) * 0.5};
  const glm::dvec2 d(glm::dot(basis.right, m.direction), glm::dot(basis.up, m.direction));
  if (glm::length(d) > 1e-3) {
    a.dir = glm::normalize(d);
    a.flat = false;
  }
  return a;
}

} // namespace

double GridView::gridStepSource() const {
  return kBaseGridStepSource * std::pow(2.0, static_cast<double>(m_gridLevelOffset));
}

int GridView::gridLevel() const {
  return m_gridLevelOffset + Engine3D::kGridLevelBase;
}

void GridView::applyGridLevel(const int offset) {
  if (offset == m_gridLevelOffset)
    return;
  m_gridLevelOffset = offset;
  emit gridStepSourceChanged(gridStepSource());
  update();
}

GridView::GridView(QQuickItem *parent) : QQuickRhiItem(parent) {
  g_gridViews.push_back(this);
  setSampleCount(1);
  m_pollTimer.setInterval(100);
  connect(&m_pollTimer, &QTimer::timeout, this, &GridView::pollScene);
  m_pollTimer.start();

  if (g::engine3D) {
    m_gridLevelOffset = g::engine3D->gridLevelOffset();
    connect(g::engine3D, &Engine3D::gridLevelChanged, this,
            [this](const int offset) { applyGridLevel(offset); });
    // The marquee and selection live on the engine: every view repaints on a change.
    connect(g::engine3D, &Engine3D::marqueeChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::selectionChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::sceneChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::entitiesChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::ioSimChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::cutModeChanged, this, [this] {
      // The QML MouseArea re-reads the cursor on this signal, so a parked mouse updates too.
      emit hoverHandleChanged();
      update();
    });
    connect(g::engine3D, &Engine3D::cutLineChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::mirrorChanged, this, [this] {
      emit hoverHandleChanged(); // placing shows a crosshair
      update();
    });
    connect(g::engine3D, &Engine3D::cutHoleChanged, this, [this] {
      emit hoverHandleChanged();
      update();
    });
    connect(g::engine3D, &Engine3D::cutoutLayoutChanged, this, [this] { update(); });
    // Zen mode: the box follows the 3D camera.
    connect(g::engine3D, &Engine3D::zenChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::zenModeChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::pathsChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::modOutputsChanged, this, [this] { update(); });
    connect(g::engine3D, &Engine3D::penChanged, this, [this] {
      emit hoverHandleChanged();
      update();
    });
    connect(g::engine3D, &Engine3D::gizmoModeChanged, this, [this] {
      emit hoverHandleChanged();
      update();
    });
  }
  // Queued: runs after the QML cursorShape binding, so the rotate cursor stays.
  connect(this, &GridView::hoverHandleChanged, this, &GridView::applyCursor, Qt::QueuedConnection);
}

GridView::~GridView() {
  g_gridViews.removeAll(this);
}

const QVector<GridView *> &GridView::instances() {
  return g_gridViews;
}

void GridView::restoreView(const double centerA, const double centerB, const double pxPerWorldUnit,
                          const bool showGrid) {
  m_center = glm::dvec2(centerA, centerB);
  applyZoom(pxPerWorldUnit);
  setShowGrid(showGrid);
  // Keeps the auto-fit from overriding the restored placement.
  m_fitted = true;
  m_userAdjusted = true;
  forceRepaints();
}

void GridView::setPerspective(const Perspective perspective) {
  if (m_perspective == perspective)
    return;
  m_perspective = perspective;
  m_fitted = false;
  emit perspectiveChanged(perspective);
  update();
}

void GridView::setShowGrid(const bool showGrid) {
  if (m_showGrid == showGrid)
    return;
  m_showGrid = showGrid;
  emit showGridChanged(showGrid);
  update();
}

void GridView::applyZoom(const double pxPerWorldUnit) {
  const double clamped = std::clamp(pxPerWorldUnit, 0.0005, 400.0);
  if (std::fabs(clamped - m_pxPerWorldUnit) < 1e-9)
    return;
  m_pxPerWorldUnit = clamped;
  emit pixelsPerSourceUnitChanged(pixelsPerSourceUnit());
  update();
}

void GridView::zoomAt(const qreal mouseX, const qreal mouseY, const qreal factor) {
  const double newZoom = std::clamp(m_pxPerWorldUnit * factor, 0.0005, 400.0);

  const double ox = mouseX - width() / 2.0;
  const double oy = height() / 2.0 - mouseY;

  m_center.x += ox / m_pxPerWorldUnit - ox / newZoom;
  m_center.y += oy / m_pxPerWorldUnit - oy / newZoom;

  m_userAdjusted = true;
  applyZoom(newZoom);
  update();
}

void GridView::panBy(const qreal dxPixels, const qreal dyPixels) {
  m_center.x -= dxPixels / m_pxPerWorldUnit;
  m_center.y += dyPixels / m_pxPerWorldUnit;
  m_userAdjusted = true;
  update();
}

void GridView::stepGridLevel(const int direction) {
  if (!g::engine3D)
    return;
  // The engine emits gridLevelChanged, which every view follows.
  g::engine3D->stepGridLevel(direction);
}

QSharedPointer<eng::Scene> GridView::resolveScene() const {
  return g::engine3D ? g::engine3D->scene() : QSharedPointer<eng::Scene>();
}

std::set<uint32_t> GridView::resolveSelection() const {
  return g::engine3D ? g::engine3D->drawSelection() : std::set<uint32_t>();
}

void GridView::fitToScene() {
  const auto scene = resolveScene();
  if (scene.isNull() || scene->gameObjects.empty() || width() <= 1 || height() <= 1)
    return;

  const PlaneBasis basis = basisFor(m_perspective);
  double minA = DBL_MAX, maxA = -DBL_MAX, minB = DBL_MAX, maxB = -DBL_MAX;
  for (const auto &obj: scene->gameObjects) {
    if (obj->hidden() || (g::engine3D && g::engine3D->zenHides(*obj)))
      continue;
    const auto [lo, hi] = obj->getWorldAABB();
    for (int i = 0; i < 8; ++i) {
      glm::vec3 corner = lo;
      if (i & 1)
        corner.x = hi.x;
      if (i & 2)
        corner.y = hi.y;
      if (i & 4)
        corner.z = hi.z;
      const double a = glm::dot(basis.right, corner);
      const double b = glm::dot(basis.up, corner);
      minA = std::min(minA, a);
      maxA = std::max(maxA, a);
      minB = std::min(minB, b);
      maxB = std::max(maxB, b);
    }
  }

  const double spanA = std::max(1e-3, maxA - minA);
  const double spanB = std::max(1e-3, maxB - minB);
  const double zoom = std::min((width() * 0.85) / spanA, (height() * 0.85) / spanB);

  m_center = glm::dvec2((minA + maxA) * 0.5, (minB + maxB) * 0.5);
  applyZoom(zoom);
  m_fitted = true;
  m_userAdjusted = false;
}

void GridView::resetView() {
  m_center = glm::dvec2(0.0, 0.0);
  applyZoom(kDefaultPxPerWorldUnit);
  m_fitted = false;
  m_userAdjusted = false;
}

void GridView::pollScene() {
  ++gperf().pollCalls;
  const auto scene = resolveScene();
  const auto selection = resolveSelection();

  const bool sceneChanged = scene.get() != m_scene.get()
      || (scene && scene->structureVersion() != (m_scene ? m_scene->structureVersion() : 0));
  const bool selectionChanged = selection != m_selection;

  // Global counters say whether anything moved or changed shape, without sweeping every object.
  // The physics simulation moves bodies without touching their transforms.
  const quint64 moved = eng::GameObject::globalTransformEpoch()
                        ^ (g::engine3D ? g::engine3D->physicsPoseEpoch() * 0x9E3779B97F4A7C15ull : 0ull);
  const quint64 shaped = eng::Mesh::globalGeometryEpoch();
  const quint64 vertexEpoch = g::engine3D ? g::engine3D->vertexEditEpoch() : 0;
  const bool transformsChanged = moved != m_pollXforms || shaped != m_pollGeom || vertexEpoch != m_pollVertexEpoch;
  m_pollXforms = moved;
  m_pollGeom = shaped;
  m_pollVertexEpoch = vertexEpoch;

  m_scene = scene;
  m_selection = selection;

  // Also here in case the view was built before the engine existed.
  applyGridLevel(g::engine3D ? g::engine3D->gridLevelOffset() : 0);

  if (sceneChanged || selectionChanged || transformsChanged) {
    ++gperf().pollRepaints;
    forceRepaints();
  }

  if (scene && !m_fitted && width() > 1 && height() > 1)
    fitToScene();
}

void GridView::forceRepaints() {
  if (perfOn())
    ++gperf().forceRepaints;
  update();
  QTimer::singleShot(0, this, [this] { update(); });
  QTimer::singleShot(120, this, [this] { update(); });
  QTimer::singleShot(400, this, [this] { update(); });
}

void GridView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
  QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    if (!m_userAdjusted)
      fitToScene();
    forceRepaints();
  }
}

void GridView::itemChange(const ItemChange change, const ItemChangeData &value) {
  if (change == ItemVisibleHasChanged && value.boolValue) {
    if (!m_fitted)
      fitToScene();
    forceRepaints();
  }
  QQuickRhiItem::itemChange(change, value);
}

glm::dvec2 GridView::planeAt(const qreal x, const qreal y) const {
  const double a = m_center.x + (x - width() * 0.5) / m_pxPerWorldUnit;
  const double b = m_center.y - (y - height() * 0.5) / m_pxPerWorldUnit;
  return glm::dvec2(a, b);
}

bool GridView::insideMarquee(const glm::dvec2 &p) const {
  if (!g::engine3D || !g::engine3D->hasMarquee())
    return false;
  const MarqueeBox seen = marqueeAsSeenFrom(g::engine3D->marquee(), static_cast<int>(m_perspective));
  const MarqueeBox &m = seen;
  const double a0 = std::min(m.a0, m.a1);
  const double a1 = std::max(m.a0, m.a1);
  const double b0 = std::min(m.b0, m.b1);
  const double b1 = std::max(m.b0, m.b1);
  return p.x >= a0 && p.x <= a1 && p.y >= b0 && p.y <= b1;
}

// Grabbing the selection means grabbing its box, not one mesh inside it.
bool GridView::insideSelected(const glm::dvec2 &p) const {
  glm::dvec4 box;
  if (!selectionBox(box))
    return false;
  return p.x >= box.x && p.x <= box.z && p.y >= box.y && p.y <= box.w;
}

bool GridView::selectionBox(glm::dvec4 &out) const {
  if (!g::engine3D || !m_scene)
    return false;

  // Brushes plus the output of selected modified objects.
  const std::set<uint32_t> &sel = g::engine3D->drawSelection();
  if (sel.empty())
    return false;

  const PlaneBasis basis = basisFor(m_perspective);
  glm::dvec3 mn(DBL_MAX);
  glm::dvec3 mx(-DBL_MAX);
  if (!g::engine3D->objectsBounds(std::vector<quint32>(sel.begin(), sel.end()), mn, mx))
    return false;

  double a0 = 0.0;
  double a1 = 0.0;
  double b0 = 0.0;
  double b1 = 0.0;
  projectOntoAxis(basis.right, mn, mx, a0, a1);
  projectOntoAxis(basis.up, mn, mx, b0, b1);
  out = glm::dvec4(a0, b0, a1, b1);
  return out.z > out.x && out.w > out.y;
}

bool GridView::resizeBox(glm::dvec4 &out) const {
  if (!g::engine3D)
    return false;
  // While rotating the box stays put, or the handles would move under the pointer.
  if (m_rotating) {
    out = m_rotateBox;
    return true;
  }

  // Edit mode's rubber band is drawn on its own.
  if (g::engine3D->hasMarquee() && !g::engine3D->editMode()) {
    const MarqueeBox m = marqueeAsSeenFrom(g::engine3D->marquee(), static_cast<int>(m_perspective));
    out = glm::dvec4(std::min(m.a0, m.a1), std::min(m.b0, m.b1), std::max(m.a0, m.a1),
                     std::max(m.b0, m.b1));
    return out.z > out.x && out.w > out.y;
  }

  return selectionBox(out);
}

double GridView::handleSizeWorld() const {
  const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
  const double zoom = std::max(1e-9, m_pxPerWorldUnit * std::max(1.0, dpr));
  return kHandleSizePx / zoom;
}

int GridView::hitTestHandle(const glm::dvec2 &p) const {
  glm::dvec4 box;
  if (!resizeBox(box))
    return -1;
  // An object placed by its anchors has their handles instead (anchorHandleHit).
  if (!g::engine3D->hasMarquee() && g::engine3D->anchorHandleMode())
    return -1;

  const double side = handleSizeWorld();
  const double slack = side * 0.75;
  for (int h = 0; h < SelectionHandleCount; ++h) {
    const glm::dvec4 r = handleRect(box, h, side);
    if (p.x >= r.x - slack && p.x <= r.z + slack && p.y >= r.y - slack && p.y <= r.w + slack)
      return h;
  }
  return -1;
}

quint32 GridView::anchorHandleHit(const glm::dvec2 &p) const {
  glm::dvec4 box;
  if (!g::engine3D || g::engine3D->hasMarquee() || !g::engine3D->anchorHandleMode() || !selectionBox(box))
    return 0;
  const double half = handleSizeWorld() * 0.5 * 1.75; // the handle plus some slack
  for (const auto &h: g::engine3D->anchorHandles2D(static_cast<int>(m_perspective), box))
    if (std::fabs(p.x - h.pos.x) <= half && std::fabs(p.y - h.pos.y) <= half)
      return h.anchor;
  return 0;
}

void GridView::sceneDepthRange(double &d0, double &d1) const {
  double lo = -1.0;
  double hi = 1.0;
  // The view cannot see this axis: take it from the brushes selected last (a wall's thickness, say), else the level.
  glm::dvec3 lastMin, lastMax;
  if (g::engine3D && g::engine3D->lastSelectionBox(lastMin, lastMax) && glm::all(glm::lessThan(lastMin, lastMax))) {
    const PlaneBasis basis = basisFor(m_perspective);
    const double a = glm::dot(glm::dvec3(basis.depth), lastMin), b = glm::dot(glm::dvec3(basis.depth), lastMax);
    // Exactly the brushes' extent: they need not sit on the grid.
    d0 = std::min(a, b);
    d1 = std::max(a, b);
    return;
  }
  if (m_scene && !m_scene->gameObjects.empty()) {
    const PlaneBasis basis = basisFor(m_perspective);
    lo = DBL_MAX;
    hi = -DBL_MAX;
    for (const auto &obj: m_scene->gameObjects) {
      if (obj->hidden() || (g::engine3D && g::engine3D->zenHides(*obj)))
        continue;
      const auto [a, b] = obj->getWorldAABB();
      for (int i = 0; i < 8; ++i) {
        glm::vec3 corner = a;
        if (i & 1)
          corner.x = b.x;
        if (i & 2)
          corner.y = b.y;
        if (i & 4)
          corner.z = b.z;
        const double d = glm::dot(basis.depth, corner);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
    }
  }

  // The level's extent: round the span outward to the level step.
  const double step = gridStepSource() / kUnitsPerWorldUnit;
  d0 = step > 0.0 ? std::floor(lo / step) * step : lo;
  d1 = step > 0.0 ? std::ceil(hi / step) * step : hi;
}

bool GridView::rotateHandles() const {
  return g::engine3D && g::engine3D->gizmoMode() == Engine3D::GizmoMode::Rotate && !g::engine3D->hasMarquee()
         && !g::engine3D->editMode() && !g::engine3D->materialMode()
         && g::engine3D->cutMode() == Engine3D::CutMode::Off;
}

namespace {

// The rotate cursor: white with a dark rim.
const QCursor &rotateCursor() {
  static const QCursor cursor = [] {
    const int size = 24;
    const QPixmap glyph = QIcon(QStringLiteral(":/icons_blender/gesture_rotate.svg")).pixmap(size - 4, size - 4);
    QImage white = glyph.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage dark = white;
    {
      QPainter p(&white);
      p.setCompositionMode(QPainter::CompositionMode_SourceIn);
      p.fillRect(white.rect(), Qt::white);
    }
    {
      QPainter p(&dark);
      p.setCompositionMode(QPainter::CompositionMode_SourceIn);
      p.fillRect(dark.rect(), QColor(0, 0, 0, 220));
    }
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (const QPoint d: {QPoint(1, 2), QPoint(3, 2), QPoint(2, 1), QPoint(2, 3)})
      p.drawImage(d, dark);
    p.drawImage(QPoint(2, 2), white);
    p.end();
    return QCursor(pm, size / 2, size / 2);
  }();
  return cursor;
}

} // namespace

void GridView::applyCursor() {
  if (!m_cursorTarget)
    return;
  if ((rotateHandles() && (m_hoverHandle >= 0 || m_rotating)) || m_knobHover || m_entityRotating)
    m_cursorTarget->setCursor(rotateCursor());
  else
    m_cursorTarget->setCursor(QCursor(Qt::CursorShape(hoverCursor())));
}

int GridView::hoverCursor() const {
  if (g::engine3D && g::engine3D->penActive())
    return int(Qt::CrossCursor);
  if (m_anchorHover || m_anchorDragging)
    return int(Qt::SizeAllCursor);
  if (g::engine3D && g::engine3D->mirrorActive() && g::engine3D->cutMode() == Engine3D::CutMode::Off) {
    if (g::engine3D->mirrorPlacing())
      return int(Qt::CrossCursor);
    if (m_mirrorLineHover)
      return std::fabs(viewBasisFor(int(m_perspective)).right[g::engine3D->mirrorLine().axis]) > 0.5
                 ? int(Qt::SizeHorCursor)
                 : int(Qt::SizeVerCursor);
  }
  if (g::engine3D && g::engine3D->cutMode() != Engine3D::CutMode::Off) {
    if (!g::engine3D->isCutout())
      return int(Qt::CrossCursor);
    // Pinned opening: move cursor over it, else an arrow (the next click re-pins it).
    if (!g::engine3D->cutoutPlaced())
      return int(Qt::CrossCursor);
    return m_cutHoleHover ? int(Qt::SizeAllCursor) : int(Qt::ArrowCursor);
  }
  // Handles are laid out in screen space (a right, b up), not the view's world axes.
  switch (m_hoverHandle) {
    case SelectionHandleTopLeft:
    case SelectionHandleBottomRight: return int(Qt::SizeFDiagCursor);
    case SelectionHandleTopRight:
    case SelectionHandleBottomLeft: return int(Qt::SizeBDiagCursor);
    case SelectionHandleTop:
    case SelectionHandleBottom: return int(Qt::SizeVerCursor);
    case SelectionHandleLeft:
    case SelectionHandleRight: return int(Qt::SizeHorCursor);
    default: return int(Qt::ArrowCursor);
  }
}

void GridView::hoverAt(const qreal x, const qreal y) {
  if (g::engine3D)
    g::engine3D->noteCutOriginPlane(static_cast<int>(m_perspective));

  // A cut-out tracks the pointer until it is placed.
  if (g::engine3D && g::engine3D->cutModeActive() && g::engine3D->isCutout()
      && !g::engine3D->cutoutPlaced()) {
    g::engine3D->trackCutHole(static_cast<int>(m_perspective), planeAt(x, y),
                              gridStepSource() / kUnitsPerWorldUnit);
    update();
  }

  if (g::engine3D && g::engine3D->penActive()) {
    g::engine3D->penHover(static_cast<int>(m_perspective), planeAt(x, y), gridStepSource() / kUnitsPerWorldUnit);
    update();
  }
  const bool overLine = g::engine3D && g::engine3D->mirrorActive()
      && g::engine3D->mirrorLineNear(static_cast<int>(m_perspective), planeAt(x, y),
                                     6.0 / std::max(1e-9, m_pxPerWorldUnit));
  if (overLine != m_mirrorLineHover) {
    m_mirrorLineHover = overLine;
    emit hoverHandleChanged();
  }
  {
    quint32 id = 0;
    glm::dvec2 c, d;
    const bool overKnob = entityKnobHit(planeAt(x, y), id, c, d);
    if (overKnob != m_knobHover) {
      m_knobHover = overKnob;
      emit hoverHandleChanged();
    }
  }
  const bool overAnchor = anchorHandleHit(planeAt(x, y)) != 0;
  if (overAnchor != m_anchorHover) {
    m_anchorHover = overAnchor;
    emit hoverHandleChanged();
  }
  const int h = hitTestHandle(planeAt(x, y));
  const bool overHole = g::engine3D && g::engine3D->cutoutPlaced()
      && g::engine3D->cutHoleContains(static_cast<int>(m_perspective), planeAt(x, y));
  if (overHole != m_cutHoleHover) {
    m_cutHoleHover = overHole;
    emit hoverHandleChanged();
  }
  if (h == m_hoverHandle)
    return;
  m_hoverHandle = h;
  emit hoverHandleChanged();
}

void GridView::doubleClickAt(const qreal x, const qreal y) {
  Q_UNUSED(x);
  Q_UNUSED(y);
  if (g::engine3D && g::engine3D->penActive()) {
    g::engine3D->penFinish();
    update();
  }
}

void GridView::endHover() {
  if (g::engine3D)
    g::engine3D->penLeave();
  if (m_knobHover || m_anchorHover) {
    m_knobHover = false;
    m_anchorHover = false;
    emit hoverHandleChanged();
  }
  if (m_hoverHandle == -1)
    return;
  m_hoverHandle = -1;
  emit hoverHandleChanged();
}

void GridView::beginLeftDrag(const qreal x, const qreal y, const bool additive) {
  if (!g::engine3D)
    return;

  const glm::dvec2 p = planeAt(x, y);
  // The level step is the snap increment (the major grid lines).
  const double step = gridStepSource() / kUnitsPerWorldUnit;
  const int plane = static_cast<int>(m_perspective);

  // I/O target picker: a marker answers, anything else cancels.
  if (g::engine3D->targetPickActive()) {
    if (entityMarkerHit(p))
      g::engine3D->deliverTargetPick(m_lastMarkerHit);
    else
      g::engine3D->cancelTargetPick();
    update();
    return;
  }

  // Bevel mode: a press on an end-on edge toggles it; nothing is dragged.
  if (g::engine3D->bevelMode() && g::engine3D->pickBevelEdge2D(plane, p, m_pxPerWorldUnit)) {
    update();
    return;
  }

  // While the simulation runs only selection works.
  if (g::engine3D->physicsSimState() == Engine3D::PhysicsSimState::Running) {
    if (entityMarkerHit(p)) {
      g::engine3D->selectMarker(m_lastMarkerHit, additive);
    } else {
      double d0 = 0.0;
      double d1 = 0.0;
      sceneDepthRange(d0, d1);
      g::engine3D->beginDrag(Engine3D::DragTarget::CreateMarquee, plane, p, step, d0, d1);
    }
    update();
    return;
  }

  // Cut mode owns the left button. A cut-out takes the press as its placement, unless pinned and hit (a nudge).
  if (g::engine3D->cutMode() != Engine3D::CutMode::Off) {
    if (g::engine3D->isCutout()) {
      if (g::engine3D->cutoutPlaced() && g::engine3D->cutHoleContains(plane, p))
        g::engine3D->beginCutHoleDrag(plane, p);
      else {
        g::engine3D->trackCutHole(plane, p, step);
        g::engine3D->placeCutHole();
      }
      update();
      return;
    }
    g::engine3D->beginCutDrag(plane, p, step);
    update();
    return;
  }

  if (g::engine3D->penActive()) {
    g::engine3D->penPress(plane, p, step, 8.0 / std::max(1e-9, m_pxPerWorldUnit));
    update();
    return;
  }

  // Mirror mode: without a line the drag draws it; a press on the line moves it.
  if (g::engine3D->beginMirrorDrag(plane, p, step, 6.0 / std::max(1e-9, m_pxPerWorldUnit))) {
    update();
    return;
  }

  // Paths (Object mode): the edited path's points and handles, then any path's curve.
  if (!g::engine3D->materialMode() && !g::engine3D->editMode()) {
    const double tol = 7.0 / std::max(1e-9, m_pxPerWorldUnit);
    Engine3D::PathHandleHit hit;
    if (g::engine3D->pickPathHandle2D(plane, p, tol, hit)) {
      g::engine3D->beginPathDrag(plane, p, step, hit, additive);
      update();
      return;
    }
    quint32 pathId = 0;
    double s = 0.0;
    // An entity marker on the curve wins: a Connect path's ends are its anchors.
    if (!entityMarkerHit(p) && g::engine3D->pickPathCurve2D(plane, p, tol * 0.8, pathId, s)) {
      if (QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier)
          && pathId == g::engine3D->pointsPath()) {
        g::engine3D->insertPathPointAt(pathId, s);
        update();
        return;
      }
      if (additive || !g::engine3D->selectedPaths().count(pathId))
        g::engine3D->selectPath(pathId, additive);
      if (g::engine3D->selectedPaths().count(pathId)) {
        Engine3D::PathHandleHit whole;
        whole.path = pathId; // the whole path moves
        g::engine3D->beginPathDrag(plane, p, step, whole, false);
      }
      update();
      return;
    }
  }

  // Vertex tool: the scale pivot, then handles; a press on nothing starts a handle rubber band.
  if (g::engine3D->editMode()) {
    const double tol = 9.0 / std::max(1e-9, m_pxPerWorldUnit);
    if (g::engine3D->vertexScaleActive()) {
      m_pivotDrag = true;
      g::engine3D->moveVertexPivotPlane(plane, p, step);
      update();
      return;
    }
    const bool ctrl = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier);
    Engine3D::EditHandle hit;
    // Ctrl on an edge / face handle pulls a new corner out of it.
    if (ctrl) {
      std::vector<Engine3D::VertexHandle> keep = g::engine3D->selectedVertices();
      if (g::engine3D->pickEditHandle2D(plane, p, tol, false, &hit)
          && (hit.kind == Engine3D::EditHandleKind::Edge || hit.kind == Engine3D::EditHandleKind::Face)) {
        g::engine3D->beginVertexPlaneDrag(plane, p, step, hit.pos, std::make_pair(hit.brush, hit.pos));
        update();
        return;
      }
      g::engine3D->m_selectedVerts = keep;
    }
    if (g::engine3D->pickEditHandle2D(plane, p, tol, additive || ctrl, &hit)) {
      if (g::engine3D->handleSelected(hit))
        g::engine3D->beginVertexPlaneDrag(plane, p, step, hit.pos);
      update();
      return;
    }
    double d0 = 0.0;
    double d1 = 0.0;
    sceneDepthRange(d0, d1);
    m_vertexBox = true;
    // Not grid-snapped (a corner on a grid line would land on its edge); counts once a few pixels wide.
    g::engine3D->beginDrag(Engine3D::DragTarget::CreateMarquee, plane, p, 3.0 / std::max(1e-9, m_pxPerWorldUnit), d0,
                           d1);
    update();
    return;
  }

  // The rotate knob of a selected oriented entity.
  {
    quint32 id = 0;
    glm::dvec2 centre, dir;
    if (entityKnobHit(p, id, centre, dir) && g::engine3D->beginEntityRotate2D(plane, id, centre, dir)) {
      m_entityRotating = true;
      applyCursor();
      update();
      return;
    }
  }

  // A layer seam of the live fill is grabbed ahead of the marquee and the meshes.
  if (g::engine3D->materialMode() && g::engine3D->fillHasLayers()) {
    const double tol = step * 2.0;
    if (g::engine3D->fillBoundHit(plane, p, tol) >= 0) {
      g::engine3D->beginFillBoundDrag(plane, p, step);
      update();
      return;
    }
  }

  if (const quint32 anchor = anchorHandleHit(p)) {
    if (g::engine3D->beginAnchorDrag(plane, anchor, p, step))
      m_anchorDragging = true;
    update();
    return;
  }

  // A handle grab beats everything else.
  const int handle = hitTestHandle(p);
  if (handle >= 0 && rotateHandles()) {
    glm::dvec4 box;
    if (resizeBox(box) && g::engine3D->beginRotate2D(plane, p)) {
      m_rotateBox = box;
      m_rotating = true;
      applyCursor();
    }
    update();
    return;
  }
  if (handle >= 0) {
    g::engine3D->beginResize(g::engine3D->hasMarquee(), plane, handle, p, step);
    update();
    return;
  }

  // An entity marker beats the marquee and meshes. A click on a selected entity keeps the rest of the selection;
  // only a press that ends with the entity selected starts a move.
  if (entityMarkerHit(p)) {
    const quint32 id = m_lastMarkerHit;
    // Shift on a selected entity: a drag duplicates, released in place it deselects the entity.
    if (additive && g::engine3D->markerSelected(id)) {
      g::engine3D->beginDrag(Engine3D::DragTarget::MoveSelection, plane, p, step);
      m_markerToggle = id;
      m_pressPx = QPointF(x, y);
      update();
      return;
    }
    if (additive || !g::engine3D->markerSelected(id))
      g::engine3D->selectMarker(id, additive);
    if (g::engine3D->markerSelected(id))
      g::engine3D->beginDrag(Engine3D::DragTarget::MoveSelection, plane, p, step);
    update();
    return;
  }

  // A press inside the marquee slides it; otherwise one inside the selection moves it.
  if (insideMarquee(p))
    g::engine3D->beginDrag(Engine3D::DragTarget::MoveMarquee, plane, p, step);
  else if (insideSelected(p)) {
    g::engine3D->beginDrag(Engine3D::DragTarget::MoveSelection, plane, p, step);
    // Released in place, this click toggles rotate mode.
    m_clickToggle = !additive && !g::engine3D->materialMode();
    m_pressPx = QPointF(x, y);
  } else {
    double d0 = 0.0;
    double d1 = 0.0;
    sceneDepthRange(d0, d1);
    g::engine3D->beginDrag(Engine3D::DragTarget::CreateMarquee, plane, p, step, d0, d1);
  }

  update();
}

void GridView::updateLeftDrag(const qreal x, const qreal y, const bool duplicate) {
  if (!g::engine3D)
    return;
  if (g::engine3D->penDragging()) {
    g::engine3D->penDrag(planeAt(x, y));
    update();
    return;
  }
  if (g::engine3D->pathDragging()) {
    g::engine3D->updatePathDrag(planeAt(x, y));
    update();
    return;
  }
  if (g::engine3D->mirrorDragging()) {
    g::engine3D->updateMirrorDrag(planeAt(x, y));
    update();
    return;
  }
  if (g::engine3D->fillBoundDragging()) {
    g::engine3D->updateFillBoundDrag(planeAt(x, y));
    update();
    return;
  }
  if (m_pivotDrag) {
    g::engine3D->moveVertexPivotPlane(static_cast<int>(m_perspective), planeAt(x, y),
                                      gridStepSource() / kUnitsPerWorldUnit);
    update();
    return;
  }
  if (g::engine3D->vertexPlaneDragging()) {
    g::engine3D->updateVertexPlaneDrag(planeAt(x, y));
    update();
    return;
  }
  if (g::engine3D->cutMode() != Engine3D::CutMode::Off) {
    if (g::engine3D->isCutout()) {
      g::engine3D->dragCutHole(planeAt(x, y));
      update();
      return;
    }
    g::engine3D->updateCutDrag(planeAt(x, y));
    update();
    return;
  }
  if (m_rotating) {
    g::engine3D->updateRotate2D(planeAt(x, y));
    update();
    return;
  }
  if (m_entityRotating) {
    g::engine3D->updateEntityRotate2D(planeAt(x, y));
    update();
    return;
  }
  if (m_anchorDragging) {
    g::engine3D->updateAnchorDrag(planeAt(x, y));
    update();
    return;
  }
  // A few pixels of slack before a press on the selection becomes a move, so a click can toggle rotate mode.
  if (m_clickToggle) {
    if (QLineF(m_pressPx, QPointF(x, y)).length() < 3.0)
      return;
    m_clickToggle = false;
  }
  if (m_markerToggle) {
    if (QLineF(m_pressPx, QPointF(x, y)).length() < 3.0)
      return;
    m_markerToggle = 0;
  }
  g::engine3D->updateDrag(planeAt(x, y), duplicate);
  update();
}

// Where markers overlap, the one whose centre is nearest wins.
bool GridView::entityMarkerHit(const glm::dvec2 &p) const {
  if (!g::engine3D)
    return false;

  const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
  const double zoom = m_pxPerWorldUnit * std::max(1.0, dpr);
  const double scale = markerScale(pixelsPerSourceUnit());
  const double slack = kMarkerGrabPx * std::max(1.0, dpr) / zoom;
  double best = DBL_MAX;
  for (const EntityMarker &m: markers2D()) {
    const glm::dvec4 r = markerRectOnPlane(m, m_perspective, zoom, scale);
    if (p.x < r.x - slack || p.x > r.z + slack || p.y < r.y - slack || p.y > r.w + slack)
      continue;
    const double d = glm::length(p - glm::dvec2((r.x + r.z) * 0.5, (r.y + r.w) * 0.5));
    if (d < best) {
      best = d;
      m_lastMarkerHit = m.id;
    }
  }
  return best < DBL_MAX;
}

bool GridView::entityKnobs() const {
  return g::engine3D && !g::engine3D->materialMode() && !g::engine3D->editMode()
         && g::engine3D->cutMode() == Engine3D::CutMode::Off && !g::engine3D->penActive()
         && !g::engine3D->targetPickActive();
}

bool GridView::entityKnobHit(const glm::dvec2 &p, quint32 &id, glm::dvec2 &centre, glm::dvec2 &dir) const {
  if (!entityKnobs())
    return false;
  const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
  const double zoom = m_pxPerWorldUnit * std::max(1.0, dpr);
  const double grab = (kKnobRadiusPx + 4.0) / zoom;
  for (const EntityMarker &m: g::engine3D->entityMarkers()) {
    if (!m.selected || !m.rotatable)
      continue;
    const MarkerArrow a = markerArrow(m, m_perspective, zoom);
    if (glm::length(p - a.knob(zoom)) <= grab) {
      id = m.id;
      centre = a.centre;
      dir = a.dir;
      return true;
    }
  }
  return false;
}

// Smallest projected footprint wins where meshes overlap.
void GridView::pickAt(const glm::dvec2 &p, const bool additive) {
  // Material mode selects faces: a 2D click must not make an object selection.
  if (!m_scene || !g::engine3D || g::engine3D->materialMode())
    return;

  const ViewBasis bs = viewBasisFor(static_cast<int>(m_perspective));
  int best = -1;
  double bestArea = DBL_MAX;
  const auto &objects = m_scene->gameObjects;

  for (size_t i = 0; i < objects.size(); ++i) {
    if (objects[i]->hidden() || objects[i]->locked() || g::engine3D->zenHides(*objects[i]))
      continue;
    const auto [lo, hi] = objects[i]->getWorldAABB();
    const glm::dvec3 lo3(lo);
    const glm::dvec3 hi3(hi);
    double a0 = 0.0;
    double a1 = 0.0;
    double b0 = 0.0;
    double b1 = 0.0;
    projectOntoAxis(bs.right, lo3, hi3, a0, a1);
    projectOntoAxis(bs.up, lo3, hi3, b0, b1);
    if (p.x < a0 || p.x > a1 || p.y < b0 || p.y > b1)
      continue;
    const double area = (a1 - a0) * (b1 - b0);
    if (area < bestArea) {
      bestArea = area;
      best = static_cast<int>(i);
    }
  }

  if (best >= 0)
    g::engine3D->selectObject(objects[size_t(best)]->stableId(), additive);
  else if (!additive)
    g::engine3D->clearSelection();
}

void GridView::endLeftDrag(const qreal x, const qreal y, const bool additive) {
  if (g::engine3D && g::engine3D->penActive()) {
    g::engine3D->penRelease();
    update();
    return;
  }
  if (g::engine3D && g::engine3D->pathDragging()) {
    g::engine3D->endPathDrag();
    update();
    return;
  }
  if (m_rotating) {
    m_rotating = false;
    if (g::engine3D)
      g::engine3D->endRotate2D();
    applyCursor();
    update();
    return;
  }
  if (m_entityRotating) {
    m_entityRotating = false;
    if (g::engine3D)
      g::engine3D->endEntityRotate2D();
    applyCursor();
    update();
    return;
  }
  if (m_anchorDragging) {
    m_anchorDragging = false;
    if (g::engine3D)
      g::engine3D->endAnchorDrag();
    emit hoverHandleChanged();
    update();
    return;
  }
  if (g::engine3D && g::engine3D->mirrorDragging()) {
    g::engine3D->endMirrorDrag();
    update();
    return;
  }
  if (g::engine3D && m_pivotDrag) {
    m_pivotDrag = false;
    update();
    return;
  }
  if (g::engine3D && g::engine3D->vertexPlaneDragging()) {
    g::engine3D->endVertexPlaneDrag();
    update();
    return;
  }
  if (g::engine3D && m_vertexBox) {
    m_vertexBox = false;
    const bool hadBox = g::engine3D->dragTarget() == Engine3D::DragTarget::CreateMarquee;
    g::engine3D->endDrag();
    if (hadBox && g::engine3D->hasMarquee()) {
      const MarqueeBox m = marqueeAsSeenFrom(g::engine3D->marquee(), static_cast<int>(m_perspective));
      g::engine3D->selectHandlesInBox(static_cast<int>(m_perspective), {m.a0, m.b0}, {m.a1, m.b1}, additive);
      g::engine3D->clearMarquee();
    } else if (!g::engine3D->hasMarquee() && !additive) {
      // A click on nothing: no vertices; Edit mode keeps its brushes.
      g::engine3D->clearVertexSelection();
    }
    update();
    return;
  }
  if (g::engine3D) {
    if (g::engine3D->fillBoundDragging()) {
      g::engine3D->endFillBoundDrag();
      update();
      return;
    }
    if (g::engine3D->cutMode() != Engine3D::CutMode::Off) {
      if (g::engine3D->isCutout())
        g::engine3D->endCutHoleDrag();
      else
        g::engine3D->endCutDrag();
      update();
      return;
    }
    const bool wasCreating =
        g::engine3D->dragTarget() == Engine3D::DragTarget::CreateMarquee;
    const bool toggle = m_clickToggle && g::engine3D->dragTarget() == Engine3D::DragTarget::MoveSelection;
    m_clickToggle = false;
    const quint32 markerToggle = m_markerToggle;
    m_markerToggle = 0;
    g::engine3D->endDrag();
    if (toggle)
      g::engine3D->toggleRotateMode();
    if (markerToggle)
      g::engine3D->selectMarker(markerToggle, true);
    // Never dragged a whole grid step: a click picks instead of leaving a zero-size marquee.
    if (wasCreating && !g::engine3D->hasMarquee())
      pickAt(planeAt(x, y), additive);
  }
  update();
}

class GridViewRenderer final : public QQuickRhiItemRenderer {
public:
  ~GridViewRenderer() override {
    releaseResources();
  }

  void initialize(QRhiCommandBuffer *) override {}

  QRhi *m_rhiSeen = nullptr;

  void synchronize(QQuickRhiItem *item) override {
    const ScopedGridPerf scoped(gperf().msSync);
    auto *view = static_cast<GridView *>(item);
    m_item = view;
    m_perspective = view->perspective();
    m_showGrid = view->showGrid();
    m_center = view->m_center;
    m_pxPerWorldUnit = view->m_pxPerWorldUnit;
    m_gridStepSource = view->gridStepSource();
    m_viewWidth = view->width();
    m_viewHeight = view->height();
    m_devicePixelRatio = view->window() ? view->window()->devicePixelRatio() : 1.0;
    m_scene = view->m_scene;
    m_selBoxValid = view->resizeBox(m_selBox);
    m_rotateHandles = view->rotateHandles();
    m_rotating = view->m_rotating && g::engine3D && g::engine3D->rotating2D();
    m_rotateAngle = m_rotating ? g::engine3D->rotate2DAngle() : 0.0;
    m_entityKnobs = view->entityKnobs();
    m_anchorMode = g::engine3D && !g::engine3D->hasMarquee() && g::engine3D->anchorHandleMode();
    m_anchorHandles.clear();
    if (m_anchorMode && m_selBoxValid)
      for (const auto &h: g::engine3D->anchorHandles2D(static_cast<int>(m_perspective), m_selBox))
        m_anchorHandles.push_back(h.pos);
    m_entRotId = view->m_entityRotating && g::engine3D && g::engine3D->entityRotating2D()
                     ? g::engine3D->entityRotate2DId()
                     : 0;
    m_entRotAngle = m_entRotId ? g::engine3D->entityRotate2DAngle() : 0.0;
    m_selBoxIsMarquee = g::engine3D && g::engine3D->hasMarquee() && !g::engine3D->editMode();
    m_vertexBand.reset();
    if (g::engine3D && g::engine3D->editMode() && g::engine3D->hasMarquee()) {
      const MarqueeBox m = marqueeAsSeenFrom(g::engine3D->marquee(), static_cast<int>(m_perspective));
      m_vertexBand = glm::dvec4(std::min(m.a0, m.a1), std::min(m.b0, m.b1), std::max(m.a0, m.a1), std::max(m.b0, m.b1));
    }
    m_markers = markers2D();
    m_ioLinks = g::engine3D ? g::engine3D->ioLinks() : std::vector<IOLink>();
    m_ioPulses = g::engine3D ? g::engine3D->ioPulses() : std::vector<IOPulse>();
    m_editHandles = g::engine3D && g::engine3D->editMode() ? g::engine3D->editHandleDraws()
                                                           : std::vector<Engine3D::EditHandleDraw>();
    m_cutMode = g::engine3D ? g::engine3D->cutMode() : Engine3D::CutMode::Off;
    m_cutLine = g::engine3D ? g::engine3D->cutLine() : Engine3D::CutLine();
    m_mirrorPlanes.clear();
    m_mirrorLinked.clear();
    m_mirrorTwins.clear();
    m_mirrorTwinFaces.clear();
    if (g::engine3D) {
      m_mirrorPlanes = g::engine3D->mirrorShownPlanes();
      m_mirrorTwins = g::engine3D->mirrorTwinBoxes();
      m_mirrorTwinFaces = g::engine3D->mirrorTwinFacePolygons();
      // Mirror mode on: the edges of every linked brush.
      if (g::engine3D->mirrorActive() && g::engine3D->scene()) {
        const ViewBasis vb = viewBasisFor(static_cast<int>(m_perspective));
        const glm::vec3 right(vb.right), up(vb.up);
        std::set<LineKey> seen;
        std::vector<glm::vec3> poly;
        for (const auto &o: g::engine3D->scene()->gameObjects) {
          if (!o || !o->mirrorLink() || o->hidden())
            continue;
          const auto mesh = o->getMesh();
          if (!mesh)
            continue;
          const glm::mat4 model = o->getModelMatrix();
          for (int f = 0; f < mesh->faceCount(); ++f) {
            if (!mesh->getFacePolygon(f, poly) || poly.size() < 2)
              continue;
            for (size_t k = 0; k < poly.size(); ++k) {
              const glm::vec3 wa = glm::vec3(model * glm::vec4(poly[k], 1.0f));
              const glm::vec3 wb = glm::vec3(model * glm::vec4(poly[(k + 1) % poly.size()], 1.0f));
              const glm::vec2 a(glm::dot(right, wa), glm::dot(up, wa)), b(glm::dot(right, wb), glm::dot(up, wb));
              if (glm::length(b - a) < 1e-6f || !seen.insert(makeLineKey(a, b)).second)
                continue;
              m_mirrorLinked.emplace_back(a.x, a.y, b.x, b.y);
            }
          }
        }
      }
    }
    m_mirrorOn = !m_mirrorPlanes.empty() || !m_mirrorLinked.empty() || !m_mirrorTwins.empty()
                 || !m_mirrorTwinFaces.empty();
    m_physicsEdges.clear();
    if (g::engine3D && g::engine3D->scene()) {
      const ViewBasis vb = viewBasisFor(static_cast<int>(m_perspective));
      const glm::vec3 right(vb.right), up(vb.up);
      std::set<LineKey> seen;
      std::vector<glm::vec3> poly;
      for (const quint32 id: g::engine3D->physicsBodyBrushes()) {
        const auto o = g::engine3D->scene()->findById(id);
        if (!o || o->hidden())
          continue;
        const auto mesh = o->getMesh();
        if (!mesh)
          continue;
        const glm::mat4 *pose = g::engine3D->physicsPose(id);
        const glm::mat4 model = pose ? *pose * o->getModelMatrix() : o->getModelMatrix();
        for (int f = 0; f < mesh->faceCount(); ++f) {
          if (!mesh->getFacePolygon(f, poly) || poly.size() < 2)
            continue;
          for (size_t k = 0; k < poly.size(); ++k) {
            const glm::vec3 wa = glm::vec3(model * glm::vec4(poly[k], 1.0f));
            const glm::vec3 wb = glm::vec3(model * glm::vec4(poly[(k + 1) % poly.size()], 1.0f));
            const glm::vec2 a(glm::dot(right, wa), glm::dot(up, wa)), b(glm::dot(right, wb), glm::dot(up, wb));
            if (glm::length(b - a) < 1e-6f || !seen.insert(makeLineKey(a, b)).second)
              continue;
            m_physicsEdges.emplace_back(a.x, a.y, b.x, b.y);
          }
        }
      }
    }
    m_paths = g::engine3D ? g::engine3D->pathDraws() : std::vector<Engine3D::PathDraw>();
    m_penPoly.clear();
    m_penPoints.clear();
    if (g::engine3D)
      (void)g::engine3D->penPreview(m_penPoly, m_penPoints);
    m_cutHoles = g::engine3D ? g::engine3D->cutHoles() : std::vector<Engine3D::CutHole>();
    m_cutIsCutout = g::engine3D && g::engine3D->isCutout();
    m_cutHoleBoxes.clear();
    if (g::engine3D && m_cutIsCutout) {
      for (const auto &hole: m_cutHoles) {
        glm::dvec3 mn {};
        glm::dvec3 mx {};
        Engine3D::cutHoleWorld(hole, mn, mx);
        m_cutHoleBoxes.emplace_back(mn, mx);
      }
    }
    m_cutSelValid = false;
    if (m_cutMode != Engine3D::CutMode::Off && m_cutLine.valid)
      m_cutSelValid = view->selectionBox(m_cutSelBox);

    m_layerBounds.clear();
    if (g::engine3D && g::engine3D->fillHasLayers()) {
      for (int i = 0; i < g::engine3D->fillBoundCount(); ++i)
        m_layerBounds.push_back(g::engine3D->fillBound(i));
    }
  }

  void render(QRhiCommandBuffer *cb) override {
    QElapsedTimer renderTimer;
    if (perfOn())
      renderTimer.start();
    ++gperf().renders;
    Shared::makeCurrent(rhi());
    if (m_rhiSeen && m_rhiSeen != rhi())
      releaseResources();
    m_rhiSeen = rhi();
    Shared::instance().beginFrame();

    QRhiRenderTarget *target = renderTarget();
    if (!target)
      return;

    const QSize pixelSize = target->pixelSize();
    if (pixelSize.isEmpty())
      return;

    QRhiResourceUpdateBatch *rub = rhi()->nextResourceUpdateBatch();
    Shared::instance().setUploader(rub);

    bool ok = false;
    {
      const ScopedGridPerf s(gperf().msEnsure);
      ok = ensureResources(target, rub, pixelSize);
    }
    if (!ok) {
      cb->beginPass(target, QColor::fromRgbF(0.078f, 0.094f, 0.145f, 1.0f), {1.0f, 0}, rub);
      cb->endPass();
      if (m_item)
        m_item->update();
      gridPerfReport();
      return;
    }

    {
      const ScopedGridPerf s(gperf().msUniforms);
      uploadUniforms(rub, pixelSize);
    }
    {
      const ScopedGridPerf s(gperf().msWire);
      rebuildWire(rub);
    }
    {
      const ScopedGridPerf s(gperf().msSelBox);
      rebuildSelBox(rub);
    }
    rebuildCutLine(rub);
    rebuildZenBox(rub);
    rebuildFillLines(rub);
    {
      const ScopedGridPerf s(gperf().msMarkers);
      rebuildEntityMarkers(rub);
    }
    m_text.prepare(rhi(), rub, m_ubo, target->renderPassDescriptor(), 1);

    {
    const ScopedGridPerf s(gperf().msPasses);
    bool batchUsed = false;

    if (m_wireCount && m_densityTarget && m_densityPipeline) {
      cb->beginPass(m_densityTarget, QColor(0, 0, 0, 0), {}, batchUsed ? nullptr : rub);
      batchUsed = true;
      cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));
      cb->setGraphicsPipeline(m_densityPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_wireBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_wireCount);
      cb->endPass();
    }

    cb->beginPass(target, QColor::fromRgbF(0.078f, 0.094f, 0.145f, 1.0f), {1.0f, 0},
                 batchUsed ? nullptr : rub);
    cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));

    if (m_showGrid && m_gridPipeline) {
      cb->setGraphicsPipeline(m_gridPipeline.get());
      const QRhiCommandBuffer::VertexInput q(m_quadBuffer, 0);
      cb->setVertexInput(0, 1, &q, m_quadIndexBuffer, 0, QRhiCommandBuffer::IndexUInt32);
      cb->setShaderResources(m_gridSrb);
      cb->drawIndexed(6);
    }

    if (m_wireCount && m_wirePipeline) {
      cb->setGraphicsPipeline(m_wirePipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_wireBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_wireSrb);
      cb->draw(m_wireCount);
    }

    if (m_fillCount && m_boxPipeline) {
      cb->setGraphicsPipeline(m_boxPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_boxBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_fillCount);
    }

    if (m_dashCount && m_dashPipeline) {
      cb->setGraphicsPipeline(m_dashPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_dashBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_dashCount);
    }

    if (m_handleCount && m_boxPipeline) {
      cb->setGraphicsPipeline(m_boxPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_boxBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_handleCount, 1, m_fillCount);
    }

    if (m_cutCount && m_boxPipeline) {
      cb->setGraphicsPipeline(m_boxPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_cutBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_cutCount);
    }

    if (m_zenCount && m_boxPipeline) {
      cb->setGraphicsPipeline(m_boxPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_zenBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_zenCount);
    }

    if (m_layerCount && m_boxPipeline) {
      cb->setGraphicsPipeline(m_boxPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_layerBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_layerCount);
    }

    if (m_markerCount && m_boxPipeline) {
      cb->setGraphicsPipeline(m_boxPipeline.get());
      const QRhiCommandBuffer::VertexInput in(m_markerBuffer, 0);
      cb->setVertexInput(0, 1, &in);
      cb->setShaderResources(m_gridSrb);
      cb->draw(m_markerCount);
    }

    m_text.draw(cb);

    cb->endPass();
    }
    if (perfOn()) {
      const double ms = double(renderTimer.nsecsElapsed()) * 1e-6;
      gperf().msRender += ms;
      if (ms > perfSlowMs()) {
        qInfo(perfLog(), "grid SLOW render = %.1f ms (perspective %d)", ms,
                 static_cast<int>(m_perspective));
      }
    }
    gridPerfReport();
  }

private:
  void releaseResources() {
    m_gridPipeline.reset();
    m_wirePipeline.reset();
    m_densityPipeline.reset();
    m_dashPipeline.reset();
    m_boxPipeline.reset();
    m_text.invalidate();
    delete m_gridSrb;
    delete m_wireSrb;
    m_gridSrb = m_wireSrb = nullptr;
    delete m_ubo;
    delete m_quadBuffer;
    delete m_quadIndexBuffer;
    delete m_wireBuffer;
    delete m_dashBuffer;
    delete m_boxBuffer;
    m_ubo = m_quadBuffer = m_quadIndexBuffer = m_wireBuffer = m_dashBuffer = m_boxBuffer = nullptr;
    m_wireCapacity = 0;
    m_wireCount = 0;
    m_wireBufferFresh = false;
    m_slots.clear();
    m_wireAlloc.clear();
    m_slotVerts.clear();
    m_dashCapacity = 0;
    m_dashCount = 0;
    m_boxCapacity = 0;
    m_fillCount = 0;
    m_handleCount = 0;
    delete m_cutBuffer;
    m_cutBuffer = nullptr;
    m_cutCapacity = 0;
    m_cutCount = 0;
    delete m_zenBuffer;
    m_zenBuffer = nullptr;
    m_zenCapacity = 0;
    m_zenCount = 0;
    delete m_layerBuffer;
    m_layerBuffer = nullptr;
    m_layerCapacity = 0;
    m_layerCount = 0;
    delete m_markerBuffer;
    m_markerBuffer = nullptr;
    m_markerCapacity = 0;
    m_markerCount = 0;
    m_dashZoomKey = -1.0f;
    m_dashRect = glm::dvec4(0.0, 0.0, 0.0, 0.0);
    m_selBoxValid = false;
    m_selBoxIsMarquee = false;
    delete m_densityTarget;
    delete m_densityTex;
    m_densityTarget = nullptr;
    m_densityTex = nullptr;
    m_densityRp = nullptr;
    m_builtRenderPass = nullptr;
    m_wireScene = nullptr;
  }

  bool ensureResources(QRhiRenderTarget *target, QRhiResourceUpdateBatch *rub, const QSize &pixelSize);
  void uploadUniforms(QRhiResourceUpdateBatch *rub, const QSize &pixelSize);
  void rebuildWire(QRhiResourceUpdateBatch *rub);
  // One pass over the objects: build new, patch moved, blank and free gone.
  void reconcileWire(QRhiResourceUpdateBatch *rub, const PlaneBasis &basis, bool structureChanged);
  // Zeros a slot (CPU mirror and GPU) so it stops drawing without moving anything.
  void blankWireRange(QRhiResourceUpdateBatch *rub, quint32 start, quint32 count);
  void buildSlotWire(const eng::GameObject &obj, const PlaneBasis &basis, bool zenOut = false);
  // Sets m_wireBufferFresh when re-created, so the CPU mirror goes back up into the empty buffer.
  bool wireReserve(quint32 bytes);
  void rebuildSelBox(QRhiResourceUpdateBatch *rub);
  // Uploads `verts` (`stride` floats per vertex), growing the buffer; returns the vertex count to draw (0 on
  // failure). The one place the count is derived, so a draw never reads stale bytes past what was written.
  quint32 uploadVerts(QRhiResourceUpdateBatch *rub, QRhiBuffer *&buffer, quint32 &capacity,
                      const QVector<float> &verts, int stride);
  void rebuildCutLine(QRhiResourceUpdateBatch *rub);
  void rebuildZenBox(QRhiResourceUpdateBatch *rub);
  void rebuildCutHole(QRhiResourceUpdateBatch *rub);
  void rebuildFillLines(QRhiResourceUpdateBatch *rub);
  void rebuildEntityMarkers(QRhiResourceUpdateBatch *rub);
  void addSelectionLabels(const glm::dvec4 &box, const double zoom);

  GridView::Perspective m_perspective {GridView::Perspective::Top};
  bool m_showGrid {true};
  glm::dvec2 m_center {0.0, 0.0};
  double m_pxPerWorldUnit {0.05};
  double m_gridStepSource {GridView::kBaseGridStepSource};
  double m_viewWidth {1.0};
  double m_viewHeight {1.0};
  qreal m_devicePixelRatio {1.0};
  QSharedPointer<eng::Scene> m_scene;
  QQuickRhiItem *m_item = nullptr;

  QRhiRenderPassDescriptor *m_builtRenderPass = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> m_gridPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_wirePipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_densityPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_dashPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_boxPipeline;
  QRhiShaderResourceBindings *m_gridSrb = nullptr;
  QRhiShaderResourceBindings *m_wireSrb = nullptr;
  TextRenderer m_text {QStringLiteral(":/shaders/gridtext.vert.qsb")};

  QRhiBuffer *m_ubo = nullptr;
  QRhiBuffer *m_quadBuffer = nullptr;
  QRhiBuffer *m_quadIndexBuffer = nullptr;
  QRhiBuffer *m_wireBuffer = nullptr;
  quint32 m_wireCapacity = 0;
  quint32 m_wireCount = 0;
  bool m_wireBufferFresh = false;
  // One slot per object, keyed by stable id. Vertices past `cap` are zero-filled and rasterise to nothing,
  // so a slot can shrink in place.
  struct WireSlot {
    quint32 start = 0;
    quint32 cap = 0;
    quint32 count = 0;
    quint64 key = 0;
  };
  QHash<quint32, WireSlot> m_slots;
  RangeAllocator m_wireAlloc;
  QVector<float> m_slotVerts;

  QRhiBuffer *m_dashBuffer = nullptr;
  quint32 m_dashCapacity = 0;
  quint32 m_dashCount = 0;
  float m_dashZoomKey = -1.0f;
  bool m_dashMarquee = false;
  bool m_dashRotate = false;
  bool m_rotateHandles = false; // rotate mode: black handles with a white rim
  bool m_rotating = false;      // a 2D rotate drag: the angle label
  double m_rotateAngle = 0.0;
  bool m_entityKnobs = false;   // selected oriented entities show their rotate knob
  // Anchor handles replace the resize handles.
  bool m_anchorMode = false;
  std::vector<glm::dvec2> m_anchorHandles;
  bool m_dashAnchorMode = false;
  std::vector<glm::dvec2> m_dashAnchors;
  quint32 m_entRotId = 0;       // the entity whose knob is dragged (angle label)
  double m_entRotAngle = 0.0;
  QVector<float> m_dashVerts;
  glm::dvec4 m_dashRect {0.0, 0.0, 0.0, 0.0};

  QRhiBuffer *m_boxBuffer = nullptr;
  quint32 m_boxCapacity = 0;
  quint32 m_fillCount = 0;
  quint32 m_handleCount = 0;
  QVector<float> m_boxVerts;
  glm::dvec4 m_selBox {0.0, 0.0, 0.0, 0.0};
  bool m_selBoxValid {false};
  bool m_selBoxIsMarquee {false};

  QRhiBuffer *m_cutBuffer = nullptr;
  quint32 m_cutCapacity = 0;
  quint32 m_cutCount = 0;
  QVector<float> m_cutVerts;

  QRhiBuffer *m_zenBuffer = nullptr;
  quint32 m_zenCapacity = 0;
  quint32 m_zenCount = 0;
  QVector<float> m_zenVerts;

  // Layer seams of the live auto-select fill (elevation views).
  QRhiBuffer *m_layerBuffer = nullptr;
  quint32 m_layerCapacity = 0;
  quint32 m_layerCount = 0;
  QVector<float> m_layerVerts;
  std::vector<double> m_layerBounds {};
  Engine3D::CutMode m_cutMode {Engine3D::CutMode::Off};
  Engine3D::CutLine m_cutLine {};
  std::vector<Engine3D::CutHole> m_cutHoles {};
  bool m_cutIsCutout {false};
  std::vector<std::pair<glm::dvec3, glm::dvec3>> m_cutHoleBoxes {};
  glm::dvec4 m_cutSelBox {0.0, 0.0, 0.0, 0.0};
  bool m_cutSelValid {false};

  QVector<EntityMarker> m_markers;
  std::vector<IOLink> m_ioLinks;
  bool m_mirrorOn = false;      // anything of mirror mode / links to draw
  std::vector<Engine3D::MirrorShownPlane> m_mirrorPlanes; // the line / hover preview / selection's link planes
  std::vector<glm::vec4> m_mirrorLinked; // edges of linked brushes in this view (a.xy, b.xy)
  std::vector<std::pair<glm::dvec3, glm::dvec3>> m_mirrorTwins;  // twins of the selected linked brushes
  std::vector<std::vector<glm::dvec3>> m_mirrorTwinFaces;        // twins of the selected faces (world polygons)
  std::vector<glm::vec4> m_physicsEdges; // edges of the physics simulation's bodies in this view (a.xy, b.xy)
  std::optional<glm::dvec4> m_vertexBand; // Edit mode's rubber band (a0, b0, a1, b1)
  std::vector<IOPulse> m_ioPulses;
  void appendIOLinks(double zoom, float halfThick);
  void appendMirror(double zoom);
  void appendPaths(double zoom);
  std::vector<Engine3D::PathDraw> m_paths;
  std::vector<glm::dvec3> m_penPoly, m_penPoints;
  std::vector<Engine3D::EditHandleDraw> m_editHandles;
  QRhiBuffer *m_markerBuffer = nullptr;
  quint32 m_markerCapacity = 0;
  quint32 m_markerCount = 0;
  QVector<float> m_markerVerts;

  QRhiTexture *m_densityTex = nullptr;
  QRhiTextureRenderTarget *m_densityTarget = nullptr;
  QRhiRenderPassDescriptor *m_densityRp = nullptr;
  QSize m_densitySize;

  QVector<float> m_verts;
  const eng::Scene *m_wireScene = nullptr;
  quint64 m_wireStructure = ~0ull;
  quint64 m_wireXformEpoch = ~0ull;
  quint64 m_wireGeomEpoch = ~0ull;
  quint64 m_wireZenEpoch = ~0ull;
  GridView::Perspective m_wirePerspective {GridView::Perspective::Top};
};

bool GridViewRenderer::ensureResources(QRhiRenderTarget *target, QRhiResourceUpdateBatch *rub,
                                      const QSize &pixelSize) {
  QRhi *rhi = this->rhi();
  auto &shared = Shared::instance();
  QRhiRenderPassDescriptor *rp = target->renderPassDescriptor();
  if (!rp)
    return false;

  const bool pipelinesOk = m_gridPipeline && m_wirePipeline && m_densityPipeline && m_dashPipeline
      && m_boxPipeline && m_gridSrb && m_wireSrb && m_builtRenderPass == rp
      && m_densitySize == pixelSize;
  if (pipelinesOk)
    return true;

  if (!m_ubo) {
    m_ubo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(GridUniforms));
    if (!m_ubo->create())
      return false;
  }

  if (!m_quadBuffer) {
    static const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
    static const quint32 indices[] = {0, 1, 2, 0, 2, 3};

    m_quadBuffer = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(quad));
    if (!m_quadBuffer->create())
      return false;
    m_quadIndexBuffer = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::IndexBuffer, sizeof(indices));
    if (!m_quadIndexBuffer->create())
      return false;

    rub->updateDynamicBuffer(m_quadBuffer, 0, sizeof(quad), quad);
    rub->updateDynamicBuffer(m_quadIndexBuffer, 0, sizeof(indices), indices);
  }

  if (!m_densityTex || m_densityTex->pixelSize() != pixelSize) {
    // Still bound by SRBs and frames in flight: deferred release (and it must be released, or resizes leak VRAM).
    shared.retireTexture(m_densityTex);
    m_densityTex = nullptr;
    delete m_densityTarget;
    m_densityTarget = nullptr;
    m_densityRp = nullptr;
    shared.retireShaderResourceBindings(m_wireSrb);
    m_wireSrb = nullptr;

    m_densityTex = rhi->newTexture(QRhiTexture::RGBA16F, pixelSize, 1, QRhiTexture::RenderTarget);
    if (!m_densityTex || !m_densityTex->create()) {
      qWarning(gridLog, "density texture %dx%d failed", pixelSize.width(), pixelSize.height());
      m_densityTex = nullptr;
      m_densitySize = QSize();
      return false;
    }

    QRhiTextureRenderTargetDescription desc;
    desc.setColorAttachments({QRhiColorAttachment(m_densityTex)});
    m_densityTarget = rhi->newTextureRenderTarget(desc);
    m_densityTarget->setRenderPassDescriptor(m_densityTarget->newCompatibleRenderPassDescriptor());
    if (!m_densityTarget->create()) {
      qWarning(gridLog, "density render target failed");
      delete m_densityTarget;
      m_densityTarget = nullptr;
      m_densitySize = QSize();
      return false;
    }
    m_densityRp = m_densityTarget->renderPassDescriptor();
    m_densitySize = pixelSize;
  }

  m_gridPipeline.reset();
  m_wirePipeline.reset();
  m_densityPipeline.reset();
  m_dashPipeline.reset();
  m_boxPipeline.reset();
  shared.retireShaderResourceBindings(m_gridSrb);
  m_gridSrb = nullptr;

  const auto alphaBlend = [] {
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
  };

  m_gridSrb = rhi->newShaderResourceBindings();
  m_gridSrb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubo),
  });
  if (!m_gridSrb->create())
    return false;

  m_wireSrb = rhi->newShaderResourceBindings();
  m_wireSrb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubo),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, m_densityTex, shared.nearestSampler()),
  });
  if (!m_wireSrb->create())
    return false;

  QList<QRhiShaderStage> wireStages;
  wireStages.append(
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/gridwire.vert.qsb"))});
  wireStages.append(
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/gridwire.frag.qsb"))});

  QRhiVertexInputLayout wireLayout;
  wireLayout.setBindings({{4 * sizeof(float)}});
  wireLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float4, 0}});

  m_gridPipeline.reset(rhi->newGraphicsPipeline());
  m_gridPipeline->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/grid.vert.qsb"))},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/grid.frag.qsb"))},
  });
  {
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    m_gridPipeline->setVertexInputLayout(layout);
  }
  m_gridPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_gridPipeline->setCullMode(QRhiGraphicsPipeline::None);
  m_gridPipeline->setDepthTest(false);
  m_gridPipeline->setDepthWrite(false);
  m_gridPipeline->setTargetBlends({alphaBlend()});
  m_gridPipeline->setRenderPassDescriptor(rp);
  m_gridPipeline->setShaderResourceBindings(m_gridSrb);
  if (!m_gridPipeline->create())
    qWarning(gridLog, "grid pipeline failed");

  m_wirePipeline.reset(rhi->newGraphicsPipeline());
  m_wirePipeline->setShaderStages(wireStages.begin(), wireStages.end());
  m_wirePipeline->setVertexInputLayout(wireLayout);
  m_wirePipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_wirePipeline->setCullMode(QRhiGraphicsPipeline::None);
  m_wirePipeline->setDepthTest(false);
  m_wirePipeline->setDepthWrite(false);
  m_wirePipeline->setTargetBlends({alphaBlend()});
  m_wirePipeline->setRenderPassDescriptor(rp);
  m_wirePipeline->setShaderResourceBindings(m_wireSrb);
  if (!m_wirePipeline->create())
    qWarning(gridLog, "wire pipeline failed");

  QList<QRhiShaderStage> densityStages = wireStages;
  densityStages[1] = {QRhiShaderStage::Fragment,
                      shared.shader(QStringLiteral(":/shaders/gridwire_density.frag.qsb"))};

  QRhiGraphicsPipeline::TargetBlend accumulate;
  accumulate.enable = true;
  accumulate.srcColor = QRhiGraphicsPipeline::One;
  accumulate.dstColor = QRhiGraphicsPipeline::One;
  accumulate.srcAlpha = QRhiGraphicsPipeline::One;
  accumulate.dstAlpha = QRhiGraphicsPipeline::One;

  m_densityPipeline.reset(rhi->newGraphicsPipeline());
  m_densityPipeline->setShaderStages(densityStages.begin(), densityStages.end());
  m_densityPipeline->setVertexInputLayout(wireLayout);
  m_densityPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_densityPipeline->setCullMode(QRhiGraphicsPipeline::None);
  m_densityPipeline->setDepthTest(false);
  m_densityPipeline->setDepthWrite(false);
  m_densityPipeline->setTargetBlends({accumulate});
  m_densityPipeline->setRenderPassDescriptor(m_densityRp);
  m_densityPipeline->setShaderResourceBindings(m_gridSrb);
  if (!m_densityPipeline->create())
    qWarning(gridLog, "density pipeline failed");

  m_dashPipeline.reset(rhi->newGraphicsPipeline());
  m_dashPipeline->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/gridwire.vert.qsb"))},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/dash.frag.qsb"))},
  });
  m_dashPipeline->setVertexInputLayout(wireLayout);
  m_dashPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_dashPipeline->setCullMode(QRhiGraphicsPipeline::None);
  m_dashPipeline->setDepthTest(false);
  m_dashPipeline->setDepthWrite(false);
  m_dashPipeline->setTargetBlends({alphaBlend()});
  m_dashPipeline->setRenderPassDescriptor(rp);
  m_dashPipeline->setShaderResourceBindings(m_gridSrb);
  if (!m_dashPipeline->create())
    qWarning(gridLog, "dash pipeline failed");

  m_boxPipeline.reset(rhi->newGraphicsPipeline());
  m_boxPipeline->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/selbox.vert.qsb"))},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/selbox.frag.qsb"))},
  });
  QRhiVertexInputLayout boxLayout;
  boxLayout.setBindings({{8 * sizeof(float)}});
  boxLayout.setAttributes({
      {0, 0, QRhiVertexInputAttribute::Float4, 0},
      {0, 1, QRhiVertexInputAttribute::Float4, 4 * sizeof(float)},
  });
  m_boxPipeline->setVertexInputLayout(boxLayout);
  m_boxPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_boxPipeline->setCullMode(QRhiGraphicsPipeline::None);
  m_boxPipeline->setDepthTest(false);
  m_boxPipeline->setDepthWrite(false);
  m_boxPipeline->setTargetBlends({alphaBlend()});
  m_boxPipeline->setRenderPassDescriptor(rp);
  m_boxPipeline->setShaderResourceBindings(m_gridSrb);
  if (!m_boxPipeline->create())
    qWarning(gridLog, "selection box pipeline failed");

  m_builtRenderPass = rp;
  return true;
}

void GridViewRenderer::uploadUniforms(QRhiResourceUpdateBatch *rub, const QSize &pixelSize) {
  const qreal dpr = qMax(1.0, m_devicePixelRatio);
  const PlaneBasis basis = basisFor(m_perspective);

  const double halfW = (m_viewWidth * 0.5) / m_pxPerWorldUnit;
  const double halfH = (m_viewHeight * 0.5) / m_pxPerWorldUnit;

  const glm::mat4 corr = qtToGlm(rhi()->clipSpaceCorrMatrix());
  const glm::mat4 gridToClip = corr * glm::ortho(float(m_center.x - halfW), float(m_center.x + halfW),
                                               float(m_center.y - halfH), float(m_center.y + halfH),
                                               -1.0f, 1.0f);

  GridUniforms u {};
  u.gridToClip = gridToClip;
  u.clipToGrid = glm::inverse(gridToClip);
  u.worldToClip = gridToClip * basisMatrix(basis);
  u.viewInfo = glm::vec4(float(m_pxPerWorldUnit), float(dpr), m_showGrid ? 1.0f : 0.0f, 1.0f);

  // The level step is the MAJOR line spacing (the snap); minors are its 8 subdivisions.
  const double majorWorld = m_gridStepSource / GridView::kUnitsPerWorldUnit;
  const double majorEvery = GridView::kMajorGridEvery;
  const double pxMajor = majorWorld * m_pxPerWorldUnit * dpr;
  const double pxMinor = pxMajor / majorEvery;
  const auto fade = [](const double px, const double maxAlpha) {
    return static_cast<float>(std::clamp((px - 1.0) / 2.0, 0.0, 1.0) * maxAlpha);
  };
  const float minorWidthPx = static_cast<float>(std::clamp(pxMinor, 0.35, 1.0));
  const float minorAlpha = static_cast<float>(0.55 * std::clamp(pxMinor, 0.30, 1.0));
  u.gridStyle = glm::vec4(float(majorWorld / majorEvery), float(majorEvery), minorAlpha,
                          fade(pxMajor, 0.85));
  u.colorMinor = glm::vec4(0.24f, 0.28f, 0.37f, 1.0f);
  u.colorMajor = glm::vec4(0.36f, 0.42f, 0.55f, 1.0f);
  u.colorAxis = glm::vec4(0.62f, 0.40f, 0.46f, 1.0f);
  u.lineScale = glm::vec4(2.0f / float(pixelSize.width()), 2.0f / float(pixelSize.height()),
                          minorWidthPx, 0.0f);

  // x = extra overlapping meshes for the full colour swing, yz = density texel size,
  // w = line half thickness in world units (one device pixel).
  const float halfThickWorld = float(1.0 / (m_pxPerWorldUnit * dpr));
  u.wireStyle = glm::vec4(7.0f, float(pixelSize.width()), float(pixelSize.height()), halfThickWorld);
  u.wireCool = glm::vec4(0.26f, 0.52f, 0.96f, 1.0f);
  u.wireWarm = glm::vec4(0.58f, 1.00f, 0.80f, 1.0f);

  rub->updateDynamicBuffer(m_ubo, 0, sizeof(GridUniforms), &u);
}

bool GridViewRenderer::wireReserve(quint32 bytes) {
  if (m_wireBuffer && m_wireCapacity >= bytes) {
    m_wireBufferFresh = false;
    return true;
  }

  Shared::instance().retire(m_wireBuffer);
  m_wireBuffer = nullptr;
  m_wireCapacity = std::max({bytes, m_wireCapacity * 2, 65536u});
  m_wireBuffer = rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, int(m_wireCapacity));
  if (!m_wireBuffer->create()) {
    Shared::instance().retire(m_wireBuffer);
    m_wireBuffer = nullptr;
    m_wireCapacity = 0;
    return false;
  }
  m_wireBufferFresh = true;
  return true;
}

void GridViewRenderer::blankWireRange(QRhiResourceUpdateBatch *rub, const quint32 start,
                                     const quint32 count) {
  if (count == 0 || size_t(start + count) * 4 > size_t(m_verts.size()))
    return;
  float *dst = m_verts.data() + size_t(start) * 4;
  std::memset(dst, 0, size_t(count) * 4 * sizeof(float));
  if (m_wireBuffer && rub) {
    rub->updateDynamicBuffer(m_wireBuffer, int(size_t(start) * 4 * sizeof(float)),
                            int(size_t(count) * 4 * sizeof(float)), dst);
  }
}

void GridViewRenderer::buildSlotWire(const eng::GameObject &obj, const PlaneBasis &basis, const bool zenOut) {
  m_slotVerts.clear();
  // Hiding bumps the transform version, so the slot is rebuilt; zen's "outside" is part of the slot key.
  if (obj.hidden() || zenOut)
    return;
  if (const auto &mesh = obj.meshRef()) {
    const glm::mat4 *pose = g::engine3D ? g::engine3D->physicsPose(obj.stableId()) : nullptr;
    buildObjectWire(*mesh, pose ? *pose * obj.getModelMatrix() : obj.getModelMatrix(), basis, m_slotVerts);
  }
}

// The world wire can be ~350k vertices: global epochs say whether anything moved, per-object slots what to rewrite.
void GridViewRenderer::rebuildWire(QRhiResourceUpdateBatch *rub) {
  const quint64 structure = m_scene ? m_scene->structureVersion() : 0;
  const quint64 xformEpoch = eng::GameObject::globalTransformEpoch()
                             ^ (g::engine3D ? g::engine3D->physicsPoseEpoch() * 0x9E3779B97F4A7C15ull : 0ull);
  const quint64 geomEpoch = eng::Mesh::globalGeometryEpoch();
  const bool sameTarget = m_scene.data() == m_wireScene && structure == m_wireStructure
      && m_perspective == m_wirePerspective;
  const bool structureChanged = structure != m_wireStructure || m_scene.data() != m_wireScene;

  const quint64 zenEpoch = g::engine3D ? g::engine3D->zenEpoch() : 0;
  if (sameTarget && xformEpoch == m_wireXformEpoch && geomEpoch == m_wireGeomEpoch && zenEpoch == m_wireZenEpoch)
    return;
  m_wireZenEpoch = zenEpoch;

  const bool samePlane = m_scene.data() == m_wireScene && m_perspective == m_wirePerspective;

  if (!m_scene) {
    m_verts.clear();
    m_slots.clear();
    m_wireAlloc.clear();
    m_wireCount = 0;
    m_wireScene = nullptr;
    m_wireStructure = structure;
    m_wireXformEpoch = xformEpoch;
    m_wireGeomEpoch = geomEpoch;
    return;
  }

  const PlaneBasis basis = basisFor(m_perspective);

  // A new document or another axis: nothing to patch.
  if (!samePlane) {
    if (m_scene.data() != m_wireScene)
      ++gperf().whyScene;
    else
      ++gperf().whyPersp;
    m_slots.clear();
    m_wireAlloc.clear();
    m_verts.clear();
  } else if (structureChanged) {
    ++gperf().whyStruct;
  } else {
    ++gperf().whySlots;
  }

  m_wireScene = m_scene.data();
  m_wireStructure = structure;
  m_wireXformEpoch = xformEpoch;
  m_wireGeomEpoch = geomEpoch;
  m_wirePerspective = m_perspective;

  reconcileWire(rub, basis, structureChanged && samePlane);
}

void GridViewRenderer::reconcileWire(QRhiResourceUpdateBatch *rub, const PlaneBasis &basis,
                                    const bool structureChanged) {
  struct Range {
    quint32 start;
    quint32 count;
  };
  std::vector<Range> dirty;

  auto noteRange = [&dirty](const quint32 start, const quint32 count) {
    if (count)
      dirty.push_back({start, count});
  };

  // Objects that are gone: blank their space and hand the range back (no compaction).
  if (structureChanged) {
    QHash<quint32, bool> present;
    present.reserve(qsizetype(m_scene->gameObjects.size()) * 2);
    for (const auto &obj: m_scene->gameObjects)
      present.insert(obj->stableId(), true);

    for (auto it = m_slots.begin(); it != m_slots.end();) {
      if (present.contains(it.key())) {
        ++it;
        continue;
      }
      blankWireRange(rub, it.value().start, it.value().cap);
      m_wireAlloc.release(it.value().start, it.value().cap);
      it = m_slots.erase(it);
    }
  }

  const auto &objects = m_scene->gameObjects;
  for (const auto &obj: objects) {
    const quint32 stableId = obj->stableId();
    // Zen: "outside" is part of the key, so moving the box rewrites only slots crossing its edge.
    const bool zenOut = g::engine3D && g::engine3D->zenHides(*obj);
    quint64 key = wireKeyFor(*obj) ^ (zenOut ? 0x9E3779B97F4A7C15ull : 0ull);
    // A body moved by the running simulation is drawn where it is now.
    if (g::engine3D && g::engine3D->physicsPose(stableId))
      key ^= (g::engine3D->physicsPoseEpoch() + 1) * 0xC2B2AE3D27D4EB4Full;

    auto it = m_slots.find(stableId);
    if (it != m_slots.end() && it.value().key == key)
      continue;

    buildSlotWire(*obj, basis, zenOut);
    const quint32 count = quint32(m_slotVerts.size()) / 4;

    gperf().wirePatched += 1;
    gperf().wirePatchVerts += qint64(count);

    if (it != m_slots.end()) {
      WireSlot &slot = it.value();
      slot.key = key;
      slot.count = count;
      if (count <= slot.cap) {
        float *dst = m_verts.data() + size_t(slot.start) * 4;
        if (count)
          std::memcpy(dst, m_slotVerts.data(), size_t(count) * 4 * sizeof(float));
        if (slot.cap > count)
          std::memset(dst + size_t(count) * 4, 0, size_t(slot.cap - count) * 4 * sizeof(float));
        noteRange(slot.start, slot.cap);
        continue;
      }
      // Grew past its window: take a new one.
      std::memset(m_verts.data() + size_t(slot.start) * 4, 0, size_t(slot.cap) * 4 * sizeof(float));
      noteRange(slot.start, slot.cap);
      m_wireAlloc.release(slot.start, slot.cap);
      m_slots.erase(it);
    }

    if (count == 0)
      continue;

    Q_ASSERT(count % kWireVertsPerTriangle == 0);
    const quint32 cap = alignWireVerts(count + std::max(count, 32u));
    const quint32 start = m_wireAlloc.allocate(cap);
    if (m_verts.size() < size_t(start + cap) * 4)
      m_verts.resize(size_t(start + cap) * 4, 0.0f);
    std::memcpy(m_verts.data() + size_t(start) * 4, m_slotVerts.data(),
                size_t(count) * 4 * sizeof(float));
    m_slots.insert(stableId, WireSlot {start, cap, count, key});
    noteRange(start, cap);
  }

  m_wireCount = m_wireAlloc.highWater();

  const quint32 bytes = m_wireCount * 4u * quint32(sizeof(float));
  if (m_wireCount == 0 || !wireReserve(bytes)) {
    m_wireCount = 0;
    return;
  }

  if (m_wireBufferFresh) {
    // A re-created buffer is empty, so the whole CPU mirror goes back down.
    if (bytes)
      rub->updateDynamicBuffer(m_wireBuffer, 0, int(bytes), m_verts.data());
    return;
  }

  for (const Range &r: dirty)
    rub->updateDynamicBuffer(m_wireBuffer, int(size_t(r.start) * 4 * sizeof(float)),
                            int(size_t(r.count) * 4 * sizeof(float)),
                            m_verts.data() + size_t(r.start) * 4);
}

void GridViewRenderer::addSelectionLabels(const glm::dvec4 &box, const double zoom) {
  const double side = GridView::kHandleSizePx / zoom;
  const double gap = 4.0 / zoom;
  const double midA = (box.x + box.z) * 0.5;
  const double midB = (box.y + box.w) * 0.5;

  const QString widthText =
      QString::number((box.z - box.x) * GridView::kUnitsPerWorldUnit, 'f', 1);
  const QString heightText =
      QString::number((box.w - box.y) * GridView::kUnitsPerWorldUnit, 'f', 1);

  const QSize wSprite = m_text.size(widthText);
  const QSize hSprite = m_text.size(heightText);
  const float wW = float(double(wSprite.width()) / zoom);
  const float wH = float(double(wSprite.height()) / zoom);
  const float hW = float(double(hSprite.width()) / zoom);
  const float hH = float(double(hSprite.height()) / zoom);

  // The decal origin is the glyph box's top-left, laid out down the screen (-b).
  const glm::vec3 right(1.0f, 0.0f, 0.0f);
  const glm::vec3 down(0.0f, -1.0f, 0.0f);
  const QColor white(255, 255, 255);

  m_text.drawDecal(widthText,
                   glm::vec3(float(midA - wW * 0.5), float(box.w + side + gap + wH), 0.0f), right,
                   down, wW, wH, white, 1.0f);
  m_text.drawDecal(heightText,
                   glm::vec3(float(box.x - side - gap - hW), float(midB + hH * 0.5), 0.0f), right,
                   down, hW, hH, white, 1.0f);
}

// The selection box: outline, fill, handles and size labels (box decided in GridView::resizeBox).
void GridViewRenderer::rebuildSelBox(QRhiResourceUpdateBatch *rub) {
  const qreal dpr = qMax(1.0, m_devicePixelRatio);
  const double zoom = m_pxPerWorldUnit * dpr;
  if (zoom <= 1e-9) {
    m_dashCount = 0;
    m_fillCount = 0;
    m_handleCount = 0;
    return;
  }

  m_text.setPixelSize(std::max(8, int(18.0 * dpr)));

  if (!m_selBoxValid) {
    m_dashCount = 0;
    m_fillCount = 0;
    m_handleCount = 0;
    m_dashRect = glm::dvec4(0.0, 0.0, 0.0, 0.0);
    m_dashZoomKey = -1.0f;
    return;
  }

  const glm::dvec4 box = m_selBox;
  addSelectionLabels(box, zoom);
  if (m_rotating) {
    // Counter-clockwise on screen, off the box's top-right corner.
    const QString text = QStringLiteral("%1°").arg(m_rotateAngle, 0, 'f', std::fabs(m_rotateAngle - std::round(m_rotateAngle)) < 1e-6 ? 0 : 1);
    const QSize sprite = m_text.size(text);
    const double side = GridView::kHandleSizePx / zoom, gap = 4.0 / zoom;
    const float w = float(double(sprite.width()) / zoom), h = float(double(sprite.height()) / zoom);
    m_text.drawDecal(text, glm::vec3(float(box.z + side + gap), float(box.w + side + gap + h), 0.0f),
                     glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), w, h, QColor(255, 220, 90), 1.0f);
  }

  // The marquee flag is part of the key: a new brush's box has the marquee's exact rect.
  if (box == m_dashRect && zoom == m_dashZoomKey && m_selBoxIsMarquee == m_dashMarquee
      && m_rotateHandles == m_dashRotate && m_anchorMode == m_dashAnchorMode && m_anchorHandles == m_dashAnchors)
    return;
  m_dashRect = box;
  m_dashMarquee = m_selBoxIsMarquee;
  m_dashRotate = m_rotateHandles;
  m_dashAnchorMode = m_anchorMode;
  m_dashAnchors = m_anchorHandles;
  m_dashZoomKey = zoom;

  const float side = float(GridView::kHandleSizePx / zoom);

  m_dashVerts.clear();
  dashRect(m_dashVerts, glm::vec2(float(box.x), float(box.y)), glm::vec2(float(box.z), float(box.w)),
           float(5.0 / zoom), float(4.0 / zoom));

  m_boxVerts.clear();
  appendBoxQuad(m_boxVerts, box, m_selBoxIsMarquee ? kMarqueeFill : kSelectionFill);
  // Two quads per handle: rim and inside (black in rotate mode).
  const double rim = 1.0 / zoom;
  int handleQuads = 0;
  if (m_anchorMode) {
    // Anchor mode: one handle per anchor, nothing else.
    for (const glm::dvec2 &a: m_anchorHandles) {
      const glm::dvec4 r(a.x - side * 0.5, a.y - side * 0.5, a.x + side * 0.5, a.y + side * 0.5);
      appendBoxQuad(m_boxVerts, {r.x - rim, r.y - rim, r.z + rim, r.w + rim}, kHandleColor);
      appendBoxQuad(m_boxVerts, r, kHandleColor);
      handleQuads += 2;
    }
  } else {
    for (int h = 0; h < SelectionHandleCount; ++h) {
      const glm::dvec4 r = handleRect(box, h, side);
      appendBoxQuad(m_boxVerts, {r.x - rim, r.y - rim, r.z + rim, r.w + rim}, kHandleColor);
      appendBoxQuad(m_boxVerts, r, m_rotateHandles ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : kHandleColor);
      handleQuads += 2;
    }
  }

  m_dashCount = uploadVerts(rub, m_dashBuffer, m_dashCapacity, m_dashVerts, kWireFloats);

  const bool boxes = uploadVerts(rub, m_boxBuffer, m_boxCapacity, m_boxVerts, kBoxFloats) > 0;
  m_fillCount = boxes ? 6 : 0;
  m_handleCount = boxes ? static_cast<quint32>(handleQuads) * 6 : 0;
}

quint32 GridViewRenderer::uploadVerts(QRhiResourceUpdateBatch *rub, QRhiBuffer *&buffer, quint32 &capacity,
                                      const QVector<float> &verts, const int stride) {
  const int floats = int(verts.size()) / stride * stride; // whole vertices only
  const quint32 bytes = quint32(floats) * quint32(sizeof(float));
  if (!rub || bytes == 0)
    return 0;
  if (!buffer || bytes > capacity) {
    delete buffer;
    capacity = bytes * 2;
    buffer = rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, capacity);
    if (!buffer->create()) {
      delete buffer;
      buffer = nullptr;
      capacity = 0;
      return 0;
    }
  }
  rub->updateDynamicBuffer(buffer, 0, bytes, verts.constData());
  return quint32(floats / stride);
}

// Zen mode's box around the 3D camera, outlined.
void GridViewRenderer::rebuildZenBox(QRhiResourceUpdateBatch *rub) {
  m_zenCount = 0;
  glm::vec3 mn, mx;
  if (!rub || !g::engine3D || !g::engine3D->zenBox(mn, mx))
    return;
  const double zoom = m_pxPerWorldUnit * qMax(1.0, m_devicePixelRatio);
  if (zoom <= 1e-9)
    return;
  const ViewBasis vb = viewBasisFor(static_cast<int>(m_perspective));
  double a0 = DBL_MAX, a1 = -DBL_MAX, b0 = DBL_MAX, b1 = -DBL_MAX;
  for (int i = 0; i < 8; ++i) {
    const glm::dvec3 c((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
    const double a = glm::dot(vb.right, c), b = glm::dot(vb.up, c);
    a0 = std::min(a0, a);
    a1 = std::max(a1, a);
    b0 = std::min(b0, b);
    b1 = std::max(b1, b);
  }
  static const glm::vec4 kZenColor(0.55f, 0.72f, 1.0f, 0.55f);
  const float t = float(1.0 / zoom);
  m_zenVerts.clear();
  appendWireQuad(m_zenVerts, glm::vec2(float(a0), float(b0)), glm::vec2(float(a1), float(b0)), t, kZenColor);
  appendWireQuad(m_zenVerts, glm::vec2(float(a1), float(b0)), glm::vec2(float(a1), float(b1)), t, kZenColor);
  appendWireQuad(m_zenVerts, glm::vec2(float(a1), float(b1)), glm::vec2(float(a0), float(b1)), t, kZenColor);
  appendWireQuad(m_zenVerts, glm::vec2(float(a0), float(b1)), glm::vec2(float(a0), float(b0)), t, kZenColor);
  m_zenCount = uploadVerts(rub, m_zenBuffer, m_zenCapacity, m_zenVerts, kBoxFloats);
}

// The slice line across the view plus a red fill on the doomed half; nothing where the plane is face-on.
void GridViewRenderer::rebuildCutLine(QRhiResourceUpdateBatch *rub) {
  m_cutCount = 0;
  if (m_cutMode == Engine3D::CutMode::Off || !rub)
    return;
  if (m_cutIsCutout) {
    rebuildCutHole(rub);
    return;
  }
  if (!m_cutLine.valid)
    return;

  const qreal dpr = qMax(1.0, m_devicePixelRatio);
  const double zoom = m_pxPerWorldUnit * dpr;
  if (zoom <= 1e-9)
    return;

  if (m_cutLine.angled) {
    // Free-angle line: drawn only in its own view (a plane through the depth axis there).
    m_cutVerts.clear();
    if (m_cutLine.plane == static_cast<int>(m_perspective)) {
      const glm::dvec2 d = glm::normalize(m_cutLine.b - m_cutLine.a);
      const double reach = (std::max(m_viewWidth, m_viewHeight) / m_pxPerWorldUnit) * 2.0;
      const glm::dvec2 p0 = m_cutLine.a - d * reach, p1 = m_cutLine.a + d * reach;
      // The selection box clipped to the side the cut removes (as applyCut reads it; Positive removes the front).
      if (m_cutSelValid && m_cutMode != Engine3D::CutMode::KeepBoth) {
        const ViewBasis vb = viewBasisFor(m_cutLine.plane);
        const glm::dvec3 along = glm::normalize(vb.right * d.x + vb.up * d.y);
        const glm::dvec3 n = glm::normalize(glm::cross(along, vb.depth));
        const glm::dvec2 n2(glm::dot(n, vb.right), glm::dot(n, vb.up));
        const double k = glm::dot(n2, m_cutLine.a);
        const double side = m_cutMode == Engine3D::CutMode::Positive ? 1.0 : -1.0;
        const auto inside = [&](const glm::dvec2 &q) { return side * (glm::dot(n2, q) - k); };
        const glm::dvec4 &r = m_cutSelBox;
        const glm::dvec2 rect[4] = {{r.x, r.y}, {r.z, r.y}, {r.z, r.w}, {r.x, r.w}};
        std::vector<glm::dvec2> poly; // the rect clipped to the doomed half-plane
        for (int i = 0; i < 4; ++i) {
          const glm::dvec2 &p = rect[i], &q = rect[(i + 1) % 4];
          const double fp = inside(p), fq = inside(q);
          if (fp >= 0.0)
            poly.push_back(p);
          if ((fp >= 0.0) != (fq >= 0.0))
            poly.push_back(p + (q - p) * (fp / (fp - fq)));
        }
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
          pushBoxVertex(m_cutVerts, float(poly[0].x), float(poly[0].y), kCutFill);
          pushBoxVertex(m_cutVerts, float(poly[i].x), float(poly[i].y), kCutFill);
          pushBoxVertex(m_cutVerts, float(poly[i + 1].x), float(poly[i + 1].y), kCutFill);
        }
      }
      appendWireQuad(m_cutVerts, glm::vec2(p0), glm::vec2(p1), float(1.2 / zoom), kCutLineColor);
    }
    m_cutCount = uploadVerts(rub, m_cutBuffer, m_cutCapacity, m_cutVerts, kBoxFloats);
    return;
  }

  const ViewBasis own = viewBasisFor(m_cutLine.plane);
  const glm::dvec3 n = m_cutLine.alongRight ? own.up : own.right;
  const ViewBasis vb = viewBasisFor(static_cast<int>(m_perspective));
  const double projA = glm::dot(n, vb.right);
  const double projB = glm::dot(n, vb.up);

  const double halfW = (m_viewWidth * 0.5) / m_pxPerWorldUnit;
  const double halfH = (m_viewHeight * 0.5) / m_pxPerWorldUnit;
  const float halfThick = float(1.2 / zoom);
  const bool fills = m_cutSelValid && m_cutMode != Engine3D::CutMode::KeepBoth;

  m_cutVerts.clear();
  m_text.setPixelSize(std::max(8, int(18.0 * dpr)));

  // Label on the cut axis: the selection's size there now and after the cut.
  const auto cutLabel = [&](const QString &text, const double lineCoord, const bool vertical,
                           const bool keptHigh) {
    const QSize s = m_text.size(text);
    const float w = float(double(s.width()) / zoom);
    const float h = float(double(s.height()) / zoom);
    const float gap = float(10.0 / zoom);
    double cx = 0.0;
    double cy = 0.0;
    if (vertical) {
      cx = keptHigh ? lineCoord + gap + w * 0.5 : lineCoord - gap - w * 0.5;
      cy = (m_cutSelBox.y + m_cutSelBox.w) * 0.5;
    } else {
      cx = (m_cutSelBox.x + m_cutSelBox.z) * 0.5;
      cy = keptHigh ? lineCoord + gap + h * 0.5 : lineCoord - gap - h * 0.5;
    }
    m_text.drawDecal(text, glm::vec3(float(cx - w * 0.5), float(cy + h * 0.5), 0.0f),
                    glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), w, h,
                    QColor(255, 255, 255), 1.0f);
  };

  const auto sizeText = [&](const double lineCoord, const double axisLo, const double axisHi,
                           const bool removeHigh) {
    const double unit = GridView::kUnitsPerWorldUnit;
    const double split = std::clamp(lineCoord, axisLo, axisHi);
    const QString orig = QString::number((axisHi - axisLo) * unit, 'f', 1);
    if (m_cutMode == Engine3D::CutMode::KeepBoth)
      return QStringLiteral("%1 \u2192 %2 + %3")
          .arg(orig)
          .arg(QString::number((split - axisLo) * unit, 'f', 1))
          .arg(QString::number((axisHi - split) * unit, 'f', 1));
    const double kept = removeHigh ? split - axisLo : axisHi - split;
    return QStringLiteral("%1 \u2192 %2").arg(orig).arg(QString::number(kept * unit, 'f', 1));
  };

  if (std::fabs(projA) > 0.5) {
    const double a = m_cutLine.k * projA;
    appendWireQuad(m_cutVerts, glm::vec2(float(a), float(m_center.y - halfH)),
                  glm::vec2(float(a), float(m_center.y + halfH)), halfThick, kCutLineColor);
    const bool removeHigh = (m_cutMode == Engine3D::CutMode::Positive) == (projA > 0.0);
    if (fills) {
      glm::dvec4 r = m_cutSelBox;
      if (removeHigh)
        r.x = a;
      else
        r.z = a;
      if (r.z > r.x)
        appendBoxQuad(m_cutVerts, r, kCutFill);
    }
    if (m_cutSelValid)
      cutLabel(sizeText(a, m_cutSelBox.x, m_cutSelBox.z, removeHigh), a, true, !removeHigh);
  } else if (std::fabs(projB) > 0.5) {
    const double b = m_cutLine.k * projB;
    appendWireQuad(m_cutVerts, glm::vec2(float(m_center.x - halfW), float(b)),
                  glm::vec2(float(m_center.x + halfW), float(b)), halfThick, kCutLineColor);
    const bool removeHigh = (m_cutMode == Engine3D::CutMode::Positive) == (projB > 0.0);
    if (fills) {
      glm::dvec4 r = m_cutSelBox;
      if (removeHigh)
        r.y = b;
      else
        r.w = b;
      if (r.w > r.y)
        appendBoxQuad(m_cutVerts, r, kCutFill);
    }
    if (m_cutSelValid)
      cutLabel(sizeText(b, m_cutSelBox.y, m_cutSelBox.w, removeHigh), b, false, !removeHigh);
  }

  m_cutCount = uploadVerts(rub, m_cutBuffer, m_cutCapacity, m_cutVerts, kBoxFloats);
}

// A cut-out prism: the placing view sees the opening (with its size), the others the band through the wall.
void GridViewRenderer::rebuildCutHole(QRhiResourceUpdateBatch *rub) {
  m_cutCount = 0;
  if (m_cutMode == Engine3D::CutMode::Off || m_cutHoleBoxes.empty() || !rub)
    return;

  const qreal dpr = qMax(1.0, m_devicePixelRatio);
  const double zoom = m_pxPerWorldUnit * dpr;
  if (zoom <= 1e-9)
    return;

  const ViewBasis vb = viewBasisFor(static_cast<int>(m_perspective));
  const int aimPlane = m_cutHoles.empty() ? 0 : m_cutHoles.front().plane;
  const bool faceOn = std::fabs(glm::dot(vb.depth, viewBasisFor(aimPlane).depth)) > 0.5;
  const float halfThick = float(1.4 / zoom);

  m_cutVerts.clear();
  for (const auto &[hmn, hmx]: m_cutHoleBoxes) {
    double a0 = 0.0;
    double a1 = 0.0;
    double b0 = 0.0;
    double b1 = 0.0;
    projectOntoAxis(vb.right, hmn, hmx, a0, a1);
    projectOntoAxis(vb.up, hmn, hmx, b0, b1);
    if (a1 <= a0 || b1 <= b0)
      continue;

    const glm::vec2 c[4] = {
        {float(a0), float(b0)},
        {float(a1), float(b0)},
        {float(a1), float(b1)},
        {float(a0), float(b1)},
    };
    appendBoxQuad(m_cutVerts, glm::dvec4(a0, b0, a1, b1), kCutFill);
    for (int k = 0; k < 4; ++k)
      appendWireQuad(m_cutVerts, c[k], c[(k + 1) & 3], halfThick, kCutLineColor);

    // Only the face-on view gets the size.
    if (!faceOn)
      continue;
    const QString label = QStringLiteral("%1 x %2")
                             .arg(g::engine3D->cutoutWidthSource(), 0, 'f', 0)
                             .arg(g::engine3D->cutoutHeightSource(), 0, 'f', 0);
    m_text.setPixelSize(std::max(8, int(18.0 * dpr)));
    const QSize s = m_text.size(label);
    const float w = float(double(s.width()) / zoom);
    const float h = float(double(s.height()) / zoom);
    const float gap = float(8.0 / zoom);
    m_text.drawDecal(label,
                    glm::vec3(float((a0 + a1) * 0.5 - w * 0.5), float(b1 + gap + h), 0.0f),
                    glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), w, h,
                    QColor(255, 255, 255), 1.0f);
  }

  m_cutCount = uploadVerts(rub, m_cutBuffer, m_cutCapacity, m_cutVerts, kBoxFloats);
}

// Layer seams of the live fill (horizontal planes: elevation views only), with a faint fill between seams.
void GridViewRenderer::rebuildFillLines(QRhiResourceUpdateBatch *rub) {
  m_layerCount = 0;
  if (m_layerBounds.size() < 2 || !rub)
    return;

  const qreal dpr = qMax(1.0, m_devicePixelRatio);
  const double zoom = m_pxPerWorldUnit * dpr;
  if (zoom <= 1e-9)
    return;

  const ViewBasis vb = viewBasisFor(static_cast<int>(m_perspective));
  if (std::fabs(vb.up.y) < 0.5)
    return;

  glm::dvec3 mn {};
  glm::dvec3 mx {};
  double a0 = m_center.x - (m_viewWidth * 0.5) / m_pxPerWorldUnit;
  double a1 = m_center.x + (m_viewWidth * 0.5) / m_pxPerWorldUnit;
  if (g::engine3D && g::engine3D->fillExtent(mn, mx)) {
    projectOntoAxis(vb.right, mn, mx, a0, a1);
  }

  const float halfThick = float(1.4 / zoom);
  const glm::vec4 kLayerLine(0.25f, 0.85f, 0.95f, 1.0f);
  const glm::vec4 kLayerBand(0.20f, 0.65f, 0.80f, 0.10f);

  m_layerVerts.clear();
  const double sign = vb.up.y;

  for (size_t i = 0; i + 1 < m_layerBounds.size(); ++i) {
    const double b0 = m_layerBounds[i] * sign;
    const double b1 = m_layerBounds[i + 1] * sign;
    const double lo = std::min(b0, b1);
    const double hi = std::max(b0, b1);
    if (hi <= lo)
      continue;
    appendBoxQuad(m_layerVerts, glm::dvec4(a0, lo, a1, hi), kLayerBand);
  }

  for (const double bound: m_layerBounds) {
    const double b = bound * sign;
    appendWireQuad(m_layerVerts, glm::vec2(float(a0), float(b)), glm::vec2(float(a1), float(b)),
                  halfThick, kLayerLine);
  }

  m_layerCount = uploadVerts(rub, m_layerBuffer, m_layerCapacity, m_layerVerts, kBoxFloats);
}

// Entity markers as wireframe boxes (the list the 3D pass draws from).
void GridViewRenderer::rebuildEntityMarkers(QRhiResourceUpdateBatch *rub) {
  m_markerCount = 0;
  if ((m_markers.isEmpty() && m_editHandles.empty() && m_ioLinks.empty() && m_ioPulses.empty() && !m_mirrorOn
       && m_paths.empty() && m_penPoints.empty()) ||
      !rub)
    return;

  const qreal dpr = qMax(1.0, m_devicePixelRatio);
  const double zoom = m_pxPerWorldUnit * dpr;
  if (zoom <= 1e-9)
    return;

  const float halfThick = float(0.75 / zoom);
  const glm::vec3 right(1.0f, 0.0f, 0.0f);
  const glm::vec3 down(0.0f, -1.0f, 0.0f);
  const QColor white(255, 255, 255);
  m_text.setPixelSize(std::max(8, int(18.0 * dpr)));

  m_markerVerts.clear();
  // Connection lines first, so the marker boxes sit on top of their ends.
  appendIOLinks(zoom, halfThick);
  appendMirror(zoom);
  // Edit mode's rubber band: a thin outline.
  if (m_vertexBand) {
    const float px = float(1.0 / zoom);
    const glm::vec4 kBand(1.0f, 1.0f, 1.0f, 0.9f);
    const glm::dvec4 &r = *m_vertexBand;
    const glm::vec2 q[4] = {{float(r.x), float(r.y)}, {float(r.z), float(r.y)}, {float(r.z), float(r.w)},
                            {float(r.x), float(r.w)}};
    for (int k = 0; k < 4; ++k)
      appendWireQuad(m_markerVerts, q[k], q[(k + 1) & 3], px, kBand);
  }
  // The physics simulation's bodies, orange over their wire.
  {
    const float px = float(1.0 / zoom);
    const glm::vec4 kPhysics(1.0f, 0.62f, 0.20f, 1.0f);
    for (const glm::vec4 &e: m_physicsEdges)
      appendWireQuad(m_markerVerts, {e.x, e.y}, {e.z, e.w}, 1.25f * px, kPhysics);
  }
  appendPaths(zoom);
  // Markers a little heavier than the brush wire; selected ones heavier still, on the selection fill.
  const glm::vec4 kEntity(1.0f, 1.0f, 1.0f, 1.0f);
  const glm::vec4 kKnobRim(1.0f, 1.0f, 1.0f, 1.0f), kKnob(0.0f, 0.0f, 0.0f, 1.0f);
  const float lineHalf = float(kMarkerLineHalfPx / zoom);
  const double markerSize = markerScale(m_pxPerWorldUnit / GridView::kUnitsPerWorldUnit);
  for (const EntityMarker &m: m_markers) {
    const glm::dvec4 r = markerRectOnPlane(m, m_perspective, zoom, markerSize);
    const float thick = m.selected ? lineHalf * 1.6f : lineHalf;
    if (m.selected)
      appendBoxQuad(m_markerVerts, r, kSelectionFill);
    const glm::vec2 c[4] = {
        {float(r.x), float(r.y)},
        {float(r.z), float(r.y)},
        {float(r.z), float(r.w)},
        {float(r.x), float(r.w)},
    };
    for (int k = 0; k < 4; ++k)
      appendWireQuad(m_markerVerts, c[k], c[(k + 1) & 3], thick, kEntity);

    // Facing arrow: path anchors always, oriented entities while selected (with the knob).
    const bool knob = m.rotatable && m.selected && m_entityKnobs;
    if (m.facingArrow || knob) {
      const MarkerArrow a = markerArrow(m, m_perspective, zoom);
      const glm::vec2 c0(a.centre), d(a.dir), n(-d.y, d.x);
      if (!a.flat) {
        const glm::vec2 tip(a.tip(zoom));
        const float head = float(kMarkerArrowHeadPx / zoom);
        appendWireQuad(m_markerVerts, c0, tip, thick, kEntity);
        appendWireQuad(m_markerVerts, tip, tip - d * head + n * head * 0.6f, thick, kEntity);
        appendWireQuad(m_markerVerts, tip, tip - d * head - n * head * 0.6f, thick, kEntity);
      }
      if (knob) {
        const glm::vec2 k(a.knob(zoom));
        if (a.flat)
          appendWireQuad(m_markerVerts, c0, k, lineHalf, kEntity);
        // Black with a white rim, like the rotate handles of a selection.
        appendDisc(m_markerVerts, k, float((kKnobRadiusPx + 1.5) / zoom), kKnobRim);
        appendDisc(m_markerVerts, k, float(kKnobRadiusPx / zoom), kKnob);
        if (m.id == m_entRotId) {
          m_text.setPixelSize(std::max(8, int(kMarkerTextPx * dpr)));
          const QString text =
              QStringLiteral("%1°").arg(m_entRotAngle, 0, 'f',
                                        std::fabs(m_entRotAngle - std::round(m_entRotAngle)) < 1e-6 ? 0 : 1);
          const QSize sprite = m_text.size(text);
          const float w = float(double(sprite.width()) / zoom), h = float(double(sprite.height()) / zoom);
          const float gap = float((kKnobRadiusPx + 6.0) / zoom);
          m_text.drawDecal(text, glm::vec3(k.x + gap, k.y + h * 0.5f, 0.0f), right, down, w, h,
                           QColor(255, 220, 90), 1.0f);
        }
      }
    }

    if (!m.selected)
      continue;

    // The range keeps its true scale.
    if (m.range > 0.0f && m.coneAngle > 0.0f) {
      // A spot light: its cone (rim and eight edges from the apex).
      const PlaneBasis basis = basisFor(m_perspective);
      auto toPlane = [&basis](const glm::vec3 &p) {
        return glm::vec2(float(glm::dot(basis.right, p)), float(glm::dot(basis.up, p)));
      };
      static thread_local std::vector<glm::vec3> ring;
      spotConeRing(m, 48, ring);
      const glm::vec2 apex = toPlane(m.position);
      for (size_t k = 0; k < ring.size(); ++k) {
        appendWireQuad(m_markerVerts, toPlane(ring[k]), toPlane(ring[(k + 1) % ring.size()]), lineHalf, kEntity);
        if (k % 6 == 0)
          appendWireQuad(m_markerVerts, apex, toPlane(ring[k]), lineHalf, kEntity);
      }
    } else if (m.range > 0.0f) {
      appendCircleWire(m_markerVerts,
                      glm::vec2(float((r.x + r.z) * 0.5), float((r.y + r.w) * 0.5)), m.range,
                      lineHalf, kEntity);
    }

    if (m.label.isEmpty())
      continue;

    // A little above the marker box (the decal origin is the glyph's top-left, laid out along -b).
    m_text.setPixelSize(std::max(8, int(kMarkerTextPx * dpr)));
    const QSize sprite = m_text.size(m.label);
    const float w = float(double(sprite.width()) / zoom);
    const float h = float(double(sprite.height()) / zoom);
    const float gap = float(8.0 / zoom);
    m_text.drawDecal(m.label,
                    glm::vec3(float((r.x + r.z) * 0.5 - w * 0.5), float(r.w + gap + h), 0.0f),
                    right, down, w, h, white, 1.0f);
  }
  m_text.setPixelSize(std::max(8, int(18.0 * dpr)));

  // Vertex tool handles: fixed-size squares in Hammer colours, the scale pivot as a ring.
  if (!m_editHandles.empty()) {
    const ViewBasis bs = viewBasisFor(static_cast<int>(m_perspective));
    const double half = 3.5 / zoom;
    for (const auto &h: m_editHandles) {
      const glm::dvec3 p(h.pos);
      const double a = glm::dot(p, bs.right), b = glm::dot(p, bs.up);
      glm::vec4 col(0.92f, 0.95f, 1.0f, 1.0f);
      double hs = half;
      switch (Engine3D::EditHandleKind(h.kind)) {
        case Engine3D::EditHandleKind::Edge: col = glm::vec4(1.0f, 0.86f, 0.2f, 1.0f); hs = half * 0.85; break;
        case Engine3D::EditHandleKind::Face: col = glm::vec4(0.3f, 0.9f, 1.0f, 1.0f); break;
        case Engine3D::EditHandleKind::Centre: {
          // The brush centre "x".
          const float r = float(half * 1.2);
          const glm::vec4 xc(0.95f, 0.95f, 0.95f, 0.75f);
          appendWireQuad(m_markerVerts, {float(a) - r, float(b) - r}, {float(a) + r, float(b) + r}, halfThick, xc);
          appendWireQuad(m_markerVerts, {float(a) - r, float(b) + r}, {float(a) + r, float(b) - r}, halfThick, xc);
          continue;
        }
        case Engine3D::EditHandleKind::Pivot:
          appendCircleWire(m_markerVerts, {float(a), float(b)}, float(half * 3.0), halfThick * 1.5f,
                           glm::vec4(1.0f, 0.3f, 0.9f, 1.0f));
          appendBoxQuad(m_markerVerts, {a - half * 0.6, b - half * 0.6, a + half * 0.6, b + half * 0.6},
                        glm::vec4(1.0f, 0.3f, 0.9f, 1.0f));
          continue;
        default: break;
      }
      if (h.selected)
        col = glm::vec4(1.0f, 0.22f, 0.18f, 1.0f);
      // A dark rim keeps a white handle readable on top of white wire lines.
      appendBoxQuad(m_markerVerts, {a - hs - 1.0 / zoom, b - hs - 1.0 / zoom, a + hs + 1.0 / zoom, b + hs + 1.0 / zoom},
                    glm::vec4(0.05f, 0.06f, 0.1f, 0.9f));
      appendBoxQuad(m_markerVerts, {a - hs, b - hs, a + hs, b + hs}, col);
    }
  }

  m_markerCount = uploadVerts(rub, m_markerBuffer, m_markerCapacity, m_markerVerts, kBoxFloats);
}

// Mirror: shown planes seen edge-on (dashed for the hover preview), linked-brush edges while the mode is on,
// dashed outlines of the selection's twins.
void GridViewRenderer::appendMirror(const double zoom) {
  if (!m_mirrorOn)
    return;
  const ViewBasis bs = viewBasisFor(static_cast<int>(m_perspective));
  const float px = float(1.0 / zoom);
  const glm::vec4 kLine(0.95f, 0.35f, 0.85f, 1.0f), kLinked(0.95f, 0.35f, 0.85f, 0.75f),
      kTwin(1.0f, 0.55f, 0.95f, 1.0f);
  auto dashed = [&](const glm::vec2 &a, const glm::vec2 &b, const float thick, const glm::vec4 &c) {
    const float len = glm::length(b - a);
    const float on = 8.0f * px, period = 13.0f * px;
    if (len < 1e-6f)
      return;
    const glm::vec2 dir = (b - a) / len;
    for (float t = 0.0f; t < len; t += period)
      appendWireQuad(m_markerVerts, a + dir * t, a + dir * std::min(len, t + on), thick, c);
  };
  // A world box dropped onto this view: (a0, b0, a1, b1).
  auto rectOf = [&](const glm::dvec3 &mn, const glm::dvec3 &mx) {
    const double a0 = glm::dot(mn, bs.right), a1 = glm::dot(mx, bs.right);
    const double b0 = glm::dot(mn, bs.up), b1 = glm::dot(mx, bs.up);
    return glm::dvec4(std::min(a0, a1), std::min(b0, b1), std::max(a0, a1), std::max(b0, b1));
  };
  auto outline = [&](const glm::dvec4 &r, const float thick, const glm::vec4 &c) {
    const glm::vec2 q[4] = {{float(r.x), float(r.y)}, {float(r.z), float(r.y)}, {float(r.z), float(r.w)},
                            {float(r.x), float(r.w)}};
    for (int k = 0; k < 4; ++k)
      dashed(q[k], q[(k + 1) & 3], thick, c);
  };

  for (const Engine3D::MirrorShownPlane &sp: m_mirrorPlanes) {
    const int i = sp.plane.axis;
    const double halfW = (m_viewWidth * 0.5) / m_pxPerWorldUnit, halfH = (m_viewHeight * 0.5) / m_pxPerWorldUnit;
    const float l = float(m_center.x - halfW), r = float(m_center.x + halfW);
    const float b = float(m_center.y - halfH), t = float(m_center.y + halfH);
    glm::vec2 p0, p1;
    bool edgeOn = true;
    if (std::fabs(bs.right[i]) > 0.5) {
      const float a = float(sp.plane.d * bs.right[i]);
      p0 = {a, b};
      p1 = {a, t};
    } else if (std::fabs(bs.up[i]) > 0.5) {
      const float v = float(sp.plane.d * bs.up[i]);
      p0 = {l, v};
      p1 = {r, v};
    } else {
      edgeOn = false; // the plane faces this view
    }
    if (edgeOn) {
      if (sp.preview)
        dashed(p0, p1, 1.5f * px, kLine);
      else
        appendWireQuad(m_markerVerts, p0, p1, 1.5f * px, kLine);
    }
  }
  for (const glm::vec4 &e: m_mirrorLinked)
    appendWireQuad(m_markerVerts, {e.x, e.y}, {e.z, e.w}, px, kLinked);
  for (const auto &[mn, mx]: m_mirrorTwins)
    outline(rectOf(mn, mx), 1.5f * px, kTwin);
  for (const auto &poly: m_mirrorTwinFaces)
    for (size_t k = 0; k < poly.size(); ++k) {
      const glm::dvec3 &wa = poly[k], &wb = poly[(k + 1) % poly.size()];
      dashed({float(glm::dot(wa, bs.right)), float(glm::dot(wa, bs.up))},
             {float(glm::dot(wb, bs.right)), float(glm::dot(wb, bs.up))}, 1.5f * px, kTwin);
    }
}

// Paths: curves (red where a follower cannot be built), start dot, chevron; edited points / handles; the pen.
void GridViewRenderer::appendPaths(const double zoom) {
  if (m_paths.empty() && m_penPoints.empty())
    return;
  const ViewBasis bs = viewBasisFor(static_cast<int>(m_perspective));
  const float px = float(1.0 / zoom);
  auto v2 = [&](const glm::dvec3 &w) { return glm::vec2(float(glm::dot(w, bs.right)), float(glm::dot(w, bs.up))); };
  const glm::vec4 kPath(0.30f, 0.80f, 1.00f, 0.85f), kSel(0.55f, 0.95f, 1.00f, 1.0f), kErr(1.0f, 0.32f, 0.32f, 1.0f),
      kConnect(0.75f, 0.60f, 1.00f, 0.9f), kHandle(0.85f, 0.85f, 0.90f, 0.9f), kPoint(1.0f, 1.0f, 1.0f, 1.0f),
      kPointSel(1.0f, 0.62f, 0.15f, 1.0f), kRim(0.05f, 0.06f, 0.1f, 0.9f);
  for (const Engine3D::PathDraw &d: m_paths) {
    if (d.polyline.size() < 2)
      continue;
    const glm::vec4 col = d.error ? kErr : d.selected ? kSel : d.connect ? kConnect : kPath;
    const float thick = (d.selected ? 1.6f : 1.0f) * px;
    for (size_t i = 0; i + 1 < d.polyline.size(); ++i) {
      bool red = false;
      if (i < d.polylineS.size())
        for (const auto &[a, b]: d.errorSpans)
          red = red || (d.polylineS[i] >= a && d.polylineS[i] <= b);
      appendWireQuad(m_markerVerts, v2(d.polyline[i]), v2(d.polyline[i + 1]), red ? thick * 1.8f : thick,
                     red ? kErr : col);
    }
    // Start dot and a chevron half way.
    const glm::vec2 s0 = v2(d.polyline.front());
    appendBoxQuad(m_markerVerts, {s0.x - 3.0 * px, s0.y - 3.0 * px, s0.x + 3.0 * px, s0.y + 3.0 * px}, col);
    const size_t mid = d.polyline.size() / 2;
    const glm::vec2 a = v2(d.polyline[mid]), b = v2(d.polyline[std::min(mid + 1, d.polyline.size() - 1)]);
    if (glm::length(b - a) > 1e-6f) {
      const glm::vec2 dir = glm::normalize(b - a), nrm(-dir.y, dir.x);
      const float sz = 7.0f * px;
      appendWireQuad(m_markerVerts, a, a - dir * sz + nrm * sz * 0.6f, thick, col);
      appendWireQuad(m_markerVerts, a, a - dir * sz - nrm * sz * 0.6f, thick, col);
    }
    if (!d.editing)
      continue;
    for (const auto &pt: d.points) {
      const glm::vec2 c = v2(pt.pos);
      for (const glm::dvec3 &h: {pt.in, pt.out}) {
        const glm::vec2 hc = v2(h);
        if (glm::length(hc - c) < 1e-5f)
          continue;
        appendWireQuad(m_markerVerts, c, hc, 0.6f * px, kHandle);
        appendBoxQuad(m_markerVerts, {hc.x - 2.5 * px, hc.y - 2.5 * px, hc.x + 2.5 * px, hc.y + 2.5 * px}, kHandle);
      }
      const double hs = 4.0 * px;
      appendBoxQuad(m_markerVerts, {c.x - hs - px, c.y - hs - px, c.x + hs + px, c.y + hs + px}, kRim);
      appendBoxQuad(m_markerVerts, {c.x - hs, c.y - hs, c.x + hs, c.y + hs}, pt.selected ? kPointSel : kPoint);
    }
  }
  // The pen: its unfinished path and points.
  for (size_t i = 0; i + 1 < m_penPoly.size(); ++i)
    appendWireQuad(m_markerVerts, v2(m_penPoly[i]), v2(m_penPoly[i + 1]), 1.2f * px, kSel);
  for (const glm::dvec3 &w: m_penPoints) {
    const glm::vec2 c = v2(w);
    const double hs = 3.5 * px;
    appendBoxQuad(m_markerVerts, {c.x - hs - px, c.y - hs - px, c.x + hs + px, c.y + hs + px}, kRim);
    appendBoxQuad(m_markerVerts, {c.x - hs, c.y - hs, c.x + hs, c.y + hs}, kPoint);
  }
}

// Entity I/O links: arrowheads, dashes for delayed / Target links, a red stub when broken, simulator pulses.
void GridViewRenderer::appendIOLinks(const double zoom, const float halfThick) {
  if (m_ioLinks.empty() && m_ioPulses.empty())
    return;
  const ViewBasis bs = viewBasisFor(static_cast<int>(m_perspective));
  auto onPlane = [&](const glm::vec3 &p) {
    const glm::dvec3 d(p);
    return glm::vec2(float(glm::dot(d, bs.right)), float(glm::dot(d, bs.up)));
  };
  const float px = float(1.0 / zoom);
  const glm::vec4 kOut(0.31f, 0.82f, 1.0f, 1.0f), kIn(1.0f, 0.62f, 0.25f, 1.0f), kOther(0.45f, 0.72f, 0.95f, 0.45f),
      kRef(0.78f, 0.78f, 0.85f, 0.7f), kBroken(1.0f, 0.35f, 0.35f, 1.0f), kPulse(1.0f, 0.95f, 0.45f, 1.0f);
  auto dashed = [&](const glm::vec2 &a, const glm::vec2 &b, float thick, const glm::vec4 &c) {
    const float len = glm::length(b - a);
    const float on = 7.0f * px, period = 12.0f * px;
    if (len < 1e-6f)
      return;
    const glm::vec2 dir = (b - a) / len;
    for (float t = 0.0f; t < len; t += period)
      appendWireQuad(m_markerVerts, a + dir * t, a + dir * std::min(len, t + on), thick, c);
  };
  for (const IOLink &l: m_ioLinks) {
    const bool sel = l.selectedSide != 0;
    const glm::vec4 col = l.kind == IOLink::Reference ? kRef
        : l.kind == IOLink::Broken                  ? kBroken
        : l.selectedSide == 1                       ? kOut
        : l.selectedSide == 2                       ? kIn
                                                    : kOther;
    const float thick = sel ? halfThick * 1.8f : halfThick;
    const glm::vec2 a = onPlane(l.a);
    if (l.kind == IOLink::Broken) {
      const glm::vec2 b = a + glm::vec2(0.0f, 22.0f * px);
      appendWireQuad(m_markerVerts, a, b, thick, col);
      appendCircleWire(m_markerVerts, b + glm::vec2(0.0f, 4.0f * px), 3.5f * px, thick, col);
      continue;
    }
    const glm::vec2 b = onPlane(l.b);
    const float len = glm::length(b - a);
    if (len < 2.0f * px) // seen end-on in this view
      continue;
    const glm::vec2 dir = (b - a) / len;
    const glm::vec2 tip = b - dir * std::min(len * 0.4f, 8.0f * px);
    if (l.delayed || l.kind == IOLink::Reference)
      dashed(a, tip, thick, col);
    else
      appendWireQuad(m_markerVerts, a, tip, thick, col);
    // Arrowhead: two short strokes back from the tip.
    const float ah = (sel ? 10.0f : 7.0f) * px;
    const glm::vec2 n(-dir.y, dir.x);
    appendWireQuad(m_markerVerts, tip, tip - dir * ah + n * (ah * 0.5f), thick, col);
    appendWireQuad(m_markerVerts, tip, tip - dir * ah - n * (ah * 0.5f), thick, col);
  }
  for (const IOPulse &p: m_ioPulses) {
    const glm::vec2 c = onPlane(glm::mix(p.a, p.b, p.t));
    const double s = 4.0 / zoom;
    appendBoxQuad(m_markerVerts, {c.x - s, c.y - s, c.x + s, c.y + s}, kPulse);
  }
}

QQuickRhiItemRenderer *GridView::createRenderer() {
  return new GridViewRenderer();
}

} // namespace vk
