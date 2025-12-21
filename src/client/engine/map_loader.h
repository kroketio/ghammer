#pragma once

#include <memory>

#include <QSharedPointer>
#include <QString>
#include <QStringList>

#include "engine/map_format.h"

namespace eng {

class Scene;

// A .vmf only produces a scene; .ghmap also carries render settings and camera (`hasDocument`).
struct MapLoadResult {
  QSharedPointer<Scene> scene;
  fmap::Document doc;
  bool hasDocument = false;
  QString formatName;
};

// forPath() picks the implementation from the file extension.
class MapLoader {
public:
  virtual ~MapLoader() = default;

  [[nodiscard]] virtual QString formatName() const = 0;
  [[nodiscard]] virtual QStringList nameFilters() const = 0;
  [[nodiscard]] virtual bool accepts(const QString &path) const = 0;
  [[nodiscard]] virtual std::unique_ptr<MapLoader> clone() const = 0;
  virtual bool load(const QString &path, MapLoadResult &out, QString *error) = 0;

  static std::unique_ptr<MapLoader> forPath(const QString &path);
  [[nodiscard]] static QString loadNameFilter();
  [[nodiscard]] static bool isSupported(const QString &path);
};

} // namespace eng
