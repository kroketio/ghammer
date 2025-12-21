// Generated objects (stairs, arches): a ProcObject holds type + parameters, its brushes point at it. Parameter
// changes, resizes and moves rebuild the brushes in one undo step; materials and face groups carry over (same plane
// first, else by stair part).

#include <QJsonArray>

#include <glm/gtc/matrix_transform.hpp>

#include "vk_engine.h"
#include "vk_shared.h"

#include "client/engine/arch_gen.h"
#include "client/engine/shape_gen.h"
#include "client/engine/brush.h"
#include "client/engine/entity.h"
#include "client/engine/scene.h"
#include "client/engine/stair_gen.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

const QString kStairsType = QStringLiteral("stairs");
const QString kArchType = QStringLiteral("arch");

// "Stairs 3", "Arch 2": the next free number for a type.
QString nextProcName(const std::vector<eng::Scene::ProcObject> &table, const QString &type) {
  int n = 1;
  for (const auto &p: table)
    n += p.type == type ? 1 : 0;
  const QString title = type.isEmpty() ? type : type.left(1).toUpper() + type.mid(1);
  return QStringLiteral("%1 %2").arg(title).arg(n);
}

struct OldFace {
  glm::dvec3 normal;
  double distance = 0.0;
  glm::dvec3 mn, mx;
  eng::FaceGroupSet groups;
  eng::MaterialPtr material;
  eng::FaceTexBasis basis;
};

quint32 partOf(const eng::FaceGroupSet &groups) {
  for (const quint32 g: groups)
    if (g >= eng::facegroup::kStairFlat && g <= eng::facegroup::kStairStringer)
      return g;
  return 0;
}

// The texture size the step mapping measures texels in.
void texSizeOf(const eng::MaterialPtr &m, int &w, int &h) {
  w = m && m->texWidth > 0 ? m->texWidth : 512;
  h = m && m->texHeight > 0 ? m->texHeight : w;
}

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

void bounds(const std::vector<glm::dvec3> &poly, glm::dvec3 &mn, glm::dvec3 &mx) {
  mn = glm::dvec3(1e300);
  mx = glm::dvec3(-1e300);
  for (const auto &p: poly) {
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
  }
}

} // namespace

std::vector<quint32> Engine3D::procMembers(const quint32 id) const {
  std::vector<quint32> out;
  if (!m_scene || id == 0)
    return out;
  for (const auto &o: m_scene->gameObjects)
    if (o->procObject() == id)
      out.push_back(o->stableId());
  return out;
}

quint32 Engine3D::selectedProcObject() const {
  if (!m_scene || m_selectedObjects.empty())
    return 0;
  quint32 id = 0;
  for (const quint32 sid: m_selectedObjects) {
    const auto o = m_scene->findById(sid);
    if (!o || o->procObject() == 0 || (id != 0 && o->procObject() != id))
      return 0;
    id = o->procObject();
  }
  return m_scene->findProc(id) ? id : 0;
}

QString Engine3D::procType(const quint32 id) const {
  const auto *p = m_scene ? m_scene->findProc(id) : nullptr;
  return p ? p->type : QString();
}

QString Engine3D::procName(const quint32 id) const {
  const auto *p = m_scene ? m_scene->findProc(id) : nullptr;
  return p ? p->name : QString();
}

QJsonObject Engine3D::procParams(const quint32 id) const {
  const auto *p = m_scene ? m_scene->findProc(id) : nullptr;
  return p ? p->params : QJsonObject();
}

bool Engine3D::procMembersBounds(const quint32 id, glm::dvec3 &mn, glm::dvec3 &mx) const {
  return objectsBounds(procMembers(id), mn, mx);
}

namespace {
glm::dvec3 vec3Of(const QJsonValue &v, bool &ok) {
  const QJsonArray a = v.toArray();
  ok = a.size() == 3;
  return ok ? glm::dvec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : glm::dvec3(0.0);
}
QJsonArray jsonVec(const glm::dvec3 &v) { return QJsonArray {v.x, v.y, v.z}; }
} // namespace

