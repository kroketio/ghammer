// Mirror mode and mirror links: the line, linked copies, and the sync keeping both sides reflections.

#include "vk_engine.h"

#include <map>

#include <glm/gtc/matrix_inverse.hpp>

#include "client/engine/scene.h"
#include "vk_shared.h"
#include "vk_undo.h"

namespace vk {

namespace {

using eng::mirror::Plane;

// A texture basis through the reflection across `p` (the texture-lock rule of
// ConvexBrush::transform), flipped back unless the link shows mirror images.
eng::FaceTexBasis reflectBasis(eng::FaceTexBasis b, const Plane &p, const bool mirrorTextures) {
  if (!b.valid)
    return b;
  glm::dvec3 t(0.0);
  t[p.axis] = 2.0 * p.d;
  b.uAxis[p.axis] = -b.uAxis[p.axis];
  b.vAxis[p.axis] = -b.vAxis[p.axis];
  b.offsetU = float(b.offsetU - glm::dot(t, glm::dvec3(b.uAxis)) * b.tilesPerUnitU);
  b.offsetV = float(b.offsetV - glm::dot(t, glm::dvec3(b.vAxis)) * b.tilesPerUnitV);
  if (!mirrorTextures)
    eng::unmirrorBasis(b, 1 << p.axis);
  return b;
}

// World plane (outward normal, distance) of every mesh face of `o`.
std::vector<std::pair<glm::dvec3, double>> worldFacePlanes(const eng::GameObject &o) {
  std::vector<std::pair<glm::dvec3, double>> out;
  const auto mesh = o.getMesh();
  if (!mesh)
    return out;
  const glm::dmat4 model(o.getModelMatrix());
  const glm::dmat3 nm = glm::inverseTranspose(glm::dmat3(model));
  for (int f = 0; f < mesh->faceCount(); ++f) {
    glm::vec3 n(0.0f);
    float d = 0.0f;
    if (!mesh->getFacePlane(f, n, d)) {
      out.emplace_back(glm::dvec3(0.0), 0.0);
      continue;
    }
    const glm::dvec3 pw = glm::dvec3(model * glm::dvec4(glm::dvec3(n) * double(d), 1.0));
    const glm::dvec3 nw = glm::normalize(nm * glm::dvec3(n));
    out.emplace_back(nw, glm::dot(nw, pw));
  }
  return out;
}

std::pair<glm::dvec3, glm::dvec3> objectBox(const eng::GameObject &o) {
  const auto [lo, hi] = o.getWorldAABB();
  return {glm::dvec3(lo), glm::dvec3(hi)};
}

bool sameBox(const std::pair<glm::dvec3, glm::dvec3> &a, const std::pair<glm::dvec3, glm::dvec3> &b,
             const double tol) {
  return glm::all(glm::lessThanEqual(glm::abs(a.first - b.first), glm::dvec3(tol)))
         && glm::all(glm::lessThanEqual(glm::abs(a.second - b.second), glm::dvec3(tol)));
}

double snapTo(const double v, const double step) { return step > 0.0 ? std::round(v / step) * step : v; }

} // namespace

bool Engine3D::mirrorLineValid() const { return m_scene && m_scene->mirrorLineValid; }

eng::mirror::Plane Engine3D::mirrorLine() const { return m_scene ? m_scene->mirrorLine : Plane {}; }

void Engine3D::mirrorLineChanged() {
  ++m_viewEpoch;
  emit mirrorChanged();
  requestRedraw();
}

void Engine3D::setMirrorActive(const bool on) {
  if (on == m_mirrorOn)
    return;
  m_mirrorOn = on;
  m_mirrorDrag = {};
  m_mirrorHoverValid = false;
  mirrorLineChanged();
}

void Engine3D::setMirrorLine(const eng::mirror::Plane &p) {
  if (!m_scene || (m_scene->mirrorLineValid && m_scene->mirrorLine == p))
    return;
  m_scene->mirrorLineValid = true;
  m_scene->mirrorLine = p;
  m_scene->mirrorActiveSet = 0; // a new line placement starts a new mirror
  m_mirrorHoverValid = false;
  mirrorLineChanged();
}

void Engine3D::resetMirrorLine() {
  if (!m_scene || !m_scene->mirrorLineValid)
    return;
  m_scene->mirrorLineValid = false;
  m_scene->mirrorActiveSet = 0;
  m_mirrorDrag = {};
  mirrorLineChanged();
}

void Engine3D::setMirrorTextures(const bool on) {
  if (m_mirrorTextures == on)
    return;
  m_mirrorTextures = on;
  emit mirrorChanged();
}

bool Engine3D::mirrorLineNear(const int plane, const glm::dvec2 &p, const double grab) const {
  if (!mirrorLineValid())
    return false;
  const Plane l = mirrorLine();
  const ViewBasis bs = viewBasisFor(plane);
  if (std::fabs(bs.right[l.axis]) > 0.5)
    return std::fabs(p.x - l.d * bs.right[l.axis]) <= grab;
  if (std::fabs(bs.up[l.axis]) > 0.5)
    return std::fabs(p.y - l.d * bs.up[l.axis]) <= grab;
  return false; // the plane faces this view
}

bool Engine3D::beginMirrorDrag(const int plane, const glm::dvec2 &p, const double step, const double grab) {
  m_mirrorDrag = {};
  if (!m_mirrorOn)
    return false;
  const bool placing = !mirrorLineValid();
  if (!placing && !mirrorLineNear(plane, p, grab))
    return false;
  m_mirrorDrag.plane = plane;
  m_mirrorDrag.move = !placing;
  m_mirrorDrag.step = step > 0.0 ? step : 1.0;
  m_mirrorDrag.start = glm::dvec2(snapTo(p.x, m_mirrorDrag.step), snapTo(p.y, m_mirrorDrag.step));
  if (m_mirrorDrag.move) {
    // The line keeps its direction; only its position follows.
    const ViewBasis bs = viewBasisFor(plane);
    m_mirrorDrag.active = true;
    m_mirrorDrag.alongRight = std::fabs(bs.up[mirrorLine().axis]) > 0.5;
  }
  return true;
}

void Engine3D::updateMirrorDrag(const glm::dvec2 &p) {
  if (m_mirrorDrag.plane < 0)
    return;
  if (!m_mirrorDrag.active) {
    const double da = p.x - m_mirrorDrag.start.x, db = p.y - m_mirrorDrag.start.y;
    if (std::fabs(da) < m_mirrorDrag.step && std::fabs(db) < m_mirrorDrag.step)
      return;
    // Latched once: the line never turns mid-drag.
    m_mirrorDrag.active = true;
    m_mirrorDrag.alongRight = std::fabs(da) > std::fabs(db);
  }
  const ViewBasis bs = viewBasisFor(m_mirrorDrag.plane);
  // A line along the view's right is the plane across its up and vice versa.
  const glm::dvec3 n = m_mirrorDrag.alongRight ? bs.up : bs.right;
  int axis = 0;
  for (int i = 1; i < 3; ++i)
    if (std::fabs(n[i]) > std::fabs(n[axis]))
      axis = i;
  const double k = snapTo(m_mirrorDrag.alongRight ? p.y : p.x, m_mirrorDrag.step);
  setMirrorLine({axis, k * n[axis]});
}

void Engine3D::endMirrorDrag() { m_mirrorDrag = {}; }

// 3D placement: like the cut tool, the plane stands across the face under the pointer, grid-snapped.
void Engine3D::hoverMirror3D(const float px, const float py) {
  if (!mirrorPlacing() || !m_scene)
    return;
  m_mirrorHoverPx = {px, py};
  glm::vec3 ro, rd;
  makeRay(px, py, std::max(1.0f, float(m_lastViewSize.width())), std::max(1.0f, float(m_lastViewSize.height())), ro,
          rd);
  float t = 0.0f;
  glm::vec3 n(0.0f);
  glm::dvec3 hit;
  int faceAxis = 1;
  if (raycastScene(ro, rd, t, n, nullptr)) {
    hit = glm::dvec3(ro + rd * t);
    const glm::dvec3 an = glm::abs(glm::dvec3(n));
    faceAxis = an.x >= an.y && an.x >= an.z ? 0 : an.y >= an.z ? 1 : 2;
  } else if (std::fabs(rd.y) > 1e-6f && (-ro.y / rd.y) > 0.0f) {
    hit = glm::dvec3(ro + rd * (-ro.y / rd.y)); // the ground
  } else {
    if (m_mirrorHoverValid) {
      m_mirrorHoverValid = false;
      mirrorLineChanged();
    }
    return;
  }
  const int u = (faceAxis + 1) % 3, v = (faceAxis + 2) % 3;
  int axis;
  if (faceAxis != 1) {
    axis = u == 1 ? v : u;
  } else {
    const glm::dvec3 right(m_camera->getRight());
    axis = std::fabs(right[u]) >= std::fabs(right[v]) ? u : v;
  }
  if (m_mirrorTurned)
    axis = axis == u ? v : u;
  const double step = std::max(1e-3, gizmoSnapStep());
  const Plane p {axis, snapTo(hit[axis], step)};
  if (m_mirrorHoverValid && m_mirrorHover == p)
    return;
  m_mirrorHover = p;
  m_mirrorHoverValid = true;
  mirrorLineChanged();
}

bool Engine3D::clickMirror3D(const float px, const float py) {
  if (!mirrorPlacing())
    return false;
  hoverMirror3D(px, py);
  if (!m_mirrorHoverValid)
    return true;
  setMirrorLine(m_mirrorHover);
  return true;
}

void Engine3D::turnMirror3D() {
  m_mirrorTurned = !m_mirrorTurned;
  if (m_mirrorHoverValid)
    hoverMirror3D(m_mirrorHoverPx.x, m_mirrorHoverPx.y);
}

void Engine3D::leaveMirror3D() {
  if (!m_mirrorHoverValid)
    return;
  m_mirrorHoverValid = false;
  mirrorLineChanged();
}

std::vector<Engine3D::MirrorShownPlane> Engine3D::mirrorShownPlanes() const {
  std::vector<MirrorShownPlane> out;
  if (!m_scene)
    return out;
  if (m_mirrorOn) {
    if (mirrorLineValid())
      out.push_back({mirrorLine(), false});
    else if (m_mirrorHoverValid)
      out.push_back({m_mirrorHover, true});
    return out;
  }
  std::set<quint32> links;
  for (const quint32 id: selectedBrushIds())
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      links.insert(o->mirrorLink());
  for (const quint32 l: links)
    if (const auto *link = m_scene->findMirrorLink(l))
      if (std::none_of(out.begin(), out.end(), [&](const MirrorShownPlane &s) { return s.plane == link->plane; }))
        out.push_back({link->plane, false});
  return out;
}

std::shared_ptr<eng::GameObject> Engine3D::mirrorTwinOf(const eng::GameObject &s) const {
  if (!m_scene || !s.mirrorLink())
    return nullptr;
  const eng::Scene::MirrorLink *link = m_scene->findMirrorLink(s.mirrorLink());
  if (!link)
    return nullptr;
  auto [mn, mx] = objectBox(s);
  eng::mirror::box(eng::mirror::matrix(link->plane), mn, mx);
  for (const auto &o: m_scene->gameObjects)
    if (o && o.get() != &s && o->mirrorLink() == s.mirrorLink() && o->mirrorSide() != s.mirrorSide()
        && sameBox(objectBox(*o), {mn, mx}, 1e-3))
      return o;
  return nullptr;
}

int Engine3D::mirrorTwinFace(const eng::GameObject &s, const int f, const eng::GameObject &twin, const Plane &p) {
  const auto sp = worldFacePlanes(s);
  if (f < 0 || f >= int(sp.size()))
    return -1;
  glm::dvec3 n = sp[size_t(f)].first;
  if (glm::length(n) < 0.5)
    return -1;
  const glm::dvec3 pt = glm::dvec3(eng::mirror::matrix(p) * glm::dvec4(n * sp[size_t(f)].second, 1.0));
  n[p.axis] = -n[p.axis];
  const double d = glm::dot(n, pt);
  const auto tp = worldFacePlanes(twin);
  for (int k = 0; k < int(tp.size()); ++k)
    if (glm::dot(tp[size_t(k)].first, n) > 0.9999 && std::fabs(tp[size_t(k)].second - d) < 1e-3)
      return k;
  return -1;
}

std::vector<std::shared_ptr<eng::GameObject>> Engine3D::mirrorTwinObjects() const {
  std::vector<std::shared_ptr<eng::GameObject>> out;
  if (!m_scene)
    return out;
  const std::set<quint32> sel = selectedBrushIds();
  for (const quint32 id: sel)
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      if (auto t = mirrorTwinOf(*o); t && !sel.count(t->stableId()) && std::find(out.begin(), out.end(), t) == out.end())
        out.push_back(std::move(t));
  return out;
}

std::vector<std::pair<std::shared_ptr<eng::GameObject>, int>> Engine3D::mirrorTwinFaces() const {
  std::vector<std::pair<std::shared_ptr<eng::GameObject>, int>> out;
  if (!m_scene || !m_materialMode)
    return out;
  std::map<quint32, std::shared_ptr<eng::GameObject>> twins; // per source brush
  for (const quint32 pick: m_selectedFaces) {
    const auto s = m_scene->findById(eng::stableIdFromPicking(pick));
    if (!s || !s->mirrorLink() || !s->getMesh())
      continue;
    auto it = twins.find(s->stableId());
    if (it == twins.end())
      it = twins.emplace(s->stableId(), mirrorTwinOf(*s)).first;
    const auto &t = it->second;
    const eng::Scene::MirrorLink *link = m_scene->findMirrorLink(s->mirrorLink());
    if (!t || !link)
      continue;
    const int g = mirrorTwinFace(*s, faceIndexOfPicking(s->getMesh(), pick), *t, link->plane);
    if (g < 0 || m_selectedFaces.count(t->getMesh() ? t->getMesh()->getFacePickingID(g) : 0))
      continue;
    if (std::find(out.begin(), out.end(), std::make_pair(t, g)) == out.end())
      out.emplace_back(t, g);
  }
  return out;
}

std::vector<std::pair<glm::dvec3, glm::dvec3>> Engine3D::mirrorTwinBoxes() const {
  std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
  if (m_materialMode)
    return out;
  for (const auto &t: mirrorTwinObjects())
    out.push_back(objectBox(*t));
  return out;
}

std::vector<std::vector<glm::dvec3>> Engine3D::mirrorTwinFacePolygons() const {
  std::vector<std::vector<glm::dvec3>> out;
  std::vector<glm::vec3> poly;
  for (const auto &[t, g]: mirrorTwinFaces()) {
    const auto mesh = t->getMesh();
    if (!mesh || !mesh->getFacePolygon(g, poly) || poly.size() < 3)
      continue;
    const glm::dmat4 model(t->getModelMatrix());
    std::vector<glm::dvec3> w;
    w.reserve(poly.size());
    for (const glm::vec3 &v: poly)
      w.push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(v), 1.0)));
    out.push_back(std::move(w));
  }
  return out;
}

