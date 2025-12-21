#pragma once

#include "engine/map_loader.h"

namespace eng {

// Read-only VMF importer.
class VmfMapLoader final : public MapLoader {
public:
  [[nodiscard]] QString formatName() const override { return QStringLiteral("Valve VMF"); }
  [[nodiscard]] QStringList nameFilters() const override { return {QStringLiteral("*.vmf")}; }
  [[nodiscard]] bool accepts(const QString &path) const override;
  [[nodiscard]] std::unique_ptr<MapLoader> clone() const override {
    return std::make_unique<VmfMapLoader>(*this);
  }
  bool load(const QString &path, MapLoadResult &out, QString *error) override;
};

} // namespace eng
