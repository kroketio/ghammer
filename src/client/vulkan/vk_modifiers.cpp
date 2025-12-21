// Modifiers: modified objects, their stacks and the evaluated output. Output is derived (GameObjects tagged
// modOutput, rebuilt by pumpModifiers from the inputs): never saved, never in an undo command, so undo restores
// inputs and the next pump makes the matching output.

#include "vk_engine.h"

#include <algorithm>
#include <cstring>

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>

#include "client/engine/scene.h"
#include "vk_shared.h"

namespace vk {

namespace {

quint64 mixKey(quint64 seed, const quint64 v) { return (seed ^ v) * 0x100000001b3ull + 0x9E3779B97F4A7C15ull; }

// The horizontal host-frame axis the brushes run along, as a curve axis code (0 +X, 1 -X, 4 +Z, 5 -Z): the only
// axis every brush spans end to end, else the one `hint` points along most, else the longest side.
// The sign follows `hint` when given.
int runAxis(const std::vector<eng::ConvexBrush> &brushes, const glm::dquat &orientation, const glm::dvec3 *hint) {
  const glm::dquat inv = glm::inverse(orientation);
  glm::dvec3 lo(1e300), hi(-1e300);
  std::vector<std::pair<glm::dvec3, glm::dvec3>> boxes;
  for (const eng::ConvexBrush &b: brushes) {
    glm::dvec3 a(1e300), c(-1e300);
    for (const glm::dvec3 &v: b.vertices()) {
      const glm::dvec3 l = inv * v;
      a = glm::min(a, l);
      c = glm::max(c, l);
    }
    if (a.x > c.x)
      continue;
    boxes.emplace_back(a, c);
    lo = glm::min(lo, a);
    hi = glm::max(hi, c);
  }
  const glm::dvec3 h = hint ? inv * *hint : glm::dvec3(0.0);
  int axis = -1; // 0 = X, 2 = Z
  if (!boxes.empty()) {
    bool spans[3] = {true, true, true};
    for (const int a: {0, 2}) {
      const double tol = 1e-3 * (hi[a] - lo[a]) + 0.01;
      for (const auto &[a0, a1]: boxes)
        spans[a] = spans[a] && a0[a] <= lo[a] + tol && a1[a] >= hi[a] - tol;
    }
    if (spans[0] != spans[2])
      axis = spans[0] ? 0 : 2;
    else if (hint && std::fabs(h.x) + std::fabs(h.z) > 1e-6)
      axis = std::fabs(h.x) >= std::fabs(h.z) ? 0 : 2;
    else
      axis = (hi.x - lo.x) >= (hi.z - lo.z) ? 0 : 2;
  } else {
    axis = 0;
  }
  const bool negative = hint && h[axis] < 0.0;
  return axis == 0 ? (negative ? 1 : 0) : (negative ? 5 : 4);
}

quint64 hashDouble(const double d) {
  quint64 u = 0;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

quint64 hashJson(const QJsonObject &o) {
  return quint64(qHash(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

// What an output mesh shows: planes, materials, mapping, groups.
quint64 contentHash(const eng::ConvexBrush &b) {
  quint64 h = 0xcbf29ce484222325ull;
  for (const eng::BrushFace &f: b.faces) {
    for (int i = 0; i < 3; ++i)
      h = mixKey(h, hashDouble(f.normal[i]));
    h = mixKey(h, hashDouble(f.distance));
    h = mixKey(h, quint64(reinterpret_cast<uintptr_t>(f.material.data())));
    const eng::FaceTexBasis &t = f.basis;
    for (int i = 0; i < 3; ++i) {
      h = mixKey(h, hashDouble(t.uAxis[i]));
      h = mixKey(h, hashDouble(t.vAxis[i]));
    }
    h = mixKey(h, hashDouble(t.tilesPerUnitU) ^ (hashDouble(t.tilesPerUnitV) << 1));
    h = mixKey(h, hashDouble(t.offsetU) ^ (hashDouble(t.offsetV) << 1) ^ (t.valid ? 1u : 0u));
    for (const quint32 g: f.groups)
      h = mixKey(h, g);
  }
  return h;
}

QString kindLabel(const QString &kind) {
  const eng::mods::KindInfo *k = eng::mods::kindInfo(kind);
  return k ? k->label : kind;
}

} // namespace

void Engine3D::pushModTable(const std::vector<eng::mods::Host> &before, const QString &label,
                            const QString &mergeKey) {
  if (!m_scene)
    return;
  ++m_scene->modEpoch;
  emit modHostsChanged();
  if (!m_undoStack || m_undoStack->isApplying())
    return;
  const auto after = m_scene->modHosts;
  auto apply = [this](const std::vector<eng::mods::Host> &t) {
    if (!m_scene)
      return;
    m_scene->modHosts = t;
    ++m_scene->modEpoch;
    if (m_editSourceHost && !m_scene->findModHost(m_editSourceHost))
      setEditSourceHost(0);
    for (auto it = m_selectedMods.begin(); it != m_selectedMods.end();)
      it = m_scene->findModHost(*it) ? std::next(it) : m_selectedMods.erase(it);
    emit modHostsChanged();
    emit selectionChanged();
  };
  m_undoStack->push(std::make_unique<eng::LambdaCommand>(
      label, [apply, before] { apply(before); }, [apply, after] { apply(after); }, mergeKey));
}

const eng::mods::Host *Engine3D::modHost(const quint32 id) const {
  return m_scene ? m_scene->findModHost(id) : nullptr;
}

std::vector<quint32> Engine3D::modSources(const quint32 host) const {
  std::vector<quint32> out;
  if (!m_scene || !host)
    return out;
  for (const auto &o: m_scene->gameObjects)
    if (o && !o->isDerived() && o->modHost() == host)
      out.push_back(o->stableId());
  return out;
}

std::vector<quint32> Engine3D::modOutputs(const quint32 host) const {
  std::vector<quint32> out;
  const auto it = m_modRuntime.find(host);
  if (it == m_modRuntime.end())
    return out;
  for (const auto &[key, o]: it->second.outputs)
    out.push_back(o->stableId());
  return out;
}

Engine3D::ModStatus Engine3D::modStatus(const quint32 host) const {
  ModStatus s;
  const auto it = m_modRuntime.find(host);
  if (it == m_modRuntime.end())
    return s;
  s.active = it->second.active;
  s.error = it->second.error;
  s.outputs = int(it->second.outputs.size());
  s.diagnostics = it->second.diagnostics;
  s.summary = it->second.summary;
  return s;
}

quint32 Engine3D::panelModHost() const {
  auto alive = [this](const quint32 h) { return h && m_scene && m_scene->findModHost(h) ? h : 0u; };
  if (m_materialMode) {
    // Faces are textured through the sources: the edited host, or the one whose output was clicked.
    if (m_editSourceHost)
      return m_editSourceHost;
    if (const quint32 h = alive(m_materialPickHost))
      return h;
    return m_selectedMods.size() == 1 ? *m_selectedMods.begin() : 0u;
  }
  if (m_selectedMods.size() == 1)
    return *m_selectedMods.begin();
  if (m_editSourceHost && m_selectedObjects.empty() && m_selectedEntities.empty() && m_selectedPaths.empty()
      && m_selectedMods.empty())
    return m_editSourceHost;
  // A selection of only one host's sources edits that host.
  quint32 host = 0;
  for (const quint32 id: m_selectedObjects) {
    const auto o = m_scene ? m_scene->findById(id) : nullptr;
    if (!o || !o->modHost() || (host && o->modHost() != host))
      return 0;
    host = o->modHost();
  }
  return host;
}

void Engine3D::selectModHost(const quint32 host, const bool additive) {
  if (!m_scene || !m_scene->findModHost(host))
    return;
  if (!additive) {
    m_selectedObjects.clear();
    m_selectedFaces.clear();
    m_selectedEntities.clear();
    m_selectedPaths.clear();
    m_selectedMods = {host};
  } else if (m_selectedMods.count(host)) {
    m_selectedMods.erase(host);
  } else {
    m_selectedMods.insert(host);
  }
  refreshDrawSelection();
  emit selectionChanged();
}

void Engine3D::setSelectedMods(const std::vector<quint32> &hosts) {
  m_selectedMods.clear();
  for (const quint32 h: hosts)
    if (m_scene && m_scene->findModHost(h))
      m_selectedMods.insert(h);
  refreshDrawSelection();
  emit selectionChanged();
}

void Engine3D::refreshDrawSelection() {
  m_drawSelection = m_selectedObjects;
  for (const quint32 h: m_selectedMods) {
    const auto it = m_modRuntime.find(h);
    if (it != m_modRuntime.end())
      for (const auto &[key, o]: it->second.outputs)
        m_drawSelection.insert(o->stableId());
  }
}

void Engine3D::refreshModVisibility() {
  if (!m_scene)
    return;
  bool changed = false;
  std::map<quint32, std::shared_ptr<eng::GameObject>> sourceById;
  for (const auto &o: m_scene->gameObjects) {
    if (!o || o->isDerived())
      continue;
    bool replaced = false;
    if (const quint32 h = o->modHost()) {
      const auto it = m_modRuntime.find(h);
      replaced = it != m_modRuntime.end() && it->second.active && !it->second.error;
      sourceById[o->stableId()] = o;
    }
    // Sources hide behind their output, except in Edit Source and while Show generated is off.
    const bool hide = replaced && m_showModOutput && m_editSourceHost != o->modHost();
    o->setModReplaced(replaced);
    if (o->modHidden() != hide) {
      o->setModHidden(hide);
      changed = true;
    }
  }
  // Output follows its source: the user's hide / lock, and a hidden mirror copy.
  for (auto &[host, rt]: m_modRuntime)
    for (const auto &[key, out]: rt.outputs) {
      const auto it = sourceById.find(std::get<0>(key));
      if (it == sourceById.end())
        continue;
      const eng::GameObject &src = *it->second;
      if (out->hiddenFlag() != src.hiddenFlag()) {
        out->setHidden(src.hiddenFlag());
        changed = true;
      }
      // Show generated off: no output anywhere.
      const bool hide = src.mirrorHidden() || !m_showModOutput;
      if (out->modHidden() != hide) {
        out->setModHidden(hide);
        changed = true;
      }
      out->setLocked(src.locked());
    }
  if (changed) {
    // Hidden things cannot stay selected.
    std::vector<quint32> keep;
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->hidden())
        keep.push_back(id);
    if (keep.size() != m_selectedObjects.size())
      setSelected(keep, std::vector<quint32>(m_selectedFaces.begin(), m_selectedFaces.end()));
    invalidateDerivedCaches();
    requestRedraw();
  }
}

quint64 Engine3D::hostSignature(const eng::mods::Host &host,
                                const std::vector<std::shared_ptr<eng::GameObject>> &sources) {
  quint64 h = mixKey(0x84222325cbf29ce4ull, hashJson(eng::mods::toJson(host)));
  h = mixKey(h, host.id == m_editSourceHost ? 1 : 0);
  for (const auto &o: sources) {
    h = mixKey(h, o->stableId());
    const auto &mesh = o->meshRef();
    h = mixKey(h, quint64(reinterpret_cast<uintptr_t>(mesh.get())));
    h = mixKey(h, mesh ? mesh->geometryVersion() : 0);
    h = mixKey(h, mesh ? mesh->faceVersion() : 0);
    // Not transformVersion: hiding the sources (refreshModVisibility) bumps it and would make every host stale.
    h = mixKey(h, o->placementVersion());
    h = mixKey(h, (quint64(o->group()) << 32) ^ o->brushEntity());
  }
  // The paths its curves follow (a Connect path also through its anchors).
  for (const eng::mods::Modifier &m: host.stack) {
    if (!m.enabled)
      continue;
    quint32 pathId = 0;
    if (m.kind == QLatin1String("curve"))
      pathId = eng::mods::CurveParams::fromJson(m.params).path;
    else if (m.kind == QLatin1String("array"))
      pathId = eng::mods::ArrayParams::fromJson(m.params).curvePath;
    if (!pathId)
      continue;
    (void)pathCurve(pathId); // refreshes the cache key
    const auto it = m_curveCache.find(pathId);
    h = mixKey(h, it == m_curveCache.end() ? 0 : it->second.key);
  }
  return h;
}

void Engine3D::clearHostOutputs(ModRuntime &rt, std::vector<quint32> &removeIds) {
  for (const auto &[key, o]: rt.outputs) {
    removeIds.push_back(o->stableId());
    if (rt.spare.size() < 4096)
      rt.spare.push_back(o);
  }
  rt.outputs.clear();
  rt.contentHash.clear();
}

void Engine3D::evaluateHost(const eng::mods::Host &host, const std::vector<std::shared_ptr<eng::GameObject>> &sources,
                            ModRuntime &rt) {
  QElapsedTimer timer;
  timer.start();
  eng::mods::EvalInput in;
  in.orientation = host.orientation;
  in.stack = host.stack;
  in.editSource = host.id == m_editSourceHost;
  in.curveFor = [this](quint32 id) { return pathCurve(id); };
  std::map<quint32, std::shared_ptr<eng::GameObject>> byId;
  for (const auto &o: sources) {
    if (const auto b = eng::ConvexBrush::fromObject(*o)) {
      eng::mods::Piece p;
      p.brush = *b;
      p.sourceId = o->stableId();
      in.sources.push_back(std::move(p));
      byId[o->stableId()] = o;
    }
  }
  const eng::mods::EvalOutput out = eng::mods::evaluate(in);
  rt.diagnostics = out.diagnostics;
  rt.summary = out.summary;
  rt.error = out.error;
  rt.active = out.active;

  std::vector<quint32> removeIds;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  if (!out.active || out.error || out.pieces.empty()) {
    clearHostOutputs(rt, removeIds);
  } else {
    using Key = std::tuple<quint32, int, int>;
    std::map<Key, std::shared_ptr<eng::GameObject>> next;
    std::vector<const eng::mods::Piece *> unmatched;
    for (const eng::mods::Piece &p: out.pieces) {
      const Key key {p.sourceId, p.copy, p.slice};
      const auto it = rt.outputs.find(key);
      if (it != rt.outputs.end() && !next.count(key)) {
        next[key] = it->second;
        rt.outputs.erase(it);
      } else {
        unmatched.push_back(&p);
      }
    }
    // Reuse leftovers from the last result, then spares, then new objects.
    std::vector<std::shared_ptr<eng::GameObject>> leftovers;
    for (const auto &[key, o]: rt.outputs)
      leftovers.push_back(o);
    rt.outputs.clear();
    for (const eng::mods::Piece *p: unmatched) {
      std::shared_ptr<eng::GameObject> o;
      if (!leftovers.empty()) {
        o = leftovers.back();
        leftovers.pop_back();
      } else if (!rt.spare.empty()) {
        o = rt.spare.back();
        rt.spare.pop_back();
        add.push_back(o);
      } else {
        o = std::make_shared<eng::GameObject>(std::make_shared<eng::Mesh>());
        add.push_back(o);
      }
      rt.contentHash.erase(o->stableId()); // force a mesh
      next[Key {p->sourceId, p->copy, p->slice}] = o;
    }
    for (const auto &o: leftovers) {
      removeIds.push_back(o->stableId());
      rt.contentHash.erase(o->stableId());
      if (rt.spare.size() < 4096)
        rt.spare.push_back(o);
    }
    rt.outputs = std::move(next);
    // Meshes only where what they show changed.
    std::map<Key, const eng::mods::Piece *> pieceOf;
    for (const eng::mods::Piece &p: out.pieces)
      pieceOf[Key {p.sourceId, p.copy, p.slice}] = &p;
    for (const auto &[key, o]: rt.outputs) {
      const eng::mods::Piece *p = pieceOf[key];
      const quint64 hash = contentHash(p->brush);
      const auto src = byId.find(p->sourceId);
      o->setModOutput(host.id);
      if (src != byId.end()) {
        o->setGroup(src->second->group());
        o->setBrushEntity(src->second->brushEntity());
        o->setName(src->second->name());
      }
      const auto hit = rt.contentHash.find(o->stableId());
      if (hit != rt.contentHash.end() && hit->second == hash)
        continue;
      auto mesh = p->brush.toMesh();
      if (!mesh || mesh->faceCount() == 0)
        continue;
      for (int f = 0; f < mesh->faceCount(); ++f)
        mesh->setFacePickingID(f, eng::makePickingId(o->stableId(), uint32_t(f)));
      o->setMesh(mesh);
      o->setPosition(glm::vec3(0.0f));
      o->setRotation(glm::vec3(0.0f));
      o->setScale(glm::vec3(1.0f));
      o->computeWorldAABB();
      rt.contentHash[o->stableId()] = hash;
    }
  }
  if (!removeIds.empty())
    m_scene->removeObjects(std::set<quint32>(removeIds.begin(), removeIds.end()));
  if (!add.empty())
    m_scene->addObjects(add);
  rt.lastMs = double(timer.nsecsElapsed()) * 1e-6;
  rt.lastAt = QDateTime::currentMSecsSinceEpoch();
}

void Engine3D::pumpModifiers(const ModPump mode) {
  if (!m_scene || m_modPumping || m_mirrorSyncing)
    return;
  if (m_undoStack && (m_undoStack->isApplying() || m_undoStack->inMacro()))
    return;
  auto globalKey = [this] {
    quint64 k = mixKey(0x1234567ull, m_scene->structureVersion());
    k = mixKey(k, eng::GameObject::globalTransformEpoch());
    k = mixKey(k, eng::Mesh::globalGeometryEpoch());
    k = mixKey(k, eng::Mesh::globalFaceEpoch());
    k = mixKey(k, m_scene->modEpoch);
    k = mixKey(k, m_scene->pathEpoch);
    k = mixKey(k, m_entityEpoch);
    k = mixKey(k, m_editSourceHost);
    return k;
  };
  if (m_scene->modHosts.empty() && m_modRuntime.empty())
    return;
  const quint64 key = globalKey();
  const bool forced = mode == ModPump::Blocking || mode == ModPump::Required;
  if (key == m_modKey && !forced)
    return;
  // Show generated off: nothing to evaluate. m_modKey stays behind, so the next pump catches up.
  const bool evaluate = m_showModOutput || mode == ModPump::Required;
  m_modPumping = true;

  bool changed = false, leftDirty = false;
  // Hosts that are gone take their output with them.
  for (auto it = m_modRuntime.begin(); it != m_modRuntime.end();) {
    if (m_scene->findModHost(it->first)) {
      ++it;
      continue;
    }
    std::vector<quint32> removeIds;
    clearHostOutputs(it->second, removeIds);
    if (!removeIds.empty()) {
      m_scene->removeObjects(std::set<quint32>(removeIds.begin(), removeIds.end()));
      changed = true;
    }
    it = m_modRuntime.erase(it);
  }
  if (!evaluate) {
    if (changed) {
      refreshModVisibility();
      refreshDrawSelection();
      invalidateDerivedCaches();
      emit modOutputsChanged();
      requestRedraw();
    }
    m_modPumping = false;
    return;
  }

  std::map<quint32, std::vector<std::shared_ptr<eng::GameObject>>> sources;
  for (const auto &o: m_scene->gameObjects)
    if (o && !o->isDerived() && o->modHost())
      sources[o->modHost()].push_back(o);

  QElapsedTimer budget;
  budget.start();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const double limitMs = mode == ModPump::Frame ? 8.0 : 16.0;
  const std::vector<eng::mods::Host> hosts = m_scene->modHosts; // evaluation may not touch the table, but be safe
  for (const eng::mods::Host &host: hosts) {
    const auto &src = sources[host.id];
    ModRuntime &rt = m_modRuntime[host.id];
    const quint64 sig = hostSignature(host, src);
    if (rt.evaluated && rt.signature == sig)
      continue;
    if (!forced) {
      if (double(budget.nsecsElapsed()) * 1e-6 > limitMs) {
        leftDirty = true;
        break;
      }
      // A slow host follows a drag at about 10 updates a second.
      if (rt.evaluated && rt.lastMs > 30.0 && now - rt.lastAt < 100) {
        leftDirty = true;
        continue;
      }
    }
    evaluateHost(host, src, rt);
    rt.signature = sig;
    rt.evaluated = true;
    changed = true;
  }
  if (changed) {
    refreshModVisibility();
    refreshDrawSelection();
    invalidateDerivedCaches();
    emit modOutputsChanged();
    requestRedraw();
  }
  m_modKey = leftDirty ? 0 : globalKey();
  m_modPumping = false;
}

void Engine3D::resetModRuntime() {
  m_modRuntime.clear();
  m_curveCache.clear();
  m_selectedMods.clear();
  m_selectedPaths.clear();
  m_drawSelection.clear();
  m_editSourceHost = 0;
  m_materialPickHost = 0;
  m_modKey = 0;
  m_pen = {};
  m_pathDrag = {};
  m_selectedPathPoints.clear();
}

quint32 Engine3D::addModifier(const QString &kind, const glm::dvec3 *runHint) {
  if (!m_scene || !eng::mods::kindInfo(kind))
    return 0;
  quint32 hostId = panelModHost();
  const auto before = m_scene->modHosts;
  const QString label = QStringLiteral("Add %1 modifier").arg(kindLabel(kind));
  const bool record = m_undoStack && !m_undoStack->isApplying();

  std::vector<quint32> newSources;
  if (!hostId) {
    for (const quint32 id: withMirrorTwins(m_selectedObjects))
      if (const auto o = m_scene->findById(id); o && !o->isDerived()) {
        if (o->modHost()) {
          emit toastRequested(tr("Some of these brushes already belong to a modified object"));
          return 0;
        }
        newSources.push_back(id);
      }
    if (newSources.empty()) {
      emit toastRequested(tr("Select brushes (or a modified object) first"));
      return 0;
    }
  }
  if (record)
    m_undoStack->beginMacro(label);
  if (!hostId) {
    eng::mods::Host h;
    h.id = m_scene->mintModHostId();
    quint32 group = 0;
    bool sameGroup = true, sameRot = true;
    glm::dquat rot(1.0, 0.0, 0.0, 0.0);
    for (size_t i = 0; i < newSources.size(); ++i) {
      const auto o = m_scene->findById(newSources[i]);
      if (i == 0) {
        group = o->group();
        rot = o->orientation();
      } else {
        sameGroup = sameGroup && o->group() == group;
        sameRot = sameRot && std::fabs(glm::dot(o->orientation(), rot)) > 1.0 - 1e-9;
      }
    }
    h.name = newModHostName(kind, sameGroup ? group : eng::kWorldspawnGroup);
    if (sameRot)
      h.orientation = rot;
    m_scene->modHosts.push_back(h);
    hostId = h.id;
    std::vector<std::shared_ptr<eng::GameObject>> objs;
    for (const quint32 id: newSources)
      if (const auto o = m_scene->findById(id))
        objs.push_back(o);
    auto tag = [this, objs](quint32 v) {
      for (const auto &o: objs)
        o->setModHost(v);
      if (m_scene)
        ++m_scene->modEpoch;
      refreshModVisibility();
    };
    tag(hostId);
    if (record)
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(
          label, [tag] { tag(0); }, [tag, hostId] { tag(hostId); }));
  }
  eng::mods::Host *h = m_scene->findModHost(hostId);
  eng::mods::Modifier m;
  m.id = h->nextModifierId++;
  m.kind = kind;
  m.name = kindLabel(kind);
  m.params = eng::mods::kindInfo(kind)->defaults();
  // Copies repeat and the curve bends along the way the sources run (runAxis).
  std::vector<eng::ConvexBrush> brushes;
  for (const quint32 id: modSources(hostId))
    if (const auto o = m_scene->findById(id))
      if (const auto b = eng::ConvexBrush::fromObject(*o))
        brushes.push_back(*b);
  const int run = runAxis(brushes, h->orientation, runHint);
  if (kind == QLatin1String("array")) {
    eng::mods::ArrayParams ap = eng::mods::ArrayParams::fromJson(m.params);
    ap.relative = run <= 1 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 0.0, 1.0);
    m.params = ap.toJson();
  } else if (kind == QLatin1String("curve")) {
    // Follow the selected (or only) path, along the array's offset or the way the sources run.
    eng::mods::CurveParams cp;
    if (m_selectedPaths.size() == 1)
      cp.path = *m_selectedPaths.begin();
    else if (m_scene->paths.size() == 1)
      cp.path = m_scene->paths.front().id;
    int axis = run;
    for (const eng::mods::Modifier &o: h->stack)
      if (o.kind == QLatin1String("array")) {
        const glm::dvec3 rel = eng::mods::ArrayParams::fromJson(o.params).relative;
        const bool alongX = std::fabs(rel.x) >= std::fabs(rel.z);
        // The direction from the hint when there is one, else the offset's.
        const glm::dvec3 hl = runHint ? glm::inverse(h->orientation) * *runHint : rel;
        const bool negative = alongX ? hl.x < 0 : hl.z < 0;
        axis = alongX ? (negative ? 1 : 0) : (negative ? 5 : 4);
      }
    cp.axis = axis;
    m.params = cp.toJson();
  }
  h->stack.push_back(m);
  pushModTable(before, label);
  if (record)
    m_undoStack->endMacro();
  m_selectedObjects.clear();
  m_selectedFaces.clear();
  if (m_editSourceHost != hostId)
    m_selectedMods = {hostId};
  pumpModifiers(ModPump::Blocking);
  refreshDrawSelection();
  emit selectionChanged();
  return hostId;
}

namespace {

eng::mods::Modifier *findMod(eng::mods::Host *h, const quint32 mod) {
  if (!h)
    return nullptr;
  for (auto &m: h->stack)
    if (m.id == mod)
      return &m;
  return nullptr;
}

} // namespace

void Engine3D::removeModifier(const quint32 host, const quint32 mod) {
  if (!m_scene)
    return;
  eng::mods::Host *h = m_scene->findModHost(host);
  const eng::mods::Modifier *m = findMod(h, mod);
  if (!m)
    return;
  if (h->stack.size() == 1) {
    dissolveModHost(host);
    return;
  }
  const auto before = m_scene->modHosts;
  const QString label = QStringLiteral("Remove %1 modifier").arg(kindLabel(m->kind));
  h->stack.erase(std::remove_if(h->stack.begin(), h->stack.end(), [mod](const auto &x) { return x.id == mod; }),
                 h->stack.end());
  pushModTable(before, label);
  pumpModifiers(ModPump::Blocking);
}

void Engine3D::moveModifier(const quint32 host, const quint32 mod, const int delta) {
  if (!m_scene)
    return;
  eng::mods::Host *h = m_scene->findModHost(host);
  if (!h)
    return;
  int i = -1;
  for (int k = 0; k < int(h->stack.size()); ++k)
    if (h->stack[size_t(k)].id == mod)
      i = k;
  const int j = i + delta;
  if (i < 0 || j < 0 || j >= int(h->stack.size()))
    return;
  const auto before = m_scene->modHosts;
  std::swap(h->stack[size_t(i)], h->stack[size_t(j)]);
  pushModTable(before, QStringLiteral("Move modifier"));
  pumpModifiers(ModPump::Blocking);
}

void Engine3D::setModifierEnabled(const quint32 host, const quint32 mod, const bool on) {
  eng::mods::Modifier *m = m_scene ? findMod(m_scene->findModHost(host), mod) : nullptr;
  if (!m || m->enabled == on)
    return;
  const auto before = m_scene->modHosts;
  m->enabled = on;
  pushModTable(before, on ? QStringLiteral("Enable modifier") : QStringLiteral("Disable modifier"));
  pumpModifiers(ModPump::Blocking);
}

void Engine3D::setModifierShowInEditSource(const quint32 host, const quint32 mod, const bool on) {
  eng::mods::Modifier *m = m_scene ? findMod(m_scene->findModHost(host), mod) : nullptr;
  if (!m || m->showInEditSource == on)
    return;
  const auto before = m_scene->modHosts;
  m->showInEditSource = on;
  pushModTable(before, QStringLiteral("Modifier in Edit Source"));
  pumpModifiers(ModPump::Blocking);
}

void Engine3D::setModifierExpanded(const quint32 host, const quint32 mod, const bool on) {
  if (eng::mods::Modifier *m = m_scene ? findMod(m_scene->findModHost(host), mod) : nullptr)
    m->expanded = on; // panel state only: not undone, does not change the output
}

void Engine3D::setModifierParams(const quint32 host, const quint32 mod, const QJsonObject &params,
                                 const QString &mergeKey) {
  eng::mods::Modifier *m = m_scene ? findMod(m_scene->findModHost(host), mod) : nullptr;
  if (!m || m->params == params)
    return;
  const auto before = m_scene->modHosts;
  const QJsonObject old = m->params;
  m->params = params;
  if (m->kind == QLatin1String("curve")) {
    // Deform axis changed: arrays before it that repeated along the old one turn with it.
    const int oa = std::clamp(eng::mods::CurveParams::fromJson(old).axis, 0, 5) / 2;
    const int na = std::clamp(eng::mods::CurveParams::fromJson(params).axis, 0, 5) / 2;
    eng::mods::Host *h = m_scene->findModHost(host);
    if (oa != na && h)
      for (eng::mods::Modifier &a: h->stack) {
        if (a.id == mod)
          break;
        if (a.kind != QLatin1String("array"))
          continue;
        eng::mods::ArrayParams ap = eng::mods::ArrayParams::fromJson(a.params);
        bool turned = false;
        for (glm::dvec3 *v: {&ap.relative, &ap.constant}) {
          const double len = glm::length(*v);
          if (len < 1e-9 || std::fabs((*v)[oa]) < 0.5 * len)
            continue;
          std::swap((*v)[oa], (*v)[na]);
          turned = true;
        }
        if (turned)
          a.params = ap.toJson();
      }
  }
  pushModTable(before, QStringLiteral("Edit %1 modifier").arg(kindLabel(m->kind)),
               mergeKey.isEmpty() ? QString() : QStringLiteral("mod.%1.%2.%3").arg(host).arg(mod).arg(mergeKey));
  pumpModifiers(ModPump::Blocking);
}

void Engine3D::renameModHost(const quint32 host, const QString &name) {
  eng::mods::Host *h = m_scene ? m_scene->findModHost(host) : nullptr;
  const QString n = name.trimmed();
  if (!h || n.isEmpty() || h->name == n)
    return;
  const auto before = m_scene->modHosts;
  h->name = n;
  pushModTable(before, QStringLiteral("Rename modified object"));
}

void Engine3D::dissolveModHost(const quint32 host) {
  if (!m_scene || !m_scene->findModHost(host))
    return;
  const std::vector<quint32> sources = modSources(host);
  const QString label = QStringLiteral("Remove modifiers");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  if (m_editSourceHost == host)
    setEditSourceHost(0);
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  for (const quint32 id: sources)
    if (const auto o = m_scene->findById(id))
      objs.push_back(o);
  auto tag = [this, objs](quint32 v) {
    for (const auto &o: objs)
      o->setModHost(v);
    if (m_scene)
      ++m_scene->modEpoch;
  };
  tag(0);
  if (record)
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        label, [tag, host] { tag(host); }, [tag] { tag(0); }));
  const auto before = m_scene->modHosts;
  m_scene->modHosts.erase(std::remove_if(m_scene->modHosts.begin(), m_scene->modHosts.end(),
                                         [host](const auto &h) { return h.id == host; }),
                          m_scene->modHosts.end());
  pushModTable(before, label);
  if (record)
    m_undoStack->endMacro();
  m_selectedMods.erase(host);
  pumpModifiers(ModPump::Blocking);
  refreshModVisibility();
  setSelected(sources, {});
}

