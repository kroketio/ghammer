// Map persistence glue: the .ghmap document <-> the editor's live state.

#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <set>
#include <cmath>

#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QLoggingCategory>

#include "vk_engine.h"
#include "vk_shared.h"
#include "vk_gridview.h"

namespace vk {

static QLoggingCategory mapStateLog("map.state", QtInfoMsg);

fmap::Document Engine3D::captureMapDocument() const {
  fmap::Document doc;

  const QString sceneName = m_scene && !m_scene->objectName().isEmpty() ? m_scene->objectName()
                                                                     : QStringLiteral("Untitled");
  doc.meta.mapName = sceneName;
  doc.meta.generator = QStringLiteral("level-editor");
  doc.meta.sourceFormat = QStringLiteral("ghmap");
  doc.meta.createdMs = QDateTime::currentMSecsSinceEpoch();
  doc.meta.modifiedMs = doc.meta.createdMs;

  if (m_scene) {
    const auto &objects = m_scene->gameObjects;
    doc.meshes.reserve(objects.size());
    doc.nodes.reserve(objects.size());

    for (size_t i = 0; i < objects.size(); ++i) {
      const auto &object = objects[i];
      const auto mesh = object->meshRef();
      if (!mesh || mesh->vertexData().empty() || object->isDerived() || isShapePreview(object->stableId()))
        continue; // props are rebuilt from their entity, not saved as meshes

      fmap::MeshRecord meshRec;
      meshRec.id = uint32_t(doc.meshes.size());
      meshRec.name = QStringLiteral("mesh_%1").arg(meshRec.id);
      meshRec.vertices = mesh->vertexData();
      meshRec.indices = mesh->indexData();

      const int faceCount = mesh->faceCount();
      meshRec.faces.reserve(size_t(faceCount));
      for (int face = 0; face < faceCount; ++face) {
        fmap::FaceRecord f;
        f.indexOffset = mesh->faceIndexStart(face);
        f.indexCount = mesh->faceIndexCount(face);
        const auto material = mesh->faceMaterialRef(face);
        // The ref as the user / server spells it ("pack/Name#variant").
        f.material = material ? (material->ref.isEmpty() ? material->name : material->ref) : QString();
        f.basis = mesh->faceTexBasis(face);
        f.pickingId = mesh->getFacePickingID(face);
        f.groups = mesh->faceGroups(face);
        meshRec.faces.push_back(f);
      }
      doc.meshes.push_back(std::move(meshRec));

      fmap::NodeRecord node;
      // The stable id, not the array position, so ids survive a save/load round trip.
      node.id = object->stableId();
      node.meshId = uint32_t(doc.meshes.size() - 1);
      node.name = object->name().isEmpty() ? QStringLiteral("brush_%1").arg(node.id) : object->name();
      node.flags = (object->hiddenFlag() ? 1u : 0u) | (object->locked() ? 2u : 0u)
                   | (object->smoothShading() ? 4u : 0u);
      node.group = object->group();
      node.brushEntity = object->brushEntity();
      node.procObject = object->procObject();
      node.mirrorLink = object->mirrorLink();
      node.mirrorSide = uint8_t(object->mirrorSide());
      {
        const glm::dquat &q = object->orientation();
        node.orientation = glm::vec4(float(q.x), float(q.y), float(q.z), float(q.w));
        node.modHost = object->modHost();
      }
      node.position = object->getPosition();
      node.rotation = object->getRotation();
      node.scale = object->getScale();
      doc.nodes.push_back(node);
    }
  }

  captureEntities(doc.entities);
  captureConnections(doc.connections);
  for (const auto &[id, pos]: m_ioGraphLayout)
    if (m_scene && m_scene->findEntity(id))
      doc.ioGraphLayout.push_back({id, pos});

  if (m_scene) {
    for (const eng::MeshGroup &g: m_scene->groups)
      doc.groups.push_back({g.id, g.name, g.color, (g.hidden ? 1u : 0u) | (g.locked ? 2u : 0u)});
    doc.activeGroup = m_scene->activeGroup;
    for (const eng::FaceGroup &g: m_scene->faceGroups.all())
      if (!g.builtin)
        doc.faceGroups.push_back({g.id, g.name, g.color, 0u});
    {
      std::set<quint32> used;
      for (const auto &o: m_scene->gameObjects)
        if (o->procObject())
          used.insert(o->procObject());
      for (const auto &p: m_scene->procObjects)
        if (used.count(p.id))
          doc.procObjects.push_back({p.id, p.type, p.name, QJsonDocument(p.params).toJson(QJsonDocument::Compact)});
    }
    {
      // Only links some brush still names.
      std::set<quint32> used;
      for (const auto &o: m_scene->gameObjects)
        if (o->mirrorLink())
          used.insert(o->mirrorLink());
      doc.mirror.lineValid = m_scene->mirrorLineValid;
      doc.mirror.axis = uint8_t(m_scene->mirrorLine.axis);
      doc.mirror.d = m_scene->mirrorLine.d;
      std::set<quint32> usedSets;
      for (const auto &l: m_scene->mirrorLinks)
        if (used.count(l.id)) {
          doc.mirror.links.push_back(
              {l.id, uint8_t(l.plane.axis), l.plane.d, uint8_t(l.mirrorTextures ? 1 : 0), l.set});
          usedSets.insert(l.set);
        }
      // Sets with a link left, plus the one new links join.
      for (const auto &st: m_scene->mirrorSets)
        if (usedSets.count(st.id) || st.id == m_scene->mirrorActiveSet)
          doc.mirror.sets.push_back({st.id, uint8_t(st.hideMirrored ? 1 : 0), st.name});
      doc.mirror.activeSet = m_scene->findMirrorSet(m_scene->mirrorActiveSet) ? m_scene->mirrorActiveSet : 0;
    }
    doc.layouts = m_layoutBlob;
    {
      // Modified objects that still have sources (output is never stored), and every path.
      std::set<quint32> used;
      for (const auto &o: m_scene->gameObjects)
        if (o->modHost() && !o->isDerived())
          used.insert(o->modHost());
      QJsonArray hosts;
      for (const auto &h: m_scene->modHosts)
        if (used.count(h.id))
          hosts.append(eng::mods::toJson(h));
      if (!hosts.isEmpty())
        doc.modifiers = QJsonDocument(hosts).toJson(QJsonDocument::Compact);
      QJsonArray paths;
      for (const auto &p: m_scene->paths)
        paths.append(eng::path::toJson(p));
      if (!paths.isEmpty())
        doc.paths = QJsonDocument(paths).toJson(QJsonDocument::Compact);
    }

    // MATL: overrides + the effective properties of every material used (offline use, the Godot export).
    QHash<QString, MaterialProps> resolved;
    for (const auto &o: m_scene->gameObjects) {
      const auto mesh = o->getMesh();
      if (!mesh)
        continue;
      for (int f = 0; f < mesh->faceCount(); ++f) {
        const auto m = mesh->faceMaterialRef(f);
        if (!m || m->kind != eng::Material::Kind::Server || !m->hasEntry())
          continue;
        const QString ref = m->ref.isEmpty() ? m->name : m->ref;
        if (!resolved.contains(ref))
          resolved.insert(ref, m->props);
      }
    }
    if (!m_scene->materialOverrides.isEmpty() || !resolved.isEmpty())
      doc.materials = QJsonDocument(m_scene->materialOverrides.toJson(resolved)).toJson(QJsonDocument::Compact);
    if (!m_scene->levelSettings.isEmpty())
      doc.levelSettings = QJsonDocument(m_scene->levelSettings).toJson(QJsonDocument::Compact);
  }

  fmap::SceneState &s = doc.scene;
  s.sun.enabled = m_shadowsEnabled;
  s.sun.direction = m_sunDirection;
  s.sun.color = m_sunColor;
  s.sun.intensity = m_sunIntensity;
  s.sun.azimuth = sunAzimuth();
  s.sun.elevation = sunElevation();

  s.ambient.color = m_ambientColor;
  s.ambient.intensity = m_ambientIntensity;

  s.fog.density = m_fogDensity;
  s.fog.color = m_fogColor;

  s.shadows.enabled = m_shadowsEnabled;
  s.shadows.bias = m_shadowBias;
  s.shadows.normalBias = m_shadowNormalBias;
  s.shadows.radius = m_shadowRadius;
  s.shadows.strength = m_shadowStrength;
  s.shadows.range = m_shadowFar;
  s.shadows.mapSize = m_shadowMapSize;

  s.ssao.enabled = m_ssaoEnabled;
  s.ssao.radius = m_ssaoRadius;
  s.ssao.intensity = m_ssaoIntensity;

  s.render.renderMode = static_cast<int32_t>(m_renderMode);
  s.render.msaaSamples = m_msaaSamples;
  s.render.exposure = m_exposure;
  s.render.wireframeThickness = m_wireframeThicknessPx;
  s.render.gridLevel = gridLevel();
  s.sky = m_skyRef;
  s.lut = m_lutRef;

  fmap::PerspectiveCameraState &cam = doc.cameras.perspective3d;
  if (m_capturingParked) {
    // A level not on screen (withDocument): the camera and views it was left with.
    if (m_levelCameraValid)
      cam = m_levelCamera;
    if (m_camera) {
      cam.fov = m_camera->getFov();
      cam.nearPlane = 0.1f;
      cam.farPlane = m_camera->getFar();
    }
    cam.speed = m_cameraSpeed;
    doc.cameras.views2d = m_levelViews;
    return doc;
  }
  if (m_camera) {
    // While previewing a Camera3D, the editor camera is the one to keep.
    const bool preview = m_previewCamera != 0;
    cam.position = preview ? m_previewSavedPos : m_camera->getPosition();
    cam.yaw = preview ? m_previewSavedYaw : m_camera->getYaw();
    cam.pitch = preview ? m_previewSavedPitch : m_camera->getPitch();
    cam.fov = preview ? m_previewSavedFov : m_camera->getFov();
    cam.nearPlane = 0.1f;
    cam.farPlane = m_camera->getFar();
  }
  cam.speed = m_cameraSpeed;

  for (const GridView *view: GridView::instances()) {
    fmap::OrthoViewState v;
    v.perspective = static_cast<int32_t>(view->perspective());
    v.centerA = view->viewCenterA();
    v.centerB = view->viewCenterB();
    v.pxPerWorldUnit = view->pixelsPerWorldUnit();
    v.gridLevel = view->gridLevel();
    v.showGrid = view->showGrid();
    doc.cameras.views2d.push_back(v);
  }

  return doc;
}

void Engine3D::applyMapDocument(const fmap::Document &doc) {
  const fmap::SceneState &s = doc.scene;

  if (glm::length(s.sun.direction) > 1e-5f)
    m_sunDirection = glm::normalize(s.sun.direction);
  m_sunColor = s.sun.color;
  m_sunIntensity = s.sun.intensity;

  m_ambientColor = s.ambient.color;
  m_ambientIntensity = s.ambient.intensity;

  m_fogColor = s.fog.color;
  setFogDensity(s.fog.density);
  setExposure(s.render.exposure);

  m_shadowsEnabled = s.shadows.enabled;
  setShadowBias(s.shadows.bias);
  setShadowNormalBias(s.shadows.normalBias);
  setShadowRadius(s.shadows.radius);
  setShadowStrength(s.shadows.strength);
  setShadowRange(s.shadows.range);
  // Shadow resolution, MSAA and wire thickness are quality preferences: the map's saved values are ignored.

  setSsaoEnabled(s.ssao.enabled);
  setSsaoRadius(s.ssao.radius);
  setSsaoIntensity(s.ssao.intensity);

  setRenderMode(s.render.renderMode);
  setGridLevel(int(s.render.gridLevel));
  setSkybox(s.sky);
  setColorLut(s.lut);

  syncSunToSceneLight();
  emit sunChanged();

  const fmap::PerspectiveCameraState &cam = doc.cameras.perspective3d;
  if (m_previewCamera) {
    // A Camera3D preview does not survive loading another map.
    m_previewCamera = 0;
    m_camera->setFOV(m_previewSavedFov);
    emit cameraPreviewChanged();
  }
  if (m_camera)
    m_camera->setPose(cam.position, cam.yaw, cam.pitch);
  // Field of view is a user preference, not a property of the map.
  setSpeed(cam.speed);

  int applied = 0;
  if (m_scene) {
    if (!doc.groups.empty()) {
      m_scene->groups.clear();
      for (const fmap::GroupRecord &g: doc.groups)
        m_scene->groups.push_back({g.id, g.name, g.color, (g.flags & 1u) != 0, (g.flags & 2u) != 0});
      // Every map starts with worldspawn active, so a forgotten active group cannot silently collect brushes.
      m_scene->activeGroup = eng::kWorldspawnGroup;
    }
    m_scene->normalizeGroups();
    {
      std::vector<eng::FaceGroup> faceGroups;
      for (const fmap::FaceGroupRecord &g: doc.faceGroups)
        faceGroups.push_back({g.id, g.name, g.color, false});
      m_scene->faceGroups.assign(faceGroups);
    }
    m_scene->procObjects.clear();
    for (const fmap::ProcObjectRecord &p: doc.procObjects)
      m_scene->procObjects.push_back({p.id, p.type, p.name, QJsonDocument::fromJson(p.params).object()});
    emit procObjectsChanged();
    m_scene->mirrorLineValid = doc.mirror.lineValid;
    m_scene->mirrorLine = {int(doc.mirror.axis), doc.mirror.d};
    m_scene->mirrorLinks.clear();
    m_scene->mirrorSets.clear();
    m_scene->mirrorActiveSet = 0;
    for (const fmap::MirrorLinkRecord &l: doc.mirror.links)
      m_scene->mirrorLinks.push_back({l.id, {int(l.axis), l.d}, (l.flags & 1u) != 0, l.set});
    for (const fmap::MirrorSetRecord &st: doc.mirror.sets)
      m_scene->mirrorSets.push_back({st.id, st.name, (st.flags & 1u) != 0});
    m_scene->mirrorActiveSet = m_scene->findMirrorSet(doc.mirror.activeSet) ? doc.mirror.activeSet : 0;
    // A brush naming a link the file does not have is not linked.
    for (const auto &o: m_scene->gameObjects)
      if (o->mirrorLink() && !m_scene->findMirrorLink(o->mirrorLink()))
        o->setMirrorLink(0, 0);
    emit mirrorChanged();
    // A source naming a host the file lacks is a plain brush again.
    m_scene->modHosts.clear();
    for (const QJsonValue &v: QJsonDocument::fromJson(doc.modifiers).array())
      m_scene->modHosts.push_back(eng::mods::hostFromJson(v.toObject()));
    m_scene->paths.clear();
    for (const QJsonValue &v: QJsonDocument::fromJson(doc.paths).array()) {
      eng::path::Path p = eng::path::fromJson(v.toObject());
      eng::path::resolveHandles(p.points, p.closed);
      m_scene->paths.push_back(std::move(p));
    }
    for (const auto &o: m_scene->gameObjects)
      if (o->modHost() && !m_scene->findModHost(o->modHost()))
        o->setModHost(0);
    ++m_scene->modEpoch;
    ++m_scene->pathEpoch;
    emit modHostsChanged();
    emit pathsChanged();
    m_layoutBlob = doc.layouts;
    m_scene->materialOverrides =
        doc.materials.isEmpty() ? eng::MaterialOverrides()
                                : eng::MaterialOverrides::fromJson(QJsonDocument::fromJson(doc.materials).object());
    vk::TextureManager::instance().setOverrides(&m_scene->materialOverrides);
    m_scene->levelSettings = QJsonDocument::fromJson(doc.levelSettings).object();
    emit levelSettingsChanged();
    emit materialOverridesChanged();
    emit meshGroupsChanged();
    emit layoutsChanged();
  }

  for (const auto &entity: doc.entities) {
    if (applyEntity(entity)) {
      ++applied;
    } else {
      qCDebug(mapStateLog).noquote()
          << QStringLiteral("[map] entity type '%1' (%2) has no editor support yet; kept in the file")
                 .arg(entity.type).arg(entity.id);
    }
  }

  applyConnections(doc.connections); // also clears the graph layout
  for (const auto &g: doc.ioGraphLayout)
    m_ioGraphLayout[g.entityId] = g.pos;

  for (const auto &v: doc.cameras.views2d) {
    for (GridView *view: GridView::instances()) {
      if (int(view->perspective()) != int(v.perspective))
        continue;
      view->restoreView(v.centerA, v.centerB, v.pxPerWorldUnit, v.showGrid);
      break;
    }
  }

  qCInfo(mapStateLog).noquote()
      << QStringLiteral("[map] applied scene state: %1 meshes, %2 nodes, %3 entities (%4 understood), "
                        "grid level %5, %6 2D views restored")
             .arg(doc.meshes.size()).arg(doc.nodes.size()).arg(doc.entities.size()).arg(applied)
             .arg(s.render.gridLevel).arg(doc.cameras.views2d.size());

  ensureWorldEnv();

  // The old history names ids from the previous map.
  clearUndoHistory();
  // Modifier output is never stored: evaluate it now, before the first frame.
  pumpModifiers(ModPump::Blocking);
}

} // namespace vk
