#pragma once

#include <QMainWindow>
#include <QPointer>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QComboBox;
class QSlider;
class QToolButton;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QSplitter;
class QTimer;
class QScrollArea;
class QStackedWidget;

namespace vk {
class Engine3D;
}

namespace ui {

class ServerTextureModel;
class TextureCardDelegate;
class FlowLayout;
class MaterialPropsPanel;
class MaterialPreview3D;
class TagPicker;

// The full texture browser, a top-level window. Pages come from the server; other editors' changes
// update cards in place, or show a "N changes · Refresh" pill when the result may have changed.
class TextureBrowserWindow final : public QMainWindow {
  Q_OBJECT

public:
  static TextureBrowserWindow *open(vk::Engine3D *engine, QWidget *owner, const QString &revealRef = {});
  static TextureBrowserWindow *openSkyboxes(vk::Engine3D *engine, QWidget *owner);
  static void closeIfOpen();

protected:
  void closeEvent(QCloseEvent *e) override;
  bool eventFilter(QObject *obj, QEvent *e) override;

private:
  TextureBrowserWindow(vk::Engine3D *engine, QWidget *owner);
  QWidget *buildSidebar();
  QWidget *buildCenter();
  QWidget *buildDetails();
  QWidget *buildPreviewSwitch();

  void applyQuery();
  void rebuildFilterChips();
  void rebuildPacks();
  void updatePopularFilter();
  void rebuildLicenses();
  // `requery` runs the query afterwards.
  void clearFilters(bool requery);
  void rebuildPager();
  void showDetails(const QString &ref);
  void refreshDetails();
  void layoutPreview();
  void reveal(const QString &ref);
  [[nodiscard]] QString selectedRef() const;
  [[nodiscard]] QStringList mapRefs() const;
  [[nodiscard]] static QStringList recentRefs();
  void saveWindowState();
  void syncTintButton();

  // Sky: only skyboxes; the others leave them out.
  enum class Library { All, Map, Recent, Sky };
  enum class PreviewMode { Flat = 0, Cube, Sphere, Plane };
  void setPreviewMode(PreviewMode mode);

  QPointer<vk::Engine3D> m_engine;
  Library m_library = Library::All;
  QString m_pack;
  QStringList m_licenses; // license keys to show (any of them); empty = all
  QString m_revealPending;

  // sidebar
  QListWidget *m_libraryList = nullptr;
  QCheckBox *m_alphaOnly = nullptr;
  QCheckBox *m_freeOnly = nullptr; // the server decides what is free
  QListWidget *m_licenseList = nullptr;
  quint32 m_seed = 0;
  QPushButton *m_tintButton = nullptr;
  QListWidget *m_packList = nullptr;
  TagPicker *m_tagPicker = nullptr;
  // center
  QLineEdit *m_search = nullptr;
  QComboBox *m_sort = nullptr;
  QSlider *m_thumbSize = nullptr;
  QWidget *m_chipBox = nullptr;
  FlowLayout *m_chipFlow = nullptr;
  QLabel *m_resultCount = nullptr;
  QToolButton *m_refreshPill = nullptr;
  QListView *m_grid = nullptr;
  QLabel *m_gridMessage = nullptr;
  ServerTextureModel *m_model = nullptr;
  TextureCardDelegate *m_delegate = nullptr;
  QHBoxLayout *m_pagerLayout = nullptr;
  QComboBox *m_pageSize = nullptr;
  // details
  QString m_detailsRef;
  QStackedWidget *m_previewStack = nullptr;
  QLabel *m_preview = nullptr;
  MaterialPreview3D *m_preview3d = nullptr;
  PreviewMode m_previewMode = PreviewMode::Flat;
  QButtonGroup *m_previewModes = nullptr;
  QWidget *m_previewLights = nullptr;  // Sun / Omni, only in 3D
  QWidget *m_previewTiling = nullptr;  // Single / Repeated, only in 2D
  QWidget *m_previewSwitch = nullptr;  // hidden for a skybox
  bool m_previewRepeated = false;      // 2D: 2 x 2
  QLabel *m_detailsTitle = nullptr;
  QLabel *m_detailsRef_ = nullptr;
  QLabel *m_detailsLock = nullptr;
  QWidget *m_mapChips = nullptr;
  FlowLayout *m_mapFlow = nullptr;
  QLabel *m_detailsInfo = nullptr;
  QWidget *m_detailTags = nullptr;
  FlowLayout *m_detailTagFlow = nullptr;
  MaterialPropsPanel *m_detailsProps = nullptr;
  QPushButton *m_applyButton = nullptr;
  QPushButton *m_currentButton = nullptr;
  QPushButton *m_copyButton = nullptr;
  QPushButton *m_editButton = nullptr;
  QWidget *m_detailsBody = nullptr;
  QLabel *m_detailsEmpty = nullptr;

  QSplitter *m_split = nullptr;
  QTimer *m_searchDebounce = nullptr;
};

} // namespace ui
