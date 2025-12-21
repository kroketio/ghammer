#include "map_loader.h"

#include <QFileInfo>
#include <QLoggingCategory>

#include "engine/fmap_loader.h"
#include "engine/vmf_map_loader.h"

namespace eng {

static QLoggingCategory loaderLog("map.load", QtInfoMsg);

// Registration order is match order; .ghmap first.
static const std::vector<std::unique_ptr<MapLoader>> &registry() {
  static const std::vector<std::unique_ptr<MapLoader>> kLoaders = [] {
    std::vector<std::unique_ptr<MapLoader>> v;
    v.push_back(std::make_unique<FmapMapLoader>());
    v.push_back(std::make_unique<VmfMapLoader>());
    return v;
  }();
  return kLoaders;
}

std::unique_ptr<MapLoader> MapLoader::forPath(const QString &path) {
  for (const auto &loader: registry()) {
    if (loader->accepts(path))
      return loader->clone();
  }
  qCWarning(loaderLog).noquote()
      << QStringLiteral("[map] no loader for '%1' (supported: %2)").arg(path, loadNameFilter());
  return nullptr;
}

QString MapLoader::loadNameFilter() {
  QStringList filters;
  for (const auto &loader: registry()) {
    filters << QStringLiteral("%1 (%2)").arg(loader->formatName()).arg(loader->nameFilters().join(' '));
  }
  filters << QStringLiteral("All files (*)");
  return filters.join(QStringLiteral(";;"));
}

bool MapLoader::isSupported(const QString &path) {
  for (const auto &loader: registry()) {
    if (loader->accepts(path))
      return true;
  }
  return false;
}

} // namespace eng
