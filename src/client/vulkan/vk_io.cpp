// Entity I/O on the engine side (the model itself is engine/io.*).

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include "vk_engine.h"
#include "vk_undo.h"

#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

using eng::io::Connection;
using eng::io::ConnectionList;

namespace {

eng::Entity *findEnt(const QSharedPointer<eng::Scene> &scene, quint32 id) {
  return scene ? scene->findEntity(id) : nullptr;
}

} // namespace

void Engine3D::setEntityOutputs(const quint32 id, const ConnectionList &outputs, const QString &text) {
  eng::Entity *e = findEnt(m_scene, id);
  if (!e || e->outputs == outputs)
    return;
  const ConnectionList before = e->outputs;
  auto apply = [this, id](const ConnectionList &v) {
    if (eng::Entity *x = findEnt(m_scene, id)) {
      x->outputs = v;
      ++m_entityEpoch;
      emit entitiesChanged();
    }
  };
  apply(outputs);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(text.isEmpty() ? QStringLiteral("Edit outputs") : text,
                                                           [apply, before] { apply(before); },
                                                           [apply, outputs] { apply(outputs); }));
}

void Engine3D::renameEntity(const quint32 id, const QString &rawName) {
  eng::Entity *e = findEnt(m_scene, id);
  if (!e)
    return;
  const QString name = rawName.trimmed();
  const QString old = e->name;
  if (name == old)
    return;
  const bool updateRefs = !old.isEmpty() && prefs::b("IO/RenameUpdatesReferences")
      && eng::io::countNameReferences(m_scene->entities, old) > 0;
  // Another entity still answering to the old name keeps the references valid (they address the set).
  bool othersKeepName = false;
  for (const auto &x: m_scene->entities)
    othersKeepName = othersKeepName || (x.id != id && x.name.compare(old, Qt::CaseInsensitive) == 0);
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (!updateRefs || othersKeepName) {
    setEntityTransform(id, e->origin, e->angles, name);
    return;
  }
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Rename %1 → %2").arg(old, name));
  setEntityTransform(id, e->origin, e->angles, name);
  // Snapshot every entity whose references change, then apply as one step.
  std::vector<std::pair<quint32, eng::Entity>> before, after;
  for (const auto &x: m_scene->entities) {
    eng::Entity copy = x;
    if (eng::io::renameReferences(copy, old, name)) {
      before.emplace_back(x.id, x);
      after.emplace_back(x.id, copy);
    }
  }
  auto apply = [this](const std::vector<std::pair<quint32, eng::Entity>> &v) {
    for (const auto &[eid, ent]: v)
      if (eng::Entity *x = findEnt(m_scene, eid)) {
        x->outputs = ent.outputs;
        x->props = ent.props;
      }
    ++m_entityEpoch;
    emit entitiesChanged();
  };
  apply(after);
  if (record) {
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Update references"),
                                                           [apply, before] { apply(before); },
                                                           [apply, after] { apply(after); }));
    m_undoStack->endMacro();
  }
  qInfo().noquote() << QStringLiteral("[io] renamed %1 → %2, updated %3 entit%4").arg(old, name).arg(after.size())
                           .arg(after.size() == 1 ? "y" : "ies");
}

quint32 Engine3D::addEntityFrom(const eng::Entity &tmpl, const glm::vec3 &origin) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  const eng::EntityClass *cls = eng::EntityRegistry::instance().find(tmpl.classname);
  if (!cls)
    return 0;
  eng::Entity e = tmpl;
  e.id = m_scene->mintEntityId();
  e.origin = origin;
  const quint32 id = e.id;
  auto apply = [this, e](bool add) {
    if (!m_scene)
      return;
    auto &v = m_scene->entities;
    if (add) {
      if (!m_scene->findEntity(e.id))
        v.push_back(e);
    } else {
      v.erase(std::remove_if(v.begin(), v.end(), [&e](const eng::Entity &x) { return x.id == e.id; }), v.end());
      m_selectedEntities.erase(e.id);
    }
    ++m_entityEpoch;
    emit entitiesChanged();
    emit selectionChanged();
  };
  apply(true);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Add %1").arg(cls->label),
                                                           [apply] { apply(false); }, [apply] { apply(true); }));
  return id;
}

