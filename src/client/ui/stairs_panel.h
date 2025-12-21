#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;

// Inspector section for a selected stairs object: every change rebuilds it.
class StairsPanel final : public QWidget {
  Q_OBJECT

public:
  explicit StairsPanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  void refresh();

private:
  void set(const char *key, double value);
  void setInt(const char *key, int value);

  vk::Engine3D *m_engine;
  quint32 m_id = 0;
  QLineEdit *m_name;
  QToolButton *m_bySize, *m_byCount;
  QWidget *m_sizeRow, *m_countRow, *m_thicknessRow;
  NumberField *m_height, *m_depth, *m_steps, *m_thickness, *m_nosingDepth, *m_nosingHeight;
  QCheckBox *m_nosing, *m_thinPlanks, *m_collision, *m_sides;
  QWidget *m_sidesRow;
  NumberField *m_sideWidth, *m_sideHeight;
  QWidget *m_nosingRow, *m_plankRow;
  NumberField *m_plankThickness;
  QPushButton *m_fit;
  QComboBox *m_direction;
  QToolButton *m_underside[3];
  QLabel *m_info;
  QPushButton *m_dissolve;
};

} // namespace ui
