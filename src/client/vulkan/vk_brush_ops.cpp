// Brush geometry operations: each converts brushes to world-space ConvexBrushes, edits them and swaps the
// scene objects in one undo step.

#include <array>
#include <cfloat>
#include <map>

#include <QHash>
#include <QLoggingCategory>
#include <QSet>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "vk_engine.h"

#include <glm/gtc/matrix_inverse.hpp>
#include "vk_shared.h"

#include "client/engine/brush.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

static QLoggingCategory brushLog("vk.brush", QtInfoMsg);

namespace {

// Removes and adds objects as one step; undo restores the removed ones at their old slots, with the selection.
class StructureCommand final : public eng::UndoCommand {
public:
  StructureCommand(Engine3D *engine, QString label) : m_engine(engine), m_label(std::move(label)) {}

  std::vector<std::shared_ptr<eng::GameObject>> removed;
  std::vector<int> removedSlots;
  std::vector<std::shared_ptr<eng::GameObject>> added;
  std::vector<quint32> selBefore, selAfter;
  std::vector<quint32> facesBefore;

  void undo() override {
    auto *scene = m_engine->scene().get();
    if (!scene)
      return;
    std::set<quint32> ids;
    for (const auto &o: added)
      ids.insert(o->stableId());
    scene->removeObjects(ids);
    std::vector<size_t> order(removed.size());
    for (size_t k = 0; k < order.size(); ++k)
      order[k] = k;
    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) { return removedSlots[a] < removedSlots[b]; });
    std::vector<std::shared_ptr<eng::GameObject>> objs;
    std::vector<int> slotList;
    for (const size_t k: order) {
      objs.push_back(removed[k]);
      slotList.push_back(removedSlots[k]);
    }
    if (!objs.empty())
      scene->insertObjects(objs, slotList);
    m_engine->notifyStructureChanged();
    m_engine->setSelected(selBefore, facesBefore);
  }

  void redo() override {
    auto *scene = m_engine->scene().get();
    if (!scene)
      return;
    std::set<quint32> ids;
    for (const auto &o: removed)
      ids.insert(o->stableId());
    scene->removeObjects(ids);
    if (!added.empty())
      scene->addObjects(added);
    m_engine->notifyStructureChanged();
    m_engine->setSelected(selAfter, {});
  }

  [[nodiscard]] QString text() const override { return m_label; }
  void touched(eng::Touched &t) const override {
    t.objects.insert(t.objects.end(), removed.begin(), removed.end());
    t.objects.insert(t.objects.end(), added.begin(), added.end());
  }

private:
  Engine3D *m_engine;
  QString m_label;
};

} // namespace

std::shared_ptr<eng::GameObject> Engine3D::objectFromBrush(const eng::ConvexBrush &brush,
                                                           const eng::GameObject *metaFrom) const {
  auto mesh = brush.toMesh();
  if (!mesh || mesh->faceCount() == 0)
    return nullptr;
  auto obj = std::make_shared<eng::GameObject>(mesh);
  for (int f = 0; f < mesh->faceCount(); ++f)
    mesh->setFacePickingID(f, eng::makePickingId(obj->stableId(), uint32_t(f)));
  obj->setPosition(glm::vec3(0.0f));
  obj->setRotation(glm::vec3(0.0f));
  obj->setScale(glm::vec3(1.0f));
  if (metaFrom) {
    obj->setName(metaFrom->name());
    obj->setLayer(metaFrom->layer());
    obj->setGroup(metaFrom->group());
    obj->setBrushEntity(metaFrom->brushEntity());
    obj->setMirrorLink(metaFrom->mirrorLink(), metaFrom->mirrorSide());
    obj->setModHost(metaFrom->modHost()); // an edited modifier source stays a source
    obj->setOrientation(metaFrom->orientation());
    if (metaFrom->smoothShading())
      obj->setSmoothShading(true); // the new mesh shades as the old one did
  } else if (m_scene) {
    obj->setGroup(m_scene->activeGroup);
  }
  obj->computeWorldAABB();
  return obj;
}

void Engine3D::replaceObjects(const std::vector<quint32> &removeIds,
                              const std::vector<std::shared_ptr<eng::GameObject>> &add, const QString &label) {
  if (!m_scene || (removeIds.empty() && add.empty()))
    return;
  auto cmd = std::make_unique<StructureCommand>(this, label);
  cmd->selBefore.assign(m_selectedObjects.begin(), m_selectedObjects.end());
  cmd->facesBefore.assign(m_selectedFaces.begin(), m_selectedFaces.end());
  for (const quint32 id: removeIds) {
    const int slot = m_scene->indexOf(id);
    if (slot < 0)
      continue;
    // Derived objects are never part of a recorded step: the evaluation owns them.
    if (m_scene->gameObjects[size_t(slot)]->isDerived()) {
      qWarning("[modifiers] replaceObjects: skipped derived object %u", id);
      Q_ASSERT_X(false, "replaceObjects", "derived object in a recorded edit");
      continue;
    }
    cmd->removed.push_back(m_scene->gameObjects[size_t(slot)]);
    cmd->removedSlots.push_back(slot);
  }
  cmd->added = add;
  // A rebuilt generated object keeps its old brushes' mirror link and modifier host.
  for (const auto &o: add) {
    if (!o || !o->procObject())
      continue;
    for (const auto &r: cmd->removed)
      if (r->procObject() == o->procObject()) {
        if (!o->mirrorLink() && r->mirrorLink())
          o->setMirrorLink(r->mirrorLink(), r->mirrorSide());
        if (!o->modHost() && r->modHost())
          o->setModHost(r->modHost());
        break;
      }
  }
  for (const auto &o: add)
    cmd->selAfter.push_back(o->stableId());
  cmd->redo();
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::move(cmd));
  emit sceneChanged();
}

