#include "vmf_map_loader.h"

#include <QFileInfo>

#include "engine/scene.h"

namespace eng {

bool VmfMapLoader::accepts(const QString &path) const {
  return QFileInfo(path).suffix().compare(QLatin1String("vmf"), Qt::CaseInsensitive) == 0;
}

bool VmfMapLoader::load(const QString &path, MapLoadResult &out, QString *error) {
  const auto scene = Scene::VMF(std::filesystem::path(path.toStdString()));
  if (scene.isNull()) {
    if (error)
      *error = QStringLiteral("VMF importer returned no scene for '%1'").arg(path);
    return false;
  }

  out.scene = scene;
  out.hasDocument = false;
  out.formatName = formatName();
  out.doc.meta.mapName = QFileInfo(path).completeBaseName();
  out.doc.meta.sourceFormat = QStringLiteral("vmf");
  return true;
}

} // namespace eng
