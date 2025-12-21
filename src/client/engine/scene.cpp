#include "scene.h"
#include "brush.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QSharedPointer>

#include "client/lib/globals.h"
#include "client/vulkan/vk_shared.h"
#include "client/vulkan/vk_common.h"
#include "client/lib/vmfpp/vmfpp.h"

namespace eng {

// Source's Z-up only exists at import (vk::sourceToYUp()); the unit scale is kept separate from the axis handling.
static constexpr float kSourceUnitsPerWorldUnit = 10.0f;

static QVector3D importVertex(float x, float y, float z) {
  const glm::vec3 w = vk::sourceToYUp(glm::vec3(x, y, z)) / kSourceUnitsPerWorldUnit;
  return QVector3D(w.x, w.y, w.z);
}

static std::array<float, 5> importUvAxis(float x, float y, float z, float offset, float scale) {
  const glm::vec3 d = vk::sourceToYUp(glm::vec3(x, y, z));
  return {d.x, d.y, d.z, offset, scale};
}

// VMF side uaxis/vaxis -> world texture basis. VMF: U = dot(p_src, uAxis) / (texW * scale) + shift / texW (tiles).
// World = source / 10, so the repeat rate picks up the unit scale. Axes arrive rotated by importUvAxis and are
// normalised: direction in the axes, magnitude in tilesPerUnit.
static eng::FaceTexBasis makeTexBasis(const Side &side, const eng::MaterialPtr &material) {
  auto unitAxis = [](float x, float y, float z) {
    const glm::vec3 v(x, y, z);
    const float len = glm::length(v);
    return len > 1e-6f ? v / len : glm::vec3(0.0f);
  };

  eng::FaceTexBasis b;
  b.uAxis = unitAxis(side.uaxis[0], side.uaxis[1], side.uaxis[2]);
  b.vAxis = unitAxis(side.vaxis[0], side.vaxis[1], side.vaxis[2]);

  const double texW = material ? material->texWidth : 512;
  const double texH = material ? material->texHeight : 512;
  double us = side.uaxis[4];
  double vs = side.vaxis[4];
  if (std::fabs(us) < 1e-6)
    us = 1.0;
  if (std::fabs(vs) < 1e-6)
    vs = 1.0;

  b.tilesPerUnitU = static_cast<float>(kSourceUnitsPerWorldUnit / (texW * us));
  b.tilesPerUnitV = static_cast<float>(kSourceUnitsPerWorldUnit / (texH * vs));
  b.offsetU = static_cast<float>(side.uaxis[3] / texW);
  b.offsetV = static_cast<float>(side.vaxis[3] / texH);
  b.valid = true;
  return b;
}

// Plain ASCII: a straight scan is an order of magnitude faster than sscanf.
static int parseIntFast(std::string_view s, const int fallback = 0) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  long value = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    value = value * 10 + (s[i] - '0');
    ++i;
    if (value > (1L << 30))
      break;
  }
  return neg ? -static_cast<int>(value) : static_cast<int>(value);
}

// The five numbers of "[ x y z shift ] scale", ignoring the brackets.
static bool parseFiveFloats(std::string_view s, float *out) {
  const char *p = s.data();
  const char *e = s.data() + s.size();
  int n = 0;
  while (p < e && n < 5) {
    const char c = *p;
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
      char *end = nullptr;
      out[n++] = std::strtof(p, &end);
      p = end;
    } else {
      ++p;
    }
  }
  return n == 5;
}

// "(x y z) (x y z) (x y z)" -> 9 floats.
static bool parsePlanePoints(const std::string &text, float out[9]) {
  int n = 0;
  const char *c = text.c_str();
  while (*c && n < 9) {
    if (*c == '(' || *c == ')' || *c == ' ' || *c == '\t') {
      ++c;
      continue;
    }
    char *end = nullptr;
    out[n++] = std::strtof(c, &end);
    if (end == c)
      return false;
    c = end;
  }
  return n == 9;
}

// Plane point order fixes the normal's sign; the import axis swap can mirror it, so if the planes do not close
// a solid one way round they are tried the other way.
static std::shared_ptr<GameObject> solidFromPlanes(const std::vector<Side> &sides) {
  std::vector<BrushFace> faces;
  for (const Side &sd: sides) {
    if (!sd.hasPlane)
      return nullptr;
    const glm::dvec3 p0(sd.planePoints[0].x(), sd.planePoints[0].y(), sd.planePoints[0].z());
    const glm::dvec3 p1(sd.planePoints[1].x(), sd.planePoints[1].y(), sd.planePoints[1].z());
    const glm::dvec3 p2(sd.planePoints[2].x(), sd.planePoints[2].y(), sd.planePoints[2].z());
    glm::dvec3 n = glm::cross(p0 - p1, p2 - p1);
    const double len = glm::length(n);
    if (len < 1e-12)
      continue;
    n /= len;
    BrushFace f;
    f.normal = n;
    f.distance = glm::dot(n, p1);
    f.material = g::textureManager->materialFor(QString::fromStdString(sd.material));
    f.basis = makeTexBasis(sd, f.material);
    faces.push_back(f);
  }
  auto brush = ConvexBrush::fromPlanes(faces);
  if (!brush) {
    for (auto &f: faces) {
      f.normal = -f.normal;
      f.distance = -f.distance;
    }
    brush = ConvexBrush::fromPlanes(faces);
  }
  if (!brush)
    return nullptr;
  auto mesh = brush->toMesh();
  auto obj = std::make_shared<GameObject>(mesh);
  for (int f = 0; f < mesh->faceCount(); ++f)
    mesh->setFacePickingID(f, makePickingId(obj->stableId(), uint32_t(f)));
  return obj;
}

