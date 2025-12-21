// Interactive shape tools (stairs, arches) from the 2D marquee. While open, the brushes are a locked, unsaved
// preview rebuilt on every parameter change; Commit swaps it for the real thing in one undo step (ring / archway =
// a generated arch, carve = a cut).

#include <cmath>

#include "vk_engine.h"
#include "vk_shared.h"

#include "client/engine/arch_gen.h"
#include "client/engine/brush.h"
#include "client/engine/shape_gen.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {
constexpr int kStairs = int(eng::PrimitiveKind::Stairs);
constexpr int kArch = int(eng::PrimitiveKind::Arch);

eng::ArchParams archParamsOf(const int profile, const int mode, const int segments, const double thicknessSource,
                             const double springSource, const double rise, const bool smooth) {
  eng::ArchParams p;
  p.profile = static_cast<eng::ArchProfile>(profile);
  p.mode = static_cast<eng::ArchMode>(mode);
  p.segments = segments;
  p.thickness = thicknessSource / Engine3D::kSourceUnitsPerWorldUnit;
  p.spring = springSource / Engine3D::kSourceUnitsPerWorldUnit;
  p.rise = rise;
  p.smooth = smooth;
  return p;
}

int spanAxisFor(const MarqueeBox &m, const glm::dvec3 &mn, const glm::dvec3 &mx) {
  if (m.plane == 1)
    return 0; // front view: X across
  if (m.plane == 2)
    return 2; // side view: Z across
  return (mx.x - mn.x) >= (mx.z - mn.z) ? 0 : 2;
}
eng::ShapeKind shapeKindFor(const int kind) {
  return kind == Engine3D::kShapeCapsule   ? eng::ShapeKind::Capsule
         : kind == Engine3D::kShapePyramid ? eng::ShapeKind::Pyramid
                                           : eng::ShapeKind::Cylinder;
}

// Remembered per kind: "Brush/CylinderSides", "Brush/CapsuleSmooth", ...
QString shapePref(const eng::ShapeKind kind, const char *what) {
  const QString name = eng::shapeTypeName(kind);
  return QStringLiteral("Brush/%1%2").arg(name.left(1).toUpper() + name.mid(1), QLatin1String(what));
}

eng::ShapeParams shapeParamsFromPrefs(const eng::ShapeKind kind) {
  eng::ShapeParams p;
  p.kind = kind;
  p.sides = std::clamp(prefs::get().intValue(shapePref(kind, "Sides")), 3, 64);
  p.smooth = prefs::get().boolValue(shapePref(kind, "Smooth"));
  if (kind == eng::ShapeKind::Cylinder)
    p.wall = prefs::d("Brush/CylinderWall") / Engine3D::kSourceUnitsPerWorldUnit;
  if (kind == eng::ShapeKind::Capsule)
    p.capSegments = std::clamp(prefs::i("Brush/CapsuleCaps"), 1, 16);
  return p;
}

void shapeParamsToPrefs(const eng::ShapeParams &p) {
  prefs::get().setValue(shapePref(p.kind, "Sides"), p.sides);
  prefs::get().setValue(shapePref(p.kind, "Smooth"), p.smooth);
  if (p.kind == eng::ShapeKind::Cylinder)
    prefs::get().setValue(QStringLiteral("Brush/CylinderWall"), p.wall * Engine3D::kSourceUnitsPerWorldUnit);
  if (p.kind == eng::ShapeKind::Capsule)
    prefs::get().setValue(QStringLiteral("Brush/CapsuleCaps"), p.capSegments);
}
} // namespace

