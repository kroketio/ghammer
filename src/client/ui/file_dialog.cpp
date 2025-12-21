#include "file_dialog.h"
#include "file_icons.h"
#include "client/project/project_manager.h"

#include <QAbstractTableModel>
#include <QActionGroup>
#include <QCollator>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QSet>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#include "client/ui/action_manager.h"
#include "client/ui/dialog_style.h"
#include "client/ui/scale.h"
#include "client/ui/texture_ui.h"
#include "shared/lib/config.h"
#include "shared/lib/hdr_image.h"

namespace ui {
namespace {

const QString kGeometry = QStringLiteral("FileDialog/Geometry");
const QString kSplitter = QStringLiteral("FileDialog/Splitter");
const QString kThumbs = QStringLiteral("FileDialog/Thumbnails");
const QString kSortColumn = QStringLiteral("FileDialog/SortColumn");
const QString kSortOrder = QStringLiteral("FileDialog/SortOrder");
const QString kHidden = QStringLiteral("FileDialog/ShowHidden");
const QString kFilterOn = QStringLiteral("FileDialog/FilterOn");
const QString kCollapsed = QStringLiteral("FileDialog/Collapsed");
const QString kBookmarks = QStringLiteral("FileDialog/Bookmarks");
const QString kRecent = QStringLiteral("FileDialog/Recent");
const QString kLastDir = QStringLiteral("FileDialog/LastDir");
constexpr int kRecentMax = 20;
constexpr int kThumbPx = 256;  // decoded thumbnail size (longest side)

QString fileDialogStyle() {
  return dialogStyle() + QStringLiteral(R"(
#fdSide, #fdSideInner { background: #0f1328; }
#fdMain { background: #101428; }
QToolButton#fdSideHead { background: transparent; border: none; font-weight: 600; padding: 4px 2px; color: #d7def5; }
QToolButton#fdSideHead:hover { color: white; }
QToolButton#fdTool { background: #1a2042; border: 1px solid #262e57; border-radius: 4px; padding: 4px; }
QToolButton#fdTool:hover { background: #242c58; border-color: #3a4580; }
QToolButton#fdTool:checked { background: #4866C5; border-color: #4866C5; }
QToolButton#fdTool:disabled { background: #141936; border-color: #1d2347; }
QToolButton#fdTool::menu-indicator { image: none; width: 0; }
QLineEdit#fdPath, QLineEdit#fdSearch, QLineEdit#fdName {
  background: #0b0f20; border: 1px solid #232A4E; border-radius: 4px; padding: 5px 8px; }
QLineEdit#fdPath:focus, QLineEdit#fdSearch:focus, QLineEdit#fdName:focus { border-color: #4866C5; }
QLineEdit#fdPath[bad="true"] { border-color: #c5485e; }
QLineEdit#fdName[exists="true"] { background: #3b1724; border-color: #8f2f45; }
QTreeView#fdList, QListView#fdThumbs { background: #151a33; alternate-background-color: #192040; border: 1px solid #1f2747;
  border-radius: 6px; outline: 0; }
QTreeView#fdList::item { padding: 3px 2px; border: none; }
QTreeView#fdList::item:hover, QListView#fdThumbs::item:hover { background: #222a52; }
QTreeView#fdList::item:selected, QListView#fdThumbs::item:selected { background: #4866C5; color: white; }
QListView#fdThumbs::item { border-radius: 6px; padding: 4px; }
QTreeView#fdList QHeaderView::section { background: #151a33; color: #c9d2ee; border: none;
  border-bottom: 1px solid #232A4E; padding: 5px 8px; }
QComboBox#fdType { padding: 4px 8px; }
QPushButton#dlgPrimary, QPushButton#dlgFlat { min-width: 140px; }
)");
}

QString cleanDir(const QString &path) { return QDir::cleanPath(QDir(path).absolutePath()); }

QString homeDir() { return QDir::homePath(); }

QString shortSize(const qint64 bytes) {
  const double b = double(bytes);
  if (bytes < 1024)
    return QStringLiteral("%1 B").arg(bytes);
  if (b < 1024.0 * 1024.0)
    return QStringLiteral("%1 KB").arg(qRound(b / 1024.0));
  if (b < 1024.0 * 1024.0 * 1024.0)
    return QStringLiteral("%1 MB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
  return QStringLiteral("%1 GB").arg(b / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

QString shortDate(const QDateTime &t) {
  if (!t.isValid())
    return {};
  const QDate today = QDate::currentDate();
  const QString time = t.toString(QStringLiteral("HH:mm"));
  if (t.date() == today)
    return QObject::tr("Today %1").arg(time);
  if (t.date() == today.addDays(-1))
    return QObject::tr("Yesterday %1").arg(time);
  return QLocale::system().toString(t, QStringLiteral("d MMM yyyy HH:mm"));
}

bool canThumbnail(const QString &suffix) {
  static const QSet<QString> formats = [] {
    QSet<QString> s;
    for (const QByteArray &f: QImageReader::supportedImageFormats())
      s.insert(QString::fromLatin1(f).toLower());
    s.insert(QStringLiteral("exr"));
    s.remove(QStringLiteral("svg"));  // vector: the type icon is fine
    return s;
  }();
  return formats.contains(suffix.toLower());
}

QImage loadThumb(const QString &path) {
  if (path.endsWith(QLatin1String(".exr"), Qt::CaseInsensitive)) {
    const QSize full = hdr::exrSize(path);
    if (!full.isValid() || full.isEmpty())
      return {};
    const QSize s = full.scaled(kThumbPx, kThumbPx, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    hdr::Image img;
    if (!hdr::loadExr(path, img, nullptr, s) || img.isNull())
      return {};
    return hdr::tonemapped(img, img.w, img.h);
  }
  QImageReader r(path);
  r.setAutoTransform(true);
  const QSize full = r.size();
  if (full.isValid() && (full.width() > kThumbPx || full.height() > kThumbPx))
    r.setScaledSize(full.scaled(kThumbPx, kThumbPx, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
  QImage img = r.read();
  if (!img.isNull() && (img.width() > kThumbPx || img.height() > kThumbPx))
    img = img.scaled(kThumbPx, kThumbPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return img;
}

// Decoded thumbnails by path + modification time, shared by every dialog.
QHash<QString, QPixmap> &thumbCache() {
  static QHash<QString, QPixmap> cache;
  return cache;
}

QString thumbKey(const QString &path, const QDateTime &mtime) {
  return path + QLatin1Char('|') + QString::number(mtime.toMSecsSinceEpoch());
}

QRegularExpression wildcard(const QString &pattern, const bool anchored) {
  return QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive,
                                          anchored ? QRegularExpression::DefaultWildcardConversion
                                                   : QRegularExpression::UnanchoredWildcardConversion);
}

QStringList configList(const QString &key) { return config()->getRaw(key).toStringList(); }

} // namespace

struct FileEntry {
  QString name;
  QString suffix;
  bool dir = false;
  bool godot = false;  // a folder with project.godot
  bool hidden = false;
  qint64 size = 0;
  QDateTime mtime;
};

class FileListModel final : public QAbstractTableModel {
public:
  enum Col { Name, Modified, Size, ColCount };

  explicit FileListModel(QObject *parent) : QAbstractTableModel(parent) {
    m_pool.setMaxThreadCount(2);
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
  }
  ~FileListModel() override {
    m_pool.clear();
    m_pool.waitForDone();
  }

  std::function<void(const QString &)> renamed;

  void list(const QString &dir) {
    m_pool.clear();
    m_pending.clear();
    m_dir = dir;
    m_all.clear();
    const QFileInfoList infos =
        QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
    m_all.reserve(size_t(infos.size()));
    for (const QFileInfo &fi: infos) {
      if (!fi.exists())
        continue;  // a broken link
      FileEntry e;
      e.name = fi.fileName();
      e.dir = fi.isDir();
      e.suffix = e.dir ? QString() : fi.suffix();
      e.hidden = fi.isHidden();
      e.size = e.dir ? 0 : fi.size();
      e.mtime = fi.lastModified();
      if (e.dir)
        e.godot = QFileInfo::exists(fi.absoluteFilePath() + QStringLiteral("/project.godot"));
      m_all.push_back(std::move(e));
    }
    rebuild();
  }

  void setOptions(const QStringList &patterns, const bool dirsOnly, const bool showHidden, const QString &search) {
    m_patterns.clear();
    for (const QString &p: patterns)
      m_patterns << wildcard(p, true);
    m_dirsOnly = dirsOnly;
    m_showHidden = showHidden;
    m_search = search.trimmed();
    rebuild();
  }

  void setThumbnails(const bool on) {
    if (m_thumbs == on)
      return;
    m_thumbs = on;
    if (!m_rows.empty())
      emit dataChanged(index(0, Name), index(rowCount() - 1, Name), {Qt::DecorationRole});
  }

  [[nodiscard]] const FileEntry &entry(const int row) const { return m_all[size_t(m_rows[size_t(row)])]; }
  [[nodiscard]] QString path(const int row) const { return QDir(m_dir).filePath(entry(row).name); }
  [[nodiscard]] int rowOf(const QString &name) const {
    for (int r = 0; r < int(m_rows.size()); ++r)
      if (entry(r).name == name)
        return r;
    return -1;
  }
  [[nodiscard]] int sortColumn() const { return m_sortCol; }
  [[nodiscard]] Qt::SortOrder sortOrder() const { return m_order; }

  int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : int(m_rows.size()); }
  int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : ColCount; }

  QVariant data(const QModelIndex &idx, const int role) const override {
    if (!idx.isValid() || idx.row() >= int(m_rows.size()))
      return {};
    const FileEntry &e = entry(idx.row());
    const int col = idx.column();
    switch (role) {
    case Qt::DisplayRole:
      if (col == Name)
        return e.name;
      if (col == Modified)
        return shortDate(e.mtime);
      if (col == Size)
        return e.dir ? QString() : shortSize(e.size);
      return {};
    case Qt::EditRole:
      return col == Name ? QVariant(e.name) : QVariant();
    case Qt::DecorationRole:
      return col == Name ? QVariant(iconFor(e)) : QVariant();
    case Qt::ToolTipRole:
      return col == Name ? QVariant(e.name) : QVariant();
    case Qt::TextAlignmentRole:
      if (col == Size)
        return int(Qt::AlignRight | Qt::AlignVCenter);
      return {};
    case Qt::ForegroundRole:
      if (e.hidden)
        return QColor(0x7b, 0x86, 0xa8);
      if (col != Name)
        return QColor(0xb4, 0xbd, 0xd8);
      return {};
    default:
      return {};
    }
  }

  QVariant headerData(const int section, const Qt::Orientation o, const int role) const override {
    if (o != Qt::Horizontal)
      return {};
    if (role == Qt::DisplayRole) {
      switch (section) {
      case Name: return QObject::tr("Name");
      case Modified: return QObject::tr("Date Modified");
      case Size: return QObject::tr("Size");
      default: return {};
      }
    }
    if (role == Qt::TextAlignmentRole)
      return int((section == Size ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    return {};
  }

  Qt::ItemFlags flags(const QModelIndex &idx) const override {
    if (!idx.isValid())
      return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
    if (idx.column() == Name)
      f |= Qt::ItemIsEditable;
    return f;
  }

  bool setData(const QModelIndex &idx, const QVariant &value, const int role) override {
    if (role != Qt::EditRole || idx.column() != Name)
      return false;
    const QString to = value.toString().trimmed();
    const QString from = entry(idx.row()).name;
    if (to.isEmpty() || to == from || to.contains(QLatin1Char('/')) || QFileInfo::exists(QDir(m_dir).filePath(to)))
      return false;
    if (!QDir(m_dir).rename(from, to))
      return false;
    list(m_dir);
    if (renamed)
      renamed(to);
    return true;
  }

  void sort(const int column, const Qt::SortOrder order) override {
    m_sortCol = std::clamp(column, 0, ColCount - 1);
    m_order = order;
    rebuild();
  }

private:
  void rebuild() {
    beginResetModel();
    m_rows.clear();
    const bool wild = m_search.contains(QLatin1Char('*')) || m_search.contains(QLatin1Char('?'));
    const QRegularExpression search = wild ? wildcard(m_search, false) : QRegularExpression();
    for (int i = 0; i < int(m_all.size()); ++i) {
      const FileEntry &e = m_all[size_t(i)];
      if (e.hidden && !m_showHidden)
        continue;
      if (!e.dir && m_dirsOnly)
        continue;
      if (!e.dir && !m_patterns.isEmpty() &&
          std::none_of(m_patterns.begin(), m_patterns.end(),
                       [&](const QRegularExpression &re) { return re.match(e.name).hasMatch(); }))
        continue;
      if (!m_search.isEmpty() &&
          !(wild ? search.match(e.name).hasMatch() : e.name.contains(m_search, Qt::CaseInsensitive)))
        continue;
      m_rows.push_back(i);
    }
    const bool asc = m_order == Qt::AscendingOrder;
    std::stable_sort(m_rows.begin(), m_rows.end(), [&](const int ia, const int ib) {
      const FileEntry &a = m_all[size_t(ia)], &b = m_all[size_t(ib)];
      if (a.dir != b.dir)
        return a.dir;  // folders first, whichever way
      int c = 0;
      if (m_sortCol == Modified)
        c = a.mtime < b.mtime ? -1 : (b.mtime < a.mtime ? 1 : 0);
      else if (m_sortCol == Size)
        c = a.size < b.size ? -1 : (b.size < a.size ? 1 : 0);
      if (c == 0)
        c = m_collator.compare(a.name, b.name);
      return asc ? c < 0 : c > 0;
    });
    endResetModel();
  }

  QIcon iconFor(const FileEntry &e) const {
    if (e.dir)
      return ActionManager::icon(e.godot ? QStringLiteral("Godot") : QStringLiteral("file_folder"));
    if (m_thumbs && canThumbnail(e.suffix)) {
      const QString p = QDir(m_dir).filePath(e.name);
      const QString key = thumbKey(p, e.mtime);
      auto it = thumbCache().constFind(key);
      if (it != thumbCache().constEnd()) {
        if (!it->isNull())
          return QIcon(*it);
      } else {
        requestThumb(p, key);
      }
    }
    return ActionManager::icon(iconNameForSuffix(e.suffix));
  }

  void requestThumb(const QString &path, const QString &key) const {
    if (m_pending.contains(key))
      return;
    m_pending.insert(key);
    auto *self = const_cast<FileListModel *>(this);
    m_pool.start([self, path, key] {
      const QImage img = loadThumb(path);
      QMetaObject::invokeMethod(
          self,
          [self, path, key, img] {
            self->m_pending.remove(key);
            auto &cache = thumbCache();
            if (cache.size() > 600)
              cache.clear();
            cache.insert(key, img.isNull() ? QPixmap() : QPixmap::fromImage(img));
            const QFileInfo fi(path);
            if (fi.absolutePath() != self->m_dir)
              return;
            const int r = self->rowOf(fi.fileName());
            if (r >= 0)
              emit self->dataChanged(self->index(r, Name), self->index(r, Name), {Qt::DecorationRole});
          },
          Qt::QueuedConnection);
    });
  }

  QString m_dir;
  std::vector<FileEntry> m_all;
  std::vector<int> m_rows;
  QList<QRegularExpression> m_patterns;
  bool m_dirsOnly = false;
  bool m_showHidden = false;
  QString m_search;
  int m_sortCol = Name;
  Qt::SortOrder m_order = Qt::AscendingOrder;
  bool m_thumbs = false;
  QCollator m_collator;
  mutable QThreadPool m_pool;
  mutable QSet<QString> m_pending;
};

namespace {

QStringList run(QWidget *parent, const FileDialog::Mode mode, const QString &caption, const QString &dir,
                const QString &filter, const QString &acceptLabel) {
  FileDialog dlg(parent, mode, caption, dir, filter, acceptLabel);
  if (dlg.exec() != QDialog::Accepted)
    return {};
  return dlg.selectedPaths();
}

} // namespace

QString FileDialog::getOpenFileName(QWidget *parent, const QString &caption, const QString &dir,
                                    const QString &filter, const QString &acceptLabel) {
  return run(parent, Mode::Open, caption, dir, filter, acceptLabel).value(0);
}

QStringList FileDialog::getOpenFileNames(QWidget *parent, const QString &caption, const QString &dir,
                                         const QString &filter, const QString &acceptLabel) {
  return run(parent, Mode::OpenMany, caption, dir, filter, acceptLabel);
}

QString FileDialog::getSaveFileName(QWidget *parent, const QString &caption, const QString &dir,
                                    const QString &filter, const QString &acceptLabel) {
  return run(parent, Mode::Save, caption, dir, filter, acceptLabel).value(0);
}

QString FileDialog::getExistingDirectory(QWidget *parent, const QString &caption, const QString &dir,
                                         const QString &acceptLabel) {
  return run(parent, Mode::Directory, caption, dir, {}, acceptLabel).value(0);
}

FileDialog::FileDialog(QWidget *parent, const Mode mode, const QString &caption, const QString &dir,
                       const QString &filter, const QString &acceptLabel)
    : QDialog(parent), m_mode(mode) {
  setWindowTitle(caption);
  setModal(true);
  setSizeGripEnabled(true);

  // "Label (*.a *.b);;Other (*.c)"
  for (const QString &part: filter.split(QStringLiteral(";;"), Qt::SkipEmptyParts)) {
    Filter f;
    f.label = part.trimmed();
    const qsizetype open = part.lastIndexOf(QLatin1Char('(')), close = part.lastIndexOf(QLatin1Char(')'));
    const QString pats = open >= 0 && close > open ? part.mid(open + 1, close - open - 1) : part;
    for (const QString &p: pats.split(QLatin1Char(' '), Qt::SkipEmptyParts))
      if (p.contains(QLatin1Char('*')) && p != QLatin1String("*") && p != QLatin1String("*.*"))
        f.patterns << p;
    m_filters << f;
  }
  if (m_filters.isEmpty())
    m_filters << Filter {tr("All files"), {}};

  m_model = new FileListModel(this);
  m_model->renamed = [this](const QString &name) { selectName(name); };
  m_watcher = new QFileSystemWatcher(this);
  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(150);
  connect(m_refreshTimer, &QTimer::timeout, this, &FileDialog::refresh);
  connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_refreshTimer, qOverload<>(&QTimer::start));

  buildUi(acceptLabel);
  scale::setScaledStyleSheet(this, fileDialogStyle());

  QString start = config()->getRaw(kLastDir).toString();
  if (start.isEmpty() || !QFileInfo(start).isDir())
    start = homeDir();
  QString name;
  if (!dir.isEmpty()) {
    const QFileInfo fi(dir);
    if (fi.isRelative() && !dir.contains(QLatin1Char('/'))) {
      name = dir;  // a bare file name: the last folder
    } else if (fi.isDir()) {
      start = fi.absoluteFilePath();
    } else {
      name = fi.fileName();
      start = fi.absolutePath();
    }
  }
  start = cleanDir(start);
  while (!QFileInfo(start).isDir() && start != QLatin1String("/"))
    start = QFileInfo(start).absolutePath();

  const QByteArray geo = config()->getRaw(kGeometry).toByteArray();
  if (geo.isEmpty() || !restoreGeometry(geo))
    resize(scale::initialSize(this, QSize(1120, 700)));
  const QByteArray split = config()->getRaw(kSplitter).toByteArray();
  if (split.isEmpty() || !m_split->restoreState(split))
    m_split->setSizes({scale::px(260), scale::px(860)});

  navigate(start, false);
  if (m_mode != Mode::Directory) {
    m_name->setText(name);
    if (!name.isEmpty())
      selectName(name);
  }
  if (m_mode == Mode::Save) {
    m_name->setFocus();
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    m_name->setSelection(0, dot > 0 ? int(dot) : int(name.size()));
  } else {
    m_views->currentWidget()->setFocus();
  }
  updateState();
}

FileDialog::~FileDialog() = default;

static QToolButton *makeTool(QWidget *parent, const QString &icon, const QString &tip, const bool checkable = false) {
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("fdTool"));
  b->setIcon(ActionManager::icon(icon));
  b->setToolTip(tip);
  b->setCheckable(checkable);
  b->setFocusPolicy(Qt::TabFocus);
  return b;
}

void FileDialog::buildUi(const QString &acceptLabel) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  m_split = new QSplitter(Qt::Horizontal, this);
  m_split->setChildrenCollapsible(false);
  m_split->setHandleWidth(3);
  root->addWidget(m_split);

  m_side = buildSidebar();
  m_split->addWidget(m_side);

  auto *main = new QWidget(m_split);
  main->setObjectName(QStringLiteral("fdMain"));
  auto *mv = new QVBoxLayout(main);
  mv->setContentsMargins(10, 10, 10, 10);
  mv->setSpacing(8);
  m_split->addWidget(main);
  m_split->setStretchFactor(1, 1);

  auto *top = new QHBoxLayout;
  top->setSpacing(4);
  m_backBtn = makeTool(main, QStringLiteral("back"), tr("Back"));
  m_forwardBtn = makeTool(main, QStringLiteral("forward"), tr("Forward"));
  m_upBtn = makeTool(main, QStringLiteral("file_parent"), tr("Parent folder"));
  auto *refreshBtn = makeTool(main, QStringLiteral("file_refresh"), tr("Refresh"));
  auto *newBtn = makeTool(main, QStringLiteral("newfolder"), tr("New folder"));
  connect(m_backBtn, &QToolButton::clicked, this, &FileDialog::goBack);
  connect(m_forwardBtn, &QToolButton::clicked, this, &FileDialog::goForward);
  connect(m_upBtn, &QToolButton::clicked, this, &FileDialog::goUp);
  connect(refreshBtn, &QToolButton::clicked, this, &FileDialog::refresh);
  connect(newBtn, &QToolButton::clicked, this, &FileDialog::newFolder);
  top->addWidget(m_backBtn);
  top->addWidget(m_forwardBtn);
  top->addWidget(m_upBtn);
  top->addWidget(refreshBtn);
  top->addSpacing(6);
  top->addWidget(newBtn);
  top->addSpacing(6);

  m_path = new QLineEdit(main);
  m_path->setObjectName(QStringLiteral("fdPath"));
  auto *dirs = new QFileSystemModel(m_path);
  dirs->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
  dirs->setRootPath(QString());
  auto *completer = new QCompleter(dirs, m_path);
  completer->setCaseSensitivity(Qt::CaseInsensitive);
  m_path->setCompleter(completer);
  connect(m_path, &QLineEdit::returnPressed, this, [this] {
    QString t = m_path->text().trimmed();
    if (t.startsWith(QLatin1Char('~')))
      t = homeDir() + t.mid(1);
    const QFileInfo fi(t);
    if (fi.isDir()) {
      navigate(t);
      m_views->currentWidget()->setFocus();
    } else if (fi.isFile() && m_mode != Mode::Directory) {
      navigate(fi.absolutePath());
      m_name->setText(fi.fileName());
      selectName(fi.fileName());
    } else {
      m_path->setProperty("bad", true);
      m_path->style()->unpolish(m_path);
      m_path->style()->polish(m_path);
    }
  });
  connect(m_path, &QLineEdit::textEdited, this, [this] {
    if (m_path->property("bad").toBool()) {
      m_path->setProperty("bad", false);
      m_path->style()->unpolish(m_path);
      m_path->style()->polish(m_path);
    }
  });
  top->addWidget(m_path, 1);

  m_search = new QLineEdit(main);
  m_search->setObjectName(QStringLiteral("fdSearch"));
  m_search->setPlaceholderText(tr("Search"));
  m_search->setClearButtonEnabled(true);
  m_search->addAction(ActionManager::icon(QStringLiteral("viewzoom")), QLineEdit::LeadingPosition);
  m_search->installEventFilter(this);
  connect(m_search, &QLineEdit::textChanged, this, &FileDialog::applyFilter);
  connect(m_search, &QLineEdit::returnPressed, this, [this] { m_views->currentWidget()->setFocus(); });
  top->addWidget(m_search);
  top->addSpacing(6);

  m_listBtn = makeTool(main, QStringLiteral("longdisplay"), tr("List"), true);
  m_thumbBtn = makeTool(main, QStringLiteral("imgdisplay"), tr("Thumbnails"), true);
  connect(m_listBtn, &QToolButton::clicked, this, [this] { setThumbnails(false); });
  connect(m_thumbBtn, &QToolButton::clicked, this, [this] { setThumbnails(true); });
  top->addWidget(m_listBtn);
  top->addWidget(m_thumbBtn);
  top->addSpacing(6);

  m_filterBtn = makeTool(main, QStringLiteral("filter"), tr("Show only files of the chosen type"), true);
  m_filterBtn->setChecked(config()->getRaw(kFilterOn, true).toBool());
  m_filterBtn->setVisible(m_mode != Mode::Directory);
  connect(m_filterBtn, &QToolButton::toggled, this, [this](const bool on) {
    config()->setRaw(kFilterOn, on);
    applyFilter();
  });
  top->addWidget(m_filterBtn);

  auto *more = makeTool(main, QStringLiteral("tria_down"), tr("Display options"));
  more->setPopupMode(QToolButton::InstantPopup);
  auto *menu = new QMenu(more);
  QAction *hidden = menu->addAction(tr("Show Hidden Files"));
  hidden->setCheckable(true);
  hidden->setChecked(config()->getRaw(kHidden, false).toBool());
  hidden->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
  hidden->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  addAction(hidden);
  connect(hidden, &QAction::toggled, this, [this](const bool on) {
    config()->setRaw(kHidden, on);
    applyFilter();
  });
  menu->addSeparator();
  menu->addSection(tr("Sort by"));
  auto *sortGroup = new QActionGroup(menu);
  const QStringList sortNames = {tr("Name"), tr("Date Modified"), tr("Size")};
  for (int c = 0; c < FileListModel::ColCount; ++c) {
    QAction *a = menu->addAction(sortNames[c]);
    a->setCheckable(true);
    sortGroup->addAction(a);
    connect(a, &QAction::triggered, this, [this, c] {
      m_tree->sortByColumn(c, c == FileListModel::Name ? Qt::AscendingOrder : Qt::DescendingOrder);
    });
  }
  connect(menu, &QMenu::aboutToShow, this, [this, sortGroup] {
    const auto acts = sortGroup->actions();
    for (int c = 0; c < acts.size(); ++c)
      acts[c]->setChecked(c == m_model->sortColumn());
  });
  more->setMenu(menu);
  top->addWidget(more);
  mv->addLayout(top);

  // The views share the model and the selection.
  m_views = new QStackedWidget(main);
  m_tree = new QTreeView(m_views);
  m_tree->setObjectName(QStringLiteral("fdList"));
  m_tree->setModel(m_model);
  m_tree->setRootIsDecorated(false);
  m_tree->setItemsExpandable(false);
  m_tree->setUniformRowHeights(true);
  m_tree->setAlternatingRowColors(true);
  m_tree->setAllColumnsShowFocus(true);
  m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
  m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_tree->setSelectionMode(m_mode == Mode::OpenMany ? QAbstractItemView::ExtendedSelection
                                                    : QAbstractItemView::SingleSelection);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  m_tree->header()->setStretchLastSection(false);
  m_tree->header()->setSectionResizeMode(FileListModel::Name, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(FileListModel::Modified, QHeaderView::Interactive);
  m_tree->header()->setSectionResizeMode(FileListModel::Size, QHeaderView::Interactive);
  m_tree->header()->setSectionsMovable(false);
  m_tree->setSortingEnabled(true);
  m_tree->sortByColumn(std::clamp(config()->getRaw(kSortColumn, 0).toInt(), 0, FileListModel::ColCount - 1),
                       Qt::SortOrder(config()->getRaw(kSortOrder, int(Qt::AscendingOrder)).toInt()));
  m_views->addWidget(m_tree);

  m_thumbs = new QListView(m_views);
  m_thumbs->setObjectName(QStringLiteral("fdThumbs"));
  m_thumbs->setModel(m_model);
  m_thumbs->setSelectionModel(m_tree->selectionModel());
  m_thumbs->setViewMode(QListView::IconMode);
  m_thumbs->setResizeMode(QListView::Adjust);
  m_thumbs->setMovement(QListView::Static);
  m_thumbs->setUniformItemSizes(true);
  m_thumbs->setWordWrap(true);
  m_thumbs->setTextElideMode(Qt::ElideMiddle);
  m_thumbs->setSpacing(4);
  m_thumbs->setEditTriggers(QAbstractItemView::EditKeyPressed);
  m_thumbs->setSelectionMode(m_tree->selectionMode());
  m_thumbs->setContextMenuPolicy(Qt::CustomContextMenu);
  m_views->addWidget(m_thumbs);
  mv->addWidget(m_views, 1);

  for (QAbstractItemView *v: {static_cast<QAbstractItemView *>(m_tree), static_cast<QAbstractItemView *>(m_thumbs)}) {
    v->installEventFilter(this);
    connect(v, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &i) { activate(i.row()); });
    connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint &p) { showListMenu(p, v); });
  }
  connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FileDialog::selectionToName);
  connect(m_tree->header(), &QHeaderView::sortIndicatorChanged, this, [](const int c, const Qt::SortOrder o) {
    config()->setRaw(kSortColumn, c);
    config()->setRaw(kSortOrder, int(o));
  });