Scene::Scene(QObject *parent) : QObject(parent) {
  setObjectName(QStringLiteral("Untitled Scene"));
  auto *sun = new DirectionalLight();
  sun->setParent(this);
  m_directionalLights.push_back(sun);
}

void Scene::addObject(const std::shared_ptr<GameObject> &object) {
  if (!object)
    return;
  gameObjects.push_back(object);
  ++m_structureVersion;
  emit structureChanged();
}

void Scene::addObjects(const std::vector<std::shared_ptr<GameObject>> &objects) {
  if (objects.empty())
    return;
  gameObjects.reserve(gameObjects.size() + objects.size());
  for (const auto &object: objects) {
    if (object)
      gameObjects.push_back(object);
  }
  ++m_structureVersion;
  emit structureChanged();
}

void Scene::rebuildIdIndex() const {
  m_idToSlot.clear();
  m_idToSlot.reserve(qsizetype(gameObjects.size()) * 2);
  for (size_t i = 0; i < gameObjects.size(); ++i)
    m_idToSlot.insert(gameObjects[i]->stableId(), int(i));
  m_idIndexVersion = m_structureVersion;
}

int Scene::indexOf(const quint32 stableId) const {
  if (m_idIndexVersion != m_structureVersion)
    rebuildIdIndex();
  const auto it = m_idToSlot.constFind(stableId);
  return it == m_idToSlot.constEnd() ? -1 : int(it.value());
}

std::shared_ptr<GameObject> Scene::findById(const quint32 stableId) const {
  const int slot = indexOf(stableId);
  return slot < 0 ? nullptr : gameObjects[size_t(slot)];
}

void Scene::removeObjects(const std::set<quint32> &stableIds) {
  if (stableIds.empty())
    return;

  const size_t before = gameObjects.size();
  std::vector<std::shared_ptr<GameObject>> kept;
  kept.reserve(before);
  for (size_t i = 0; i < before; ++i) {
    if (stableIds.count(gameObjects[i]->stableId()) != 0)
      continue;
    kept.push_back(std::move(gameObjects[i]));
  }
  if (kept.size() == before)
    return;
  gameObjects = std::move(kept);

  ++m_structureVersion;
  emit structureChanged();
}

void Scene::insertObjects(const std::vector<std::shared_ptr<GameObject>> &objects,
                         const std::vector<int> &slotList) {
  const size_t count = std::min(objects.size(), slotList.size());
  if (count == 0)
    return;

  for (size_t i = 0; i < count; ++i) {
    if (!objects[i])
      continue;
    const size_t at = std::clamp<size_t>(slotList[i] < 0 ? 0 : static_cast<size_t>(slotList[i]),
                                       0, gameObjects.size());
    gameObjects.insert(gameObjects.begin() + static_cast<long>(at), objects[i]);
  }

  ++m_structureVersion;
  emit structureChanged();
}

