// Face groups: membership is stored per face on the mesh; the names live in Scene::faceGroups.

#include "vk_engine.h"
#include "vk_undo.h"

#include "client/engine/scene.h"

namespace vk {

const eng::FaceGroupTable *Engine3D::faceGroupTable() const { return m_scene ? &m_scene->faceGroups : nullptr; }

std::vector<Engine3D::FaceGroupUse> Engine3D::faceGroupsOfSelection() const {
  std::vector<FaceGroupUse> out;
  if (!m_scene)
    return out;
  std::map<quint32, int> counts;
  for (const auto &[sid, f]: selectedFaceRefs())
    if (const auto o = m_scene->findById(sid); o && o->getMesh())
      for (const quint32 g: o->getMesh()->faceGroups(f))
        ++counts[g];
  for (const auto &g: m_scene->faceGroups.all())
    if (const auto it = counts.find(g.id); it != counts.end())
      out.push_back({g.id, g.name, g.color, g.builtin, it->second});
  return out;
}

void Engine3D::editSelectionGroups(const QString &label, const std::function<void(eng::Mesh &, int)> &edit,
                                   const eng::FaceGroupTable *tableBefore) {
  const auto refs = selectedFaceRefs();
  std::set<quint32> owners;
  for (const auto &r: refs)
    owners.insert(r.first);
  std::vector<ObjectState> before, after;
  for (const quint32 id: owners)
    before.push_back(captureObjectState(id, true));
  for (const auto &[sid, f]: refs)
    if (const auto o = m_scene->findById(sid); o && o->getMesh())
      edit(*o->getMesh(), f);
  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  recordFaceGroupEdit(label, tableBefore, std::move(before), std::move(after));
}

void Engine3D::recordFaceGroupEdit(const QString &label, const eng::FaceGroupTable *tableBefore,
                                   std::vector<ObjectState> before, std::vector<ObjectState> after) {
  if (m_undoStack && !m_undoStack->isApplying()) {
    m_undoStack->beginMacro(label);
    if (tableBefore) {
      auto apply = [this](const eng::FaceGroupTable &t) {
        if (!m_scene)
          return;
        m_scene->faceGroups = t;
        emit faceGroupsChanged();
      };
      const eng::FaceGroupTable was = *tableBefore, now = m_scene->faceGroups;
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, was] { apply(was); },
                                                             [apply, now] { apply(now); }));
    }
    if (!before.empty())
      m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, std::move(before), std::move(after)));
    m_undoStack->endMacro();
  }
  emit faceGroupsChanged();
  emit sceneChanged();
}

bool Engine3D::addSelectionToFaceGroup(const QString &name) {
  if (!m_scene || name.trimmed().isEmpty() || selectedFaceRefs().empty())
    return false;
  const eng::FaceGroupTable before = m_scene->faceGroups;
  const quint32 id = m_scene->faceGroups.ensure(name);
  if (id == 0)
    return false;
  const bool created = before.find(id) == nullptr;
  editSelectionGroups(QStringLiteral("Add to face group \"%1\"").arg(m_scene->faceGroups.name(id)),
                      [id](eng::Mesh &m, int f) { m.addFaceGroup(f, id); }, created ? &before : nullptr);
  return true;
}

bool Engine3D::removeSelectionFromFaceGroup(const quint32 id) {
  if (!m_scene || selectedFaceRefs().empty())
    return false;
  editSelectionGroups(QStringLiteral("Remove from face group \"%1\"").arg(m_scene->faceGroups.name(id)),
                      [id](eng::Mesh &m, int f) { m.removeFaceGroup(f, id); }, nullptr);
  return true;
}

std::vector<quint32> Engine3D::facesInGroup(const quint32 id) const {
  std::vector<quint32> faces;
  if (!m_scene)
    return faces;
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o ? o->getMesh() : nullptr;
    if (!mesh || o->hidden())
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (eng::facegroup::contains(mesh->faceGroups(f), id) && mesh->getFacePickingID(f) != 0)
        faces.push_back(mesh->getFacePickingID(f));
  }
  return faces;
}

int Engine3D::selectFaceGroup(const quint32 id, const bool add) {
  std::vector<quint32> faces = facesInGroup(id);
  if (add)
    faces.insert(faces.end(), m_selectedFaces.begin(), m_selectedFaces.end());
  clearAutoSelectRegion();
  setSelected({}, faces);
  return int(faces.size());
}

bool Engine3D::renameFaceGroup(const quint32 id, const QString &name) {
  if (!m_scene)
    return false;
  const eng::FaceGroupTable before = m_scene->faceGroups;
  if (!m_scene->faceGroups.rename(id, name))
    return false;
  recordFaceGroupEdit(QStringLiteral("Rename face group"), &before, {}, {});
  return true;
}

bool Engine3D::deleteFaceGroup(const quint32 id) {
  if (!m_scene)
    return false;
  const eng::FaceGroupTable before = m_scene->faceGroups;
  const eng::FaceGroup *g = before.find(id);
  if (!g || g->builtin)
    return false;
  std::vector<ObjectState> was, now;
  std::vector<std::shared_ptr<eng::GameObject>> touched;
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o ? o->getMesh() : nullptr;
    if (!mesh)
      continue;
    bool has = false;
    for (int f = 0; f < mesh->faceCount() && !has; ++f)
      has = eng::facegroup::contains(mesh->faceGroups(f), id);
    if (has)
      touched.push_back(o);
  }
  for (const auto &o: touched) {
    was.push_back(captureObjectState(o->stableId(), true));
    for (int f = 0; f < o->getMesh()->faceCount(); ++f)
      o->getMesh()->removeFaceGroup(f, id);
    now.push_back(captureObjectState(o->stableId(), true));
  }
  m_scene->faceGroups.remove(id);
  recordFaceGroupEdit(QStringLiteral("Delete face group \"%1\"").arg(g->name), &before, std::move(was), std::move(now));
  return true;
}

} // namespace vk
