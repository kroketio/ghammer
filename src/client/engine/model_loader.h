#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "mesh.h"

namespace eng {

// A model file (anything assimp reads) flattened into sub-meshes, each with its own material.
struct ModelData {
  struct SubMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    QString albedoPath;   // absolute, empty when the material has no texture
    QString normalPath;
    glm::vec3 baseColor {0.8f};
  };
  QString path;
  std::vector<SubMesh> subMeshes;
  glm::vec3 boundsMin {0.0f}, boundsMax {0.0f};
  QString error;
  [[nodiscard]] bool ok() const { return error.isEmpty() && !subMeshes.empty(); }
  [[nodiscard]] size_t triangleCount() const;
};

class ModelCache {
public:
  static ModelCache &instance();
  // `unitScale`: file units -> world units.
  std::shared_ptr<const ModelData> get(const QString &path, float unitScale);
  // Non-blocking: the model if loaded, else null and the load starts on a worker; `onReady` then runs on
  // `context`'s (GUI) thread.
  std::shared_ptr<const ModelData> request(const QString &path, float unitScale, QObject *context,
                                           std::function<void()> onReady);
  static QStringList fileFilters();

private:
  QMutex m_mutex;
  QHash<QString, std::shared_ptr<const ModelData>> m_cache;
  QSet<QString> m_loading;
};

// Each sub-mesh is a "face" whose UVs come from the vertices, not a projected basis.
std::shared_ptr<Mesh> meshFromModel(const ModelData &model);

} // namespace eng