int Engine3D::selectedMirrorLinked() const {
  if (!m_scene)
    return 0;
  int n = 0;
  for (const quint32 id: selectedBrushIds())
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      ++n;
  return n;
}

QStringList Engine3D::selectedMirrorSetNames() const {
  QStringList names;
  if (!m_scene)
    return names;
  std::set<quint32> seen;
  for (const quint32 id: selectedBrushIds()) {
    const auto o = m_scene->findById(id);
    const eng::Scene::MirrorLink *l = o && o->mirrorLink() ? m_scene->findMirrorLink(o->mirrorLink()) : nullptr;
    if (!l || !seen.insert(l->set).second)
      continue;
    if (const eng::Scene::MirrorSet *set = m_scene->findMirrorSet(l->set); set && !set->name.isEmpty())
      names << set->name;
  }
  names.sort();
  return names;
}

void Engine3D::selectMirrorTwins(const bool add) {
  if (!m_scene)
    return;
  if (m_materialMode) {
    std::vector<quint32> faces;
    if (add)
      faces.assign(m_selectedFaces.begin(), m_selectedFaces.end());
    bool any = false;
    for (const auto &[t, g]: mirrorTwinFaces())
      if (const auto mesh = t->getMesh(); mesh && !t->hidden()) {
        faces.push_back(mesh->getFacePickingID(g));
        any = true;
      }
    if (any)
      setSelected({}, faces);
    return;
  }
  std::vector<quint32> objects;
  if (add)
    objects.assign(m_selectedObjects.begin(), m_selectedObjects.end());
  bool any = false;
  for (const auto &t: mirrorTwinObjects())
    if (!t->hidden()) {
      objects.push_back(t->stableId());
      any = true;
    }
  if (any)
    setSelected(objects, {});
}