QJsonObject Engine3D::procParamsForBounds(const quint32 id, const glm::dvec3 &boundsMin,
                                          const glm::dvec3 &boundsMax) const {
  // Brushes built with builtMin/Max now have (or will get) boundsMin/Max: the stairs follow (the box for plain
  // stairs, the placement for angled ones).
  const auto *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po)
    return {};
  QJsonObject params = po->params;
  bool ok1, ok2;
  const glm::dvec3 gmn = vec3Of(params.value(QStringLiteral("builtMin")), ok1);
  const glm::dvec3 gmx = vec3Of(params.value(QStringLiteral("builtMax")), ok2);
  glm::dvec3 f(1.0);
  if (ok1 && ok2)
    for (int a = 0; a < 3; ++a) {
      const double ge = gmx[a] - gmn[a];
      f[a] = ge > 1e-6 ? (boundsMax[a] - boundsMin[a]) / ge : 1.0;
    }
  if (eng::ShapeKind kind; eng::shapeKindOf(po->type, kind)) {
    // A shape's box goes where the brushes' bounds go, scaled with them.
    glm::dvec3 smn, smx;
    eng::ShapeParams sp;
    eng::shapeFromJson(params, smn, smx, sp);
    if (!(ok1 && ok2)) {
      smn = boundsMin;
      smx = boundsMax;
    } else {
      const glm::dvec3 bmn = smn, bmx = smx;
      for (int a = 0; a < 3; ++a) {
        smn[a] = boundsMin[a] + (bmn[a] - gmn[a]) * f[a];
        smx[a] = boundsMin[a] + (bmx[a] - gmn[a]) * f[a];
      }
    }
    const QJsonObject placed = eng::shapeToJson(smn, smx, sp);
    params[QStringLiteral("min")] = placed.value(QStringLiteral("min"));
    params[QStringLiteral("max")] = placed.value(QStringLiteral("max"));
    return params;
  }
  if (po->type == kArchType) {
    // The arch's box goes where the brushes' bounds go, scaled with them.
    eng::ArchPlacement pl;
    eng::ArchParams ap;
    eng::archFromJson(params, pl, ap);
    if (!(ok1 && ok2)) {
      pl.mn = boundsMin;
      pl.mx = boundsMax;
    } else {
      const glm::dvec3 bmn = pl.mn, bmx = pl.mx;
      for (int a = 0; a < 3; ++a) {
        pl.mn[a] = boundsMin[a] + (bmn[a] - gmn[a]) * f[a];
        pl.mx[a] = boundsMin[a] + (bmx[a] - gmn[a]) * f[a];
      }
    }
    const QJsonObject placed = eng::archToJson(pl, ap);
    params[QStringLiteral("min")] = placed.value(QStringLiteral("min"));
    params[QStringLiteral("max")] = placed.value(QStringLiteral("max"));
    return params;
  }
  eng::stairs::Params sp = eng::stairs::fromJson(params);
  if (!(ok1 && ok2)) {
    sp.mn = boundsMin;
    sp.mx = boundsMax;
  } else if (eng::stairs::isIdentity(sp.xform)) {
    const glm::dvec3 bmn = sp.mn, bmx = sp.mx;
    for (int a = 0; a < 3; ++a) {
      sp.mn[a] = boundsMin[a] + (bmn[a] - gmn[a]) * f[a];
      sp.mx[a] = boundsMin[a] + (bmx[a] - gmn[a]) * f[a];
    }
  } else {
    const glm::dmat4 map = glm::translate(glm::dmat4(1.0), boundsMin) * glm::scale(glm::dmat4(1.0), f)
                           * glm::translate(glm::dmat4(1.0), -gmn);
    sp.xform = map * sp.xform;
  }
  const QJsonObject placed = eng::stairs::toJson(sp);
  params[QStringLiteral("min")] = placed.value(QStringLiteral("min"));
  params[QStringLiteral("max")] = placed.value(QStringLiteral("max"));
  if (placed.contains(QStringLiteral("xform")))
    params[QStringLiteral("xform")] = placed.value(QStringLiteral("xform"));
  else
    params.remove(QStringLiteral("xform"));
  return params;
}

QJsonObject Engine3D::procCurrentParams(const quint32 id) const {
  const auto *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po)
    return {};
  glm::dvec3 lo, hi;
  const int expected = po->params.value(QStringLiteral("brushes")).toInt(-1);
  if (int(procMembers(id).size()) == expected && procMembersBounds(id, lo, hi))
    return procParamsForBounds(id, lo, hi);
  return po->params;
}

bool Engine3D::procBox(const quint32 id, glm::dvec3 &mn, glm::dvec3 &mx) const {
  const QJsonObject p = procCurrentParams(id);
  if (p.isEmpty())
    return false;
  if (procType(id) == kArchType) {
    eng::ArchPlacement pl;
    eng::ArchParams ap;
    eng::archFromJson(p, pl, ap);
    mn = pl.mn;
    mx = pl.mx;
    return true;
  }
  if (eng::ShapeKind kind; eng::shapeKindOf(procType(id), kind)) {
    eng::ShapeParams sp;
    eng::shapeFromJson(p, mn, mx, sp);
    return true;
  }
  const eng::stairs::Params sp = eng::stairs::fromJson(p);
  mn = sp.mn;
  mx = sp.mx;
  return true;
}

bool Engine3D::setProcParams(const quint32 id, const QJsonObject &params, const QString &label) {
  return rebuildProc(id, params, label, false);
}

void Engine3D::pushProcTable(const QString &label, const std::vector<eng::Scene::ProcObject> &before) {
  if (!m_undoStack || m_undoStack->isApplying())
    return;
  auto apply = [this](const std::vector<eng::Scene::ProcObject> &t) {
    if (!m_scene)
      return;
    m_scene->procObjects = t;
    emit procObjectsChanged();
  };
  const auto after = m_scene->procObjects;
  m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, before] { apply(before); },
                                                         [apply, after] { apply(after); }));
}

