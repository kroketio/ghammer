#include "project_panel.h"

#include "client/ui/external_apps.h"

#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "client/project/document_manager.h"
#include "client/project/project.h"
#include "client/ui/action_manager.h"
#include "client/ui/file_icons.h"
#include "client/ui/scale.h"

namespace ui {

// The model is rooted at the project's parent, so the project is a row of its own. Folders first,
// natural sort, our icons.
class ProjectFilterModel final : public QSortFilterProxyModel {
public:
  explicit ProjectFilterModel(QObject *parent) : QSortFilterProxyModel(parent) {
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setRecursiveFilteringEnabled(false);
    setDynamicSortFilter(true);
  }

  QString root;      // the project folder
  QString parentDir; // its parent (the model's root)
  bool showHidden = false;
  std::function<bool(const QString &)> isOpen;

  void refilter() { invalidateFilter(); }

  [[nodiscard]] QFileSystemModel *fs() const { return static_cast<QFileSystemModel *>(sourceModel()); }

protected:
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
    const QModelIndex idx = sourceModel()->index(row, 0, parent);
    const QString path = fs()->filePath(idx);
    if (root.isEmpty())
      return false;
    // Above the project: only the folders leading to it. The filesystem root ("/", "C:/") already ends in a separator.
    if (path == root)
      return true;
    if (!path.startsWith(root + QLatin1Char('/')))
      return root.startsWith(path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/'));
    if (showHidden)
      return true;
    const QString name = fs()->fileName(idx);
    if (name.startsWith(QLatin1Char('.')))
      return false;
    if (name.endsWith(QLatin1String(".import")) && !fs()->isDir(idx))
      return false;
    return true;
  }

  bool lessThan(const QModelIndex &a, const QModelIndex &b) const override {
    const bool da = fs()->isDir(a), db = fs()->isDir(b);
    if (da != db)
      return da;
    return m_collator.compare(fs()->fileName(a), fs()->fileName(b)) < 0;
  }

  QVariant data(const QModelIndex &index, int role) const override {
    if (role == Qt::DecorationRole && index.column() == 0)
      return iconForFile(fs()->filePath(mapToSource(index)));
    if (role == Qt::FontRole && index.column() == 0) {
      const QString path = fs()->filePath(mapToSource(index));
      if (path == root || (isOpen && isOpen(path))) {
        QFont f = QApplication::font();
        f.setBold(true);
        return f;
      }
    }
    if (role == Qt::ToolTipRole && index.column() == 0)
      return QDir::toNativeSeparators(fs()->filePath(mapToSource(index)));
    return QSortFilterProxyModel::data(index, role);
  }

private:
  QCollator m_collator;
};

namespace {

// The project row: its path in grey after the name.
class ProjectRowDelegate final : public QStyledItemDelegate {
public:
  ProjectRowDelegate(ProjectFilterModel *model, QObject *parent) : QStyledItemDelegate(parent), m_model(model) {}

  void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    QStyledItemDelegate::paint(p, option, index);
    const QString path = m_model->fs()->filePath(m_model->mapToSource(index));
    if (path != m_model->root)
      return;
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *w = opt.widget;
    const QStyle *style = w ? w->style() : QApplication::style();
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w);
    const QFontMetrics bold(opt.font);
    const int x = textRect.left() + bold.horizontalAdvance(opt.text) + scale::px(10);
    const QString where = homeShortPath(path);
    QFont f = option.font;
    f.setBold(false);
    p->save();
    p->setFont(f);
    // Grey is unreadable on the highlight's blue.
    const bool selected = opt.state & QStyle::State_Selected;
    p->setPen(selected ? QColor(255, 255, 255, 205) : QColor(QStringLiteral("#8a95b5")));
    const QRect r(x, textRect.top(), std::max(0, opt.rect.right() - x), textRect.height());
    p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, elidePath(where, QFontMetrics(f), r.width()));
    p->restore();
  }

