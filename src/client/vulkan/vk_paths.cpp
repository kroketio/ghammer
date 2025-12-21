// Paths (vk_engine.h, "Paths"): the curves Curve modifiers follow. Drawn (points with handles, edited in 2D), or a
// Connect path between two oriented anchor entities, rebuilt whenever they move.

#include "vk_engine.h"
#include "client/engine/physics_world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QGuiApplication>
#include <QJsonDocument>

#include "client/engine/scene.h"
#include "vk_shared.h"

#include <rhi/qrhi.h>

namespace vk {

namespace {

quint64 mixPath(quint64 seed, const quint64 v) { return (seed ^ v) * 0x100000001b3ull + 0x9E3779B97F4A7C15ull; }

quint64 bitsOf(const double d) {
  quint64 u = 0;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

} // namespace

void Engine3D::pushPathTable(const std::vector<eng::path::Path> &before, const QString &label,
                             const QString &mergeKey) {
  if (!m_scene)
    return;
  ++m_scene->pathEpoch;
  emit pathsChanged();
  if (!m_undoStack || m_undoStack->isApplying())
    return;
  const auto after = m_scene->paths;
  auto apply = [this](const std::vector<eng::path::Path> &t) {
    if (!m_scene)
      return;
    m_scene->paths = t;
    ++m_scene->pathEpoch;
    for (auto it = m_selectedPaths.begin(); it != m_selectedPaths.end();)
      it = m_scene->findPath(*it) ? std::next(it) : m_selectedPaths.erase(it);
    emit pathsChanged();
    emit selectionChanged();
    requestRedraw();
  };
  m_undoStack->push(std::make_unique<eng::LambdaCommand>(
      label, [apply, before] { apply(before); }, [apply, after] { apply(after); }, mergeKey));
}

std::vector<quint32> Engine3D::pathUsers(const quint32 path) const {
  std::vector<quint32> out;
  if (!m_scene)
    return out;
  for (const auto &h: m_scene->modHosts)
    for (const auto &m: h.stack)
      if (m.kind == QLatin1String("curve") && eng::mods::CurveParams::fromJson(m.params).path == path) {
        out.push_back(h.id);
        break;
      }
  return out;
}

std::shared_ptr<const eng::path::Curve> Engine3D::pathCurve(const quint32 id) {
  if (!m_scene)
    return nullptr;
  const eng::path::Path *p = m_scene->findPath(id);
  if (!p) {
    m_curveCache.erase(id);
    return nullptr;
  }
  // The key: the path itself, and for a Connect path where its anchors are
  // and what follows it (the turn radius it needs comes from their width).
  quint64 key = mixPath(0x51ed27ull, qHash(QJsonDocument(eng::path::toJson(*p)).toJson(QJsonDocument::Compact)));
  const eng::Entity *ea = nullptr, *eb = nullptr;
  double autoStraight = 1.6, autoRadius = 3.2; // 16 / 32 Source units when nothing follows it
  if (p->kind == eng::path::Path::Kind::Connect) {
    ea = entityById(p->connect.anchorA);
    eb = entityById(p->connect.anchorB);
    for (const eng::Entity *e: {ea, eb})
      if (e)
        for (int i = 0; i < 3; ++i)
          key = mixPath(mixPath(key, bitsOf(e->origin[i])), bitsOf(e->angles[i]));
    // Half the widest object that follows it, sideways to its curve.
    double half = 0.0, needRadius = 0.0;
    for (const quint32 hid: pathUsers(id)) {
      const eng::mods::Host *h = m_scene->findModHost(hid);
      if (!h)
        continue;
      for (const auto &m: h->stack) {
        if (!m.enabled || m.kind != QLatin1String("curve"))
          continue;
        const eng::mods::CurveParams cp = eng::mods::CurveParams::fromJson(m.params);
        if (cp.path != id)
          continue;
        glm::dvec3 f, u, r;
        eng::mods::curveAxes(h->orientation, cp.axis, f, u, r);
        double lo = 1e300, hi = -1e300;
        for (const quint32 sid: modSources(hid))
          if (const auto o = m_scene->findById(sid))
            if (const auto b = eng::ConvexBrush::fromObject(*o))
              for (const auto &v: b->vertices()) {
                lo = std::min(lo, glm::dot(v, r));
                hi = std::max(hi, glm::dot(v, r));
              }
        if (hi < lo)
          continue;
        const double w = (hi - lo) * 0.5 + std::fabs(cp.offsetSide);
        half = std::max(half, w);
        if (cp.mode == eng::mods::CurveParams::Mode::Bend)
          needRadius = std::max(needRadius, w / (1.0 - cp.minJacobian));
        else
          needRadius = std::max(needRadius, w);
      }
    }
    if (half > 0.0) {
      autoStraight = half;
      autoRadius = needRadius + 0.8;
    }
    key = mixPath(mixPath(key, bitsOf(autoStraight)), bitsOf(autoRadius));
  }
  CurveCacheEntry &entry = m_curveCache[id];
  if (entry.key == key && (entry.curve || !entry.error.isEmpty()))
    return entry.curve;
  entry.key = key;
  entry.curve.reset();
  entry.error.clear();
  if (p->kind == eng::path::Path::Kind::Drawn) {
    if (p->points.size() < 2) {
      entry.error = tr("The path needs at least two points");
      return nullptr;
    }
    entry.curve = eng::path::Curve::build(p->points, p->closed);
    return entry.curve;
  }
  if (!ea || !eb) {
    entry.error = tr("Pick both anchors");
    return nullptr;
  }
  const eng::path::ConnectEnd a {glm::dvec3(ea->origin), glm::dvec3(ea->forward())};
  const eng::path::ConnectEnd b {glm::dvec3(eb->origin), glm::dvec3(eb->forward())};
  const eng::path::ConnectResult r = eng::path::buildConnect(a, b, p->connect, autoStraight, autoRadius);
  if (!r.ok) {
    entry.error = r.error;
    return nullptr;
  }
  entry.error = r.warning.isEmpty() ? QString() : r.warning;
  entry.points = r.points;
  entry.curve = eng::path::Curve::build(r.points, false);
  return entry.curve;
}

QString Engine3D::pathError(const quint32 id) {
  const auto c = pathCurve(id);
  const auto it = m_curveCache.find(id);
  if (it == m_curveCache.end())
    return m_scene && m_scene->findPath(id) ? QString() : tr("The path is gone");
  return c ? QString() : it->second.error;
}

const eng::path::Path *Engine3D::path(const quint32 id) const { return m_scene ? m_scene->findPath(id) : nullptr; }

void Engine3D::selectPath(const quint32 id, const bool additive) {
  if (!m_scene || !m_scene->findPath(id))
    return;
  if (!additive) {
    m_selectedObjects.clear();
    m_selectedFaces.clear();
    m_selectedEntities.clear();
    m_selectedMods.clear();
    m_selectedPaths = {id};
  } else if (!m_selectedPaths.erase(id)) {
    m_selectedPaths.insert(id);
  }
  m_selectedPathPoints.clear();
  emit selectionChanged();
  requestRedraw();
}

void Engine3D::editPath(const quint32 id, const std::function<void(eng::path::Path &)> &fn, const QString &label,
                        const QString &mergeKey) {
  eng::path::Path *p = m_scene ? m_scene->findPath(id) : nullptr;
  if (!p)
    return;
  const auto before = m_scene->paths;
  fn(*p);
  eng::path::resolveHandles(p->points, p->closed);
  pushPathTable(before, label, mergeKey.isEmpty() ? QString() : QStringLiteral("path.%1.%2").arg(id).arg(mergeKey));
  requestRedraw();
}

void Engine3D::setPathHidden(const quint32 id, const bool hidden) {
  editPath(id, [hidden](eng::path::Path &p) { p.hidden = hidden; }, hidden ? tr("Hide path") : tr("Show path"));
  if (hidden)
    m_selectedPaths.erase(id);
}

void Engine3D::setPathLocked(const quint32 id, const bool locked) {
  editPath(id, [locked](eng::path::Path &p) { p.locked = locked; }, locked ? tr("Lock path") : tr("Unlock path"));
}

void Engine3D::renamePath(const quint32 id, const QString &name) {
  const QString n = name.trimmed();
  if (n.isEmpty())
    return;
  editPath(id, [n](eng::path::Path &p) { p.name = n; }, tr("Rename path"));
}

void Engine3D::deletePaths(const std::vector<quint32> &ids) {
  if (!m_scene || ids.empty())
    return;
  const auto before = m_scene->paths;
  const std::set<quint32> gone(ids.begin(), ids.end());
  m_scene->paths.erase(std::remove_if(m_scene->paths.begin(), m_scene->paths.end(),
                                      [&](const auto &p) { return gone.count(p.id) != 0; }),
                       m_scene->paths.end());
  for (const quint32 id: ids)
    m_selectedPaths.erase(id);
  m_selectedPathPoints.clear();
  pushPathTable(before, ids.size() == 1 ? tr("Delete path") : tr("Delete paths"));
  emit selectionChanged();
}

quint32 Engine3D::pointsPath() const {
  if (m_selectedPaths.size() != 1 || !m_scene)
    return 0;
  const eng::path::Path *p = m_scene->findPath(*m_selectedPaths.begin());
  return p && p->kind == eng::path::Path::Kind::Drawn && !p->locked && !p->hidden ? p->id : 0;
}

void Engine3D::setPathPointType(const eng::path::HandleType type) {
  const quint32 id = pointsPath();
  if (!id || m_selectedPathPoints.empty())
    return;
  const std::set<int> sel = m_selectedPathPoints;
  editPath(id, [&](eng::path::Path &p) {
    for (const int i: sel) {
      if (i < 0 || i >= int(p.points.size()))
        continue;
      eng::path::Point &q = p.points[size_t(i)];
      q.type = type;
      if (type == eng::path::HandleType::Aligned) {
        // Line the in handle up with the out handle (each keeps its length).
        const double lo = glm::length(q.out), li = glm::length(q.in);
        if (lo > 1e-9)
          q.in = -q.out / lo * (li > 1e-9 ? li : lo);
      }
    }
  }, tr("Point type"));
}

void Engine3D::cyclePathPointType() {
  const quint32 id = pointsPath();
  const eng::path::Path *p = id ? m_scene->findPath(id) : nullptr;
  if (!p || m_selectedPathPoints.empty())
    return;
  const int first = *m_selectedPathPoints.begin();
  if (first < 0 || first >= int(p->points.size()))
    return;
  const int next = (int(p->points[size_t(first)].type) + 1) % 4;
  setPathPointType(eng::path::HandleType(next));
  static const char *names[4] = {"Auto", "Aligned", "Free", "Vector"};
  emit toastRequested(tr("Point type: %1").arg(QLatin1String(names[next])));
}

bool Engine3D::deleteSelectedPathPoints() {
  const quint32 id = pointsPath();
  if (!id || m_selectedPathPoints.empty())
    return false;
  const eng::path::Path *p = m_scene->findPath(id);
  if (int(p->points.size()) - int(m_selectedPathPoints.size()) < 2) {
    deletePaths({id});
    return true;
  }
  const std::set<int> sel = m_selectedPathPoints;
  editPath(id, [&](eng::path::Path &path) {
    std::vector<eng::path::Point> keep;
    for (int i = 0; i < int(path.points.size()); ++i)
      if (!sel.count(i))
        keep.push_back(path.points[size_t(i)]);
    path.points = std::move(keep);
    if (path.points.size() < 3)
      path.closed = false;
  }, tr("Delete points"));
  m_selectedPathPoints.clear();
  emit selectionChanged();
  return true;
}

bool Engine3D::connectSelectedAnchors() {
  if (!m_scene || m_selectedEntities.size() != 2) {
    emit toastRequested(tr("Select two anchor entities (Path Anchor) first"));
    return false;
  }
  const quint32 a = *m_selectedEntities.begin(), b = *std::next(m_selectedEntities.begin());
  const auto before = m_scene->paths;
  eng::path::Path p;
  p.id = m_scene->mintPathId();
  p.name = tr("Connection %1").arg(p.id);
  p.kind = eng::path::Path::Kind::Connect;
  p.connect.anchorA = a;
  p.connect.anchorB = b;
  m_scene->paths.push_back(p);
  pushPathTable(before, tr("Connect anchors"));
  m_selectedPaths = {p.id};
  emit selectionChanged();
  if (const QString err = pathError(p.id); !err.isEmpty())
    emit toastRequested(err);
  return true;
}

bool Engine3D::convertPathToDrawn(const quint32 id) {
  const eng::path::Path *p = m_scene ? m_scene->findPath(id) : nullptr;
  if (!p || p->kind != eng::path::Path::Kind::Connect)
    return false;
  if (!pathCurve(id)) {
    emit toastRequested(pathError(id));
    return false;
  }
  const std::vector<eng::path::Point> pts = m_curveCache[id].points;
  editPath(id, [pts](eng::path::Path &path) {
    path.kind = eng::path::Path::Kind::Drawn;
    path.points = pts;
    path.closed = false;
  }, tr("Convert to drawn path"));
  return true;
}

bool Engine3D::brushesBetweenAnchors() {
  if (!m_scene)
    return false;
  if (m_selectedEntities.size() != 2 || (m_selectedObjects.empty() && m_selectedMods.size() != 1)) {
    emit toastRequested(tr("Select the brushes (or a modified object) and two anchor entities"));
    return false;
  }
  const QString label = tr("Brushes between anchors");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  const std::set<quint32> objects = m_selectedObjects;
  const std::set<quint32> mods = m_selectedMods;
  connectSelectedAnchors();
  const quint32 pathId = m_selectedPaths.empty() ? 0 : *m_selectedPaths.begin();
  m_selectedPaths.clear();
  m_selectedEntities.clear();
  m_selectedObjects = objects;
  m_selectedMods = mods;
  // The brushes leave anchor A along its facing: that is the way they run
  // when their shape alone does not say.
  glm::dvec3 runHint(0.0);
  const glm::dvec3 *hint = nullptr;
  if (const eng::path::Path *p = pathId ? m_scene->findPath(pathId) : nullptr)
    if (const eng::Entity *a = entityById(p->connect.anchorA)) {
      runHint = glm::dvec3(a->forward());
      hint = &runHint;
    }
  const quint32 host = addModifier(QStringLiteral("array"), hint);
  if (host && pathId) {
    if (const eng::mods::Host *h = modHost(host)) {
      const quint32 arrayId = h->stack.back().id;
      eng::mods::ArrayParams ap = eng::mods::ArrayParams::fromJson(h->stack.back().params);
      ap.fit = eng::mods::ArrayParams::Fit::Curve;
      // One texture through all the copies, for the Curve to lay along the path.
      ap.textureFollow = false;
      setModifierParams(host, arrayId, ap.toJson());
    }
    addModifier(QStringLiteral("curve"), hint);
    if (const eng::mods::Host *h = modHost(host)) {
      const quint32 curveId = h->stack.back().id;
      eng::mods::CurveParams cp = eng::mods::CurveParams::fromJson(h->stack.back().params);
      cp.path = pathId;
      cp.stretch = true;
      setModifierParams(host, curveId, cp.toJson());
    }
  }
  if (record)
    m_undoStack->endMacro();
  pumpModifiers(ModPump::Blocking);
  if (host)
    setSelectedMods({host});
  return host != 0;
}

// anchor handles (2D)

std::set<quint32> Engine3D::selectedHostAnchors() const {
  std::set<quint32> out;
  if (!m_scene || m_selectedMods.empty() || !m_selectedObjects.empty() || !m_selectedEntities.empty()
      || !m_selectedPaths.empty() || materialMode() || editMode())
    return out;
  for (const quint32 hid: m_selectedMods) {
    const eng::mods::Host *h = m_scene->findModHost(hid);
    if (!h)
      continue;
    for (const eng::mods::Modifier &m: h->stack) {
      if (!m.enabled || m.kind != QLatin1String("curve"))
        continue;
      const eng::path::Path *p = m_scene->findPath(eng::mods::CurveParams::fromJson(m.params).path);
      if (!p || p->kind != eng::path::Path::Kind::Connect)
        continue;
      for (const quint32 a: {p->connect.anchorA, p->connect.anchorB})
        if (entityById(a))
          out.insert(a);
    }
  }
  return out;
}

bool Engine3D::anchorHandleMode() const { return !selectedHostAnchors().empty(); }

std::vector<Engine3D::AnchorHandle> Engine3D::anchorHandles2D(const int plane, const glm::dvec4 &box) const {
  std::vector<AnchorHandle> out;
  const std::set<quint32> anchors = selectedHostAnchors();
  if (anchors.empty() || !m_scene)
    return out;
  const ViewBasis bs = viewBasisFor(plane);
  const glm::dvec3 right(bs.right), up(bs.up);
  // What is drawn selected, each piece's box in the view plane: the pieces that
  // touch a side of the selection box are the object's end there.
  std::vector<glm::dvec4> rects;
  for (const quint32 id: m_drawSelection)
    if (const auto o = m_scene->findById(id)) {
      const auto [lo, hi] = o->getWorldAABB();
      glm::dvec4 r(1e300, 1e300, -1e300, -1e300);
      for (int i = 0; i < 8; ++i) {
        const glm::dvec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const double a = glm::dot(c, right), b = glm::dot(c, up);
        r = {std::min(r.x, a), std::min(r.y, b), std::max(r.z, a), std::max(r.w, b)};
      }
      rects.push_back(r);
    }
  const double tol = 1e-3 * std::max(box.z - box.x, box.w - box.y) + 0.01;
  for (const quint32 id: anchors) {
    const eng::Entity *e = entityById(id);
    if (!e)
      continue;
    // An anchor faces into the gap, so the object ends at the side of its box
    // facing the other way (anchors ignore pitch: horizontal only).
    glm::dvec3 n = -glm::dvec3(e->forward());
    n.y = 0.0;
    if (glm::length(n) < 1e-6)
      continue;
    n = glm::normalize(n);
    const glm::dvec2 n2(glm::dot(n, right), glm::dot(n, up));
    if (glm::length(n2) < 0.5) // seen end-on in this view
      continue;
    const glm::dvec3 o(e->origin);
    const bool vertical = std::fabs(n2.x) >= std::fabs(n2.y); // the side is a vertical edge
    const double side = vertical ? (n2.x > 0.0 ? box.z : box.x) : (n2.y > 0.0 ? box.w : box.y);
    // Centred on the end face: the pieces touching that side, as runs along it
    // (both ends of a U touch the same side), the run nearest the anchor (the
    // anchor itself when nothing touches).
    const double at = vertical ? glm::dot(o, up) : glm::dot(o, right);
    std::vector<std::pair<double, double>> spans;
    for (const glm::dvec4 &r: rects) {
      const bool touches = vertical ? (n2.x > 0.0 ? r.z >= side - tol : r.x <= side + tol)
                                    : (n2.y > 0.0 ? r.w >= side - tol : r.y <= side + tol);
      if (touches)
        spans.emplace_back(vertical ? r.y : r.x, vertical ? r.w : r.z);
    }
    std::sort(spans.begin(), spans.end());
    double along = at, bestGap = 1e300;
    for (size_t i = 0; i < spans.size();) {
      double lo = spans[i].first, hi = spans[i].second;
      size_t j = i + 1;
      for (; j < spans.size() && spans[j].first <= hi + tol; ++j)
        hi = std::max(hi, spans[j].second);
      const double gap = at < lo ? lo - at : (at > hi ? at - hi : 0.0);
      if (gap < bestGap) {
        bestGap = gap;
        along = (lo + hi) * 0.5;
      }
      i = j;
    }
    AnchorHandle h;
    h.anchor = id;
    h.pos = vertical ? glm::dvec2(side, std::clamp(along, box.y, box.w))
                     : glm::dvec2(std::clamp(along, box.x, box.z), side);
    out.push_back(h);
  }
  return out;
}

bool Engine3D::beginAnchorDrag(const int plane, const quint32 anchor, const glm::dvec2 &p, const double step) {
  m_anchorDrag = {};
  const eng::Entity *e = entityById(anchor);
  if (!e)
    return false;
  m_anchorDrag.active = true;
  m_anchorDrag.plane = plane;
  m_anchorDrag.anchor = anchor;
  m_anchorDrag.grab = p;
  m_anchorDrag.step = step > 0.0 ? step : 1.0;
  m_anchorDrag.base = e->origin;
  return true;
}

void Engine3D::updateAnchorDrag(const glm::dvec2 &p) {
  if (!m_anchorDrag.active)
    return;
  const eng::Entity *e = entityById(m_anchorDrag.anchor);
  if (!e)
    return;
  const ViewBasis bs = viewBasisFor(m_anchorDrag.plane);
  const glm::dvec3 right(bs.right), up(bs.up), base(m_anchorDrag.base);
  // The anchor lands on the grid (Ctrl: free), like a dragged entity.
  const bool free = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier);
  auto snap = [&](const double v) {
    return free ? v : std::round(v / m_anchorDrag.step) * m_anchorDrag.step;
  };
  const double a0 = glm::dot(base, right), b0 = glm::dot(base, up);
  const glm::dvec2 d = p - m_anchorDrag.grab;
  const double da = snap(a0 + d.x) - a0, db = snap(b0 + d.y) - b0;
  const glm::vec3 origin(base + da * right + db * up);
  if (origin == e->origin)
    return;
  setEntityTransform(m_anchorDrag.anchor, origin, e->angles, e->name, false);
}

void Engine3D::endAnchorDrag() {
  if (!m_anchorDrag.active)
    return;
  const AnchorDragState st = m_anchorDrag;
  m_anchorDrag = {};
  recordEntityMoves({{st.anchor, st.base}}, tr("Move anchor"));
  pumpModifiers(ModPump::Blocking);
}

glm::dvec3 Engine3D::planeToWorld(const int plane, const glm::dvec2 &p, const double depth) const {
  const ViewBasis bs = viewBasisFor(plane);
  return bs.right * p.x + bs.up * p.y + bs.depth * depth;
}

namespace {

double snapTo(const double v, const double step) { return step > 0.0 ? std::round(v / step) * step : v; }

bool noSnap() { return QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier); }

} // namespace

