// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
// Copyright (C) 2011 Felix Geyer <debfx@fobos.de>
// Copyright (c) 2020-2021, The Monero Project.

#include "shared/lib/config.h"
#include "shared/lib/utils.h"

#define QS QStringLiteral

struct ConfigDirective {
  QString name;
  QVariant defaultValue;
};

static const QHash<ConfigKeys::ConfigKey, ConfigDirective> configStrings = {
    {ConfigKeys::Test, {QS("Test"), "test"}},
    {ConfigKeys::PathDirectoryTexture, {QS("DirectoryTexture"), ""}},
    {ConfigKeys::CacheDir, {QS("CacheDir"), ""}},
    {ConfigKeys::MainWindowTextureThumbDisplaySize, {QS("MainWindowTextureThumbDisplaySize"), 256}},
    {ConfigKeys::MainWindowTextureDisplayCount, {QS("MainWindowTextureDisplayCount"), 250}},
    {ConfigKeys::TextureMetadata, {QS("TextureMetadata"), "[]"}},
    // Mirrors vk::Engine3D's k*RenderDistanceDefaultSource constants.
    {ConfigKeys::CameraRenderDistance, {QS("CameraRenderDistance"), 7168.0}},
    {ConfigKeys::LightRenderDistance, {QS("LightRenderDistance"), 6144.0}},
    {ConfigKeys::ShadowRenderDistance, {QS("ShadowRenderDistance"), 10240.0}},
    // Absolute paths of the maps the user opened or saved last, newest first.
    {ConfigKeys::RecentMaps, {QS("RecentMaps"), QStringList{}}},
    // Cleared once the editor has opened Install Asset Packs after the first project.
    {ConfigKeys::FirstRun, {QS("FirstRun"), true}},
};

QPointer<Config> Config::m_instance(nullptr);

Q_INVOKABLE QVariant Config::get(unsigned int key) { return this->get(ConfigKeys::ConfigKey(key)); }

QVariant Config::get(ConfigKeys::ConfigKey key) {
  auto cfg = configStrings[key];
  auto defaultValue = configStrings[key].defaultValue;

  return m_settings->value(cfg.name, defaultValue);
}

QString Config::getFileName() { return m_settings->fileName(); }

void Config::set(ConfigKeys::ConfigKey key, const QVariant &value) {
  if (get(key) == value) {
    return;
  }

  auto cfg = configStrings[key];
  m_settings->setValue(cfg.name, value);

  // Writing the whole JSON file on every slider tick stalled the UI; collect a
  // burst of edits and write once. aboutToQuit still syncs synchronously.
  scheduleSync();
  emit changed(key);
}

/**
 * Sync configuration with persistent storage.
 *
 * Usually, you don't need to call this method manually, but if you are writing
 * configurations after an emitted \link QCoreApplication::aboutToQuit() signal,
 * use it to guarantee your config values are persisted.
 */
void Config::sync() {
  if (m_syncTimer)
    m_syncTimer->stop();
  m_settings->sync();
}

void Config::scheduleSync() {
  if (!m_syncTimer) {
    m_syncTimer = new QTimer(this);
    m_syncTimer->setSingleShot(true);
    m_syncTimer->setInterval(500);
    connect(m_syncTimer, &QTimer::timeout, this, [this] { m_settings->sync(); });
  }
  m_syncTimer->start();
}

QVariant Config::getRaw(const QString &name, const QVariant &defaultValue) const {
  return m_settings->value(name, defaultValue);
}

void Config::setRaw(const QString &name, const QVariant &value) {
  if (m_settings->value(name) == value && m_settings->contains(name))
    return;
  m_settings->setValue(name, value);
  scheduleSync();
}

void Config::removeRaw(const QString &name) {
  m_settings->remove(name);
  scheduleSync();
}

void Config::resetToDefaults() { m_settings->clear(); }

Config::Config(const QString &fileName, QObject *parent) : QObject(parent) { init(fileName); }

Config::Config(QObject *parent) : QObject(parent) {
  QString configPath;

  configPath = QDir::homePath();
  configPath += QString("/.config/%1/settings.json").arg(QCoreApplication::applicationName());
  // The server's --config-dir (tests run it on a temporary copy): its settings
  // go there too, never to the real ~/.config/<app>.
  if (const QString over = qEnvironmentVariable("ASSET_SERVER_CONFIG_DIR"); !over.isEmpty())
    configPath = QDir(over).filePath(QStringLiteral("settings.json"));

  init(QDir::toNativeSeparators(configPath));
}


Config::~Config() {}

void Config::init(const QString &configFileName) {
  const QSettings::Format jsonFormat = QSettings::registerFormat("json", Utils::readJsonFile, Utils::writeJsonFile);
  QSettings::setDefaultFormat(jsonFormat);
  m_settings.reset(new QSettings(configFileName, jsonFormat));

  connect(qApp, &QCoreApplication::aboutToQuit, this, &Config::sync);
}

Config *Config::instance() {
  if (!m_instance) {
    m_instance = new Config(qApp);
  }

  return m_instance;
}
