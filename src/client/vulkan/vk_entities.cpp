// Point entities: create / delete / edit with undo, selection, the light list
// the shader reads, and marker data for the views.

#include <algorithm>
#include <cfloat>

#include <QScopeGuard>

#include <glm/gtc/matrix_transform.hpp>

#include "vk_engine.h"
#include "vk_undo.h"

#include "client/engine/entity.h"
#include "client/engine/map_format.h"
#include "client/engine/scene.h"
#include "client/engine/model_loader.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

eng::Entity *findEnt(const QSharedPointer<eng::Scene> &scene, quint32 id) {
  return scene ? scene->findEntity(id) : nullptr;
}

} // namespace

const eng::Entity *Engine3D::entityById(const quint32 id) const {
  return findEnt(m_scene, id);
}

std::vector<eng::Entity> *Engine3D::sceneEntities() const {
  return m_scene ? &m_scene->entities : nullptr;
}

quint32 Engine3D::addEntity(const QString &classname, const glm::vec3 &origin, const QVariantMap &props) {
  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  const eng::EntityClass *cls = eng::EntityRegistry::instance().find(classname);
  if (!cls)
    return 0;
  eng::Entity e;
  e.id = m_scene->mintEntityId();
  e.classname = classname;
  e.origin = origin;
  e.props = props;
  // Spots face down by default; a new sun starts where the map's sun is.
  if (cls->kind == eng::EntityClass::Kind::SpotLight)
    e.angles = glm::vec3(-90.0f, 0.0f, 0.0f);
  if (cls->kind == eng::EntityClass::Kind::Sun) {
    const glm::vec3 d = glm::normalize(m_sunDirection);
    e.angles = glm::vec3(glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f))), glm::degrees(std::atan2(d.x, d.z)), 0.0f);
    e.props.insert(QStringLiteral("intensity"), double(m_sunIntensity));
    e.props.insert(QStringLiteral("color"), sunColor().name());
  }
  const quint32 id = e.id;
  auto apply = [this, e](bool add) {
    if (!m_scene)
      return;
    auto &v = m_scene->entities;
    if (add) {
      if (!m_scene->findEntity(e.id))
        v.push_back(e);
    } else {
      v.erase(std::remove_if(v.begin(), v.end(), [&e](const eng::Entity &x) { return x.id == e.id; }), v.end());
      m_selectedEntities.erase(e.id);
    }
    ++m_entityEpoch;
    emit entitiesChanged();
    emit selectionChanged();
  };
  apply(true);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Add %1").arg(cls->label),
                                                           [apply] { apply(false); }, [apply] { apply(true); }));
  selectMarker(id, false);
  return id;
}

const eng::Entity *Engine3D::worldEnvEntity() const {
  if (!m_scene)
    return nullptr;
  for (const eng::Entity &e: m_scene->entities)
    if (e.classname == eng::kWorldEnvClass)
      return &e;
  return nullptr;
}

bool Engine3D::worldEnvSelected() const {
  const eng::Entity *w = worldEnvEntity();
  return w && m_selectedEntities.size() == 1 && m_selectedEntities.count(w->id) && m_selectedObjects.empty()
      && m_selectedFaces.empty();
}

void Engine3D::selectWorldEnv() {
  ensureWorldEnv();
  if (const eng::Entity *w = worldEnvEntity()) {
    m_selectedObjects.clear();
    m_selectedFaces.clear();
    m_selectedEntities = {w->id};
    emit selectionChanged();
  }
}

void Engine3D::ensureWorldEnv() {
  if (!m_scene)
    return;
  // One per map: a loaded map's own world_env wins over the stand-in made when the scene was set.
  auto &ents = m_scene->entities;
  int count = 0;
  for (const eng::Entity &x: ents)
    count += x.classname == eng::kWorldEnvClass ? 1 : 0;
  if (count > 1) {
    bool kept = false;
    const quint32 standIn = m_autoWorldEnvId;
    std::vector<eng::Entity> out;
    for (const eng::Entity &x: ents) {
      if (x.classname != eng::kWorldEnvClass) {
        out.push_back(x);
      } else if (!kept && x.id != standIn) {
        out.push_back(x);
        kept = true;
      }
    }
    if (!kept) // only stand-ins (cannot happen twice, but be safe)
      for (const eng::Entity &x: ents)
        if (x.classname == eng::kWorldEnvClass) {
          out.insert(out.begin(), x);
          break;
        }
    ents = std::move(out);
    m_selectedEntities.erase(standIn);
    ++m_entityEpoch;
    emit entitiesChanged();
    return;
  }
  if (count == 1)
    return;
  // Part of every map, not an edit: no undo step, the map stays clean. Placed above the middle of the level.
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  for (const auto &o: m_scene->gameObjects) {
    const auto [a, b] = o->getWorldAABB();
    lo = glm::min(lo, a);
    hi = glm::max(hi, b);
  }
  eng::Entity e;
  e.id = m_scene->mintEntityId();
  e.classname = eng::kWorldEnvClass;
  e.origin = lo.x <= hi.x ? glm::vec3((lo.x + hi.x) * 0.5f, hi.y + 6.4f, (lo.z + hi.z) * 0.5f)
                          : glm::vec3(12.8f, 12.8f, 0.0f); // 128 units up and 128 along +X
  m_scene->entities.insert(m_scene->entities.begin(), e);
  m_autoWorldEnvId = e.id;
  ++m_entityEpoch;
  emit entitiesChanged();
}