void Engine3D::detachMirror() {
  if (!m_scene)
    return;
  std::set<quint32> links;
  for (const quint32 id: selectedBrushIds())
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      links.insert(o->mirrorLink());
  detachLinks(links, QStringLiteral("Detach mirror"));
}

void Engine3D::detachLinks(const std::set<quint32> &links, const QString &label) {
  if (!m_scene || links.empty())
    return;
  // Both sides of each link: one side alone would be meaningless.
  struct Tag {
    std::shared_ptr<eng::GameObject> o;
    quint32 link;
    int side;
  };
  std::vector<Tag> tags;
  for (const auto &o: m_scene->gameObjects)
    if (o && links.count(o->mirrorLink()))
      tags.push_back({o, o->mirrorLink(), o->mirrorSide()});
  if (tags.empty())
    return;
  for (const Tag &t: tags)
    t.o->setMirrorLink(0, 0);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        label,
        [this, tags] {
          for (const Tag &t: tags)
            t.o->setMirrorLink(t.link, t.side);
          emit mirrorChanged();
        },
        [this, tags] {
          for (const Tag &t: tags)
            t.o->setMirrorLink(0, 0);
          emit mirrorChanged();
        }));
  emit mirrorChanged();
  requestRedraw();
}

std::vector<quint32> Engine3D::mirrorSetMembers(const quint32 set) const {
  std::vector<quint32> out;
  if (!m_scene)
    return out;
  std::set<quint32> links;
  for (const auto &l: m_scene->mirrorLinks)
    if (l.set == set)
      links.insert(l.id);
  for (const auto &o: m_scene->gameObjects)
    if (o && o->mirrorLink() && links.count(o->mirrorLink()))
      out.push_back(o->stableId());
  return out;
}

std::set<quint32> Engine3D::selectedMirrorSets() const {
  std::set<quint32> out;
  if (!m_scene)
    return out;
  for (const quint32 id: selectedBrushIds())
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      if (const auto *l = m_scene->findMirrorLink(o->mirrorLink()); l && l->set)
        out.insert(l->set);
  return out;
}

void Engine3D::setMirrorSetsHidden(const std::set<quint32> &sets, const bool hide) {
  if (!m_scene || sets.empty())
    return;
  const MirrorTable before = mirrorTable();
  bool changed = false;
  for (const quint32 id: sets)
    if (auto *st = m_scene->findMirrorSet(id); st && st->hideMirrored != hide) {
      st->hideMirrored = hide;
      changed = true;
    }
  if (!changed)
    return;
  pushMirrorLinkTable(before, hide ? QStringLiteral("Hide mirrored copies") : QStringLiteral("Show mirrored copies"));
  emit mirrorChanged();
}

void Engine3D::renameMirrorSet(const quint32 set, const QString &name) {
  auto *st = m_scene ? m_scene->findMirrorSet(set) : nullptr;
  const QString n = name.trimmed();
  if (!st || n.isEmpty() || st->name == n)
    return;
  const MirrorTable before = mirrorTable();
  st->name = n;
  pushMirrorLinkTable(before, QStringLiteral("Rename mirror"));
  emit mirrorChanged();
}

void Engine3D::dissolveMirrorSet(const quint32 set) {
  if (!m_scene)
    return;
  std::set<quint32> links;
  for (const auto &l: m_scene->mirrorLinks)
    if (l.set == set)
      links.insert(l.id);
  detachLinks(links, QStringLiteral("Detach mirror"));
}

bool Engine3D::selectSameMirror(const bool add) {
  if (!m_scene)
    return false;
  std::set<quint32> sets = selectedMirrorSets();
  if (autoSelectContinuing())
    for (const quint32 seed: m_autoSel.seedPicks) {
      const auto o = m_scene->findById(m_materialMode ? eng::stableIdFromPicking(seed) : seed);
      if (o && o->mirrorLink())
        if (const auto *l = m_scene->findMirrorLink(o->mirrorLink()); l && l->set)
          sets.insert(l->set);
    }
  if (sets.empty())
    return false;
  std::vector<quint32> members;
  for (const quint32 s: sets)
    for (const quint32 id: mirrorSetMembers(s))
      if (const auto o = m_scene->findById(id); o && !o->hidden() && !o->locked())
        members.push_back(id);
  clearAutoSelect();
  if (m_materialMode) {
    std::set<quint32> faces;
    if (add)
      faces = m_selectedFaces;
    for (const quint32 id: members)
      if (const auto mesh = m_scene->findById(id)->getMesh())
        for (int f = 0; f < mesh->faceCount(); ++f)
          if (const quint32 pid = mesh->getFacePickingID(f))
            faces.insert(pid);
    setSelected({}, {faces.begin(), faces.end()});
  } else {
    std::set<quint32> objects(members.begin(), members.end());
    if (add)
      objects.insert(m_selectedObjects.begin(), m_selectedObjects.end());
    setSelected({objects.begin(), objects.end()}, {});
  }
  return true;
}

void Engine3D::refreshMirrorVisibility() {
  if (!m_scene)
    return;
  bool changed = false, hid = false;
  for (const auto &o: m_scene->gameObjects) {
    if (!o)
      continue;
    bool want = false;
    if (o->mirrorLink() && o->mirrorSide() == 1)
      if (const auto *l = m_scene->findMirrorLink(o->mirrorLink()))
        if (const auto *st = m_scene->findMirrorSet(l->set))
          want = st->hideMirrored;
    if (want == o->mirrorHidden())
      continue;
    o->setMirrorHidden(want);
    changed = true;
    hid = hid || want;
  }
  if (!changed)
    return;
  if (hid) {
    // Hidden things cannot stay selected.
    std::vector<quint32> objects, faces;
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->hidden())
        objects.push_back(id);
    for (const quint32 pid: m_selectedFaces)
      if (const auto o = m_scene->findById(eng::stableIdFromPicking(pid)); o && !o->hidden())
        faces.push_back(pid);
    if (objects.size() != m_selectedObjects.size() || faces.size() != m_selectedFaces.size())
      setSelected(objects, faces);
  }
  invalidateDerivedCaches();
  emit objectStateChanged();
  requestRedraw();
}

Engine3D::MirrorTable Engine3D::mirrorTable() const {
  if (!m_scene)
    return {};
  return {m_scene->mirrorLinks, m_scene->mirrorSets, m_scene->mirrorActiveSet};
}

void Engine3D::restoreMirrorTable(const MirrorTable &t) {
  if (!m_scene)
    return;
  m_scene->mirrorLinks = t.links;
  m_scene->mirrorSets = t.sets;
  m_scene->mirrorActiveSet = t.activeSet;
  emit mirrorChanged();
}

void Engine3D::pushMirrorLinkTable(const MirrorTable &before, const QString &label) {
  if (!m_scene || !m_undoStack || m_undoStack->isApplying())
    return;
  const MirrorTable after = mirrorTable();
  m_undoStack->push(std::make_unique<eng::LambdaCommand>(
      label, [this, before] { restoreMirrorTable(before); }, [this, after] { restoreMirrorTable(after); }));
}

quint32 Engine3D::mirrorSetForNewLink() {
  if (!m_scene)
    return 0;
  if (m_scene->findMirrorSet(m_scene->mirrorActiveSet))
    return m_scene->mirrorActiveSet;
  eng::Scene::MirrorSet st;
  st.id = m_scene->mintMirrorSetId();
  st.name = QStringLiteral("Mirror %1").arg(st.id);
  m_scene->mirrorSets.push_back(st);
  m_scene->mirrorActiveSet = st.id;
  return st.id;
}

