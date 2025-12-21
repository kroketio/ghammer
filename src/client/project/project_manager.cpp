#include "project_manager.h"

#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "client/prefs/preferences.h"
#include "shared/lib/config.h"

namespace proj {

namespace {

const QString kRecent = QStringLiteral("Projects/Recent");
const QString kLast = QStringLiteral("Projects/Last");
constexpr int kRecentMax = 10;

class ProjectStore final : public prefs::Store {
public:
  explicit ProjectStore(ProjectConfig &c) : m_c(c) {}
  QVariant getRaw(const QString &name, const QVariant &defaultValue) const override {
    return m_c.getRaw(name, defaultValue);
  }
  void setRaw(const QString &name, const QVariant &value) override { m_c.setRaw(name, value); }
  void removeRaw(const QString &name) override { m_c.removeRaw(name); }

private:
  ProjectConfig &m_c;
};

QString nameOf(const QString &root) {
  QFile f(QDir(root).filePath(QStringLiteral(".ghammer/settings.json")));
  if (f.open(QIODevice::ReadOnly)) {
    const QString n = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("name")).toString();
    if (!n.trimmed().isEmpty())
      return n;
  }
  return QFileInfo(root).fileName();
}

} // namespace

ProjectManager &ProjectManager::instance() {
  static ProjectManager *m = new ProjectManager();
  return *m;
}

ProjectManager::ProjectManager() : QObject(nullptr) {}

ProjectManager::~ProjectManager() {
  close();
}

bool ProjectManager::open(const QString &dir, QString *error) {
  std::unique_ptr<Project> p = Project::open(dir, error);
  if (!p)
    return false;
  close();
  m_project = std::move(p);
  m_store = std::make_unique<ProjectStore>(m_project->settings());
  prefs::get().setStore(prefs::Scope::Project, m_store.get(), m_project->name());
  remember(*m_project);
  qInfo().noquote() << QStringLiteral("[project] opened %1 (%2)")
                           .arg(m_project->name(), QDir::toNativeSeparators(m_project->root()));
  emit projectChanged();
  return true;
}

NewGodotSettings newGodotSettings() {
  NewGodotSettings s;
  // A share of the screen's resolution in pixels, even sizes.
  if (const QScreen *screen = QGuiApplication::primaryScreen()) {
    const QSize px = screen->size() * screen->devicePixelRatio();
    const double share = std::clamp(prefs::i("Godot/NewProjectResolution"), 10, 100) / 100.0;
    s.viewportWidth = int(std::lround(px.width() * share / 2.0)) * 2;
    s.viewportHeight = int(std::lround(px.height() * share / 2.0)) * 2;
  }
  s.maxFps = std::max(0, prefs::i("Godot/NewProjectMaxFps"));
  return s;
}

QString ProjectManager::createProject(const QString &location, const QString &name, QString *error) {
  std::unique_ptr<Project> p = Project::create(location, name, error, newGodotSettings());
  if (!p)
    return {};
  // Carry over the old application setting.
  if (const QVariant addon = config()->getRaw(QStringLiteral("Godot/WriteAddon")); addon.isValid())
    p->settings().setRaw(QStringLiteral("Export/WriteAddon"), addon.toBool());
  p->settings().sync();
  return p->root();
}

bool ProjectManager::create(const QString &location, const QString &name, QString *error) {
  const QString root = createProject(location, name, error);
  return !root.isEmpty() && open(root, error);
}

void ProjectManager::close() {
  if (!m_project)
    return;
  emit aboutToClose();
  prefs::get().setStore(prefs::Scope::Project, nullptr);
  m_project->settings().sync();
  m_store.reset();
  m_project.reset();
  emit projectChanged();
}

void ProjectManager::remember(const Project &p) {
  if (!m_remember)
    return;
  QVariantList list = config()->getRaw(kRecent).toList();
  for (int i = int(list.size()) - 1; i >= 0; --i)
    if (QDir::cleanPath(list[i].toMap().value(QStringLiteral("root")).toString()) == p.root())
      list.removeAt(i);
  QVariantMap entry;
  entry.insert(QStringLiteral("root"), p.root());
  entry.insert(QStringLiteral("opened"), QDateTime::currentDateTime().toString(Qt::ISODate));
  list.prepend(entry);
  while (list.size() > kRecentMax)
    list.removeLast();
  config()->setRaw(kRecent, list);
  config()->setRaw(kLast, p.root());
}

QVector<ProjectManager::Recent> ProjectManager::recent() const {
  QVector<Recent> out;
  for (const QVariant &v: config()->getRaw(kRecent).toList()) {
    const QVariantMap m = v.toMap();
    Recent r;
    r.root = QDir::cleanPath(m.value(QStringLiteral("root")).toString());
    if (r.root.isEmpty() || std::any_of(out.begin(), out.end(), [&r](const Recent &x) { return x.root == r.root; }))
      continue;
    r.opened = QDateTime::fromString(m.value(QStringLiteral("opened")).toString(), Qt::ISODate);
    r.exists = Project::isProjectDir(r.root);
    r.name = r.exists ? nameOf(r.root) : QFileInfo(r.root).fileName();
    out.push_back(r);
  }
  return out;
}

QVector<ProjectManager::Recent> ProjectManager::projectList() const {
  QVector<Recent> out = recent();
  // The same folder by any spelling (a symlinked ~/GodotProjects) is one entry.
  const auto canonical = [](const QString &p) {
    const QString c = QFileInfo(p).canonicalFilePath();
    return c.isEmpty() ? QDir::cleanPath(p) : c;
  };
  QSet<QString> listed;
  for (const Recent &r: out)
    listed.insert(canonical(r.root));
  for (const QString &root: Project::findProjects(Project::standardLocation())) {
    if (listed.contains(canonical(root)))
      continue;
    listed.insert(canonical(root));
    Recent r;
    r.root = root;
    r.name = nameOf(root);
    r.recent = false;
    out.push_back(r);
  }
  return out;
}

void ProjectManager::forget(const QString &root) {
  QVariantList list = config()->getRaw(kRecent).toList();
  const QString clean = QDir::cleanPath(root);
  for (int i = int(list.size()) - 1; i >= 0; --i)
    if (QDir::cleanPath(list[i].toMap().value(QStringLiteral("root")).toString()) == clean)
      list.removeAt(i);
  config()->setRaw(kRecent, list);
  if (QDir::cleanPath(config()->getRaw(kLast).toString()) == clean)
    config()->removeRaw(kLast);
}

QString ProjectManager::lastProject() const {
  const QString last = config()->getRaw(kLast).toString();
  return Project::isProjectDir(last) ? QDir::cleanPath(last) : QString();
}

} // namespace proj