QString Engine3D::uniqueEntityName(const QString &base, const QSet<QString> &reserved) const {
  // "door" -> "door_1", "door_3" -> "door_4": count up past every name in use.
  static const QRegularExpression tail(QStringLiteral("^(.*?)_(\\d+)$"));
  QString stem = base.isEmpty() ? QStringLiteral("entity") : base;
  int n = 1;
  if (const auto m = tail.match(stem); m.hasMatch()) {
    stem = m.captured(1);
    n = m.captured(2).toInt() + 1;
  }
  QSet<QString> used = reserved;
  if (m_scene)
    for (const auto &e: m_scene->entities)
      if (!e.name.isEmpty())
        used.insert(e.name.toLower());
  for (;; ++n) {
    const QString cand = QStringLiteral("%1_%2").arg(stem).arg(n);
    if (!used.contains(cand.toLower()))
      return cand;
  }
}

quint32 Engine3D::ioFocusEntity() const {
  if (!m_scene)
    return 0;
  if (m_selectedEntities.size() == 1)
    return *m_selectedEntities.begin();
  // A brush entity picked through its brushes.
  quint32 owner = 0;
  for (const quint32 sid: m_selectedObjects)
    if (const auto o = m_scene->findById(sid)) {
      if (!o->brushEntity() || (owner && owner != o->brushEntity()))
        return 0;
      owner = o->brushEntity();
    }
  return owner;
}

std::vector<quint32> Engine3D::ioSelectionEntities() const {
  std::set<quint32> ids(m_selectedEntities.begin(), m_selectedEntities.end());
  if (m_scene)
    for (const quint32 sid: m_selectedObjects)
      if (const auto o = m_scene->findById(sid); o && o->brushEntity())
        ids.insert(o->brushEntity());
  return {ids.begin(), ids.end()};
}

void Engine3D::selectEntitiesWithBrushes(const std::vector<quint32> &ids) {
  if (!m_scene)
    return;
  m_selectedObjects.clear();
  m_selectedFaces.clear();
  m_selectedEntities.clear();
  for (const quint32 id: ids) {
    const eng::Entity *e = findEnt(m_scene, id);
    if (!e)
      continue;
    m_selectedEntities.insert(id);
    if (const eng::EntityClass *c = e->cls(); c && c->isBrushEntity())
      for (const quint32 b: brushesOfEntity(id))
        m_selectedObjects.insert(b);
  }
  emit selectionChanged();
}

int Engine3D::selectIOTargets() {
  if (!m_scene)
    return 0;
  std::set<quint32> hits;
  for (const quint32 id: ioSelectionEntities())
    if (const eng::Entity *e = findEnt(m_scene, id))
      for (const auto &c: e->outputs)
        for (const quint32 t: eng::io::resolve(m_scene->entities, c.target, id).ids)
          hits.insert(t);
  if (!hits.empty())
    selectEntitiesWithBrushes({hits.begin(), hits.end()});
  return int(hits.size());
}

int Engine3D::selectIOSources() {
  if (!m_scene)
    return 0;
  std::set<quint32> hits;
  for (const quint32 id: ioSelectionEntities())
    for (const auto &ref: eng::io::incoming(m_scene->entities, id))
      hits.insert(ref.source);
  if (!hits.empty())
    selectEntitiesWithBrushes({hits.begin(), hits.end()});
  return int(hits.size());
}

int Engine3D::selectSameName() {
  if (!m_scene)
    return 0;
  QSet<QString> names;
  for (const quint32 id: ioSelectionEntities())
    if (const eng::Entity *e = findEnt(m_scene, id); e && !e->name.isEmpty())
      names.insert(e->name.toLower());
  std::vector<quint32> hits;
  for (const auto &e: m_scene->entities)
    if (!e.name.isEmpty() && names.contains(e.name.toLower()))
      hits.push_back(e.id);
  if (!hits.empty())
    selectEntitiesWithBrushes(hits);
  return int(hits.size());
}

