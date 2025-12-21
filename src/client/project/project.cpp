#include "project.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include "godot_project.h"

namespace proj {

namespace {
const QString kMeta = QStringLiteral(".ghammer");
const QString kSettings = QStringLiteral(".ghammer/settings.json");
} // namespace

const QStringList &Project::standardFolders() {
  static const QStringList folders {
      QStringLiteral("tools"),  QStringLiteral("game"),   QStringLiteral("sounds"), QStringLiteral("music"),
      QStringLiteral("images"), QStringLiteral("notes"),  QStringLiteral("models"), QStringLiteral("levels"),
      kMeta,
  };
  return folders;
}

QString Project::standardLocation() {
  return QDir(QDir::homePath()).filePath(QStringLiteral("GodotProjects"));
}

Project::Project(QString root)
    : m_root(QDir::cleanPath(QDir(root).absolutePath())),
      m_settings(std::make_unique<ProjectConfig>(QDir(m_root).filePath(kSettings))) {}

bool Project::isProjectDir(const QString &dir) {
  return !dir.isEmpty() && QFileInfo(QDir(dir).filePath(kSettings)).isFile();
}

QStringList Project::findProjects(const QString &dir) {
  QStringList out;
  QDir d(dir);
  if (!d.exists())
    return out;
  QCollator collator;
  collator.setNumericMode(true);
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  QFileInfoList entries = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  std::sort(entries.begin(), entries.end(),
            [&collator](const QFileInfo &a, const QFileInfo &b) { return collator.compare(a.fileName(), b.fileName()) < 0; });
  for (const QFileInfo &fi: entries)
    if (isProjectDir(fi.absoluteFilePath()))
      out << QDir::cleanPath(fi.absoluteFilePath());
  return out;
}

QString Project::findEnclosing(const QString &path) {
  if (path.isEmpty())
    return {};
  QFileInfo fi(path);
  QDir d(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
  for (int i = 0; i < 64; ++i) {
    if (isProjectDir(d.absolutePath()))
      return QDir::cleanPath(d.absolutePath());
    if (!d.cdUp())
      break;
  }
  return {};
}

QString Project::validateName(const QString &location, const QString &name, bool *existingProject) {
  if (existingProject)
    *existingProject = false;
  const QString n = name.trimmed();
  if (n.isEmpty())
    return QStringLiteral("Name the project");
  if (n != name)
    return QStringLiteral("The name starts or ends with a space");
  static const QString kBad = QStringLiteral("/\\:*?\"<>|");
  for (const QChar c: n)
    if (kBad.contains(c) || c.unicode() < 32)
      return QStringLiteral("A project name cannot contain %1").arg(c.unicode() < 32 ? QStringLiteral("control characters") : QString(c));
  if (n.startsWith(QLatin1Char('.')))
    return QStringLiteral("A project name cannot start with a dot");
  if (location.trimmed().isEmpty())
    return QStringLiteral("Pick a location");
  const QString dir = QDir(location).filePath(n);
  const QFileInfo fi(dir);
  if (fi.exists()) {
    if (isProjectDir(dir)) {
      if (existingProject)
        *existingProject = true;
      return QStringLiteral("A project named ‘%1’ already exists").arg(n);
    }
    return fi.isDir() ? QStringLiteral("A folder named ‘%1’ already exists").arg(n)
                      : QStringLiteral("A file named ‘%1’ already exists").arg(n);
  }
  return {};
}

std::unique_ptr<Project> Project::create(const QString &location, const QString &name, QString *error,
                                         const NewGodotSettings &godot) {
  if (const QString why = validateName(location, name); !why.isEmpty()) {
    if (error)
      *error = why;
    return nullptr;
  }
  const QString root = QDir(location).filePath(name);
  for (const QString &f: standardFolders()) {
    if (!QDir().mkpath(QDir(root).filePath(f))) {
      if (error)
        *error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(QDir(root).filePath(f)));
      return nullptr;
    }
  }
  std::unique_ptr<Project> p(new Project(root));
  p->m_settings->setName(name);
  p->m_settings->setCreated(QDateTime::currentDateTime().toString(Qt::ISODate));
  if (!p->m_settings->sync(error))
    return nullptr;
  if (!writeNewGodotProject(p->gameDir(), name, error, godot))
    return nullptr;
  qInfo().noquote() << QStringLiteral("[project] created %1").arg(QDir::toNativeSeparators(root));
  return p;
}

std::unique_ptr<Project> Project::open(const QString &dir, QString *error) {
  if (!isProjectDir(dir)) {
    if (error)
      *error = QStringLiteral("%1 is not a project (no %2)").arg(QDir::toNativeSeparators(dir), kSettings);
    return nullptr;
  }
  std::unique_ptr<Project> p(new Project(dir));
  if (!p->m_settings->load(error))
    return nullptr;
  return p;
}

QString Project::name() const {
  const QString n = m_settings->name().trimmed();
  return n.isEmpty() ? QFileInfo(m_root).fileName() : n;
}

QString Project::path(const QString &rel) const {
  return QDir(m_root).filePath(rel);
}

bool Project::contains(const QString &abs) const {
  const QString p = QDir::cleanPath(QFileInfo(abs).absoluteFilePath());
  return p == m_root || p.startsWith(m_root + QLatin1Char('/'));
}

QString Project::relative(const QString &abs) const {
  return contains(abs) ? QDir(m_root).relativeFilePath(abs) : abs;
}

} // namespace proj