  auto *bottom = new QHBoxLayout;
  bottom->setSpacing(6);
  m_name = new QLineEdit(main);
  m_name->setObjectName(QStringLiteral("fdName"));
  connect(m_name, &QLineEdit::textChanged, this, &FileDialog::updateState);
  connect(m_name, &QLineEdit::returnPressed, this, &FileDialog::tryAccept);
  auto *minus = makeTool(main, QStringLiteral("remove"), tr("Lower the number in the name"));
  auto *plus = makeTool(main, QStringLiteral("add"), tr("Raise the number in the name"));
  connect(minus, &QToolButton::clicked, this, [this] { bumpNumber(-1); });
  connect(plus, &QToolButton::clicked, this, [this] { bumpNumber(1); });
  bottom->addWidget(m_name, 1);
  bottom->addWidget(minus);
  bottom->addWidget(plus);
  const bool named = m_mode != Mode::Directory;
  m_name->setVisible(named);
  minus->setVisible(m_mode == Mode::Save);
  plus->setVisible(m_mode == Mode::Save);
  if (!named)
    bottom->addStretch(1);

  m_type = new QComboBox(main);
  m_type->setObjectName(QStringLiteral("fdType"));
  for (const Filter &f: m_filters)
    m_type->addItem(f.label);
  m_type->setVisible(named && m_filters.size() > 1);
  connect(m_type, &QComboBox::currentIndexChanged, this, [this](const int i) {
    m_filterIndex = std::max(0, i);
    applyFilter();
  });
  bottom->addWidget(m_type);
  bottom->addSpacing(8);

