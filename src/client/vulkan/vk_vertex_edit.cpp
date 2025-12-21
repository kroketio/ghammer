// Edit mode: the vertex tool.
// Handles: corners (white), edge midpoints (yellow), face centres (cyan) and a centre "x" per brush, filtered by
// the sub-mode. The selection is a set of corners; an edge or face handle stands for its corners.
// Every edit runs beginVertexEdit (snapshot) -> previewVertexEdit (live rebuild from a corner -> position map via
// eng::moveBrushVertices) -> finishVertexEdit (restore the snapshot, or swap the brushes in one undo step).

#include <cfloat>
#include <cmath>

#include <QGuiApplication>
#include <QLoggingCategory>

#include "vk_engine.h"

#include "client/engine/brush.h"
#include "client/engine/scene.h"
#include "client/engine/vertex_ops.h"
#include "client/prefs/preferences.h"

namespace vk {

static QLoggingCategory vertexLog("vk.vertex", QtInfoMsg);

namespace {
constexpr float kHandlePx = 7.0f;
constexpr float kSamePos = 1e-3f;

bool samePos(const glm::vec3 &a, const glm::vec3 &b) { return glm::distance(a, b) < kSamePos; }

glm::dvec2 onPlane(const glm::vec3 &p, int plane) {
  const ViewBasis bs = viewBasisFor(plane);
  return {glm::dot(glm::dvec3(p), bs.right), glm::dot(glm::dvec3(p), bs.up)};
}

double snapValue(double v, double step) { return step > 0.0 ? std::round(v / step) * step : v; }
} // namespace

void Engine3D::setEditMode(const bool on) {
  if (m_editMode == on)
    return;
  if (!on) {
    if (m_vertexScaleActive)
      cancelVertexScale();
    if (m_vertexEditOpen)
      finishVertexEdit(false, {});
  }
  m_editMode = on;
  m_selectedVerts.clear();
  touchVertexEdit();
  emit selectionChanged();
  requestRedraw();
}

void Engine3D::setVertexSubMode(const int mode) {
  const int m = std::clamp(mode, 0, 3);
  if (m == m_vertexSub)
    return;
  m_vertexSub = m;
  touchVertexEdit();
  requestRedraw();
}

void Engine3D::cycleVertexSubMode() { setVertexSubMode((m_vertexSub + 1) % 4); }

void Engine3D::touchVertexEdit() {
  ++m_vertexEpoch;
  emit vertexToolChanged();
}

std::vector<Engine3D::VertexHandle> Engine3D::vertexHandles() const {
  std::vector<VertexHandle> out;
  for (const EditHandle &h: editHandles())
    if (h.kind == EditHandleKind::Vertex)
      out.push_back({h.brush, h.pos});
  return out;
}

std::vector<Engine3D::EditHandle> Engine3D::editHandles() const {
  std::vector<EditHandle> out;
  if (!m_editMode || !m_scene)
    return out;
  const bool verts = m_vertexSub == SubVertsEdges || m_vertexSub == SubVerts;
  const bool edges = m_vertexSub == SubVertsEdges || m_vertexSub == SubEdges;
  const bool faces = m_vertexSub == SubFaces;
  int brushes = 0;
  for (const quint32 id: m_selectedObjects) {
    if (++brushes > 64)
      break; // handles for a huge selection would be unreadable and slow
    const auto obj = m_scene->findById(id);
    if (!obj || obj->isDerived() || isShapePreview(id))
      continue;
    const auto b = eng::ConvexBrush::fromObject(*obj);
    if (!b)
      continue;
    if (verts)
      for (const auto &v: b->vertices())
        out.push_back({EditHandleKind::Vertex, id, glm::vec3(v), {glm::vec3(v)}});
    if (edges)
      for (const auto &[a, c]: eng::brushEdges(*b))
        out.push_back({EditHandleKind::Edge, id, glm::vec3((a + c) * 0.5), {glm::vec3(a), glm::vec3(c)}});
    if (faces)
      for (const auto &poly: b->polygons) {
        glm::dvec3 c(0.0);
        EditHandle h {EditHandleKind::Face, id, {}, {}};
        for (const auto &q: poly) {
          c += q;
          h.verts.push_back(glm::vec3(q));
        }
        h.pos = glm::vec3(c / double(std::max<size_t>(1, poly.size())));
        out.push_back(std::move(h));
      }
    out.push_back({EditHandleKind::Centre, id, glm::vec3(b->centroid()), {}});
  }
  return out;
}

std::vector<Engine3D::EditHandleDraw> Engine3D::editHandleDraws() const {
  std::vector<EditHandleDraw> out;
  for (const EditHandle &h: editHandles())
    out.push_back({h.pos, int(h.kind), handleSelected(h), false});
  if (m_vertexScaleActive)
    out.push_back({m_vertexPivot, int(EditHandleKind::Pivot), true, false});
  return out;
}

bool Engine3D::vertexSelected(const quint32 brush, const glm::vec3 &p) const {
  for (const auto &s: m_selectedVerts)
    if (s.brush == brush && samePos(s.pos, p))
      return true;
  return false;
}

bool Engine3D::handleSelected(const EditHandle &h) const {
  if (h.verts.empty())
    return false;
  for (const auto &v: h.verts)
    if (!vertexSelected(h.brush, v))
      return false;
  return true;
}

namespace {
// Applies a click on a group of handles: plain replaces (unless the handle is
// already selected, so a drag can carry the whole selection), additive toggles.
void applyHandleClick(std::vector<Engine3D::VertexHandle> &sel, const std::vector<Engine3D::EditHandle> &hits,
                      const bool additive, const std::function<bool(const Engine3D::EditHandle &)> &isSelected) {
  bool allSelected = true;
  for (const auto &h: hits)
    allSelected = allSelected && isSelected(h);
  auto remove = [&sel](quint32 brush, const glm::vec3 &p) {
    sel.erase(std::remove_if(sel.begin(), sel.end(),
                             [&](const Engine3D::VertexHandle &s) { return s.brush == brush && samePos(s.pos, p); }),
              sel.end());
  };
  auto add = [&sel](quint32 brush, const glm::vec3 &p) {
    for (const auto &s: sel)
      if (s.brush == brush && samePos(s.pos, p))
        return;
    sel.push_back({brush, p});
  };
  if (additive) {
    for (const auto &h: hits)
      for (const auto &v: h.verts)
        allSelected ? remove(h.brush, v) : add(h.brush, v);
    return;
  }
  if (allSelected)
    return;
  sel.clear();
  for (const auto &h: hits)
    for (const auto &v: h.verts)
      add(h.brush, v);
}
} // namespace

bool Engine3D::pickVertexHandle(const float x, const float y, const bool additive) {
  if (!m_editMode)
    return false;
  const auto handles = editHandles();
  int best = -1;
  float bestScore = kHandlePx * 1.8f;
  const glm::vec2 cursor(x, y);
  const glm::vec3 camFwd = m_camera->getDirection();
  for (size_t i = 0; i < handles.size(); ++i) {
    if (glm::dot(handles[i].pos - m_camera->getPosition(), camFwd) <= 0.0f)
      continue;
    // Corners beat edges beat faces beat the centre when they are close.
    const float d = glm::length(projectToScreen(handles[i].pos) - cursor) + float(int(handles[i].kind)) * 1.5f;
    if (d < bestScore) {
      bestScore = d;
      best = int(i);
    }
  }
  if (best < 0)
    return false;
  const EditHandle &h = handles[size_t(best)];
  if (h.kind == EditHandleKind::Centre) {
    // The "x": edit this brush (Ctrl/Shift adds or removes it).
    std::vector<quint32> objs;
    if (additive) {
      objs.assign(m_selectedObjects.begin(), m_selectedObjects.end());
      auto it = std::find(objs.begin(), objs.end(), h.brush);
      if (it != objs.end() && objs.size() > 1)
        objs.erase(it);
      else if (it == objs.end())
        objs.push_back(h.brush);
    } else {
      objs = {h.brush};
    }
    std::vector<VertexHandle> keep;
    for (const auto &v: m_selectedVerts)
      if (std::find(objs.begin(), objs.end(), v.brush) != objs.end())
        keep.push_back(v);
    setSelected(objs, {});
    m_selectedVerts = keep;
  } else {
    applyHandleClick(m_selectedVerts, {h}, additive, [this](const EditHandle &e) { return handleSelected(e); });
  }
  touchVertexEdit();
  emit selectionChanged();
  requestRedraw();
  return true;
}

bool Engine3D::pickEditHandle2D(const int plane, const glm::dvec2 &p, const double tol, const bool additive,
                                EditHandle *hit) {
  if (!m_editMode)
    return false;
  const auto handles = editHandles();
  int best = -1;
  double bestScore = tol;
  for (size_t i = 0; i < handles.size(); ++i) {
    if (handles[i].kind == EditHandleKind::Centre)
      continue; // centres overlap their own corners in 2D; switch brushes in 3D or by clicking the brush
    const double d = glm::length(onPlane(handles[i].pos, plane) - p) + double(int(handles[i].kind)) * tol * 0.15;
    if (d < bestScore) {
      bestScore = d;
      best = int(i);
    }
  }
  if (best < 0)
    return false;
  // Everything stacked behind the picked handle in this view goes with it.
  const glm::dvec2 at = onPlane(handles[size_t(best)].pos, plane);
  std::vector<EditHandle> hits;
  for (const auto &h: handles)
    if (h.kind == handles[size_t(best)].kind && glm::length(onPlane(h.pos, plane) - at) < 1e-4)
      hits.push_back(h);
  applyHandleClick(m_selectedVerts, hits, additive, [this](const EditHandle &e) { return handleSelected(e); });
  if (hit)
    *hit = handles[size_t(best)];
  touchVertexEdit();
  emit selectionChanged();
  requestRedraw();
  return true;
}

int Engine3D::selectHandlesInBox(const int plane, const glm::dvec2 &a, const glm::dvec2 &b, const bool additive) {
  if (!m_editMode)
    return 0;
  // A hair of slack: a corner exactly on the band's edge is inside.
  const glm::dvec2 lo = glm::min(a, b) - glm::dvec2(1e-3), hi = glm::max(a, b) + glm::dvec2(1e-3);
  if (!additive)
    m_selectedVerts.clear();
  // Edges-only / faces-only boxes pick whole edges / faces; otherwise corners.
  const EditHandleKind want = m_vertexSub == SubEdges   ? EditHandleKind::Edge
                              : m_vertexSub == SubFaces ? EditHandleKind::Face
                                                        : EditHandleKind::Vertex;
  int n = 0;
  for (const EditHandle &h: editHandles()) {
    if (h.kind != want)
      continue;
    const glm::dvec2 q = onPlane(h.pos, plane);
    if (q.x < lo.x || q.x > hi.x || q.y < lo.y || q.y > hi.y)
      continue;
    for (const auto &v: h.verts)
      if (!vertexSelected(h.brush, v)) {
        m_selectedVerts.push_back({h.brush, v});
        ++n;
      }
  }
  touchVertexEdit();
  emit selectionChanged();
  requestRedraw();
  return n;
}

void Engine3D::selectAllVertices() {
  if (!m_editMode)
    return;
  m_selectedVerts.clear();
  for (const EditHandle &h: editHandles())
    for (const auto &v: h.verts)
      if (!vertexSelected(h.brush, v))
        m_selectedVerts.push_back({h.brush, v});
  touchVertexEdit();
  emit selectionChanged();
  requestRedraw();
}

void Engine3D::clearVertexSelection() {
  if (m_selectedVerts.empty())
    return;
  m_selectedVerts.clear();
  touchVertexEdit();
  emit selectionChanged();
  requestRedraw();
}

glm::vec3 Engine3D::selectedVertexCentre() const {
  glm::vec3 c(0.0f);
  for (const auto &v: m_selectedVerts)
    c += v.pos;
  return m_selectedVerts.empty() ? c : c / float(m_selectedVerts.size());
}

void Engine3D::remapSelectedVertices() {
  if (!m_scene || m_selectedVerts.empty())
    return;
  std::vector<VertexHandle> out;
  for (const auto &v: m_selectedVerts) {
    if (m_scene->findById(v.brush) && m_selectedObjects.count(v.brush)) {
      out.push_back(v);
      continue;
    }
    // The brush was swapped by an undo / redo: find a selected brush with a corner there.
    for (const quint32 id: m_selectedObjects) {
      const auto obj = m_scene->findById(id);
      const auto b = obj ? eng::ConvexBrush::fromObject(*obj) : std::nullopt;
      if (!b)
        continue;
      bool found = false;
      for (const auto &c: b->vertices())
        if (samePos(glm::vec3(c), v.pos)) {
          out.push_back({id, v.pos});
          found = true;
          break;
        }
      if (found)
        break;
    }
  }
  m_selectedVerts = out;
  touchVertexEdit();
}

void Engine3D::beginVertexEdit() {
  if (m_vertexEditOpen)
    finishVertexEdit(false, {});
  m_vertexDragBase = m_selectedVerts;
  m_vertexDragOriginal.clear();
  m_vertexDragMeshes.clear();
  m_vertexMapper = nullptr;
  m_vertexExtra.clear();
  m_vertexAbsorbed = 0;
  std::set<quint32> brushes;
  for (const auto &v: m_selectedVerts)
    brushes.insert(v.brush);
  for (const quint32 id: brushes) {
    const auto obj = m_scene ? m_scene->findById(id) : nullptr;
    if (!obj)
      continue;
    if (auto b = eng::ConvexBrush::fromObject(*obj)) {
      m_vertexDragOriginal.emplace(id, *b);
      m_vertexDragMeshes[id] = {obj->getMesh(), obj->getPosition(), obj->getRotation(), obj->getScale()};
    }
  }
  m_vertexEditOpen = true;
  beginMirrorPreview({brushes.begin(), brushes.end()});
}

bool Engine3D::buildEditedBrush(const quint32 brushId, eng::ConvexBrush &out, int *absorbed) const {
  const auto it = m_vertexDragOriginal.find(brushId);
  if (it == m_vertexDragOriginal.end())
    return false;
  const eng::ConvexBrush &orig = it->second;
  const std::vector<glm::dvec3> oldV = orig.vertices();
  std::vector<glm::dvec3> newV = oldV;
  if (m_vertexMapper)
    for (size_t i = 0; i < oldV.size(); ++i) {
      bool selected = false;
      for (const auto &s: m_vertexDragBase)
        if (s.brush == brushId && samePos(glm::vec3(oldV[i]), s.pos)) {
          selected = true;
          break;
        }
      if (selected)
        newV[i] = m_vertexMapper(brushId, oldV[i]);
    }
  const auto ex = m_vertexExtra.find(brushId);
  const std::vector<glm::dvec3> extra = ex != m_vertexExtra.end() ? ex->second : std::vector<glm::dvec3> {};
  auto res = eng::moveBrushVertices(orig, newV, extra, m_textureLock);
  if (!res.brush)
    return false;
  if (absorbed)
    *absorbed += res.absorbed;
  out = std::move(*res.brush);
  return true;
}

void Engine3D::previewVertexEdit(const VertexMapper &mapper, const std::map<quint32, std::vector<glm::dvec3>> &extra) {
  if (!m_vertexEditOpen || !m_scene)
    return;
  m_vertexMapper = mapper;
  m_vertexExtra = extra;
  m_vertexAbsorbed = 0;
  for (const auto &[id, original]: m_vertexDragOriginal) {
    eng::ConvexBrush moved;
    if (!buildEditedBrush(id, moved, &m_vertexAbsorbed))
      continue; // the move would break the brush: keep the last valid shape
    const auto obj = m_scene->findById(id);
    if (!obj)
      continue;
    // Preview in place: the object shows the edited hull in world space.
    obj->setMesh(moved.toMesh());
    obj->setPosition(glm::vec3(0.0f));
    obj->setRotation(glm::vec3(0.0f));
    obj->setScale(glm::vec3(1.0f));
    auto mesh = obj->getMesh();
    for (int f = 0; f < mesh->faceCount(); ++f)
      mesh->setFacePickingID(f, eng::makePickingId(obj->stableId(), uint32_t(f)));
  }
  // The selection follows its corners.
  m_selectedVerts = m_vertexDragBase;
  if (mapper)
    for (auto &v: m_selectedVerts)
      v.pos = glm::vec3(mapper(v.brush, glm::dvec3(v.pos)));
  updateMirrorPreview();
  invalidateDerivedCaches();
  touchVertexEdit();
  emit sceneChanged();
  requestRedraw();
}

bool Engine3D::finishVertexEdit(const bool commit, const QString &label) {
  if (!m_vertexEditOpen)
    return false;
  m_vertexEditOpen = false;
  endMirrorPreview();
  std::vector<eng::ConvexBrush> finals;
  std::vector<quint32> ids;
  int absorbed = 0;
  for (const auto &[id, original]: m_vertexDragOriginal) {
    eng::ConvexBrush moved;
    const bool ok = commit && (m_vertexMapper || !m_vertexExtra.empty()) && buildEditedBrush(id, moved, &absorbed);
    // Restore the original first; the commit swaps whole objects.
    if (const auto obj = m_scene ? m_scene->findById(id) : nullptr; obj && m_vertexDragMeshes.count(id)) {
      const auto &st = m_vertexDragMeshes[id];
      obj->setMesh(st.mesh);
      obj->setPosition(st.pos);
      obj->setRotation(st.rot);
      obj->setScale(st.scale);
    }
    if (ok) {
      finals.push_back(std::move(moved));
      ids.push_back(id);
    }
  }
  const auto mapper = m_vertexMapper;
  const auto base = m_vertexDragBase;
  const auto extra = m_vertexExtra;
  m_vertexDragOriginal.clear();
  m_vertexDragMeshes.clear();
  m_vertexDragBase.clear();
  m_vertexMapper = nullptr;
  m_vertexExtra.clear();
  invalidateDerivedCaches();

  if (commit && absorbed > 0 && prefs::b("Brush/VertexStrict")) {
    qCWarning(vertexLog).noquote() << QStringLiteral("[vertex] edit refused: %1 corner(s) would fold into the brush "
                                                     "(Preferences > Brushes > strict vertex editing)")
                                          .arg(absorbed);
    m_selectedVerts = base;
    touchVertexEdit();
    emit sceneChanged();
    return false;
  }
  if (!ids.empty()) {
    if (absorbed > 0)
      qCWarning(vertexLog).noquote()
          << QStringLiteral("[vertex] %1 corner(s) folded into the hull: brushes stay convex").arg(absorbed);
    std::vector<std::shared_ptr<eng::GameObject>> add;
    for (size_t i = 0; i < ids.size(); ++i)
      if (auto o = objectFromBrush(finals[i], m_scene->findById(ids[i]).get()))
        add.push_back(o);
    const bool record = m_undoStack && !m_undoStack->isApplying();
    if (record)
      m_undoStack->beginMacro(label);
    replaceObjects(ids, add, label);
    // Keep the corner selection on the new objects so the next edit continues.
    m_selectedVerts.clear();
    for (const auto &v: base)
      for (size_t i = 0; i < ids.size() && i < add.size(); ++i)
        if (v.brush == ids[i])
          m_selectedVerts.push_back({add[i]->stableId(), mapper ? glm::vec3(mapper(v.brush, glm::dvec3(v.pos))) : v.pos});
    // A split selects the new corner.
    for (const auto &[bid, pts]: extra)
      for (size_t i = 0; i < ids.size() && i < add.size(); ++i)
        if (bid == ids[i])
          for (const auto &p: pts)
            m_selectedVerts.push_back({add[i]->stableId(), glm::vec3(p)});
    remapSelectedVertices();
    if (record) {
      // The corner selection is part of the step: undo inside the vertex tool
      // brings back the corners that were selected before the edit.
      const auto after = m_selectedVerts;
      auto apply = [this](const std::vector<VertexHandle> &v) {
        m_selectedVerts = v;
        touchVertexEdit();
      };
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, base] { apply(base); },
                                                             [apply, after] { apply(after); }));
      m_undoStack->endMacro();
    }
  } else {
    m_selectedVerts = base;
  }
  touchVertexEdit();
  emit sceneChanged();
  emit selectionChanged();
  requestRedraw();
  return !ids.empty();
}

