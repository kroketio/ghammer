#pragma once

#include <QHash>
#include <QPointer>
#include <QWidget>

#include "shared/models/material_props.h"

class QLabel;
class QComboBox;
class QPushButton;
class QToolButton;
class QLineEdit;
class QCheckBox;

namespace vk {
class Engine3D;
}

namespace ui {

class ResetDot;

class Section;
class SliderField;
class ColorButton;

// A texture's Godot material properties, in Godot's eight sections; no scroll area of its own.
// Selection mode (inspector) or Pinned (the Texture Browser's details). Each row's dot shows the layer
// its value comes from and resets it. Edits go to the variant's layer ("pack/Name#shiny"), else the map layer.
class MaterialPropsPanel final : public QWidget {
  Q_OBJECT

public:
  enum class Mode { Selection, Pinned };
  MaterialPropsPanel(vk::Engine3D *engine, Mode mode, QWidget *parent = nullptr);

  // Selection mode: one of the selection's materials, until the selection changes.
  void showMaterial(const QString &ref);
  [[nodiscard]] QString shownRef() const { return m_ref; }

private:
  struct Row {
    matprops::Key key;
    QWidget *editor = nullptr;
    SliderField *slider = nullptr;
    QCheckBox *check = nullptr;
    QComboBox *combo = nullptr;
    ColorButton *color = nullptr;
    ResetDot *dot = nullptr;
    QLabel *label = nullptr;
  };

  void scheduleRefresh();
  void refresh();
  void refreshHeader();
  void buildRow(Section *section, matprops::Key key);
  void commit(matprops::Key key, const QVariant &value);
  void saveToServer(bool force);
  void startVariant();
  [[nodiscard]] QString activeRef() const;

  QPointer<vk::Engine3D> m_engine;
  Mode m_mode;
  QString m_pinnedRef;    // shown explicitly
  QString m_ref;          // shown now
  QString m_expandedFor;  // the ref the sub-sections were last opened for

  QLabel *m_title = nullptr;
  QLabel *m_scope = nullptr;
  QComboBox *m_picker = nullptr;
  QLabel *m_empty = nullptr;
  QWidget *m_body = nullptr;
  QHash<int, Row> m_rows;
  QHash<int, Section *> m_sections;
  QPushButton *m_save = nullptr;
  QPushButton *m_revert = nullptr;
  QPushButton *m_variant = nullptr;
  QPushButton *m_merge = nullptr;
  QPushButton *m_deleteVariant = nullptr;
  QPushButton *m_copy = nullptr;     // base-pack textures
  QPushButton *m_editMap = nullptr;  // user-pack textures
  QWidget *m_variantForm = nullptr;
  QLineEdit *m_variantName = nullptr;
  QLabel *m_status = nullptr;
  QTimer *m_refreshTimer = nullptr;
};

} // namespace ui
