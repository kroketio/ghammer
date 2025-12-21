// Brush entities: ordinary brushes tagged by GameObject::brushEntity(); the entity's origin follows their centre.

#include <cfloat>
#include <map>

#include <QSet>

#include <QGuiApplication>

#include "vk_engine.h"

#include "client/engine/entity.h"
#include "client/engine/scene.h"

namespace vk {

std::vector<quint32> Engine3D::brushesOfEntity(const quint32 entityId) const {
  std::vector<quint32> out;
  if (!m_scene || !entityId)
    return out;
  for (const auto &o: m_scene->gameObjects)
    if (o->brushEntity() == entityId)
      out.push_back(o->stableId());
  return out;
}

bool Engine3D::selectionHasBrushEntity() const {
  if (!m_scene)
    return false;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && o->brushEntity())
      return true;
  return false;
}

void Engine3D::syncBrushEntityOrigins() {
  if (!m_scene)
    return;
  bool any = false;
  for (auto &e: m_scene->entities) {
    const eng::EntityClass *c = e.cls();
    if (!c || !c->isBrushEntity())
      continue;
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    bool has = false;
    for (const auto &o: m_scene->gameObjects)
      if (o->brushEntity() == e.id) {
        const auto [lo, hi] = o->getWorldAABB();
        mn = glm::min(mn, lo);
        mx = glm::max(mx, hi);
        has = true;
      }
    if (!has)
      continue;
    const glm::vec3 c3 = (mn + mx) * 0.5f;
    if (glm::distance(c3, e.origin) > 1e-4f) {
      e.origin = c3;
      any = true;
    }
  }
  if (any) {
    ++m_entityEpoch;
    emit entitiesChanged();
  }
}

void Engine3D::expandSelectionToBrushEntities() {
  if (!m_scene || QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier))
    return;
  std::set<quint32> owners, procs, groups;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id)) {
      if (o->brushEntity())
        owners.insert(o->brushEntity());
      if (o->procObject())
        procs.insert(o->procObject());
      if (o->group() != eng::kWorldspawnGroup)
        groups.insert(o->group());
    }
  // Generated objects and groups are picked as a whole (Ctrl picks one brush).
  for (const auto &o: m_scene->gameObjects)
    if (!o->isDerived() && !o->hidden() && !o->locked()
        && ((o->procObject() && procs.count(o->procObject())) || groups.count(o->group())))
      m_selectedObjects.insert(o->stableId());
  for (const quint32 eid: owners) {
    for (const quint32 b: brushesOfEntity(eid))
      if (const auto o = m_scene->findById(b); o && !o->hidden() && !o->locked())
        m_selectedObjects.insert(b);
    if (m_scene->findEntity(eid))
      m_selectedEntities.insert(eid);
  }
}

void Engine3D::dropSelectionUnitOf(const quint32 id) {
  if (!m_scene || QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier))
    return;
  const auto o = m_scene->findById(id);
  if (!o)
    return;
  const quint32 proc = o->procObject(), group = o->group(), owner = o->brushEntity();
  for (const auto &x: m_scene->gameObjects)
    if ((proc && x->procObject() == proc) || (group != eng::kWorldspawnGroup && x->group() == group)
        || (owner && x->brushEntity() == owner))
      m_selectedObjects.erase(x->stableId());
  if (owner)
    m_selectedEntities.erase(owner);
}

void Engine3D::cloneBrushEntities(const std::vector<quint32> &originals,
                                  const std::vector<std::shared_ptr<eng::GameObject>> &clones, DuplicateSet &set) {
  if (!m_scene)
    return;
  // Which entities are copied whole.
  std::map<quint32, size_t> copied;
  for (size_t k = 0; k < originals.size() && k < clones.size(); ++k)
    if (const auto src = m_scene->findById(originals[k]); src && src->brushEntity() && !src->procObject())
      ++copied[src->brushEntity()];
  std::map<quint32, quint32> remap;
  QSet<QString> taken;
  for (const auto &[eid, n]: copied) {
    const eng::Entity *e = entityById(eid);
    size_t own = 0;  // its brushes (modifier output carries the id too)
    for (const auto &o: m_scene->gameObjects)
      if (o->brushEntity() == eid && !o->isDerived())
        ++own;
    if (!e || n < own)
      continue;  // a partial copy joins the same entity
    eng::Entity copy = *e;
    copy.id = m_scene->mintEntityId();
    if (!copy.name.isEmpty()) {
      copy.name = uniqueEntityName(copy.name, taken);
      taken.insert(copy.name.toLower());
    }
    m_scene->entities.push_back(copy);
    set.entitiesAdded.push_back(copy);
    remap[eid] = copy.id;
  }
  if (remap.empty())
    return;
  for (const auto &c: clones)
    if (const auto it = remap.find(c->brushEntity()); it != remap.end())
      c->setBrushEntity(it->second);
  ++m_entityEpoch;
  emit entitiesChanged();
}

