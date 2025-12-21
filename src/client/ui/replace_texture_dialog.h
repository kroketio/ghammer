#pragma once

#include <QPointer>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QRadioButton;
class QSlider;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace vk {
class Engine3D;
}

namespace ui {

class FlowLayout;
class MaterialPreview3D;
class ServerTextureModel;
class ShuffleButton;
class TagPicker;
class TextureCardDelegate;

// Replace texture window: one at a time, follows the selection. A click only picks; Replace is one undo step.
class ReplaceTextureWindow final : public QWidget {
  Q_OBJECT

public:
  static ReplaceTextureWindow *open(vk::Engine3D *engine, QWidget *owner);

private:
  ReplaceTextureWindow(vk::Engine3D *engine, QWidget *owner);
  QWidget *buildLeft();
  QWidget *buildCenter();
  // The sidebar's "New texture".
  void buildPick(QWidget *side, QVBoxLayout *v);

protected:
  bool eventFilter(QObject *obj, QEvent *e) override;

private:

  [[nodiscard]] QString fromRef() const;
  [[nodiscard]] bool selectionOnly() const;
  void applyQuery();
  void refreshFromList();
  void refreshFrom();
  void refreshPick();
  void layoutPreview();
  void rebuildChips();
  void replaceWith(const QString &ref);

  enum class PreviewMode { Flat = 0, Cube, Sphere };
  void setPreviewMode(PreviewMode mode);

  QPointer<vk::Engine3D> m_engine;
  QString m_pick; // not on any face yet
  PreviewMode m_previewMode = PreviewMode::Flat;

  // left
  QComboBox *m_from = nullptr;
  QLabel *m_fromThumb = nullptr, *m_fromName = nullptr, *m_fromCount = nullptr;
  QRadioButton *m_wholeMap = nullptr, *m_selOnly = nullptr;
  TagPicker *m_tagPicker = nullptr;
  // center
  QLineEdit *m_search = nullptr;
  ShuffleButton *m_shuffle = nullptr;
  QSlider *m_thumbSize = nullptr;
  QWidget *m_chipBox = nullptr;
  FlowLayout *m_chipFlow = nullptr;
  ServerTextureModel *m_model = nullptr;
  TextureCardDelegate *m_delegate = nullptr;
  QListView *m_grid = nullptr;
  // left: the pick
  QStackedWidget *m_previewStack = nullptr;
  QLabel *m_preview = nullptr;
  MaterialPreview3D *m_preview3d = nullptr;
  QButtonGroup *m_previewModes = nullptr;
  QLabel *m_pickName = nullptr, *m_pickRef = nullptr;
  QPushButton *m_replace = nullptr;
  QWidget *m_side = nullptr;  // the preview spans its width

  QTimer *m_searchTimer = nullptr;
  QTimer *m_followTimer = nullptr;
};

} // namespace ui