void Engine3D::deleteEntities(const std::vector<quint32> &ids) {
  if (!m_scene || ids.empty())
    return;
  std::vector<std::pair<int, eng::Entity>> removed;
  auto &v = m_scene->entities;
  for (int i = int(v.size()) - 1; i >= 0; --i)
    if (std::find(ids.begin(), ids.end(), v[size_t(i)].id) != ids.end()
        && v[size_t(i)].classname != eng::kWorldEnvClass) // part of every map
      removed.emplace_back(i, v[size_t(i)]);
  if (removed.empty())
    return;
  auto doRemove = [this, removed] {
    auto &ents = m_scene->entities;
    for (const auto &[slot, e]: removed) {
      ents.erase(std::remove_if(ents.begin(), ents.end(), [&e](const eng::Entity &x) { return x.id == e.id; }),
                 ents.end());
      m_selectedEntities.erase(e.id);
    }
    ++m_entityEpoch;
    emit entitiesChanged();
    emit selectionChanged();
  };
  auto doRestore = [this, removed] {
    auto &ents = m_scene->entities;
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
      const int slot = std::min<int>(it->first, int(ents.size()));
      ents.insert(ents.begin() + slot, it->second);
    }
    ++m_entityEpoch;
    emit entitiesChanged();
  };
  // A brush entity's brushes go with it (or back to the world, by preference).
  std::vector<quint32> ownedBrushes;
  for (const auto &[slot, e]: removed)
    if (const eng::EntityClass *c = e.cls(); c && c->isBrushEntity())
      for (const quint32 b: brushesOfEntity(e.id))
        ownedBrushes.push_back(b);
  const bool record = m_undoStack && !m_undoStack->isApplying();
  if (record && !ownedBrushes.empty())
    m_undoStack->beginMacro(QStringLiteral("Delete brush entity"));
  if (!ownedBrushes.empty()) {
    if (prefs::b("Entity/DeleteBrushesWithEntity")) {
      replaceObjects(ownedBrushes, {}, QStringLiteral("Delete entity brushes"));
    } else {
      auto setOwner = [this, ownedBrushes](bool clear, quint32 owner) {
        for (const quint32 b: ownedBrushes)
          if (const auto o = m_scene ? m_scene->findById(b) : nullptr)
            o->setBrushEntity(clear ? 0 : owner);
        emit objectStateChanged();
      };
      const quint32 owner = m_scene->findById(ownedBrushes.front())->brushEntity();
      setOwner(true, owner);
      if (record)
        m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Untie brushes"),
                                                               [setOwner, owner] { setOwner(false, owner); },
                                                               [setOwner, owner] { setOwner(true, owner); }));
    }
  }
  doRemove();
  if (record)
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        removed.size() == 1 ? QStringLiteral("Delete entity") : QStringLiteral("Delete %1 entities").arg(removed.size()),
        doRestore, doRemove));
  if (record && !ownedBrushes.empty())
    m_undoStack->endMacro();
}

void Engine3D::setEntityProperty(const quint32 id, const QString &key, const QVariant &value) {
  eng::Entity *e = findEnt(m_scene, id);
  if (!e)
    return;
  const QVariant before = e->props.value(key);
  const bool hadBefore = e->props.contains(key);
  auto apply = [this, id, key](const QVariant &v, bool present) {
    if (eng::Entity *x = findEnt(m_scene, id)) {
      if (present)
        x->props.insert(key, v);
      else
        x->props.remove(key);
      ++m_entityEpoch;
      emit entitiesChanged();
    }
  };
  apply(value, true);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        QStringLiteral("Set %1").arg(key), [apply, before, hadBefore] { apply(before, hadBefore); },
        [apply, value] { apply(value, true); }, QStringLiteral("ent.%1.%2").arg(id).arg(key)));
}

void Engine3D::resetEntityProperty(const quint32 id, const QString &key) {
  eng::Entity *e = findEnt(m_scene, id);
  if (!e || !e->props.contains(key))
    return;
  const QVariant before = e->props.value(key);
  auto apply = [this, id, key](const QVariant &v, bool present) {
    if (eng::Entity *x = findEnt(m_scene, id)) {
      if (present)
        x->props.insert(key, v);
      else
        x->props.remove(key);
      ++m_entityEpoch;
      emit entitiesChanged();
    }
  };
  apply({}, false);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        QStringLiteral("Reset %1").arg(key), [apply, before] { apply(before, true); },
        [apply] { apply({}, false); }));
}

