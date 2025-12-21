#pragma once

#include <vector>

#include <functional>

#include <QPointer>
#include <QWidget>

class QLabel;
class QToolButton;
class QCheckBox;
class QComboBox;
class QPushButton;
class QTimer;
class QScrollArea;

namespace vk {
class Engine3D;
}

namespace ui {

class Section;
class MaterialPropsPanel;
class NumberField;
class ResetDot;
class SliderField;
class ColorButton;
class AxisRow;
class LutWindow;

// The properties panel for the selection. Every field writes through an undoable engine call and
// re-reads the engine on change: the panel never holds a second copy of the state.
class Inspector final : public QWidget {
  Q_OBJECT

public:
  explicit Inspector(vk::Engine3D *engine, QWidget *parent = nullptr);

  // Opens the "Texture" section, showing `ref` if it is one of the selection's materials.
  void revealTextureProperties(const QString &ref = {});
  [[nodiscard]] bool showsTextureProperties() const;

signals:
  void browseMaterialRequested(const QString &current);

private:
  void buildHeader();
  void buildTransform();
  void buildTexture();
  void buildWorld();
  void scheduleRefresh();
  void refresh();
  void refreshTransform();
  void refreshTexture();
  void refreshWorld();

  vk::Engine3D *m_engine;
  QTimer *m_refreshTimer;
  QWidget *m_body = nullptr;

  // header
  QLabel *m_icon = nullptr;
  QLabel *m_title = nullptr;
  QLabel *m_subtitle = nullptr;

  // transform
  Section *m_transform = nullptr;
  AxisRow *m_pos = nullptr;
  AxisRow *m_rotation = nullptr;
  AxisRow *m_size = nullptr;
  QCheckBox *m_smoothShading = nullptr;
  QWidget *m_physApplyRow = nullptr; // Apply Simulated Transform

  // texture
  Section *m_texture = nullptr;
  Section *m_texProps = nullptr;
  MaterialPropsPanel *m_texPropsPanel = nullptr;
  QScrollArea *m_scroll = nullptr;
  QLabel *m_matThumb = nullptr;
  QLabel *m_matName = nullptr;
  NumberField *m_scaleU = nullptr;
  NumberField *m_scaleV = nullptr;
  NumberField *m_offU = nullptr;
  NumberField *m_offV = nullptr;
  NumberField *m_rot = nullptr;
  ResetDot *m_dotScaleU = nullptr, *m_dotScaleV = nullptr, *m_dotOffU = nullptr, *m_dotOffV = nullptr,
           *m_dotRot = nullptr;
  QCheckBox *m_texLock = nullptr;
  QCheckBox *m_scaleLock = nullptr;

  Section *m_cut = nullptr;
  Section *m_bevel = nullptr;
  Section *m_mirror = nullptr;
  Section *m_modifiers = nullptr;
  // Stairs / arch first, then Modifiers above Transform when the selection has modifiers, else below;
  // Material mode: Modifiers below Positioning.
  void arrangeSections(bool materialMode, bool hasModifiers);
  Section *m_pathSection = nullptr;
  class PathPanel *m_pathPanel = nullptr;
  Section *m_stairs = nullptr;
  class StairsPanel *m_stairsPanel = nullptr;
  Section *m_stairsTex = nullptr;
  class StairsTexturePanel *m_stairsTexPanel = nullptr;
  Section *m_arch = nullptr;
  class ArchPanel *m_archPanel = nullptr;
  Section *m_shape = nullptr;
  // World Environment rows' reset dots: amber while the value is off the new-level default.
  struct WorldDot {
    ResetDot *dot = nullptr;
    std::function<bool()> changed;
  };
  std::vector<WorldDot> m_worldDots;
  std::vector<QWidget *> m_sunRows;  // editor + dot, read-only while a sun entity drives the sun
  class ShapePanel *m_shapePanel = nullptr;
  Section *m_faceGroups = nullptr;
  QWidget *m_replaceRow = nullptr;
  Section *m_entity = nullptr;
  class EntityEditor *m_entityEditor = nullptr;
  Section *m_io = nullptr;
  class IOSummary *m_ioSummary = nullptr;

  // world
  Section *m_sun = nullptr;
  Section *m_ambient = nullptr;
  Section *m_shadows = nullptr;
  Section *m_post = nullptr;
  QCheckBox *m_sunOn = nullptr;
  SliderField *m_sunAz = nullptr;
  SliderField *m_sunEl = nullptr;
  SliderField *m_sunIntensity = nullptr;
  ColorButton *m_sunColor = nullptr;
  ColorButton *m_ambColor = nullptr;
  SliderField *m_ambIntensity = nullptr;
  ColorButton *m_fogColor = nullptr;
  QLabel *m_skyName = nullptr;
  QToolButton *m_skyClear = nullptr;
  SliderField *m_fogDensity = nullptr;
  SliderField *m_exposure = nullptr;
  QLabel *m_lutName = nullptr;
  QToolButton *m_lutShow = nullptr, *m_lutClear = nullptr;
  QWidget *m_nothingHint = nullptr;
  QPointer<LutWindow> m_lutWindow;
  SliderField *m_shStrength = nullptr;
  SliderField *m_shBias = nullptr;
  SliderField *m_shNormalBias = nullptr;
  SliderField *m_shSoftness = nullptr;
  QCheckBox *m_ssaoOn = nullptr;
  SliderField *m_ssaoRadius = nullptr;
  SliderField *m_ssaoIntensity = nullptr;
  QComboBox *m_renderMode = nullptr;

  QString m_lastThumbFor;
};

} // namespace ui
