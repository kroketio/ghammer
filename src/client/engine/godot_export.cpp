#include "godot_export.h"
#include "collision_hulls.h"
#include "smooth_units.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainterPath>
#include <QTransform>
#include <private/qtriangulator_p.h>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "brush.h"
#include "entity.h"
#include "face_graph.h"
#include "gameobject.h"
#include "godot_io_export.h"
#include "godot_text.h"
#include "gltf_writer.h"
#include "io.h"
#include "material.h"
#include "material_overrides.h"
#include "mesh.h"
#include "orm_compose.h"
#include "scene.h"

namespace eng::godot {

namespace {

const QString kMeta = QStringLiteral("metadata/level_editor_id");
// The name the export gave a node: a node still carrying it was not renamed in Godot, so it follows editor renames.
const QString kMetaName = QStringLiteral("metadata/level_editor_name");
const QString kIoMapScript = QStringLiteral("res://addons/ghammer/io_map.gd");

QString fileSafe(const QString &s) {
  QString out = s;
  out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
  return out.isEmpty() ? QStringLiteral("unnamed") : out;
}

QString res(const QString &rel) { return QStringLiteral("res://") + rel; }

// Entity ids live above kEntityIdBase; names use the short number.
quint32 shortId(quint32 id) { return id >= kEntityIdBase ? id - kEntityIdBase : id; }

const Entity *entityById(const Scene &scene, quint32 id) {
  for (const Entity &e: scene.entities)
    if (e.id == id)
      return &e;
  return nullptr;
}

// Source tool textures and the editor's own tools/* materials: never drawn.
bool toolMaterialName(const QString &name) {
  const QString n = name.toLower();
  if (n.startsWith(QLatin1String("tools/")))
    return true;
  static const char *kTools[] = {"nodraw", "skybox", "toolsclip", "playerclip", "npcclip", "toolshint", "toolsskip",
                                 "areaportal", "occluder", "toolsfog", "toolsinvisible", "toolstrigger"};
  for (const char *t: kTools)
    if (n.contains(QLatin1String(t)))
      return true;
  return false;
}

glm::dvec3 newell(const std::vector<glm::dvec3> &poly) {
  glm::dvec3 n(0.0);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 &a = poly[i];
    const glm::dvec3 &b = poly[(i + 1) % poly.size()];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  return n;
}

// An object's face centre and winding, from the sign of its volume over the stored winding (read, not assumed
// inward). Never per face: a bent modifier piece is not convex, so "away from the centre" can point inside.
// No volume (a single plane): fall back to the centre.
struct FaceFrame {
  glm::dmat4 model {1.0};
  glm::dvec3 centre {0.0};
  int sign = 0; // +1: stored winding is outward, -1: inward, 0: unknown
};

FaceFrame faceFrame(const GameObject &o, const Mesh &mesh) {
  FaceFrame fr;
  fr.model = glm::dmat4(o.getModelMatrix());
  std::vector<glm::vec3> local;
  std::vector<glm::dvec3> poly;
  glm::dvec3 sum(0.0), mn(1e300), mx(-1e300);
  size_t n = 0;
  double volume = 0.0;
  for (int f = 0; f < mesh.faceCount(); ++f) {
    if (!mesh.getFacePolygon(f, local) || local.size() < 3)
      continue;
    poly.clear();
    for (const glm::vec3 &p: local) {
      poly.push_back(glm::dvec3(fr.model * glm::dvec4(glm::dvec3(p), 1.0)));
      sum += poly.back();
      mn = glm::min(mn, poly.back());
      mx = glm::max(mx, poly.back());
      ++n;
    }
    volume += glm::dot(poly.front(), newell(poly)) / 6.0;
  }
  if (n)
    fr.centre = sum / double(n);
  const glm::dvec3 e = n ? mx - mn : glm::dvec3(0.0);
  const double boxVolume = e.x * e.y * e.z;
  if (std::fabs(volume) > 1e-6 * std::max(boxVolume, 1e-12))
    fr.sign = volume > 0.0 ? 1 : -1;
  return fr;
}

// One face in world space, counter-clockwise from outside (glTF front faces), by the object's winding.
bool worldFace(const Mesh &mesh, int f, const FaceFrame &frame, std::vector<glm::dvec3> &out, glm::dvec3 &normal) {
  std::vector<glm::vec3> local;
  if (!mesh.getFacePolygon(f, local) || local.size() < 3)
    return false;
  out.clear();
  glm::dvec3 faceCentre(0.0);
  for (const glm::vec3 &p: local) {
    out.push_back(glm::dvec3(frame.model * glm::dvec4(glm::dvec3(p), 1.0)));
    faceCentre += out.back();
  }
  faceCentre /= double(out.size());
  glm::dvec3 nw = newell(out);
  const bool flip = frame.sign != 0 ? frame.sign < 0 : glm::dot(nw, faceCentre - frame.centre) < 0.0;
  if (flip) {
    std::reverse(out.begin(), out.end());
    nw = -nw;
  }
  const double len = glm::length(nw);
  if (len < 1e-12)
    return false;
  normal = nw / len;
  return true;
}

struct Builder {
  const Scene &scene;
  const LevelOptions &opt;
  LevelPlan &plan;
  QHash<QString, int> materialByKey;  // Material::name -> plan.materials index
  QHash<QString, int> textureByRel;

  int texture(const TextureJob &t) {
    if (const auto it = textureByRel.constFind(t.rel); it != textureByRel.constEnd())
      return *it;
    plan.textures.push_back(t);
    textureByRel.insert(t.rel, int(plan.textures.size()) - 1);
    return int(plan.textures.size()) - 1;
  }