namespace {
struct PropState {
  quint32 id;
  QVariant value;
  bool present;
};
} // namespace

void Engine3D::setEntitiesProperty(const std::vector<quint32> &ids, const QString &key, const QVariant &value) {
  if (ids.size() == 1) {
    setEntityProperty(ids.front(), key, value);
    return;
  }
  std::vector<PropState> was, now;
  quint64 group = 1469598103934665603ull;
  for (const quint32 id: ids)
    if (const eng::Entity *e = findEnt(m_scene, id)) {
      was.push_back({id, e->props.value(key), e->props.contains(key)});
      now.push_back({id, value, true});
      group = (group ^ id) * 1099511628211ull;
    }
  if (was.empty())
    return;
  auto apply = [this, key](const std::vector<PropState> &states) {
    for (const PropState &st: states)
      if (eng::Entity *x = findEnt(m_scene, st.id)) {
        if (st.present)
          x->props.insert(key, st.value);
        else
          x->props.remove(key);
      }
    ++m_entityEpoch;
    emit entitiesChanged();
  };
  apply(now);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        QStringLiteral("Set %1").arg(key), [apply, was] { apply(was); }, [apply, now] { apply(now); },
        QStringLiteral("ents.%1.%2").arg(group).arg(key)));
}

void Engine3D::resetEntitiesProperty(const std::vector<quint32> &ids, const QString &key) {
  if (ids.size() == 1) {
    resetEntityProperty(ids.front(), key);
    return;
  }
  std::vector<PropState> was, now;
  for (const quint32 id: ids)
    if (const eng::Entity *e = findEnt(m_scene, id); e && e->props.contains(key)) {
      was.push_back({id, e->props.value(key), true});
      now.push_back({id, {}, false});
    }
  if (was.empty())
    return;
  auto apply = [this, key](const std::vector<PropState> &states) {
    for (const PropState &st: states)
      if (eng::Entity *x = findEnt(m_scene, st.id)) {
        if (st.present)
          x->props.insert(key, st.value);
        else
          x->props.remove(key);
      }
    ++m_entityEpoch;
    emit entitiesChanged();
  };
  apply(now);
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(QStringLiteral("Reset %1").arg(key),
                                                           [apply, was] { apply(was); }, [apply, now] { apply(now); }));
}

void Engine3D::setEntityTransform(const quint32 id, const glm::vec3 &origin, const glm::vec3 &angles,
                                  const QString &name, const bool record) {
  eng::Entity *e = findEnt(m_scene, id);
  if (!e)
    return;
  const glm::vec3 o0 = e->origin, a0 = e->angles;
  const QString n0 = e->name;
  auto apply = [this, id](const glm::vec3 &o, const glm::vec3 &a, const QString &n) {
    if (eng::Entity *x = findEnt(m_scene, id)) {
      x->origin = o;
      x->angles = a;
      x->name = n;
      ++m_entityEpoch;
      emit entitiesChanged();
    }
  };
  apply(origin, angles, name);
  if (record && m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        QStringLiteral("Edit entity"), [apply, o0, a0, n0] { apply(o0, a0, n0); },
        [apply, origin, angles, name] { apply(origin, angles, name); }, QStringLiteral("enttf.%1").arg(id)));
}

void Engine3D::recordEntityMoves(const std::map<quint32, glm::vec3> &before, const QString &label) {
  if (!m_scene || before.empty() || !m_undoStack || m_undoStack->isApplying())
    return;
  std::map<quint32, glm::vec3> after;
  bool moved = false;
  for (const auto &[id, o]: before)
    if (const eng::Entity *e = findEnt(m_scene, id)) {
      after[id] = e->origin;
      moved = moved || glm::length(e->origin - o) > 1e-5f;
    }
  if (!moved)
    return;
  auto apply = [this](const std::map<quint32, glm::vec3> &m) {
    for (const auto &[id, o]: m)
      if (eng::Entity *e = findEnt(m_scene, id))
        e->origin = o;
    ++m_entityEpoch;
    emit entitiesChanged();
  };
  m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply, before] { apply(before); },
                                                         [apply, after] { apply(after); }));
}

std::map<quint32, glm::vec3> Engine3D::selectedEntityOrigins() const {
  std::map<quint32, glm::vec3> out;
  for (const quint32 id: m_selectedEntities)
    if (const eng::Entity *e = findEnt(m_scene, id))
      out[id] = e->origin;
  return out;
}

void Engine3D::moveSelectedEntities(const std::map<quint32, glm::vec3> &base, const glm::vec3 &delta) {
  for (const auto &[id, o]: base)
    if (eng::Entity *e = findEnt(m_scene, id))
      e->origin = o + delta;
  ++m_entityEpoch;
  emit entitiesChanged();
}

bool Engine3D::markerSelected(const quint32 id) const {
  return m_selectedEntities.count(id) != 0;
}