eng::MaterialPtr Engine3D::newBrushMaterial(double &tilesPerUnit) const {
  QString name = m_currentMaterial;
  if (name.isEmpty())
    name = prefs::s("Brush/DefaultMaterial");
  auto mat = TextureManager::instance().materialFor(name);
  const double scale = prefs::d("Brush/DefaultTextureScale");
  const double texW = mat && mat->texWidth > 0 ? mat->texWidth : 512.0;
  tilesPerUnit = kSourceUnitsPerWorldUnit / (texW * scale);
  return mat;
}

std::vector<std::pair<quint32, eng::ConvexBrush>> Engine3D::selectedBrushes() const {
  std::vector<std::pair<quint32, eng::ConvexBrush>> out;
  if (!m_scene)
    return out;
  for (const quint32 id: m_selectedObjects) {
    const auto obj = m_scene->findById(id);
    if (!obj || obj->locked() || obj->isDerived())
      continue;
    if (auto b = eng::ConvexBrush::fromObject(*obj))
      out.emplace_back(id, std::move(*b));
    else
      qCWarning(brushLog) << "[brush] object" << id << "is not a closed convex brush; skipped";
  }
  return out;
}

bool Engine3D::createPrimitive(const int kind, const glm::vec3 &mn, const glm::vec3 &mx) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  const glm::vec3 size = mx - mn;
  if (size.x <= 1e-4f || size.y <= 1e-4f || size.z <= 1e-4f)
    return false;
  double tpu = 0.0;
  const auto mat = newBrushMaterial(tpu);
  eng::PrimitiveParams params;
  params.sides = prefs::i("Brush/CylinderSides");
  params.steps = prefs::i("Brush/StairSteps");
  params.segments = prefs::i("Brush/ArchSegments");
  params.up = 1;
  const auto brushes = eng::buildPrimitive(static_cast<eng::PrimitiveKind>(kind), glm::dvec3(mn), glm::dvec3(mx),
                                           params, mat, tpu);
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  for (const auto &b: brushes)
    if (auto o = objectFromBrush(b))
      objs.push_back(o);
  if (objs.empty())
    return false;
  static const char *names[] = {"Box", "Wedge", "Cylinder", "Cone", "Spike", "Sphere", "Stairs", "Arch", "Pyramid"};
  replaceObjects({}, objs, QStringLiteral("Create %1").arg(QLatin1String(names[std::clamp(kind, 0, 8)])));
  return true;
}

bool Engine3D::createPrimitiveFromMarquee(const int kind) {
  if (!m_marquee.valid)
    return false;
  glm::dvec3 mn, mx;
  marqueeToWorld(m_marquee, mn, mx);
  const bool ok = withMirror(QStringLiteral("Create (mirrored)"),
                             [&] { return createPrimitive(kind, glm::vec3(mn), glm::vec3(mx)); });
  if (ok)
    clearMarquee();
  return ok;
}

int Engine3D::csgSubtract() {
  if (!m_scene || m_selectedObjects.empty())
    return 0;
  const auto cutters = selectedBrushes();
  if (cutters.empty())
    return 0;
  glm::dvec3 cmn(DBL_MAX), cmx(-DBL_MAX);
  for (const auto &[id, b]: cutters) {
    glm::dvec3 a, c;
    b.bounds(a, c);
    cmn = glm::min(cmn, a);
    cmx = glm::max(cmx, c);
  }
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  for (const auto &obj: m_scene->gameObjects) {
    if (m_selectedObjects.count(obj->stableId()) || obj->hidden() || obj->locked() || obj->isDerived())
      continue;
    const auto [lo, hi] = obj->getWorldAABB();
    if (lo.x > cmx.x || lo.y > cmx.y || lo.z > cmx.z || hi.x < cmn.x || hi.y < cmn.y || hi.z < cmn.z)
      continue;
    auto minuend = eng::ConvexBrush::fromObject(*obj);
    if (!minuend)
      continue;
    std::vector<eng::ConvexBrush> pieces = {*minuend};
    bool touched = false;
    for (const auto &[cid, cutter]: cutters) {
      std::vector<eng::ConvexBrush> next;
      for (const auto &p: pieces) {
        if (!p.overlaps(cutter)) {
          next.push_back(p);
          continue;
        }
        touched = true;
        for (auto &frag: p.subtract(cutter))
          next.push_back(std::move(frag));
      }
      pieces = std::move(next);
    }
    if (!touched)
      continue;
    remove.push_back(obj->stableId());
    for (const auto &p: pieces)
      if (auto o = objectFromBrush(p, obj.get()))
        add.push_back(o);
  }
  if (remove.empty())
    return 0;
  // The cutters go too (Hammer's "carve" keeps them; subtract consumes them).
  if (prefs::b("Brush/SubtractRemovesCutter"))
    for (const auto &[id, b]: cutters)
      remove.push_back(id);
  replaceObjects(remove, add, QStringLiteral("Subtract"));
  return int(add.size());
}