quint32 Engine3D::makeBrushEntity(const QString &classname) {
  if (!m_scene || m_selectedObjects.empty())
    return 0;
  const eng::EntityClass *cls = eng::EntityRegistry::instance().find(classname);
  if (!cls || !cls->isBrushEntity())
    return 0;
  std::vector<quint32> brushes;
  glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && !o->isDerived() && !isShapePreview(id)) {
      brushes.push_back(id);
      const auto [lo, hi] = o->getWorldAABB();
      mn = glm::min(mn, lo);
      mx = glm::max(mx, hi);
    }
  if (brushes.empty())
    return 0;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Make %1").arg(cls->label));
  // Brushes of another entity move over; that entity is removed when left empty.
  std::vector<std::pair<quint32, quint32>> before;
  for (const quint32 b: brushes)
    before.emplace_back(b, m_scene->findById(b)->brushEntity());
  // Exactly one entity of this class selected: the other brushes join it (name, properties, I/O kept).
  quint32 join = 0;
  bool several = false;
  for (const auto &[b, e]: before)
    if (const eng::Entity *ent = e ? entityById(e) : nullptr; ent && ent->classname == classname) {
      if (join && join != e)
        several = true;
      join = e;
    }
  const quint32 eid = join && !several ? join : addEntity(classname, (mn + mx) * 0.5f);
  auto apply = [this](const std::vector<std::pair<quint32, quint32>> &st) {
    for (const auto &[b, e]: st)
      if (const auto o = m_scene ? m_scene->findById(b) : nullptr)
        o->setBrushEntity(e);
    syncBrushEntityOrigins();
    emit objectStateChanged();
    emit entitiesChanged();
  };
  std::vector<std::pair<quint32, quint32>> after;
  for (const auto &[b, e]: before)
    after.emplace_back(b, eid);
  apply(after);
  if (record)
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Tie brushes"), [apply, before] { apply(before); },
                                                           [apply, after] { apply(after); }));
  const bool isTrigger = classname.startsWith(QLatin1String("trigger_"));
  if (classname == eng::kClipClass || isTrigger)
    if (const auto mat = isTrigger ? triggerMaterial() : collisionMaterial()) {
      std::vector<ObjectState> was, now;
      for (const quint32 b: brushes)
        was.push_back(captureObjectState(b, true));
      for (const quint32 b: brushes)
        if (const auto o = m_scene->findById(b); o && o->getMesh())
          for (int f = 0; f < o->getMesh()->faceCount(); ++f)
            o->getMesh()->setFaceMaterial(f, mat);
      for (const quint32 b: brushes)
        now.push_back(captureObjectState(b, true));
      if (record)
        m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, isTrigger ? QStringLiteral("Trigger material")
                                                                                : QStringLiteral("Collision material"),
                                                                std::move(was), std::move(now)));
      emit sceneChanged();
    }
  std::vector<quint32> empty;
  for (const auto &[b, e]: before)
    if (e && brushesOfEntity(e).empty() && std::find(empty.begin(), empty.end(), e) == empty.end())
      empty.push_back(e);
  if (!empty.empty())
    deleteEntities(empty);
  if (record)
    m_undoStack->endMacro();
  m_selectedEntities.insert(eid);
  emit selectionChanged();
  return eid;
}

int Engine3D::selectionSmoothShading() const {
  if (!m_scene)
    return -1;
  int smooth = 0, flat = 0;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && !o->isDerived() && o->getMesh())
      (o->smoothShading() ? smooth : flat)++;
  if (smooth + flat == 0 || (smooth && flat))
    return -1;
  return smooth ? 1 : 0;
}

void Engine3D::setSelectionSmoothShading(const bool on) {
  if (!m_scene)
    return;
  std::vector<std::pair<quint32, bool>> before;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && !o->isDerived() && o->getMesh() && o->smoothShading() != on)
      before.emplace_back(id, o->smoothShading());
  if (before.empty())
    return;
  auto apply = [this](const std::vector<std::pair<quint32, bool>> &st) {
    for (const auto &[id, smooth]: st)
      if (const auto o = m_scene ? m_scene->findById(id) : nullptr)
        o->setSmoothShading(smooth);
    emit objectStateChanged();
    emit sceneChanged();
    requestRedraw();
  };
  std::vector<std::pair<quint32, bool>> after;
  for (const auto &[id, smooth]: before)
    after.emplace_back(id, on);
  apply(after);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(on ? tr("Smooth shading") : tr("Flat shading"),
                                                           [apply, before] { apply(before); },
                                                           [apply, after] { apply(after); }));
}

void Engine3D::moveSelectionToWorldspawn() {
  if (!m_scene || m_selectedObjects.empty())
    return;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Move to worldspawn"));
  moveSelectionToWorld(); // brush entities let go of them (no-op without any)
  moveSelectionToGroup(eng::kWorldspawnGroup);
  if (record)
    m_undoStack->endMacro();
}

bool Engine3D::moveSelectionToWorld() {
  if (!m_scene)
    return false;
  std::vector<std::pair<quint32, quint32>> before;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && o->brushEntity())
      before.emplace_back(id, o->brushEntity());
  if (before.empty())
    return false;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Move to World"));
  auto apply = [this](const std::vector<std::pair<quint32, quint32>> &st) {
    for (const auto &[b, e]: st)
      if (const auto o = m_scene ? m_scene->findById(b) : nullptr)
        o->setBrushEntity(e);
    syncBrushEntityOrigins();
    emit objectStateChanged();
    emit entitiesChanged();
  };
  std::vector<std::pair<quint32, quint32>> after;
  for (const auto &[b, e]: before)
    after.emplace_back(b, 0);
  apply(after);
  if (record)
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Untie brushes"), [apply, before] { apply(before); },
                                                           [apply, after] { apply(after); }));
  std::vector<quint32> empty;
  for (const auto &[b, e]: before)
    if (brushesOfEntity(e).empty() && std::find(empty.begin(), empty.end(), e) == empty.end())
      empty.push_back(e);
  if (!empty.empty())
    deleteEntities(empty);
  for (const quint32 e: empty)
    m_selectedEntities.erase(e);
  if (record)
    m_undoStack->endMacro();
  emit selectionChanged();
  return true;
}

} // namespace vk