  auto *cancel = new QPushButton(tr("Cancel"), main);
  cancel->setObjectName(QStringLiteral("dlgFlat"));
  cancel->setAutoDefault(false);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  QString label = acceptLabel;
  if (label.isEmpty())
    label = m_mode == Mode::Save ? tr("Save") : m_mode == Mode::Directory ? tr("Select Folder") : tr("Open");
  m_accept = new QPushButton(label, main);
  m_accept->setObjectName(QStringLiteral("dlgPrimary"));
  m_accept->setAutoDefault(false);
  connect(m_accept, &QPushButton::clicked, this, &FileDialog::tryAccept);
  bottom->addWidget(cancel);
  bottom->addWidget(m_accept);
  mv->addLayout(bottom);

  const auto key = [this](const QKeySequence &k, auto fn) {
    auto *s = new QShortcut(k, this);
    s->setContext(Qt::WidgetWithChildrenShortcut);
    connect(s, &QShortcut::activated, this, fn);
  };
  key(QKeySequence(Qt::ALT | Qt::Key_Left), [this] { goBack(); });
  key(QKeySequence(Qt::ALT | Qt::Key_Right), [this] { goForward(); });
  key(QKeySequence(Qt::ALT | Qt::Key_Up), [this] { goUp(); });
  key(QKeySequence(Qt::Key_F5), [this] { refresh(); });
  key(QKeySequence(Qt::CTRL | Qt::Key_L), [this] {
    m_path->setFocus();
    m_path->selectAll();
  });
  key(QKeySequence(Qt::CTRL | Qt::Key_F), [this] {
    m_search->setFocus();
    m_search->selectAll();
  });
  key(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] { newFolder(); });

  setThumbnails(config()->getRaw(kThumbs, false).toBool());

  scale::bind(this, [this] {
    const QSize ic = scale::icon(scale::Icon::Medium);
    for (auto *b: findChildren<QToolButton *>(QStringLiteral("fdTool")))
      b->setIconSize(ic);
    m_search->setFixedWidth(scale::tpx(230));
    m_tree->setIconSize(scale::icon(scale::Icon::Small));
    m_tree->header()->resizeSection(FileListModel::Modified, scale::tpx(170));
    m_tree->header()->resizeSection(FileListModel::Size, scale::tpx(90));
    const int t = scale::px(96);
    m_thumbs->setIconSize(QSize(t, t));
    m_thumbs->setGridSize(QSize(scale::px(128), t + scale::tpx(44)));
    refitPlaces();
  });
}