bool Engine3D::applyVertexEditNow(const VertexMapper &mapper, const QString &label,
                                  const std::map<quint32, std::vector<glm::dvec3>> &extra) {
  if (!m_editMode || m_selectedVerts.empty())
    return false;
  beginVertexEdit();
  previewVertexEdit(mapper, extra);
  return finishVertexEdit(true, label);
}

// gizmo drag (3D view)

void Engine3D::beginVertexDrag() { beginVertexEdit(); }

void Engine3D::updateVertexDrag(const glm::vec3 &delta) {
  const glm::dvec3 d(delta);
  previewVertexEdit([d](quint32, const glm::dvec3 &p) { return p + d; });
}

void Engine3D::endVertexDrag(const glm::vec3 &delta) {
  if (glm::length(delta) < 1e-6f) {
    finishVertexEdit(false, {});
    return;
  }
  const glm::dvec3 d(delta);
  m_vertexMapper = [d](quint32, const glm::dvec3 &p) { return p + d; };
  finishVertexEdit(true, QStringLiteral("Move vertices"));
}

void Engine3D::beginVertexPlaneDrag(const int plane, const glm::dvec2 &p, const double step, const glm::vec3 &anchor,
                                    std::optional<std::pair<quint32, glm::vec3>> splitFrom) {
  m_vertexPlaneDrag = {};
  m_vertexPlaneDrag.active = true;
  m_vertexPlaneDrag.plane = plane;
  m_vertexPlaneDrag.start = p;
  m_vertexPlaneDrag.anchor = anchor;
  m_vertexPlaneDrag.step = step;
  m_vertexEditPlane = plane;
  if (splitFrom) {
    m_vertexPlaneDrag.split = true;
    m_vertexPlaneDrag.splitBrush = splitFrom->first;
    m_vertexPlaneDrag.splitFrom = splitFrom->second;
    // The split brush must be in the snapshot even though none of its corners move.
    const auto keep = m_selectedVerts;
    m_selectedVerts = {{splitFrom->first, splitFrom->second}};
    beginVertexEdit();
    m_vertexDragBase.clear();
    m_selectedVerts = keep;
  } else {
    beginVertexEdit();
  }
}