bool Engine3D::rebuildProc(const quint32 id, QJsonObject params, const QString &label, const bool boxFromMembers,
                           const std::vector<quint32> &alsoRemove) {
  if (!m_scene)
    return false;
  eng::Scene::ProcObject *po = m_scene->findProc(id);
  eng::ShapeKind shapeKind = eng::ShapeKind::Cylinder;
  const bool shape = po && eng::shapeKindOf(po->type, shapeKind);
  if (!po || (po->type != kStairsType && po->type != kArchType && !shape))
    return false;
  const bool stairs = po->type == kStairsType;
  const std::vector<quint32> oldIds = procMembers(id);

  // Moved brushes carry the stairs with them: rebuilt where they are now.
  if (boxFromMembers) {
    const QJsonObject cur = procCurrentParams(id);
    for (const char *key: {"min", "max"})
      params[QLatin1String(key)] = cur.value(QLatin1String(key));
    if (cur.contains(QStringLiteral("xform")))
      params[QStringLiteral("xform")] = cur.value(QStringLiteral("xform"));
    else
      params.remove(QStringLiteral("xform"));
  }
  eng::stairs::Params sp = eng::stairs::fromJson(params);

  // What the user put on the old brushes.
  std::vector<OldFace> oldFaces;
  std::map<quint32, std::map<QString, int>> partVotes;
  std::map<QString, eng::MaterialPtr> byName;
  const eng::GameObject *meta = nullptr;
  for (const quint32 sid: oldIds) {
    const auto o = m_scene->findById(sid);
    if (!o || isCollisionBrush(*o))
      continue;
    if (!meta)
      meta = o.get();
    const auto brush = eng::ConvexBrush::fromObject(*o);
    if (!brush)
      continue;
    for (size_t i = 0; i < brush->faces.size() && i < brush->polygons.size(); ++i) {
      const eng::BrushFace &f = brush->faces[i];
      OldFace of {f.normal, f.distance, {}, {}, f.groups, f.material, f.basis};
      bounds(brush->polygons[i], of.mn, of.mx);
      oldFaces.push_back(of);
      if (const quint32 part = partOf(f.groups); part && f.material) {
        ++partVotes[part][f.material->name];
        byName[f.material->name] = f.material;
      }
    }
  }

  QString matName = params.value(QStringLiteral("material")).toString();
  if (matName.isEmpty())
    matName = m_currentMaterial.isEmpty() ? prefs::s("Brush/DefaultMaterial") : m_currentMaterial;
  const auto mat = TextureManager::instance().materialFor(matName);
  const double texW = mat && mat->texWidth > 0 ? mat->texWidth : 512.0;
  const double tpu = kSourceUnitsPerWorldUnit / (texW * prefs::d("Brush/DefaultTextureScale"));
  eng::ArchPlacement archPlace;
  eng::ArchParams archParams;
  glm::dvec3 shapeMin(0.0), shapeMax(0.0);
  eng::ShapeParams shapeParams;
  shapeParams.kind = shapeKind;
  if (shape)
    eng::shapeFromJson(params, shapeMin, shapeMax, shapeParams);
  else if (!stairs)
    eng::archFromJson(params, archPlace, archParams);
  std::vector<eng::ConvexBrush> brushes =
      stairs  ? eng::stairs::build(sp, mat, tpu)
      : shape ? eng::buildShape(shapeMin, shapeMax, shapeParams, mat, tpu)
              : eng::buildArch(archPlace.mn, archPlace.mx, archPlace.spanAxis, archParams, mat, tpu);
  if (brushes.empty())
    return false;

  std::map<quint32, eng::MaterialPtr> partMaterial;
  for (const auto &[part, votes]: partVotes) {
    int best = 0;
    for (const auto &[name, n]: votes)
      if (n > best) {
        best = n;
        partMaterial[part] = byName[name];
      }
  }
  for (auto &b: brushes)
    for (size_t i = 0; i < b.faces.size() && i < b.polygons.size(); ++i) {
      eng::BrushFace &f = b.faces[i];
      const quint32 part = partOf(f.groups);
      glm::dvec3 fmn, fmx;
      bounds(b.polygons[i], fmn, fmx);
      const OldFace *match = nullptr;
      double bestArea = 0.0;
      for (const OldFace &of: oldFaces) {
        if (glm::dot(of.normal, f.normal) < 0.999 || std::fabs(of.distance - f.distance) > 0.01)
          continue;
        const double a = planarOverlap(f.normal, fmn, fmx, of.mn, of.mx);
        if (a > bestArea) {
          bestArea = a;
          match = &of;
        }
      }
      if (match) {
        if (match->material) {
          f.material = match->material;
          f.basis = match->basis;
        }
        for (const quint32 g: match->groups)
          if (g >= eng::facegroup::kFirstUser)
            eng::facegroup::insert(f.groups, g);
      } else if (const auto it = partMaterial.find(part); it != partMaterial.end() && it->second) {
        f.material = it->second;
        const double w = it->second->texWidth > 0 ? it->second->texWidth : 512.0;
        f.basis = eng::paraxialBasis(f.normal, kSourceUnitsPerWorldUnit / (w * prefs::d("Brush/DefaultTextureScale")));
      }
    }

  // Parts with a step mapping get it on every face, whatever the faces were.
  for (int pi = 0; stairs && pi < int(eng::stairs::Part::Count); ++pi) {
    const auto part = eng::stairs::Part(pi);
    const eng::stairs::StepTex t = eng::stairs::stepTexOf(params, part);
    if (!t.on)
      continue;
    const eng::MaterialPtr tmat = t.material.isEmpty() ? nullptr : TextureManager::instance().materialFor(t.material);
    for (auto &b: brushes)
      for (size_t i = 0; i < b.faces.size() && i < b.polygons.size(); ++i) {
        eng::BrushFace &f = b.faces[i];
        if (!eng::facegroup::contains(f.groups, quint32(eng::stairs::groupOfPart(part))))
          continue;
        if (tmat)
          f.material = tmat;
        int w, h;
        texSizeOf(f.material, w, h);
        f.basis = eng::stairs::stepTexBasis(sp, f.normal, b.polygons[i], t, w, h);
      }
  }

  std::vector<std::shared_ptr<eng::GameObject>> objs;
  for (const auto &b: brushes)
    if (auto o = objectFromBrush(b, meta)) {
      o->setProcObject(id);
      if (!meta)
        o->setName(po->name);
      if (!stairs)
        o->setSmoothShading(shape ? shapeParams.smooth : archParams.smooth);
      objs.push_back(o);
    }

  // The walkable ramp, tied to this object's own func_clip entity (a new
  // one when it has none yet, or shares it with a copy or a detached brush).
  std::shared_ptr<eng::GameObject> ramp;
  if (stairs) {
    const auto cmat = collisionMaterial();
    const double cw = cmat && cmat->texWidth > 0 ? cmat->texWidth : 512.0;
    const double ctpu = kSourceUnitsPerWorldUnit / (cw * prefs::d("Brush/DefaultTextureScale"));
    if (const auto rb = eng::stairs::buildCollision(sp, cmat, ctpu))
      if ((ramp = objectFromBrush(*rb, meta))) {
        ramp->setProcObject(id);
        if (!meta)
          ramp->setName(po->name);
        objs.push_back(ramp);
      }
  }
  const quint32 oldEntity = quint32(po->params.value(QStringLiteral("collisionEntity")).toDouble(0));
  quint32 entity = 0;
  if (ramp && oldEntity) {
    const eng::Entity *e = entityById(oldEntity);
    bool own = e && e->classname == eng::kClipClass;
    for (const quint32 b: own ? brushesOfEntity(oldEntity) : std::vector<quint32> {})
      if (const auto o = m_scene->findById(b); o && o->procObject() != id)
        own = false;
    if (own)
      entity = oldEntity;
  }

  QJsonObject stored = stairs  ? eng::stairs::toJson(sp)
                       : shape ? eng::shapeToJson(shapeMin, shapeMax, shapeParams)
                               : eng::archToJson(archPlace, archParams);
  stored[QStringLiteral("material")] = matName;
  stored[QStringLiteral("brushes")] = int(objs.size());
  if (params.contains(QStringLiteral("tex")))
    stored[QStringLiteral("tex")] = params.value(QStringLiteral("tex"));
  {
    glm::dvec3 gmn(1e300), gmx(-1e300);
    for (const auto &o: objs) {
      if (o == ramp)
        continue;
      const auto [lo, hi] = o->getWorldAABB();
      gmn = glm::min(gmn, glm::dvec3(lo));
      gmx = glm::max(gmx, glm::dvec3(hi));
    }
    stored[QStringLiteral("builtMin")] = jsonVec(gmn);
    stored[QStringLiteral("builtMax")] = jsonVec(gmx);
  }

  const auto tableBefore = m_scene->procObjects;
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  if (ramp) {
    if (!entity) {
      const auto [lo, hi] = ramp->getWorldAABB();
      entity = addEntity(eng::kClipClass, (lo + hi) * 0.5f);
      po = m_scene->findProc(id);
    }
    ramp->setBrushEntity(entity);
    stored[QStringLiteral("collisionEntity")] = double(entity);
  }
  po->params = stored;
  pushProcTable(label, tableBefore);
  std::vector<quint32> remove = oldIds;
  remove.insert(remove.end(), alsoRemove.begin(), alsoRemove.end());
  replaceObjects(remove, objs, label);
  if (oldEntity && oldEntity != entity && entityById(oldEntity) && brushesOfEntity(oldEntity).empty())
    deleteEntities({oldEntity});
  syncBrushEntityOrigins();
  if (record)
    m_undoStack->endMacro();
  emit procObjectsChanged();
  return true;
}