QWidget *FileDialog::buildSidebar() {
  auto *scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("fdSide"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *inner = new QWidget(scroll);
  inner->setObjectName(QStringLiteral("fdSideInner"));
  auto *v = new QVBoxLayout(inner);
  v->setContentsMargins(8, 10, 8, 10);
  v->setSpacing(4);

  const auto makeList = [this](QWidget *parent) {
    QListWidget *l = sidebarList(parent);
    l->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    l->setContextMenuPolicy(Qt::CustomContextMenu);
    l->setFocusPolicy(Qt::NoFocus);
    connect(l, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
      const QString d = it->data(Qt::UserRole).toString();
      if (QFileInfo(d).isDir())
        navigate(d);
      else
        litPlaces();
    });
    return l;
  };

  // Projects: the recent ones and the others in ~/GodotProjects.
  m_projects = makeList(inner);
  addSection(v, QStringLiteral("projects"), tr("Projects"), m_projects);

  auto *bm = new QWidget(inner);
  auto *bl = new QVBoxLayout(bm);
  bl->setContentsMargins(0, 0, 0, 0);
  bl->setSpacing(4);
  auto *add = new QPushButton(ActionManager::icon(QStringLiteral("add")), tr("Add Bookmark"), bm);
  add->setObjectName(QStringLiteral("dlgFlat"));
  add->setAutoDefault(false);
  add->setStyleSheet(QStringLiteral("min-width: 0;"));
  add->setToolTip(tr("Bookmark the current folder"));
  connect(add, &QPushButton::clicked, this, [this] { addBookmark(m_dir); });
  m_bookmarks = makeList(bm);
  connect(m_bookmarks, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
    QListWidgetItem *it = m_bookmarks->itemAt(p);
    if (!it)
      return;
    QMenu menu(this);
    const QString d = it->data(Qt::UserRole).toString();
    menu.addAction(ActionManager::icon(QStringLiteral("remove")), tr("Remove Bookmark"), this, [this, d] {
      QStringList l = configList(kBookmarks);
      l.removeAll(d);
      config()->setRaw(kBookmarks, l);
      fillBookmarks();
    });
    menu.exec(m_bookmarks->viewport()->mapToGlobal(p));
  });
  bl->addWidget(add);
  bl->addWidget(m_bookmarks);
  addSection(v, QStringLiteral("bookmarks"), tr("Bookmarks"), bm);

  m_system = makeList(inner);
  addSection(v, QStringLiteral("system"), tr("System"), m_system);
  m_volumes = makeList(inner);
  addSection(v, QStringLiteral("volumes"), tr("Volumes"), m_volumes);
  m_recent = makeList(inner);
  connect(m_recent, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
    QMenu menu(this);
    if (QListWidgetItem *it = m_recent->itemAt(p)) {
      const QString d = it->data(Qt::UserRole).toString();
      menu.addAction(ActionManager::icon(QStringLiteral("bookmarks")), tr("Add Bookmark"), this,
                     [this, d] { addBookmark(d); });
      menu.addAction(ActionManager::icon(QStringLiteral("remove")), tr("Remove"), this, [this, d] {
        QStringList l = configList(kRecent);
        l.removeAll(d);
        config()->setRaw(kRecent, l);
        fillRecent();
      });
    }
    menu.addAction(ActionManager::icon(QStringLiteral("trash")), tr("Clear Recent"), this, [this] {
      config()->setRaw(kRecent, QStringList());
      fillRecent();
    });
    menu.exec(m_recent->viewport()->mapToGlobal(p));
  });
  addSection(v, QStringLiteral("recent"), tr("Recent"), m_recent);
  v->addStretch(1);
  scroll->setWidget(inner);

  fillProjects();
  fillPlaces();
  fillBookmarks();
  fillRecent();
  return scroll;
}

