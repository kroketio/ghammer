#include "vk_undo.h"

#include <algorithm>

#include <QLoggingCategory>
#include <QStringList>

#include "vk_engine.h"

namespace vk {

static QLoggingCategory undoLog("editor.undo", QtInfoMsg);

ObjectStatesCommand::ObjectStatesCommand(Engine3D *engine, QString label,
                                       std::vector<ObjectState> before, std::vector<ObjectState> after)
    : m_engine(engine), m_label(std::move(label)), m_before(std::move(before)),
      m_after(std::move(after)) {}

void ObjectStatesCommand::undo() {
  m_engine->applyObjectStates(m_before);
}

void ObjectStatesCommand::redo() {
  m_engine->applyObjectStates(m_after);
}

void ObjectStatesCommand::touched(eng::Touched &t) const {
  for (size_t i = 0; i < m_after.size(); ++i) {
    const ObjectState &a = m_after[i];
    const ObjectState *b = i < m_before.size() && m_before[i].id == a.id ? &m_before[i] : nullptr;
    if (!b)
      for (const ObjectState &s: m_before)
        if (s.id == a.id)
          b = &s;
    const bool moved = !b || b->position != a.position || b->rotation != a.rotation || b->scale != a.scale;
    if (moved)
      t.moved.push_back(a.id);
    else if (!a.faceBases.empty() || !a.faceMaterials.empty() || !a.faceGroups.empty())
      t.faces.push_back(a.id);
  }
}

DeleteCommand::DeleteCommand(Engine3D *engine) : m_engine(engine) {}

void DeleteCommand::addBrush(const std::shared_ptr<eng::GameObject> &object, const int slot) {
  if (!object)
    return;
  if (object->isDerived()) { // evaluated / render objects are never recorded
    qWarning("[modifiers] DeleteCommand: skipped derived object %u", object->stableId());
    Q_ASSERT_X(false, "DeleteCommand::addBrush", "derived object in a recorded edit");
    return;
  }
  m_objects.push_back(object);
  m_slots.push_back(slot);
}

void DeleteCommand::rememberSelection(const std::vector<quint32> &objects,
                                    const std::vector<quint32> &faces) {
  m_prevObjects = objects;
  m_prevFaces = faces;
}

QString DeleteCommand::text() const {
  QStringList parts;
  if (!m_objects.empty())
    parts << QStringLiteral("%1 brush%2").arg(m_objects.size())
                                 .arg(m_objects.size() == 1 ? QString() : QStringLiteral("es"));
  return parts.isEmpty() ? QStringLiteral("Delete") : QStringLiteral("Delete ") + parts.join(QStringLiteral(", "));
}

void DeleteCommand::redo() {
  auto *scene = m_engine->scene().get();
  if (scene == nullptr || m_objects.empty())
    return;

  std::set<quint32> ids;
  for (const auto &object: m_objects)
    ids.insert(object->stableId());

  scene->removeObjects(ids);
  m_engine->clearSelection();
  m_engine->notifyStructureChanged();
}

void DeleteCommand::undo() {
  auto *scene = m_engine->scene().get();
  if (scene == nullptr) {
    qWarning(undoLog, "[undo] cannot restore delete: no scene");
    return;
  }

  // Ascending slot order reproduces the original layout: each brush refills the gap it left.
  std::vector<size_t> order(m_objects.size());
  for (size_t k = 0; k < order.size(); ++k)
    order[k] = k;
  std::sort(order.begin(), order.end(),
            [this](const size_t a, const size_t b) { return m_slots[a] < m_slots[b]; });

  if (!m_objects.empty()) {
    std::vector<std::shared_ptr<eng::GameObject>> objects;
    std::vector<int> slotList;
    objects.reserve(m_objects.size());
    slotList.reserve(m_objects.size());
    for (const size_t k: order) {
      objects.push_back(m_objects[k]);
      slotList.push_back(m_slots[k]);
    }
    scene->insertObjects(objects, slotList);
    m_engine->notifyStructureChanged();
  }

  m_engine->setSelected(m_prevObjects, m_prevFaces);
}

DuplicateCommand::DuplicateCommand(Engine3D *engine, QString label)
    : m_engine(engine), m_label(std::move(label)) {}

void DuplicateCommand::addClone(const std::shared_ptr<eng::GameObject> &object) {
  if (!object)
    return;
  m_objects.push_back(object);
}

void DuplicateCommand::rememberSelection(const std::vector<quint32> &objects,
                                       const std::vector<quint32> &faces) {
  m_prevObjects = objects;
  m_prevFaces = faces;
}

void DuplicateCommand::redo() {
  auto *scene = m_engine->scene().get();
  if (scene == nullptr || m_objects.empty())
    return;

  std::vector<std::shared_ptr<eng::GameObject>> add;
  add.reserve(m_objects.size());
  for (const auto &object: m_objects)
    if (!scene->findById(object->stableId()))
      add.push_back(object);

  if (!add.empty()) {
    scene->addObjects(add);
    m_engine->notifyStructureChanged();
  }

  std::vector<quint32> ids;
  ids.reserve(m_objects.size());
  for (const auto &object: m_objects)
    ids.push_back(object->stableId());
  m_engine->setSelected(ids, {});

  qInfo(undoLog, "[undo] duplicate re-applied: %zu brush(es)", m_objects.size());
}

void DuplicateCommand::undo() {
  auto *scene = m_engine->scene().get();
  if (scene == nullptr) {
    qWarning(undoLog, "[undo] cannot undo duplicate: no scene");
    return;
  }

  std::set<quint32> ids;
  for (const auto &object: m_objects)
    ids.insert(object->stableId());

  scene->removeObjects(ids);
  m_engine->notifyStructureChanged();
  m_engine->setSelected(m_prevObjects, m_prevFaces);

  qInfo(undoLog, "[undo] duplicate removed: %zu brush(es)", ids.size());
}

CutCommand::CutCommand(Engine3D *engine, QString label) : m_engine(engine), m_label(std::move(label)) {}

void CutCommand::addRemoved(const std::shared_ptr<eng::GameObject> &object, const int slot) {
  if (!object)
    return;
  m_removed.push_back(object);
  m_slots.push_back(slot);
}

void CutCommand::addPiece(const std::shared_ptr<eng::GameObject> &object) {
  if (!object)
    return;
  m_pieces.push_back(object);
}

void CutCommand::rememberSelection(const std::vector<quint32> &objects,
                                  const std::vector<quint32> &faces) {
  m_prevObjects = objects;
  m_prevFaces = faces;
}

void CutCommand::redo() {
  auto *scene = m_engine->scene().get();
  if (scene == nullptr) {
    qWarning(undoLog, "[undo] cannot re-apply cut: no scene");
    return;
  }

  std::set<quint32> gone;
  for (const auto &object: m_removed)
    gone.insert(object->stableId());
  scene->removeObjects(gone);

  std::vector<std::shared_ptr<eng::GameObject>> add;
  add.reserve(m_pieces.size());
  for (const auto &piece: m_pieces)
    if (!scene->findById(piece->stableId()))
      add.push_back(piece);
  if (!add.empty())
    scene->addObjects(add);

  m_engine->notifyStructureChanged();
  m_engine->setSelected(m_newSelection, {});

  qInfo(undoLog, "[undo] cut re-applied: %zu brush(es) -> %zu piece(es)", m_removed.size(),
        m_pieces.size());
}

// Pieces out first, then the originals back in ascending slot order, which reproduces the original layout.
void CutCommand::undo() {
  auto *scene = m_engine->scene().get();
  if (scene == nullptr) {
    qWarning(undoLog, "[undo] cannot undo cut: no scene");
    return;
  }

  std::set<quint32> pieces;
  for (const auto &piece: m_pieces)
    pieces.insert(piece->stableId());
  if (!pieces.empty())
    scene->removeObjects(pieces);

  std::vector<size_t> order(m_removed.size());
  for (size_t k = 0; k < order.size(); ++k)
    order[k] = k;
  std::sort(order.begin(), order.end(),
            [this](const size_t a, const size_t b) { return m_slots[a] < m_slots[b]; });

  std::vector<std::shared_ptr<eng::GameObject>> objects;
  std::vector<int> slotList;
  objects.reserve(m_removed.size());
  slotList.reserve(m_removed.size());
  for (const size_t k: order) {
    objects.push_back(m_removed[k]);
    slotList.push_back(m_slots[k]);
  }
  if (!objects.empty())
    scene->insertObjects(objects, slotList);

  m_engine->notifyStructureChanged();
  m_engine->setSelected(m_prevObjects, m_prevFaces);

  qInfo(undoLog, "[undo] cut undone: %zu piece(s) out, %zu brush(es) restored",
        pieces.size(), objects.size());
}

} // namespace vk
