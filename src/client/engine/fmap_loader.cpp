#include "fmap_loader.h"

#include <set>
#include <vector>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>

#include "client/lib/globals.h"
#include "client/vulkan/vk_shared.h"
#include "engine/mesh.h"
#include "engine/scene.h"

namespace eng {

static QLoggingCategory fmapLoaderLog("map.load", QtInfoMsg);

bool FmapMapLoader::accepts(const QString &path) const {
  return fmap::isMapPath(path);
}

bool FmapMapLoader::load(const QString &path, MapLoadResult &out, QString *error) {
  QElapsedTimer timer;
  timer.start();

  fmap::Document doc;
  if (!fmap::loadMap(path, doc, error))
    return false;

  QHash<uint32_t, const fmap::MeshRecord *> meshById;
  meshById.reserve(qsizetype(doc.meshes.size()));
  for (const auto &m: doc.meshes)
    meshById.insert(m.id, &m);

  const auto scene = QSharedPointer<Scene>(new Scene(nullptr));
  scene->setObjectName(doc.meta.mapName.isEmpty() ? QFileInfo(path).completeBaseName()
                                                : doc.meta.mapName);

  int missingMesh = 0;
  int reusedIds = 0;
  std::set<uint32_t> seenIds;
  std::vector<std::shared_ptr<GameObject>> imported;
  imported.reserve(doc.nodes.size());

  for (const auto &node: doc.nodes) {
    const auto it = meshById.constFind(node.meshId);
    if (it == meshById.constEnd()) {
      ++missingMesh;
      continue;
    }
    const fmap::MeshRecord &rec = *it.value();

    // One Mesh per node: face records carry this object's picking ids, so meshes cannot be shared.
    const auto mesh = std::make_shared<Mesh>();
    mesh->addVertices(rec.vertices);
    mesh->addIndices(rec.indices);
    {
      // Faces are index ranges (any polygon count per face); old files are all 6-index quads.
      std::vector<uint32_t> first, count;
      for (const auto &f: rec.faces) {
        first.push_back(f.indexOffset);
        count.push_back(f.indexCount);
      }
      if (!rec.faces.empty())
        mesh->setFaceRanges(std::move(first), std::move(count));
    }

    // NodeRecord.id is identity; older files can have duplicate or zero ids, so claimed ones get a fresh id.
    uint32_t stableId = node.id;
    if (stableId == 0 || seenIds.count(stableId) != 0) {
      stableId = 0;
      ++reusedIds;
    }
    const auto object = std::make_shared<GameObject>(mesh, stableId);
    seenIds.insert(object->stableId());
    GameObject::reserveIdsAbove(object->stableId());

    const int faceCount = mesh->faceCount();
    for (int face = 0; face < faceCount; ++face) {
      const fmap::FaceRecord *fr = face < int(rec.faces.size()) ? &rec.faces[face] : nullptr;
      const QString materialName = fr && !fr->material.isEmpty() ? fr->material
                                                               : QStringLiteral("devtexture01");
      const auto material = g::textureManager ? g::textureManager->materialFor(materialName)
                                             : eng::MaterialPtr {};
      mesh->setFaceMaterial(face, material);
      if (fr && fr->basis.valid)
        mesh->setFaceTexBasis(face, fr->basis);
      if (fr && !fr->groups.empty())
        mesh->setFaceGroups(face, fr->groups);
      // Derived from identity: never collides and survives deletes of other objects.
      mesh->setFacePickingID(face, makePickingId(object->stableId(), uint32_t(face)));
    }

    if (!node.name.startsWith(QLatin1String("brush_")))
      object->setName(node.name);
    object->setHidden(node.flags & 1u);
    object->setLocked(node.flags & 2u);
    if (node.flags & 4u)
      object->setSmoothShading(true);
    object->setGroup(node.group);
    object->setBrushEntity(node.brushEntity);
    object->setProcObject(node.procObject);
    object->setMirrorLink(node.mirrorLink, node.mirrorSide);
    object->setModHost(node.modHost);
    object->setOrientation(glm::normalize(
        glm::dquat(node.orientation.w, node.orientation.x, node.orientation.y, node.orientation.z)));
    object->setPosition(node.position);
    object->setRotation(node.rotation);
    object->setScale(node.scale);
    imported.push_back(object);
  }

  scene->addObjects(imported);

  for (auto &object: scene->gameObjects)
    object->computeWorldAABB();

  if (!scene->directionalLights().isEmpty()) {
    if (auto *sun = scene->directionalLights().first()) {
      sun->setDirection(QVector3D(doc.scene.sun.direction.x, doc.scene.sun.direction.y,
                                  doc.scene.sun.direction.z));
      sun->setColor(QVector3D(doc.scene.sun.color.x, doc.scene.sun.color.y, doc.scene.sun.color.z));
      sun->setIntensity(doc.scene.sun.intensity);
      sun->setEnabled(doc.scene.shadows.enabled);
    }
  }

  if (missingMesh)
    qCWarning(fmapLoaderLog).noquote()
        << QStringLiteral("[map] '%1': %2 nodes referenced a missing mesh and were skipped").arg(path)
               .arg(missingMesh);
  if (reusedIds)
    qCWarning(fmapLoaderLog).noquote()
        << QStringLiteral("[map] '%1': %2 nodes carried no usable identity and were minted new ones")
               .arg(path).arg(reusedIds);

  out.scene = scene;
  out.hasDocument = true;
  out.formatName = formatName();
  out.doc = std::move(doc);

  qCInfo(fmapLoaderLog).noquote() << QStringLiteral("[map] fmap '%1' -> %2 objects, %3 entities in %4 ms")
                                        .arg(path).arg(scene->gameObjects.size())
                                        .arg(out.doc.entities.size()).arg(timer.elapsed());
  return true;
}

} // namespace eng