void Engine3D::setPenActive(const bool on) {
  if (m_pen.active == on && (on || m_pen.points.empty()))
    return;
  m_pen = {};
  m_pen.active = on;
  if (on) {
    m_selectedPathPoints.clear();
  }
  ++m_viewEpoch;
  emit penChanged();
  requestRedraw();
}

bool Engine3D::penPress(const int plane, const glm::dvec2 &pIn, const double step, const double grab) {
  if (!m_pen.active || !m_scene)
    return false;
  const ViewBasis bs = viewBasisFor(plane);
  const glm::dvec2 p = noSnap() ? pIn : glm::dvec2(snapTo(pIn.x, step), snapTo(pIn.y, step));
  // Closing: a press on the first point of three or more.
  if (m_pen.points.size() >= 3) {
    const glm::dvec3 f = m_pen.points.front().pos;
    if (glm::length(glm::dvec2(glm::dot(f, bs.right), glm::dot(f, bs.up)) - pIn) <= grab) {
      const auto before = m_scene->paths;
      eng::path::Path path;
      path.id = m_scene->mintPathId();
      path.name = tr("Path %1").arg(path.id);
      path.points = m_pen.points;
      path.closed = true;
      eng::path::resolveHandles(path.points, true);
      m_scene->paths.push_back(path);
      pushPathTable(before, tr("Draw path"));
      m_pen = {};
      m_selectedPaths = {path.id};
      emit penChanged();
      emit selectionChanged();
      return true;
    }
  }
  // The depth this view does not show: from the previous point, else the top
  // of the smallest brush under the pointer, else the ground.
  double depth = 0.0;
  if (!m_pen.points.empty()) {
    depth = glm::dot(m_pen.points.back().pos, bs.depth);
  } else {
    double bestArea = 1e300;
    for (const auto &o: m_scene->gameObjects) {
      if (!o || o->hidden() || !o->getMesh())
        continue;
      const auto [lo, hi] = o->getWorldAABB();
      const glm::dvec3 l(lo), h(hi);
      const double a0 = std::min(glm::dot(l, bs.right), glm::dot(h, bs.right));
      const double a1 = std::max(glm::dot(l, bs.right), glm::dot(h, bs.right));
      const double b0 = std::min(glm::dot(l, bs.up), glm::dot(h, bs.up));
      const double b1 = std::max(glm::dot(l, bs.up), glm::dot(h, bs.up));
      if (pIn.x < a0 || pIn.x > a1 || pIn.y < b0 || pIn.y > b1)
        continue;
      const double area = (a1 - a0) * (b1 - b0);
      if (area < bestArea) {
        bestArea = area;
        depth = std::min(glm::dot(l, bs.depth), glm::dot(h, bs.depth)); // the side facing the viewer
      }
    }
  }
  // A press on the newest point (the second press of a double click, or to
  // pull its handles again) adds nothing.
  if (!m_pen.points.empty()) {
    const glm::dvec3 last = m_pen.points.back().pos;
    if (glm::length(glm::dvec2(glm::dot(last, bs.right), glm::dot(last, bs.up)) - p) < 1e-6) {
      m_pen.dragging = true;
      m_pen.plane = plane;
      return true;
    }
  }
  eng::path::Point pt;
  pt.pos = planeToWorld(plane, p, depth);
  pt.type = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::AltModifier) ? eng::path::HandleType::Vector
                                                                                : eng::path::HandleType::Auto;
  m_pen.points.push_back(pt);
  m_pen.dragging = true;
  m_pen.plane = plane;
  eng::path::resolveHandles(m_pen.points, false);
  emit penChanged();
  requestRedraw();
  return true;
}