  int material(const MaterialPtr &m) {
    if (const auto it = materialByKey.constFind(m->name); it != materialByKey.constEnd())
      return *it;
    MaterialJob job;
    job.ref = m->ref;
    const MaterialRef r = MaterialRef::parse(m->ref);
    const TextureEntry *entry = nullptr;
    TextureEntry standIn;
    std::array<TextureJob, size_t(MaterialSlot::Count)> slotJobs;
    std::array<bool, size_t(MaterialSlot::Count)> has {};
    if (isDevMaterial(m->name, opt.devMaterialPref)) {
      // Kept dev faces (Export/DevMaterials): the editor's own measurewall image.
      job.gltfName = QStringLiteral("dev__measurewall01");
      standIn = libraryStandIn(m->name, false, false, false, false, false);
      entry = &standIn;
      TextureJob tj;
      tj.rel = QStringLiteral("textures/dev/measurewall01_Color.jpg");
      tj.localSource = QStringLiteral(":/assets/measurewall01_Color.jpg");
      slotJobs[size_t(MaterialSlot::Albedo)] = tj;
      has[size_t(MaterialSlot::Albedo)] = true;
    } else if (!r.isLegacy()) {
      entry = opt.entryFor ? opt.entryFor(r.base()) : nullptr;
      if (!entry && m->hasEntry())
        entry = &m->entry;
      job.gltfName = fileSafe(r.pack + QStringLiteral("__") + r.name
                              + (r.isVariant() ? QStringLiteral("__") + r.variant : QString()));
      if (entry) {
        for (int s = 0; s < int(MaterialSlot::Count); ++s) {
          const MapType t = slotMapType(*entry, MaterialSlot(s));
          const MapFile *f = t == MapType::Count ? nullptr : entry->map(t);
          if (!f)
            continue;
          TextureJob tj;
          tj.rel = QStringLiteral("textures/%1/%2").arg(fileSafe(entry->pack), f->file);
          tj.pack = entry->pack;
          tj.name = entry->name;
          tj.mapKey = QString::fromLatin1(mapTypeKey(t));
          tj.checksum = f->checksum;
          tj.normalDirectX = f->directX;
          slotJobs[size_t(s)] = tj;
          has[size_t(s)] = true;
        }
      } else {
        job.writeTres = false;
        plan.warnings << QStringLiteral("%1: not in the texture catalogue (server offline?); its .tres is left as is")
                             .arg(m->ref);
      }
    } else {
      job.gltfName = fileSafe(QStringLiteral("local__") + m->name);
      standIn = libraryStandIn(m->name, !m->libArm.isEmpty(), !m->libRoughness.isEmpty(), !m->libMetallic.isEmpty(),
                               !m->libAo.isEmpty(), !m->libEmissive.isEmpty());
      entry = &standIn;
      const QString dir = QStringLiteral("textures/local/%1").arg(fileSafe(m->name));
      const auto local = [&](MaterialSlot s, const QString &path) {
        if (path.isEmpty() || !QFileInfo::exists(path))
          return;
        TextureJob tj;
        tj.rel = dir + QLatin1Char('/') + QFileInfo(path).fileName();
        tj.localSource = path;
        slotJobs[size_t(s)] = tj;
        has[size_t(s)] = true;
      };
      local(MaterialSlot::Albedo, m->libAlbedo);
      local(MaterialSlot::Normal, m->libNormal);
      slotJobs[size_t(MaterialSlot::Normal)].normalDirectX = m->libNormalDirectX;
      if (!m->libArm.isEmpty()) {
        local(MaterialSlot::Roughness, m->libArm);
        local(MaterialSlot::Metallic, m->libArm);
        local(MaterialSlot::Ao, m->libArm);
      } else {
        local(MaterialSlot::Roughness, m->libRoughness);
        local(MaterialSlot::Metallic, m->libMetallic);
        local(MaterialSlot::Ao, m->libAo);
      }
      local(MaterialSlot::Emission, m->libEmissive);
      if (!has[size_t(MaterialSlot::Albedo)])
        plan.warnings << QStringLiteral("%1: local texture without an image").arg(m->ref);
    }

    const ResolvedProps rp = scene.materialOverrides.resolve(m->ref, entry);
    job.props = rp.props;
    job.isShared = rp.map.isEmpty() && rp.variant.isEmpty();
    if (!r.isLegacy() && job.isShared && !r.isVariant())
      job.tresRel = QStringLiteral("textures/%1/%2.tres").arg(fileSafe(r.pack), fileSafe(r.name).toLower());
    else if (r.isLegacy() && job.isShared)
      job.tresRel = QStringLiteral("textures/local/%1/%1.tres").arg(fileSafe(m->name));
    else
      job.tresRel = QStringLiteral("%1/materials/%2.tres").arg(plan.levelRel, job.gltfName);
    if (!r.isLegacy() && r.isVariant())
      job.isShared = false;

    const MaterialProps &p = job.props;
    const auto chan = [](matprops::Channel c) { return int(c); };
    const int channel[size_t(MaterialSlot::Count)] = {0, 0, chan(p.roughnessChannel), chan(p.metallicChannel),
                                                      chan(p.aoChannel), 0, 0};
    // Alpha scissor with a separate opacity map: the alpha goes into a copy of the colour map (StandardMaterial3D
    // has no opacity slot).
    if (!r.isLegacy() && entry && p.transparency && has[size_t(MaterialSlot::Albedo)] && entry->hasMap(MapType::Opacity)) {
      const MapFile *of = entry->map(MapType::Opacity);
      TextureJob op;
      op.rel = QStringLiteral("textures/%1/%2").arg(fileSafe(entry->pack), of->file);
      op.pack = entry->pack;
      op.name = entry->name;
      op.mapKey = QString::fromLatin1(mapTypeKey(MapType::Opacity));
      op.checksum = of->checksum;
      texture(op);
      DerivedTexture d;
      d.colorRel = slotJobs[size_t(MaterialSlot::Albedo)].rel;
      d.opacityRel = op.rel;
      d.rel = QStringLiteral("textures/%1/%2_ColorAlpha.png").arg(fileSafe(entry->pack), fileSafe(entry->name));
      texture(slotJobs[size_t(MaterialSlot::Albedo)]);
      plan.derived.push_back(d);
      job.textures[size_t(MaterialSlot::Albedo)] = SlotTexture {d.rel, 0};
      has[size_t(MaterialSlot::Albedo)] = false;  // handled
    }
    const QString normalRel = has[size_t(MaterialSlot::Normal)] ? slotJobs[size_t(MaterialSlot::Normal)].rel : QString();
    for (int s = 0; s < int(MaterialSlot::Count); ++s) {
      if (!has[size_t(s)])
        continue;
      TextureJob tj = slotJobs[size_t(s)];
      const int idx = texture(tj);
      TextureJob &stored = plan.textures[size_t(idx)];
      if (MaterialSlot(s) == MaterialSlot::Normal) {
        stored.normal = true;
        stored.normalDirectX = tj.normalDirectX;
      }
      if (MaterialSlot(s) == MaterialSlot::Roughness) {
        stored.roughnessChannel = channel[s];
        stored.roughnessSrcNormal = normalRel;
      }
      job.textures[size_t(s)] = SlotTexture {tj.rel, channel[s]};
    }
    plan.materials.push_back(job);
    const int index = int(plan.materials.size()) - 1;
    materialByKey.insert(m->name, index);
    return index;
  }
};

struct GlbBuild {
  GlbJob job;
  bool world = false;    // worldspawn / group: hidden faces are removed
  bool collide = false;  // gets a <key>_collision-colonly node
  std::vector<std::shared_ptr<GameObject>> objects;
  std::vector<std::shared_ptr<GameObject>> clips;
};

struct CollisionPoly {
  std::vector<glm::dvec3> pts;  // world, counter-clockwise from outside
  glm::dvec3 n;
};

// Collision surface with few triangles: coplanar same-facing faces merge into one outline per plane
// (QPainterPath union), then Qt's triangulator (holes included). Godot makes one trimesh from it (-colonly).
gltf::Primitive collisionPrimitive(const std::vector<CollisionPoly> &polys, double scale) {
  struct Group {
    glm::dvec3 n;
    double d;
    std::vector<int> members;
  };
  std::vector<Group> groups;
  for (int i = 0; i < int(polys.size()); ++i) {
    const auto &p = polys[size_t(i)];
    const double d = glm::dot(p.n, p.pts[0]);
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
      return glm::dot(g.n, p.n) > 1.0 - 1e-6 && std::fabs(g.d - d) < 1e-3;
    });
    if (it == groups.end())
      groups.push_back({p.n, d, {i}});
    else
      it->members.push_back(i);
  }
  gltf::Primitive prim;
  const auto put = [&](const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, const glm::dvec3 &n) {
    const quint32 base = quint32(prim.positions.size());
    for (const glm::dvec3 &q: {a, b, c}) {
      prim.positions.push_back(glm::vec3(q * scale));
      prim.normals.push_back(glm::vec3(n));
    }
    prim.indices.insert(prim.indices.end(), {base, base + 1, base + 2});
  };
  for (const Group &g: groups) {
    if (g.members.size() == 1) {
      const auto &pts = polys[size_t(g.members[0])].pts;
      for (size_t k = 1; k + 1 < pts.size(); ++k)
        put(pts[0], pts[k], pts[k + 1], g.n);
      continue;
    }
    // 2D frame with e1 x e2 = n: counter-clockwise in 2D is counter-clockwise from outside.
    const glm::dvec3 ref = std::fabs(g.n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, g.n));
    const glm::dvec3 e2 = glm::cross(g.n, e1);
    QPainterPath all;
    all.setFillRule(Qt::WindingFill);
    // Snapped to 1e-4 world units: float noise would keep touching faces from sharing an edge in the union.
    const auto snap = [](double v) { return std::round(v * 1e4) / 1e4; };
    for (const int m: g.members) {
      QPolygonF poly;
      for (const glm::dvec3 &q: polys[size_t(m)].pts)
        poly << QPointF(snap(glm::dot(q, e1)), snap(glm::dot(q, e2)));
      all.addPolygon(poly);
      all.closeSubpath();
    }
    const QPainterPath merged = all.simplified();
    // Drop points on straight runs (where brushes met) before triangulating.
    QPainterPath clean;
    clean.setFillRule(Qt::OddEvenFill);
    for (QPolygonF ring: merged.toSubpathPolygons()) {
      if (ring.size() > 1 && ring.first() == ring.last())
        ring.removeLast();
      for (bool again = true; again && ring.size() > 3;) {
        again = false;
        for (int k = 0; k < ring.size() && ring.size() > 3; ++k) {
          const QPointF a = ring[(k + ring.size() - 1) % ring.size()], b = ring[k], c = ring[(k + 1) % ring.size()];
          // b's distance from line a-c, world units (vertex float noise is far below 1e-4).
          const double cross = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
          const double ac = std::hypot(c.x() - a.x(), c.y() - a.y());
          if (ac < 1e-9 || std::fabs(cross) / ac < 1e-4) {
            ring.removeAt(k);
            again = true;
            break;
          }
        }
      }
      if (ring.size() >= 3) {
        clean.addPolygon(ring);
        clean.closeSubpath();
      }
    }
    // The triangulator rounds to 1/32 of a unit: work at 1024x.
    constexpr double kS = 1024.0;
    const QTriangleSet ts = qTriangulate(clean, QTransform::fromScale(kS, kS), 1, true);
    const auto index = [&ts](int k) -> int {
      return ts.indices.type() == QVertexIndexVector::UnsignedInt
          ? int(static_cast<const quint32 *>(ts.indices.data())[k])
          : int(static_cast<const quint16 *>(ts.indices.data())[k]);
    };
    const auto at = [&](int i) {
      const double x = ts.vertices[2 * i] / kS, y = ts.vertices[2 * i + 1] / kS;
      return std::pair<glm::dvec2, glm::dvec3>(glm::dvec2(x, y), e1 * x + e2 * y + g.n * g.d);
    };
    for (int k = 0; k + 2 < ts.indices.size(); k += 3) {
      auto [a2, a] = at(index(k));
      auto [b2, b] = at(index(k + 1));
      auto [c2, c] = at(index(k + 2));
      const double area = (b2.x - a2.x) * (c2.y - a2.y) - (b2.y - a2.y) * (c2.x - a2.x);
      if (std::fabs(area) < 1e-12)
        continue;
      if (area > 0.0)
        put(a, b, c, g.n);
      else
        put(a, c, b, g.n);
    }
  }
  return prim;
}

