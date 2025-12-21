// Layout Designer output: layout documents -> brushes named "layout:<id>" (replaced on regeneration);
// the documents are saved in the LAYO chunk.

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include "vk_engine.h"
#include "vk_shared.h"

#include "client/engine/brush.h"
#include "client/engine/layout_gen.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {
QString tagFor(int id) { return QStringLiteral("layout:%1").arg(id); }

QJsonArray parseStore(const QByteArray &blob) {
  if (blob.isEmpty())
    return {};
  return QJsonDocument::fromJson(blob).object().value(QStringLiteral("layouts")).toArray();
}

// A face replaced by a regeneration, so the matching new face inherits what the user put on it.
struct OldFace {
  glm::dvec3 normal;
  double distance = 0.0;
  glm::dvec3 mn, mx;
  eng::FaceGroupSet userGroups;
  eng::MaterialPtr material;
  eng::FaceTexBasis basis;
  bool customMaterial = false;
};

void polyBounds(const std::vector<glm::dvec3> &poly, glm::dvec3 &mn, glm::dvec3 &mx) {
  mn = glm::dvec3(1e300);
  mx = glm::dvec3(-1e300);
  for (const auto &p: poly) {
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
  }
}

// Overlap area of two faces' bounds on the same plane (the normal axis ignored).
double planarOverlap(const glm::dvec3 &n, const glm::dvec3 &a0, const glm::dvec3 &a1, const glm::dvec3 &b0,
                     const glm::dvec3 &b1) {
  int skip = 0;
  for (int i = 1; i < 3; ++i)
    if (std::fabs(n[i]) > std::fabs(n[skip]))
      skip = i;
  double area = 1.0;
  for (int i = 0; i < 3; ++i) {
    if (i == skip)
      continue;
    const double e = std::min(a1[i], b1[i]) - std::max(a0[i], b0[i]);
    if (e <= 1e-4)
      return 0.0;
    area *= e;
  }
  return area;
}

QByteArray writeStore(const QJsonArray &arr) {
  QJsonObject root;
  root[QStringLiteral("version")] = 1;
  root[QStringLiteral("layouts")] = arr;
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
} // namespace

QList<QJsonObject> Engine3D::layoutDocs() const {
  QList<QJsonObject> out;
  for (const auto &v: parseStore(m_layoutBlob))
    out.push_back(v.toObject());
  return out;
}

QJsonObject Engine3D::layoutDoc(const int id) const {
  for (const auto &v: parseStore(m_layoutBlob))
    if (v.toObject().value(QStringLiteral("id")).toInt() == id)
      return v.toObject();
  return {};
}

int Engine3D::mintLayoutId() const {
  int next = 1;
  for (const auto &d: layoutDocs())
    next = std::max(next, d.value(QStringLiteral("id")).toInt() + 1);
  return next;
}

int Engine3D::layoutOfObject(const quint32 stableId) const {
  const auto obj = m_scene ? m_scene->findById(stableId) : nullptr;
  if (!obj || !obj->name().startsWith(QLatin1String("layout:")))
    return 0;
  return obj->name().mid(7).toInt();
}

void Engine3D::removeLayoutBrushes(const int id) {
  if (!m_scene)
    return;
  std::vector<quint32> old;
  for (const auto &o: m_scene->gameObjects)
    if (o->name() == tagFor(id))
      old.push_back(o->stableId());
  if (!old.empty())
    replaceObjects(old, {}, QStringLiteral("Remove Layout"));
}