std::vector<quint32> Engine3D::reflectInto(const std::vector<quint32> &ids, const eng::Scene::MirrorLink &link,
                                           const int side, const QString &label) {
  std::vector<quint32> out;
  if (!m_scene || ids.empty())
    return out;
  setSelected(ids, {});
  // A copy in place, then reflected: stairs are rebuilt from their parameters, brushes keep their mapping.
  std::vector<quint32> work = ids;
  std::vector<glm::vec3> basePos;
  for (const quint32 id: work) {
    const auto o = m_scene->findById(id);
    basePos.push_back(o ? o->getPosition() : glm::vec3(0.0f));
  }
  std::vector<DragFaceUv> baseUv;
  captureFaceUv(work, baseUv);
  DuplicateSet set;
  if (!spawnDuplicates(work, basePos, baseUv, set))
    return out;
  pushDuplicateUndo(set, label);
  transformSelectionBrushes(eng::mirror::matrix(link.plane), label,
                            link.mirrorTextures ? 0 : (1 << link.plane.axis));
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id)) {
      o->setMirrorLink(link.id, side);
      out.push_back(id);
    }
  return out;
}

void Engine3D::mirrorCreated(const std::vector<quint32> &created, const QString &label) {
  if (!m_scene || created.empty() || !mirrorApplies())
    return;
  // Only what is not linked yet (an edit of a linked brush is synced instead).
  std::vector<quint32> fresh;
  for (const quint32 id: created)
    if (const auto o = m_scene->findById(id); o && !o->mirrorLink())
      fresh.push_back(id);
  if (fresh.empty())
    return;
  glm::dvec3 mn {}, mx {};
  if (!objectsBounds(fresh, mn, mx))
    return;
  const Plane p = mirrorLine();
  if (eng::mirror::selfOverlapping(p, mn, mx))
    return; // across the line: it is its own mirror image

  const MirrorTable before = mirrorTable();
  eng::Scene::MirrorLink link;
  link.id = m_scene->mintMirrorLinkId();
  link.plane = p;
  link.mirrorTextures = m_mirrorTextures;
  link.set = mirrorSetForNewLink();
  m_scene->mirrorLinks.push_back(link);
  m_mirrorFresh.insert(link.id);
  pushMirrorLinkTable(before, label);

  // Tag the originals before the copy, so a redo brings the tags back with the same objects.
  std::vector<quint32> originals;
  for (const quint32 id: fresh) {
    const auto o = m_scene->findById(id);
    if (!o)
      continue;
    if (o->procObject())
      for (const quint32 m: procMembers(o->procObject()))
        if (const auto mo = m_scene->findById(m))
          mo->setMirrorLink(link.id, 0);
    o->setMirrorLink(link.id, 0);
    originals.push_back(id);
  }
  const bool was = m_mirrorSyncing;
  m_mirrorSyncing = true;
  const std::vector<quint32> copies = reflectInto(originals, link, 1, label);
  m_mirrorSyncing = was;
  // Only what was made stays selected; redo must end on it too (the last part redone would select the copy).
  setSelected(created, {});
  if (!copies.empty() && m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [] {}, [this, created] { setSelected(created, {}); }));
  emit sceneChanged();
  emit mirrorChanged();
}

// Duplicates / paste of linked brushes: every copied unit gets its own link. One side copied: same mirror and
// line, twin made across it. Both sides copied: a new mirror, its line moved with the copy.
void Engine3D::relinkDuplicates(const DuplicateSet &set, const QString &label) {
  if (!m_scene || set.sources.size() != set.clones.size())
    return;
  std::vector<MirrorCopy> copies;
  for (size_t i = 0; i < set.clones.size(); ++i) {
    const auto src = m_scene->findById(set.sources[i]);
    const auto *link = src && src->mirrorLink() ? m_scene->findMirrorLink(src->mirrorLink()) : nullptr;
    if (!link || !set.clones[i])
      continue;
    MirrorCopy c;
    c.clone = set.clones[i];
    c.link = *link;
    c.side = src->mirrorSide();
    // A duplicate is a pure translation: the copy's offset.
    const auto [cmn, cmx] = objectBox(*set.clones[i]);
    const auto [smn, smx] = objectBox(*src);
    c.offset = (cmn + cmx - smn - smx) * 0.5;
    if (const auto *st = m_scene->findMirrorSet(link->set))
      c.set = *st;
    copies.push_back(std::move(c));
  }
  relinkCopies(copies, label);
}

void Engine3D::relinkCopies(const std::vector<MirrorCopy> &copies, const QString &label) {
  if (!m_scene || copies.empty())
    return;
  std::map<quint32, std::vector<const MirrorCopy *>> byLink;
  for (const MirrorCopy &c: copies)
    if (c.clone)
      byLink[c.link.id].push_back(&c);
  if (byLink.empty())
    return;

  const MirrorTable before = mirrorTable();
  std::map<quint32, quint32> newSetFor;  // both-sided copies: one new set per original set
  std::map<quint32, quint32> lostSetFor; // one-sided copies whose set is gone (cut, other map)
  auto mintSet = [this](const MirrorCopy &c) {
    eng::Scene::MirrorSet ns;
    ns.id = m_scene->mintMirrorSetId();
    ns.name = QStringLiteral("Mirror %1").arg(ns.id);
    ns.hideMirrored = c.set.hideMirrored;
    m_scene->mirrorSets.push_back(ns);
    return ns.id;
  };
  struct Tag {
    std::shared_ptr<eng::GameObject> o;
    quint32 link;
    int side;
  };
  std::vector<Tag> tags;
  std::vector<std::pair<eng::Scene::MirrorLink, std::pair<std::vector<quint32>, int>>> missing;
  for (const auto &[oldId, entries]: byLink) {
    const MirrorCopy &first = *entries.front();
    bool sides[2] = {false, false};
    for (const MirrorCopy *c: entries)
      sides[c->side] = true;
    const bool both = sides[0] && sides[1];

    eng::Scene::MirrorLink nl = first.link;
    nl.id = m_scene->mintMirrorLinkId();
    if (both) {
      // Both sides copied: a mirror of its own, its line moved.
      auto st = newSetFor.find(first.link.set);
      if (st == newSetFor.end())
        st = newSetFor.emplace(first.link.set, mintSet(first)).first;
      nl.plane.d = first.link.plane.d + first.offset[first.link.plane.axis];
      nl.set = st->second;
    } else if (!m_scene->findMirrorSet(first.link.set)) {
      // One side, its mirror gone: a new mirror on the same line.
      auto st = lostSetFor.find(first.link.set);
      if (st == lostSetFor.end())
        st = lostSetFor.emplace(first.link.set, mintSet(first)).first;
      nl.set = st->second;
    }
    m_scene->mirrorLinks.push_back(nl);
    m_mirrorFresh.insert(nl.id);

    std::vector<quint32> sideIds[2];
    for (const MirrorCopy *c: entries) {
      c->clone->setMirrorLink(nl.id, c->side);
      tags.push_back({c->clone, nl.id, c->side});
      sideIds[c->side].push_back(c->clone->stableId());
    }
    if (!both)
      missing.push_back({nl, {sides[0] ? sideIds[0] : sideIds[1], sides[0] ? 1 : 0}});
  }
  pushMirrorLinkTable(before, label);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        label,
        [tags] {
          for (const Tag &t: tags)
            t.o->setMirrorLink(0, 0);
        },
        [tags] {
          for (const Tag &t: tags)
            t.o->setMirrorLink(t.link, t.side);
        }));
  // One side copied: its twins, across the line.
  const bool was = m_mirrorSyncing;
  m_mirrorSyncing = true;
  for (const auto &[link, what]: missing)
    reflectInto(what.first, link, what.second, label);
  m_mirrorSyncing = was;
  emit mirrorChanged();
}

