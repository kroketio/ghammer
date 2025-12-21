#pragma once

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QWidget>

class QListView;
class QLineEdit;
class QComboBox;
class QToolButton;
class QSlider;
class QLabel;
class QStackedWidget;
class QButtonGroup;
class QHBoxLayout;
class QTimer;

namespace vk {
class Engine3D;
}

namespace ui {

class ServerTextureModel;
class MapMaterialsModel;
class TextureCardDelegate;

// Textures from the local texture library folder.
class LocalLibraryModel final : public QAbstractListModel {
  Q_OBJECT

public:
  explicit LocalLibraryModel(QObject *parent = nullptr);
  void rebuild();
  [[nodiscard]] QStringList categories() const;

  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  QStringList mimeTypes() const override;
  QMimeData *mimeData(const QModelIndexList &indexes) const override;

  static constexpr int CategoryRole = Qt::UserRole + 200;

private:
  struct Item {
    QString name, display, category, path;
    QSize size;
  };
  QVector<Item> m_items;
};

class LocalFilterModel final : public QSortFilterProxyModel {
  Q_OBJECT

public:
  using QSortFilterProxyModel::QSortFilterProxyModel;
  void setText(const QString &t) {
    m_text = t;
    invalidateFilter();
  }
  void setCategory(const QString &c) {
    m_category = c;
    invalidateFilter();
  }

protected:
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
  QString m_text, m_category;
};

// The Materials dock: Server (paged while scrolling, never all of it), This map, Local.
class MaterialBrowser final : public QWidget {
  Q_OBJECT

public:
  explicit MaterialBrowser(vk::Engine3D *engine, QWidget *parent = nullptr);

  void reveal(const QString &materialName);

signals:
  void materialActivated(const QString &name);
  void openBrowserRequested();

private:
  enum Tab { ServerTab = 0, MapTab = 1, LocalTab = 2 };
  void setTab(int tab);
  void applyServerQuery();
  void refreshUsed();
  void refreshChips();
  void refreshServerState();
  void showContextMenu(QListView *view, const QPoint &pos);
  void updateCount();
  QListView *makeView(QAbstractItemModel *model);

  vk::Engine3D *m_engine;
  int m_tab = ServerTab;
  QButtonGroup *m_tabs = nullptr;
  QLineEdit *m_search = nullptr;
  QComboBox *m_pack = nullptr;
  // Server tab order: shuffled or by name; starts as Textures/RandomOrder says.
  class ShuffleButton *m_shuffle = nullptr;
  QComboBox *m_category = nullptr;
  QWidget *m_chipRow = nullptr;
  QHBoxLayout *m_chipLayout = nullptr;
  QStringList m_activeTags;
  QStackedWidget *m_stack = nullptr;
  QStackedWidget *m_serverStack = nullptr;  // list / offline message
  QLabel *m_offline = nullptr;
  ServerTextureModel *m_serverModel = nullptr;
  MapMaterialsModel *m_mapModel = nullptr;
  LocalLibraryModel *m_localModel = nullptr;
  LocalFilterModel *m_localProxy = nullptr;
  TextureCardDelegate *m_delegate = nullptr;
  QListView *m_serverView = nullptr;
  QListView *m_mapView = nullptr;
  QListView *m_localView = nullptr;
  QToolButton *m_refreshPill = nullptr;
  QSlider *m_size = nullptr;
  QLabel *m_current = nullptr;
  QLabel *m_count = nullptr;
  QTimer *m_searchDebounce = nullptr;
};

} // namespace ui
