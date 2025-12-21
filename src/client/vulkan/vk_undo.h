#pragma once

#include <memory>
#include <set>
#include <vector>

#include <QSharedPointer>
#include <QString>
#include <QVector3D>
#include <glm/glm.hpp>

#include "engine/map_format.h"
#include "engine/gameobject.h"
#include "engine/mesh.h"
#include "engine/undo_stack.h"

namespace vk {

class Engine3D;

// Everything editable on one placed object. An empty face array = aspect untouched, so a move neither rewrites
// material state nor forces a per-face re-upload on undo.
struct ObjectState {
  quint32 id = 0;
  glm::vec3 position {0.0f};
  glm::vec3 rotation {0.0f};
  glm::vec3 scale {1.0f};
  std::vector<eng::FaceTexBasis> faceBases;
  std::vector<QSharedPointer<eng::Material>> faceMaterials;
  std::vector<eng::FaceGroupSet> faceGroups;
};

// A settled transform edit of brushes: before / after, plus both per-face mappings when texture lock rewrote them.
class ObjectStatesCommand final : public eng::UndoCommand {
public:
  ObjectStatesCommand(Engine3D *engine, QString label, std::vector<ObjectState> before,
                     std::vector<ObjectState> after);

  void undo() override;
  void redo() override;
  void touched(eng::Touched &t) const override;

  [[nodiscard]] QString text() const override { return m_label; }

private:
  Engine3D *m_engine;
  QString m_label;
  std::vector<ObjectState> m_before;
  std::vector<ObjectState> m_after;
};

// A delete: the brushes with their slots and the prior selection. (Entities: Engine3D::deleteEntities.)
class DeleteCommand final : public eng::UndoCommand {
public:
  explicit DeleteCommand(Engine3D *engine);

  void addBrush(const std::shared_ptr<eng::GameObject> &object, int slot);
  void rememberSelection(const std::vector<quint32> &objects, const std::vector<quint32> &faces);

  [[nodiscard]] bool empty() const { return m_objects.empty(); }
  [[nodiscard]] size_t brushCount() const { return m_objects.size(); }

  void undo() override;
  void redo() override;
  void touched(eng::Touched &t) const override { t.objects.insert(t.objects.end(), m_objects.begin(), m_objects.end()); }

  [[nodiscard]] QString text() const override;

private:
  Engine3D *m_engine;
  std::vector<std::shared_ptr<eng::GameObject>> m_objects;
  std::vector<int> m_slots;
  std::vector<quint32> m_prevObjects;
  std::vector<quint32> m_prevFaces;
};

// A shift-drag duplicate: the spawned brushes and the prior selection. The clones are already live when pushed
// (the drag drew them), so pushing records the step without re-running it.
class DuplicateCommand final : public eng::UndoCommand {
public:
  DuplicateCommand(Engine3D *engine, QString label);

  void addClone(const std::shared_ptr<eng::GameObject> &object);
  void rememberSelection(const std::vector<quint32> &objects, const std::vector<quint32> &faces);

  void undo() override;
  void redo() override;

  [[nodiscard]] QString text() const override { return m_label; }

private:
  Engine3D *m_engine;
  QString m_label;
  std::vector<std::shared_ptr<eng::GameObject>> m_objects;
  std::vector<quint32> m_prevObjects;
  std::vector<quint32> m_prevFaces;
};

// A cut: the brushes taken out (with their slots), the pieces put in, and the selection before / after.
class CutCommand final : public eng::UndoCommand {
public:
  CutCommand(Engine3D *engine, QString label);

  void addRemoved(const std::shared_ptr<eng::GameObject> &object, int slot);
  void addPiece(const std::shared_ptr<eng::GameObject> &object);
  void rememberSelection(const std::vector<quint32> &objects, const std::vector<quint32> &faces);
  void setNewSelection(const std::vector<quint32> &objects) { m_newSelection = objects; }

  void undo() override;
  void redo() override;
  void touched(eng::Touched &t) const override {
    t.objects.insert(t.objects.end(), m_removed.begin(), m_removed.end());
    t.objects.insert(t.objects.end(), m_pieces.begin(), m_pieces.end());
  }

  [[nodiscard]] QString text() const override { return m_label; }

private:
  Engine3D *m_engine;
  QString m_label;
  std::vector<std::shared_ptr<eng::GameObject>> m_removed;
  std::vector<int> m_slots;
  std::vector<std::shared_ptr<eng::GameObject>> m_pieces;
  std::vector<quint32> m_newSelection;
  std::vector<quint32> m_prevObjects;
  std::vector<quint32> m_prevFaces;
};

} // namespace vk