void Engine3D::selectMarker(const quint32 id, const bool additive) {
  if (!findEnt(m_scene, id))
    return;
  // A marker click is a selection step like a brush click (Object mode).
  const bool step = !m_materialMode && !m_editMode;
  if (step)
    beginSelectionStep();
  const auto stepEnd = qScopeGuard([this, step] {
    if (step)
      endSelectionStep();
  });
  bool on = true;
  if (additive) {
    on = !m_selectedEntities.erase(id);
    if (on)
      m_selectedEntities.insert(id);
  } else {
    m_selectedObjects.clear();
    m_selectedFaces.clear();
    m_selectedMods.clear();
    m_selectedPaths.clear();
    m_selectedEntities = {id};
  }
  // A brush entity is its brushes: selected as a click on one of them would.
  if (const eng::Entity *e = findEnt(m_scene, id); e && !m_materialMode && e->cls() && e->cls()->isBrushEntity()) {
    for (const quint32 b: brushesOfEntity(id)) {
      const auto o = m_scene->findById(b);
      if (!o || o->hidden() || o->locked() || o->isDerived())
        continue;
      if (on)
        m_selectedObjects.insert(b);
      else
        m_selectedObjects.erase(b);
    }
  }
  refreshDrawSelection();
  emit selectionChanged();
}

void Engine3D::setSelectedEntities(const std::vector<quint32> &ids) {
  m_selectedEntities.clear();
  for (const quint32 id: ids)
    if (findEnt(m_scene, id))
      m_selectedEntities.insert(id);
  emit selectionChanged();
}

void Engine3D::appendEntityMarkers(QVector<EntityMarker> &markers) const {
  if (!m_scene)
    return;
  for (const eng::Entity &e: m_scene->entities) {
    if (e.hidden)
      continue;
    const eng::EntityClass *cls = e.cls();
    // Brush entities are their brushes: no marker.
    if (cls && cls->isBrushEntity())
      continue;
    EntityMarker m;
    m.kind = EntityKind::Generic;
    m.id = e.id;
    m.position = e.origin;
    const glm::vec3 size = cls ? cls->size : glm::vec3(1.2f);
    m.halfSize = size * 0.5f;
    QColor col = cls ? cls->color : QColor(95, 211, 255);
    const bool light = cls && (cls->kind == eng::EntityClass::Kind::PointLight
                               || cls->kind == eng::EntityClass::Kind::SpotLight);
    if (light)
      col = e.value(QStringLiteral("enabled")).toBool() ? e.color(QStringLiteral("light_color")) : QColor(90, 90, 90);
    m.color = glm::vec4(col.redF(), col.greenF(), col.blueF(), 1.0f);
    m.selected = m_selectedEntities.count(e.id) != 0;
    m.icon = entityIconFor(cls);
    // Icons keep their own colours; a light that is off shows grey.
    const bool off = light && !e.value(QStringLiteral("enabled")).toBool();
    m.iconTint = off ? glm::vec4(0.45f, 0.45f, 0.45f, 0.9f) : glm::vec4(1.0f);
    // I/O simulator: an input flashes, a killed entity goes dark, the selected one reads out its state.
    if (m_ioSim) {
      if (ioSimKilled(e.id))
        m.iconTint = glm::vec4(0.3f, 0.3f, 0.3f, 0.6f);
      else if (ioSimFlashing(e.id))
        m.iconTint = glm::vec4(1.0f, 0.95f, 0.35f, 1.0f);
    }
    if (light)
      m.range = eng::lightRangeWorld(e);
    if (cls && cls->kind == eng::EntityClass::Kind::SpotLight) {
      m.direction = e.forward();
      m.coneAngle = float(e.num(QStringLiteral("spot_angle"), 45.0));
    }
    if (entityRotatable(e)) {
      // Local +Z: forward() for most classes, the model's own axis for props.
      m.rotatable = true;
      m.direction = eng::entityRotation(e) * glm::vec3(0.0f, 0.0f, 1.0f);
    }
    if (e.classname == QLatin1String("path_anchor")) {
      m.direction = e.forward();
      m.facingArrow = true;
    }
    m.label = (e.name.isEmpty() ? (cls ? cls->label : e.classname) : e.name);
    if (light)
      m.label += QStringLiteral("  energy %1").arg(e.num(QStringLiteral("light_energy")), 0, 'f', 2);
    if (m_ioSim)
      if (const QString st = ioSimState(e.id); !st.isEmpty())
        m.label += QStringLiteral("  [%1]").arg(st);
    markers.push_back(m);
  }
}

const eng::Entity *Engine3D::sunEntity() const {
  if (!m_scene)
    return nullptr;
  for (const eng::Entity &e: m_scene->entities)
    if (e.classname == QLatin1String("sun") && !e.hidden)
      return &e;
  return nullptr;
}

