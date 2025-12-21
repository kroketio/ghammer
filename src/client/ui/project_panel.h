#pragma once

#include <functional>

#include <QWidget>

class QFileSystemModel;
class QLabel;
class QModelIndex;
class QTreeView;

namespace proj {
class DocumentManager;
class Project;
}

namespace ui {

class ProjectFilterModel;

// The open project's folder as a tree; the first row is the project itself.
class ProjectPanel final : public QWidget {
  Q_OBJECT

public:
  explicit ProjectPanel(proj::DocumentManager *docs, QWidget *parent = nullptr);

  void setProject(proj::Project *project);
  void reveal(const QString &path);
  void focusTree();

  // Absolute paths of every row shown.
  [[nodiscard]] QStringList listedPaths() const;

  // Relative to the project ("." = the project row); kept in workspace.json.
  [[nodiscard]] QStringList expandedFolders() const;
  void setExpandedFolders(const QStringList &folders);

  // Before files under `path` are deleted / renamed; false cancels.
  std::function<bool(const QString &path)> closeDocumentsUnder;

signals:
  void openRequested(const QString &path);
  void openExternallyRequested(const QString &path);
  void newLevelRequested(const QString &folder);
  void referenceRequested(const QString &path); // "As Reference Image"
  void createGodotProjectRequested(const QString &folder);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void showContextMenu(const QPoint &pos);
  [[nodiscard]] QString pathAt(const QModelIndex &proxyIndex) const;
  void renameAt(const QModelIndex &proxyIndex);
  void deletePath(const QString &path);
  void newFolderIn(const QString &dir);
  // Clipboard into `dir`: files / folders, or image data.
  [[nodiscard]] static bool canPaste();
  void pasteInto(const QString &dir);
  [[nodiscard]] QString absolute(const QString &rel) const;

  proj::DocumentManager *m_docs = nullptr;
  proj::Project *m_project = nullptr;
  QFileSystemModel *m_fs = nullptr;
  ProjectFilterModel *m_filter = nullptr;
  QTreeView *m_tree = nullptr;
  QLabel *m_empty = nullptr;
  QStringList m_pendingExpanded;
};

} // namespace ui
