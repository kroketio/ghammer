#pragma once

#include <QDialog>
#include <QStringList>

class QComboBox;
class QFileSystemWatcher;
class QLineEdit;
class QListView;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeView;
class QVBoxLayout;

namespace ui {

class FileListModel;

// The editor's own file browser (after Blender's File View), used instead of QFileDialog
// everywhere, with the same static calls. State is kept in config ("FileDialog/…").
class FileDialog final : public QDialog {
  Q_OBJECT

public:
  enum class Mode { Open, OpenMany, Save, Directory };

  // `dir`: a folder, a file (selected in its folder) or a bare file name (in the last folder used).
  // `filter` is Qt's "Label (*.a *.b);;…". Empty `acceptLabel` = Open / Save / Select Folder.
  static QString getOpenFileName(QWidget *parent, const QString &caption, const QString &dir = {},
                                 const QString &filter = {}, const QString &acceptLabel = {});
  static QStringList getOpenFileNames(QWidget *parent, const QString &caption, const QString &dir = {},
                                      const QString &filter = {}, const QString &acceptLabel = {});
  static QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir = {},
                                 const QString &filter = {}, const QString &acceptLabel = {});
  static QString getExistingDirectory(QWidget *parent, const QString &caption, const QString &dir = {},
                                      const QString &acceptLabel = {});

  FileDialog(QWidget *parent, Mode mode, const QString &caption, const QString &dir, const QString &filter,
             const QString &acceptLabel);
  ~FileDialog() override;

  [[nodiscard]] QStringList selectedPaths() const { return m_result; }

protected:
  bool eventFilter(QObject *o, QEvent *e) override;
  void showEvent(QShowEvent *event) override;
  void keyPressEvent(QKeyEvent *e) override;
  void done(int r) override;

private:
  struct Filter {
    QString label;
    QStringList patterns;  // empty = every file
  };

  void buildUi(const QString &acceptLabel);
  QWidget *buildSidebar();
  QWidget *addSection(QVBoxLayout *into, const QString &key, const QString &title, QWidget *body);
  void fillPlaces();
  void fillRecent();
  void fillProjects();
  // Fits each sidebar list to its rows; call once shown too (the stylesheet's row height).
  void refitPlaces();
  void fillBookmarks();
  void litPlaces();

  void navigate(const QString &dir, bool record = true);
  void refresh();
  void goBack();
  void goForward();
  void goUp();
  void newFolder();
  void applyFilter();
  void setThumbnails(bool on);
  void selectName(const QString &name);
  void activate(int row);
  void selectionToName();
  void bumpNumber(int delta);
  void updateState();
  void tryAccept();
  void showListMenu(const QPoint &pos, QWidget *view);
  [[nodiscard]] QString resolve(const QString &name) const;
  [[nodiscard]] QList<int> selectedRows() const;
  [[nodiscard]] const Filter &activeFilter() const;

  void addBookmark(const QString &dir);
  static void addRecent(const QString &dir);

  Mode m_mode;
  QList<Filter> m_filters;
  int m_filterIndex = 0;
  QString m_dir;
  QStringList m_back, m_forward;
  QStringList m_result;

  FileListModel *m_model = nullptr;
  QFileSystemWatcher *m_watcher = nullptr;
  QTimer *m_refreshTimer = nullptr;

  QSplitter *m_split = nullptr;
  QWidget *m_side = nullptr;
  QListWidget *m_projects = nullptr;
  QListWidget *m_bookmarks = nullptr;
  QListWidget *m_system = nullptr;
  QListWidget *m_volumes = nullptr;
  QListWidget *m_recent = nullptr;

  QToolButton *m_backBtn = nullptr;
  QToolButton *m_forwardBtn = nullptr;
  QToolButton *m_upBtn = nullptr;
  QToolButton *m_listBtn = nullptr;
  QToolButton *m_thumbBtn = nullptr;
  QToolButton *m_filterBtn = nullptr;
  QLineEdit *m_path = nullptr;
  QLineEdit *m_search = nullptr;
  QStackedWidget *m_views = nullptr;
  QTreeView *m_tree = nullptr;
  QListView *m_thumbs = nullptr;
  QLineEdit *m_name = nullptr;
  QComboBox *m_type = nullptr;
  QPushButton *m_accept = nullptr;
};

} // namespace ui