void Engine3D::updateVertexPlaneDrag(const glm::dvec2 &p) {
  if (!m_vertexPlaneDrag.active)
    return;
  const ViewBasis bs = viewBasisFor(m_vertexPlaneDrag.plane);
  const glm::dvec2 a0 = onPlane(m_vertexPlaneDrag.anchor, m_vertexPlaneDrag.plane);
  const bool free = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::AltModifier);
  const double step = free ? 0.0 : m_vertexPlaneDrag.step;
  const glm::dvec2 target = a0 + (p - m_vertexPlaneDrag.start);
  const glm::dvec2 snapped(snapValue(target.x, step), snapValue(target.y, step));
  const glm::dvec2 dd = snapped - a0;
  const glm::dvec3 d = dd.x * bs.right + dd.y * bs.up;
  if (glm::length(glm::vec3(d) - m_vertexPlaneDrag.delta) < 1e-7f && glm::length(d) > 0.0)
    return;
  m_vertexPlaneDrag.delta = glm::vec3(d);
  if (m_vertexPlaneDrag.split) {
    std::map<quint32, std::vector<glm::dvec3>> extra;
    if (glm::length(d) > 1e-9)
      extra[m_vertexPlaneDrag.splitBrush] = {glm::dvec3(m_vertexPlaneDrag.splitFrom) + d};
    previewVertexEdit(nullptr, extra);
  } else {
    previewVertexEdit([d](quint32, const glm::dvec3 &q) { return q + d; });
  }
}