quint32 Engine3D::createStairs(const glm::dvec3 &mn, const glm::dvec3 &mx, const int direction,
                               const std::vector<quint32> &replace, const QString &material) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  if (mx.x - mn.x < 1e-3 || mx.y - mn.y < 1e-3 || mx.z - mn.z < 1e-3)
    return 0;
  eng::stairs::Params sp;
  sp.mn = mn;
  sp.mx = mx;
  sp.direction = std::clamp(direction, 0, 3);
  sp.sizing = eng::stairs::Sizing(std::clamp(prefs::i("Brush/StairSizing"), 0, 1));
  sp.stepHeight = prefs::d("Brush/StairStepHeight");
  sp.stepDepth = prefs::d("Brush/StairStepDepth");
  sp.steps = std::clamp(prefs::i("Brush/StairSteps"), 1, 256);
  sp.underside = eng::stairs::Underside(std::clamp(prefs::i("Brush/StairStyle"), 0, 2));
  sp.thickness = prefs::d("Brush/StairThickness");
  sp.thinPlanks = prefs::b("Brush/StairThinPlanks");
  sp.plankThickness = prefs::d("Brush/StairPlankThickness");
  sp.nosing = prefs::b("Brush/StairNosing");
  sp.nosingDepth = prefs::d("Brush/StairNosingDepth");
  sp.nosingHeight = prefs::d("Brush/StairNosingHeight");
  sp.collision = prefs::b("Brush/StairCollision");
  sp.sides = prefs::b("Brush/StairSides");
  sp.sideWidth = prefs::d("Brush/StairSideWidth");
  sp.sideHeight = prefs::d("Brush/StairSideHeight");
  QJsonObject params = eng::stairs::toJson(sp);
  if (!material.isEmpty())
    params[QStringLiteral("material")] = material;

  const auto tableBefore = m_scene->procObjects;
  eng::Scene::ProcObject po;
  po.id = m_scene->mintProcId();
  po.type = kStairsType;
  po.name = nextProcName(m_scene->procObjects, kStairsType);
  po.params = params;

  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(QStringLiteral("Create Stairs"));
  m_scene->procObjects.push_back(po);
  pushProcTable(QStringLiteral("Create Stairs"), tableBefore);
  const bool ok = rebuildProc(po.id, params, QStringLiteral("Create Stairs"), false, replace);
  if (record)
    m_undoStack->endMacro();
  if (!ok) {
    m_scene->procObjects = tableBefore;
    emit procObjectsChanged();
    return 0;
  }
  return po.id;
}

quint32 Engine3D::createArch(const glm::dvec3 &mn, const glm::dvec3 &mx, const int spanAxis,
                             const eng::ArchParams &arch) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  if (mx.x - mn.x < 1e-3 || mx.y - mn.y < 1e-3 || mx.z - mn.z < 1e-3)
    return 0;
  eng::ArchPlacement pl;
  pl.mn = mn;
  pl.mx = mx;
  pl.spanAxis = spanAxis == 2 ? 2 : 0;
  const QJsonObject params = eng::archToJson(pl, arch);
  const auto tableBefore = m_scene->procObjects;
  eng::Scene::ProcObject po;
  po.id = m_scene->mintProcId();
  po.type = kArchType;
  po.name = nextProcName(m_scene->procObjects, kArchType);
  po.params = params;
  const QString label = QStringLiteral("Create Arch");
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  m_scene->procObjects.push_back(po);
  pushProcTable(label, tableBefore);
  const bool ok = rebuildProc(po.id, params, label, false);
  if (record)
    m_undoStack->endMacro();
  if (!ok) {
    m_scene->procObjects = tableBefore;
    emit procObjectsChanged();
    return 0;
  }
  return po.id;
}

