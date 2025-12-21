// Rotate and scale modes of the 3D gizmo. Drags preview through object transforms; on release the preview is
// undone and the edit is baked into the brush planes.

#include <cfloat>

#include <QGuiApplication>

#include <glm/gtc/matrix_transform.hpp>

#include "vk_engine.h"

#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

glm::vec3 axisOf(int i) {
  glm::vec3 v(0.0f);
  v[i] = 1.0f;
  return v;
}

const glm::vec3 kColors[3] = {{0.95f, 0.28f, 0.28f}, {0.35f, 0.85f, 0.30f}, {0.30f, 0.55f, 1.00f}};

bool hitPlane(const glm::vec3 &ro, const glm::vec3 &rd, const glm::vec3 &n, const glm::vec3 &p, glm::vec3 &out) {
  const float denom = glm::dot(n, rd);
  if (std::fabs(denom) < 1e-6f)
    return false;
  const float t = glm::dot(p - ro, n) / denom;
  if (t < 0.0f)
    return false;
  out = ro + rd * t;
  return true;
}

bool hitSphere(const glm::vec3 &ro, const glm::vec3 &rd, const glm::vec3 &c, float r, float &t) {
  const glm::vec3 oc = ro - c;
  const float b = glm::dot(oc, rd);
  const float cc = glm::dot(oc, oc) - r * r;
  const float disc = b * b - cc;
  if (disc < 0.0f)
    return false;
  t = -b - std::sqrt(disc);
  return t > 0.0f;
}

constexpr float kScaleHandleDist = 1.35f;
constexpr float kScaleHandleSize = 0.075f;

} // namespace

void Engine3D::setGizmoMode(const GizmoMode mode) {
  if (m_gizmoMode == mode)
    return;
  if (isGizmoDragging())
    endGizmoDrag();
  m_gizmoMode = mode;
  m_gizmoHover = GizmoHandle::None;
  emit gizmoModeChanged();
  requestRedraw();
}

Engine3D::GizmoHandle Engine3D::gizmoHitTestModes(const float x, const float y) const {
  const float viewW = std::max(1.0f, float(m_lastViewSize.width()));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  glm::vec3 ro, rd;
  makeRay(x, y, viewW, viewH, ro, rd);
  const glm::vec3 c = gizmoCenter();
  const float s = gizmoScale(c);

  if (m_gizmoMode == GizmoMode::Rotate) {
    // The ring passing closest to the cursor on screen.
    GizmoHandle best = GizmoHandle::None;
    float bestPx = 14.0f;
    const glm::vec2 cursor(x, y);
    for (int i = 0; i < 3; ++i) {
      glm::vec3 p;
      if (!hitPlane(ro, rd, axisOf(i), c, p))
        continue;
      const glm::vec3 d = p - c;
      if (glm::length(d) < 1e-6f)
        continue;
      const glm::vec3 onRing = c + glm::normalize(d) * s;
      const float px = glm::length(projectToScreen(onRing) - cursor);
      if (px < bestPx) {
        bestPx = px;
        best = static_cast<GizmoHandle>(int(GizmoHandle::RingX) + i);
      }
    }
    return best;
  }

  // Scale: the centre cube first (it sits under the axis lines), then the axes.
  float t = 0.0f;
  if (hitSphere(ro, rd, c, kScaleHandleSize * 2.2f * s, t))
    return GizmoHandle::ScaleUniform;
  GizmoHandle best = GizmoHandle::None;
  float bestT = FLT_MAX;
  for (int i = 0; i < 3; ++i) {
    const glm::vec3 g = c + axisOf(i) * kScaleHandleDist * s;
    if (hitSphere(ro, rd, g, kScaleHandleSize * 2.5f * s, t) && t < bestT) {
      bestT = t;
      best = static_cast<GizmoHandle>(int(GizmoHandle::ScaleX) + i);
    }
  }
  return best;
}