void Engine3D::pushDuplicateUndoMirrored(DuplicateSet &set, const QString &label) {
  if (!set.entityClones.empty()) {
    // Copied entities: one step with the copied brushes.
    const bool record = m_undoStack && !m_undoStack->isApplying();
    std::vector<eng::Entity> ents;
    for (const quint32 id: set.entityClones)
      if (const eng::Entity *e = m_scene ? m_scene->findEntity(id) : nullptr)
        ents.push_back(*e);
    const std::vector<quint32> copies = set.entityClones, prev = set.prevEntities;
    if (record) {
      m_undoStack->beginMacro(label);
      auto apply = [this, ents, copies, prev](const bool add) {
        if (!m_scene)
          return;
        auto &v = m_scene->entities;
        for (const auto &e: ents) {
          v.erase(std::remove_if(v.begin(), v.end(), [&e](const eng::Entity &x) { return x.id == e.id; }), v.end());
          if (add)
            v.push_back(e);
        }
        ++m_entityEpoch;
        emit entitiesChanged();
        setSelectedEntities(add ? copies : prev);
      };
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply] { apply(false); },
                                                             [apply] { apply(true); }));
    }
    DuplicateSet brushes = set;
    brushes.entityClones.clear();
    brushes.prevEntities.clear();
    set = {};
    if (!brushes.clones.empty())
      pushDuplicateUndoMirrored(brushes, label);
    if (record)
      m_undoStack->endMacro();
    setSelectedEntities(copies);
    return;
  }
  std::vector<quint32> ids;
  bool linked = false;
  for (size_t i = 0; i < set.clones.size(); ++i) {
    ids.push_back(set.clones[i]->stableId());
    if (i < set.sources.size())
      if (const auto src = m_scene ? m_scene->findById(set.sources[i]) : nullptr; src && src->mirrorLink())
        linked = true;
  }
  // Copies of a selected modified object make a new one; copies of source brushes stay in theirs.
  bool hosted = false;
  for (const auto &c: set.clones)
    hosted = hosted || (c->modHost() && c->modHost() != m_editSourceHost && set.wholeHosts.count(c->modHost()));
  if (!linked && !hosted && !mirrorApplies()) {
    pushDuplicateUndo(set, label);
    return;
  }
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  const DuplicateSet copy = set; // pushDuplicateUndo empties it
  pushDuplicateUndo(set, label);
  relinkDuplicates(copy, label);
  const std::vector<quint32> hosts = hosted ? relinkHostDuplicates(copy, label) : std::vector<quint32> {};
  // Brush copies stay selected; copied modified objects are selected as such.
  const std::set<quint32> made(hosts.begin(), hosts.end());
  std::vector<quint32> plain;
  for (const auto &c: copy.clones)
    if (!made.count(c->modHost()))
      plain.push_back(c->stableId());
  setSelected(plain, {});
  if (mirrorApplies())
    mirrorCreated(plain, label); // the clones that are not linked yet get their copy
  setSelected(plain, {});
  if (record) {
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [] {}, [this, plain, hosts] {
      setSelected(plain, {});
      setSelectedMods(hosts);
    }));
    m_undoStack->endMacro();
  }
  if (!hosts.empty())
    setSelectedMods(hosts);
}

QString Engine3D::mirrorSelectionAcrossLine() {
  if (!m_scene)
    return {};
  if (!mirrorLineValid()) {
    setMirrorActive(true);
    return tr("No mirror line");
  }
  const std::set<quint32> sel = selectedBrushIds();
  if (sel.empty())
    return tr("Nothing selected");
  return mirrorUnits(sel, mirrorLine(), 0, m_mirrorTextures, tr("Mirror selection"));
}

quint32 Engine3D::mirrorSetToAddTo() const {
  if (!m_scene || m_materialMode)
    return 0;
  quint32 set = 0;
  bool unlinked = false;
  for (const quint32 id: m_selectedObjects) {
    const auto o = m_scene->findById(id);
    if (!o || o->isDerived())
      continue;
    if (!o->mirrorLink()) {
      unlinked = true;
      continue;
    }
    const eng::Scene::MirrorLink *l = m_scene->findMirrorLink(o->mirrorLink());
    if (!l)
      continue;
    if (set && l->set != set)
      return 0; // two mirrors: which one is meant?
    set = l->set;
  }
  return unlinked ? set : 0;
}

QString Engine3D::addSelectionToMirrorSet() {
  const quint32 set = mirrorSetToAddTo();
  if (!set)
    return tr("No mirror to add to");
  // Plane and texture rule from a selected brush of it.
  const eng::Scene::MirrorLink *ref = nullptr;
  std::set<quint32> add;
  // The modified object all selected brushes of the mirror are sources of (0: none, or several).
  quint32 host = 0;
  bool oneHost = true;
  for (const quint32 id: m_selectedObjects) {
    const auto o = m_scene->findById(id);
    if (!o || o->isDerived())
      continue;
    if (!o->mirrorLink()) {
      add.insert(id);
    } else if (const eng::Scene::MirrorLink *l = m_scene->findMirrorLink(o->mirrorLink()); l && l->set == set) {
      if (!ref)
        ref = l;
      if (!o->modHost() || (host && o->modHost() != host))
        oneHost = false;
      host = o->modHost();
    }
  }
  if (!ref || add.empty())
    return tr("Nothing to add");
  const eng::Scene::MirrorSet *ms = m_scene->findMirrorSet(set);
  const QString label = tr("Add to %1").arg(ms ? ms->name : tr("mirror"));
  // Added to a mirror of one modified object's sources: they (and their copies) become its sources too.
  std::vector<std::shared_ptr<eng::GameObject>> joining;
  if (oneHost && host && m_scene->findModHost(host))
    for (const quint32 id: add)
      if (const auto o = m_scene->findById(id); o && !o->modHost())
        joining.push_back(o);
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  if (!joining.empty()) {
    auto tag = [this, joining](const quint32 v) {
      for (const auto &o: joining)
        o->setModHost(v);
      if (m_scene)
        ++m_scene->modEpoch;
      refreshModVisibility();
    };
    tag(host);
    if (record)
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [tag] { tag(0); },
                                                             [tag, host] { tag(host); }));
  }
  const QString result = mirrorUnits(add, ref->plane, set, ref->mirrorTextures, label);
  if (record)
    m_undoStack->endMacro();
  if (!joining.empty())
    pumpModifiers(ModPump::Blocking);
  return result;
}