// `cornerNormals` (smooth shading): one normal per corner of `poly`, else `n` for all.
void addPolygon(gltf::Primitive &prim, const std::vector<glm::dvec3> &poly, const glm::dvec3 &n, const FaceTexBasis *basis,
                double scale, const std::vector<glm::vec3> *cornerNormals = nullptr) {
  const quint32 base = quint32(prim.positions.size());
  for (size_t k = 0; k < poly.size(); ++k) {
    const glm::dvec3 &p = poly[k];
    prim.positions.push_back(glm::vec3(p * scale));
    prim.normals.push_back(cornerNormals && k < cornerNormals->size() ? (*cornerNormals)[k] : glm::vec3(n));
    if (basis)
      prim.uvs.push_back(basis->uvAt(glm::vec3(p)));
  }
  for (quint32 i = 1; i + 1 < quint32(poly.size()); ++i)
    prim.indices.insert(prim.indices.end(), {base, base + i, base + i + 1});
}

} // namespace

void basisLookingAlong(const glm::vec3 &forward, glm::vec3 &x, glm::vec3 &y, glm::vec3 &z) {
  const glm::vec3 f = glm::length(forward) > 1e-6f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
  z = -f;
  glm::vec3 side = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), z);
  if (glm::length(side) < 1e-4f)
    side = glm::cross(glm::vec3(0.0f, 0.0f, z.y > 0.0f ? -1.0f : 1.0f), z);
  x = glm::normalize(side);
  y = glm::normalize(glm::cross(z, x));
}

