#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QToolButton;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;

// Inspector section for cut mode: shape (slice, door, window, vent) and opening sizes.
class CutPanel final : public QWidget {
  Q_OBJECT

public:
  explicit CutPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QToolButton *m_mode[3];
  QToolButton *m_shape[4];
  QWidget *m_modeRow;
  QComboBox *m_template;
  NumberField *m_width, *m_height, *m_sill, *m_gap, *m_margin;
  QCheckBox *m_autoFill;
  QLabel *m_sillInfo;
  QPushButton *m_apply, *m_cancel;
};

} // namespace ui
