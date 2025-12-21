#pragma once

#include <QWidget>

#include <array>

#include "client/engine/stair_gen.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;
class ResetDot;
class Section;

// Material mode, faces on one stairs object: one texture placement per stair part, centred per face in
// its own frame; kept through rebuilds.
class StairsTexturePanel final : public QWidget {
  Q_OBJECT

public:
  explicit StairsTexturePanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  // False when the selection is not on a stairs object.
  bool refresh();

private:
  struct Block {
    Section *section = nullptr;
    QLabel *status = nullptr;
    QPushButton *fit = nullptr, *rotate = nullptr, *select = nullptr, *release = nullptr;
    NumberField *offU = nullptr, *offV = nullptr, *tile = nullptr;
    ResetDot *dotOffU = nullptr, *dotOffV = nullptr, *dotTile = nullptr;
    QWidget *fields = nullptr;
  };
  void buildBlock(eng::stairs::Part part, const QString &title);
  void edit(eng::stairs::Part part, const std::function<void(eng::stairs::StepTex &)> &fn);
  [[nodiscard]] double defaultTile(const eng::stairs::StepTex &t) const;

  vk::Engine3D *m_engine;
  quint32 m_id = 0;
  int m_lastParts = -1;
  quint32 m_lastId = 0;
  QLabel *m_title;
  QVBoxLayout *m_blocks;
  std::array<Block, size_t(eng::stairs::Part::Count)> m_block {};
};

} // namespace ui