bool Engine3D::applyModifiers(const quint32 host, const bool firstOnly) {
  if (!m_scene)
    return false;
  const eng::mods::Host *hp = m_scene->findModHost(host);
  if (!hp)
    return false;
  eng::mods::Host h = *hp;
  // What gets baked: everything enabled, or only the first enabled modifier.
  std::vector<eng::mods::Modifier> stack;
  quint32 firstId = 0;
  for (const auto &m: h.stack) {
    if (!m.enabled)
      continue;
    stack.push_back(m);
    if (firstOnly) {
      firstId = m.id;
      break;
    }
  }
  if (stack.empty()) {
    emit toastRequested(tr("Nothing to apply: every modifier is off"));
    return false;
  }
  eng::mods::EvalInput in;
  in.orientation = h.orientation;
  in.stack = stack;
  in.curveFor = [this](quint32 id) { return pathCurve(id); };
  const std::vector<quint32> sourceIds = modSources(host);
  std::map<quint32, std::shared_ptr<eng::GameObject>> byId;
  for (const quint32 id: sourceIds) {
    const auto o = m_scene->findById(id);
    if (!o)
      continue;
    if (const auto b = eng::ConvexBrush::fromObject(*o)) {
      eng::mods::Piece p;
      p.brush = *b;
      p.sourceId = id;
      in.sources.push_back(std::move(p));
      byId[id] = o;
    }
  }
  const eng::mods::EvalOutput out = eng::mods::evaluate(in);
  if (out.error) {
    for (const auto &d: out.diagnostics)
      if (d.severity == eng::mods::Diagnostic::Severity::Error) {
        emit toastRequested(tr("Cannot apply: %1").arg(d.text));
        break;
      }
    return false;
  }
  // Ordinary brushes (still sources of the host when only the first modifier was applied).
  std::vector<std::shared_ptr<eng::GameObject>> results;
  for (const eng::mods::Piece &p: out.pieces) {
    const auto src = byId.find(p.sourceId);
    auto o = objectFromBrush(p.brush, src != byId.end() ? src->second.get() : nullptr);
    if (!o)
      continue;
    o->setMirrorLink(0, 0); // copies of a linked brush are not twins
    o->setModHost(firstOnly ? host : 0);
    results.push_back(o);
  }
  const QString label = firstOnly ? QStringLiteral("Apply modifier") : QStringLiteral("Apply modifiers");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  if (m_editSourceHost == host && !firstOnly)
    setEditSourceHost(0);
  replaceObjects(sourceIds, results, label);
  const auto before = m_scene->modHosts;
  if (firstOnly) {
    eng::mods::Host *hh = m_scene->findModHost(host);
    hh->stack.erase(std::remove_if(hh->stack.begin(), hh->stack.end(),
                                   [firstId](const auto &m) { return m.id == firstId; }),
                    hh->stack.end());
    if (hh->stack.empty())
      m_scene->modHosts.erase(std::remove_if(m_scene->modHosts.begin(), m_scene->modHosts.end(),
                                             [host](const auto &x) { return x.id == host; }),
                              m_scene->modHosts.end());
  } else {
    m_scene->modHosts.erase(std::remove_if(m_scene->modHosts.begin(), m_scene->modHosts.end(),
                                           [host](const auto &x) { return x.id == host; }),
                            m_scene->modHosts.end());
  }
  const bool hostGone = !m_scene->findModHost(host);
  if (hostGone && firstOnly) {
    // Last modifier gone: the results are plain brushes.
    auto tag = [results](quint32 v) {
      for (const auto &o: results)
        o->setModHost(v);
    };
    tag(0);
    if (record)
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [tag, host] { tag(host); },
                                                             [tag] { tag(0); }));
  }
  pushModTable(before, label);
  if (record)
    m_undoStack->endMacro();
  m_selectedMods.erase(host);
  pumpModifiers(ModPump::Blocking);
  if (hostGone) {
    std::vector<quint32> ids;
    for (const auto &o: results)
      ids.push_back(o->stableId());
    setSelected(ids, {});
  } else {
    m_selectedObjects.clear();
    m_selectedMods = {host};
    refreshDrawSelection();
    emit selectionChanged();
  }
  return true;
}

