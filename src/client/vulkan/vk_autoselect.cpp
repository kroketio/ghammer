// Auto-select. Material mode grows an intent from the selected faces over shared edges; Object mode does the
// same for whole brushes (engine/brush_select). Editing the result's 2D box re-runs the query fenced by it.
// Each mode keeps its own run (m_autoSel / m_autoSelOther), restored on return while the selection is the result.

#include "vk_engine.h"

#include <QDebug>

#include "client/engine/face_graph.h"
#include "client/engine/face_select.h"
#include "client/engine/scene.h"

namespace vk {

namespace {
// The face graph is rebuilt when any of these move.
std::array<quint64, 6> graphKeyFor(const eng::Scene *scene) {
  quint64 hidden = 0, groups = 0;
  for (const auto &o: scene->gameObjects) {
    hidden = hidden * 31 + (o->hidden() ? o->stableId() : 0);
    if (const auto m = o->getMesh())
      groups += m->groupVersion() * 2654435761ull + o->stableId();
  }
  return {quint64(quintptr(scene)), scene->structureVersion(), eng::Mesh::globalGeometryEpoch(),
          eng::Mesh::globalFaceEpoch(), hidden, groups};
}
} // namespace

const eng::FaceGraph &Engine3D::faceGraph() {
  const auto key = m_scene ? graphKeyFor(m_scene.data()) : std::array<quint64, 6> {};
  if (!m_faceGraph || key != m_faceGraphKey) {
    QElapsedTimer t;
    t.start();
    m_faceGraph = std::make_unique<eng::FaceGraph>(m_scene ? eng::FaceGraph::fromScene(*m_scene) : eng::FaceGraph {});
    m_faceGraphKey = key;
    m_faceExposure = {};
    qDebug().noquote() << QStringLiteral("[autosel] face graph: %1 faces in %2 ms")
                              .arg(m_faceGraph->nodes.size())
                              .arg(t.elapsed());
  }
  return *m_faceGraph;
}

bool Engine3D::autoSelectContinuing() const {
  if (!m_autoSel.active)
    return false;
  return m_materialMode ? m_selectedFaces == m_autoSel.resultFaces : m_selectedObjects == m_autoSel.resultObjects;
}

void Engine3D::parkAutoSelect() {
  AutoSelectState &s = m_autoSel;
  s.parkedBox.reset();
  if (s.boxIsOurs && m_marquee.valid) {
    s.parkedBox = m_marquee;
    s.parkedBoxEdited = s.boxEdited;
    setAutoSelectMarquee(false, {}, {});
  }
  // The storey overlay belongs to Material mode; it comes back with it.
  s.storeysWereShown = m_storeysShown;
  if (m_storeysShown) {
    m_storeysShown = false;
    clearFill();
  }
  std::swap(m_autoSel, m_autoSelOther);
}

void Engine3D::unparkAutoSelect() {
  AutoSelectState &s = m_autoSel;
  if (s.parkedBox && autoSelectContinuing() && !m_marquee.valid) {
    m_settingAutoMarquee = true;
    setMarquee(*s.parkedBox);
    m_settingAutoMarquee = false;
    s.boxIsOurs = true;
    s.boxEdited = s.parkedBoxEdited;
  }
  s.parkedBox.reset();
  const bool storeys = s.storeysWereShown;
  s.storeysWereShown = false;
  if (storeys && m_materialMode && autoSelectIsFacade())
    showStoreys(); // emits autoSelectChanged
  else
    emit autoSelectChanged();
}

bool Engine3D::runAutoSelect(const int intent, const int exposure, const bool allSides, const quint32 groupId,
                             const bool contiguous, const bool add, const bool sameMaterial, const bool byObject) {
  if (!m_scene)
    return false;
  const eng::FaceGraph &g = faceGraph();
  const bool continuing = autoSelectContinuing();

  const bool userBox = m_marquee.valid && !m_autoSel.boxIsOurs;
  const bool editedBox = m_marquee.valid && m_autoSel.boxIsOurs && m_autoSel.boxEdited && continuing;
  glm::dvec3 bmn(0.0), bmx(0.0);
  if (m_marquee.valid)
    marqueeToWorld(m_marquee, bmn, bmx);
  auto insideBox = [&](const glm::dvec3 &c) {
    return !glm::any(glm::lessThan(c, bmn - 0.1)) && !glm::any(glm::greaterThan(c, bmx + 0.1));
  };

  if (!m_materialMode) {
    eng::sel::BrushQuery q;
    q.intent = eng::sel::BrushIntent(std::clamp(intent, 0, int(eng::sel::BrushIntent::Count) - 1));
    q.exposure = eng::sel::Exposure(std::clamp(exposure, 0, 2));
    q.allSides = allSides;
    q.contiguous = contiguous;
    q.sameMaterial = sameMaterial;
    if (continuing) {
      q.seeds = m_autoSel.seedPicks;
    } else {
      for (const quint32 id: m_selectedObjects) {
        if (userBox) {
          const auto obj = m_scene->findById(id);
          if (!obj)
            continue;
          const auto [lo, hi] = obj->getWorldAABB();
          if (!insideBox((glm::dvec3(lo) + glm::dvec3(hi)) * 0.5))
            continue;
        }
        q.seeds.push_back(id);
      }
    }
    if (userBox || editedBox) {
      q.hasRegion = true;
      q.regionMin = bmn;
      q.regionMax = bmx;
    }
    if (add)
      m_autoSel.baseObjects = continuing ? m_autoSel.baseObjects : m_selectedObjects;
    else
      m_autoSel.baseObjects.clear();
    m_autoSel.brushQuery = q;
    m_autoSel.seedPicks = q.seeds;
    m_autoSel.active = true;
    m_autoSel.hasRun = true;
    m_autoSel.lastIntent = int(q.intent);
    m_autoSel.lastExposure = int(q.exposure);
    if (!editedBox && !userBox && m_autoSel.boxIsOurs)
      setAutoSelectMarquee(false, {}, {});
    runAutoSelectQuery(true);
    return !m_autoSel.brushResult.empty();
  }

  if (intent == int(eng::sel::Intent::Group) && (groupId & kMeshGroupPick)) {
    // The picked face's brush group: every face of its brushes.
    const quint32 mg = groupId & ~kMeshGroupPick;
    std::set<quint32> members;
    for (const auto &o: m_scene->gameObjects)
      if (o && o->group() == mg && !o->isDerived() && !o->locked())
        members.insert(o->stableId());
    std::set<quint32> faces;
    if (add)
      faces = m_selectedFaces;
    for (const quint32 pid: facePicksOfBrushes(members))
      faces.insert(pid);
    clearAutoSelect();
    setSelected({}, {faces.begin(), faces.end()});
    return !faces.empty();
  }

  eng::sel::Query q;
  q.intent = eng::sel::Intent(std::clamp(intent, 0, 4));
  q.exposure = eng::sel::Exposure(std::clamp(exposure, 0, 2));
  q.allSides = allSides;
  q.groupId = groupId;
  q.contiguous = contiguous;
  q.sameMaterial = sameMaterial;

  std::vector<quint32> seeds;
  if (continuing) {
    seeds = m_autoSel.seedPicks;
  } else {
    for (const quint32 pid: m_selectedFaces) {
      if (userBox) {
        const int n = g.indexOfPick(pid);
        if (n < 0)
          continue;
        if (!insideBox(g.nodes[size_t(n)].centroid))
          continue;
      }
      seeds.push_back(pid);
    }
  }
  if (userBox || editedBox) {
    q.hasRegion = true;
    q.regionMin = bmn;
    q.regionMax = bmx;
  }
  for (const quint32 pid: seeds)
    if (const int n = g.indexOfPick(pid); n >= 0)
      q.seeds.push_back(n);
  m_autoSel.byObjectProcs.clear();
  if (byObject)
    for (const quint32 pid: seeds)
      if (const auto o = m_scene->findById(eng::stableIdFromPicking(pid)); o && o->procObject())
        m_autoSel.byObjectProcs.insert(o->procObject());

  if (add)
    m_autoSel.baseFaces = continuing ? m_autoSel.baseFaces : m_selectedFaces;
  else
    m_autoSel.baseFaces.clear();
  m_autoSel.query = q;
  m_autoSel.seedPicks = seeds;
  m_autoSel.active = true;
  m_autoSel.hasRun = true;
  m_autoSel.lastIntent = int(q.intent);
  m_autoSel.lastExposure = int(q.exposure);
  if (!editedBox && !userBox && m_autoSel.boxIsOurs)
    setAutoSelectMarquee(false, {}, {});
  runAutoSelectQuery(true);
  return !m_autoSel.result.empty();
}

void Engine3D::runAutoSelectQuery(const bool fresh) {
  const eng::FaceGraph &g = faceGraph();
  QElapsedTimer t;
  t.start();
  if (!m_materialMode) {
    auto &q = m_autoSel.brushQuery;
    q.facadeTypes = m_autoSel.typeOn;
    m_autoSel.brushResult = eng::sel::selectBrushes(g, q, m_faceExposure);
    const auto &r = m_autoSel.brushResult;
    m_autoSel.typeCounts = r.typeCounts;
    std::vector<quint32> objects(m_autoSel.baseObjects.begin(), m_autoSel.baseObjects.end());
    objects.insert(objects.end(), r.objects.begin(), r.objects.end());
    setSelected(objects, {});
    m_autoSel.resultObjects = m_selectedObjects;
    if (fresh && !q.hasRegion && !r.empty())
      setAutoSelectMarquee(true, r.mn - 0.01, r.mx + 0.01);
    qDebug().noquote() << QStringLiteral("[autosel] brushes intent=%1 exposure=%2 contiguous=%3 seeds=%4 region=%5 -> "
                                         "%6 brush(es) in %7 ms %8")
                              .arg(int(q.intent))
                              .arg(int(q.exposure))
                              .arg(q.contiguous ? 1 : 0)
                              .arg(q.seeds.size())
                              .arg(q.hasRegion ? 1 : 0)
                              .arg(r.objects.size())
                              .arg(t.elapsed())
                              .arg(r.note);
    emit autoSelectChanged();
    return;
  }
  m_autoSel.result = eng::sel::select(g, m_autoSel.query, m_faceExposure);
  m_autoSel.typeCounts.fill(0);
  std::vector<quint32> faces(m_autoSel.baseFaces.begin(), m_autoSel.baseFaces.end());
  const auto &r = m_autoSel.result;
  const auto &procs = m_autoSel.byObjectProcs;
  for (size_t k = 0; k < r.faces.size(); ++k) {
    const quint32 pid = g.nodes[size_t(r.faces[k])].pickId;
    if (!procs.empty()) {
      const auto o = m_scene->findById(eng::stableIdFromPicking(pid));
      if (!o || !procs.count(o->procObject()))
        continue;
    }
    if (k < r.types.size()) {
      const int type = int(r.types[k]);
      ++m_autoSel.typeCounts[size_t(type)];
      if (!m_autoSel.typeOn[size_t(type)])
        continue;
    }
    faces.push_back(pid);
  }
  setSelected({}, faces);
  m_autoSel.resultFaces = m_selectedFaces;
  if (fresh) {
    if (!m_autoSel.query.hasRegion && !r.empty())
      setAutoSelectMarquee(true, r.mn - 0.01, r.mx + 0.01);
  }
  qDebug().noquote() << QStringLiteral("[autosel] intent=%1 exposure=%2 contiguous=%3 seeds=%4 region=%5 -> %6 face(s) "
                                       "in %7 ms %8")
                            .arg(int(m_autoSel.query.intent))
                            .arg(int(m_autoSel.query.exposure))
                            .arg(m_autoSel.query.contiguous ? 1 : 0)
                            .arg(m_autoSel.query.seeds.size())
                            .arg(m_autoSel.query.hasRegion ? 1 : 0)
                            .arg(r.faces.size())
                            .arg(t.elapsed())
                            .arg(r.note);
  emit autoSelectChanged();
}

void Engine3D::setAutoSelectMarquee(const bool show, const glm::dvec3 &mn, const glm::dvec3 &mx) {
  m_settingAutoMarquee = true;
  if (show) {
    MarqueeBox m;
    m.plane = m_marquee.valid ? m_marquee.plane : 0;
    const ViewBasis bs = viewBasisFor(m.plane);
    projectOntoAxis(bs.right, mn, mx, m.a0, m.a1);
    projectOntoAxis(bs.up, mn, mx, m.b0, m.b1);
    projectOntoAxis(bs.depth, mn, mx, m.d0, m.d1);
    setMarquee(m);
    m_autoSel.boxIsOurs = true;
    m_autoSel.boxEdited = false;
  } else {
    clearMarquee();
    m_autoSel.boxIsOurs = false;
    m_autoSel.boxEdited = false;
  }
  m_settingAutoMarquee = false;
}

void Engine3D::onMarqueeChangedForAutoSelect() {
  if (m_settingAutoMarquee)
    return;
  if (!m_marquee.valid) {
    m_autoSel.boxIsOurs = false;
    m_autoSel.boxEdited = false;
    return;
  }
  if (m_dragTarget == DragTarget::CreateMarquee) {
    // A new box becomes the region for the next intent.
    m_autoSel.boxIsOurs = false;
    m_autoSel.active = false;
    emit autoSelectChanged();
    return;
  }
  if (!m_autoSel.boxIsOurs || !autoSelectContinuing())
    return;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  if (m_materialMode) {
    m_autoSel.query.hasRegion = true;
    m_autoSel.query.regionMin = mn;
    m_autoSel.query.regionMax = mx;
  } else {
    m_autoSel.brushQuery.hasRegion = true;
    m_autoSel.brushQuery.regionMin = mn;
    m_autoSel.brushQuery.regionMax = mx;
  }
  m_autoSel.boxEdited = true;
  runAutoSelectQuery(false);
}

void Engine3D::clearAutoSelect() {
  if (m_autoSel.boxIsOurs)
    setAutoSelectMarquee(false, {}, {});
  m_autoSel.active = false;
  m_autoSel.result = {};
  m_autoSel.brushResult = {};
  m_autoSel.typeCounts.fill(0);
  m_storeysShown = false;
  emit autoSelectChanged();
}

void Engine3D::clearAutoSelectRegion() {
  if (m_autoSel.boxIsOurs)
    setAutoSelectMarquee(false, {}, {});
  m_autoSel.active = false;
}

bool Engine3D::autoSelectActive() const { return autoSelectContinuing(); }

bool Engine3D::autoSelectIsFacade() const {
  if (!autoSelectContinuing())
    return false;
  return m_materialMode ? m_autoSel.query.intent == eng::sel::Intent::Facade
                        : m_autoSel.brushQuery.intent == eng::sel::BrushIntent::Facade;
}

QString Engine3D::autoSelectSummary() const {
  if (!m_autoSel.hasRun)
    return {};
  const bool empty = m_materialMode ? m_autoSel.result.empty() : m_autoSel.brushResult.empty();
  const QString &note = m_materialMode ? m_autoSel.result.note : m_autoSel.brushResult.note;
  if (!autoSelectContinuing() || empty)
    return empty ? note : QString();
  QString s = m_materialMode ? QStringLiteral("%1 face(s)").arg(m_selectedFaces.size())
                             : QStringLiteral("%1 brush(es)").arg(m_selectedObjects.size());
  if (m_marquee.valid) {
    glm::dvec3 mn, mx;
    marqueeToWorld(m_marquee, mn, mx);
    const glm::dvec3 e = (mx - mn) * double(kSourceUnitsPerWorldUnit);
    s += QStringLiteral(" · box %1 × %2 × %3").arg(qRound(e.x)).arg(qRound(e.z)).arg(qRound(e.y));
  }
  return s;
}

std::array<int, 6> Engine3D::facadeTypeCounts() const { return m_autoSel.typeCounts; }

bool Engine3D::facadeTypeEnabled(const int t) const {
  return t >= 0 && t < 6 && m_autoSel.typeOn[size_t(t)];
}

void Engine3D::setFacadeTypeEnabled(const int t, const bool on) {
  if (t < 0 || t >= 6 || m_autoSel.typeOn[size_t(t)] == on)
    return;
  m_autoSel.typeOn[size_t(t)] = on;
  if (autoSelectIsFacade())
    runAutoSelectQuery(false);
  else
    emit autoSelectChanged();
}

std::vector<Engine3D::FaceGroupUse> Engine3D::seedFaceGroups() const {
  std::vector<FaceGroupUse> out;
  if (!m_scene)
    return out;
  quint32 seed = 0;
  if (autoSelectContinuing() && !m_autoSel.seedPicks.empty())
    seed = m_autoSel.seedPicks.front();
  else if (!m_selectedFaces.empty())
    seed = *m_selectedFaces.begin();
  const auto obj = seed ? m_scene->findById(eng::stableIdFromPicking(seed)) : nullptr;
  if (!obj || !obj->getMesh())
    return out;
  const int f = faceIndexOfPicking(obj->getMesh(), seed);
  if (obj->group() != eng::kWorldspawnGroup)
    if (const eng::MeshGroup *mg = m_scene->findGroup(obj->group()))
      out.push_back({kMeshGroupPick | mg->id, tr("%1 (brush group)").arg(mg->name), mg->color, false, 1});
  for (const quint32 id: obj->getMesh()->faceGroups(f))
    if (const eng::FaceGroup *g = m_scene->faceGroups.find(id))
      out.push_back({g->id, g->name, g->color, g->builtin, 1});
  return out;
}

bool Engine3D::selectByFaceGroup() {
  if (!m_materialMode)
    return false;
  // Face groups only (the brush group leads the list for the panel).
  std::vector<FaceGroupUse> groups;
  for (const auto &g: seedFaceGroups())
    if (!(g.id & kMeshGroupPick))
      groups.push_back(g);
  if (groups.empty())
    return false;
  quint32 id = groups.front().id;
  for (const auto &g: groups)
    if (!g.builtin) {
      id = g.id;
      break;
    }
  clearAutoSelectRegion();
  return runAutoSelect(int(eng::sel::Intent::Group), 0, false, id, true, false);
}

bool Engine3D::selectSameBrushFaces(const bool add) {
  if (!m_scene || !m_materialMode)
    return false;
  std::set<quint32> picks(m_selectedFaces.begin(), m_selectedFaces.end());
  if (autoSelectContinuing())
    picks.insert(m_autoSel.seedPicks.begin(), m_autoSel.seedPicks.end());
  std::set<quint32> brushes;
  for (const quint32 pick: picks)
    if (pick)
      brushes.insert(eng::stableIdFromPicking(pick));
  if (brushes.empty())
    return false;
  std::set<quint32> faces;
  if (add)
    faces = m_selectedFaces;
  for (const quint32 pid: facePicksOfBrushes(brushes))
    faces.insert(pid);
  clearAutoSelect();
  setSelected({}, {faces.begin(), faces.end()});
  return true;
}

bool Engine3D::selectSameMaterialFaces(const bool add) {
  if (!m_scene || !m_materialMode)
    return false;
  std::set<quint32> picks(m_selectedFaces.begin(), m_selectedFaces.end());
  if (autoSelectContinuing())
    picks.insert(m_autoSel.seedPicks.begin(), m_autoSel.seedPicks.end());
  std::set<QString> names;
  for (const quint32 pick: picks) {
    const auto o = pick ? m_scene->findById(eng::stableIdFromPicking(pick)) : nullptr;
    const auto mesh = o ? o->getMesh() : nullptr;
    const int f = mesh ? faceIndexOfPicking(mesh, pick) : -1;
    if (f >= 0)
      if (const auto m = mesh->faceMaterialRef(f))
        names.insert(m->name);
  }
  if (names.empty())
    return false;
  std::set<quint32> faces;
  if (add)
    faces = m_selectedFaces;
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o ? o->getMesh() : nullptr;
    // Hidden faces and modifier sources count; modifier output is never selected (its sources carry the texture).
    if (!mesh || o->isDerived())
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (const auto m = mesh->faceMaterialRef(f); m && names.count(m->name))
        if (const quint32 pid = mesh->getFacePickingID(f))
          faces.insert(pid);
  }
  clearAutoSelect();
  setSelected({}, {faces.begin(), faces.end()});
  return true;
}