void Engine3D::addGizmoModeDraws(
    const std::function<void(const EntityGeom &, int, const glm::mat4 &, const glm::vec4 &)> &add) {
  const glm::vec3 c = gizmoCenter();
  const float s = gizmoScale(c);
  const auto hotColor = [](const glm::vec3 &col) { return glm::mix(col, glm::vec3(1.0f), 0.55f); };
  const int overlay = int(EntityPass::TriOverlay);
  if (m_gizmoMode == GizmoMode::Rotate) {
    const glm::mat4 base = glm::translate(glm::mat4(1.0f), c) * glm::scale(glm::mat4(1.0f), glm::vec3(s));
    for (int i = 0; i < 3; ++i) {
      const auto h = static_cast<GizmoHandle>(int(GizmoHandle::RingX) + i);
      const bool hot = m_gizmoHover == h || m_gizmoDrag == h;
      add(m_geoRing[i], overlay, base, glm::vec4(hot ? hotColor(kColors[i]) : kColors[i], hot ? 1.0f : 0.85f));
    }
    return;
  }
  // Scale: the translate arrows' shafts (thin), cubes at the ends.
  for (int i = 0; i < 3; ++i) {
    const auto h = static_cast<GizmoHandle>(int(GizmoHandle::ScaleX) + i);
    const bool hot = m_gizmoHover == h || m_gizmoDrag == h || m_gizmoDrag == GizmoHandle::ScaleUniform;
    const glm::vec4 col(hot ? hotColor(kColors[i]) : kColors[i], 1.0f);
    const glm::mat4 shaft = glm::translate(glm::mat4(1.0f), c) * glm::scale(glm::mat4(1.0f), glm::vec3(s * 0.82f));
    add(m_geoArrow[i], overlay, shaft, glm::vec4(glm::vec3(col), 0.6f));
    const glm::vec3 at = c + axisOf(i) * kScaleHandleDist * s;
    const glm::mat4 cube = glm::translate(glm::mat4(1.0f), at) * glm::scale(glm::mat4(1.0f), glm::vec3(kScaleHandleSize * s));
    add(m_geoMarkerBox, overlay, cube, col);
  }
  const bool hotU = m_gizmoHover == GizmoHandle::ScaleUniform || m_gizmoDrag == GizmoHandle::ScaleUniform;
  const glm::mat4 centre = glm::translate(glm::mat4(1.0f), c) * glm::scale(glm::mat4(1.0f), glm::vec3(kScaleHandleSize * 1.2f * s));
  add(m_geoMarkerBox, overlay, centre, glm::vec4(hotU ? glm::vec3(1.0f) : glm::vec3(0.85f), 0.9f));
}

bool Engine3D::beginGizmoModeDrag(const GizmoHandle handle, const float x, const float y) {
  if (!m_scene)
    return false;
  const float viewW = std::max(1.0f, float(m_lastViewSize.width()));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  makeRay(x, y, viewW, viewH, m_gizmoClickOrigin, m_gizmoClickDir);
  captureGizmoModeDrag(handle);
  if (handle >= GizmoHandle::RingX && handle <= GizmoHandle::RingZ) {
    const int a = int(handle) - int(GizmoHandle::RingX);
    glm::vec3 p;
    if (!hitPlane(m_gizmoClickOrigin, m_gizmoClickDir, axisOf(a), m_gizmoDragCenter, p))
      p = m_gizmoDragCenter + axisOf((a + 1) % 3);
    m_gizmoClickVec = p - m_gizmoDragCenter;
  }
  return true;
}

