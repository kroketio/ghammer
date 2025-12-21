#pragma once

#include <QWidget>

class QTreeWidget;
class QLineEdit;
class QLabel;

namespace vk {
class Engine3D;
}

namespace ui {

// Entity classes by category; double-click or drag into the 3D view to place one.
class EntityBrowser final : public QWidget {
  Q_OBJECT

public:
  explicit EntityBrowser(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void rebuild();

  vk::Engine3D *m_engine;
  QTreeWidget *m_tree;
  QLineEdit *m_search;
  QLabel *m_info;
};

} // namespace ui
