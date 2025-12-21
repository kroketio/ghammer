// Mesh groups: membership on GameObject::group(), the list on the Scene; every edit goes through recordGroupEdit.

#include <tuple>

#include "vk_engine.h"

#include "client/engine/scene.h"

namespace vk {

namespace {
const std::vector<eng::MeshGroup> kNoGroups;
std::vector<quint32> toVector(const std::set<quint32> &s) { return {s.begin(), s.end()}; }

struct GroupSnapshot {
  std::vector<eng::MeshGroup> groups;
  quint32 active = 0;
  std::vector<std::tuple<quint32, quint32, bool, bool>> objects; // id, group, hidden, locked
};
} // namespace

const std::vector<eng::MeshGroup> &Engine3D::meshGroups() const {
  return m_scene ? m_scene->groups : kNoGroups;
}

quint32 Engine3D::activeMeshGroup() const {
  return m_scene ? m_scene->activeGroup : eng::kWorldspawnGroup;
}

void Engine3D::recordGroupEdit(const QString &label, const std::vector<quint32> &objects,
                               const std::function<void()> &mutate) {
  if (!m_scene)
    return;
  auto capture = [this, objects] {
    GroupSnapshot s;
    s.groups = m_scene->groups;
    s.active = m_scene->activeGroup;
    for (const quint32 id: objects)
      if (const auto o = m_scene->findById(id))
        s.objects.emplace_back(id, o->group(), o->hiddenFlag(), o->locked());
    return s;
  };
  auto apply = [this](const GroupSnapshot &s) {
    if (!m_scene)
      return;
    m_scene->groups = s.groups;
    m_scene->activeGroup = s.active;
    for (const auto &[id, g, hidden, locked]: s.objects)
      if (const auto o = m_scene->findById(id)) {
        o->setGroup(g);
        o->setHidden(hidden);
        o->setLocked(locked);
      }
    m_scene->normalizeGroups();
    // Hidden or locked brushes cannot stay selected.
    std::vector<quint32> keep;
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->hidden() && !o->locked())
        keep.push_back(id);
    if (keep.size() != m_selectedObjects.size())
      setSelected(keep, toVector(m_selectedFaces));
    invalidateDerivedCaches();
    emit meshGroupsChanged();
    emit objectStateChanged();
    emit sceneChanged();
    requestRedraw();
  };
  const GroupSnapshot before = capture();
  mutate();
  const GroupSnapshot after = capture();
  apply(after);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, before] { apply(before); },
                                                           [apply, after] { apply(after); }));
}

void Engine3D::setActiveMeshGroup(const quint32 id) {
  if (!m_scene || !m_scene->findGroup(id) || m_scene->activeGroup == id)
    return;
  // Not an undo step: a tool setting.
  m_scene->activeGroup = id;
  emit meshGroupsChanged();
}

QString Engine3D::uniqueGroupName(const QString &base) const {
  const QString stem = base.trimmed().isEmpty() ? QStringLiteral("group") : base.trimmed();
  auto taken = [this](const QString &n) {
    for (const auto &g: meshGroups())
      if (g.name.compare(n, Qt::CaseInsensitive) == 0)
        return true;
    return false;
  };
  if (!taken(stem))
    return stem;
  for (int i = 2;; ++i) {
    const QString n = QStringLiteral("%1_%2").arg(stem).arg(i);
    if (!taken(n))
      return n;
  }
}

quint32 Engine3D::createMeshGroup(const QString &name, const bool moveSelection, const bool makeActive) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  const quint32 id = m_scene->mintGroupId();
  const QString unique = uniqueGroupName(name);
  std::vector<quint32> members;
  if (moveSelection)
    members = toVector(m_selectedObjects);
  recordGroupEdit(QStringLiteral("New Group \"%1\"").arg(unique), members, [&] {
    eng::MeshGroup g;
    g.id = id;
    g.name = unique;
    g.color = QColor::fromHsv(int((id * 67 + 200) % 360), 130, 230);
    m_scene->groups.push_back(g);
    if (makeActive)
      m_scene->activeGroup = id;
    for (const quint32 oid: members)
      if (const auto o = m_scene->findById(oid))
        o->setGroup(id);
  });
  return id;
}

bool Engine3D::renameMeshGroup(const quint32 id, const QString &name) {
  if (!m_scene || id == eng::kWorldspawnGroup || name.trimmed().isEmpty())
    return false;
  const eng::MeshGroup *g = m_scene->findGroup(id);
  if (!g || g->name == name.trimmed())
    return false;
  const QString unique = uniqueGroupName(name);
  recordGroupEdit(QStringLiteral("Rename Group"), {}, [&] {
    if (eng::MeshGroup *m = m_scene->findGroup(id))
      m->name = unique;
  });
  return true;
}

void Engine3D::setMeshGroupColor(const quint32 id, const QColor &color) {
  if (!m_scene || !m_scene->findGroup(id) || !color.isValid())
    return;
  recordGroupEdit(QStringLiteral("Group Colour"), {}, [&] {
    if (eng::MeshGroup *m = m_scene->findGroup(id))
      m->color = color;
  });
}

