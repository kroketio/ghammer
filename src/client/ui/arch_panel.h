#pragma once

#include <QJsonValue>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;
class SliderField;

// Inspector section for arches: the Arch tool's preview settings, or a selected generated arch's (each change
// rebuilds it, one undo step).
class ArchPanel final : public QWidget {
  Q_OBJECT

public:
  explicit ArchPanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  void refresh();

private:
  // A selected generated arch, or 0 (the tool's preview).
  [[nodiscard]] quint32 editedArch() const;
  void setParam(const char *key, const QJsonValue &value);

  vk::Engine3D *m_engine;
  QLabel *m_info;
  QToolButton *m_mode[3];
  QComboBox *m_profile;
  SliderField *m_segments;
  QLabel *m_thicknessLabel;
  NumberField *m_thickness;
  NumberField *m_spring;
  QLabel *m_riseLabel;
  SliderField *m_rise;
  QCheckBox *m_smooth;
  QPushButton *m_create;
  QWidget *m_toolButtons;
  QPushButton *m_dissolve;
};

} // namespace ui
