#pragma once

#include <memory>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include "project.h"

namespace prefs {
class Store;
}

namespace proj {

// From the prefs Godot/NewProjectResolution and Godot/NewProjectMaxFps.
NewGodotSettings newGodotSettings();

// The open project (at most one; its settings are the prefs::Scope::Project store) and the recent ones
// (app config Projects/Recent, newest first, and Projects/Last).
class ProjectManager final : public QObject {
  Q_OBJECT

public:
  struct Recent {
    QString root;
    QString name;      // settings.json's name, else the folder's
    QDateTime opened;  // invalid when unknown
    bool exists = true;
    bool recent = true; // false: found in ~/GodotProjects, never opened from the recents
  };

  static ProjectManager &instance();

  [[nodiscard]] Project *current() const { return m_project.get(); }
  [[nodiscard]] bool isOpen() const { return m_project != nullptr; }

  // Closes the current project first. False and `error` when `dir` is not a project.
  bool open(const QString &dir, QString *error = nullptr);
  // Creates <location>/<name> and opens it.
  bool create(const QString &location, const QString &name, QString *error = nullptr);
  // Without opening it: its root, or empty (and `error`).
  static QString createProject(const QString &location, const QString &name, QString *error = nullptr);
  void close();

  // Off for scripted runs: the recents and the startup project stay untouched.
  void setRemember(bool on) { m_remember = on; }

  [[nodiscard]] QVector<Recent> recent() const;
  // The recents, then every other project in ~/GodotProjects; each once.
  [[nodiscard]] QVector<Recent> projectList() const;
  void forget(const QString &root);
  // To reopen at startup, when it still exists.
  [[nodiscard]] QString lastProject() const;

signals:
  void projectChanged();  // after open / create / close
  void aboutToClose();

private:
  ProjectManager();
  ~ProjectManager() override;
  void remember(const Project &p);

  std::unique_ptr<Project> m_project;
  std::unique_ptr<prefs::Store> m_store;
  bool m_remember = true;
};

inline Project *currentProject() { return ProjectManager::instance().current(); }

} // namespace proj