void Engine3D::penDrag(const glm::dvec2 &p) {
  if (!m_pen.active || !m_pen.dragging || m_pen.points.empty())
    return;
  const ViewBasis bs = viewBasisFor(m_pen.plane);
  eng::path::Point &last = m_pen.points.back();
  const glm::dvec3 at = planeToWorld(m_pen.plane, p, glm::dot(last.pos, bs.depth));
  const glm::dvec3 handle = at - last.pos;
  if (glm::length(handle) > 1e-3) {
    last.type = eng::path::HandleType::Aligned;
    last.out = handle;
    last.in = -handle;
  }
  eng::path::resolveHandles(m_pen.points, false);
  emit penChanged();
  requestRedraw();
}

void Engine3D::penRelease() { m_pen.dragging = false; }

void Engine3D::penHover(const int plane, const glm::dvec2 &pIn, const double step) {
  if (!m_pen.active)
    return;
  const ViewBasis bs = viewBasisFor(plane);
  const glm::dvec2 p = noSnap() ? pIn : glm::dvec2(snapTo(pIn.x, step), snapTo(pIn.y, step));
  const double depth = m_pen.points.empty() ? 0.0 : glm::dot(m_pen.points.back().pos, bs.depth);
  m_pen.hover = planeToWorld(plane, p, depth);
  m_pen.hoverValid = true;
  emit penChanged();
  requestRedraw();
}

