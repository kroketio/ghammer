#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;

// Inspector section of the bevel tool (vk_bevel.cpp).
class BevelPanel final : public QWidget {
  Q_OBJECT

public:
  explicit BevelPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  NumberField *m_radius, *m_segments;
  QCheckBox *m_inverse, *m_smooth;
  QComboBox *m_newFaces;
  QLabel *m_limit, *m_status;
  QPushButton *m_go, *m_cancel, *m_clear;
};

} // namespace ui