quint32 Engine3D::createShape(const glm::dvec3 &mn, const glm::dvec3 &mx, const eng::ShapeParams &shape) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  if (mx.x - mn.x < 1e-3 || mx.y - mn.y < 1e-3 || mx.z - mn.z < 1e-3)
    return 0;
  const QString type = eng::shapeTypeName(shape.kind);
  const QJsonObject params = eng::shapeToJson(mn, mx, shape);
  const auto tableBefore = m_scene->procObjects;
  eng::Scene::ProcObject po;
  po.id = m_scene->mintProcId();
  po.type = type;
  po.name = nextProcName(m_scene->procObjects, type);
  po.params = params;
  const QString label = QStringLiteral("Create %1").arg(po.name.section(QLatin1Char(' '), 0, 0));
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  m_scene->procObjects.push_back(po);
  pushProcTable(label, tableBefore);
  const bool ok = rebuildProc(po.id, params, label, false);
  if (record)
    m_undoStack->endMacro();
  if (!ok) {
    m_scene->procObjects = tableBefore;
    emit procObjectsChanged();
    return 0;
  }
  return po.id;
}

bool Engine3D::stairsFromMarqueeOrSelection() {
  if (m_marquee.valid) {
    glm::dvec3 mn, mx;
    marqueeToWorld(m_marquee, mn, mx);
    int dir = std::clamp(prefs::i("Brush/StairDirection"), 0, 3);
    if (m_marquee.plane == 1)
      dir = 0;
    else if (m_marquee.plane == 2)
      dir = 3;
    quint32 id = 0;
    withMirror(QStringLiteral("Create Stairs"), [&] {
      id = createStairs(mn, mx, dir, {}, {});
      return id != 0;
    });
    if (id)
      clearMarquee();
    return id != 0;
  }
  // Turn the selected brushes into stairs filling their bounds.
  if (m_selectedObjects.empty() || selectedProcObject() != 0)
    return false;
  glm::dvec3 mn(1e300), mx(-1e300);
  std::vector<quint32> ids;
  std::map<QString, int> votes;
  for (const quint32 sid: m_selectedObjects) {
    const auto o = m_scene->findById(sid);
    if (!o || !o->getMesh() || o->isDerived())
      continue;
    const auto [lo, hi] = o->getWorldAABB();
    mn = glm::min(mn, glm::dvec3(lo));
    mx = glm::max(mx, glm::dvec3(hi));
    ids.push_back(sid);
    for (int f = 0; f < o->getMesh()->faceCount(); ++f)
      if (const auto m = o->getMesh()->faceMaterialRef(f))
        ++votes[m->name];
  }
  if (ids.empty())
    return false;
  QString material;
  int best = 0;
  for (const auto &[name, n]: votes)
    if (n > best) {
      best = n;
      material = name;
    }
  // Climb along the longer horizontal side.
  const int dir = (mx.x - mn.x) >= (mx.z - mn.z) ? 0 : 2;
  return createStairs(mn, mx, dir, ids, material) != 0;
}

bool Engine3D::setProcParam(const quint32 id, const QString &key, const QJsonValue &value) {
  const auto *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po || po->params.value(key) == value)
    return false;
  QJsonObject params = po->params;
  params[key] = value;
  const bool ok = rebuildProc(id, params, QStringLiteral("Edit %1").arg(po->name), true);
  if (ok && po->type == kStairsType) {
    if (key == QLatin1String("sizing"))
      prefs::get().setValue(QStringLiteral("Brush/StairSizing"), value.toInt());
    else if (key == QLatin1String("stepHeight"))
      prefs::get().setValue(QStringLiteral("Brush/StairStepHeight"), value.toDouble());
    else if (key == QLatin1String("stepDepth"))
      prefs::get().setValue(QStringLiteral("Brush/StairStepDepth"), value.toDouble());
    else if (key == QLatin1String("steps"))
      prefs::get().setValue(QStringLiteral("Brush/StairSteps"), value.toInt());
    else if (key == QLatin1String("underside"))
      prefs::get().setValue(QStringLiteral("Brush/StairStyle"), value.toInt());
    else if (key == QLatin1String("thickness"))
      prefs::get().setValue(QStringLiteral("Brush/StairThickness"), value.toDouble());
    else if (key == QLatin1String("direction"))
      prefs::get().setValue(QStringLiteral("Brush/StairDirection"), value.toInt());
    else if (key == QLatin1String("thinPlanks"))
      prefs::get().setValue(QStringLiteral("Brush/StairThinPlanks"), value.toBool());
    else if (key == QLatin1String("plankThickness"))
      prefs::get().setValue(QStringLiteral("Brush/StairPlankThickness"), value.toDouble());
    else if (key == QLatin1String("nosing"))
      prefs::get().setValue(QStringLiteral("Brush/StairNosing"), value.toBool());
    else if (key == QLatin1String("nosingDepth"))
      prefs::get().setValue(QStringLiteral("Brush/StairNosingDepth"), value.toDouble());
    else if (key == QLatin1String("nosingHeight"))
      prefs::get().setValue(QStringLiteral("Brush/StairNosingHeight"), value.toDouble());
    else if (key == QLatin1String("collision"))
      prefs::get().setValue(QStringLiteral("Brush/StairCollision"), value.toBool());
    else if (key == QLatin1String("sides"))
      prefs::get().setValue(QStringLiteral("Brush/StairSides"), value.toBool());
    else if (key == QLatin1String("sideWidth"))
      prefs::get().setValue(QStringLiteral("Brush/StairSideWidth"), value.toDouble());
    else if (key == QLatin1String("sideHeight"))
      prefs::get().setValue(QStringLiteral("Brush/StairSideHeight"), value.toDouble());
  } else if (ok && po->type == kArchType) {
    // The next arch starts like this one.
    if (key == QLatin1String("segments"))
      prefs::get().setValue(QStringLiteral("Brush/ArchSegments"), value.toInt());
    else if (key == QLatin1String("profile"))
      prefs::get().setValue(QStringLiteral("Brush/ArchProfile"), value.toInt());
    else if (key == QLatin1String("mode"))
      prefs::get().setValue(QStringLiteral("Brush/ArchMode"), value.toInt());
    else if (key == QLatin1String("thickness"))
      prefs::get().setValue(QStringLiteral("Brush/ArchThickness"), value.toDouble() * kSourceUnitsPerWorldUnit);
  }
  return ok;
}