void Engine3D::setMeshGroupHidden(const quint32 id, const bool hidden) {
  if (!m_scene || !m_scene->findGroup(id))
    return;
  std::vector<quint32> members;
  for (const auto &o: m_scene->gameObjects)
    if (o->group() == id && !o->isDerived())
      members.push_back(o->stableId());
  recordGroupEdit(hidden ? QStringLiteral("Hide Group") : QStringLiteral("Show Group"), members, [&] {
    if (eng::MeshGroup *m = m_scene->findGroup(id))
      m->hidden = hidden;
    for (const quint32 oid: members)
      if (const auto o = m_scene->findById(oid))
        o->setHidden(hidden);
  });
}

void Engine3D::setMeshGroupLocked(const quint32 id, const bool locked) {
  if (!m_scene || !m_scene->findGroup(id))
    return;
  std::vector<quint32> members;
  for (const auto &o: m_scene->gameObjects)
    if (o->group() == id && !o->isDerived())
      members.push_back(o->stableId());
  recordGroupEdit(locked ? QStringLiteral("Lock Group") : QStringLiteral("Unlock Group"), members, [&] {
    if (eng::MeshGroup *m = m_scene->findGroup(id))
      m->locked = locked;
    for (const quint32 oid: members)
      if (const auto o = m_scene->findById(oid))
        o->setLocked(locked);
  });
}

void Engine3D::soloMeshGroup(const quint32 id) {
  if (!m_scene || !m_scene->findGroup(id))
    return;
  bool othersVisible = false;
  for (const auto &g: m_scene->groups)
    othersVisible |= g.id != id && !g.hidden;
  std::vector<quint32> members;
  for (const auto &o: m_scene->gameObjects)
    if (!o->isDerived())
      members.push_back(o->stableId());
  recordGroupEdit(othersVisible ? QStringLiteral("Solo Group") : QStringLiteral("Show All Groups"), members, [&] {
    for (auto &g: m_scene->groups)
      g.hidden = othersVisible && g.id != id;
    for (const quint32 oid: members)
      if (const auto o = m_scene->findById(oid)) {
        const eng::MeshGroup *g = m_scene->findGroup(o->group());
        o->setHidden(g && g->hidden);
      }
  });
}

bool Engine3D::deleteMeshGroup(const quint32 id, const bool deleteBrushes) {
  if (!m_scene || id == eng::kWorldspawnGroup || !m_scene->findGroup(id))
    return false;
  std::vector<quint32> members;
  for (const auto &o: m_scene->gameObjects)
    if (o->group() == id && !o->isDerived())
      members.push_back(o->stableId());
  const bool macro = m_undoStack && !m_undoStack->isApplying();
  if (macro)
    m_undoStack->beginMacro(QStringLiteral("Delete Group"));
  if (deleteBrushes && !members.empty())
    replaceObjects(members, {}, QStringLiteral("Delete Group Brushes"));
  recordGroupEdit(QStringLiteral("Delete Group"), deleteBrushes ? std::vector<quint32> {} : members, [&] {
    for (const quint32 oid: members)
      if (const auto o = m_scene->findById(oid)) {
        o->setGroup(eng::kWorldspawnGroup);
        // Worldspawn's own flags apply now.
        o->setHidden(m_scene->groups.front().hidden);
        o->setLocked(m_scene->groups.front().locked);
      }
    auto &gs = m_scene->groups;
    gs.erase(std::remove_if(gs.begin(), gs.end(), [id](const eng::MeshGroup &g) { return g.id == id; }), gs.end());
    if (m_scene->activeGroup == id)
      m_scene->activeGroup = eng::kWorldspawnGroup;
  });
  if (macro)
    m_undoStack->endMacro();
  return true;
}

void Engine3D::moveSelectionToGroup(const quint32 id) {
  if (!m_scene || !m_scene->findGroup(id) || m_selectedObjects.empty())
    return;
  const std::vector<quint32> members = toVector(m_selectedObjects);
  const eng::MeshGroup target = *m_scene->findGroup(id);
  recordGroupEdit(QStringLiteral("Move to \"%1\"").arg(target.name), members, [&] {
    for (const quint32 oid: members)
      if (const auto o = m_scene->findById(oid)) {
        o->setGroup(id);
        if (target.hidden)
          o->setHidden(true);
        if (target.locked)
          o->setLocked(true);
      }
  });
}

void Engine3D::selectMeshGroup(const quint32 id, const bool additive) {
  if (!m_scene)
    return;
  std::vector<quint32> ids = additive ? toVector(m_selectedObjects) : std::vector<quint32> {};
  for (const auto &o: m_scene->gameObjects)
    if (o->group() == id && !o->isDerived() && !o->hidden() && !o->locked())
      ids.push_back(o->stableId());
  setSelected(ids, {});
}

QHash<quint32, int> Engine3D::meshGroupCounts() const {
  QHash<quint32, int> out;
  if (m_scene)
    for (const auto &o: m_scene->gameObjects)
      if (!o->isDerived())
        ++out[o->group()];
  return out;
}

QHash<quint32, int> Engine3D::meshGroupSelectedCounts() const {
  QHash<quint32, int> out;
  if (m_scene)
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id))
        ++out[o->group()];
  return out;
}

} // namespace vk