void Engine3D::endVertexPlaneDrag() {
  if (!m_vertexPlaneDrag.active)
    return;
  const bool moved = glm::length(m_vertexPlaneDrag.delta) > 1e-7f;
  const bool split = m_vertexPlaneDrag.split;
  m_vertexPlaneDrag.active = false;
  finishVertexEdit(moved, split ? QStringLiteral("Split edge") : QStringLiteral("Move vertices"));
}

void Engine3D::nudgeVertices(const glm::vec3 &delta) {
  const glm::dvec3 d(delta);
  applyVertexEditNow([d](quint32, const glm::dvec3 &p) { return p + d; }, QStringLiteral("Nudge vertices"));
}

bool Engine3D::weldSelectedVertices() {
  std::map<quint32, std::pair<glm::dvec3, int>> sums;
  for (const auto &v: m_selectedVerts) {
    auto &s = sums[v.brush];
    s.first += glm::dvec3(v.pos);
    ++s.second;
  }
  bool any = false;
  for (const auto &[id, s]: sums)
    any = any || s.second >= 2;
  if (!any)
    return false;
  std::map<quint32, glm::dvec3> centre;
  for (const auto &[id, s]: sums)
    centre[id] = s.first / double(s.second);
  const bool ok = applyVertexEditNow(
      [centre](quint32 b, const glm::dvec3 &p) {
        const auto it = centre.find(b);
        return it != centre.end() ? it->second : p;
      },
      QStringLiteral("Weld vertices"));
  if (ok) {
    // One corner per brush is left; drop the duplicates.
    std::vector<VertexHandle> uniq;
    for (const auto &v: m_selectedVerts)
      if (std::none_of(uniq.begin(), uniq.end(), [&v](const VertexHandle &u) { return u.brush == v.brush && samePos(u.pos, v.pos); }))
        uniq.push_back(v);
    m_selectedVerts = uniq;
    touchVertexEdit();
  }
  return ok;
}