int Engine3D::csgHollow(double thicknessSource) {
  if (thicknessSource <= 0.0)
    thicknessSource = prefs::d("Brush/HollowThickness");
  const double t = thicknessSource / kSourceUnitsPerWorldUnit;
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  const bool outward = prefs::b("Brush/HollowOutward");
  for (const auto &[id, b]: selectedBrushes()) {
    std::vector<eng::ConvexBrush> walls;
    if (outward) {
      // The brush becomes the room: walls grow outside it.
      eng::ConvexBrush outer = b;
      for (auto &f: outer.faces)
        f.distance += t;
      if (outer.build())
        walls = outer.subtract(b);
    } else {
      walls = b.hollow(t);
    }
    if (walls.size() <= 1)
      continue;
    remove.push_back(id);
    const auto src = m_scene->findById(id);
    for (const auto &w: walls)
      if (auto o = objectFromBrush(w, src.get()))
        add.push_back(o);
  }
  if (remove.empty())
    return 0;
  replaceObjects(remove, add, QStringLiteral("Hollow"));
  return int(add.size());
}

bool Engine3D::csgMerge(const bool force) {
  m_mergeWouldFillGaps = false;
  const auto brushes = selectedBrushes();
  if (brushes.size() < 2)
    return false;
  std::vector<glm::dvec3> pts;
  for (const auto &[id, b]: brushes)
    for (const auto &v: b.vertices())
      pts.push_back(v);
  double tpu = 0.0;
  const auto fallback = newBrushMaterial(tpu);
  auto hull = eng::ConvexBrush::fromPoints(pts, fallback, tpu);
  if (!hull)
    return false;
  // Hull faces that lie on an original face keep that face's look.
  for (auto &f: hull->faces)
    for (const auto &[id, b]: brushes)
      for (const auto &of: b.faces)
        if (glm::dot(of.normal, f.normal) > 1.0 - 1e-6 && std::fabs(of.distance - f.distance) < 1e-3) {
          f.material = of.material;
          f.basis = of.basis;
          f.groups = eng::facegroup::unite(f.groups, of.groups);
        }
  // Exact only when the union is convex: hull volume vs. union volume (sum minus pairwise overlaps).
  // Anything more would fill gaps, which needs an explicit "merge anyway".
  double sum = 0.0;
  for (const auto &[id, b]: brushes)
    sum += b.volume();
  for (size_t i = 0; i < brushes.size(); ++i)
    for (size_t j = i + 1; j < brushes.size(); ++j)
      if (const auto x = brushes[i].second.intersect(brushes[j].second))
        sum -= x->volume();
  if (hull->volume() > sum * 1.001 + 1e-6) {
    if (!force) {
      m_mergeWouldFillGaps = true;
      qCInfo(brushLog) << "[brush] merge refused: the convex hull would fill gaps between the brushes";
      return false;
    }
    qCInfo(brushLog) << "[brush] merge filled gaps between the brushes (convex hull)";
  }
  auto obj = objectFromBrush(*hull, m_scene->findById(brushes.front().first).get());
  if (!obj)
    return false;
  std::vector<quint32> remove;
  for (const auto &[id, b]: brushes)
    remove.push_back(id);
  replaceObjects(remove, {obj}, QStringLiteral("Merge"));
  return true;
}

bool Engine3D::csgIntersect() {
  const auto brushes = selectedBrushes();
  if (brushes.size() < 2)
    return false;
  std::optional<eng::ConvexBrush> acc = brushes.front().second;
  for (size_t i = 1; i < brushes.size() && acc; ++i)
    acc = acc->intersect(brushes[i].second);
  if (!acc)
    return false;
  auto obj = objectFromBrush(*acc, m_scene->findById(brushes.front().first).get());
  if (!obj)
    return false;
  std::vector<quint32> remove;
  for (const auto &[id, b]: brushes)
    remove.push_back(id);
  replaceObjects(remove, {obj}, QStringLiteral("Intersect"));
  return true;
}