int Engine3D::generateLayout(const QJsonObject &json) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  eng::layout::Doc doc = eng::layout::fromJson(json);
  if (doc.id <= 0)
    doc.id = mintLayoutId();

  // A layout in the map stays where its brushes are: the stored origin plus how far they were moved (if only moved).
  const QJsonObject prevStored = layoutDoc(doc.id);
  if (!prevStored.isEmpty()) {
    doc.origin = eng::layout::fromJson(prevStored).origin;
    const QJsonArray bmn = prevStored.value(QStringLiteral("builtMin")).toArray();
    const QJsonArray bmx = prevStored.value(QStringLiteral("builtMax")).toArray();
    const int builtCount = prevStored.value(QStringLiteral("brushes")).toInt(-1);
    glm::dvec3 cmn(1e300), cmx(-1e300);
    int count = 0;
    for (const auto &o: m_scene->gameObjects)
      if (o->name() == tagFor(doc.id)) {
        const auto [lo, hi] = o->getWorldAABB();
        cmn = glm::min(cmn, glm::dvec3(lo));
        cmx = glm::max(cmx, glm::dvec3(hi));
        ++count;
      }
    if (bmn.size() == 3 && bmx.size() == 3 && count > 0 && count == builtCount) {
      const glm::dvec3 gmn(bmn[0].toDouble(), bmn[1].toDouble(), bmn[2].toDouble());
      const glm::dvec3 gmx(bmx[0].toDouble(), bmx[1].toDouble(), bmx[2].toDouble());
      const glm::dvec3 delta = cmn - gmn;
      if (glm::length((cmx - cmn) - (gmx - gmn)) < 1e-3 && glm::length(delta) > 1e-4)
        doc.origin += glm::vec3(delta);
    }
  }
  const eng::layout::Config &cfg = doc.config;
  const auto boxes = eng::layout::generate(doc);

  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Generate \"%1\"").arg(doc.name));

  quint32 group = eng::kWorldspawnGroup;
  if (cfg.target == 1) {
    // A group of that name from an earlier generation is reused.
    bool found = false;
    for (const auto &g: m_scene->groups)
      if (g.name == cfg.groupName && g.id != eng::kWorldspawnGroup) {
        group = g.id;
        found = true;
        break;
      }
    if (!found)
      group = createMeshGroup(cfg.groupName.isEmpty() ? QStringLiteral("layout") : cfg.groupName, false, false);
  } else if (cfg.target == 2 && m_scene->findGroup(quint32(cfg.groupId))) {
    group = quint32(cfg.groupId);
  }

  // Materials by face orientation; vents use their own on every face.
  auto material = [this](const QString &name, double &tpu) {
    QString n = name;
    if (n.isEmpty())
      n = m_currentMaterial.isEmpty() ? prefs::s("Brush/DefaultMaterial") : m_currentMaterial;
    auto mat = TextureManager::instance().materialFor(n);
    const double texW = mat && mat->texWidth > 0 ? mat->texWidth : 512.0;
    tpu = kSourceUnitsPerWorldUnit / (texW * prefs::d("Brush/DefaultTextureScale"));
    return mat;
  };
  double tFloor = 0, tWall = 0, tCeil = 0, tVent = 0;
  const auto mFloor = material(cfg.matFloor, tFloor);
  const auto mWall = material(cfg.matWall, tWall);
  const auto mCeil = material(cfg.matCeiling, tCeil);
  const auto mVent = material(cfg.matVent.isEmpty() ? cfg.matWall : cfg.matVent, tVent);

  // User groups and hand-applied materials carry over to the new face on the same plane overlapping most.
  std::vector<OldFace> oldFaces;
  {
    QSet<QString> defaults;
    const eng::layout::Doc prev = eng::layout::fromJson(layoutDoc(doc.id));
    double unused = 0.0;
    for (const QString &n: {prev.config.matFloor, prev.config.matWall, prev.config.matCeiling,
                            prev.config.matVent.isEmpty() ? prev.config.matWall : prev.config.matVent})
      if (const auto m = material(n, unused))
        defaults.insert(m->name);
    for (const auto &o: m_scene->gameObjects) {
      if (o->name() != tagFor(doc.id))
        continue;
      const auto brush = eng::ConvexBrush::fromObject(*o);
      if (!brush)
        continue;
      for (size_t i = 0; i < brush->faces.size() && i < brush->polygons.size(); ++i) {
        const eng::BrushFace &f = brush->faces[i];
        OldFace of;
        of.normal = f.normal;
        of.distance = f.distance;
        polyBounds(brush->polygons[i], of.mn, of.mx);
        for (const quint32 g: f.groups)
          if (g >= eng::facegroup::kFirstUser)
            of.userGroups.push_back(g);
        of.material = f.material;
        of.basis = f.basis;
        of.customMaterial = f.material && !defaults.contains(f.material->name);
        if (!of.userGroups.empty() || of.customMaterial)
          oldFaces.push_back(std::move(of));
      }
    }
  }

  const float k = 1.0f / kSourceUnitsPerWorldUnit;
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  objs.reserve(boxes.size());
  for (const auto &b: boxes) {
    // Plan x -> world X, plan y -> world Z, plan z (up) -> world Y.
    const glm::dvec3 mn(doc.origin.x + b.mn.x * k, doc.origin.y + b.mn.z * k, doc.origin.z + b.mn.y * k);
    const glm::dvec3 mx(doc.origin.x + b.mx.x * k, doc.origin.y + b.mx.z * k, doc.origin.z + b.mx.y * k);
    eng::ConvexBrush brush = eng::ConvexBrush::box(mn, mx, mWall, tWall);
    for (size_t fi = 0; fi < brush.faces.size(); ++fi) {
      eng::BrushFace &f = brush.faces[fi];
      // World normal -> plan face slot (+x, -x, +y, -y, top, bottom).
      int slot = 0;
      if (f.normal.x > 0.5)
        slot = 0;
      else if (f.normal.x < -0.5)
        slot = 1;
      else if (f.normal.z > 0.5)
        slot = 2;
      else if (f.normal.z < -0.5)
        slot = 3;
      else if (f.normal.y > 0.5)
        slot = 4;
      else
        slot = 5;
      const eng::layout::FaceCategory cat = b.faces[size_t(slot)];
      switch (cat) {
        case eng::layout::FaceFloor:
          f.material = mFloor;
          f.basis = eng::paraxialBasis(f.normal, tFloor);
          f.groups = {eng::facegroup::kFloor};
          break;
        case eng::layout::FaceCeiling:
          f.material = mCeil;
          f.basis = eng::paraxialBasis(f.normal, tCeil);
          f.groups = {eng::facegroup::kCeiling};
          break;
        case eng::layout::FaceWall:
          f.groups = {eng::facegroup::kWall};
          break;
        case eng::layout::FaceVent:
          f.material = mVent;
          f.basis = eng::paraxialBasis(f.normal, tVent);
          f.groups = {eng::facegroup::kVent};
          break;
        case eng::layout::FaceNone:
          if (b.vent) {
            f.material = mVent;
            f.basis = eng::paraxialBasis(f.normal, tVent);
          }
          break;
      }
      if (oldFaces.empty() || fi >= brush.polygons.size())
        continue;
      glm::dvec3 fmn, fmx;
      polyBounds(brush.polygons[fi], fmn, fmx);
      const OldFace *best = nullptr;
      double bestArea = 0.0;
      for (const OldFace &of: oldFaces) {
        if (glm::dot(of.normal, f.normal) < 0.999 || std::fabs(of.distance - f.distance) > 0.01)
          continue;
        const double a = planarOverlap(f.normal, fmn, fmx, of.mn, of.mx);
        if (a > bestArea) {
          bestArea = a;
          best = &of;
        }
      }
      if (!best)
        continue;
      for (const quint32 g: best->userGroups)
        eng::facegroup::insert(f.groups, g);
      if (best->customMaterial) {
        f.material = best->material;
        f.basis = best->basis;
      }
    }
    if (auto o = objectFromBrush(brush)) {
      o->setGroup(group);
      o->setName(tagFor(doc.id));
      objs.push_back(o);
    }
  }
  std::vector<quint32> old;
  for (const auto &o: m_scene->gameObjects)
    if (o->name() == tagFor(doc.id))
      old.push_back(o->stableId());
  if (!old.empty() || !objs.empty())
    replaceObjects(old, objs, QStringLiteral("Generate Layout"));

  const QByteArray before = m_layoutBlob;
  QJsonArray arr = parseStore(m_layoutBlob);
  bool replaced = false;
  QJsonObject stored = eng::layout::toJson(doc);
  {
    glm::dvec3 gmn(1e300), gmx(-1e300);
    for (const auto &o: objs) {
      const auto [lo, hi] = o->getWorldAABB();
      gmn = glm::min(gmn, glm::dvec3(lo));
      gmx = glm::max(gmx, glm::dvec3(hi));
    }
    if (!objs.empty()) {
      stored[QStringLiteral("builtMin")] = QJsonArray {gmn.x, gmn.y, gmn.z};
      stored[QStringLiteral("builtMax")] = QJsonArray {gmx.x, gmx.y, gmx.z};
    }
    stored[QStringLiteral("brushes")] = int(objs.size());
  }
  for (int i = 0; i < arr.size(); ++i)
    if (arr[i].toObject().value(QStringLiteral("id")).toInt() == doc.id) {
      arr[i] = stored;
      replaced = true;
    }
  if (!replaced)
    arr.append(stored);
  m_layoutBlob = writeStore(arr);
  const QByteArray after = m_layoutBlob;
  if (record) {
    auto apply = [this](const QByteArray &b) {
      m_layoutBlob = b;
      emit layoutsChanged();
    };
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Store Layout"), [apply, before] { apply(before); },
                                                           [apply, after] { apply(after); }));
    m_undoStack->endMacro();
  }
  emit layoutsChanged();
  return int(objs.size());
}