bool Engine3D::snapSelectedVertices() {
  const double step = gizmoSnapStep();
  return applyVertexEditNow(
      [step](quint32, const glm::dvec3 &p) {
        return glm::dvec3(snapValue(p.x, step), snapValue(p.y, step), snapValue(p.z, step));
      },
      QStringLiteral("Snap vertices"));
}

bool Engine3D::splitSelectedEdges(double bulge) {
  if (!m_editMode || !m_scene)
    return false;
  if (bulge <= 0.0)
    bulge = gizmoSnapStep();
  std::map<quint32, std::vector<glm::dvec3>> extra;
  for (const quint32 id: m_selectedObjects) {
    const auto obj = m_scene->findById(id);
    const auto b = obj ? eng::ConvexBrush::fromObject(*obj) : std::nullopt;
    if (!b)
      continue;
    const glm::dvec3 c = b->centroid();
    for (const auto &[a, e]: eng::brushEdges(*b)) {
      if (!vertexSelected(id, glm::vec3(a)) || !vertexSelected(id, glm::vec3(e)))
        continue;
      const glm::dvec3 mid = (a + e) * 0.5;
      glm::dvec3 out = mid - c;
      out -= glm::dot(out, glm::normalize(e - a)) * glm::normalize(e - a);
      if (glm::length(out) < 1e-9)
        continue;
      extra[id].push_back(mid + glm::normalize(out) * bulge);
    }
  }
  if (extra.empty())
    return false;
  // The snapshot needs those brushes even though no selected corner moves.
  const auto keep = m_selectedVerts;
  beginVertexEdit();
  for (const auto &[id, pts]: extra)
    if (!m_vertexDragOriginal.count(id))
      if (const auto obj = m_scene->findById(id))
        if (auto b = eng::ConvexBrush::fromObject(*obj)) {
          m_vertexDragOriginal.emplace(id, *b);
          m_vertexDragMeshes[id] = {obj->getMesh(), obj->getPosition(), obj->getRotation(), obj->getScale()};
        }
  m_vertexDragBase.clear();
  previewVertexEdit(nullptr, extra);
  const bool ok = finishVertexEdit(true, QStringLiteral("Split edges"));
  if (!ok)
    m_selectedVerts = keep;
  return ok;
}