Engine3D::FlatSplit Engine3D::splitFlatObject(const eng::GameObject &obj, const glm::dvec3 &n, const double d,
                                              const bool keepBack, const bool keepFront) const {
  FlatSplit out;
  const auto mesh = obj.getMesh();
  if (!mesh)
    return out;
  const glm::dmat4 model(obj.getModelMatrix());
  const glm::dmat3 normalMat = glm::inverseTranspose(glm::dmat3(model));
  constexpr double kEps = 1e-6;
  // The faces in brush order (outward winding: the mesh keeps Source's inward one).
  struct Face {
    eng::BrushFace attr;
    std::vector<glm::dvec3> poly;
  };
  std::vector<Face> faces;
  for (int f = 0; f < mesh->faceCount(); ++f) {
    std::vector<glm::vec3> local;
    if (!mesh->getFacePolygon(f, local) || local.size() < 3)
      continue;
    Face face;
    for (auto it = local.rbegin(); it != local.rend(); ++it)
      face.poly.push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(*it), 1.0)));
    glm::vec3 fn;
    float fd = 0.0f;
    if (mesh->getFacePlane(f, fn, fd)) {
      face.attr.normal = glm::normalize(normalMat * glm::dvec3(fn));
      face.attr.distance = glm::dot(face.attr.normal, face.poly.front());
    }
    face.attr.material = mesh->faceMaterialRef(f);
    face.attr.basis = mesh->faceTexBasis(f);
    face.attr.groups = mesh->faceGroups(f);
    for (const auto &p: face.poly) {
      const double v = glm::dot(n, p) - d;
      out.sides |= v < -kEps ? 1 : v > kEps ? 2 : 0;
    }
    faces.push_back(std::move(face));
  }
  if (out.sides != 3)
    return out; // on one side (or in the plane): nothing to cut
  // Sutherland-Hodgman on each face polygon: keep where s·(n·p - d) <= 0.
  auto clip = [&](const std::vector<glm::dvec3> &poly, const double s) {
    std::vector<glm::dvec3> kept;
    for (size_t i = 0; i < poly.size(); ++i) {
      const glm::dvec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
      const double da = s * (glm::dot(n, a) - d), db = s * (glm::dot(n, b) - d);
      if (da <= kEps)
        kept.push_back(a);
      if ((da < -kEps && db > kEps) || (da > kEps && db < -kEps))
        kept.push_back(a + (b - a) * (da / (da - db)));
    }
    return kept;
  };
  for (const double s: {1.0, -1.0}) {
    if ((s > 0.0 && !keepBack) || (s < 0.0 && !keepFront))
      continue;
    eng::ConvexBrush piece; // faces and polygons only: no volume to build
    for (const Face &face: faces) {
      std::vector<glm::dvec3> kept = clip(face.poly, s);
      if (kept.size() < 3)
        continue;
      piece.faces.push_back(face.attr);
      piece.polygons.push_back(std::move(kept));
    }
    if (piece.faces.empty())
      continue;
    if (auto o = objectFromBrush(piece, &obj))
      out.pieces.push_back(o);
  }
  return out;
}

int Engine3D::clipSelection(const glm::vec3 &normal, const float distance, const int keep) {
  // keep: 0 = back (behind the plane), 1 = front, 2 = both pieces.
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  std::set<quint32> solid;
  for (const auto &[id, b]: selectedBrushes()) {
    solid.insert(id);
    std::optional<eng::ConvexBrush> front, back;
    b.split(glm::dvec3(normal), double(distance), front, back);
    if (!front || !back)
      continue; // plane misses the brush
    remove.push_back(id);
    const auto src = m_scene->findById(id);
    if (keep != 1 && back)
      if (auto o = objectFromBrush(*back, src.get()))
        add.push_back(o);
    if (keep != 0 && front)
      if (auto o = objectFromBrush(*front, src.get()))
        add.push_back(o);
  }
  // A brush with no volume (a single plane) is cut through its face polygons.
  if (m_scene)
    for (const quint32 id: m_selectedObjects) {
      const auto o = m_scene->findById(id);
      if (!o || solid.count(id) || o->locked() || o->isDerived())
        continue;
      FlatSplit split = splitFlatObject(*o, glm::dvec3(normal), double(distance), keep != 1, keep != 0);
      if (split.sides != 3)
        continue; // the plane misses it
      remove.push_back(id);
      add.insert(add.end(), split.pieces.begin(), split.pieces.end());
    }
  if (remove.empty())
    return 0;
  replaceObjects(remove, add, QStringLiteral("Clip"));
  return std::max(1, int(add.size()));
}

bool Engine3D::transformSelectionBrushes(const glm::dmat4 &m, const QString &label, const int unmirrorMask) {
  auto brushes = selectedBrushes();
  if (brushes.empty())
    return false;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  // Whole generated objects (stairs) are rebuilt from their parameters.
  std::set<quint32> handled;
  const std::vector<quint32> procs = transformProcObjects(m, label, handled);
  const bool lock = unmirrorMask >= 0 || m_textureLock;
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  for (auto [id, b]: brushes) {
    if (handled.count(id))
      continue;
    b.transform(m, lock);
    if (!b.valid())
      continue;
    if (unmirrorMask > 0)
      for (eng::BrushFace &f: b.faces)
        if (f.basis.valid)
          eng::unmirrorBasis(f.basis, unmirrorMask);
    if (auto o = objectFromBrush(b, m_scene->findById(id).get())) {
      o->setOrientation(carryOrientation(o->orientation(), m));
      remove.push_back(id);
      add.push_back(o);
    }
  }
  if (!remove.empty())
    replaceObjects(remove, add, label);
  if (record)
    m_undoStack->endMacro();
  if (!procs.empty()) {
    // Each rebuild selected only its own brushes: select everything transformed.
    std::vector<quint32> sel;
    for (const auto &o: add)
      sel.push_back(o->stableId());
    for (const quint32 p: procs)
      for (const quint32 sid: procMembers(p))
        sel.push_back(sid);
    setSelected(sel, {});
  }
  return !remove.empty() || !procs.empty();
}

glm::dquat Engine3D::carryOrientation(const glm::dquat &q, const glm::dmat4 &m) {
  const glm::dmat3 a(m);
  const glm::dmat3 ata = glm::transpose(a) * a;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (std::fabs(ata[i][j] - (i == j ? 1.0 : 0.0)) > 1e-6)
        return q;
  const glm::dmat3 r = glm::mat3_cast(q);
  const glm::dmat3 out = glm::determinant(a) > 0.0 ? a * r : a * r * glm::transpose(a);
  return glm::normalize(glm::quat_cast(out));
}

