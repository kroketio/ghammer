#pragma once

#include <QJsonValue>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;
class SliderField;

// Inspector section for cylinders, capsules and pyramids: the shape tool's preview settings, or a selected generated
// shape's (each change rebuilds it, one undo step).
class ShapePanel final : public QWidget {
  Q_OBJECT

public:
  explicit ShapePanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  void refresh();
  // The section title for what the panel shows ("Cylinder", ...), and its icon.
  [[nodiscard]] QString title() const;
  [[nodiscard]] QString icon() const;
  // The shape tool is open, or a generated shape is selected.
  [[nodiscard]] static bool relevant(const vk::Engine3D *engine);

private:
  [[nodiscard]] quint32 editedShape() const;
  void change(const char *key, const QJsonValue &value);

  vk::Engine3D *m_engine;
  QLabel *m_info;
  QToolButton *m_axis[3];
  QLabel *m_axisLabel;
  SliderField *m_sides;
  QLabel *m_wallLabel;
  NumberField *m_wall;
  QLabel *m_capsLabel;
  SliderField *m_caps;
  QCheckBox *m_smooth;
  QPushButton *m_create;
  QWidget *m_toolButtons;
  QPushButton *m_dissolve;
  int m_kind = 0;  // eng::ShapeKind shown
};

} // namespace ui