void Engine3D::setEditSourceHost(const quint32 host) {
  const quint32 h = m_scene && m_scene->findModHost(host) ? host : 0;
  if (h == m_editSourceHost)
    return;
  const quint32 was = m_editSourceHost;
  m_editSourceHost = h;
  if (h) {
    // The sources come back; the host itself is edited through them.
    m_selectedMods.erase(h);
    m_selectedObjects.clear();
    m_selectedFaces.clear();
  } else if (was && m_scene && m_scene->findModHost(was)) {
    // Back to the result: the host is the selection again.
    m_selectedObjects.clear();
    m_selectedFaces.clear();
    if (m_materialMode)
      m_materialPickHost = was;
    else
      m_selectedMods = {was};
  }
  pumpModifiers(ModPump::Blocking);
  refreshModVisibility();
  // Object mode starts with the sources selected; Material mode starts empty.
  if (h && !m_materialMode && m_scene)
    for (const auto &o: m_scene->gameObjects)
      if (o && o->modHost() == h && !o->isDerived() && !o->hidden() && !o->locked())
        m_selectedObjects.insert(o->stableId());
  refreshDrawSelection();
  noteFillSelection();
  emit editSourceChanged();
  emit selectionChanged();
  requestRedraw();
}

void Engine3D::setShowModOutput(const bool show) {
  if (show == m_showModOutput)
    return;
  m_showModOutput = show;
  // Nothing was evaluated while it was off: catch up first.
  if (show)
    pumpModifiers(ModPump::Blocking);
  refreshModVisibility();
  invalidateDerivedCaches();
  emit editSourceChanged();
  requestRedraw();
}