glm::dvec3 Engine3D::eulerFromOrientation(const glm::dquat &q) {
  // R = Ry(y) Rx(x) Rz(z); r(i, j) = row i, column j (glm is column-major).
  const glm::dmat3 m = glm::mat3_cast(q);
  auto r = [&](int i, int j) { return m[j][i]; };
  const double sx = std::clamp(-r(1, 2), -1.0, 1.0);
  double x = std::asin(sx), y = 0.0, z = 0.0;
  if (std::fabs(sx) < 1.0 - 1e-9) {
    y = std::atan2(r(0, 2), r(2, 2));
    z = std::atan2(r(1, 0), r(1, 1));
  } else {
    y = std::atan2(-r(2, 0), r(0, 0)); // gimbal lock: z folded into y
  }
  glm::dvec3 d = glm::degrees(glm::dvec3(x, y, z));
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(d[i]) < 1e-7)
      d[i] = 0.0;
    if (d[i] <= -180.0 + 1e-7)
      d[i] = 180.0;
  }
  return d;
}

glm::dquat Engine3D::orientationFromEuler(const glm::dvec3 &degrees) {
  const glm::dvec3 r = glm::radians(degrees);
  return glm::normalize(glm::angleAxis(r.y, glm::dvec3(0, 1, 0)) * glm::angleAxis(r.x, glm::dvec3(1, 0, 0))
                        * glm::angleAxis(r.z, glm::dvec3(0, 0, 1)));
}

bool Engine3D::selectionRotation(glm::dvec3 &degrees, bool mixed[3]) const {
  bool any = false;
  for (int i = 0; i < 3; ++i)
    mixed[i] = false;
  if (!m_scene)
    return false;
  for (const quint32 id: m_selectedObjects) {
    const auto o = m_scene->findById(id);
    if (!o || !o->getMesh() || o->isDerived())
      continue;
    const glm::dvec3 e = eulerFromOrientation(o->orientation());
    if (!any) {
      degrees = e;
      any = true;
      continue;
    }
    for (int i = 0; i < 3; ++i)
      if (std::fabs(e[i] - degrees[i]) > 1e-4)
        mixed[i] = true;
  }
  return any;
}

namespace {

// A rotation matrix with the float noise of 90° steps cleaned off.
glm::dmat4 cleanRotation(glm::dmat4 m) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      if (std::fabs(m[i][j]) < 1e-12)
        m[i][j] = 0.0;
      else if (std::fabs(std::fabs(m[i][j]) - 1.0) < 1e-12)
        m[i][j] = m[i][j] > 0.0 ? 1.0 : -1.0;
    }
  return m;
}

} // namespace

void Engine3D::setSelectionRotation(const int axis, const double degrees) {
  if (!m_scene || axis < 0 || axis > 2)
    return;
  std::vector<quint32> ids;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && o->getMesh() && !o->isDerived() && !o->locked())
      ids.push_back(id);
  if (ids.empty())
    return;
  // The same rotation everywhere: one turn about the selection centre.
  const glm::dquat q0 = m_scene->findById(ids.front())->orientation();
  bool same = true;
  for (const quint32 id: ids)
    same = same && std::fabs(glm::dot(m_scene->findById(id)->orientation(), q0)) > 1.0 - 1e-9;
  auto deltaFor = [&](const glm::dquat &q) {
    glm::dvec3 e = eulerFromOrientation(q);
    e[axis] = degrees;
    return orientationFromEuler(e) * glm::inverse(q);
  };
  auto about = [](const glm::dquat &d, const glm::dvec3 &c) {
    return cleanRotation(glm::translate(glm::dmat4(1.0), c) * glm::dmat4(glm::mat4_cast(d))
                         * glm::translate(glm::dmat4(1.0), -c));
  };
  const QString label = tr("Rotate");
  if (same) {
    const glm::dquat d = deltaFor(q0);
    if (std::fabs(std::fabs(d.w) - 1.0) < 1e-12)
      return;
    glm::dvec3 mn, mx;
    if (!objectsBounds(ids, mn, mx))
      return;
    setSelected(ids, {});
    transformSelectionBrushes(about(d, (mn + mx) * 0.5), label);
    return;
  }
  // Each brush about its own centre.
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  std::vector<quint32> result;
  for (const quint32 id: ids) {
    const auto o = m_scene->findById(id);
    if (!o)
      continue;
    const glm::dquat d = deltaFor(o->orientation());
    glm::dvec3 mn, mx;
    if (std::fabs(std::fabs(d.w) - 1.0) < 1e-12 || !objectsBounds({id}, mn, mx)) {
      result.push_back(id);
      continue;
    }
    setSelected({id}, {});
    transformSelectionBrushes(about(d, (mn + mx) * 0.5), label);
    result.insert(result.end(), m_selectedObjects.begin(), m_selectedObjects.end());
  }
  setSelected(result, {});
  if (record) {
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [] {}, [this, result] { setSelected(result, {}); }));
    m_undoStack->endMacro();
  }
}