void Engine3D::penLeave() {
  if (!m_pen.hoverValid)
    return;
  m_pen.hoverValid = false;
  emit penChanged();
  requestRedraw();
}

bool Engine3D::penFinish() {
  if (!m_pen.active || !m_scene)
    return false;
  if (m_pen.points.size() < 2) {
    setPenActive(false);
    return true;
  }
  const auto before = m_scene->paths;
  eng::path::Path path;
  path.id = m_scene->mintPathId();
  path.name = tr("Path %1").arg(path.id);
  path.points = m_pen.points;
  eng::path::resolveHandles(path.points, false);
  m_scene->paths.push_back(path);
  pushPathTable(before, tr("Draw path"));
  m_pen = {};
  m_selectedObjects.clear();
  m_selectedFaces.clear();
  m_selectedMods.clear();
  m_selectedPaths = {path.id};
  emit penChanged();
  emit selectionChanged();
  requestRedraw();
  return true;
}

void Engine3D::penCancel() {
  if (!m_pen.points.empty()) {
    m_pen.points.clear();
    m_pen.dragging = false;
    emit penChanged();
    requestRedraw();
    return;
  }
  setPenActive(false);
}

bool Engine3D::penBackspace() {
  if (!m_pen.active || m_pen.points.empty())
    return false;
  m_pen.points.pop_back();
  eng::path::resolveHandles(m_pen.points, false);
  emit penChanged();
  requestRedraw();
  return true;
}