namespace {

// The brushes of one map split into .glb files (key order in `order`).
struct Partition {
  std::map<QString, GlbBuild> glbs;
  std::vector<QString> order;
};

// `collision` false (Export/Collision off): no trimesh collision node and no clip brushes.
Partition partitionScene(const Scene &scene, const QString &levelRel, bool collision = true) {
  Partition p;
  const auto glbFor = [&](const QString &key, bool world, QString suffix, quint32 entity) -> GlbBuild & {
    if (!collision)
      suffix.clear();
    auto it = p.glbs.find(key);
    if (it == p.glbs.end()) {
      GlbBuild g;
      g.job.key = key;
      g.job.rel = QStringLiteral("%1/%2.glb").arg(levelRel, key);
      g.job.entity = entity;
      g.world = world;
      g.collide = !suffix.isEmpty();
      it = p.glbs.emplace(key, std::move(g)).first;
      p.order.push_back(key);
    }
    return it->second;
  };
  QHash<quint32, QString> groupKeys;
  QSet<QString> usedGroupKeys;
  const auto groupKey = [&](quint32 gid) {
    if (const auto it = groupKeys.constFind(gid); it != groupKeys.constEnd())
      return *it;
    QString key;
    if (gid == kWorldspawnGroup) {
      key = QStringLiteral("worldspawn");
    } else {
      const MeshGroup *g = scene.findGroup(gid);
      key = QStringLiteral("group_") + fileSafe(g && !g->name.isEmpty() ? g->name : QString::number(gid));
      if (usedGroupKeys.contains(key))
        key += QStringLiteral("_%1").arg(gid);
    }
    usedGroupKeys.insert(key);
    groupKeys.insert(gid, key);
    return key;
  };
  const auto procKey = [&](quint32 pid) {
    const Scene::ProcObject *pr = scene.findProc(pid);
    return fileSafe((pr && !pr->type.isEmpty() ? pr->type : QStringLiteral("generated")) + QStringLiteral("_%1").arg(pid));
  };

  for (const auto &o: scene.gameObjects) {
    if (!o || o->hiddenFlag() || o->ownerEntity() || o->modReplaced() || !o->getMesh() || o->getMesh()->faceCount() == 0)
      continue;
    const Entity *ent = o->brushEntity() ? entityById(scene, o->brushEntity()) : nullptr;
    const QString cls = ent ? ent->classname : QString();
    if (cls == kClipClass) {
      if (!collision)
        continue;
      const QString owner = o->procObject() ? procKey(o->procObject()) : groupKey(o->group());
      glbFor(owner, !o->procObject(), o->procObject() ? QString() : QStringLiteral("-col"), 0).clips.push_back(o);
      continue;
    }
    if (cls.startsWith(QLatin1String("trigger_")))
      continue;  // the I/O runtime builds trigger volumes from their hulls
    if (o->procObject()) {
      // Generated objects: visual only, their clip ramp collides.
      glbFor(procKey(o->procObject()), false, QString(), 0).objects.push_back(o);
    } else if (ent) {
      const QString key = fileSafe(cls + QLatin1Char('_') + (ent->name.isEmpty() ? QString::number(shortId(ent->id)) : ent->name));
      // A rigid body collides through its own convex hulls (RigidBody3D): a trimesh cannot move.
      const bool solid = cls != QLatin1String("func_illusionary") && cls != kRigidBodyClass;
      glbFor(key, false, solid ? QStringLiteral("-col") : QString(), ent->id).objects.push_back(o);
    } else {
      glbFor(groupKey(o->group()), true, QStringLiteral("-col"), 0).objects.push_back(o);
    }
  }
  return p;
}

// Hidden-face solids: worldspawn and groups only (brush entities and generated objects neither lose nor hide faces).
std::vector<SolidInput> worldSolids(const Partition &p) {
  std::vector<SolidInput> solids;
  for (const auto &[key, g]: p.glbs) {
    if (!g.world)
      continue;
    for (const auto &o: g.objects) {
      const Mesh &mesh = *o->getMesh();
      SolidInput s;
      s.objId = o->stableId();
      int tools = 0;
      const FaceFrame frame = faceFrame(*o, mesh);
      for (int f = 0; f < mesh.faceCount(); ++f) {
        SolidInput::Face face;
        glm::dvec3 n;
        if (!worldFace(mesh, f, frame, face.poly, n))
          continue;
        face.face = f;
        face.pickId = mesh.getFacePickingID(f);
        if (const auto &m = mesh.faceMaterialRef(f)) {
          face.material = m->name;
          if (toolMaterialName(m->name))
            ++tools;
        }
        s.faces.push_back(std::move(face));
      }
      s.occluder = tools < int(s.faces.size()) / 2 + 1;
      if (!s.faces.empty())
        solids.push_back(std::move(s));
    }
  }
  return solids;
}

quint64 mix64(quint64 h, const quint64 v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

quint64 bitsOf(const double d) {
  quint64 b = 0;
  std::memcpy(&b, &d, sizeof b);
  return b;
}

} // namespace

std::vector<SolidInput> hiddenFaceInputs(const Scene &scene) { return worldSolids(partitionScene(scene, QString())); }

quint64 hiddenFaceKey(const std::vector<SolidInput> &solids) {
  // Everything FaceGraph::faceCovered reads: polygons, solid / face indices, occluder flag.
  quint64 h = mix64(0xcbf29ce484222325ull, solids.size());
  for (const SolidInput &s: solids) {
    h = mix64(h, (quint64(s.objId) << 1) | (s.occluder ? 1u : 0u));
    h = mix64(h, s.faces.size());
    for (const SolidInput::Face &f: s.faces) {
      h = mix64(h, (quint64(quint32(f.face)) << 32) | quint64(f.poly.size()));
      for (const glm::dvec3 &v: f.poly) {
        h = mix64(h, bitsOf(v.x));
        h = mix64(h, bitsOf(v.y));
        h = mix64(h, bitsOf(v.z));
      }
    }
  }
  return h;
}

HiddenFaces computeHiddenFaces(const std::vector<SolidInput> &solids) {
  HiddenFaces out;
  out.inputKey = hiddenFaceKey(solids);
  auto graph = std::make_shared<FaceGraph>(FaceGraph::build(solids, false));  // no links: only `hidden` is read
  for (const FaceNode &n: graph->nodes)
    if (n.hidden)
      out.faces.insert((quint64(n.objId) << 32) | quint32(n.face));
  out.graph = std::move(graph);
  return out;
}

LevelPlan planLevel(const Scene &scene, const LevelOptions &opt, const HiddenFaces *precomputed) {
  LevelPlan plan;
  plan.mapName = fileSafe(io::snake(opt.mapName.isEmpty() ? QStringLiteral("untitled") : opt.mapName));
  plan.levelRel = QStringLiteral("levels/") + plan.mapName;
  Builder b {scene, opt, plan, {}, {}};

  Partition part = partitionScene(scene, plan.levelRel, opt.exportCollision);
  std::map<QString, GlbBuild> &glbs = part.glbs;
  const std::vector<QString> &order = part.order;

  // Covered faces: computed here unless the caller did (worker thread, or cached).
  HiddenFaces own;
  if (!precomputed)
    own = computeHiddenFaces(worldSolids(part));
  const HiddenFaces &hiddenFaces = precomputed ? *precomputed : own;
  const std::unordered_set<quint64> &hidden = hiddenFaces.faces;
  static const FaceGraph kNoGraph;
  const FaceGraph &graph = hiddenFaces.graph ? *hiddenFaces.graph : kNoGraph;

  // Smooth brushes of one unit (generated object, brush entity, group) shade as one surface, as in the editor.
  std::unordered_map<quint32, std::vector<std::vector<glm::vec3>>> unitNormals;
  {
    std::map<quint64, std::vector<const GameObject *>> units;
    for (const auto &o: scene.gameObjects)
      if (o && o->getMesh() && !o->isDerived() && o->smoothShading())
        if (const quint64 k = smoothUnitKey(*o))
          units[k].push_back(o.get());
    for (const auto &[k, unit]: units) {
      if (unit.size() < 2)
        continue;
      std::vector<std::vector<std::vector<glm::dvec3>>> polys(unit.size());
      std::vector<std::vector<glm::dvec3>> normals(unit.size());
      for (size_t s = 0; s < unit.size(); ++s) {
        const Mesh &m = *unit[s]->getMesh();
        const FaceFrame frame = faceFrame(*unit[s], m);
        polys[s].resize(size_t(m.faceCount()));
        normals[s].assign(size_t(m.faceCount()), glm::dvec3(0.0));
        for (int f = 0; f < m.faceCount(); ++f)
          worldFace(m, f, frame, polys[s][size_t(f)], normals[s][size_t(f)]);
      }
      auto smoothed = smoothSolidsTogether(polys, normals);
      for (size_t s = 0; s < unit.size(); ++s)
        unitNormals[unit[s]->stableId()] = std::move(smoothed[s]);
    }
  }

  const double scale = opt.unitScale;
  for (const QString &key: order) {
    GlbBuild &g = glbs[key];
    gltf::Document doc;
    gltf::Mesh mesh;
    mesh.name = key;
    QHash<int, int> primByMaterial;     // plan material -> primitive
    QHash<int, int> gltfMatByMaterial;  // plan material -> glTF material
    QHash<QString, int> imageByRel;
    const auto image = [&](const QString &rel) {
      if (const auto it = imageByRel.constFind(rel); it != imageByRel.constEnd())
        return *it;
      doc.images.push_back({QStringLiteral("../../") + rel, QFileInfo(rel).completeBaseName()});
      doc.textures.push_back({int(doc.images.size()) - 1});
      imageByRel.insert(rel, int(doc.textures.size()) - 1);
      return int(doc.textures.size()) - 1;
    };
    std::vector<glm::dvec3> poly;
    std::vector<CollisionPoly> collision;
    // EDITOR_GODOT_DROPDUMP=<file>: every face left out, why and where.
    static QFile *dropDump = [] {
      const QString path = qEnvironmentVariable("EDITOR_GODOT_DROPDUMP");
      if (path.isEmpty())
        return static_cast<QFile *>(nullptr);
      auto *f = new QFile(path);
      return f->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) ? f : nullptr;
    }();
    const auto dropped = [&](const char *why, const GameObject &o, const Mesh &m, const int f, const FaceFrame &fr) {
      if (!dropDump)
        return;
      std::vector<glm::dvec3> p;
      glm::dvec3 nn(0.0);
      if (!worldFace(m, f, fr, p, nn))
        p.clear();
      glm::dvec3 c(0.0);
      for (const auto &v: p)
        c += v;
      if (!p.empty())
        c /= double(p.size());
      const MaterialPtr &mat = m.faceMaterialRef(f);
      // A hidden face: the solids its test points are in (as computeHidden).
      QString by;
      if (QLatin1String(why) == QLatin1String("hidden"))
        for (const FaceNode &nd: graph.nodes)
          if (nd.objId == o.stableId() && nd.face == f) {
            const glm::dvec3 off = nd.normal * (FaceGraph::kEps * 5.0);
            std::vector<glm::dvec3> pts {nd.centroid + off};
            for (const auto &v: nd.poly)
              pts.push_back(nd.centroid + (v - nd.centroid) * 0.9 + off);
            for (const auto &pt: pts) {
              const int s = graph.solidContaining(pt, nd.solid);
              by += QStringLiteral(" %1").arg(s >= 0 ? graph.solidObjId(s) : 0u);
            }
            break;
          }
      dropDump->write(QStringLiteral("%1 obj %2 derived %3 verts %4 c %5 %6 %7 n %8 %9 %10 %11%12\n")
                          .arg(QLatin1String(why))
                          .arg(o.stableId())
                          .arg(o.isDerived() ? 1 : 0)
                          .arg(p.size())
                          .arg(c.x, 0, 'f', 3).arg(c.y, 0, 'f', 3).arg(c.z, 0, 'f', 3)
                          .arg(nn.x, 0, 'f', 3).arg(nn.y, 0, 'f', 3).arg(nn.z, 0, 'f', 3)
                          .arg(mat ? mat->name : QStringLiteral("-"))
                          .arg(by.isEmpty() ? QString() : QStringLiteral(" by") + by)
                          .toUtf8());
      dropDump->flush();
    };
    for (const auto &o: g.objects) {
      const Mesh &m = *o->getMesh();
      const FaceFrame frame = faceFrame(*o, m);
      // Smooth shading: the editor's corner normals from all faces (dropped ones too, so hiding does not change them).
      std::vector<std::vector<glm::vec3>> smoothNormals;
      if (const auto u = unitNormals.find(o->stableId()); u != unitNormals.end()) {
        smoothNormals = u->second;
      } else if (o->smoothShading()) {
        std::vector<std::vector<glm::dvec3>> polys(size_t(m.faceCount()));
        std::vector<glm::dvec3> normals(polys.size(), glm::dvec3(0.0));
        for (int f = 0; f < m.faceCount(); ++f)
          worldFace(m, f, frame, polys[size_t(f)], normals[size_t(f)]);
        smoothNormals = smoothPolygonNormals(polys, normals);
      }
      for (int f = 0; f < m.faceCount(); ++f) {
        const MaterialPtr &mat = m.faceMaterialRef(f);
        if (!mat || (!opt.exportDevMaterials && isDevMaterial(mat->name, opt.devMaterialPref))) {
          ++plan.devFaces;
          dropped("dev", *o, m, f, frame);
          continue;
        }
        if (toolMaterialName(mat->name)) {
          dropped("tool", *o, m, f, frame);
          continue;
        }
        if (g.world && hidden.count((quint64(o->stableId()) << 32) | quint32(f))) {
          ++plan.hiddenFaces;
          dropped("hidden", *o, m, f, frame);
          continue;
        }
        glm::dvec3 n;
        if (!worldFace(m, f, frame, poly, n)) {
          dropped("degenerate", *o, m, f, frame);
          continue;
        }
        const int mi = b.material(mat);
        int pi = primByMaterial.value(mi, -1);
        if (pi < 0) {
          const MaterialJob &mj = plan.materials[size_t(mi)];
          gltf::Material gm;
          gm.name = mj.gltfName;
          gm.metallic = mj.props.metallic;
          gm.roughness = mj.props.roughness;
          if (const auto &a = mj.textures[size_t(MaterialSlot::Albedo)])
            gm.baseColorTexture = image(a->rel);
          if (const auto &nm = mj.textures[size_t(MaterialSlot::Normal)]) {
            gm.normalTexture = image(nm->rel);
            gm.normalScale = mj.props.normalScale;
          }
          doc.materials.push_back(gm);
          gltfMatByMaterial.insert(mi, int(doc.materials.size()) - 1);
          gltf::Primitive prim;
          prim.material = int(doc.materials.size()) - 1;
          mesh.primitives.push_back(prim);
          pi = int(mesh.primitives.size()) - 1;
          primByMaterial.insert(mi, pi);
          g.job.materials.insert(mj.gltfName, mj.tresRel);
        }
        addPolygon(mesh.primitives[size_t(pi)], poly, n, &m.faceTexBasis(f), scale,
                   size_t(f) < smoothNormals.size() ? &smoothNormals[size_t(f)] : nullptr);
        if (g.collide)
          collision.push_back({poly, n});
        ++g.job.faces;
      }
    }
    plan.faces += g.job.faces;
    if (!mesh.primitives.empty()) {
      doc.meshes.push_back(mesh);
      gltf::Node node;
      node.name = key;
      node.mesh = 0;
      doc.nodes.push_back(node);
      doc.roots.push_back(0);
    }
    // Collision surface: its own merged node (-colonly: Godot keeps a StaticBody3D with one trimesh, drops the mesh).
    if (!collision.empty()) {
      gltf::Mesh cm;
      cm.name = key + QStringLiteral("_collision");
      cm.primitives.push_back(collisionPrimitive(collision, scale));
      if (!cm.primitives.back().indices.empty()) {
        g.job.collisionTriangles = int(cm.primitives.back().indices.size() / 3);
        doc.meshes.push_back(cm);
        gltf::Node node;
        node.name = cm.name + QStringLiteral("-colonly");
        node.mesh = int(doc.meshes.size()) - 1;
        doc.nodes.push_back(node);
        doc.roots.push_back(int(doc.nodes.size()) - 1);
      }
    }
    // Clip brushes: one convex collision shape each, no mesh (-convcolonly).
    for (const auto &o: g.clips) {
      gltf::Mesh cm;
      cm.name = QStringLiteral("clip_%1").arg(o->stableId());
      gltf::Primitive prim;
      const Mesh &m = *o->getMesh();
      const FaceFrame frame = faceFrame(*o, m);
      for (int f = 0; f < m.faceCount(); ++f) {
        glm::dvec3 n;
        if (worldFace(m, f, frame, poly, n))
          addPolygon(prim, poly, n, nullptr, scale);
      }
      if (prim.indices.empty())
        continue;
      cm.primitives.push_back(prim);
      doc.meshes.push_back(cm);
      gltf::Node node;
      node.name = cm.name + QStringLiteral("-convcolonly");
      node.mesh = int(doc.meshes.size()) - 1;
      doc.nodes.push_back(node);
      doc.roots.push_back(int(doc.nodes.size()) - 1);
    }
    if (doc.nodes.empty())
      continue;
    g.job.bytes = gltf::toGlb(doc);
    plan.glbs.push_back(g.job);
  }
  if (const MaterialRef lr = MaterialRef::parse(opt.lutRef); !lr.isEmpty()) {
    if (!opt.lutStrip.isNull())
      plan.luts = planLuts({ColorLut{lr.name, opt.lutStrip, std::nullopt, {}}});
    else
      plan.warnings << QStringLiteral("colour LUT %1: not loaded (server offline?); not exported").arg(lr.base());
  }