void Engine3D::resetSelectionRotation() {
  if (!m_scene)
    return;
  // Undo the whole rotation at once by q⁻¹ (axis by axis would leave Euler-order leftovers).
  std::vector<quint32> ids;
  for (const quint32 id: m_selectedObjects)
    if (const auto o = m_scene->findById(id); o && o->getMesh() && !o->isDerived() && !o->locked()
        && std::fabs(std::fabs(o->orientation().w) - 1.0) > 1e-12)
      ids.push_back(id);
  if (ids.empty())
    return;
  const QString label = tr("Reset rotation");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  std::map<std::array<qint64, 4>, std::vector<quint32>> byRotation;
  for (const quint32 id: ids) {
    const glm::dquat q = m_scene->findById(id)->orientation();
    const glm::dquat k = q.w < 0.0 ? -q : q;
    byRotation[{std::llround(k.w * 1e8), std::llround(k.x * 1e8), std::llround(k.y * 1e8), std::llround(k.z * 1e8)}]
        .push_back(id);
  }
  std::vector<quint32> result;
  for (const auto &[key, group]: byRotation) {
    glm::dvec3 mn, mx;
    if (!objectsBounds(group, mn, mx))
      continue;
    const glm::dquat inv = glm::inverse(m_scene->findById(group.front())->orientation());
    const glm::dvec3 c = (mn + mx) * 0.5;
    setSelected(group, {});
    transformSelectionBrushes(cleanRotation(glm::translate(glm::dmat4(1.0), c) * glm::dmat4(glm::mat4_cast(inv))
                                            * glm::translate(glm::dmat4(1.0), -c)),
                              label);
    result.insert(result.end(), m_selectedObjects.begin(), m_selectedObjects.end());
  }
  setSelected(result, {});
  if (record) {
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [] {}, [this, result] { setSelected(result, {}); }));
    m_undoStack->endMacro();
  }
}

glm::dvec3 Engine3D::selectionPivot() const {
  glm::vec3 mn, mx;
  if (!selectionBounds(mn, mx))
    return glm::dvec3(0.0);
  return glm::dvec3((mn + mx) * 0.5f);
}

bool Engine3D::rotateSelection(const int axis, const double degrees) {
  if (axis < 0 || axis > 2)
    return false;
  glm::dvec3 ax(0.0);
  ax[axis] = 1.0;
  const std::vector<EntityPose> entities = selectedPointEntityPoses();
  const bool entitiesOnly = m_selectedObjects.empty() && !entities.empty();
  const glm::dvec3 c = entitiesOnly ? glm::dvec3(gizmoCenter()) : selectionPivot();
  // Pivot snapped to the grid so repeated 90° turns stay on it (entities alone turn about their own origin).
  const double step = gizmoSnapStep();
  const glm::dvec3 pivot = step > 0 && !entitiesOnly ? glm::round(c / step) * step : c;
  glm::dmat4 m = glm::translate(glm::dmat4(1.0), pivot) * glm::rotate(glm::dmat4(1.0), glm::radians(degrees), ax) *
                 glm::translate(glm::dmat4(1.0), -pivot);
  // Exact quarter turns: kill the cos/sin noise so planes stay axis aligned.
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (std::fabs(m[i][j]) < 1e-12)
        m[i][j] = 0.0;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Rotate"));
  const bool brushes = transformSelectionBrushes(m, QStringLiteral("Rotate"));
  rotateEntityPoses(entities, glm::mat3(m), glm::vec3(pivot), true);
  if (record)
    m_undoStack->endMacro();
  return brushes || !entities.empty();
}

bool Engine3D::mirrorSelection(const int axis) {
  if (axis < 0 || axis > 2)
    return false;
  const glm::dvec3 c = selectionPivot();
  glm::dvec3 s(1.0);
  s[axis] = -1.0;
  const glm::dmat4 m = glm::translate(glm::dmat4(1.0), c) * glm::scale(glm::dmat4(1.0), s) *
                       glm::translate(glm::dmat4(1.0), -c);
  return transformSelectionBrushes(m, QStringLiteral("Mirror"));
}

bool Engine3D::snapSelectionToGridCell() {
  if (!m_scene)
    return false;
  const double step = gizmoSnapStep();
  if (step <= 0.0)
    return false;
  const auto objects = m_selectedObjects;
  const auto faces = m_selectedFaces;
  const auto entities = m_selectedEntities;
  const auto mods = m_selectedMods;
  const auto paths = m_selectedPaths;

  // Point entities: the middle of their cell.
  std::vector<std::pair<quint32, glm::vec3>> entityMoves;
  for (const auto &[id, origin]: selectedEntityOrigins()) {
    const eng::Entity *e = entityById(id);
    if (!e || (e->cls() && e->cls()->isBrushEntity()))
      continue;
    const glm::dvec3 o(origin);
    const glm::dvec3 centre = (glm::floor(o / step) + 0.5) * step;
    const glm::vec3 d(centre - o);
    if (glm::length(d) > 1e-6f)
      entityMoves.emplace_back(id, d);
  }

  // Brushes as one block: per axis, the corner closest to a grid line goes onto it (size unchanged).
  glm::vec3 brushMove(0.0f);
  if (!objects.empty() || !mods.empty()) {
    m_selectedEntities.clear();
    m_selectedPaths.clear();
    glm::vec3 mn, mx;
    if (selectionBounds(mn, mx))
      for (int a = 0; a < 3; ++a) {
        const double lo = mn[a], hi = mx[a];
        const double dLo = std::round(lo / step) * step - lo;
        const double dHi = std::round(hi / step) * step - hi;
        brushMove[a] = float(std::fabs(dLo) <= std::fabs(dHi) ? dLo : dHi);
      }
    m_selectedEntities = entities;
    m_selectedPaths = paths;
  }
  const bool moveBrushes = glm::length(brushMove) > 1e-6f;
  if (!moveBrushes && entityMoves.empty())
    return false;

  const QString label = QStringLiteral("Snap to grid");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  if (moveBrushes) {
    m_selectedEntities.clear();
    m_selectedPaths.clear();
    translateSelection(brushMove, label);
  }
  for (const auto &[id, d]: entityMoves) {
    m_selectedObjects.clear();
    m_selectedFaces.clear();
    m_selectedMods.clear();
    m_selectedPaths.clear();
    m_selectedEntities = {id};
    translateSelection(d, label);
  }
  m_selectedObjects = objects;
  m_selectedFaces = faces;
  m_selectedEntities = entities;
  m_selectedMods = mods;
  m_selectedPaths = paths;
  refreshDrawSelection();
  if (record)
    m_undoStack->endMacro();
  emit selectionChanged();
  return true;
}

