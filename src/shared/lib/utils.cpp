#include <QObject>
#include <QJsonDocument>
#include <QVector3D>

#include "shared/lib/config.h"
#include "utils.h"

#define eps (1e-4f)

bool Utils::isEqual(float a, float b) {
  return qAbs(a - b) < eps;
}

bool Utils::isEqual(QVector3D a, QVector3D b) {
  return isEqual(a[0], b[0]) && isEqual(a[1], b[1]) && isEqual(a[2], b[2]);
}

void Utils::init() {
  gs::configRoot = QDir::homePath();
  gs::homeDir = QDir::homePath();
  gs::configDirectory = QString("%1/.config/%2/").arg(gs::configRoot, QCoreApplication::applicationName());
  // Tests run the server against a temporary copy of the data, never the real one.
  if (const QString over = qEnvironmentVariable("ASSET_SERVER_CONFIG_DIR"); !over.isEmpty())
    gs::configDirectory = QDir(over).absolutePath() + QDir::separator();
  gs::configDirectoryAssets = QString("%1%2").arg(gs::configDirectory, "data");
  gs::configDirectoryAssetsTextures = QString("%1/%2").arg(gs::configDirectoryAssets, "textures");
  gs::configDirectoryAssetsModels = QString("%1/%2").arg(gs::configDirectoryAssets, "models");
  gs::cacheDirectory = QString("%1/cache").arg(gs::configDirectory);
  gs::pathDatabase = QFileInfo(gs::configDirectory + QDir::separator() + "db.sqlite3");
}

bool Utils::readJsonFile(QIODevice &device, QSettings::SettingsMap &map) {
  QJsonDocument json = QJsonDocument::fromJson(device.readAll());
  map = json.object().toVariantMap();
  return true;
}

bool Utils::writeJsonFile(QIODevice &device, const QSettings::SettingsMap &map) {
  device.write(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Indented));
  return true;
}