std::vector<std::pair<glm::dvec3, glm::dvec3>> Engine3D::modSourceBoxes() const {
  // One box per modified object around its sources; brushes the user hid stay out.
  std::map<quint32, std::pair<glm::dvec3, glm::dvec3>> boxes;
  if (!m_scene)
    return {};
  for (const auto &o: m_scene->gameObjects) {
    if (!o || !o->modHost() || o->isDerived() || o->hiddenFlag() || !o->getMesh())
      continue;
    const auto [lo, hi] = o->getWorldAABB();
    auto it = boxes.find(o->modHost());
    if (it == boxes.end())
      boxes.emplace(o->modHost(), std::make_pair(glm::dvec3(lo), glm::dvec3(hi)));
    else {
      it->second.first = glm::min(it->second.first, glm::dvec3(lo));
      it->second.second = glm::max(it->second.second, glm::dvec3(hi));
    }
  }
  std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
  out.reserve(boxes.size());
  for (const auto &[host, box]: boxes)
    out.push_back(box);
  return out;
}

void Engine3D::toggleEditSource() {
  if (m_editSourceHost) {
    setEditSourceHost(0);
    return;
  }
  const quint32 h = panelModHost();
  if (!h) {
    emit toastRequested(tr("Select a modified object first"));
    return;
  }
  setEditSourceHost(h);
}

