#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>

namespace ConfigKeys {
  Q_NAMESPACE
  enum ConfigKey {
    Test,
    CacheDir,
    PathDirectoryTexture,
    MainWindowTextureThumbDisplaySize,
    MainWindowTextureDisplayCount,
    TextureMetadata,
    CameraRenderDistance,
    LightRenderDistance,
    ShadowRenderDistance,
    RecentMaps,
    FirstRun,
  };
  Q_ENUM_NS(ConfigKey)
} // namespace ConfigKeys

class Config : public QObject {
  Q_OBJECT

public:
  Q_DISABLE_COPY(Config)

  ~Config() override;
  Q_INVOKABLE QVariant get(unsigned int configKey);
  QVariant get(ConfigKeys::ConfigKey key);
  QString getFileName();
  void set(ConfigKeys::ConfigKey key, const QVariant &value);
  void sync();
  void resetToDefaults();

  // Free-form keys for the editor's preference registry, which owns its own
  // names and defaults rather than growing the ConfigKey enum.
  QVariant getRaw(const QString &name, const QVariant &defaultValue = {}) const;
  void setRaw(const QString &name, const QVariant &value);
  void removeRaw(const QString &name);

  static Config *instance();

signals:
  void changed(ConfigKeys::ConfigKey key);

private:
  Config(const QString &fileName, QObject *parent = nullptr);
  explicit Config(QObject *parent);
  void init(const QString &configFileName);
  void scheduleSync();
  QTimer *m_syncTimer = nullptr;

  static QPointer<Config> m_instance;

  QScopedPointer<QSettings> m_settings;
  QHash<QString, QVariant> m_defaults;
};

inline Config *config() { return Config::instance(); }
