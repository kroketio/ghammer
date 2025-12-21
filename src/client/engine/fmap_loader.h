#pragma once

#include "engine/map_loader.h"

namespace eng {

// Native binary map format. Read and write; the writer lives in fmap_saver.
class FmapMapLoader final : public MapLoader {
public:
  [[nodiscard]] QString formatName() const override { return QStringLiteral("gHammer map"); }
  [[nodiscard]] QStringList nameFilters() const override { return {QStringLiteral("*.ghmap")}; }
  [[nodiscard]] bool accepts(const QString &path) const override;
  [[nodiscard]] std::unique_ptr<MapLoader> clone() const override {
    return std::make_unique<FmapMapLoader>(*this);
  }
  bool load(const QString &path, MapLoadResult &out, QString *error) override;
};

} // namespace eng