QSharedPointer<Scene> Scene::VMF(const std::filesystem::path &path) {
  // No cache: reopening must show the file, not the edited copy in memory.

  auto scene = QSharedPointer<Scene>(new Scene(nullptr));
  std::vector<std::shared_ptr<GameObject>> imported;

  QElapsedTimer prof;
  prof.start();
  auto vmf = vmfpp::VMF::openFile(path.string());
  const qint64 parseNs = prof.nsecsElapsed();
  if (!vmf.has_value()) {
    qWarning() << "VMF open failed; faulty VMF?";
    return scene;
  }

  auto *node_ptr = reinterpret_cast<vmfpp::Node *>(&*vmf);

  for (const auto &[fst, snd]: node_ptr->getChildren()) {
    // Only brush geometry is imported.
    if (fst != "world" || snd.empty())
      continue;

    const auto *solidNodes = snd[0].findChild("solid");
    if (!solidNodes)
      continue;

    for (const auto &solid: *solidNodes) {
      const auto *sideNodes = solid.findChild("side");
      if (!sideNodes)
        continue;

      std::vector<Side> cubeSides;
      cubeSides.reserve(8);

      for (const auto &side: *sideNodes) {
        Side side_struct;

        const auto *rotationVals = side.findValue("rotation");
        const auto *idVals = side.findValue("id");
        const auto *materialVals = side.findValue("material");
        const auto *vaxisVals = side.findValue("vaxis");
        const auto *uaxisVals = side.findValue("uaxis");
        if (!rotationVals || rotationVals->empty() || !idVals || idVals->empty() || !materialVals
            || materialVals->empty() || !vaxisVals || vaxisVals->empty() || !uaxisVals || uaxisVals->empty())
          continue;

        side_struct.rotation = parseIntFast((*rotationVals)[0]);
        side_struct.id = parseIntFast((*idVals)[0]);

        std::string_view material = (*materialVals)[0];
        const size_t slash = material.find('/');
        if (slash != std::string_view::npos)
          material = material.substr(slash + 1);

        bool all_upper = true;
        for (const char c: material) {
          if (std::isalpha(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(c))) {
            all_upper = false;
            break;
          }
        }
        side_struct.material.assign(material.data(), material.size());
        if (all_upper) {
          for (char &c: side_struct.material)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        float nums[5];
        if (!parseFiveFloats((*vaxisVals)[0], nums)) {
          qWarning() << "vaxis parse error:" << (*vaxisVals)[0].data();
          continue;
        }
        side_struct.vaxis = importUvAxis(nums[0], nums[1], nums[2], nums[3], nums[4]);
        if (!parseFiveFloats((*uaxisVals)[0], nums)) {
          qWarning() << "uaxis parse error:" << (*uaxisVals)[0].data();
          continue;
        }
        side_struct.uaxis = importUvAxis(nums[0], nums[1], nums[2], nums[3], nums[4]);

        if (const auto *vp = side.findChild("vertices_plus"); vp && !vp->empty()) {
          const auto *vert = vp->at(0).findValue("v");
          if (vert && vert->size() >= 4) {
            for (size_t i = 0; i < 4; ++i) {
              const char *cstr = (*vert)[i].c_str();
              char *end = nullptr;
              const float x = std::strtof(cstr, &end);
              cstr = end;
              const float y = std::strtof(cstr, &end);
              cstr = end;
              const float z = std::strtof(cstr, &end);
              side_struct.vertices_plus[i] = importVertex(x, y, z);
            }
            side_struct.hasVerticesPlus = true;
          }
        }

        if (const auto *planeVals = side.findValue("plane"); planeVals && !planeVals->empty()) {
          float p[9];
          if (parsePlanePoints((*planeVals)[0], p)) {
            for (int i = 0; i < 3; ++i)
              side_struct.planePoints[size_t(i)] = importVertex(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
            side_struct.hasPlane = true;
          }
        }

        cubeSides.push_back(std::move(side_struct));
      }

      const bool fastPath = cubeSides.size() == 6
          && std::all_of(cubeSides.begin(), cubeSides.end(), [](const Side &sd) { return sd.hasVerticesPlus; });
      if (!fastPath) {
        // Any side count, or no Hammer++ vertices_plus: rebuild the solid from its planes.
        if (auto obj = solidFromPlanes(cubeSides)) {
          imported.push_back(obj);
        } else {
          const auto *solidIdVals = solid.findValue("id");
          const int solidId = solidIdVals && !solidIdVals->empty() ? parseIntFast((*solidIdVals)[0]) : -1;
          qWarning() << "Solid id" << solidId << "is not a valid convex solid; skipping";
        }
        continue;
      }

      std::array<Side, 6> six;
      std::copy_n(cubeSides.begin(), 6, six.begin());
      const auto mesh = Mesh::createFromCubeSides(six);
      const auto cube = std::make_shared<GameObject>(mesh);
      const uint32_t stableId = cube->stableId();

      for (int face = 0; face < 6; ++face) {
        const auto &side = cubeSides.at(face);
        const auto material = g::textureManager->materialFor(QString::fromStdString(side.material));
        mesh->setFacePickingID(face, makePickingId(stableId, uint32_t(face)));
        mesh->setFaceMaterial(face, material);
        mesh->setFaceTexBasis(face, makeTexBasis(side, material));
      }

      imported.push_back(cube);
    }
  }

  scene->addObjects(imported);

  for (auto &obj: scene->gameObjects)
    obj->computeWorldAABB();

  ++scene->m_structureVersion;

  qInfo().noquote() << QStringLiteral("[vmf] parse %1 ms  import %2 ms  total %3 ms  objects %4")
                        .arg(parseNs / 1e6).arg((prof.nsecsElapsed() - parseNs) / 1e6)
                        .arg(prof.elapsed()).arg(scene->gameObjects.size());
  return scene;
}

void Scene::normalizeGroups() {
  auto it = std::find_if(groups.begin(), groups.end(), [](const MeshGroup &g) { return g.id == kWorldspawnGroup; });
  if (it == groups.end()) {
    groups.insert(groups.begin(), MeshGroup {kWorldspawnGroup, QStringLiteral("worldspawn"), QColor(120, 160, 220)});
  } else if (it != groups.begin()) {
    MeshGroup w = *it;
    groups.erase(it);
    groups.insert(groups.begin(), w);
  }
  groups.front().name = QStringLiteral("worldspawn");
  for (const auto &o: gameObjects)
    if (!findGroup(o->group())) {
      MeshGroup g;
      g.id = o->group();
      g.name = QStringLiteral("group %1").arg(g.id);
      g.color = QColor::fromHsv(int(g.id * 67 % 360), 120, 220);
      groups.push_back(g);
    }
  if (!findGroup(activeGroup))
    activeGroup = kWorldspawnGroup;
}

} // namespace eng
