#pragma once

#include <QWidget>

class QListWidget;

namespace vk {
class Engine3D;
}

namespace ui {

// The undo stack as a list; clicking a row undoes / redoes to just after that step.
class HistoryPanel final : public QWidget {
  Q_OBJECT

public:
  explicit HistoryPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void rebuild();

  vk::Engine3D *m_engine;
  QListWidget *m_list;
  bool m_jumping = false;
};

} // namespace ui