// scaling (Alt+E)

bool Engine3D::beginVertexScale() {
  if (!m_editMode || m_selectedVerts.size() < 2)
    return false;
  if (m_vertexScaleActive)
    return true;
  m_vertexPivot = selectedVertexCentre();
  m_vertexScale = 1.0;
  m_vertexScaleActive = true;
  beginVertexEdit();
  touchVertexEdit();
  return true;
}

void Engine3D::setVertexScale(const double s) {
  const double v = std::clamp(s, 0.01, 100.0);
  if (std::fabs(v - m_vertexScale) < 1e-9 && m_vertexMapper)
    return;
  m_vertexScale = v;
  if (!m_vertexScaleActive)
    return;
  const glm::dvec3 pivot(m_vertexPivot);
  glm::dvec3 k(1.0);
  for (int a = 0; a < 3; ++a)
    if (m_vertexScaleAxes & (1 << a))
      k[a] = v;
  previewVertexEdit([pivot, k](quint32, const glm::dvec3 &p) { return pivot + (p - pivot) * k; });
}

void Engine3D::setVertexScaleAxes(const int mask) {
  const int m = mask & 7 ? mask & 7 : 7;
  if (m == m_vertexScaleAxes)
    return;
  m_vertexScaleAxes = m;
  if (m_vertexScaleActive) {
    m_vertexMapper = nullptr;
    setVertexScale(m_vertexScale);
  }
  touchVertexEdit();
}