bool Engine3D::setProcBox(const quint32 id, const glm::dvec3 &mn, const glm::dvec3 &mx, const QString &label) {
  const auto *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po || mx.x - mn.x < 1e-3 || mx.y - mn.y < 1e-3 || mx.z - mn.z < 1e-3)
    return false;
  QJsonObject params = procCurrentParams(id);
  const QJsonArray bmin {mn.x, mn.y, mn.z}, bmax {mx.x, mx.y, mx.z};
  params[QStringLiteral("min")] = bmin;
  params[QStringLiteral("max")] = bmax;
  return rebuildProc(id, params, label, false);
}

bool Engine3D::fitStairsDepth(const quint32 id) {
  const auto *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po || po->type != kStairsType)
    return false;
  glm::dvec3 mn, mx;
  if (!procBox(id, mn, mx))
    return false;
  eng::stairs::Params sp = eng::stairs::fromJson(po->params);
  sp.mn = mn;
  sp.mx = mx;
  const int a = eng::stairs::climbAxis(sp.direction);
  const double len = eng::stairs::stepCount(sp) * sp.stepDepth / kSourceUnitsPerWorldUnit;
  // Keep the bottom step where it is.
  if (sp.direction == 0 || sp.direction == 2)
    mx[a] = mn[a] + len;
  else
    mn[a] = mx[a] - len;
  return setProcBox(id, mn, mx, QStringLiteral("Fit stairs to tread depth"));
}

bool Engine3D::dissolveProc(const quint32 id) {
  if (!m_scene || !m_scene->findProc(id))
    return false;
  const auto members = procMembers(id);
  const auto tableBefore = m_scene->procObjects;
  const QString label = QStringLiteral("Convert to brushes");
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->beginMacro(label);
  std::vector<std::shared_ptr<eng::GameObject>> objs;
  for (const quint32 sid: members)
    if (const auto o = m_scene->findById(sid))
      if (auto b = eng::ConvexBrush::fromObject(*o))
        if (auto c = objectFromBrush(*b, o.get()))
          objs.push_back(c);
  m_scene->procObjects.erase(std::remove_if(m_scene->procObjects.begin(), m_scene->procObjects.end(),
                                            [id](const eng::Scene::ProcObject &p) { return p.id == id; }),
                             m_scene->procObjects.end());
  pushProcTable(label, tableBefore);
  replaceObjects(members, objs, label);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->endMacro();
  emit procObjectsChanged();
  return true;
}

bool Engine3D::renameProc(const quint32 id, const QString &name) {
  eng::Scene::ProcObject *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po || name.trimmed().isEmpty() || po->name == name.trimmed())
    return false;
  const auto before = m_scene->procObjects;
  po->name = name.trimmed();
  pushProcTable(QStringLiteral("Rename %1").arg(po->name), before);
  emit procObjectsChanged();
  return true;
}

void Engine3D::cloneProcObjects(const std::vector<quint32> &originals,
                                const std::vector<std::shared_ptr<eng::GameObject>> &clones, DuplicateSet &set) {
  if (!m_scene)
    return;
  std::map<quint32, quint32> remap, entityRemap;
  const auto before = m_scene->procObjects;
  for (size_t k = 0; k < originals.size() && k < clones.size(); ++k) {
    const auto src = m_scene->findById(originals[k]);
    const quint32 pid = src ? src->procObject() : 0;
    const eng::Scene::ProcObject *po = pid ? m_scene->findProc(pid) : nullptr;
    if (!po)
      continue;
    auto it = remap.find(pid);
    if (it == remap.end()) {
      eng::Scene::ProcObject copy = *po;
      copy.id = m_scene->mintProcId();
      const quint32 oldEntity = quint32(copy.params.value(QStringLiteral("collisionEntity")).toDouble(0));
      if (const eng::Entity *e = oldEntity ? entityById(oldEntity) : nullptr) {
        const quint32 eid = addCollisionEntityRaw(e->origin);
        copy.params[QStringLiteral("collisionEntity")] = double(eid);
        entityRemap[oldEntity] = eid;
        set.entitiesAdded.push_back(*entityById(eid));
      }
      copy.name = nextProcName(m_scene->procObjects, copy.type);
      m_scene->procObjects.push_back(copy);
      it = remap.emplace(pid, copy.id).first;
    }
    clones[k]->setProcObject(it->second);
    if (const auto e = entityRemap.find(clones[k]->brushEntity()); e != entityRemap.end())
      clones[k]->setBrushEntity(e->second);
    clones[k]->setName(m_scene->findProc(it->second)->name);
  }
  if (remap.empty())
    return;
  if (!set.procAdded) {
    set.procAdded = true;
    set.procBefore = before;
  }
  emit procObjectsChanged();
}

bool Engine3D::regenerateResizedProc() {
  // Called when a resize of exactly one generated object's brushes ends: put
  // the brushes back as they were and rebuild the object in the new bounds.
  const quint32 id = selectedProcObject();
  if (!id || procMembers(id).size() != m_selectedObjects.size())
    return false;
  glm::dvec3 mn, mx;
  if (!procMembersBounds(id, mn, mx))
    return false;
  const QJsonObject params = procParamsForBounds(id, mn, mx);
  for (size_t k = 0; k < m_dragObjects.size(); ++k)
    if (const auto o = m_scene->findById(m_dragObjects[k])) {
      if (k < m_dragBasePos.size())
        o->setPosition(m_dragBasePos[k]);
      if (k < m_dragBaseScale.size())
        o->setScale(m_dragBaseScale[k]);
      if (const auto mesh = o->getMesh(); mesh && k < m_dragBaseUv.size())
        for (int f = 0; f < mesh->faceCount() && f < int(m_dragBaseUv[k].bases.size()); ++f)
          mesh->setFaceTexBasis(f, m_dragBaseUv[k].bases[size_t(f)]);
      o->computeWorldAABB();
    }
  return rebuildProc(id, params, QStringLiteral("Resize %1").arg(procName(id)), false);
}