void Engine3D::applySunEntity() {
  const eng::Entity *e = sunEntity();
  if (!e)
    return;
  // Written straight through (no undo): the entity's own property edits are what is recorded.
  const glm::vec3 dir = e->forward();
  const QColor c = e->color(QStringLiteral("color"));
  const glm::vec3 col(c.redF(), c.greenF(), c.blueF());
  const float intensity = float(e->num(QStringLiteral("intensity"), 3.7));
  const bool on = e->value(QStringLiteral("enabled")).toBool();
  const bool shadows = on && e->value(QStringLiteral("castShadows")).toBool();
  if (glm::length(dir - m_sunDirection) > 1e-5f || glm::length(col - m_sunColor) > 1e-5f
      || std::fabs(intensity - m_sunIntensity) > 1e-5f || shadows != m_shadowsEnabled) {
    m_sunDirection = glm::normalize(dir);
    m_sunColor = col;
    m_sunIntensity = on ? intensity : 0.0f;
    m_shadowsEnabled = shadows;
    syncSunToSceneLight();
    emit sunChanged();
  }
}

int Engine3D::fillLightUniforms(glm::vec4 *out, const int maxLights, std::vector<quint32> *ids) const {
  if (!m_scene || noLocalLights())
    return 0;
  // Nearest lights first, so the ones that matter win when the shader's limit is hit.
  struct L {
    const eng::Entity *e;
    float d;
  };
  std::vector<L> lights;
  const glm::vec3 cam = m_camera->getPosition();
  const float maxDist = lightRenderDistanceWorld();
  for (const eng::Entity &e: m_scene->entities) {
    const eng::EntityClass *cls = e.cls();
    if (!cls || e.hidden || !e.value(QStringLiteral("enabled")).toBool())
      continue;
    if (cls->kind != eng::EntityClass::Kind::PointLight && cls->kind != eng::EntityClass::Kind::SpotLight)
      continue;
    const float range = eng::lightRangeWorld(e);
    const float d = std::max(0.0f, glm::length(e.origin - cam) - range);
    if (d > maxDist)
      continue;
    lights.push_back({&e, d});
  }
  std::sort(lights.begin(), lights.end(), [](const L &a, const L &b) { return a.d < b.d; });
  const int n = std::min<int>(int(lights.size()), maxLights);
  for (int i = 0; i < n; ++i) {
    const eng::Entity &e = *lights[size_t(i)].e;
    if (ids)
      ids->push_back(e.id);
    const bool spot = e.cls()->kind == eng::EntityClass::Kind::SpotLight;
    const QColor c = e.color(QStringLiteral("light_color"));
    const float range = eng::lightRangeWorld(e);
    // Linear-light colour: the swatch is sRGB (Godot's srgb_to_linear).
    const auto lin = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
    const glm::vec3 dir = e.forward();
    // As Godot's LightStorage::update_light_buffers: colour * energy * PI, specular_amount = light_specular * 2,
    // cone = cos(spot_angle); attenuation and spot_angle_attenuation are Godot's defaults (1).
    const float cone = float(std::cos(glm::radians(std::clamp(e.num(QStringLiteral("spot_angle"), 45.0), 0.01, 180.0))));
    out[i * 4 + 0] = glm::vec4(e.origin, range);
    out[i * 4 + 1] = glm::vec4(lin(c.redF()), lin(c.greenF()), lin(c.blueF()),
                               float(e.num(QStringLiteral("light_energy"), 1.0) * 3.14159265));
    out[i * 4 + 2] = glm::vec4(dir, spot ? cone : -2.0f);
    out[i * 4 + 3] = glm::vec4(spot ? 1.0f : -1.0f, 1.0f, spot ? 1.0f : 0.0f,
                               float(e.num(QStringLiteral("light_specular"), 0.5) * 2.0));
  }
  return n;
}

namespace {

fmap::Value toValue(const QVariant &v) {
  switch (v.typeId()) {
    case QMetaType::Bool: return fmap::Value::makeBool(v.toBool());
    case QMetaType::Int:
    case QMetaType::LongLong: return fmap::Value::makeInt(v.toInt());
    case QMetaType::Double:
    case QMetaType::Float: return fmap::Value::makeFloat(v.toFloat());
    case QMetaType::QColor: return fmap::Value::makeString(v.value<QColor>().name());
    default: return fmap::Value::makeString(v.toString());
  }
}

QVariant fromValue(const fmap::Value &v) {
  switch (v.kind) {
    case fmap::ValueKind::Bool: return v.boolValue;
    case fmap::ValueKind::Int32: return v.intValue;
    case fmap::ValueKind::Float: return double(v.floatValue);
    default: return v.string;
  }
}

} // namespace

void Engine3D::captureEntities(std::vector<fmap::EntityRecord> &out) const {
  if (!m_scene)
    return;
  for (const eng::Entity &e: m_scene->entities) {
    fmap::EntityRecord r;
    r.id = e.id;
    r.type = QStringLiteral("Entity");
    r.name = e.name;
    r.position = e.origin;
    r.set(QStringLiteral("classname"), fmap::Value::makeString(e.classname));
    r.set(QStringLiteral("angles"), fmap::Value::makeVec3(e.angles));
    if (e.hidden)
      r.set(QStringLiteral("hidden"), fmap::Value::makeBool(true));
    for (auto it = e.props.constBegin(); it != e.props.constEnd(); ++it)
      r.set(QStringLiteral("p.") + it.key(), toValue(it.value()));
    out.push_back(r);
  }
}

