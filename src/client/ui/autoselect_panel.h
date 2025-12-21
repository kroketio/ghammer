#pragma once

#include <QWidget>

#include <array>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace vk {
class Engine3D;
}

namespace ui {

// Auto-select: pick a kind, and the engine grows it from the picked faces / brushes or a 2D box (faces in
// Material mode, brushes in Object mode). Wall and floor also need Indoor / Outdoor, never pre-chosen.
// The checked state is read back from the engine.
class AutoSelectPanel final : public QWidget {
  Q_OBJECT

public:
  explicit AutoSelectPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();
  void pickIntent(int intent, bool add);
  void pickExposure(int exposure);
  void run(int intent, int exposure, bool add);
  void rerunIfActive();
  // The one waiting for Indoor / Outdoor, else the running result's, else -1.
  [[nodiscard]] int shownIntent() const;
  [[nodiscard]] bool needsExposure(int intent) const; // Wall / Floor, both modes

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QStackedWidget *m_kinds;
  std::array<QToolButton *, 5> m_faceIntents {};  // eng::sel::Intent
  std::array<QToolButton *, 7> m_brushIntents {}; // eng::sel::BrushIntent
  QToolButton *m_sameBrush;                       // Material mode
  QToolButton *m_sameMaterialFaces;               // Material mode
  QToolButton *m_sameMirrorFaces, *m_sameMirror;  // Material / Object page
  QWidget *m_exposureRow, *m_sideRow, *m_groupRow;
  QToolButton *m_indoor, *m_outdoor, *m_thisSide, *m_allSides;
  QComboBox *m_group;
  QCheckBox *m_contiguous, *m_sameMaterial, *m_byObject;
  QPushButton *m_clear;
  QLabel *m_status;
  bool m_material = false; // the mode the panel last showed
  int m_pending = -1;      // a kind waiting for Indoor / Outdoor or a group
  bool m_add = false;
};

// Shown after a Facade selection: which facade face kinds to keep.
class FacadePanel final : public QWidget {
  Q_OBJECT

public:
  explicit FacadePanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  std::array<QCheckBox *, 6> m_types {};
  QPushButton *m_storeys;
};

// Storeys of the selected facade: bands, a material per band, slicing.
class StoreysPanel final : public QWidget {
  Q_OBJECT

public:
  explicit StoreysPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QLabel *m_note;
  QListWidget *m_layers;
  QLineEdit *m_layerMat;
  QCheckBox *m_outline;
  QPushButton *m_set, *m_apply, *m_all, *m_slice, *m_close;
};

// The Auto-select dock: Auto-select, plus Facade and Storeys when they apply.
class AutoSelectDock final : public QWidget {
  Q_OBJECT

public:
  explicit AutoSelectDock(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QWidget *m_facade, *m_storeys;
};

} // namespace ui