std::vector<quint32> Engine3D::transformProcObjects(const glm::dmat4 &m, const QString &label,
                                                   std::set<quint32> &handled) {
  // Generated objects whose every brush is selected are transformed through
  // their parameters, so they stay generated. Returns the objects rebuilt and
  // fills `handled` with the brushes they replaced.
  std::vector<quint32> done;
  if (!m_scene)
    return done;
  std::set<quint32> procs;
  for (const quint32 sid: m_selectedObjects)
    if (const auto o = m_scene->findById(sid); o && o->procObject())
      procs.insert(o->procObject());
  for (const quint32 id: procs) {
    const auto members = procMembers(id);
    const bool whole = std::all_of(members.begin(), members.end(),
                                   [this](quint32 sid) { return m_selectedObjects.count(sid) > 0; });
    if (!whole)
      continue;
    if (eng::ShapeKind kind; eng::shapeKindOf(procType(id), kind)) {
      // A shape stays square to the world: turns that keep its axis on a world axis move its box (and axis); any
      // other turn leaves it to the plain brush transform.
      QJsonObject params = procCurrentParams(id);
      glm::dvec3 smn, smx;
      eng::ShapeParams sp;
      eng::shapeFromJson(params, smn, smx, sp);
      glm::dvec3 ax(0.0);
      ax[sp.axis] = 1.0;
      ax = glm::dmat3(m) * ax;
      if (glm::length(ax) < 1e-9)
        continue;
      ax = glm::normalize(ax);
      const int newAxis = std::fabs(ax.x) > 0.999 ? 0 : std::fabs(ax.y) > 0.999 ? 1 : std::fabs(ax.z) > 0.999 ? 2 : -1;
      if (newAxis < 0)
        continue;
      glm::dvec3 lo(1e300), hi(-1e300);
      for (int c = 0; c < 8; ++c) {
        const glm::dvec3 corner((c & 1) ? smx.x : smn.x, (c & 2) ? smx.y : smn.y, (c & 4) ? smx.z : smn.z);
        const glm::dvec3 w(m * glm::dvec4(corner, 1.0));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
      }
      sp.axis = newAxis;
      const QJsonObject placed = eng::shapeToJson(lo, hi, sp);
      for (const char *key: {"min", "max", "axis"})
        params[QLatin1String(key)] = placed.value(QLatin1String(key));
      if (rebuildProc(id, params, label, false)) {
        handled.insert(members.begin(), members.end());
        done.push_back(id);
      }
      continue;
    }
    if (procType(id) == kArchType) {
      // An arch stays upright and square to the world: quarter turns about
      // the vertical (and mirrors) move its box and swap its span axis; any
      // other turn leaves it to the plain brush transform.
      QJsonObject params = procCurrentParams(id);
      eng::ArchPlacement pl;
      eng::ArchParams ap;
      eng::archFromJson(params, pl, ap);
      const glm::dmat3 lin(m);
      const glm::dvec3 up = lin * glm::dvec3(0.0, 1.0, 0.0);
      glm::dvec3 span = lin * (pl.spanAxis == 0 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 0.0, 1.0));
      if (glm::length(span) < 1e-9 || glm::length(up) < 1e-9 || glm::normalize(up).y < 0.999)
        continue; // tipped over or upside down: not an arch any more
      span = glm::normalize(span);
      if (std::max(std::fabs(span.x), std::fabs(span.z)) < 0.999)
        continue;
      glm::dvec3 lo(1e300), hi(-1e300);
      for (int c = 0; c < 8; ++c) {
        const glm::dvec3 corner((c & 1) ? pl.mx.x : pl.mn.x, (c & 2) ? pl.mx.y : pl.mn.y, (c & 4) ? pl.mx.z : pl.mn.z);
        const glm::dvec3 w(m * glm::dvec4(corner, 1.0));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
      }
      pl.mn = lo;
      pl.mx = hi;
      pl.spanAxis = std::fabs(span.x) >= std::fabs(span.z) ? 0 : 2;
      const QJsonObject placed = eng::archToJson(pl, ap);
      for (const char *key: {"min", "max", "spanAxis"})
        params[QLatin1String(key)] = placed.value(QLatin1String(key));
      if (rebuildProc(id, params, label, false)) {
        handled.insert(members.begin(), members.end());
        done.push_back(id);
      }
      continue;
    }
    if (procType(id) != kStairsType)
      continue;
    QJsonObject params = procCurrentParams(id);
    eng::stairs::Params sp = eng::stairs::fromJson(params);
    eng::stairs::applyTransform(sp, m);
    const QJsonObject placed = eng::stairs::toJson(sp);
    for (const char *key: {"min", "max", "direction"})
      params[QLatin1String(key)] = placed.value(QLatin1String(key));
    if (placed.contains(QStringLiteral("xform")))
      params[QStringLiteral("xform")] = placed.value(QStringLiteral("xform"));
    else
      params.remove(QStringLiteral("xform"));
    if (rebuildProc(id, params, label, false)) {
      handled.insert(members.begin(), members.end());
      done.push_back(id);
    }
  }
  return done;
}

quint32 Engine3D::selectedFacesProcObject() const {
  if (!m_scene || m_selectedFaces.empty())
    return 0;
  quint32 id = 0;
  for (const auto &[sid, f]: selectedFaceRefs()) {
    const auto o = m_scene->findById(sid);
    if (!o || isProcRamp(*o))
      continue;
    if (o->procObject() == 0 || (id != 0 && o->procObject() != id))
      return 0;
    id = o->procObject();
  }
  return id && m_scene->findProc(id) ? id : 0;
}

int Engine3D::selectedStairParts(const quint32 id) const {
  int bits = 0;
  if (!m_scene || id == 0)
    return bits;
  for (const auto &[sid, f]: selectedFaceRefs()) {
    const auto o = m_scene->findById(sid);
    if (!o || o->procObject() != id || !o->getMesh())
      continue;
    for (const quint32 g: o->getMesh()->faceGroups(f))
      if (const auto part = eng::stairs::partOfGroup(int(g)); part != eng::stairs::Part::Count)
        bits |= 1 << int(part);
  }
  return bits;
}