bool Engine3D::snapSelectionToGrid() {
  const double step = gizmoSnapStep();
  if (step <= 0)
    return false;
  std::vector<quint32> remove;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  for (const auto &[id, b]: selectedBrushes()) {
    std::vector<glm::dvec3> pts;
    for (const auto &v: b.vertices())
      pts.push_back(glm::round(v / step) * step);
    auto snapped = eng::ConvexBrush::fromPoints(pts, b.faces.empty() ? eng::MaterialPtr() : b.faces.front().material,
                                                0.04);
    if (!snapped)
      continue;
    for (auto &f: snapped->faces)
      if (const eng::BrushFace *src = b.mostParallelFace(f.normal)) {
        f.material = src->material;
        f.basis = src->basis;
        f.groups = src->groups;
      }
    if (auto o = objectFromBrush(*snapped, m_scene->findById(id).get())) {
      remove.push_back(id);
      add.push_back(o);
    }
  }
  if (remove.empty())
    return false;
  replaceObjects(remove, add, QStringLiteral("Snap to grid"));
  return true;
}

} // namespace vk

namespace vk {

bool Engine3D::copySelection() {
  m_clipBrushes.clear();
  m_clipEntities.clear();
  m_clipBrushOwner.clear();
  m_clipMeta.clear();
  for (auto &[id, b]: selectedBrushes()) {
    m_clipBrushes.push_back(b);
    const auto o = m_scene ? m_scene->findById(id) : nullptr;
    m_clipBrushOwner.push_back(o ? o->brushEntity() : 0u);
    ClipBrushMeta meta;
    if (o) {
      meta.orientation = o->orientation();
      // A source picked on its own (its modified object not selected as such).
      if (o->modHost() && !m_selectedMods.count(o->modHost()))
        if (const eng::mods::Host *h = m_scene->findModHost(o->modHost())) {
          meta.joinHost = h->id;
          meta.joinHostName = h->name;
        }
      if (const auto *l = o->mirrorLink() ? m_scene->findMirrorLink(o->mirrorLink()) : nullptr) {
        meta.linked = true;
        meta.link = *l;
        meta.side = o->mirrorSide();
        if (const auto *st = m_scene->findMirrorSet(l->set))
          meta.set = *st;
      }
    }
    m_clipMeta.push_back(meta);
  }
  // Selected modified objects: their sources and their stacks.
  m_clipHosts.clear();
  for (const quint32 h: m_selectedMods) {
    const eng::mods::Host *host = m_scene ? m_scene->findModHost(h) : nullptr;
    if (!host)
      continue;
    m_clipHosts[h] = *host;
    for (const quint32 id: modSources(h)) {
      const auto o = m_scene->findById(id);
      const auto b = o ? eng::ConvexBrush::fromObject(*o) : std::nullopt;
      if (!b)
        continue;
      m_clipBrushes.push_back(*b);
      m_clipBrushOwner.push_back(o->brushEntity());
      ClipBrushMeta meta;
      meta.orientation = o->orientation();
      meta.host = h;
      if (const auto *l = o->mirrorLink() ? m_scene->findMirrorLink(o->mirrorLink()) : nullptr) {
        meta.linked = true;
        meta.link = *l;
        meta.side = o->mirrorSide();
        if (const auto *st = m_scene->findMirrorSet(l->set))
          meta.set = *st;
      }
      m_clipMeta.push_back(meta);
    }
  }
  for (const quint32 id: m_selectedEntities)
    if (const eng::Entity *e = entityById(id); e && e->classname != eng::kWorldEnvClass) // one per map
      m_clipEntities.push_back(*e);
  return !m_clipBrushes.empty() || !m_clipEntities.empty();
}

bool Engine3D::cutSelection() {
  // Deletes the whole selection even when part of it cannot be copied (a brush with no volume).
  const bool copied = copySelection();
  const bool any = !m_selectedObjects.empty() || !m_selectedEntities.empty() || !m_selectedMods.empty();
  if (!copied && !any)
    return false;
  deleteSelected();
  return true;
}

bool Engine3D::paste(const bool inPlace, const bool uniqueNames) {
  if (m_clipBrushes.empty() && m_clipEntities.empty())
    return false;
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  // Repeated pastes step along so copies do not stack invisibly.
  const float step = float(gizmoSnapStep());
  const glm::vec3 offset = inPlace ? glm::vec3(0.0f) : glm::vec3(step * float(++m_pasteCount), 0.0f, step * float(m_pasteCount));
  const glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(offset));

