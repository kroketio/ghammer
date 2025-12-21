#pragma once

#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <QVariant>

namespace proj {

// The editor's own project settings, <project>/.ghammer/settings.json (never project.godot), with Config's raw
// calls; the store of prefs::Scope::Project.
//
//   {"format": 1, "name": "my_game", "created": "2026-11-19T12:00:00",
//    "settings": {"Layout/Wall": 8, ...}}
//
// Writes are debounced; sync() writes now.
class ProjectConfig {
public:
  static constexpr int kFormat = 1;

  explicit ProjectConfig(QString file);
  ~ProjectConfig();
  ProjectConfig(const ProjectConfig &) = delete;
  ProjectConfig &operator=(const ProjectConfig &) = delete;

  // False (and `error`) when the file cannot be read or parsed.
  bool load(QString *error = nullptr);
  // Writes now if anything changed since the last write.
  bool sync(QString *error = nullptr);

  [[nodiscard]] QVariant getRaw(const QString &name, const QVariant &defaultValue = {}) const;
  void setRaw(const QString &name, const QVariant &value);
  void removeRaw(const QString &name);
  [[nodiscard]] QJsonObject settings() const { return m_settings; }

  [[nodiscard]] QString name() const { return m_name; }
  void setName(const QString &name);
  [[nodiscard]] QString created() const { return m_created; }
  void setCreated(const QString &iso);
  [[nodiscard]] QString fileName() const { return m_file; }

private:
  void scheduleSync();

  QString m_file;
  QString m_name;
  QString m_created;
  QJsonObject m_settings;
  QJsonObject m_extra; // unknown top-level keys, kept as they were
  bool m_dirty = false;
  QTimer m_timer;
};

} // namespace proj