bool Engine3D::penPreview(std::vector<glm::dvec3> &polyline, std::vector<glm::dvec3> &points) const {
  polyline.clear();
  points.clear();
  if (!m_pen.active || m_pen.points.empty())
    return false;
  std::vector<eng::path::Point> pts = m_pen.points;
  for (const auto &q: pts)
    points.push_back(q.pos);
  if (m_pen.hoverValid && !m_pen.dragging) {
    eng::path::Point h;
    h.pos = m_pen.hover;
    pts.push_back(h);
    eng::path::resolveHandles(pts, false);
  }
  if (pts.size() >= 2)
    polyline = eng::path::Curve::build(pts, false)->polyline();
  return true;
}

bool Engine3D::pickPathHandle2D(const int plane, const glm::dvec2 &p, const double tol, PathHandleHit &hit) const {
  const quint32 id = pointsPath();
  const eng::path::Path *path = id ? m_scene->findPath(id) : nullptr;
  if (!path)
    return false;
  const ViewBasis bs = viewBasisFor(plane);
  auto at = [&](const glm::dvec3 &w) { return glm::dvec2(glm::dot(w, bs.right), glm::dot(w, bs.up)); };
  double best = tol;
  bool found = false;
  for (int i = 0; i < int(path->points.size()); ++i) {
    const auto &q = path->points[size_t(i)];
    const struct {
      glm::dvec3 w;
      int handle;
    } cands[3] = {{q.pos, 0}, {q.pos + q.in, -1}, {q.pos + q.out, 1}};
    for (const auto &c: cands) {
      if (c.handle != 0 && glm::length(c.w - q.pos) < 1e-6)
        continue;
      // Points win over handles at the same spot.
      const double d = glm::length(at(c.w) - p) + (c.handle != 0 ? tol * 0.25 : 0.0);
      if (d <= best) {
        best = d;
        hit = {id, i, c.handle};
        found = true;
      }
    }
  }
  return found;
}