void Engine3D::addSelectionToModHost() {
  if (!m_scene || !m_editSourceHost)
    return;
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  std::vector<quint32> was;
  for (const quint32 id: withMirrorTwins(m_selectedObjects))
    if (const auto o = m_scene->findById(id); o && !o->isDerived() && o->modHost() != m_editSourceHost) {
      objs.push_back(o);
      was.push_back(o->modHost());
    }
  if (objs.empty())
    return;
  const quint32 host = m_editSourceHost;
  auto apply = [this, objs](const std::vector<quint32> &v) {
    for (size_t i = 0; i < objs.size(); ++i)
      objs[i]->setModHost(v[i]);
    if (m_scene)
      ++m_scene->modEpoch;
    refreshModVisibility();
    emit modHostsChanged(); // membership: the outliner's relations, the panels
  };
  const QString label = QStringLiteral("Add to modified object");
  // Sources all in one mirror collection: what joins is mirrored into it too (the copies are sources as well).
  const eng::Scene::MirrorLink *mirrorOf = nullptr;
  for (const quint32 id: modSources(host)) {
    const auto o = m_scene->findById(id);
    const eng::Scene::MirrorLink *l = o && o->mirrorLink() ? m_scene->findMirrorLink(o->mirrorLink()) : nullptr;
    if (!l || (mirrorOf && (l->set != mirrorOf->set || !(l->plane == mirrorOf->plane)))) {
      mirrorOf = nullptr;
      break;
    }
    mirrorOf = l;
  }
  const std::optional<eng::Scene::MirrorLink> mirror =
      mirrorOf ? std::optional<eng::Scene::MirrorLink>(*mirrorOf) : std::nullopt;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  const std::vector<quint32> now(objs.size(), host);
  apply(now);
  if (record)
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, was] { apply(was); },
                                                           [apply, now] { apply(now); }));
  // Brushes taken from another modified object may leave it empty.
  dropEmptyModHosts({was.begin(), was.end()}, label);
  if (mirror) {
    std::set<quint32> unlinked;
    for (const auto &o: objs)
      if (!o->mirrorLink())
        unlinked.insert(o->stableId());
    if (!unlinked.empty())
      mirrorUnits(unlinked, mirror->plane, mirror->set, mirror->mirrorTextures, label);
  }
  if (record)
    m_undoStack->endMacro();
  pumpModifiers(ModPump::Blocking);
}