// The selection at the start of a rotate / scale drag: previews offset from it, the commit starts from it.
void Engine3D::captureGizmoModeDrag(const GizmoHandle handle) {
  m_gizmoDrag = handle;
  m_gizmoHover = handle;
  m_gizmoDragCenter = gizmoCenter();
  m_gizmoAngle = 0.0;
  m_gizmoScaleFactor = glm::vec3(1.0f);

  m_gizmoMeshIds.clear();
  m_gizmoMeshBasePos.clear();
  m_gizmoBaseRot.clear();
  m_gizmoBaseScale.clear();
  for (const quint32 id: m_selectedObjects) {
    const auto obj = m_scene->findById(id);
    if (!obj || obj->locked())
      continue;
    m_gizmoMeshIds.push_back(id);
    m_gizmoMeshBasePos.push_back(obj->getPosition());
    m_gizmoBaseRot.push_back(obj->getRotation());
    m_gizmoBaseScale.push_back(obj->getScale());
  }
  captureFaceUv(m_gizmoMeshIds, m_gizmoBaseUv);
  beginMirrorPreview(m_gizmoMeshIds);
  m_gizmoEntities = selectedPointEntityPoses();
  glm::vec3 mn, mx;
  if (selectionBounds(mn, mx)) {
    m_gizmoBoundsMin = mn;
    m_gizmoBoundsMax = mx;
  }
}

void Engine3D::toggleRotateMode() {
  setGizmoMode(m_gizmoMode == GizmoMode::Rotate ? GizmoMode::Translate : GizmoMode::Rotate);
}

bool Engine3D::beginRotate2D(const int plane, const glm::dvec2 &p) {
  m_rot2D = {};
  if (!m_scene || (m_selectedObjects.empty() && m_selectedEntities.empty()))
    return false;
  const ViewBasis bs = viewBasisFor(plane);
  const glm::dvec3 n = glm::cross(glm::dvec3(bs.right), glm::dvec3(bs.up)); // out of the view
  int axis = 0;
  for (int i = 1; i < 3; ++i)
    if (std::fabs(n[i]) > std::fabs(n[axis]))
      axis = i;
  captureGizmoModeDrag(GizmoHandle(int(GizmoHandle::RingX) + axis));
  const glm::dvec3 c(m_gizmoDragCenter);
  m_rot2D.active = true;
  m_rot2D.axis = axis;
  m_rot2D.sign = n[axis] >= 0.0 ? 1.0 : -1.0;
  m_rot2D.centre = {glm::dot(c, glm::dvec3(bs.right)), glm::dot(c, glm::dvec3(bs.up))};
  const glm::dvec2 v = p - m_rot2D.centre;
  m_rot2D.startVec = glm::length(v) > 1e-9 ? v : glm::dvec2(1.0, 0.0);
  requestRedraw();
  return true;
}

void Engine3D::updateRotate2D(const glm::dvec2 &p) {
  if (!m_rot2D.active)
    return;
  const glm::dvec2 v0 = m_rot2D.startVec, v1 = p - m_rot2D.centre;
  if (glm::length(v1) < 1e-9)
    return;
  const double screen = snapRotationAngle(glm::degrees(std::atan2(v0.x * v1.y - v0.y * v1.x, glm::dot(v0, v1))));
  if (std::fabs(screen - m_rot2D.screenAngle) < 1e-9 && std::fabs(screen) > 0.0)
    return;
  m_rot2D.screenAngle = screen;
  previewGizmoRotation(m_rot2D.axis, screen * m_rot2D.sign);
}

void Engine3D::endRotate2D() {
  if (!m_rot2D.active)
    return;
  endGizmoModeDrag();
  m_gizmoDrag = GizmoHandle::None;
  m_gizmoHover = GizmoHandle::None;
  m_rot2D = {};
  requestRedraw();
}

bool Engine3D::entityRotatable(const eng::Entity &e) {
  const eng::EntityClass *c = e.cls();
  return c && c->oriented && c->kind != eng::EntityClass::Kind::PointLight
         && c->kind != eng::EntityClass::Kind::WorldEnv && !c->isBrushEntity();
}

bool Engine3D::beginEntityRotate2D(const int plane, const quint32 id, const glm::dvec2 &centre,
                                   const glm::dvec2 &dir) {
  m_entRot2D = {};
  if (!m_scene || !m_selectedEntities.count(id))
    return false;
  std::vector<EntityPose> poses;
  for (const EntityPose &p: selectedPointEntityPoses())
    if (const eng::Entity *e = entityById(p.id); e && entityRotatable(*e))
      poses.push_back(p);
  if (poses.empty())
    return false;
  const ViewBasis bs = viewBasisFor(plane);
  m_entRot2D.active = true;
  m_entRot2D.id = id;
  m_entRot2D.axis = glm::normalize(glm::cross(glm::dvec3(bs.right), glm::dvec3(bs.up)));
  m_entRot2D.centre = centre;
  m_entRot2D.startAngle = glm::length(dir) > 1e-9 ? glm::degrees(std::atan2(dir.y, dir.x)) : 0.0;
  m_entRot2D.poses = std::move(poses);
  return true;
}