bool Engine3D::beginShapeTool(const int kind) {
  if (kind == kStairs)
    return stairsFromMarqueeOrSelection();
  if (!m_marquee.valid || (kind != kArch && !isShapeKind(kind)))
    return false;
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  if (!m_shapeToolConnected) {
    m_shapeToolConnected = true;
    // The marquee stays editable (resize it in any 2D view): follow it.
    connect(this, &Engine3D::marqueeChanged, this, [this] {
      if (!m_shapeTool.active)
        return;
      if (!m_marquee.valid)
        cancelShapeTool();
      else
        rebuildShapePreview();
    });
  }
  if (m_cutMode != CutMode::Off)
    exitCutMode();
  const bool wasActive = m_shapeTool.active;
  const int wasKind = m_shapeTool.kind;
  m_shapeTool.active = true;
  m_shapeTool.kind = kind;
  if (isShapeKind(kind) && (!wasActive || wasKind != kind)) {
    m_shapeTool.shape = shapeParamsFromPrefs(shapeKindFor(kind));
    // A cylinder / capsule runs along the axis the drawing view looks along (its circle shows there); a pyramid
    // points up.
    m_shapeTool.shape.axis = kind == kShapePyramid ? 1 : m_marquee.plane == 1 ? 2 : m_marquee.plane == 2 ? 0 : 1;
  }
  if (!wasActive) {
    m_shapeTool.steps = std::clamp(prefs::i("Brush/StairSteps"), 2, 64);
    m_shapeTool.style = std::clamp(prefs::i("Brush/StairStyle"), 0, 2);
    m_shapeTool.archSegments = std::clamp(prefs::i("Brush/ArchSegments"), 2, 64);
    m_shapeTool.archProfile = std::clamp(prefs::i("Brush/ArchProfile"), 0, 4);
    m_shapeTool.archMode = std::clamp(prefs::i("Brush/ArchMode"), 0, 2);
    m_shapeTool.archThicknessSource = prefs::d("Brush/ArchThickness");
    m_shapeTool.archJambSource = prefs::d("Brush/ArchJamb");
    m_shapeTool.archSmooth = prefs::b("Brush/ArchSmooth");
    // Stairs drawn in a front or side view climb to the right on screen.
    if (m_marquee.plane == 1)
      m_shapeTool.direction = 0;
    else if (m_marquee.plane == 2)
      m_shapeTool.direction = 3;
    else
      m_shapeTool.direction = std::clamp(prefs::i("Brush/StairDirection"), 0, 3);
    // Archway in a selected wall: carving is the useful default.
    if (kind == kArch && !m_selectedObjects.empty())
      m_shapeTool.archMode = int(eng::ArchMode::Opening);
  }
  rebuildShapePreview();
  emit shapeToolChanged();
  return true;
}

std::vector<std::shared_ptr<eng::GameObject>> Engine3D::buildShapeToolObjects() const {
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  if (!m_marquee.valid)
    return objs;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  if (mx.x - mn.x < 1e-4 || mx.y - mn.y < 1e-4 || mx.z - mn.z < 1e-4)
    return objs;
  double tpu = 0.0;
  const auto mat = newBrushMaterial(tpu);
  std::vector<eng::ConvexBrush> brushes;
  if (m_shapeTool.kind == kStairs) {
    eng::PrimitiveParams p;
    p.steps = m_shapeTool.steps;
    p.stairDirection = m_shapeTool.direction;
    p.stairStyle = m_shapeTool.style;
    p.up = 1;
    brushes = eng::buildPrimitive(eng::PrimitiveKind::Stairs, mn, mx, p, mat, tpu);
  } else if (isShapeKind(m_shapeTool.kind)) {
    brushes = eng::buildShape(mn, mx, m_shapeTool.shape, mat, tpu);
  } else {
    const eng::ArchParams p = archParamsOf(m_shapeTool.archProfile, m_shapeTool.archMode, m_shapeTool.archSegments,
                                           archThicknessSource(), m_shapeTool.archSpringSource,
                                           m_shapeTool.archRise, m_shapeTool.archSmooth);
    brushes = eng::buildArch(mn, mx, spanAxisFor(m_marquee, mn, mx), p, mat, tpu);
  }
  for (const auto &b: brushes)
    if (auto o = objectFromBrush(b)) {
      if (m_shapeTool.kind == kArch && m_shapeTool.archSmooth && m_shapeTool.archMode != 2)
        o->setSmoothShading(true);
      if (isShapeKind(m_shapeTool.kind) && m_shapeTool.shape.smooth)
        o->setSmoothShading(true);
      objs.push_back(o);
    }
  return objs;
}

void Engine3D::removeShapePreview() {
  if (m_shapePreviewIds.empty() || !m_scene)
    return;
  m_scene->removeObjects(m_shapePreviewIds);
  m_shapePreviewIds.clear();
  notifyStructureChanged();
}

void Engine3D::rebuildShapePreview() {
  if (!m_scene)
    return;
  if (!m_shapePreviewIds.empty()) {
    m_scene->removeObjects(m_shapePreviewIds);
    m_shapePreviewIds.clear();
  }
  auto objs = buildShapeToolObjects();
  for (auto &o: objs) {
    o->setLocked(true); // not pickable while it is only a preview
    o->setName(QStringLiteral("(preview)"));
    m_shapePreviewIds.insert(o->stableId());
  }
  if (!objs.empty())
    m_scene->addObjects(objs);
  notifyStructureChanged();
  emit shapeToolChanged();
  requestRedraw();
}