void Engine3D::removeSelectionFromModHost() {
  if (!m_scene)
    return;
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  std::vector<quint32> was;
  for (const quint32 id: withMirrorTwins(m_selectedObjects))
    if (const auto o = m_scene->findById(id); o && !o->isDerived() && o->modHost()) {
      objs.push_back(o);
      was.push_back(o->modHost());
    }
  if (objs.empty())
    return;
  auto apply = [this, objs](const std::vector<quint32> &v) {
    for (size_t i = 0; i < objs.size(); ++i)
      objs[i]->setModHost(v[i]);
    if (m_scene)
      ++m_scene->modEpoch;
    refreshModVisibility();
    emit modHostsChanged(); // membership: the outliner's relations, the panels
  };
  const QString label = QStringLiteral("Remove from modified object");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  const std::vector<quint32> now(objs.size(), 0u);
  apply(now);
  if (record)
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, was] { apply(was); },
                                                           [apply, now] { apply(now); }));
  dropEmptyModHosts({was.begin(), was.end()}, label);
  if (record)
    m_undoStack->endMacro();
  pumpModifiers(ModPump::Blocking);
}

QString Engine3D::newModHostName(const QString &kind, const quint32 group) const {
  const QString what = kind.isEmpty() ? tr("Modified") : kindLabel(kind);
  QString base = what;
  if (group != eng::kWorldspawnGroup && m_scene)
    if (const eng::MeshGroup *g = m_scene->findGroup(group))
      base = QStringLiteral("%1 · %2").arg(g->name, what);
  auto taken = [this](const QString &n) {
    if (!m_scene)
      return false;
    for (const auto &h: m_scene->modHosts)
      if (h.name.compare(n, Qt::CaseInsensitive) == 0)
        return true;
    for (const auto &st: m_scene->mirrorSets)
      if (st.name.compare(n, Qt::CaseInsensitive) == 0)
        return true;
    return false;
  };
  // "test · Curve" alone while it is free; plain kinds are always numbered.
  if (base != what && !taken(base))
    return base;
  for (int n = 1;; ++n)
    if (const QString name = QStringLiteral("%1 %2").arg(base).arg(n); !taken(name))
      return name;
}