private:
  ProjectFilterModel *m_model;
};

} // namespace

ProjectPanel::ProjectPanel(proj::DocumentManager *docs, QWidget *parent) : QWidget(parent), m_docs(docs) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  m_fs = new QFileSystemModel(this);
  m_fs->setReadOnly(false);
  m_fs->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  m_fs->setOption(QFileSystemModel::DontUseCustomDirectoryIcons, true);
  m_filter = new ProjectFilterModel(this);
  m_filter->setSourceModel(m_fs);
  m_filter->isOpen = [this](const QString &path) { return m_docs->find(path) != nullptr; };

  m_tree = new QTreeView(this);
  m_tree->setObjectName(QStringLiteral("projectTree"));
  m_tree->setModel(m_filter);
  m_tree->setHeaderHidden(true);
  for (int c = 1; c < m_filter->columnCount(); ++c)
    m_tree->hideColumn(c);
  m_tree->setSortingEnabled(true);
  m_tree->sortByColumn(0, Qt::AscendingOrder);
  m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  m_tree->setUniformRowHeights(true);
  m_tree->setItemDelegate(new ProjectRowDelegate(m_filter, m_tree));
  m_tree->setAnimated(false);
  scale::bind(m_tree, [this] { m_tree->setIconSize(scale::icon(scale::Icon::Small)); });
  v->addWidget(m_tree, 1);

  m_empty = new QLabel(tr("No project open"), this);
  m_empty->setAlignment(Qt::AlignCenter);
  m_empty->setEnabled(false);
  v->addWidget(m_empty, 1);

  connect(m_tree, &QTreeView::activated, this, [this](const QModelIndex &idx) {
    const QString path = pathAt(idx);
    if (path.isEmpty() || QFileInfo(path).isDir())
      return;
    emit openRequested(path);
  });
  connect(m_tree, &QTreeView::customContextMenuRequested, this, &ProjectPanel::showContextMenu);
  connect(m_fs, &QFileSystemModel::fileRenamed, this, [this](const QString &dir, const QString &from, const QString &to) {
    m_docs->renamed(QDir(dir).filePath(from), QDir(dir).filePath(to));
  });
  connect(m_fs, &QFileSystemModel::directoryLoaded, this, [this](const QString &) {
    for (int i = int(m_pendingExpanded.size()) - 1; i >= 0; --i) {
      const QModelIndex src = m_fs->index(absolute(m_pendingExpanded[i]));
      if (!src.isValid())
        continue;
      const QModelIndex idx = m_filter->mapFromSource(src);
      if (idx.isValid()) {
        m_tree->expand(idx);
        m_pendingExpanded.removeAt(i);
      }
    }
  });
  connect(m_docs, &proj::DocumentManager::documentsChanged, m_tree->viewport(), qOverload<>(&QWidget::update));

  // eventFilter claims Delete / Ctrl+V from the window's shortcuts while the tree has focus.
  m_tree->installEventFilter(this);
  setProject(nullptr);
}

void ProjectPanel::setProject(proj::Project *project) {
  m_project = project;
  m_pendingExpanded.clear();
  const bool on = project != nullptr;
  m_tree->setVisible(on);
  m_empty->setVisible(!on);
  if (!on) {
    m_filter->root.clear();
    m_filter->refilter();
    return;
  }
  m_filter->root = QDir::cleanPath(project->root());
  m_filter->parentDir = QFileInfo(m_filter->root).absolutePath();
  const QModelIndex parentSrc = m_fs->setRootPath(m_filter->parentDir);
  m_filter->refilter();
  // Collapsed until setExpandedFolders.
  m_tree->setRootIndex(m_filter->mapFromSource(parentSrc));
  m_tree->collapseAll();
}

QString ProjectPanel::absolute(const QString &rel) const {
  if (!m_project)
    return {};
  return rel == QLatin1String(".") ? m_project->root() : m_project->path(rel);
}