bool Engine3D::applyEntity(const fmap::EntityRecord &r) {
  if (r.type != QLatin1String("Entity") || !m_scene)
    return false;
  eng::Entity e;
  e.id = r.id >= eng::kEntityIdBase ? r.id : m_scene->mintEntityId();
  e.name = r.name;
  e.origin = r.position;
  for (const auto &[key, value]: r.properties) {
    if (key == QLatin1String("classname"))
      e.classname = value.string;
    else if (key == QLatin1String("angles"))
      e.angles = value.asVec3();
    else if (key == QLatin1String("hidden"))
      e.hidden = value.asBool();
    else if (key.startsWith(QLatin1String("p.")))
      e.props.insert(key.mid(2), fromValue(value));
  }
  if (e.classname.isEmpty())
    return false;
  // Maps from before entity I/O: trigger_multiple had a plain "target" string.
  if (e.classname == QLatin1String("trigger_multiple") && e.props.contains(QStringLiteral("target"))) {
    const QString t = e.props.take(QStringLiteral("target")).toString().trimmed();
    if (!t.isEmpty()) {
      eng::io::Connection c;
      c.output = QStringLiteral("OnTrigger");
      c.target = t;
      c.input = QStringLiteral("Trigger");
      e.outputs.push_back(c);
      qInfo().noquote() << QStringLiteral("[io] trigger_multiple %1: old target \"%2\" became OnTrigger → %2 → Trigger")
                               .arg(e.id).arg(t);
    }
  }
  m_scene->nextEntityId = std::max(m_scene->nextEntityId, e.id + 1);
  m_scene->entities.push_back(e);
  ++m_entityEpoch;
  return true;
}

} // namespace vk

namespace vk {

bool Engine3D::raycastScene(const glm::vec3 &origin, const glm::vec3 &dir, float &tOut, glm::vec3 &normalOut,
                            quint32 *objectOut, int *faceOut) const {
  if (!m_scene)
    return false;
  float bestT = 1e30f;
  bool hit = false;
  const glm::vec3 invDir(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);
  for (const auto &obj: m_scene->gameObjects) {
    if (obj->hidden() || toolBrushHidden(*obj))
      continue;
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;
    const auto [lo, hi] = obj->getWorldAABB();
    // slab test
    float t0 = 0.0f, t1 = 1e30f;
    bool miss = false;
    for (int a = 0; a < 3 && !miss; ++a) {
      float ta = (lo[a] - origin[a]) * invDir[a];
      float tb = (hi[a] - origin[a]) * invDir[a];
      if (ta > tb)
        std::swap(ta, tb);
      t0 = std::max(t0, ta);
      t1 = std::min(t1, tb);
      miss = t0 > t1;
    }
    if (miss || t0 > bestT)
      continue;
    const glm::mat4 model = obj->getModelMatrix();
    const auto &idx = mesh->indexData();
    const auto &vtx = mesh->vertexData();
    for (int f = 0; f < mesh->faceCount(); ++f) {
      const size_t base = mesh->faceIndexStart(f), cnt = mesh->faceIndexCount(f);
      for (size_t t = 0; t + 2 < cnt && base + t + 2 < idx.size(); t += 3) {
        const glm::vec3 a(model * glm::vec4(vtx[idx[base + t]].position, 1.0f));
        const glm::vec3 b(model * glm::vec4(vtx[idx[base + t + 1]].position, 1.0f));
        const glm::vec3 c(model * glm::vec4(vtx[idx[base + t + 2]].position, 1.0f));
        // Möller–Trumbore, both sides.
        const glm::vec3 e1 = b - a, e2 = c - a, p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-9f)
          continue;
        const float inv = 1.0f / det;
        const glm::vec3 s = origin - a;
        const float u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f)
          continue;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f)
          continue;
        const float t2 = glm::dot(e2, q) * inv;
        if (t2 > 1e-4f && t2 < bestT) {
          bestT = t2;
          glm::vec3 n = glm::normalize(glm::cross(e1, e2));
          if (glm::dot(n, dir) > 0.0f)
            n = -n;
          normalOut = n;
          hit = true;
          if (objectOut)
            *objectOut = obj->stableId();
          if (faceOut)
            *faceOut = f;
        }
      }
    }
  }
  tOut = bestT;
  return hit;
}