QString Engine3D::mirrorUnits(const std::set<quint32> &sel, const Plane &plane, const quint32 intoSet,
                              const bool mirrorTextures, const QString &label) {
  const glm::dmat4 m = eng::mirror::matrix(plane);

  // Units: a generated object as a whole, else one brush.
  std::vector<std::vector<quint32>> units;
  std::set<quint32> procs;
  for (const quint32 id: sel) {
    const auto o = m_scene->findById(id);
    if (!o)
      continue;
    if (o->procObject()) {
      if (procs.insert(o->procObject()).second)
        units.push_back(procMembers(o->procObject()));
    } else {
      units.push_back({id});
    }
  }

  const std::vector<quint32> selObjects(m_selectedObjects.begin(), m_selectedObjects.end());
  const std::vector<quint32> selFaces(m_selectedFaces.begin(), m_selectedFaces.end());
  const MirrorTable tableBefore = mirrorTable();
  struct Tag {
    std::shared_ptr<eng::GameObject> o;
    quint32 oldLink;
    int oldSide;
    quint32 newLink;
    int newSide;
  };
  std::vector<Tag> tags;
  auto retag = [&](const std::shared_ptr<eng::GameObject> &o, const quint32 link, const int side) {
    tags.push_back({o, o->mirrorLink(), o->mirrorSide(), link, side});
    o->setMirrorLink(link, side);
  };
  // An existing brush linked as a twin joins its original's modified object.
  struct HostTag {
    std::shared_ptr<eng::GameObject> o;
    quint32 oldHost, newHost;
  };
  std::vector<HostTag> hostTags;
  auto newLink = [&] {
    eng::Scene::MirrorLink l;
    l.id = m_scene->mintMirrorLinkId();
    l.plane = plane;
    l.mirrorTextures = mirrorTextures;
    l.set = intoSet ? intoSet : mirrorSetForNewLink();
    m_scene->mirrorLinks.push_back(l);
    m_mirrorFresh.insert(l.id);
    return l;
  };

  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  const bool was = m_mirrorSyncing;
  m_mirrorSyncing = true;
  int created = 0, linked = 0, replaced = 0, skipped = 0;

  for (const std::vector<quint32> &unit: units) {
    if (unit.empty())
      continue;
    const auto first = m_scene->findById(unit.front());
    if (!first)
      continue;
    if (first->mirrorLink()) {
      const eng::Scene::MirrorLink *l = m_scene->findMirrorLink(first->mirrorLink());
      if (l && l->plane == plane) {
        ++skipped; // already mirrored across this line
        continue;
      }
      // Linked across another line: that link ends (its brushes stay).
      const quint32 old = first->mirrorLink();
      for (const auto &o: m_scene->gameObjects)
        if (o && o->mirrorLink() == old)
          retag(o, 0, 0);
    }

    // A plain brush whose mirror image exists: link to it, or replace it when its materials differ.
    if (unit.size() == 1 && !first->procObject()) {
      auto r = eng::ConvexBrush::fromObject(*first);
      if (r) {
        r->transform(m, true);
        glm::dvec3 rmn(1e300), rmx(-1e300);
        for (const auto &poly: r->polygons)
          for (const auto &v: poly) {
            rmn = glm::min(rmn, v);
            rmx = glm::max(rmx, v);
          }
        std::shared_ptr<eng::GameObject> match;
        std::vector<std::pair<size_t, int>> faceMap; // reflected face -> mesh face of the match
        for (const auto &o: m_scene->gameObjects) {
          if (!o || o == first || o->mirrorLink() || o->procObject() || sel.count(o->stableId())
              || !sameBox(objectBox(*o), {rmn, rmx}, 1e-3))
            continue;
          const auto planes = worldFacePlanes(*o);
          std::vector<std::pair<size_t, int>> map;
          bool all = true;
          for (size_t f = 0; f < r->faces.size() && all; ++f) {
            int g = -1;
            for (int k = 0; k < int(planes.size()); ++k)
              if (glm::dot(planes[size_t(k)].first, r->faces[f].normal) > 0.9999
                  && std::fabs(planes[size_t(k)].second - r->faces[f].distance) < 1e-3) {
                g = k;
                break;
              }
            if (g < 0)
              all = false;
            else
              map.emplace_back(f, g);
          }
          if (all) {
            match = o;
            faceMap = std::move(map);
            break;
          }
        }
        if (match) {
          const auto mesh = match->getMesh();
          bool same = true;
          for (const auto &[f, g]: faceMap) {
            const auto &a = r->faces[f].material;
            const auto &b = mesh->faceMaterialRef(g);
            if ((a ? a->name : QString()) != (b ? b->name : QString()))
              same = false;
          }
          if (same) {
            const eng::Scene::MirrorLink l = newLink();
            retag(first, l.id, 0);
            retag(match, l.id, 1);
            if (match->modHost() != first->modHost()) {
              hostTags.push_back({match, match->modHost(), first->modHost()});
              match->setModHost(first->modHost());
              ++m_scene->modEpoch;
            }
            ++linked;
            continue;
          }
          replaceObjects({match->stableId()}, {}, label);
          ++replaced;
        }
      }
    }

    const eng::Scene::MirrorLink l = newLink();
    for (const quint32 id: unit)
      if (const auto o = m_scene->findById(id))
        retag(o, l.id, 0);
    reflectInto(unit, l, 1, label);
    ++created;
  }

  m_mirrorSyncing = was;
  if (!hostTags.empty())
    refreshModVisibility();
  setSelected(selObjects, selFaces);
  if (record) {
    pushMirrorLinkTable(tableBefore, label);
    if (!tags.empty())
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(
          label,
          [this, tags] {
            for (auto it = tags.rbegin(); it != tags.rend(); ++it)
              it->o->setMirrorLink(it->oldLink, it->oldSide);
            emit mirrorChanged();
          },
          [this, tags] {
            for (const Tag &t: tags)
              t.o->setMirrorLink(t.newLink, t.newSide);
            emit mirrorChanged();
          }));
    if (!hostTags.empty())
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(
          label,
          [this, hostTags] {
            for (const HostTag &t: hostTags)
              t.o->setModHost(t.oldHost);
            if (m_scene)
              ++m_scene->modEpoch;
            refreshModVisibility();
          },
          [this, hostTags] {
            for (const HostTag &t: hostTags)
              t.o->setModHost(t.newHost);
            if (m_scene)
              ++m_scene->modEpoch;
            refreshModVisibility();
          }));
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        label, [] {}, [this, selObjects, selFaces] { setSelected(selObjects, selFaces); }));
    m_undoStack->endMacro();
  }
  emit sceneChanged();
  emit mirrorChanged();
  requestRedraw();

  QStringList parts;
  if (created)
    parts << tr("mirrored %1").arg(created);
  if (linked)
    parts << tr("linked to %1 existing").arg(linked);
  if (replaced)
    parts << tr("replaced %1").arg(replaced);
  if (skipped)
    parts << tr("%1 already mirrored").arg(skipped);
  return parts.isEmpty() ? tr("Nothing to mirror") : tr("Mirror: %1").arg(parts.join(QStringLiteral(" · ")));
}

void Engine3D::beginMirrorPreview(const std::vector<quint32> &dragIds) {
  endMirrorPreview();
  if (!m_scene || dragIds.empty())
    return;
  const std::set<quint32> drag(dragIds.begin(), dragIds.end());
  // Links with both sides in the drag move as a whole: nothing to preview.
  std::set<std::pair<quint32, int>> sides;
  for (const quint32 id: drag)
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      sides.insert({o->mirrorLink(), o->mirrorSide()});
  for (const quint32 id: drag) {
    const auto s = m_scene->findById(id);
    if (!s || !s->mirrorLink() || sides.count({s->mirrorLink(), 1 - s->mirrorSide()}))
      continue;
    const eng::Scene::MirrorLink *link = m_scene->findMirrorLink(s->mirrorLink());
    const auto o = mirrorTwinOf(*s);
    if (!link || !o
        || std::any_of(m_mirrorPreview.begin(), m_mirrorPreview.end(),
                       [&](const MirrorPreviewPair &p) { return p.twin == o; }))
      continue;
    MirrorPreviewPair p;
    p.source = s;
    p.twin = o;
    p.plane = link->plane;
    p.mirrorTextures = link->mirrorTextures;
    p.twinMesh = o->getMesh();
    p.pos = o->getPosition();
    p.rot = o->getRotation();
    p.scale = o->getScale();
    m_mirrorPreview.push_back(std::move(p));
  }
}

void Engine3D::updateMirrorPreview() {
  if (m_mirrorPreview.empty())
    return;
  for (MirrorPreviewPair &p: m_mirrorPreview) {
    auto b = eng::ConvexBrush::fromObject(*p.source);
    if (!b)
      continue;
    b->transform(eng::mirror::matrix(p.plane), true);
    if (!p.mirrorTextures)
      for (auto &f: b->faces)
        if (f.basis.valid)
          eng::unmirrorBasis(f.basis, 1 << p.plane.axis);
    auto mesh = b->toMesh();
    if (!mesh || mesh->faceCount() == 0)
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      mesh->setFacePickingID(f, eng::makePickingId(p.twin->stableId(), uint32_t(f)));
    p.twin->setMesh(mesh);
    p.twin->setPosition(glm::vec3(0.0f));
    p.twin->setRotation(glm::vec3(0.0f));
    p.twin->setScale(glm::vec3(1.0f));
    p.twin->computeWorldAABB();
  }
  requestRedraw();
}

void Engine3D::endMirrorPreview() {
  if (m_mirrorPreview.empty())
    return;
  for (MirrorPreviewPair &p: m_mirrorPreview) {
    p.twin->setMesh(p.twinMesh);
    p.twin->setPosition(p.pos);
    p.twin->setRotation(p.rot);
    p.twin->setScale(p.scale);
    p.twin->computeWorldAABB();
  }
  m_mirrorPreview.clear();
  requestRedraw();
}