void Engine3D::cancelShapeTool() {
  if (!m_shapeTool.active)
    return;
  m_shapeTool.active = false;
  removeShapePreview();
  emit shapeToolChanged();
  requestRedraw();
}

bool Engine3D::commitShapeTool() {
  if (!m_shapeTool.active)
    return false;
  removeShapePreview();
  auto objs = buildShapeToolObjects();
  m_shapeTool.active = false;
  bool ok = false;
  if (m_shapeTool.kind == kArch && m_shapeTool.archMode == int(eng::ArchMode::Opening)) {
    // Carve out of the selected brushes it touches; with nothing selected, whatever it overlaps.
    std::vector<eng::ConvexBrush> cutters;
    for (const auto &o: objs)
      if (auto b = eng::ConvexBrush::fromObject(*o)) {
        // No material on the cutter: the faces the cut exposes then take the
        // wall's own (split() copies the most parallel face).
        for (auto &f: b->faces)
          f.material.reset();
        cutters.push_back(std::move(*b));
      }
    std::vector<quint32> targets;
    if (!m_selectedObjects.empty()) {
      targets.assign(m_selectedObjects.begin(), m_selectedObjects.end());
    } else {
      for (const auto &o: m_scene->gameObjects) {
        if (o->locked() || o->hidden() || o->isDerived())
          continue;
        targets.push_back(o->stableId());
      }
    }
    std::vector<quint32> removed;
    std::vector<std::shared_ptr<eng::GameObject>> added;
    for (const quint32 id: targets) {
      const auto obj = m_scene->findById(id);
      if (!obj)
        continue;
      auto brush = eng::ConvexBrush::fromObject(*obj);
      if (!brush)
        continue;
      bool touched = false;
      std::vector<eng::ConvexBrush> pieces {*brush};
      for (const auto &c: cutters) {
        std::vector<eng::ConvexBrush> next;
        for (const auto &p: pieces) {
          if (!p.overlaps(c)) {
            next.push_back(p);
            continue;
          }
          touched = true;
          for (auto &frag: p.subtract(c))
            next.push_back(std::move(frag));
        }
        pieces = std::move(next);
      }
      if (!touched)
        continue;
      removed.push_back(id);
      for (const auto &p: pieces)
        if (auto o = objectFromBrush(p, obj.get()))
          added.push_back(o);
    }
    if (!removed.empty()) {
      replaceObjects(removed, added, QStringLiteral("Carve Arch"));
      ok = true;
    }
  } else if (!objs.empty() && m_shapeTool.kind == kArch) {
    // A generated arch: selecting it later brings its parameters back.
    glm::dvec3 mn, mx;
    marqueeToWorld(m_marquee, mn, mx);
    const eng::ArchParams p = archParamsOf(m_shapeTool.archProfile, m_shapeTool.archMode, m_shapeTool.archSegments,
                                           archThicknessSource(), m_shapeTool.archSpringSource,
                                           m_shapeTool.archRise, m_shapeTool.archSmooth);
    const int span = spanAxisFor(m_marquee, mn, mx);
    ok = withMirror(QStringLiteral("Create Arch"), [&] { return createArch(mn, mx, span, p) != 0; });
  } else if (!objs.empty() && isShapeKind(m_shapeTool.kind)) {
    glm::dvec3 mn, mx;
    marqueeToWorld(m_marquee, mn, mx);
    const eng::ShapeParams p = m_shapeTool.shape;
    ok = withMirror(QStringLiteral("Create Shape"), [&] { return createShape(mn, mx, p) != 0; });
  } else if (!objs.empty()) {
    const QString label = QStringLiteral("Create Stairs");
    ok = withMirror(label, [&] {
      replaceObjects({}, objs, label);
      return true;
    });
  }
  // Remember the settings for next time.
  if (isShapeKind(m_shapeTool.kind)) {
    shapeParamsToPrefs(m_shapeTool.shape);
  } else if (m_shapeTool.kind == kStairs) {
    prefs::get().setValue(QStringLiteral("Brush/StairSteps"), m_shapeTool.steps);
    prefs::get().setValue(QStringLiteral("Brush/StairStyle"), m_shapeTool.style);
    prefs::get().setValue(QStringLiteral("Brush/StairDirection"), m_shapeTool.direction);
  } else {
    prefs::get().setValue(QStringLiteral("Brush/ArchSegments"), m_shapeTool.archSegments);
    prefs::get().setValue(QStringLiteral("Brush/ArchProfile"), m_shapeTool.archProfile);
    prefs::get().setValue(QStringLiteral("Brush/ArchMode"), m_shapeTool.archMode);
    prefs::get().setValue(QStringLiteral("Brush/ArchThickness"), m_shapeTool.archThicknessSource);
    prefs::get().setValue(QStringLiteral("Brush/ArchJamb"), m_shapeTool.archJambSource);
    prefs::get().setValue(QStringLiteral("Brush/ArchSmooth"), m_shapeTool.archSmooth);
  }
  clearMarquee();
  emit shapeToolChanged();
  requestRedraw();
  return ok;
}