QStringList ProjectPanel::listedPaths() const {
  QStringList out;
  std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
    for (int r = 0; r < m_filter->rowCount(parent); ++r) {
      const QModelIndex idx = m_filter->index(r, 0, parent);
      out << pathAt(idx);
      if (m_tree->isExpanded(idx))
        walk(idx);
    }
  };
  if (m_project)
    walk(m_tree->rootIndex());
  return out;
}

QString ProjectPanel::pathAt(const QModelIndex &proxyIndex) const {
  if (!proxyIndex.isValid())
    return {};
  return QDir::cleanPath(m_fs->filePath(m_filter->mapToSource(proxyIndex)));
}

void ProjectPanel::focusTree() {
  m_tree->setFocus(Qt::ShortcutFocusReason);
  if (!m_tree->currentIndex().isValid() && m_project)
    m_tree->setCurrentIndex(m_filter->mapFromSource(m_fs->index(m_project->root())));
}

void ProjectPanel::reveal(const QString &path) {
  if (!m_project || !m_project->contains(path))
    return;
  // Folders on the way first (they load as expanded).
  QStringList chain;
  for (QString p = QFileInfo(path).absolutePath(); p.size() >= m_project->root().size();
       p = QFileInfo(p).absolutePath()) {
    chain.prepend(m_project->relative(p));
    if (p == m_project->root())
      break;
  }
  for (const QString &rel: chain) {
    const QModelIndex idx = m_filter->mapFromSource(m_fs->index(absolute(rel)));
    if (idx.isValid())
      m_tree->expand(idx);
    else
      m_pendingExpanded << rel;
  }
  const QModelIndex target = m_filter->mapFromSource(m_fs->index(path));
  if (target.isValid()) {
    m_tree->setCurrentIndex(target);
    m_tree->scrollTo(target);
  }
}

QStringList ProjectPanel::expandedFolders() const {
  QStringList out;
  if (!m_project)
    return out;
  std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
    for (int r = 0; r < m_filter->rowCount(parent); ++r) {
      const QModelIndex idx = m_filter->index(r, 0, parent);
      if (!m_tree->isExpanded(idx))
        continue;
      const QString path = pathAt(idx);
      out << (path == m_project->root() ? QStringLiteral(".") : m_project->relative(path));
      walk(idx);
    }
  };
  walk(m_tree->rootIndex());
  // Folders still loading count as expanded.
  for (const QString &rel: m_pendingExpanded)
    if (!out.contains(rel))
      out << rel;
  return out;
}

void ProjectPanel::setExpandedFolders(const QStringList &folders) {
  m_pendingExpanded = folders;
  // The loaded ones expand now, the rest as they load.
  emit m_fs->directoryLoaded(QString());
}

void ProjectPanel::renameAt(const QModelIndex &proxyIndex) {
  const QString path = pathAt(proxyIndex);
  if (path.isEmpty() || !m_project || path == m_project->root())
    return;
  // Open tabs follow the new name (QFileSystemModel::fileRenamed).
  m_tree->edit(proxyIndex);
}