eng::layout::Config Engine3D::layoutDefaults() {
  eng::layout::Config c;
  c.wall = prefs::i("Layout/Wall");
  c.floor = prefs::i("Layout/Floor");
  c.ceiling = prefs::i("Layout/Ceiling");
  c.ventWall = prefs::i("Layout/VentWall");
  c.doorWidth = prefs::i("Layout/DoorWidth");
  c.doorHeight = prefs::i("Layout/DoorHeight");
  c.corridorWidth = prefs::i("Layout/CorridorWidth");
  c.corridorHeight = prefs::i("Layout/CorridorHeight");
  c.roomHeight = prefs::i("Layout/RoomHeight");
  c.ventWidth = prefs::i("Layout/VentWidth");
  c.ventHeight = prefs::i("Layout/VentHeight");
  return c;
}

int Engine3D::createRoomFromMarquee() {
  if (!m_marquee.valid)
    return 0;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  const double k = kSourceUnitsPerWorldUnit;
  const int w = int(std::lround((mx.x - mn.x) * k));
  const int d = int(std::lround((mx.z - mn.z) * k));
  const int h = int(std::lround((mx.y - mn.y) * k));
  if (w < 1 || d < 1 || h < 1)
    return 0;

  // Plan (0, 0) at the box's corner: plan x = world X, plan y = world Z.
  eng::layout::Doc doc;
  doc.id = mintLayoutId();
  doc.name = tr("Room %1").arg(doc.id);
  doc.origin = glm::vec3(mn);
  eng::layout::RoomShape room;
  room.id = doc.nextShapeId++;
  room.rects = {QRect(0, 0, w, d)};
  room.height = h;
  room.elevation = 0;
  doc.rooms.push_back(room);
  doc.config = layoutDefaults();
  doc.config.roomHeight = h;
  if (const quint32 g = activeMeshGroup(); g != eng::kWorldspawnGroup) {
    doc.config.target = 2;
    doc.config.groupId = int(g);
  }
  const int n = generateLayout(eng::layout::toJson(doc));
  if (n > 0)
    clearMarquee();
  return n;
}

} // namespace vk