QWidget *FileDialog::addSection(QVBoxLayout *into, const QString &key, const QString &title, QWidget *body) {
  auto *head = new QToolButton(body->parentWidget());
  head->setObjectName(QStringLiteral("fdSideHead"));
  head->setText(title);
  head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  head->setCheckable(true);
  head->setFocusPolicy(Qt::NoFocus);
  head->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  const bool open = !configList(kCollapsed).contains(key);
  const auto show = [head, body](const bool on) {
    head->setIcon(ActionManager::icon(on ? QStringLiteral("tria_down") : QStringLiteral("tria_right")));
    body->setVisible(on);
  };
  head->setChecked(open);
  show(open);
  connect(head, &QToolButton::toggled, this, [key, show](const bool on) {
    show(on);
    QStringList c = configList(kCollapsed);
    c.removeAll(key);
    if (!on)
      c << key;
    config()->setRaw(kCollapsed, c);
  });
  scale::bind(head, [head] { head->setIconSize(scale::icon(scale::Icon::Small)); });
  into->addWidget(head);
  into->addWidget(body);
  into->addSpacing(6);
  return head;
}

namespace {

QListWidgetItem *placeItem(QListWidget *list, const QString &icon, const QString &label, const QString &dir) {
  auto *it = new QListWidgetItem(ActionManager::icon(icon), label, list);
  it->setData(Qt::UserRole, cleanDir(dir));
  it->setToolTip(QDir::toNativeSeparators(dir));
  return it;
}

} // namespace