void ProjectPanel::deletePath(const QString &path) {
  const QFileInfo fi(path);
  const QString what = fi.isDir() ? tr("the folder ‘%1’ and everything in it").arg(fi.fileName())
                                  : tr("‘%1’").arg(fi.fileName());
  if (QMessageBox::question(this, tr("Delete"), tr("Delete %1? This cannot be undone.").arg(what),
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
      != QMessageBox::Yes)
    return;
  if (closeDocumentsUnder && !closeDocumentsUnder(path))
    return;
  // A link goes, never what it points to.
  const bool ok = fi.isDir() && !fi.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
  if (!ok)
    QMessageBox::warning(this, tr("Delete"), tr("Could not delete %1.").arg(fi.fileName()));
}

namespace {

// `name`, or "name (2).ext", ... when taken.
QString freeName(const QString &dir, const QString &name) {
  const QFileInfo fi(name);
  const QString base = fi.isDir() || !name.contains(QLatin1Char('.')) ? name : fi.completeBaseName();
  const QString ext = base == name ? QString() : QStringLiteral(".") + fi.suffix();
  QString candidate = name;
  for (int n = 2; QFileInfo::exists(QDir(dir).filePath(candidate)); ++n)
    candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext);
  return QDir(dir).filePath(candidate);
}

bool copyRecursively(const QString &from, const QString &to) {
  const QFileInfo fi(from);
  if (!fi.isDir())
    return QFile::copy(from, to);
  if (!QDir().mkpath(to))
    return false;
  bool ok = true;
  for (const QFileInfo &e: QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
    ok = copyRecursively(e.absoluteFilePath(), QDir(to).filePath(e.fileName())) && ok;
  return ok;
}

} // namespace

bool ProjectPanel::eventFilter(QObject *watched, QEvent *event) {
  if (watched != m_tree || (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress))
    return QWidget::eventFilter(watched, event);
  auto *key = static_cast<QKeyEvent *>(event);
  const bool del = key->matches(QKeySequence::Delete);
  const bool paste = key->matches(QKeySequence::Paste);
  if (!del && !paste)
    return QWidget::eventFilter(watched, event);
  if (event->type() == QEvent::ShortcutOverride) {
    event->accept(); // ours, not the window's shortcut
    return true;
  }
  if (!m_project)
    return true;
  const QString path = pathAt(m_tree->currentIndex());
  if (del) {
    if (!path.isEmpty() && path != m_project->root())
      deletePath(path);
  } else {
    // The selected folder (a file's folder; nothing selected: the project).
    QString dir = path.isEmpty() ? m_project->root() : path;
    if (!QFileInfo(dir).isDir())
      dir = QFileInfo(dir).absolutePath();
    pasteInto(dir);
  }
  return true;
}

bool ProjectPanel::canPaste() {
  const QMimeData *mime = QApplication::clipboard()->mimeData();
  if (!mime)
    return false;
  if (mime->hasUrls())
    for (const QUrl &u: mime->urls())
      if (u.isLocalFile())
        return true;
  return mime->hasImage();
}

void ProjectPanel::pasteInto(const QString &dir) {
  const QMimeData *mime = QApplication::clipboard()->mimeData();
  if (!mime || !QFileInfo(dir).isDir())
    return;
  QString first;
  QStringList failed;
  bool any = false;
  if (mime->hasUrls()) {
    // From a file manager: copied under a free name when taken.
    for (const QUrl &u: mime->urls()) {
      if (!u.isLocalFile())
        continue;
      const QString src = QDir::cleanPath(u.toLocalFile());
      const QFileInfo fi(src);
      if (!fi.exists())
        continue;
      any = true;
      if (fi.isDir() && (QDir::cleanPath(dir) + QLatin1Char('/')).startsWith(src + QLatin1Char('/'))) {
        failed << fi.fileName(); // a folder into itself
        continue;
      }
      const QString dst = freeName(dir, fi.fileName());
      if (!copyRecursively(src, dst))
        failed << fi.fileName();
      else if (first.isEmpty())
        first = dst;
    }
  }
  if (!any && mime->hasImage()) {
    // Image data: saved as a PNG.
    const QImage img = qvariant_cast<QImage>(mime->imageData());
    const QString dst = freeName(dir, QStringLiteral("pasted image.png"));
    if (!img.isNull() && img.save(dst, "PNG"))
      first = dst;
    else
      failed << QFileInfo(dst).fileName();
  }
  if (!failed.isEmpty())
    QMessageBox::warning(this, tr("Paste"), tr("Could not paste %1").arg(failed.join(QStringLiteral(", "))));
  if (!first.isEmpty())
    reveal(first);
}

void ProjectPanel::newFolderIn(const QString &dir) {
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Name"), QLineEdit::Normal, QString(), &ok).trimmed();
  if (!ok || name.isEmpty())
    return;
  if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
    QMessageBox::warning(this, tr("New Folder"), tr("A folder name cannot contain / or \\"));
    return;
  }
  if (!QDir(dir).mkdir(name))
    QMessageBox::warning(this, tr("New Folder"), tr("Could not create %1").arg(name));
  else
    reveal(QDir(dir).filePath(name));
}