bool Engine3D::pickPathCurve2D(const int plane, const glm::dvec2 &p, const double tol, quint32 &pathOut,
                               double &sOut) const {
  if (!m_scene)
    return false;
  const ViewBasis bs = viewBasisFor(plane);
  double best = tol;
  bool found = false;
  for (const auto &path: m_scene->paths) {
    if (path.hidden || path.locked)
      continue;
    const auto c = const_cast<Engine3D *>(this)->pathCurve(path.id);
    if (!c)
      continue;
    double d = 0.0;
    const double s = c->nearest2D(p, bs.right, bs.up, d);
    if (d <= best) {
      best = d;
      pathOut = path.id;
      sOut = s;
      found = true;
    }
  }
  return found;
}

bool Engine3D::beginPathDrag(const int plane, const glm::dvec2 &p, const double step, const PathHandleHit &hit,
                             const bool additive) {
  if (!m_scene || !m_scene->findPath(hit.path))
    return false;
  if (hit.point >= 0 && hit.handle == 0) {
    if (additive) {
      if (!m_selectedPathPoints.erase(hit.point))
        m_selectedPathPoints.insert(hit.point);
    } else if (!m_selectedPathPoints.count(hit.point)) {
      m_selectedPathPoints = {hit.point};
    }
    emit selectionChanged();
  }
  m_pathDrag = {};
  m_pathDrag.active = true;
  m_pathDrag.plane = plane;
  m_pathDrag.path = hit.path;
  m_pathDrag.point = hit.point;
  m_pathDrag.handle = hit.handle;
  m_pathDrag.start = p;
  m_pathDrag.step = step > 0.0 ? step : 1.0;
  m_pathDrag.before = m_scene->paths;
  requestRedraw();
  return true;
}

void Engine3D::updatePathDrag(const glm::dvec2 &p) {
  if (!m_pathDrag.active || !m_scene)
    return;
  eng::path::Path *path = m_scene->findPath(m_pathDrag.path);
  const eng::path::Path *base = nullptr;
  for (const auto &b: m_pathDrag.before)
    if (b.id == m_pathDrag.path)
      base = &b;
  if (!path || !base)
    return;
  const ViewBasis bs = viewBasisFor(m_pathDrag.plane);
  const bool free = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::AltModifier);
  *path = *base;
  if (m_pathDrag.point >= 0 && m_pathDrag.handle != 0) {
    // A handle: where the pointer is, at the point's depth.
    const eng::path::Point &q = base->points[size_t(m_pathDrag.point)];
    const glm::dvec3 at = planeToWorld(m_pathDrag.plane, p, glm::dot(q.pos, bs.depth));
    eng::path::dragHandle(path->points, path->closed, m_pathDrag.point, m_pathDrag.handle > 0, at - q.pos, free);
  } else {
    // Points (or the whole path): the grabbed one lands on the grid, the rest keep their offsets.
    glm::dvec2 d = p - m_pathDrag.start;
    if (!free && !noSnap()) {
      glm::dvec3 anchor = m_pathDrag.point >= 0 ? base->points[size_t(m_pathDrag.point)].pos
                                                : (base->points.empty() ? glm::dvec3(0.0) : base->points.front().pos);
      const glm::dvec2 a(glm::dot(anchor, bs.right), glm::dot(anchor, bs.up));
      const glm::dvec2 target(snapTo(a.x + d.x, m_pathDrag.step), snapTo(a.y + d.y, m_pathDrag.step));
      d = target - a;
    }
    const glm::dvec3 delta = bs.right * d.x + bs.up * d.y;
    for (int i = 0; i < int(path->points.size()); ++i)
      if (m_pathDrag.point < 0 || m_selectedPathPoints.count(i))
        path->points[size_t(i)].pos += delta;
    eng::path::resolveHandles(path->points, path->closed);
  }
  m_pathDrag.moved = true;
  ++m_scene->pathEpoch;
  emit penChanged(); // repaint the views
  requestRedraw();
}

