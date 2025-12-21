#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace vk {
class Engine3D;
}

namespace ui {

// The face groups of the selected faces.
class FaceGroupsPanel final : public QWidget {
  Q_OBJECT

public:
  explicit FaceGroupsPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();
  void addToGroup();
  void rowMenu(quint32 id, bool builtin, const QPoint &globalPos);

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QWidget *m_rows;
  QVBoxLayout *m_rowsLayout;
  QLabel *m_empty;
  QComboBox *m_name;
  QPushButton *m_add;
};

} // namespace ui