void Engine3D::setVertexPivot(const QVector3D &p) {
  m_vertexPivot = glm::vec3(p.x(), p.y(), p.z());
  if (m_vertexScaleActive) {
    m_vertexMapper = nullptr;
    setVertexScale(m_vertexScale);
  }
  touchVertexEdit();
}

void Engine3D::moveVertexPivotPlane(const int plane, const glm::dvec2 &p, const double step) {
  const ViewBasis bs = viewBasisFor(plane);
  const glm::dvec3 cur(m_vertexPivot);
  const double depth = glm::dot(cur, bs.depth);
  const glm::dvec3 np = snapValue(p.x, step) * bs.right + snapValue(p.y, step) * bs.up + depth * bs.depth;
  setVertexPivot(QVector3D(float(np.x), float(np.y), float(np.z)));
}

void Engine3D::resetVertexPivot() {
  if (!m_vertexScaleActive)
    return;
  glm::vec3 c(0.0f);
  for (const auto &v: m_vertexDragBase)
    c += v.pos;
  if (!m_vertexDragBase.empty())
    c /= float(m_vertexDragBase.size());
  setVertexPivot(QVector3D(c.x, c.y, c.z));
}

void Engine3D::commitVertexScale() {
  if (!m_vertexScaleActive)
    return;
  m_vertexScaleActive = false;
  const bool changed = std::fabs(m_vertexScale - 1.0) > 1e-9;
  finishVertexEdit(changed, QStringLiteral("Scale vertices"));
  touchVertexEdit();
}

void Engine3D::cancelVertexScale() {
  if (!m_vertexScaleActive)
    return;
  m_vertexScaleActive = false;
  finishVertexEdit(false, {});
  touchVertexEdit();
}

} // namespace vk