void ProjectPanel::showContextMenu(const QPoint &pos) {
  if (!m_project)
    return;
  const QModelIndex idx = m_tree->indexAt(pos);
  const QString path = idx.isValid() ? pathAt(idx) : m_project->root();
  const QFileInfo fi(path);
  const bool dir = fi.isDir();
  const QString folder = dir ? path : fi.absolutePath();
  const bool isRoot = path == m_project->root();

  QMenu menu(this);
  if (!dir) {
    menu.addAction(tr("Open"), this, [this, path] { emit openRequested(path); });
    if (proj::DocumentManager::isImageFile(path))
      menu.addAction(tr("As Reference Image"), this, [this, path] { emit referenceRequested(path); });
    menu.addAction(tr("Open With External Program"), this, [this, path] { emit openExternallyRequested(path); });
    menu.addSeparator();
  }
  QMenu *create = menu.addMenu(ActionManager::icon(QStringLiteral("add")), tr("New"));
  create->addAction(ActionManager::icon(QStringLiteral("file_3d")), tr("Level…"), this,
                    [this, folder] { emit newLevelRequested(folder); });
  create->addAction(ActionManager::icon(QStringLiteral("file_folder")), tr("Folder…"), this,
                    [this, folder] { newFolderIn(folder); });
  if (dir && path == m_project->gameDir() && !QFileInfo::exists(QDir(path).filePath(QStringLiteral("project.godot"))))
    menu.addAction(ActionManager::icon(QStringLiteral("Godot")), tr("Create Godot Project Here"), this,
                   [this, path] { emit createGodotProjectRequested(path); });
  if (dir && QFileInfo::exists(QDir(path).filePath(QStringLiteral("project.godot"))))
    menu.addAction(ActionManager::icon(QStringLiteral("Godot")), tr("Start in Godot"), this,
                   [this, path] { ui::external::startGodot(path, this); });
  menu.addAction(ActionManager::icon(QStringLiteral("pastedown")), tr("Paste"), this,
                 [this, folder] { pasteInto(folder); });
  menu.actions().constLast()->setShortcut(QKeySequence::Paste);
  menu.actions().constLast()->setEnabled(canPaste());
  menu.addSeparator();
  menu.addAction(tr("Show in File Manager"), this, [path, dir] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir ? path : QFileInfo(path).absolutePath()));
  });
  menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
  if (!isRoot)
    menu.addAction(tr("Copy Relative Path"), this, [this, path] {
      QApplication::clipboard()->setText(QDir::toNativeSeparators(m_project->relative(path)));
    });
  if (!isRoot) {
    menu.addSeparator();
    menu.addAction(tr("Rename"), this, [this, idx] { renameAt(idx); })->setShortcut(QKeySequence(Qt::Key_F2));
    menu.addAction(ActionManager::icon(QStringLiteral("trash")), tr("Delete"), this, [this, path] { deletePath(path); })
        ->setShortcut(QKeySequence::Delete);
  }
  menu.addSeparator();
  QAction *hidden = menu.addAction(tr("Show Hidden Files"));
  hidden->setCheckable(true);
  hidden->setChecked(m_filter->showHidden);
  connect(hidden, &QAction::toggled, this, [this](bool on) {
    m_filter->showHidden = on;
    m_filter->refilter();
  });
  menu.addAction(tr("Collapse All"), this, [this] { m_tree->collapseAll(); });
  menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

} // namespace ui
