#pragma once

#include <memory>

#include <QString>
#include <QStringList>

#include "godot_project.h"
#include "project_config.h"

namespace proj {

// A level editor project: one folder, by default ~/GodotProjects/<name>:
//
//   tools/  sounds/  music/  images/  notes/  models/   the user's own files
//   levels/                                             the .ghmap levels
//   game/                                               the Godot project
//   .ghammer/settings.json                              our project settings
//   .ghammer/workspace.json                             open tabs, panel state
//
// A folder is a project when it has .ghammer/settings.json.
class Project {
public:
  // Folders made in a new project, in this order.
  static const QStringList &standardFolders();
  // ~/GodotProjects on every platform.
  static QString standardLocation();

  // Why `name` cannot become a new project in `location`, or empty. `existingProject`: the folder is one already.
  static QString validateName(const QString &location, const QString &name, bool *existingProject = nullptr);

  // Makes <location>/<name> with the standard folders, our settings and a Godot project in game/.
  // Null (and `error`) on failure.
  static std::unique_ptr<Project> create(const QString &location, const QString &name, QString *error = nullptr,
                                         const NewGodotSettings &godot = {});
  // Null (and `error`) when `dir` is not a project.
  static std::unique_ptr<Project> open(const QString &dir, QString *error = nullptr);

  static bool isProjectDir(const QString &dir);
  // The projects directly in `dir`, in natural name order.
  static QStringList findProjects(const QString &dir);
  // The project folder `path` is in (walking up), or empty.
  static QString findEnclosing(const QString &path);

  [[nodiscard]] QString root() const { return m_root; }
  // The display name: settings.json's name, else the folder's.
  [[nodiscard]] QString name() const;
  [[nodiscard]] QString path(const QString &rel) const;
  [[nodiscard]] QString levelsDir() const { return path(QStringLiteral("levels")); }
  [[nodiscard]] QString gameDir() const { return path(QStringLiteral("game")); }
  [[nodiscard]] QString metaDir() const { return path(QStringLiteral(".ghammer")); }
  [[nodiscard]] QString workspaceFile() const { return path(QStringLiteral(".ghammer/workspace.json")); }
  // Relative to the root when inside it, else as given.
  [[nodiscard]] QString relative(const QString &abs) const;
  [[nodiscard]] bool contains(const QString &abs) const;

  ProjectConfig &settings() { return *m_settings; }
  [[nodiscard]] const ProjectConfig &settings() const { return *m_settings; }

private:
  explicit Project(QString root);

  QString m_root;
  std::unique_ptr<ProjectConfig> m_settings;
};

} // namespace proj