void Engine3D::endPathDrag() {
  if (!m_pathDrag.active)
    return;
  const bool moved = m_pathDrag.moved;
  const auto before = std::move(m_pathDrag.before);
  m_pathDrag = {};
  if (moved)
    pushPathTable(before, tr("Edit path"));
  requestRedraw();
}

bool Engine3D::insertPathPointAt(const quint32 id, const double s) {
  const auto c = pathCurve(id);
  if (!c)
    return false;
  int seg = 0;
  double t = 0.0;
  c->segmentAt(s, seg, t);
  editPath(id, [&](eng::path::Path &p) { eng::path::insertPoint(p.points, p.closed, seg, t); }, tr("Insert point"));
  m_selectedPathPoints = {seg + 1};
  emit selectionChanged();
  return true;
}

std::vector<Engine3D::PathDraw> Engine3D::pathDraws() {
  std::vector<PathDraw> out;
  if (!m_scene)
    return out;
  const quint32 editing = pointsPath();
  for (const auto &p: m_scene->paths) {
    if (p.hidden)
      continue;
    PathDraw d;
    d.id = p.id;
    d.selected = m_selectedPaths.count(p.id) != 0;
    d.editing = p.id == editing;
    d.connect = p.kind == eng::path::Path::Kind::Connect;
    const auto c = pathCurve(p.id);
    d.error = !c;
    if (c) {
      d.polyline = c->polyline();
      d.polylineS = c->polylineS();
    }
    if (d.editing)
      for (int i = 0; i < int(p.points.size()); ++i) {
        const auto &q = p.points[size_t(i)];
        d.points.push_back({q.pos, q.pos + q.in, q.pos + q.out, m_selectedPathPoints.count(i) != 0, int(q.type)});
      }
    // Where an object following it cannot be built.
    for (const quint32 hid: pathUsers(p.id)) {
      const auto it = m_modRuntime.find(hid);
      if (it == m_modRuntime.end())
        continue;
      for (const auto &diag: it->second.diagnostics)
        if (diag.severity == eng::mods::Diagnostic::Severity::Error && diag.pathId == p.id && diag.s0 >= 0.0)
          d.errorSpans.emplace_back(diag.s0, diag.s1);
    }
    out.push_back(std::move(d));
  }
  return out;
}