  // The skybox panorama (downloaded with the other maps) and a PanoramaSkyMaterial next to it.
  if (const MaterialRef sr = MaterialRef::parse(opt.skyRef); !sr.isEmpty() && !sr.isLegacy()) {
    const TextureEntry *e = opt.entryFor ? opt.entryFor(sr.base()) : nullptr;
    const MapFile *pano = e ? e->map(MapType::Panorama) : nullptr;
    if (pano) {
      TextureJob tj;
      tj.rel = QStringLiteral("textures/%1/%2").arg(fileSafe(e->pack), pano->file);
      tj.pack = e->pack;
      tj.name = e->name;
      tj.mapKey = QString::fromLatin1(mapTypeKey(MapType::Panorama));
      tj.checksum = pano->checksum;
      tj.sky = true;
      plan.textures.push_back(tj);
      plan.sky = SkyJob{e->ref,
                        QStringLiteral("textures/%1/%2.tres").arg(fileSafe(e->pack), fileSafe(e->name).toLower()),
                        tj.rel};
    } else {
      plan.warnings << QStringLiteral("skybox %1: not a skybox in the texture catalogue (server offline?); not exported")
                           .arg(sr.base());
    }
  }
  return plan;
}

std::vector<LutJob> planLuts(const std::vector<ColorLut> &luts) {
  std::vector<LutJob> out;
  QSet<QString> used;
  for (const ColorLut &l: luts) {
    if (!l.isValid())
      continue;
    const QString base = fileSafe(l.name);
    QString rel = QStringLiteral("textures/luts/%1.png").arg(base);
    for (int n = 2; used.contains(rel.toLower()); ++n)
      rel = QStringLiteral("textures/luts/%1_%2.png").arg(base).arg(n);
    used.insert(rel.toLower());
    out.push_back({rel, l.name, l.strip.convertToFormat(QImage::Format_RGB888)});
  }
  return out;
}

QString mergeMaterialTres(const QString &existing, const MaterialJob &m, const UidFn &uidFor) {
  GdTextFile f = existing.trimmed().isEmpty()
      ? GdTextFile::create(QStringLiteral("gd_resource"), {{QStringLiteral("type"), GdValue::string(QStringLiteral("StandardMaterial3D"))},
                                                           {QStringLiteral("format"), GdValue::integer(3)}})
      : GdTextFile::parse(existing);
  if (!f.sections.isEmpty() && f.sections[0].tag == QLatin1String("gd_resource")
      && f.attr(0, QStringLiteral("type")).toString() != QLatin1String("StandardMaterial3D"))
    return existing;  // the user turned it into another material type: theirs
  f.ensureSection(QStringLiteral("resource"));
  const MaterialProps &p = m.props;
  // Looked up every time: adding an ext_resource shifts the sections.
  const auto R = [&f] { return f.find(QStringLiteral("resource")); };

  const auto put = [&](const QString &key, const GdValue &v, bool isDefault) {
    if (isDefault)
      f.removeProp(R(), key);
    else
      f.setProp(R(), key, v);
  };
  const auto num = [&](const QString &key, double v, double def) {
    put(key, GdValue::number(v), std::fabs(v - def) < 1e-6);
  };
  const auto tex = [&](MaterialSlot s) -> QString {
    const auto &st = m.textures[size_t(s)];
    if (!st)
      return {};
    return f.extResource(QStringLiteral("Texture2D"), res(st->rel), uidFor ? uidFor(st->rel) : QString());
  };
  const auto texProp = [&](const QString &key, const QString &id) {
    if (id.isEmpty())
      f.removeProp(R(), key);
    else
      f.setProp(R(), key, GdValue::extResource(id));
  };
  const auto channelProp = [&](const QString &key, MaterialSlot s) {
    const auto &st = m.textures[size_t(s)];
    put(key, GdValue::integer(st ? st->channel : 0), !st || st->channel == 0);
  };

  put(QStringLiteral("transparency"), GdValue::integer(2), !p.transparency);
  put(QStringLiteral("shading_mode"), GdValue::integer(0), p.shadingMode != matprops::Shading::Unshaded);

  texProp(QStringLiteral("albedo_texture"), tex(MaterialSlot::Albedo));

  num(QStringLiteral("metallic"), p.metallic, 0.0);
  num(QStringLiteral("metallic_specular"), p.metallicSpecular, 0.5);
  texProp(QStringLiteral("metallic_texture"), tex(MaterialSlot::Metallic));
  channelProp(QStringLiteral("metallic_texture_channel"), MaterialSlot::Metallic);

  num(QStringLiteral("roughness"), p.roughness, 1.0);
  texProp(QStringLiteral("roughness_texture"), tex(MaterialSlot::Roughness));
  channelProp(QStringLiteral("roughness_texture_channel"), MaterialSlot::Roughness);

  const QString emissionTex = p.emissionEnabled ? tex(MaterialSlot::Emission) : QString();
  put(QStringLiteral("emission_enabled"), GdValue::boolean(true), !p.emissionEnabled);
  put(QStringLiteral("emission"), GdValue::color(QColor(p.emissionColor)), !p.emissionEnabled || (p.emissionColor & 0xffffff) == 0);
  put(QStringLiteral("emission_energy_multiplier"), GdValue::number(p.emissionEnergy),
      !p.emissionEnabled || std::fabs(p.emissionEnergy - 1.0f) < 1e-6f);
  texProp(QStringLiteral("emission_texture"), emissionTex);

  const QString normalTex = tex(MaterialSlot::Normal);
  put(QStringLiteral("normal_enabled"), GdValue::boolean(true), normalTex.isEmpty());
  num(QStringLiteral("normal_scale"), normalTex.isEmpty() ? 1.0 : p.normalScale, 1.0);
  texProp(QStringLiteral("normal_texture"), normalTex);

  const QString aoTex = tex(MaterialSlot::Ao);
  put(QStringLiteral("ao_enabled"), GdValue::boolean(true), aoTex.isEmpty());
  num(QStringLiteral("ao_light_affect"), aoTex.isEmpty() ? 0.0 : p.aoLightAffect, 0.0);
  texProp(QStringLiteral("ao_texture"), aoTex);
  channelProp(QStringLiteral("ao_texture_channel"), MaterialSlot::Ao);

  const QString heightTex = tex(MaterialSlot::Height);
  put(QStringLiteral("heightmap_enabled"), GdValue::boolean(true), heightTex.isEmpty());
  // Ours is 0..1, Godot's -16..16 (default 5).
  num(QStringLiteral("heightmap_scale"), heightTex.isEmpty() ? 5.0 : p.heightmapScale * 16.0, 5.0);
  put(QStringLiteral("heightmap_deep_parallax"), GdValue::boolean(true), heightTex.isEmpty() || !p.heightmapDeepParallax);
  texProp(QStringLiteral("heightmap_texture"), heightTex);

  f.pruneExtResources();
  return f.toText();
}