std::optional<Plane> Engine3D::fitMirrorPlane(const std::vector<quint32> &side0, const std::vector<quint32> &side1,
                                              const int preferAxis) const {
  glm::dvec3 a0, b0, a1, b1;
  if (side0.empty() || side1.empty() || !objectsBounds(side0, a0, b0) || !objectsBounds(side1, a1, b1))
    return std::nullopt;
  auto boxes = [this](const std::vector<quint32> &ids) {
    std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
    for (const quint32 id: ids)
      if (const auto o = m_scene->findById(id); o && o->getMesh())
        out.push_back(objectBox(*o));
    return out;
  };
  const auto boxes0 = boxes(side0), boxes1 = boxes(side1);
  // The plane sits midway; it fits when the reflected bounds land on the other side's, checked brush by brush
  // (overall bounds can fit two axes).
  std::optional<Plane> loose;
  for (int k = 0; k < 3; ++k) {
    const int ax = (std::clamp(preferAxis, 0, 2) + k) % 3;
    const Plane p {ax, (a0[ax] + b0[ax] + a1[ax] + b1[ax]) * 0.25};
    const glm::dmat4 m = eng::mirror::matrix(p);
    glm::dvec3 ra = a0, rb = b0;
    eng::mirror::box(m, ra, rb);
    if (!sameBox({ra, rb}, {a1, b1}, 1e-3))
      continue;
    const bool each = std::all_of(boxes0.begin(), boxes0.end(), [&](const auto &b) {
      glm::dvec3 lo = b.first, hi = b.second;
      eng::mirror::box(m, lo, hi);
      return std::any_of(boxes1.begin(), boxes1.end(), [&](const auto &c) { return sameBox({lo, hi}, c, 1e-3); });
    });
    if (each)
      return p;
    if (!loose)
      loose = p;
  }
  return loose;
}

void Engine3D::syncMirrors(eng::UndoCommand &cmd) {
  if (m_mirrorSyncing)
    return;
  const std::set<quint32> fresh = std::move(m_mirrorFresh);
  m_mirrorFresh.clear();
  if (!m_scene)
    return;
  eng::Touched t;
  cmd.touched(t);
  if (t.objects.empty() && t.moved.empty() && t.faces.empty())
    return;

  struct Hit {
    bool structure = false;
    std::set<quint32> faces;
  };
  std::map<std::pair<quint32, int>, Hit> hits;
  for (const auto &o: t.objects)
    if (o && o->mirrorLink())
      hits[{o->mirrorLink(), o->mirrorSide()}].structure = true;
  for (const quint32 id: t.moved)
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      hits[{o->mirrorLink(), o->mirrorSide()}].structure = true;
  for (const quint32 id: t.faces)
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      hits[{o->mirrorLink(), o->mirrorSide()}].faces.insert(id);
  std::set<quint32> links;
  for (const auto &[key, h]: hits)
    if (!fresh.count(key.first))
      links.insert(key.first);
  if (links.empty())
    return;

  m_mirrorSyncing = true;
  const std::vector<quint32> selObjects(m_selectedObjects.begin(), m_selectedObjects.end());
  const std::vector<quint32> selFaces(m_selectedFaces.begin(), m_selectedFaces.end());
  const QString label = cmd.text();
  bool changed = false;

  auto members = [this](const quint32 link, const int side) {
    std::vector<quint32> ids;
    for (const auto &o: m_scene->gameObjects)
      if (o && o->mirrorLink() == link && o->mirrorSide() == side)
        ids.push_back(o->stableId());
    return ids;
  };

  for (const quint32 linkId: links) {
    const eng::Scene::MirrorLink *lp = m_scene->findMirrorLink(linkId);
    if (!lp)
      continue;
    const eng::Scene::MirrorLink link = *lp;
    const auto h0 = hits.find({linkId, 0}), h1 = hits.find({linkId, 1});
    const std::vector<quint32> side0 = members(linkId, 0), side1 = members(linkId, 1);

    if (h0 != hits.end() && h1 != hits.end()) {
      // Both sides edited together: no rebuild; the plane follows if they are still mirror images.
      if (side0.empty() || side1.empty())
        continue;
      const std::optional<Plane> fit = fitMirrorPlane(side0, side1, link.plane.axis);
      if (!fit) {
        // No axis fits between the sides: the line cannot follow.
        if (!t.objects.empty() || !t.moved.empty()) {
          const eng::Scene::MirrorSet *set = m_scene->findMirrorSet(link.set);
          emit toastRequested(tr("%1: the mirror line can only lie along X, Y or Z, so it stays where it was")
                                  .arg(set ? set->name : tr("Mirror")));
        }
        continue;
      }
      if (fit->axis == link.plane.axis && std::fabs(fit->d - link.plane.d) < 1e-9)
        continue;
      const MirrorTable before = mirrorTable();
      m_scene->findMirrorLink(linkId)->plane = *fit;
      pushMirrorLinkTable(before, label);
      changed = true;
      continue;
    }

    const int src = h0 != hits.end() ? 0 : 1;
    const Hit &h = src == 0 ? h0->second : h1->second;
    const std::vector<quint32> &from = src == 0 ? side0 : side1;
    const std::vector<quint32> &to = src == 0 ? side1 : side0;

    if (!h.structure && !to.empty()) {
      // Face attributes only: copied onto the matching faces.
      std::vector<ObjectState> before, after;
      bool ok = true;
      for (const quint32 sid: h.faces) {
        const auto s = m_scene->findById(sid);
        if (!s || !s->getMesh()) {
          ok = false;
          break;
        }
        const auto twin = mirrorTwinOf(*s);
        if (!twin || !twin->getMesh()) {
          ok = false;
          break;
        }
        const auto sp = worldFacePlanes(*s);
        const auto sm = s->getMesh();
        const auto tm = twin->getMesh();
        before.push_back(captureObjectState(twin->stableId(), true));
        std::vector<std::pair<int, int>> pairs;
        for (int f = 0; f < int(sp.size()) && ok; ++f) {
          if (glm::length(sp[size_t(f)].first) < 0.5)
            continue;
          const int g = mirrorTwinFace(*s, f, *twin, link.plane);
          if (g < 0)
            ok = false;
          else
            pairs.emplace_back(f, g);
        }
        if (!ok) {
          before.pop_back();
          break;
        }
        for (const auto &[f, g]: pairs) {
          tm->setFaceMaterial(g, sm->faceMaterialRef(f));
          tm->setFaceTexBasis(g, reflectBasis(sm->faceTexBasis(f), link.plane, link.mirrorTextures));
          tm->setFaceGroups(g, sm->faceGroups(f));
        }
        after.push_back(captureObjectState(twin->stableId(), true));
      }
      if (ok) {
        if (!after.empty() && m_undoStack && !m_undoStack->isApplying())
          m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, before, after));
        changed = changed || !after.empty();
        continue;
      }
      // A face did not match: put back what was copied, then rebuild.
      if (!before.empty())
        applyObjectStates(before);
    }

    // Rebuild the other side from this one (generated objects whole, with their collision entity). A side
    // emptied by a delete takes the other with it. Copies keep their originals' modified object (cloneBrush).
    std::set<quint32> remove(to.begin(), to.end());
    std::set<quint32> procs;
    for (const quint32 id: to)
      if (const auto o = m_scene->findById(id); o && o->procObject())
        procs.insert(o->procObject());
    std::vector<quint32> entities;
    for (const quint32 pid: procs) {
      for (const quint32 mid: procMembers(pid))
        remove.insert(mid);
      const auto *p = m_scene->findProc(pid);
      const quint32 eid = p ? quint32(p->params.value(QStringLiteral("collisionEntity")).toDouble(0)) : 0;
      if (eid && m_scene->findEntity(eid)) {
        const auto owned = brushesOfEntity(eid);
        if (std::all_of(owned.begin(), owned.end(), [&](quint32 b) { return remove.count(b) != 0; }))
          entities.push_back(eid);
      }
    }
    if (!remove.empty())
      replaceObjects({remove.begin(), remove.end()}, {}, label);
    if (!entities.empty())
      deleteEntities(entities);
    if (!from.empty())
      reflectInto(from, link, 1 - src, label);
    changed = true;
  }

  if (changed) {
    // The selection stays; redo must end on it too (the last child redone would select the copies).
    setSelected(selObjects, selFaces);
    if (m_undoStack && !m_undoStack->isApplying())
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(
          label, [] {}, [this, selObjects, selFaces] { setSelected(selObjects, selFaces); }));
    emit sceneChanged();
    emit mirrorChanged();
  }
  m_mirrorSyncing = false;
}