glm::vec3 Engine3D::placementPoint(const float px, const float py, const float lift) const {
  glm::vec3 ro, rd;
  const float w = std::max(1.0f, float(m_lastViewSize.width()));
  const float h = std::max(1.0f, float(m_lastViewSize.height()));
  // A point outside the 3D view places in front of the camera.
  const bool inside = px >= 0 && py >= 0 && px < w && py < h;
  makeRay(inside ? px : w * 0.5f, inside ? py : h * 0.5f, w, h, ro, rd);
  float t = 0.0f;
  glm::vec3 n(0.0f, 1.0f, 0.0f);
  glm::vec3 p;
  if (raycastScene(ro, rd, t, n) && t < renderDistanceWorld())
    p = ro + rd * t + n * lift;
  else
    p = ro + rd * 20.0f;
  // Snapped to a quarter of the grid step.
  const double step = gizmoSnapStep() * 0.25;
  if (step > 0)
    p = glm::vec3(glm::round(glm::dvec3(p) / step) * step);
  return p;
}

quint32 Engine3D::placeEntityAt(const QString &classname, const float px, const float py) {
  const eng::EntityClass *cls = eng::EntityRegistry::instance().find(classname);
  if (!cls)
    return 0;
  // Lights hover a little off the surface; spawns stand on it.
  const float lift = cls->kind == eng::EntityClass::Kind::PlayerStart ? cls->size.y * 0.5f + 0.05f
                                                                       : std::max(cls->size.y, 2.4f);
  return addEntity(classname, placementPoint(px, py, lift));
}

bool Engine3D::applyMaterialAt(const QString &material, const float px, const float py) {
  glm::vec3 ro, rd;
  makeRay(px, py, std::max(1.0f, float(m_lastViewSize.width())), std::max(1.0f, float(m_lastViewSize.height())), ro,
          rd);
  float t;
  glm::vec3 n;
  quint32 obj = 0;
  int face = -1;
  if (!raycastScene(ro, rd, t, n, &obj, &face) || face < 0)
    return false;
  const auto o = m_scene->findById(obj);
  if (!o)
    return false;
  // Dropping on a selected brush paints the whole selection, else just the face under the cursor.
  const bool whole = m_selectedObjects.count(obj) != 0;
  const auto prevObjects = m_selectedObjects;
  const auto prevFaces = m_selectedFaces;
  if (!whole) {
    m_selectedObjects.clear();
    m_selectedFaces = {o->getMesh()->getFacePickingID(face)};
  }
  applyMaterialToSelection(material);
  if (!whole) {
    m_selectedObjects = prevObjects;
    m_selectedFaces = prevFaces;
    emit selectionChanged();
  }
  return true;
}

} // namespace vk

namespace vk {

void Engine3D::syncPropObjects(const bool blocking) {
  if (blocking)
    m_propEpoch = ~0ull;
  if (!m_scene || m_propEpoch == m_entityEpoch)
    return;
  m_propEpoch = m_entityEpoch;
  constexpr float unit = 3.937f;  // model units (metres) to world units, until models are supported properly
  std::set<quint32> wanted;
  std::vector<std::shared_ptr<eng::GameObject>> add;
  bool changed = false;
  for (const eng::Entity &e: m_scene->entities) {
    const eng::EntityClass *cls = e.cls();
    if (!cls || cls->kind != eng::EntityClass::Kind::Prop)
      continue;
    const QString path = e.value(QStringLiteral("model")).toString();
    if (path.isEmpty())
      continue;
    wanted.insert(e.id);
    const float scale = float(e.num(QStringLiteral("scale"), 1.0));
    auto &slot = m_propObjects[e.id];
    std::shared_ptr<eng::GameObject> obj = slot.stableId ? m_scene->findById(slot.stableId) : nullptr;
    if (!obj || slot.path != path) {
      if (obj) {
        m_scene->removeObjects({obj->stableId()});
        changed = true;
      }
      // Loads off the render thread; the callback re-runs this sync.
      const auto model = blocking ? eng::ModelCache::instance().get(path, unit)
                                  : eng::ModelCache::instance().request(path, unit, this, [this] {
                                      ++m_entityEpoch;
                                      requestRedraw();
                                    });
      slot.stableId = 0;
      if (!model) {
        slot.path.clear(); // not loaded yet: try again on the next sync
        continue;
      }
      slot.path = path;
      if (!model->ok())
        continue;
      auto mesh = eng::meshFromModel(*model);
      obj = std::make_shared<eng::GameObject>(mesh);
      for (int f = 0; f < mesh->faceCount(); ++f)
        mesh->setFacePickingID(f, eng::makePickingId(obj->stableId(), uint32_t(f)));
      obj->setOwnerEntity(e.id);
      slot.stableId = obj->stableId();
      add.push_back(obj);
    }
    obj->setPosition(e.origin);
    obj->setRotation(glm::vec3(e.angles.x, e.angles.y, e.angles.z));
    obj->setScale(glm::vec3(scale));
    obj->setHidden(e.hidden);
  }
  for (auto it = m_propObjects.begin(); it != m_propObjects.end();) {
    if (!wanted.count(it->first)) {
      if (it->second.stableId) {
        m_scene->removeObjects({it->second.stableId});
        changed = true;
      }
      it = m_propObjects.erase(it);
    } else {
      ++it;
    }
  }
  if (!add.empty()) {
    m_scene->addObjects(add);
    changed = true;
  }
  if (changed)
    invalidateDerivedCaches();
}

} // namespace vk