quint32 Engine3D::defaultSeedGroup() const {
  const auto groups = seedFaceGroups();
  for (const auto &g: groups)
    if (g.id >= eng::facegroup::kFirstUser)
      return g.id;
  return groups.empty() ? 0 : groups.front().id;
}

std::vector<quint32> Engine3D::facePicksOfBrushes(const std::set<quint32> &ids) const {
  std::vector<quint32> out;
  if (!m_scene)
    return out;
  for (const quint32 id: ids) {
    const auto o = m_scene->findById(id);
    const auto mesh = o ? o->getMesh() : nullptr;
    if (!mesh || o->hidden())
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (const quint32 pid = mesh->getFacePickingID(f))
        out.push_back(pid);
  }
  return out;
}

void Engine3D::selectBrushUnits(const std::set<quint32> &ids) {
  if (!m_scene)
    return;
  clearAutoSelect();
  m_materialPickHost = 0;
  m_selectedMods.clear();
  m_selectedPaths.clear();
  m_selectedVerts.clear();
  m_selectedEntities.clear();
  m_selectedFaces.clear();
  m_selectedObjects.clear();
  for (const quint32 id: ids) {
    const auto o = m_scene->findById(id);
    if (!o || o->hidden() || o->locked())
      continue;
    if (o->modOutput()) {
      if (o->modOutput() != m_editSourceHost)
        m_selectedMods.insert(o->modOutput());
    } else if (!o->isDerived()) {
      m_selectedObjects.insert(id);
    }
  }
  expandSelectionToBrushEntities();
  refreshDrawSelection();
  noteFillSelection();
  emit selectionChanged();
  requestRedraw();
}

bool Engine3D::showStoreys() {
  if (!m_scene || !m_materialMode)
    return false;
  quint32 seed = 0;
  if (autoSelectContinuing() && !m_autoSel.seedPicks.empty())
    seed = m_autoSel.seedPicks.front();
  else if (!m_selectedFaces.empty())
    seed = *m_selectedFaces.begin();
  if (seed == 0)
    return false;
  eng::FillParams p;
  p.facade = true;
  p.wrap = m_autoSel.query.allSides;
  const bool ok = solveStoreys(seed, p, false);
  m_storeysShown = ok;
  emit autoSelectChanged();
  return ok;
}

void Engine3D::hideStoreys() {
  m_storeysShown = false;
  clearFill();
  emit autoSelectChanged();
}

} // namespace vk