void FileDialog::fillPlaces() {
  m_system->clear();
  placeItem(m_system, QStringLiteral("home"), tr("Home"), homeDir());
  const struct {
    QStandardPaths::StandardLocation loc;
    const char *icon;
    const char *label;
  } xdg[] = {
      {QStandardPaths::DesktopLocation, "desktop", QT_TR_NOOP("Desktop")},
      {QStandardPaths::DocumentsLocation, "documents", QT_TR_NOOP("Documents")},
      {QStandardPaths::DownloadLocation, "import", QT_TR_NOOP("Downloads")},
      {QStandardPaths::MoviesLocation, "file_movie", QT_TR_NOOP("Videos")},
      {QStandardPaths::PicturesLocation, "file_image", QT_TR_NOOP("Pictures")},
      {QStandardPaths::MusicLocation, "file_sound", QT_TR_NOOP("Music")},
  };
  for (const auto &x: xdg) {
    const QString p = QStandardPaths::writableLocation(x.loc);
    if (!p.isEmpty() && cleanDir(p) != cleanDir(homeDir()) && QFileInfo(p).isDir())
      placeItem(m_system, QLatin1String(x.icon), tr(x.label), p);
  }

  m_volumes->clear();
  static const QSet<QByteArray> pseudo = {"tmpfs",    "devtmpfs", "squashfs", "overlay", "proc",       "sysfs",
                                          "cgroup",   "cgroup2",  "autofs",   "ramfs",   "efivarfs",   "nsfs",
                                          "tracefs",  "debugfs",  "securityfs", "pstore", "bpf",       "configfs",
                                          "fusectl",  "mqueue",   "hugetlbfs", "binfmt_misc", "fuse.portal",
                                          "fuse.gvfsd-fuse"};
  QStringList seen;
  for (const QStorageInfo &s: QStorageInfo::mountedVolumes()) {
    if (!s.isValid() || !s.isReady() || pseudo.contains(s.fileSystemType()))
      continue;
    const QString root = s.rootPath();
    const bool external = root.startsWith(QLatin1String("/media/")) || root.startsWith(QLatin1String("/mnt/")) ||
                          root.startsWith(QLatin1String("/run/media/"));
    if (!(root == QLatin1String("/") || root == QLatin1String("/home") || external) || seen.contains(root))
      continue;
    seen << root;
    QString label = root == QLatin1String("/") ? QStringLiteral("/") : s.name();
    if (label.isEmpty())
      label = QFileInfo(root).fileName();
    placeItem(m_volumes, external ? QStringLiteral("external_drive") : QStringLiteral("disk_drive"), label, root);
  }
  refitPlaces();
}

void FileDialog::fillProjects() {
  m_projects->clear();
  for (const auto &r: proj::ProjectManager::instance().projectList())
    if (r.exists)
      placeItem(m_projects, QStringLiteral("file_folder"), r.name, r.root);
  m_projects->setVisible(m_projects->count() > 0);
  refitPlaces();
  litPlaces();
}

void FileDialog::refitPlaces() {
  for (QListWidget *l: {m_projects, m_bookmarks, m_system, m_volumes, m_recent})
    if (l)
      fitListHeight(l, 40);
}

void FileDialog::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  QTimer::singleShot(0, this, [this] { refitPlaces(); });
}

void FileDialog::fillBookmarks() {
  m_bookmarks->clear();
  for (const QString &d: configList(kBookmarks)) {
    QListWidgetItem *it = placeItem(m_bookmarks, QStringLiteral("file_folder"), QFileInfo(d).fileName(), d);
    if (it->text().isEmpty())
      it->setText(d);
    if (!QFileInfo(d).isDir())
      it->setForeground(QColor(0x59, 0x62, 0x7f));
  }
  m_bookmarks->setVisible(m_bookmarks->count() > 0);
  fitListHeight(m_bookmarks, 40);
  litPlaces();
}