void Engine3D::buildPathOverlay() {
  m_pathEdgeCount = 0;
  if (!m_scene)
    return;
  bool anchors = false;
  for (const auto &e: m_scene->entities)
    anchors = anchors || e.classname == QLatin1String("path_anchor");
  // Every modified object: a green box around its sources.
  const std::vector<std::pair<glm::dvec3, glm::dvec3>> sourceBoxes = modSourceBoxes();
  // Selected rigid bodies: their hulls from the last physics build (not while a newer one is pending: they would lag
  // behind the brushes); while simulated, where the body is now.
  std::vector<std::pair<const std::vector<std::pair<glm::vec3, glm::vec3>> *, const glm::dmat4 *>> rigidEdges;
  if (const eng::phys::World *w = physicsWorld(); w && !physicsBusy())
    for (const auto &b: w->bodies())
      if (m_selectedEntities.count(b.entity))
        rigidEdges.emplace_back(&b.edges, physicsBodyPose(b.entity));
  // Bodies the physics simulation moved: a red box where they are now.
  const std::vector<std::array<glm::dvec3, 8>> dirtyBoxes =
      m_preview ? std::vector<std::array<glm::dvec3, 8>>() : physicsDirtyBoxes();
  // ... and a trail of green dots where they went.
  const std::vector<std::vector<glm::dvec3>> trails =
      m_preview ? std::vector<std::vector<glm::dvec3>>() : physicsTrails();
  if (m_scene->paths.empty() && !m_pen.active && !anchors && sourceBoxes.empty() && rigidEdges.empty()
      && dirtyBoxes.empty() && trails.empty())
    return;
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;
  std::vector<float> v;
  auto put = [&](const glm::dvec3 &p, const glm::vec4 &c) {
    v.insert(v.end(), {float(p.x), float(p.y + 0.01), float(p.z), c.r, c.g, c.b, c.a});
  };
  auto line = [&](const glm::dvec3 &a, const glm::dvec3 &b, const glm::vec4 &c) {
    put(a, c);
    put(b, c);
  };
  auto cross = [&](const glm::dvec3 &p, const double r, const glm::vec4 &c) {
    line(p - glm::dvec3(r, 0, 0), p + glm::dvec3(r, 0, 0), c);
    line(p - glm::dvec3(0, r, 0), p + glm::dvec3(0, r, 0), c);
    line(p - glm::dvec3(0, 0, r), p + glm::dvec3(0, 0, r), c);
  };
  const glm::vec4 kPath(0.30f, 0.80f, 1.0f, 0.85f), kSel(0.55f, 0.95f, 1.0f, 1.0f), kErr(1.0f, 0.32f, 0.32f, 1.0f),
      kConnect(0.75f, 0.60f, 1.0f, 0.9f), kPoint(1.0f, 1.0f, 1.0f, 1.0f), kPointSel(1.0f, 0.62f, 0.15f, 1.0f);
  for (const PathDraw &d: pathDraws()) {
    const glm::vec4 col = d.error ? kErr : d.selected ? kSel : d.connect ? kConnect : kPath;
    for (size_t i = 0; i + 1 < d.polyline.size(); ++i) {
      bool red = false;
      if (i < d.polylineS.size())
        for (const auto &[a, b]: d.errorSpans)
          red = red || (d.polylineS[i] >= a && d.polylineS[i] <= b);
      line(d.polyline[i], d.polyline[i + 1], red ? kErr : col);
    }
    for (const auto &pt: d.points)
      cross(pt.pos, 0.25, pt.selected ? kPointSel : kPoint);
  }
  // Path anchors: an arrow along their facing (where the corridor leaves).
  for (const auto &e: m_scene->entities) {
    if (e.classname != QLatin1String("path_anchor") || e.hidden)
      continue;
    glm::dvec3 f(e.forward());
    f.y = 0.0;
    if (glm::length(f) < 1e-6)
      continue;
    f = glm::normalize(f);
    const glm::dvec3 o(e.origin), tip = o + f * 2.4, side = glm::cross(f, glm::dvec3(0, 1, 0)) * 0.4;
    const glm::vec4 c(0.75f, 0.60f, 1.0f, 1.0f);
    line(o, tip, c);
    line(tip, tip - f * 0.7 + side, c);
    line(tip, tip - f * 0.7 - side, c);
  }
  for (const auto &[boxLo, boxHi]: sourceBoxes) {
    // A hair outside the brushes, so the outline never fights their edges.
    const glm::dvec3 pad(0.05);
    const glm::dvec3 a = boxLo - pad, b = boxHi + pad;
    const glm::vec4 kEditBox(0.30f, 0.95f, 0.40f, 1.0f);
    const glm::dvec3 c[8] = {{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, a.y, b.z}, {a.x, a.y, b.z},
                             {a.x, b.y, a.z}, {b.x, b.y, a.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}};
    for (int i = 0; i < 4; ++i) {
      line(c[i], c[(i + 1) % 4], kEditBox);
      line(c[i + 4], c[(i + 1) % 4 + 4], kEditBox);
      line(c[i], c[i + 4], kEditBox);
    }
  }
  {
    const double toWorld = 1.0 / eng::kMetersPerWorldUnit;
    const glm::vec4 kHull(1.0f, 0.80f, 0.25f, 1.0f);
    for (const auto &[edges, pose]: rigidEdges)
      for (const auto &[a, b]: *edges) {
        glm::dvec3 pa = glm::dvec3(a) * toWorld, pb = glm::dvec3(b) * toWorld;
        if (pose) {
          pa = glm::dvec3(*pose * glm::dvec4(pa, 1.0));
          pb = glm::dvec3(*pose * glm::dvec4(pb, 1.0));
        }
        line(pa, pb, kHull);
      }
  }
  {
    const glm::vec4 kTrail(0.35f, 1.0f, 0.45f, 1.0f);
    for (const auto &trail: trails)
      for (const glm::dvec3 &p: trail)
        cross(p, 0.24, kTrail);
  }
  {
    const glm::vec4 kDirty(0.95f, 0.25f, 0.25f, 1.0f);
    for (const auto &c: dirtyBoxes)
      for (int i = 0; i < 4; ++i) {
        line(c[size_t(i)], c[size_t((i + 1) % 4)], kDirty);
        line(c[size_t(i + 4)], c[size_t((i + 1) % 4 + 4)], kDirty);
        line(c[size_t(i)], c[size_t(i + 4)], kDirty);
      }
  }
  std::vector<glm::dvec3> penPoly, penPts;
  if (penPreview(penPoly, penPts)) {
    for (size_t i = 0; i + 1 < penPoly.size(); ++i)
      line(penPoly[i], penPoly[i + 1], kSel);
    for (const auto &p: penPts)
      cross(p, 0.25, kPoint);
  }
  if (v.empty())
    return;
  const quint32 bytes = quint32(v.size() * sizeof(float));
  if (!m_pathVb || bytes > m_pathVb->size()) {
    Shared::instance().retire(m_pathVb);
    m_pathVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, std::max(bytes * 2, 4096u));
    if (!m_pathVb->create()) {
      Shared::instance().retire(m_pathVb);
      m_pathVb = nullptr;
      return;
    }
  }
  if (!m_pathBatch)
    m_pathBatch = rhi->nextResourceUpdateBatch();
  m_pathBatch->updateDynamicBuffer(m_pathVb, 0, bytes, v.data());
  m_pathEdgeCount = quint32(v.size() / 7);
}

void Engine3D::drawPathOverlay(QRhiCommandBuffer *cb) {
  if (!m_pathVb || !m_pathEdgeCount || !m_collisionEdgePipeline || !m_cutWireSrb)
    return;
  const QRhiCommandBuffer::VertexInput vin(m_pathVb, 0);
  cb->setGraphicsPipeline(m_collisionEdgePipeline);
  cb->setVertexInput(0, 1, &vin);
  cb->setShaderResources(m_cutWireSrb);
  cb->draw(m_pathEdgeCount);
}

} // namespace vk