void Engine3D::dropEmptyModHosts(const std::set<quint32> &hosts, const QString &label) {
  if (!m_scene)
    return;
  std::set<quint32> empty;
  for (const quint32 h: hosts)
    if (h && h != m_editSourceHost && m_scene->findModHost(h) && modSources(h).empty())
      empty.insert(h);
  if (empty.empty())
    return;
  const auto before = m_scene->modHosts;
  m_scene->modHosts.erase(std::remove_if(m_scene->modHosts.begin(), m_scene->modHosts.end(),
                                         [&empty](const auto &h) { return empty.count(h.id) != 0; }),
                          m_scene->modHosts.end());
  for (const quint32 h: empty)
    m_selectedMods.erase(h);
  pushModTable(before, label);
}

std::set<quint32> Engine3D::withMirrorTwins(const std::set<quint32> &ids) const {
  std::set<quint32> out = ids;
  if (!m_scene)
    return out;
  std::set<std::pair<quint32, int>> others;
  for (const quint32 id: ids)
    if (const auto o = m_scene->findById(id); o && o->mirrorLink())
      others.insert({o->mirrorLink(), 1 - o->mirrorSide()});
  if (others.empty())
    return out;
  for (const auto &o: m_scene->gameObjects)
    if (o && !o->isDerived() && o->mirrorLink() && others.count({o->mirrorLink(), o->mirrorSide()}))
      out.insert(o->stableId());
  return out;
}