void FileDialog::fillRecent() {
  m_recent->clear();
  for (const QString &d: configList(kRecent)) {
    QListWidgetItem *it = placeItem(m_recent, QStringLiteral("file_folder"), QFileInfo(d).fileName(), d);
    if (it->text().isEmpty())
      it->setText(d);
    if (!QFileInfo(d).isDir())
      it->setForeground(QColor(0x59, 0x62, 0x7f));
  }
  fitListHeight(m_recent, 40);
  litPlaces();
}

void FileDialog::litPlaces() {
  for (QListWidget *l: {m_projects, m_bookmarks, m_system, m_volumes, m_recent}) {
    if (!l)
      continue;
    const QSignalBlocker block(l);
    l->clearSelection();
    l->setCurrentItem(nullptr);
    for (int i = 0; i < l->count(); ++i)
      if (l->item(i)->data(Qt::UserRole).toString() == m_dir) {
        l->item(i)->setSelected(true);
        break;
      }
  }
}

void FileDialog::addBookmark(const QString &dir) {
  if (dir.isEmpty())
    return;
  QStringList l = configList(kBookmarks);
  if (!l.contains(dir))
    l << dir;
  config()->setRaw(kBookmarks, l);
  fillBookmarks();
}

void FileDialog::addRecent(const QString &dir) {
  QStringList l = configList(kRecent);
  l.removeAll(dir);
  l.prepend(dir);
  while (l.size() > kRecentMax)
    l.removeLast();
  config()->setRaw(kRecent, l);
}

void FileDialog::navigate(const QString &dir, const bool record) {
  const QString d = cleanDir(dir);
  if (!QFileInfo(d).isDir())
    return;
  if (record && !m_dir.isEmpty() && d != m_dir) {
    m_back << m_dir;
    m_forward.clear();
  }
  if (!m_dir.isEmpty())
    m_watcher->removePath(m_dir);
  m_dir = d;
  m_watcher->addPath(d);
  applyFilter();  // lists the folder
  m_path->setText(d.endsWith(QLatin1Char('/')) ? d : d + QLatin1Char('/'));
  m_path->setProperty("bad", false);
  m_path->style()->unpolish(m_path);
  m_path->style()->polish(m_path);
  m_tree->scrollToTop();
  m_thumbs->scrollToTop();
  litPlaces();
  updateState();
}