eng::stairs::StepTex Engine3D::stairPartTexture(const quint32 id, const eng::stairs::Part part) const {
  const auto *po = m_scene ? m_scene->findProc(id) : nullptr;
  return po ? eng::stairs::stepTexOf(po->params, part) : eng::stairs::StepTex {};
}

bool Engine3D::setStairPartTexture(const quint32 id, const eng::stairs::Part part, const eng::stairs::StepTex &t,
                                   const QString &label, const bool force) {
  eng::Scene::ProcObject *po = m_scene ? m_scene->findProc(id) : nullptr;
  if (!po || po->type != kStairsType || part == eng::stairs::Part::Count)
    return false;
  if (!force && eng::stairs::stepTexOf(po->params, part) == t)
    return false;
  const eng::stairs::Params sp = eng::stairs::fromJson(po->params);
  const quint32 group = quint32(eng::stairs::groupOfPart(part));

  // The faces of the part, edited in place: the selection stays as it is.
  std::vector<std::shared_ptr<eng::GameObject>> owners;
  for (const quint32 sid: procMembers(id))
    if (const auto o = m_scene->findById(sid); o && o->getMesh() && !isProcRamp(*o))
      for (int f = 0; f < o->getMesh()->faceCount(); ++f)
        if (eng::facegroup::contains(o->getMesh()->faceGroups(f), group)) {
          owners.push_back(o);
          break;
        }

  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record)
    m_undoStack->beginMacro(label);
  const auto tableBefore = m_scene->procObjects;
  eng::stairs::setStepTex(po->params, part, t);
  pushProcTable(label, tableBefore);

  if (t.on) {
    std::vector<ObjectState> before, after;
    for (const auto &o: owners)
      before.push_back(captureObjectState(o->stableId(), true));
    const eng::MaterialPtr tmat = t.material.isEmpty() ? nullptr : TextureManager::instance().materialFor(t.material);
    for (const auto &o: owners) {
      auto &mesh = *o->getMesh();
      for (int f = 0; f < mesh.faceCount(); ++f) {
        if (!eng::facegroup::contains(mesh.faceGroups(f), group))
          continue;
        std::vector<glm::vec3> poly;
        glm::vec3 n;
        if (!worldFace(*o, f, poly, n))
          continue;
        if (tmat)
          mesh.setFaceMaterial(f, tmat);
        int w, h;
        texSizeOf(mesh.faceMaterialRef(f), w, h);
        const std::vector<glm::dvec3> dpoly(poly.begin(), poly.end());
        mesh.setFaceTexBasis(f, eng::stairs::stepTexBasis(sp, glm::dvec3(n), dpoly, t, w, h));
      }
    }
    for (const auto &o: owners)
      after.push_back(captureObjectState(o->stableId(), true));
    if (record)
      m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, std::move(before), std::move(after)));
  }
  if (record)
    m_undoStack->endMacro();
  emit procObjectsChanged();
  emit uvTransformChanged();
  emit objectStateChanged();
  emit sceneChanged();
  return true;
}

bool Engine3D::fitStairPartTexture(const quint32 id, const eng::stairs::Part part) {
  if (!m_scene || part == eng::stairs::Part::Count)
    return false;
  const quint32 group = quint32(eng::stairs::groupOfPart(part));
  // The material: most used by the selected faces of the part, else by the part.
  std::map<QString, int> selVotes, allVotes;
  std::set<std::pair<quint32, int>> selected;
  for (const auto &r: selectedFaceRefs())
    selected.insert(r);
  for (const quint32 sid: procMembers(id)) {
    const auto o = m_scene->findById(sid);
    if (!o || !o->getMesh() || isProcRamp(*o))
      continue;
    for (int f = 0; f < o->getMesh()->faceCount(); ++f) {
      const auto m = o->getMesh()->faceMaterialRef(f);
      if (!m || !eng::facegroup::contains(o->getMesh()->faceGroups(f), group))
        continue;
      const QString ref = m->ref.isEmpty() ? m->name : m->ref;
      ++allVotes[ref];
      if (selected.count({sid, f}))
        ++selVotes[ref];
    }
  }
  auto best = [](const std::map<QString, int> &votes) {
    QString out;
    int n = 0;
    for (const auto &[ref, c]: votes)
      if (c > n) {
        n = c;
        out = ref;
      }
    return out;
  };
  eng::stairs::StepTex t = stairPartTexture(id, part);
  const bool wasOn = t.on;
  t.on = true;
  t.material = best(selVotes.empty() ? allVotes : selVotes);
  if (!wasOn) {
    // Start from the default texture scale: one texture width per tile.
    const auto m = t.material.isEmpty() ? nullptr : TextureManager::instance().materialFor(t.material);
    int w, h;
    texSizeOf(m, w, h);
    t.tile = w * prefs::d("Brush/DefaultTextureScale");
  }
  // Forced: Fit also puts the mapping back on faces textured by hand since.
  return setStairPartTexture(id, part, t, QStringLiteral("Fit texture to steps"), true);
}

int Engine3D::selectStairPart(const quint32 id, const eng::stairs::Part part) {
  if (!m_scene || part == eng::stairs::Part::Count)
    return 0;
  const quint32 group = quint32(eng::stairs::groupOfPart(part));
  std::vector<quint32> faces;
  for (const quint32 sid: procMembers(id)) {
    const auto o = m_scene->findById(sid);
    if (!o || !o->getMesh() || o->hidden() || isProcRamp(*o))
      continue;
    for (int f = 0; f < o->getMesh()->faceCount(); ++f)
      if (eng::facegroup::contains(o->getMesh()->faceGroups(f), group) && o->getMesh()->getFacePickingID(f) != 0)
        faces.push_back(o->getMesh()->getFacePickingID(f));
  }
  clearAutoSelectRegion();
  setSelected({}, faces);
  return int(faces.size());
}

} // namespace vk