void Engine3D::setShowIOLines(const bool on) {
  if (m_showIOLines == on)
    return;
  m_showIOLines = on;
  prefs::get().setValue(QStringLiteral("View/ShowIOLines"), on);
  ++m_viewEpoch;
  emit sceneChanged();
  requestRedraw();
}

void Engine3D::setIOLinesAll(const bool on) {
  if (m_ioLinesAll == on)
    return;
  m_ioLinesAll = on;
  prefs::get().setValue(QStringLiteral("View/IOLinesAll"), on);
  ++m_viewEpoch;
  emit sceneChanged();
  requestRedraw();
}

const std::vector<IOLink> &Engine3D::ioLinks() const {
  const quint64 key = (m_entityEpoch * 1000003ull) ^ (m_viewEpoch * 2654435761ull) ^ (m_showIOLines ? 1 : 0)
      ^ (m_ioLinesAll ? 2 : 0) ^ quint64(quintptr(m_scene.data()));
  if (key == m_ioLinksKey)
    return m_ioLinks;
  m_ioLinksKey = key;
  m_ioLinks.clear();
  if (!m_scene || !m_showIOLines)
    return m_ioLinks;

  const auto &ents = m_scene->entities;
  const std::vector<quint32> selVec = ioSelectionEntities();
  const std::set<quint32> sel(selVec.begin(), selVec.end());
  QHash<quint32, const eng::Entity *> byId;
  for (const auto &e: ents)
    byId.insert(e.id, &e);

  // One link per (from, to, kind); the first connection names it.
  QHash<QString, int> slot;
  auto add = [&](const int kind, const eng::Entity &from, const eng::Entity *to, const QString &label, const bool delayed) {
    const quint32 toId = to ? to->id : 0;
    const int side = sel.count(from.id) ? 1 : (to && sel.count(toId) ? 2 : 0);
    if (!m_ioLinesAll && side == 0)
      return;
    if (from.hidden || (to && to->hidden))
      return;
    const QString k = QStringLiteral("%1>%2>%3").arg(from.id).arg(toId).arg(kind);
    if (auto it = slot.constFind(k); it != slot.constEnd()) {
      IOLink &l = m_ioLinks[size_t(*it)];
      ++l.count;
      l.delayed = l.delayed && delayed;
      return;
    }
    IOLink l;
    l.kind = kind;
    l.from = from.id;
    l.to = toId;
    l.a = from.origin;
    l.b = to ? to->origin : from.origin;
    l.selectedSide = side;
    l.delayed = delayed;
    l.label = label;
    slot.insert(k, int(m_ioLinks.size()));
    m_ioLinks.push_back(l);
  };

  for (const auto &e: ents) {
    for (const auto &c: e.outputs) {
      if (c.disabled)
        continue;
      const auto r = eng::io::resolve(ents, c.target, e.id);
      QString label = QStringLiteral("%1 → %2").arg(c.output, c.input);
      if (!c.param.isEmpty())
        label += QStringLiteral(" (%1)").arg(c.param);
      if (c.delay > 0.0f)
        label += QStringLiteral(" · %1s").arg(double(c.delay));
      if (r.kind == eng::io::TargetKind::Resolved) {
        for (const quint32 t: r.ids)
          if (const eng::Entity *te = byId.value(t))
            add(IOLink::Output, e, te, label, c.delay > 0.0f);
      } else if (r.kind == eng::io::TargetKind::Unresolved) {
        add(IOLink::Broken, e, nullptr, QStringLiteral("%1 → ? %2").arg(c.output, c.target), false);
      }
    }
    // Target props (path_node next, trigger_look target): dashed references.
    if (const eng::EntityClass *cls = e.cls())
      for (const auto &p: cls->props)
        if (p.type == eng::PropType::Target) {
          const QString v = e.value(p.key).toString().trimmed();
          if (v.isEmpty())
            continue;
          for (const quint32 t: eng::io::resolve(ents, v, e.id).ids)
            if (const eng::Entity *te = byId.value(t))
              add(IOLink::Reference, e, te, p.label, true);
        }
  }
  for (auto &l: m_ioLinks)
    if (l.count > 1)
      l.label += QStringLiteral("  (+%1)").arg(l.count - 1);
  return m_ioLinks;
}