void Engine3D::updateEntityRotate2D(const glm::dvec2 &p) {
  if (!m_entRot2D.active)
    return;
  const glm::dvec2 v = p - m_entRot2D.centre;
  if (glm::length(v) < 1e-9)
    return;
  // The knob follows the cursor, its screen direction snapped.
  double turn = snapRotationAngle(glm::degrees(std::atan2(v.y, v.x))) - m_entRot2D.startAngle;
  turn = std::remainder(turn, 360.0);
  if (std::fabs(turn - m_entRot2D.angle) < 1e-9)
    return;
  m_entRot2D.angle = turn;
  const glm::mat3 r(glm::rotate(glm::dmat4(1.0), glm::radians(turn), m_entRot2D.axis));
  for (const EntityPose &pose: m_entRot2D.poses)
    rotateEntityPoses({pose}, r, pose.origin, false);
  requestRedraw();
}

void Engine3D::endEntityRotate2D() {
  if (!m_entRot2D.active)
    return;
  const EntityRotate2D st = m_entRot2D;
  m_entRot2D = {};
  // Back to where the drag started; the committed turn starts from there.
  for (const EntityPose &pose: st.poses)
    if (const eng::Entity *e = entityById(pose.id))
      setEntityTransform(pose.id, pose.origin, pose.angles, e->name, false);
  if (std::fabs(st.angle) < 1e-6) {
    requestRedraw();
    return;
  }
  const glm::mat3 r(glm::rotate(glm::dmat4(1.0), glm::radians(st.angle), st.axis));
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Rotate %1°").arg(st.angle, 0, 'f', 1));
  for (const EntityPose &pose: st.poses)
    rotateEntityPoses({pose}, r, pose.origin, true);
  if (record)
    m_undoStack->endMacro();
  requestRedraw();
}

double Engine3D::snapRotationAngle(double degrees) const {
  // Ctrl turns freely.
  if (QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier) || !prefs::b("Grid/AngleSnapEnabled"))
    return degrees;
  const double step = std::max(0.1, prefs::d("Grid/AngleSnap"));
  return std::round(degrees / step) * step;
}

// Live rotate preview: `ang` degrees about world axis `a` through the drag centre.
void Engine3D::previewGizmoRotation(const int a, const double ang) {
  const glm::vec3 c = m_gizmoDragCenter;
  const glm::vec3 ax = axisOf(a);
  m_gizmoAngle = ang;
  // x' = R (x - c) + c, expressed through each object's transform.
  const glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(float(ang)), ax);
  for (size_t k = 0; k < m_gizmoMeshIds.size(); ++k) {
    const auto obj = m_scene->findById(m_gizmoMeshIds[k]);
    if (!obj)
      continue;
    glm::vec3 rot = m_gizmoBaseRot[k];
    rot[a] += float(ang);
    obj->setRotation(rot);
    obj->setPosition(glm::vec3(r * glm::vec4(m_gizmoMeshBasePos[k] - c, 1.0f)) + c);
  }
  // Face bases are world space: without this turned faces show smeared textures until release.
  applyFaceUvTransform(m_gizmoMeshIds, m_gizmoBaseUv, rotationAbout(a, ang, glm::dvec3(c)));
  rotateEntityPoses(m_gizmoEntities, glm::mat3(r), c, false);
  updateMirrorPreview();
  static const char *names[3] = {"X", "Y", "Z"};
  m_gizmoReadout = QStringLiteral("Rotate %1  %2°").arg(QLatin1String(names[a])).arg(ang, 0, 'f', 1);
  emit gizmoReadoutChanged(m_gizmoReadout);
  emit sceneChanged();
}