QString mergeGlbImport(const QString &existing, const QMap<QString, QString> &materialRels) {
  const bool fresh = existing.trimmed().isEmpty();
  GdTextFile f = fresh ? GdTextFile::create(QStringLiteral("remap"), {}, true) : GdTextFile::parse(existing);
  const int remap = f.ensureSection(QStringLiteral("remap"));
  if (fresh) {
    f.setProp(remap, QStringLiteral("importer"), GdValue::string(QStringLiteral("scene")));
    f.setProp(remap, QStringLiteral("importer_version"), GdValue::integer(1));
    f.setProp(remap, QStringLiteral("type"), GdValue::string(QStringLiteral("PackedScene")));
  }
  const int params = f.ensureSection(QStringLiteral("params"));
  f.setProp(params, QStringLiteral("nodes/use_name_suffixes"), GdValue::boolean(true));
  GdValue sub = f.prop(params, QStringLiteral("_subresources"));
  if (sub.kind != GdValue::Kind::Dict) {
    sub = GdValue();
    sub.kind = GdValue::Kind::Dict;
  }
  const GdValue *oldMats = sub.find(QStringLiteral("materials"));
  GdValue mats;
  mats.kind = GdValue::Kind::Dict;
  for (auto it = materialRels.constBegin(); it != materialRels.constEnd(); ++it) {
    GdValue entry;
    entry.kind = GdValue::Kind::Dict;
    if (oldMats)
      if (const GdValue *old = oldMats->find(it.key()); old && old->kind == GdValue::Kind::Dict)
        entry = *old;  // keeps the user's other keys for this material
    entry.set(QStringLiteral("use_external/enabled"), GdValue::boolean(true));
    entry.set(QStringLiteral("use_external/path"), GdValue::string(res(it.value())));
    mats.set(it.key(), entry);
  }
  sub.set(QStringLiteral("materials"), mats);
  f.setProp(params, QStringLiteral("_subresources"), sub);
  return f.toText();
}