  // Entities first, so pasted brushes tie to the pasted copies of their brush entities.
  std::vector<eng::Entity> clip = m_clipEntities;
  if (uniqueNames) {
    // Named entities get new names; references inside the set follow, outside ones stay.
    QHash<QString, QString> rename; // old (lower) -> new
    QSet<QString> reserved;
    for (const auto &e: clip)
      if (!e.name.isEmpty() && !rename.contains(e.name.toLower())) {
        const QString fresh = uniqueEntityName(e.name, reserved);
        reserved.insert(fresh.toLower());
        rename.insert(e.name.toLower(), fresh);
      }
    for (auto &e: clip) {
      if (!e.name.isEmpty())
        e.name = rename.value(e.name.toLower(), e.name);
      for (auto &c: e.outputs)
        if (const auto it = rename.constFind(c.target.toLower()); it != rename.constEnd())
          c.target = *it;
      if (const eng::EntityClass *cls = e.cls())
        for (const auto &p: cls->props)
          if (p.type == eng::PropType::Target && e.props.contains(p.key))
            if (const auto it = rename.constFind(e.props.value(p.key).toString().toLower()); it != rename.constEnd())
              e.props[p.key] = *it;
    }
  }
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(uniqueNames ? QStringLiteral("Paste with unique names") : QStringLiteral("Paste"));
  std::vector<quint32> ents;
  std::map<quint32, quint32> entityRemap;
  for (const auto &e: clip)
    if (const quint32 id = addEntityFrom(e, e.origin + offset)) {
      entityRemap[e.id] = id;
      ents.push_back(id);
    }
  std::vector<std::shared_ptr<eng::GameObject>> add;
  std::vector<MirrorCopy> mirrored;
  std::vector<std::pair<std::shared_ptr<eng::GameObject>, quint32>> joins; // pasted source, its host
  for (size_t i = 0; i < m_clipBrushes.size(); ++i) {
    auto b = m_clipBrushes[i];
    b.transform(m, true);
    if (auto o = objectFromBrush(b)) {
      const quint32 owner = i < m_clipBrushOwner.size() ? m_clipBrushOwner[i] : 0u;
      if (const auto it = entityRemap.find(owner); it != entityRemap.end())
        o->setBrushEntity(it->second);
      if (i < m_clipMeta.size()) {
        const ClipBrushMeta &meta = m_clipMeta[i];
        o->setOrientation(meta.orientation);
        if (meta.host)
          o->setModHost(meta.host); // remapped to the pasted host below
        else if (meta.joinHost)
          if (const eng::mods::Host *h = m_scene ? m_scene->findModHost(meta.joinHost) : nullptr;
              h && h->name == meta.joinHostName)
            joins.emplace_back(o, h->id);
        if (meta.linked)
          mirrored.push_back({o, meta.link, meta.side, meta.set, glm::dvec3(offset)});
      }
      add.push_back(o);
    }
  }
  // Copied modified objects come back as new ones with the same stack.
  std::vector<quint32> pastedHosts;
  if (!m_clipHosts.empty() && m_scene) {
    const auto before = m_scene->modHosts;
    std::map<quint32, quint32> remap;
    for (const auto &[oldId, rec]: m_clipHosts) {
      eng::mods::Host h = rec;
      h.id = m_scene->mintModHostId();
      h.name = newModHostName(rec.stack.empty() ? QString() : rec.stack.front().kind, eng::kWorldspawnGroup);
      m_scene->modHosts.push_back(h);
      remap[oldId] = h.id;
      pastedHosts.push_back(h.id);
    }
    for (const auto &o: add)
      if (o->modHost())
        o->setModHost(remap.count(o->modHost()) ? remap[o->modHost()] : 0);
    pushModTable(before, QStringLiteral("Paste"));
  } else {
    for (const auto &o: add)
      o->setModHost(0);
  }
  // Source brushes copied on their own paste into their modified object.
  for (const auto &[o, h]: joins)
    o->setModHost(h);
  if (!add.empty()) {
    replaceObjects({}, add, QStringLiteral("Paste"));
    std::vector<quint32> ids;
    for (const auto &o: add)
      ids.push_back(o->stableId());
    // Mirrored brushes paste as a mirror; the rest get a copy when mirror mode applies.
    relinkCopies(mirrored, QStringLiteral("Paste"));
    setSelected(ids, {});
    mirrorCreated(ids, QStringLiteral("Paste"));
    setSelected(ids, {});
    if (record && !mirrored.empty())
      m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Paste"), [] {},
                                                             [this, ids] { setSelected(ids, {}); }));
  }
  if (record)
    m_undoStack->endMacro();
  if (!add.empty())
    syncBrushEntityOrigins();
  if (!ents.empty()) {
    if (add.empty())
      setSelected({}, {}); // only entities pasted: they alone are the selection
    setSelectedEntities(ents); // keeps the pasted brushes selected too
  }
  if (!pastedHosts.empty()) {
    // The pasted modified objects are selected as such (their sources hide).
    pumpModifiers(ModPump::Blocking);
    std::vector<quint32> plain;
    for (const quint32 id: m_selectedObjects)
      if (const auto o = m_scene->findById(id); o && !o->modHost())
        plain.push_back(id);
    setSelected(plain, {});
    setSelectedMods(pastedHosts);
  }
  return true;
}

} // namespace vk