void Engine3D::beginTargetPick(std::function<void(quint32)> onPicked) {
  m_targetPick = std::move(onPicked);
  emit targetPickChanged(true);
}

void Engine3D::cancelTargetPick() {
  if (!m_targetPick)
    return;
  m_targetPick = nullptr;
  emit targetPickChanged(false);
}

bool Engine3D::deliverTargetPick(const quint32 entityId) {
  if (!m_targetPick)
    return false;
  if (!findEnt(m_scene, entityId)) {
    cancelTargetPick();
    return true;
  }
  auto cb = std::move(m_targetPick);
  m_targetPick = nullptr;
  emit targetPickChanged(false);
  cb(entityId);
  return true;
}

quint32 Engine3D::addEntityNear(const QString &classname, const quint32 near) {
  if (const eng::Entity *n = near ? findEnt(m_scene, near) : nullptr) {
    // A step to the side of the source, so the icons do not overlap.
    const float step = float(std::max(1.0, gizmoSnapStep() * 2.0));
    glm::vec3 at = n->origin + glm::vec3(step, 0.0f, 0.0f);
    for (int tries = 0; tries < 16; ++tries) {
      bool taken = false;
      for (const auto &e: m_scene->entities)
        taken = taken || glm::distance(e.origin, at) < step * 0.5f;
      if (!taken)
        break;
      at += glm::vec3(0.0f, 0.0f, step);
    }
    return addEntity(classname, at);
  }
  return placeEntityAt(classname);
}

bool Engine3D::ioGraphPos(const quint32 id, glm::vec2 *pos) const {
  const auto it = m_ioGraphLayout.find(id);
  if (it == m_ioGraphLayout.end())
    return false;
  if (pos)
    *pos = it->second;
  return true;
}

void Engine3D::setIOGraphPos(const quint32 id, const glm::vec2 &pos) { m_ioGraphLayout[id] = pos; }

void Engine3D::captureConnections(std::vector<fmap::EntityConnectionsRecord> &out) const {
  if (!m_scene)
    return;
  for (const auto &e: m_scene->entities) {
    if (e.outputs.isEmpty())
      continue;
    fmap::EntityConnectionsRecord rec;
    rec.entityId = e.id;
    for (const auto &c: e.outputs) {
      fmap::ConnectionRecord r;
      r.output = c.output;
      r.target = c.target;
      r.input = c.input;
      r.param = c.param;
      r.delay = c.delay;
      r.times = c.times;
      r.flags = c.disabled ? fmap::kConnectionDisabled : 0u;
      rec.connections.push_back(r);
    }
    out.push_back(std::move(rec));
  }
}

void Engine3D::applyConnections(const std::vector<fmap::EntityConnectionsRecord> &records) {
  m_ioGraphLayout.clear();
  if (!m_scene)
    return;
  int n = 0;
  for (const auto &rec: records) {
    eng::Entity *e = findEnt(m_scene, rec.entityId);
    if (!e) {
      qWarning("[io] connections for missing entity %u dropped", rec.entityId);
      continue;
    }
    for (const auto &r: rec.connections) {
      Connection c;
      c.output = r.output;
      c.target = r.target;
      c.input = r.input;
      c.param = r.param;
      c.delay = r.delay;
      c.times = r.times;
      c.disabled = (r.flags & fmap::kConnectionDisabled) != 0;
      e->outputs.push_back(c);
      ++n;
    }
  }
  if (n) {
    ++m_entityEpoch;
    emit entitiesChanged();
  }
}

} // namespace vk