glm::dmat4 Engine3D::rotationAbout(const int a, const double degrees, const glm::dvec3 &c) {
  glm::dvec3 ax(0.0);
  ax[a] = 1.0;
  glm::dmat4 m = glm::translate(glm::dmat4(1.0), c) * glm::rotate(glm::dmat4(1.0), glm::radians(degrees), ax)
                 * glm::translate(glm::dmat4(1.0), -c);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (std::fabs(m[i][j]) < 1e-12)
        m[i][j] = 0.0;
  return m;
}

void Engine3D::updateGizmoModeDrag(const float x, const float y) {
  const float viewW = std::max(1.0f, float(m_lastViewSize.width()));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  glm::vec3 ro, rd;
  makeRay(x, y, viewW, viewH, ro, rd);
  const glm::vec3 c = m_gizmoDragCenter;

  if (m_gizmoDrag >= GizmoHandle::RingX && m_gizmoDrag <= GizmoHandle::RingZ) {
    const int a = int(m_gizmoDrag) - int(GizmoHandle::RingX);
    const glm::vec3 ax = axisOf(a);
    glm::vec3 p;
    if (!hitPlane(ro, rd, ax, c, p))
      return;
    const glm::vec3 v0 = m_gizmoClickVec, v1 = p - c;
    const double ang =
        glm::degrees(std::atan2(double(glm::dot(ax, glm::cross(v0, v1))), double(glm::dot(v0, v1))));
    previewGizmoRotation(a, snapRotationAngle(ang));
    return;
  }

  // Project the cursor onto the dragged axis (camera plane for uniform); distance from the centre gives the factor.
  const glm::vec3 size0 = m_gizmoBoundsMax - m_gizmoBoundsMin;
  glm::vec3 factor(1.0f);
  const glm::vec3 camN = -m_camera->getDirection();
  if (m_gizmoDrag == GizmoHandle::ScaleUniform) {
    glm::vec3 p0, p1;
    if (!hitPlane(m_gizmoClickOrigin, m_gizmoClickDir, camN, c, p0) || !hitPlane(ro, rd, camN, c, p1))
      return;
    const float d0 = std::max(1e-4f, glm::length(p0 - c));
    const float f = std::max(0.01f, glm::length(p1 - c) / d0);
    factor = glm::vec3(f);
  } else {
    const int a = int(m_gizmoDrag) - int(GizmoHandle::ScaleX);
    const glm::vec3 ax = axisOf(a);
    glm::vec3 n = camN - ax * glm::dot(camN, ax);
    if (glm::length(n) < 1e-4f)
      return;
    n = glm::normalize(n);
    glm::vec3 p0, p1;
    if (!hitPlane(m_gizmoClickOrigin, m_gizmoClickDir, n, c, p0) || !hitPlane(ro, rd, n, c, p1))
      return;
    const float t0 = glm::dot(p0 - c, ax), t1 = glm::dot(p1 - c, ax);
    if (std::fabs(t0) < 1e-5f)
      return;
    factor[a] = std::max(0.01f, t1 / t0);
  }
  const double step = gizmoSnapStep();
  for (int i = 0; i < 3; ++i) {
    if (factor[i] == 1.0f || size0[i] < 1e-5f)
      continue;
    double sz = size0[i] * factor[i];
    if (step > 0)
      sz = std::max(step, std::round(sz / step) * step);
    factor[i] = float(sz / size0[i]);
  }
  m_gizmoScaleFactor = factor;
  // Scale about the bounds' minimum corner so the brush stays on the grid; uniform scales about the centre.
  const glm::vec3 anchor = m_gizmoDrag == GizmoHandle::ScaleUniform ? c : m_gizmoBoundsMin;
  for (size_t k = 0; k < m_gizmoMeshIds.size(); ++k) {
    const auto obj = m_scene->findById(m_gizmoMeshIds[k]);
    if (!obj)
      continue;
    obj->setPosition(anchor + (m_gizmoMeshBasePos[k] - anchor) * factor);
    obj->setScale(m_gizmoBaseScale[k] * factor);
  }
  updateMirrorPreview();
  const glm::vec3 sz = size0 * factor * float(kSourceUnitsPerWorldUnit);
  m_gizmoReadout = QStringLiteral("Scale  %1 × %2 × %3").arg(sz.x, 0, 'f', 0).arg(sz.y, 0, 'f', 0).arg(sz.z, 0, 'f', 0);
  emit gizmoReadoutChanged(m_gizmoReadout);
  emit sceneChanged();
}

