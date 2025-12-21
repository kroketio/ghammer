#include "project_config.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace proj {

ProjectConfig::ProjectConfig(QString file) : m_file(std::move(file)) {
  m_timer.setSingleShot(true);
  m_timer.setInterval(400);
  QObject::connect(&m_timer, &QTimer::timeout, [this] { sync(); });
}

ProjectConfig::~ProjectConfig() {
  sync();
}

bool ProjectConfig::load(QString *error) {
  QFile f(m_file);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error)
      *error = QStringLiteral("Cannot read %1").arg(QDir::toNativeSeparators(m_file));
    return false;
  }
  QJsonParseError pe {};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error)
      *error = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(m_file), pe.errorString());
    return false;
  }
  QJsonObject root = doc.object();
  m_name = root.take(QStringLiteral("name")).toString();
  m_created = root.take(QStringLiteral("created")).toString();
  m_settings = root.take(QStringLiteral("settings")).toObject();
  root.remove(QStringLiteral("format"));
  m_extra = root;
  m_dirty = false;
  return true;
}

bool ProjectConfig::sync(QString *error) {
  m_timer.stop();
  if (!m_dirty)
    return true;
  QJsonObject root = m_extra;
  root.insert(QStringLiteral("format"), kFormat);
  root.insert(QStringLiteral("name"), m_name);
  if (!m_created.isEmpty())
    root.insert(QStringLiteral("created"), m_created);
  root.insert(QStringLiteral("settings"), m_settings);
  QDir().mkpath(QFileInfo(m_file).absolutePath());
  QSaveFile f(m_file);
  if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
      || !f.commit()) {
    if (error)
      *error = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(m_file));
    qWarning("[project] cannot write %s", qPrintable(m_file));
    return false;
  }
  m_dirty = false;
  return true;
}

QVariant ProjectConfig::getRaw(const QString &name, const QVariant &defaultValue) const {
  const QJsonValue v = m_settings.value(name);
  return v.isUndefined() || v.isNull() ? defaultValue : v.toVariant();
}

void ProjectConfig::setRaw(const QString &name, const QVariant &value) {
  const QJsonValue v = QJsonValue::fromVariant(value);
  if (m_settings.value(name) == v)
    return;
  m_settings.insert(name, v);
  scheduleSync();
}

void ProjectConfig::removeRaw(const QString &name) {
  if (!m_settings.contains(name))
    return;
  m_settings.remove(name);
  scheduleSync();
}

void ProjectConfig::setName(const QString &name) {
  if (name == m_name)
    return;
  m_name = name;
  scheduleSync();
}

void ProjectConfig::setCreated(const QString &iso) {
  if (iso == m_created)
    return;
  m_created = iso;
  scheduleSync();
}

void ProjectConfig::scheduleSync() {
  m_dirty = true;
  m_timer.start();
}

} // namespace proj