void Engine3D::setStairSteps(const int n) {
  const int v = std::clamp(n, 2, 64);
  if (v == m_shapeTool.steps)
    return;
  m_shapeTool.steps = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setStairDirection(const int d) {
  const int v = std::clamp(d, 0, 3);
  if (v == m_shapeTool.direction)
    return;
  m_shapeTool.direction = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setStairStyle(const int s) {
  const int v = std::clamp(s, 0, 2);
  if (v == m_shapeTool.style)
    return;
  m_shapeTool.style = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

double Engine3D::stairStepHeightSource() const {
  if (!m_marquee.valid)
    return 0.0;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  return (mx.y - mn.y) * kSourceUnitsPerWorldUnit / std::max(1, m_shapeTool.steps);
}

double Engine3D::stairStepDepthSource() const {
  if (!m_marquee.valid)
    return 0.0;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  const int axis = m_shapeTool.direction <= 1 ? 0 : 2;
  return (mx[axis] - mn[axis]) * kSourceUnitsPerWorldUnit / std::max(1, m_shapeTool.steps);
}

void Engine3D::setArchProfile(const int p) {
  const int v = std::clamp(p, 0, 4);
  if (v == m_shapeTool.archProfile)
    return;
  m_shapeTool.archProfile = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setArchMode(const int m) {
  const int v = std::clamp(m, 0, 2);
  if (v == m_shapeTool.archMode)
    return;
  m_shapeTool.archMode = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setShapeToolParams(const eng::ShapeParams &p) {
  eng::ShapeParams v = p;
  v.kind = m_shapeTool.shape.kind;
  v.sides = std::clamp(v.sides, 3, 64);
  v.axis = std::clamp(v.axis, 0, 2);
  v.wall = std::max(0.0, v.wall);
  v.capSegments = std::clamp(v.capSegments, 1, 16);
  m_shapeTool.shape = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setArchSmooth(const bool on) {
  if (on == m_shapeTool.archSmooth)
    return;
  m_shapeTool.archSmooth = on;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setArchSegments(const int n) {
  const int v = std::clamp(n, 2, 64);
  if (v == m_shapeTool.archSegments)
    return;
  m_shapeTool.archSegments = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setArchThicknessSource(const double t) {
  const double v = std::clamp(t, 0.0, 512.0);
  double &field = m_shapeTool.archMode == 1 ? m_shapeTool.archJambSource : m_shapeTool.archThicknessSource;
  if (std::fabs(v - field) < 1e-9)
    return;
  field = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setArchSpringSource(const double t) {
  const double v = std::clamp(t, 0.0, 4096.0);
  if (std::fabs(v - m_shapeTool.archSpringSource) < 1e-9)
    return;
  m_shapeTool.archSpringSource = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

void Engine3D::setArchRise(const double r) {
  const double v = std::clamp(r, 0.05, 1.0);
  if (std::fabs(v - m_shapeTool.archRise) < 1e-9)
    return;
  m_shapeTool.archRise = v;
  if (m_shapeTool.active)
    rebuildShapePreview();
  emit shapeToolChanged();
}

double Engine3D::archWidthSource() const {
  if (!m_marquee.valid)
    return 0.0;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  const int a = spanAxisFor(m_marquee, mn, mx);
  return (mx[a] - mn[a]) * kSourceUnitsPerWorldUnit;
}

double Engine3D::archHeightSource() const {
  if (!m_marquee.valid)
    return 0.0;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  return (mx.y - mn.y) * kSourceUnitsPerWorldUnit;
}

} // namespace vk