void Engine3D::endGizmoModeDrag() {
  const GizmoHandle handle = m_gizmoDrag;
  m_gizmoReadout.clear();
  emit gizmoReadoutChanged(QString());
  if (!m_scene)
    return;
  endMirrorPreview();
  // Put the preview back first; the committed edit starts from the original.
  for (size_t k = 0; k < m_gizmoMeshIds.size(); ++k)
    if (const auto obj = m_scene->findById(m_gizmoMeshIds[k])) {
      obj->setPosition(m_gizmoMeshBasePos[k]);
      obj->setRotation(m_gizmoBaseRot[k]);
      obj->setScale(m_gizmoBaseScale[k]);
    }
  restoreFaceUv(m_gizmoMeshIds, m_gizmoBaseUv);
  for (const EntityPose &p: m_gizmoEntities)
    if (const eng::Entity *e = entityById(p.id))
      setEntityTransform(p.id, p.origin, p.angles, e->name, false);

  if (handle >= GizmoHandle::RingX && handle <= GizmoHandle::RingZ) {
    if (std::fabs(m_gizmoAngle) < 1e-6)
      return;
    const int a = int(handle) - int(GizmoHandle::RingX);
    const glm::dvec3 c(m_gizmoDragCenter);
    const glm::dmat4 m = rotationAbout(a, m_gizmoAngle, c);
    const QString label = QStringLiteral("Rotate %1°").arg(m_gizmoAngle, 0, 'f', 1);
    const bool record = m_undoStack && !m_undoStack->isApplying();
    if (record)
      m_undoStack->beginMacro(label);
    const std::vector<EntityPose> entities = m_gizmoEntities;  // the brush edit may change the selection
    transformSelectionBrushes(m, label);
    rotateEntityPoses(entities, glm::mat3(m), glm::vec3(c), true);
    if (record)
      m_undoStack->endMacro();
    return;
  }

  if (m_gizmoScaleFactor == glm::vec3(1.0f))
    return;
  const glm::vec3 anchor = handle == GizmoHandle::ScaleUniform ? m_gizmoDragCenter : m_gizmoBoundsMin;
  const glm::vec3 newMin = anchor + (m_gizmoBoundsMin - anchor) * m_gizmoScaleFactor;
  const glm::vec3 newMax = anchor + (m_gizmoBoundsMax - anchor) * m_gizmoScaleFactor;
  setSelectionBounds(newMin, newMax);
}

std::vector<Engine3D::EntityPose> Engine3D::selectedPointEntityPoses() const {
  std::vector<EntityPose> out;
  for (const quint32 id: m_selectedEntities) {
    const eng::Entity *e = entityById(id);
    if (!e)
      continue;
    if (const eng::EntityClass *c = e->cls(); c && c->isBrushEntity())
      continue;
    out.push_back({id, e->origin, e->angles});
  }
  return out;
}

void Engine3D::rotateEntityPoses(const std::vector<EntityPose> &base, const glm::mat3 &r, const glm::vec3 &pivot,
                                 const bool record) {
  for (const EntityPose &p: base) {
    const eng::Entity *e = entityById(p.id);
    if (!e)
      continue;
    eng::Entity before = *e;
    before.origin = p.origin;
    before.angles = p.angles;
    const glm::vec3 origin = r * (p.origin - pivot) + pivot;
    const glm::vec3 angles = eng::entityAnglesFromRotation(before, r * eng::entityRotation(before));
    setEntityTransform(p.id, origin, angles, e->name, record);
  }
}

} // namespace vk