void FileDialog::applyFilter() {
  const QStringList names = [this] {
    QStringList n;
    for (int r: selectedRows())
      n << m_model->entry(r).name;
    return n;
  }();
  const bool filterOn = m_filterBtn->isChecked();
  m_model->setOptions(filterOn ? activeFilter().patterns : QStringList(), m_mode == Mode::Directory,
                      config()->getRaw(kHidden, false).toBool(), m_search->text());
  m_model->list(m_dir);
  for (const QString &n: names)
    if (const int r = m_model->rowOf(n); r >= 0)
      m_tree->selectionModel()->select(m_model->index(r, 0),
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
  updateState();
}

void FileDialog::refresh() {
  const QModelIndex cur = m_tree->selectionModel()->currentIndex();
  const QString curName = cur.isValid() ? m_model->entry(cur.row()).name : QString();
  applyFilter();
  if (const int r = m_model->rowOf(curName); r >= 0)
    m_tree->selectionModel()->setCurrentIndex(m_model->index(r, 0), QItemSelectionModel::NoUpdate);
}

void FileDialog::goBack() {
  while (!m_back.isEmpty()) {
    const QString d = m_back.takeLast();
    if (QFileInfo(d).isDir()) {
      m_forward << m_dir;
      navigate(d, false);
      return;
    }
  }
  updateState();
}

void FileDialog::goForward() {
  while (!m_forward.isEmpty()) {
    const QString d = m_forward.takeLast();
    if (QFileInfo(d).isDir()) {
      m_back << m_dir;
      navigate(d, false);
      return;
    }
  }
  updateState();
}

void FileDialog::goUp() {
  if (m_dir == QLatin1String("/"))
    return;
  const QString child = QFileInfo(m_dir).fileName();
  navigate(QFileInfo(m_dir).absolutePath());
  selectName(child);
}

void FileDialog::newFolder() {
  const QDir d(m_dir);
  QString name = tr("New Folder");
  for (int n = 2; d.exists(name); ++n)
    name = tr("New Folder %1").arg(n);
  if (!d.mkdir(name)) {
    QMessageBox::warning(this, windowTitle(), tr("Could not create a folder in %1").arg(m_dir));
    return;
  }
  refresh();
  selectName(name);
  const int r = m_model->rowOf(name);
  if (r >= 0)
    qobject_cast<QAbstractItemView *>(m_views->currentWidget())->edit(m_model->index(r, 0));
}

void FileDialog::setThumbnails(const bool on) {
  m_model->setThumbnails(on);
  m_views->setCurrentWidget(on ? static_cast<QWidget *>(m_thumbs) : static_cast<QWidget *>(m_tree));
  m_listBtn->setChecked(!on);
  m_thumbBtn->setChecked(on);
  config()->setRaw(kThumbs, on);
  const QModelIndexList sel = m_tree->selectionModel()->selectedRows();
  if (!sel.isEmpty())
    qobject_cast<QAbstractItemView *>(m_views->currentWidget())->scrollTo(sel.first());
}

void FileDialog::selectName(const QString &name) {
  const int r = m_model->rowOf(name);
  if (r < 0)
    return;
  const QModelIndex i = m_model->index(r, 0);
  m_tree->selectionModel()->setCurrentIndex(i, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  m_tree->scrollTo(i);
  m_thumbs->scrollTo(i);
}

QList<int> FileDialog::selectedRows() const {
  QList<int> rows;
  for (const QModelIndex &i: m_tree->selectionModel()->selectedRows(0))
    rows << i.row();
  std::sort(rows.begin(), rows.end());
  return rows;
}

const FileDialog::Filter &FileDialog::activeFilter() const {
  return m_filters[std::clamp(m_filterIndex, 0, int(m_filters.size()) - 1)];
}

void FileDialog::activate(const int row) {
  if (row < 0 || row >= m_model->rowCount())
    return;
  const FileEntry &e = m_model->entry(row);
  if (e.dir) {
    navigate(m_model->path(row));
    return;
  }
  if (m_mode == Mode::Directory)
    return;
  m_name->setText(e.name);
  tryAccept();
}

void FileDialog::selectionToName() {
  if (m_mode == Mode::Directory) {
    updateState();
    return;
  }
  QStringList files;
  for (int r: selectedRows())
    if (!m_model->entry(r).dir)
      files << m_model->entry(r).name;
  if (files.size() == 1) {
    m_name->setText(files.first());
  } else if (files.size() > 1 && m_mode == Mode::OpenMany) {
    QStringList quoted;
    for (const QString &f: files)
      quoted << QLatin1Char('"') + f + QLatin1Char('"');
    m_name->setText(quoted.join(QLatin1Char(' ')));
  }
  updateState();
}

// "untitled.glb" + → "untitled1.glb", "wall09.png" − → "wall08.png".
void FileDialog::bumpNumber(const int delta) {
  const QString t = m_name->text();
  const qsizetype dot = t.lastIndexOf(QLatin1Char('.'));
  QString base = dot > 0 ? t.left(dot) : t;
  const QString ext = dot > 0 ? t.mid(dot) : QString();
  static const QRegularExpression digits(QStringLiteral("(\\d+)$"));
  const QRegularExpressionMatch m = digits.match(base);
  if (m.hasMatch()) {
    const QString num = m.captured(1);
    const qlonglong n = std::max(0LL, num.toLongLong() + delta);
    base = base.left(m.capturedStart(1)) + QStringLiteral("%1").arg(n, int(num.size()), 10, QLatin1Char('0'));
  } else if (delta > 0) {
    base += QLatin1Char('1');
  } else {
    return;
  }
  m_name->setText(base + ext);
}

QString FileDialog::resolve(const QString &name) const {
  QString n = name.trimmed();
  if (n == QLatin1String("~") || n.startsWith(QLatin1String("~/")))
    n = homeDir() + n.mid(1);
  return QDir::cleanPath(QDir(m_dir).absoluteFilePath(n));
}

void FileDialog::updateState() {
  if (!m_accept)
    return;
  m_backBtn->setEnabled(!m_back.isEmpty());
  m_forwardBtn->setEnabled(!m_forward.isEmpty());
  m_upBtn->setEnabled(m_dir != QLatin1String("/"));
  bool ok = true;
  bool exists = false;
  if (m_mode != Mode::Directory) {
    const QString n = m_name->text().trimmed();
    ok = !n.isEmpty();
    if (m_mode == Mode::Save && ok) {
      const QFileInfo fi(resolve(n));
      exists = fi.isFile();
    }
  }
  m_accept->setEnabled(ok);
  if (m_name->property("exists").toBool() != exists) {
    m_name->setProperty("exists", exists);
    m_name->setToolTip(exists ? tr("This file exists and will be replaced") : QString());
    m_name->style()->unpolish(m_name);
    m_name->style()->polish(m_name);
  }
}

void FileDialog::tryAccept() {
  QStringList out;
  if (m_mode == Mode::Directory) {
    const QList<int> rows = selectedRows();
    out << (rows.size() == 1 && m_model->entry(rows.first()).dir ? cleanDir(m_model->path(rows.first())) : m_dir);
  } else {
    const QString text = m_name->text().trimmed();
    if (text.isEmpty())
      return;
    QStringList names;
    if (m_mode == Mode::OpenMany && text.contains(QLatin1Char('"'))) {
      static const QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
      for (auto it = quoted.globalMatch(text); it.hasNext();)
        names << it.next().captured(1);
    } else {
      names << text;
    }
    if (names.size() == 1 && QFileInfo(resolve(names.first())).isDir()) {
      navigate(resolve(names.first()));
      m_name->clear();
      return;
    }
    for (const QString &n: names) {
      QString p = resolve(n);
      if (m_mode == Mode::Save) {
        const Filter &f = activeFilter();
        const QString file = QFileInfo(p).fileName();
        const bool matches = f.patterns.isEmpty() ||
                             std::any_of(f.patterns.begin(), f.patterns.end(), [&](const QString &pat) {
                               return wildcard(pat, true).match(file).hasMatch();
                             });
        static const QRegularExpression simple(QStringLiteral("^\\*\\.([A-Za-z0-9_]+)$"));
        if (!matches && !f.patterns.isEmpty())
          if (const auto m = simple.match(f.patterns.first()); m.hasMatch())
            p += QLatin1Char('.') + m.captured(1);
        if (!QFileInfo(QFileInfo(p).absolutePath()).isDir()) {
          QMessageBox::warning(this, windowTitle(), tr("The folder %1 does not exist.").arg(QFileInfo(p).absolutePath()));
          return;
        }
        if (QFileInfo(p).isFile() &&
            QMessageBox::question(this, windowTitle(), tr("%1 already exists. Replace it?").arg(QFileInfo(p).fileName()),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
          return;
      } else if (!QFileInfo(p).isFile()) {
        m_name->setFocus();
        m_name->selectAll();
        return;
      }
      out << p;
    }
  }
  if (out.isEmpty())
    return;
  m_result = out;
  const QString dir = m_mode == Mode::Directory ? out.first() : QFileInfo(out.first()).absolutePath();
  addRecent(dir);
  config()->setRaw(kLastDir, m_mode == Mode::Directory ? m_dir : dir);
  accept();
}

void FileDialog::showListMenu(const QPoint &pos, QWidget *view) {
  auto *v = qobject_cast<QAbstractItemView *>(view);
  const QModelIndex idx = v->indexAt(pos);
  QMenu menu(this);
  if (idx.isValid()) {
    const FileEntry &e = m_model->entry(idx.row());
    const QString path = m_model->path(idx.row());
    if (e.dir) {
      menu.addAction(ActionManager::icon(QStringLiteral("file_folder")), tr("Open"), this,
                     [this, path] { navigate(path); });
      menu.addAction(ActionManager::icon(QStringLiteral("bookmarks")), tr("Add Bookmark"), this,
                     [this, path] { addBookmark(cleanDir(path)); });
    }
    menu.addAction(tr("Rename"), QKeySequence(Qt::Key_F2), this,
                   [v, idx] { v->edit(idx.siblingAtColumn(0)); });
    menu.addSeparator();
  }
  menu.addAction(ActionManager::icon(QStringLiteral("newfolder")), tr("New Folder"), this, &FileDialog::newFolder);
  menu.addAction(ActionManager::icon(QStringLiteral("bookmarks")), tr("Bookmark This Folder"), this,
                 [this] { addBookmark(m_dir); });
  menu.addAction(ActionManager::icon(QStringLiteral("filebrowser")), tr("Open in File Manager"), this,
                 [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_dir)); });
  menu.exec(v->viewport()->mapToGlobal(pos));
}

bool FileDialog::eventFilter(QObject *o, QEvent *e) {
  if (e->type() == QEvent::KeyPress) {
    auto *k = static_cast<QKeyEvent *>(e);
    if (o == m_search && k->key() == Qt::Key_Escape && !m_search->text().isEmpty()) {
      m_search->clear();
      return true;
    }
    if (o == m_tree || o == m_thumbs) {
      if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
        const QList<int> rows = selectedRows();
        if (rows.size() == 1 && m_model->entry(rows.first()).dir && !(k->modifiers() & Qt::ControlModifier))
          activate(rows.first());
        else
          tryAccept();
        return true;
      }
      if (k->key() == Qt::Key_Backspace) {
        goUp();
        return true;
      }
    }
  }
  return QDialog::eventFilter(o, e);
}

void FileDialog::keyPressEvent(QKeyEvent *e) {
  // Enter belongs to each field, never to QDialog's default button.
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
    e->accept();
    return;
  }
  QDialog::keyPressEvent(e);
}

void FileDialog::done(const int r) {
  config()->setRaw(kGeometry, saveGeometry());
  config()->setRaw(kSplitter, m_split->saveState());
  QDialog::done(r);
}

} // namespace ui