std::vector<quint32> Engine3D::relinkHostDuplicates(const DuplicateSet &set, const QString &label) {
  std::vector<quint32> made;
  if (!m_scene)
    return made;
  const auto before = m_scene->modHosts;
  std::map<quint32, quint32> remap;
  std::vector<std::pair<std::shared_ptr<eng::GameObject>, quint32>> tags; // clone, new host
  for (const auto &c: set.clones) {
    const quint32 old = c->modHost();
    if (!old || old == m_editSourceHost || !set.wholeHosts.count(old))
      continue; // a source picked on its own: its copy stays in that host
    auto it = remap.find(old);
    if (it == remap.end()) {
      const eng::mods::Host *src = m_scene->findModHost(old);
      if (!src)
        continue;
      eng::mods::Host h = *src;
      h.id = m_scene->mintModHostId();
      h.name = newModHostName(src->stack.empty() ? QString() : src->stack.front().kind, c->group());
      m_scene->modHosts.push_back(h);
      it = remap.emplace(old, h.id).first;
      made.push_back(h.id);
    }
    tags.emplace_back(c, it->second);
  }
  if (tags.empty())
    return made;
  auto apply = [this, tags](bool on) {
    for (const auto &[o, h]: tags)
      o->setModHost(on ? h : 0);
    if (m_scene)
      ++m_scene->modEpoch;
  };
  apply(true);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply] { apply(false); }, [apply] { apply(true); }));
  pushModTable(before, label);
  return made;
}

namespace {

bool followsPath(const eng::mods::Host &h) {
  for (const auto &m: h.stack)
    if (m.enabled && m.kind == QLatin1String("curve"))
      return true;
  return false;
}

} // namespace

std::vector<quint32> Engine3D::moveTargets() const {
  std::vector<quint32> out(m_selectedObjects.begin(), m_selectedObjects.end());
  if (!m_scene)
    return out;
  for (const quint32 h: m_selectedMods) {
    const eng::mods::Host *host = m_scene->findModHost(h);
    if (!host || followsPath(*host))
      continue;
    for (const quint32 id: modSources(h))
      if (std::find(out.begin(), out.end(), id) == out.end())
        out.push_back(id);
  }
  return out;
}

QString Engine3D::modMoveBlocked() const {
  if (!m_scene)
    return {};
  for (const quint32 h: m_selectedMods)
    if (const eng::mods::Host *host = m_scene->findModHost(h); host && followsPath(*host))
      return tr("%1 follows a path: move the path (or its anchors) instead").arg(host->name);
  return {};
}

} // namespace vk