// The 3D mirror overlay (collision overlay pipelines, own buffer): shown planes, linked-brush edges while the
// mode is on, the selection's twins filled. Rebuilt only when something it shows changed.
void Engine3D::buildMirrorOverlay() {
  if (!m_scene) {
    m_mirrorFillCount = m_mirrorEdgeCount = 0;
    return;
  }
  const std::vector<MirrorShownPlane> planes = mirrorShownPlanes();
  std::vector<std::shared_ptr<eng::GameObject>> twinObjects;
  std::vector<std::pair<std::shared_ptr<eng::GameObject>, int>> twinFaces;
  if (m_materialMode)
    twinFaces = mirrorTwinFaces();
  else
    twinObjects = mirrorTwinObjects();

  auto mix = [](quint64 seed, const quint64 v) { return seed * 0x9E3779B97F4A7C15ull + v + 0x165667B19E3779F9ull; };
  quint64 key = mix(0x51ED270B27u, m_mirrorOn ? 1u : 0u);
  for (const MirrorShownPlane &sp: planes) {
    key = mix(key, quint64(sp.plane.axis) * 2 + (sp.preview ? 1 : 0));
    key = mix(key, quint64(std::llround(sp.plane.d * 4096.0)));
  }
  if (!planes.empty())
    key = mix(key, quint64(std::llround(double(m_sceneBoundsMin.x + m_sceneBoundsMax.y + m_sceneBoundsMax.z) * 64.0)));
  key = mix(key, m_scene->structureVersion());
  key = mix(key, eng::GameObject::globalTransformEpoch());
  key = mix(key, eng::Mesh::globalGeometryEpoch());
  for (const auto &t: twinObjects)
    key = mix(key, t->stableId());
  for (const auto &[t, g]: twinFaces)
    key = mix(key, (quint64(t->stableId()) << 8) ^ quint64(g) ^ (1ull << 62));
  for (const auto &o: m_scene->gameObjects)
    if (o && o->mirrorLink())
      key = mix(key, (quint64(o->stableId()) << 24) ^ (quint64(o->mirrorLink()) << 1) ^ quint64(o->mirrorSide())
                         ^ (o->hidden() ? 1ull << 63 : 0ull));
  if (key == m_mirrorOverlayKey && m_mirrorVb)
    return;
  m_mirrorOverlayKey = key;
  m_mirrorFillCount = m_mirrorEdgeCount = 0;
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;

  const glm::vec3 kPlane(0.95f, 0.35f, 0.85f);
  std::vector<float> fill, edges;
  auto put = [](std::vector<float> &v, const glm::dvec3 &q, const glm::vec3 &col, const float a) {
    v.insert(v.end(), {float(q.x), float(q.y), float(q.z), col.r, col.g, col.b, a});
  };

  for (const MirrorShownPlane &sp: planes) {
    const Plane p = sp.plane;
    const float alpha = sp.preview ? 0.6f : 1.0f;
    glm::dvec3 mn(m_sceneBoundsMin), mx(m_sceneBoundsMax);
    if (!(mn.x <= mx.x) || mx.x - mn.x > 1e6) {
      mn = glm::dvec3(-64.0);
      mx = glm::dvec3(64.0);
    }
    const double pad = 128.0 / kSourceUnitsPerWorldUnit;
    mn -= glm::dvec3(pad);
    mx += glm::dvec3(pad);
    // Vertical dashes (32 units) on a 96-unit grid, so they stay put as the scene grows.
    const int i = p.axis;
    const int up = i == 1 ? 0 : 1;                // the dashes run along this axis
    const int across = i == 1 ? 2 : (i == 0 ? 2 : 0);
    const double period = 96.0 / kSourceUnitsPerWorldUnit, dash = 32.0 / kSourceUnitsPerWorldUnit;
    const double a0 = std::ceil(mn[across] / period) * period, u0 = std::floor(mn[up] / period) * period;
    int budget = 20000; // a huge map gets a sparse sheet, never a stall
    for (double h = a0; h <= mx[across] && budget > 0; h += period)
      for (double v = u0; v <= mx[up] && budget > 0; v += period, --budget) {
        const double lo = std::max(v, mn[up]), hi = std::min(v + dash, mx[up]);
        if (hi <= lo)
          continue;
        glm::dvec3 e0, e1;
        e0[i] = e1[i] = p.d;
        e0[across] = e1[across] = h;
        e0[up] = lo;
        e1[up] = hi;
        put(edges, e0, kPlane, 0.9f * alpha);
        put(edges, e1, kPlane, 0.9f * alpha);
      }
  }

  // Linked brushes: edges pushed a hair outwards against z-fighting.
  std::map<const eng::GameObject *, std::vector<int>> facesOf;
  for (const auto &[t, g]: twinFaces)
    facesOf[t.get()].push_back(g);
  for (const auto &o: m_scene->gameObjects) {
    if (!o || !o->mirrorLink() || o->hidden())
      continue;
    const bool twin = std::find(twinObjects.begin(), twinObjects.end(), o) != twinObjects.end();
    const auto ff = facesOf.find(o.get());
    if (!twin && ff == facesOf.end() && !m_mirrorOn)
      continue;
    const auto b = eng::ConvexBrush::fromObject(*o);
    if (!b)
      continue;
    glm::dvec3 c(0.0);
    size_t n = 0;
    for (const auto &poly: b->polygons)
      for (const auto &v: poly) {
        c += v;
        ++n;
      }
    if (n)
      c /= double(n);
    auto out = [&](const glm::dvec3 &v) {
      const glm::dvec3 d = v - c;
      const double len = glm::length(d);
      return len > 1e-9 ? v + d / len * 0.01 : v;
    };
    auto loop = [&](const std::vector<glm::dvec3> &poly, const float ea, const float fa) {
      for (size_t j = 0; j < poly.size(); ++j) {
        put(edges, out(poly[j]), kPlane, ea);
        put(edges, out(poly[(j + 1) % poly.size()]), kPlane, ea);
      }
      if (fa > 0.0f)
        for (size_t j = 1; j + 1 < poly.size(); ++j) {
          put(fill, out(poly[0]), kPlane, fa);
          put(fill, out(poly[j]), kPlane, fa);
          put(fill, out(poly[j + 1]), kPlane, fa);
        }
    };
    if (m_mirrorOn || twin)
      for (const auto &poly: b->polygons)
        loop(poly, twin ? 1.0f : 0.55f, twin ? 0.12f : 0.0f);
    if (ff != facesOf.end()) {
      // Twin face polygons (world space).
      const auto mesh = o->getMesh();
      const glm::dmat4 model(o->getModelMatrix());
      std::vector<glm::vec3> poly;
      for (const int g: ff->second) {
        if (!mesh || !mesh->getFacePolygon(g, poly) || poly.size() < 3)
          continue;
        std::vector<glm::dvec3> w;
        for (const glm::vec3 &v: poly)
          w.push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(v), 1.0)));
        loop(w, 1.0f, 0.3f);
      }
    }
  }

  if (fill.empty() && edges.empty())
    return;
  std::vector<float> all = std::move(fill);
  const quint32 fillCount = quint32(all.size() / 7);
  all.insert(all.end(), edges.begin(), edges.end());
  const quint32 bytes = quint32(all.size() * sizeof(float));
  if (!m_mirrorVb || bytes > m_mirrorVb->size()) {
    Shared::instance().retire(m_mirrorVb);
    m_mirrorVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, std::max(bytes * 2, 4096u));
    if (!m_mirrorVb->create()) {
      Shared::instance().retire(m_mirrorVb);
      m_mirrorVb = nullptr;
      m_mirrorOverlayKey = ~0ull;
      return;
    }
  }
  if (!m_mirrorBatch)
    m_mirrorBatch = rhi->nextResourceUpdateBatch();
  m_mirrorBatch->updateDynamicBuffer(m_mirrorVb, 0, bytes, all.data());
  m_mirrorFillCount = fillCount;
  m_mirrorEdgeCount = quint32(all.size() / 7) - fillCount;
}

void Engine3D::drawMirrorOverlay(QRhiCommandBuffer *cb) {
  if (!m_mirrorVb || !m_cutWireSrb || (!m_mirrorFillCount && !m_mirrorEdgeCount))
    return;
  const QRhiCommandBuffer::VertexInput vin(m_mirrorVb, 0);
  if (m_mirrorFillCount && m_collisionFillPipeline) {
    cb->setGraphicsPipeline(m_collisionFillPipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_mirrorFillCount);
  }
  if (m_mirrorEdgeCount && m_collisionEdgePipeline) {
    cb->setGraphicsPipeline(m_collisionEdgePipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_mirrorEdgeCount, 1, m_mirrorFillCount);
  }
}

} // namespace vk