QString newTextureImport(const TextureJob &t) {
  QStringList out;
  out << QStringLiteral("[remap]") << QString() << QStringLiteral("importer=\"texture\"")
      << QStringLiteral("type=\"CompressedTexture2D\"") << QString() << QStringLiteral("[params]") << QString();
  if (t.sky) {
    // HDR kept (BC6H on desktop): hdr_compression 2 drops the unused alpha instead of falling back to uncompressed RGBE.
    out << QStringLiteral("compress/mode=2");
    out << QStringLiteral("compress/hdr_compression=2");
    out << QStringLiteral("mipmaps/generate=false");
    out << QStringLiteral("process/hdr_as_srgb=false");
    out << QStringLiteral("detect_3d/compress_to=0");
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
  }
  out << QStringLiteral("compress/mode=2");
  out << QStringLiteral("compress/normal_map=%1").arg(t.normal ? 1 : 2);
  out << QStringLiteral("mipmaps/generate=true");
  if (t.roughnessChannel >= 0) {
    out << QStringLiteral("roughness/mode=%1").arg(t.roughnessChannel + 2);
    out << QStringLiteral("roughness/src_normal=%1").arg(gdString(t.roughnessSrcNormal.isEmpty() ? QString() : res(t.roughnessSrcNormal)));
  } else {
    out << QStringLiteral("roughness/mode=1");
  }
  if (t.normal && t.normalDirectX)
    out << QStringLiteral("process/normal_map_invert_y=true");
  out << QStringLiteral("detect_3d/compress_to=0");
  return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString mergeSkyTres(const QString &existing, const SkyJob &sky, const UidFn &uidFor) {
  GdTextFile f = existing.trimmed().isEmpty()
      ? GdTextFile::create(QStringLiteral("gd_resource"), {{QStringLiteral("type"), GdValue::string(QStringLiteral("PanoramaSkyMaterial"))},
                                                           {QStringLiteral("format"), GdValue::integer(3)}})
      : GdTextFile::parse(existing);
  if (!f.sections.isEmpty() && f.sections[0].tag == QLatin1String("gd_resource")
      && f.attr(0, QStringLiteral("type")).toString() != QLatin1String("PanoramaSkyMaterial"))
    return existing;  // the user made it another sky material: theirs
  f.ensureSection(QStringLiteral("resource"));
  const QString id = f.extResource(QStringLiteral("Texture2D"), res(sky.texRel), uidFor ? uidFor(sky.texRel) : QString());
  f.setProp(f.find(QStringLiteral("resource")), QStringLiteral("panorama"), GdValue::extResource(id));
  f.pruneExtResources();
  return f.toText();
}

QString mergeLutImport(const QString &existing, int slices) {
  GdTextFile old = GdTextFile::parse(existing);
  const int oldRemap = existing.trimmed().isEmpty() ? -1 : old.ensureSection(QStringLiteral("remap"));
  const bool keep = oldRemap >= 0 && old.prop(oldRemap, QStringLiteral("importer")).s == QLatin1String("3d_texture");
  GdTextFile f = keep ? old : GdTextFile::create(QStringLiteral("remap"), {}, true);
  const int remap = f.ensureSection(QStringLiteral("remap"));
  if (!keep) {
    f.setProp(remap, QStringLiteral("importer"), GdValue::string(QStringLiteral("3d_texture")));
    f.setProp(remap, QStringLiteral("type"), GdValue::string(QStringLiteral("CompressedTexture3D")));
    // The file keeps its uid when it was imported as something else before.
    if (oldRemap >= 0)
      if (const GdValue u = old.prop(oldRemap, QStringLiteral("uid")); u.kind == GdValue::Kind::String)
        f.setProp(remap, QStringLiteral("uid"), u);
  }
  const int params = f.ensureSection(QStringLiteral("params"));
  f.setProp(params, QStringLiteral("compress/mode"), GdValue::integer(0));          // lossless
  f.setProp(params, QStringLiteral("compress/channel_pack"), GdValue::integer(0));  // sRGB friendly
  f.setProp(params, QStringLiteral("mipmaps/generate"), GdValue::boolean(false));
  f.setProp(params, QStringLiteral("slices/horizontal"), GdValue::integer(std::clamp(slices, 1, 256)));
  f.setProp(params, QStringLiteral("slices/vertical"), GdValue::integer(1));
  return f.toText();
}

namespace {

struct NodeSpec {
  QString meta;
  QString name;
  QString type;
  QString parentMeta;  // empty: the root
  QString instanceRel; // PackedScene
  quint32 entity = 0;
  QVector<QPair<QString, GdValue>> props;  // null value: remove (default)
};

GdValue tf(const glm::vec3 &forward, const glm::vec3 &origin) {
  glm::vec3 x, y, z;
  basisLookingAlong(forward, x, y, z);
  return GdValue::transform(x, y, z, origin);
}

GdValue orNull(bool isDefault, const GdValue &v) { return isDefault ? GdValue() : v; }

QColor srgb(const QColor &c) { return QColor::fromRgbF(c.redF(), c.greenF(), c.blueF(), 1.0f); }

} // namespace

SceneMerge mergeLevelScene(const QString &existing, const std::vector<Entity> &entities, const LevelPlan &plan,
                           const SceneSun &sun,
                           const QString &ioScriptRel, double unitScale, const UidFn &uidFor) {
  SceneMerge out;
  const float s = float(unitScale);
  QVector<NodeSpec> specs;

  for (const GlbJob &g: plan.glbs) {
    NodeSpec n;
    n.meta = QStringLiteral("glb:") + g.key;
    n.name = g.key;
    n.instanceRel = g.rel;
    n.entity = g.entity;
    specs.append(n);
  }
  QVector<NodeSpec> lights, cameras, starts;
  for (const Entity &e: entities) {
    const EntityClass *c = e.cls();
    if (!c || e.hidden)
      continue;
    const glm::vec3 origin = e.origin * s;
    const QString label = e.name.isEmpty() ? QString() : e.name;
    if (c->kind == EntityClass::Kind::PointLight || c->kind == EntityClass::Kind::SpotLight) {
      const bool spot = c->kind == EntityClass::Kind::SpotLight;
      NodeSpec n;
      n.meta = QStringLiteral("light:%1").arg(e.id);
      n.name = label.isEmpty() ? QStringLiteral("%1_%2").arg(spot ? QStringLiteral("spot") : QStringLiteral("light")).arg(shortId(e.id)) : label;
      n.type = spot ? QStringLiteral("SpotLight3D") : QStringLiteral("OmniLight3D");
      n.parentMeta = QStringLiteral("group:lights");
      n.entity = e.id;
      const QColor col = e.color(QStringLiteral("light_color"));
      const double energy = e.num(QStringLiteral("light_energy"), 1.0);
      const double specular = e.num(QStringLiteral("light_specular"), 0.5);
      const double range = lightRangeMeters(e);
      n.props.append({QStringLiteral("transform"), tf(e.forward(), origin)});
      n.props.append({QStringLiteral("visible"), orNull(e.value(QStringLiteral("enabled")).toBool(), GdValue::boolean(false))});
      n.props.append({QStringLiteral("light_color"), orNull(col.rgb() == qRgb(255, 255, 255), GdValue::color(srgb(col)))});
      n.props.append({QStringLiteral("light_energy"), orNull(std::fabs(energy - 1.0) < 1e-6, GdValue::number(energy))});
      n.props.append({QStringLiteral("light_specular"), orNull(std::fabs(specular - 0.5) < 1e-6, GdValue::number(specular))});
      n.props.append({spot ? QStringLiteral("spot_range") : QStringLiteral("omni_range"),
                      orNull(std::fabs(range - 5.0) < 1e-6, GdValue::number(range))});
      if (spot) {
        const double angle = e.num(QStringLiteral("spot_angle"), 45.0);
        n.props.append({QStringLiteral("spot_angle"), orNull(std::fabs(angle - 45.0) < 1e-6, GdValue::number(angle))});
      }
      // Godot's own defaults: omni 0.1 / 1.0, spot 0.03 / 1.0.
      const double biasDefault = spot ? 0.03 : 0.1;
      const double bias = e.num(QStringLiteral("shadow_bias"), biasDefault);
      const double normalBias = e.num(QStringLiteral("shadow_normal_bias"), 1.0);
      n.props.append({QStringLiteral("shadow_bias"), orNull(std::fabs(bias - biasDefault) < 1e-6, GdValue::number(bias))});
      n.props.append({QStringLiteral("shadow_normal_bias"), orNull(std::fabs(normalBias - 1.0) < 1e-6, GdValue::number(normalBias))});
      if (!e.value(QStringLiteral("castShadows")).toBool())
        out.warnings << QStringLiteral("%1: Godot 4.8 always casts shadows from omni / spot lights").arg(n.name);
      lights.append(n);
    } else if (c->kind == EntityClass::Kind::Camera) {
      NodeSpec n;
      n.meta = QStringLiteral("camera:%1").arg(e.id);
      n.name = label.isEmpty() ? QStringLiteral("camera_%1").arg(shortId(e.id)) : label;
      n.type = QStringLiteral("Camera3D");
      n.parentMeta = QStringLiteral("group:cameras");
      n.entity = e.id;
      const double fov = e.num(QStringLiteral("fov"), 75.0);
      n.props.append({QStringLiteral("transform"), tf(e.forward(), origin)});
      n.props.append({QStringLiteral("fov"), orNull(std::fabs(fov - 75.0) < 1e-6, GdValue::number(fov))});
      cameras.append(n);
    } else if (c->kind == EntityClass::Kind::PlayerStart) {
      NodeSpec n;
      n.meta = QStringLiteral("player_start:%1").arg(e.id);
      n.name = label.isEmpty() ? QStringLiteral("info_player_start") : label;
      n.type = QStringLiteral("Marker3D");
      n.entity = e.id;
      const float yaw = glm::radians(e.angles.y);
      n.props.append({QStringLiteral("transform"), tf(glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)), origin)});
      starts.append(n);
    }
  }
  if (!lights.isEmpty()) {
    NodeSpec g;
    g.meta = QStringLiteral("group:lights");
    g.name = QStringLiteral("lights");
    g.type = QStringLiteral("Node3D");
    specs.append(g);
    specs += lights;
  }
  if (!cameras.isEmpty()) {
    NodeSpec g;
    g.meta = QStringLiteral("group:cameras");
    g.name = QStringLiteral("cameras");
    g.type = QStringLiteral("Node3D");
    specs.append(g);
    specs += cameras;
  }
  specs += starts;
  if (sun.present) {
    NodeSpec n;
    n.meta = QStringLiteral("sun");
    n.name = QStringLiteral("Sun");
    n.type = QStringLiteral("DirectionalLight3D");
    n.props.append({QStringLiteral("transform"), tf(sun.direction, glm::vec3(0.0f))});
    n.props.append({QStringLiteral("light_color"), orNull(sun.color.rgb() == qRgb(255, 255, 255), GdValue::color(srgb(sun.color)))});
    n.props.append({QStringLiteral("light_energy"), orNull(std::fabs(sun.energy - 1.0f) < 1e-6f, GdValue::number(sun.energy))});
    n.props.append({QStringLiteral("shadow_enabled"), orNull(!sun.shadows, GdValue::boolean(true))});
    specs.append(n);
  }
  if (!ioScriptRel.isEmpty()) {
    NodeSpec n;
    n.meta = QStringLiteral("iomap");
    n.name = QStringLiteral("IOMap");
    n.type = QStringLiteral("Node3D");
    specs.append(n);
  }

  GdTextFile f = existing.trimmed().isEmpty()
      ? GdTextFile::create(QStringLiteral("gd_scene"), {{QStringLiteral("format"), GdValue::integer(3)}})
      : GdTextFile::parse(existing);
  int root = f.rootNode();
  if (root < 0) {
    root = f.addNode(validNodeName(plan.mapName), QStringLiteral("Node3D"), QString());
    f.setProp(root, kMeta, GdValue::string(QStringLiteral("root")));
  }
  const auto pathOfMeta = [&f](const QString &meta) -> QString {
    if (meta.isEmpty())
      return QStringLiteral(".");
    const int sec = f.nodeByProp(kMeta, GdValue::string(meta));
    return sec < 0 ? QStringLiteral(".") : f.nodePath(sec);
  };
  QSet<QString> wanted {QStringLiteral("root")};
  for (const NodeSpec &n: specs) {
    wanted.insert(n.meta);
    const QString parent = pathOfMeta(n.parentMeta);
    // ext_resources first: adding one shifts the node sections.
    QString ext, scriptExt, dataExt;
    if (!n.instanceRel.isEmpty())
      ext = f.extResource(QStringLiteral("PackedScene"), res(n.instanceRel), uidFor ? uidFor(n.instanceRel) : QString());
    if (n.meta == QLatin1String("iomap")) {
      scriptExt = f.extResource(QStringLiteral("Script"), kIoMapScript, uidFor ? uidFor(kIoMapScript.mid(6)) : QString());
      dataExt = f.extResource(QStringLiteral("Script"), res(ioScriptRel), uidFor ? uidFor(ioScriptRel) : QString());
    }
    int sec = f.nodeByProp(kMeta, GdValue::string(n.meta));
    const QString wantName = validNodeName(n.name);
    if (sec < 0) {
      sec = f.addNode(f.uniqueChildName(parent, wantName), n.type, parent, ext);
      f.setProp(sec, kMeta, GdValue::string(n.meta));
      f.setProp(sec, kMetaName, f.attr(sec, QStringLiteral("name")));
    } else {
      const QString cur = f.attr(sec, QStringLiteral("name")).toString();
      const QString given = f.prop(sec, kMetaName).toString();
      if (cur == given && cur != wantName && sec != f.rootNode()) {
        const QString nodeParent = f.attr(sec, QStringLiteral("parent")).toString();
        const QString fresh = f.uniqueChildName(nodeParent, wantName, sec);
        f.renameNode(sec, fresh);
        f.setProp(sec, kMetaName, GdValue::string(fresh));
      }
    }
    if (sec >= 0 && !ext.isEmpty()) {
      f.setAttr(sec, QStringLiteral("instance"), GdValue::extResource(ext));
    } else if (!n.type.isEmpty()) {
      f.setAttr(sec, QStringLiteral("type"), GdValue::string(n.type));
    }
    for (const auto &[key, v]: n.props) {
      if (v.isNull())
        f.removeProp(sec, key);
      else
        f.setProp(sec, key, v);
    }
    if (!scriptExt.isEmpty()) {
      f.setProp(sec, QStringLiteral("script"), GdValue::extResource(scriptExt));
      f.setProp(sec, QStringLiteral("map_data"), GdValue::extResource(dataExt));
    }
    if (n.entity)
      out.entityNodes.insert(n.entity, f.nodePath(sec));
  }
  // Nodes of things the map no longer has (with subtrees).
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < f.sections.size(); ++i) {
      if (f.sections[i].tag != QLatin1String("node") || f.findProp(i, kMeta) < 0)
        continue;
      if (!wanted.contains(f.prop(i, kMeta).toString())) {
        f.removeNodeTree(i);
        again = true;
        break;
      }
    }
  }
  f.pruneExtResources();
  out.text = f.toText();
  return out;
}

