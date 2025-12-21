#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace vk {
class Engine3D;
}

namespace ui {

class AxisRow;
class NumberField;
class SliderField;

// Inspector › Path: the single selected path (drawn or Connect).
class PathPanel final : public QWidget {
  Q_OBJECT

public:
  explicit PathPanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  void refresh();

private:
  vk::Engine3D *m_engine;
  QTimer *m_timer;
  quint32 m_path = 0;
  QLineEdit *m_name;
  QLabel *m_info;
  QCheckBox *m_closed;
  QWidget *m_drawn, *m_connect, *m_pointBox;
  AxisRow *m_point;
  QComboBox *m_pointType;
  QPushButton *m_reverse, *m_toDrawn;
  QLabel *m_anchors;
  SliderField *m_sharp, *m_position;
  NumberField *m_straight, *m_minRadius, *m_maxSlope;
  QCheckBox *m_straightAuto, *m_radiusAuto;
  QComboBox *m_height;
  QLabel *m_users, *m_error;
};

} // namespace ui