// Godot's Camera3D fov is vertical (keep_aspect = KEEP_HEIGHT), like the editor camera's.

namespace vk {

namespace {

// Entity yaw 0 looks down +Z (Entity::forward), the editor camera's down +X.
constexpr float kCameraYawOffset = -90.0f;
constexpr float kFrustumLength = 2.4f; // world units (24 Source units): small, it only shows direction

glm::mat4 cameraBasis(const eng::Entity &e) {
  const glm::vec3 f = e.forward();
  glm::vec3 up(0.0f, 1.0f, 0.0f);
  if (std::fabs(glm::dot(f, up)) > 0.999f)
    up = glm::vec3(0.0f, 0.0f, 1.0f);
  const glm::vec3 r = glm::normalize(glm::cross(f, up));
  const glm::vec3 u = glm::cross(r, f);
  glm::mat4 m(1.0f);
  m[0] = glm::vec4(r, 0.0f);
  m[1] = glm::vec4(u, 0.0f);
  m[2] = glm::vec4(-f, 0.0f);
  m[3] = glm::vec4(e.origin, 1.0f);
  return m;
}

} // namespace

quint32 Engine3D::selectedCameraEntity() const {
  if (!m_scene)
    return 0;
  quint32 found = 0;
  for (const quint32 id: m_selectedEntities)
    if (const eng::Entity *e = findEnt(m_scene, id); e && e->classname == eng::kCameraClass && !e->hidden) {
      if (found)
        return m_previewCamera && m_selectedEntities.count(m_previewCamera) ? m_previewCamera : 0;
      found = id;
    }
  return found;
}

void Engine3D::setCameraPreview(const bool on) {
  if (on == (m_previewCamera != 0))
    return;
  if (on) {
    const quint32 id = selectedCameraEntity();
    if (!id)
      return;
    m_previewSavedPos = m_camera->getPosition();
    m_previewSavedYaw = m_camera->getYaw();
    m_previewSavedPitch = m_camera->getPitch();
    m_previewSavedFov = m_camera->getFov();
    m_previewCamera = id;
    applyCameraPreview();
  } else {
    m_previewCamera = 0;
    m_camera->setPose(m_previewSavedPos, m_previewSavedYaw, m_previewSavedPitch);
    m_camera->setFOV(m_previewSavedFov);
  }
  m_flyVelocity = glm::vec3(0.0f);
  emit cameraPreviewChanged();
  emit cameraOrientationChanged();
  requestRedraw();
}

void Engine3D::applyCameraPreview() {
  const eng::Entity *e = m_previewCamera ? findEnt(m_scene, m_previewCamera) : nullptr;
  if (!e)
    return;
  const float yaw = e->angles.y + kCameraYawOffset;
  const float pitch = std::clamp(e->angles.x, -89.0f, 89.0f);
  const float fov = float(std::clamp(e->num(QStringLiteral("fov"), 75.0), 1.0, 179.0));
  if (m_camera->getPosition() != e->origin || m_camera->getYaw() != yaw || m_camera->getPitch() != pitch
      || m_camera->getFov() != fov) {
    m_camera->setPose(e->origin, yaw, pitch);
    m_camera->setFOV(fov);
    m_cameraMovedThisFrame = true;
  }
}

// Preview stays on while other things are selected; it ends when switched off or its camera is hidden / deleted.
void Engine3D::syncCameraPreview() {
  if (m_previewCamera) {
    const eng::Entity *e = findEnt(m_scene, m_previewCamera);
    if (!e || e->hidden)
      setCameraPreview(false);
  }
  emit cameraPreviewChanged();
}

void Engine3D::addCameraFrustumDraws(
    const std::function<void(const EntityGeom &, const glm::mat4 &, const glm::vec4 &)> &add) {
  if (!m_scene)
    return;
  const float aspect = m_lastViewSize.height() > 0 ? float(m_lastViewSize.width()) / float(m_lastViewSize.height())
                                                   : 16.0f / 9.0f;
  for (const eng::Entity &e: m_scene->entities) {
    if (e.hidden || e.id == m_previewCamera || e.classname != eng::kCameraClass)
      continue;
    const float halfH = std::tan(glm::radians(float(std::clamp(e.num(QStringLiteral("fov"), 75.0), 1.0, 179.0))) * 0.5f);
    // The far rectangle is capped for very wide cameras.
    const float len = kFrustumLength / std::max(1.0f, halfH * aspect);
    const glm::mat4 m = cameraBasis(e) * glm::scale(glm::mat4(1.0f), glm::vec3(halfH * aspect * len, halfH * len, len));
    const bool selected = m_selectedEntities.count(e.id) != 0;
    add(m_geoFrustum, m, selected ? glm::vec4(1.0f, 0.62f, 0.25f, 1.0f) : glm::vec4(0.85f, 0.88f, 0.95f, 0.8f));
  }
}

} // namespace vk