QString checkProject(const QString &root) {
  QFile f(QDir(root).filePath(QStringLiteral("project.godot")));
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return QStringLiteral("%1 is not a Godot project (no project.godot)").arg(root);
  const GdTextFile p = GdTextFile::parse(QString::fromUtf8(f.readAll()));
  int version = 0;
  QString features;
  for (int s = 0; s < p.sections.size(); ++s) {
    if (p.sections[s].tag.isEmpty() && p.findProp(s, QStringLiteral("config_version")) >= 0)
      version = int(p.prop(s, QStringLiteral("config_version")).toNumber());
    if (p.sections[s].tag == QLatin1String("application"))
      features = p.prop(s, QStringLiteral("config/features")).toText();
  }
  const QString need = QStringLiteral("Godot %1.%2 or newer required").arg(kMinGodotMajor).arg(kMinGodotMinor);
  if (version != 5)
    return QStringLiteral("%1 (project.godot config_version=%2)").arg(need).arg(version);
  static const QRegularExpression re(QStringLiteral("\"(\\d+)\\.(\\d+)\""));
  const auto m = re.match(features);
  if (!m.hasMatch())
    return QStringLiteral("%1 (no version in config/features)").arg(need);
  const int major = m.captured(1).toInt(), minor = m.captured(2).toInt();
  if (major < kMinGodotMajor || (major == kMinGodotMajor && minor < kMinGodotMinor))
    return QStringLiteral("%1 (the project is %2.%3)").arg(need).arg(major).arg(minor);
  return {};
}

QJsonObject readTextureManifest(const QString &root) {
  QFile f(QDir(root).filePath(QStringLiteral("textures/.level_editor_textures.json")));
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeTextureManifest(const QString &root, const QJsonObject &manifest) {
  return writeIfChanged(QDir(root).filePath(QStringLiteral("textures/.level_editor_textures.json")),
                        QJsonDocument(manifest).toJson(QJsonDocument::Indented))
      >= 0;
}

bool textureUpToDate(const QString &root, const TextureJob &t, const QJsonObject &manifest) {
  if (!QFileInfo::exists(QDir(root).filePath(t.rel)))
    return false;
  if (!t.localSource.isEmpty())
    return false;  // compared byte for byte by writeIfChanged
  return !t.checksum.isEmpty() && manifest.value(t.rel).toString() == t.checksum;
}

namespace {

QString readText(const QString &path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

WriteResult writeLevel(const Scene &scene, const LevelPlan &plan, const WriteOptions &opt) {
  WriteResult r;
  const QDir root(opt.root);
  const auto count = [&r](int w, const QString &err) {
    if (w > 0)
      ++r.written;
    else if (w == 0)
      ++r.unchanged;
    else
      r.errors << err;
  };
  const auto write = [&](const QString &rel, const QByteArray &bytes) {
    QString err;
    count(writeIfChanged(root.filePath(rel), bytes, &err), err);
  };
  const UidFn uid = [&root](const QString &rel) { return readUid(root.filePath(rel)); };

  // Local library images (server maps were downloaded by the caller).
  for (const TextureJob &t: plan.textures)
    if (!t.localSource.isEmpty()) {
      QFile in(t.localSource);
      if (in.open(QIODevice::ReadOnly))
        write(t.rel, in.readAll());
    }
  // Colour + opacity for alpha-scissor materials.
  for (const DerivedTexture &d: plan.derived) {
    QImage color(root.filePath(d.colorRel)), opacity(root.filePath(d.opacityRel));
    if (color.isNull()) {
      r.warnings << QStringLiteral("%1 is missing; %2 not made").arg(d.colorRel, d.rel);
      continue;
    }
    const QImage merged = mergeOpacity(color.convertToFormat(QImage::Format_RGBA8888),
                                       opacity.isNull() ? QImage() : opacity.convertToFormat(QImage::Format_RGBA8888));
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    merged.save(&buf, "PNG");
    write(d.rel, png);
  }
  // Import settings for textures Godot has not seen yet.
  const auto ensureImport = [&](const TextureJob &t) {
    const QString path = root.filePath(t.rel);
    if (QFileInfo::exists(path) && !QFileInfo::exists(path + QStringLiteral(".import")))
      write(t.rel + QStringLiteral(".import"), newTextureImport(t).toUtf8());
  };
  for (const TextureJob &t: plan.textures)
    ensureImport(t);
  for (const DerivedTexture &d: plan.derived) {
    TextureJob t;
    t.rel = d.rel;
    ensureImport(t);
  }

  for (const LutJob &l: plan.luts) {
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    l.strip.save(&buf, "PNG");
    write(l.rel, png);
    const QString imp = l.rel + QStringLiteral(".import");
    write(imp, mergeLutImport(readText(root.filePath(imp)), l.strip.height()).toUtf8());
    ++r.luts;
  }

  QStringList produced;  // files this export owns (cleanup of the next one)
  for (const MaterialJob &m: plan.materials) {
    if (!m.isShared)
      produced << m.tresRel;
    if (!m.writeTres)
      continue;
    const QString path = root.filePath(m.tresRel);
    write(m.tresRel, mergeMaterialTres(readText(path), m, uid).toUtf8());
  }
  if (plan.sky && QFileInfo::exists(root.filePath(plan.sky->texRel))) {
    write(plan.sky->tresRel, mergeSkyTres(readText(root.filePath(plan.sky->tresRel)), *plan.sky, uid).toUtf8());
    r.skyTres = plan.sky->tresRel;
  }
  for (const GlbJob &g: plan.glbs) {
    produced << g.rel;
    write(g.rel, g.bytes);
    const QString imp = g.rel + QStringLiteral(".import");
    write(imp, mergeGlbImport(readText(root.filePath(imp)), g.materials).toUtf8());
  }

  int ioEntities = 0;
  for (const Entity &e: scene.entities)
    if (const EntityClass *c = e.cls(); c && c->io)
      ++ioEntities;
  const QString ioRel = ioEntities ? QStringLiteral("%1/%2_io.gd").arg(plan.levelRel, plan.mapName) : QString();

  const QString sceneRel = QStringLiteral("%1/%2.tscn").arg(plan.levelRel, plan.mapName);
  r.scenePath = root.filePath(sceneRel);
  const SceneMerge sm = mergeLevelScene(readText(r.scenePath), scene.entities, plan, opt.sun, ioRel, opt.unitScale, uid);
  r.warnings += sm.warnings;
  write(sceneRel, sm.text.toUtf8());

  if (ioEntities) {
    HullMap hulls;
    for (const auto &o: scene.gameObjects) {
      if (!o || !o->brushEntity() || o->modReplaced()) // a replaced modifier source: its output stands in
        continue;
      const auto b = ConvexBrush::fromObject(*o);
      if (!b)
        continue;
      std::vector<glm::vec3> pts;
      for (const auto &poly: b->polygons)
        for (const auto &p: poly) {
          const glm::vec3 v(p);
          if (std::none_of(pts.begin(), pts.end(), [&v](const glm::vec3 &q) { return glm::distance(q, v) < 1e-4f; }))
            pts.push_back(v);
        }
      if (pts.size() >= 4)
        hulls[o->brushEntity()].push_back(std::move(pts));
    }
    // Rigid bodies: their few simplified hulls (as the editor's Box3D bodies), not one per brush.
    for (auto &[id, list]: hulls) {
      const Entity *ent = entityById(scene, id);
      if (!ent || ent->classname != kRigidBodyClass)
        continue;
      eng::hulls::Options ho;
      ho.maxHulls = std::clamp(int(std::lround(ent->num(QStringLiteral("max_hulls"), 4.0))), 1, 16);
      list = eng::hulls::collisionHulls(list, ho);
    }
    ExportOptions io;
    io.mapName = plan.mapName;
    io.sourcePath = opt.sourcePath;
    io.unitScale = opt.unitScale;
    io.nodePaths = sm.entityNodes;
    int ents = 0, conns = 0;
    write(ioRel, buildScript(scene.entities, hulls, io, &ents, &conns).toUtf8());
    r.ioEntities = ents;
    produced << ioRel;
    if (opt.writeAddon) {
      QString err;
      const int n = writeAddon(opt.root, &err);
      if (n < 0)
        r.errors << err;
    }
  }

  // Files the previous export wrote and this one did not: removed with their .import. Only inside this level's folder.
  const QString manifestRel = plan.levelRel + QStringLiteral("/.export_manifest.json");
  QSet<QString> now(produced.begin(), produced.end());
  {
    QFile mf(root.filePath(manifestRel));
    if (mf.open(QIODevice::ReadOnly)) {
      const QJsonArray old = QJsonDocument::fromJson(mf.readAll()).object().value(QStringLiteral("files")).toArray();
      for (const auto &v: old) {
        const QString rel = v.toString();
        if (now.contains(rel) || !rel.startsWith(plan.levelRel + QLatin1Char('/')) || rel.contains(QLatin1String("..")))
          continue;
        if (QFile::remove(root.filePath(rel)))
          ++r.removed;
        QFile::remove(root.filePath(rel + QStringLiteral(".import")));
      }
    }
  }
  produced.sort();
  QJsonArray files;
  for (const QString &p: produced)
    files.append(p);
  QJsonObject manifest;
  manifest[QStringLiteral("version")] = 1;
  manifest[QStringLiteral("files")] = files;
  write(manifestRel, QJsonDocument(manifest).toJson(QJsonDocument::Indented));
  return r;
}

} // namespace eng::godot
