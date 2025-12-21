// Material properties of the open map: map layer and variants (engine/material_overrides.h), saving to the
// server, the project pack, swapping materials on faces. Every change is undoable.

#include <QJsonArray>
#include <QJsonObject>
#include <QtDebug>

#include <map>
#include <set>

#include "vk_engine.h"
#include "vk_shared.h"
#include "vk_undo.h"

#include "client/net/texture_catalog.h"

namespace vk {

namespace {

QString baseOf(const QString &ref) { return MaterialRef::parse(ref).base(); }

} // namespace

void Engine3D::applyMaterialOverrides(const eng::MaterialOverrides &overrides) {
  if (!m_scene)
    return;
  m_scene->materialOverrides = overrides;
  TextureManager::instance().setOverrides(&m_scene->materialOverrides);
  emit materialOverridesChanged();
}

void Engine3D::pushOverridesChange(const QString &label, const eng::MaterialOverrides &before, const QString &mergeKey) {
  if (!m_scene)
    return;
  const eng::MaterialOverrides after = m_scene->materialOverrides;
  TextureManager::instance().refreshProps();
  emit materialOverridesChanged();
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<eng::LambdaCommand>(
        label, [this, before] { applyMaterialOverrides(before); }, [this, after] { applyMaterialOverrides(after); },
        mergeKey));
}

QStringList Engine3D::selectionMaterialRefs() const {
  std::map<QString, int> counts;
  std::map<QString, QString> spelling;
  for (const auto &[sid, f]: selectedFaceRefs()) {
    const auto o = m_scene->findById(sid);
    if (!o || !o->getMesh())
      continue;
    const auto m = o->getMesh()->faceMaterialRef(f);
    if (!m)
      continue;
    ++counts[m->name];
    spelling[m->name] = m->ref.isEmpty() ? m->name : m->ref;
  }
  std::vector<std::pair<int, QString>> order;
  for (const auto &[k, n]: counts)
    order.emplace_back(-n, spelling[k]);
  std::sort(order.begin(), order.end());
  QStringList out;
  for (const auto &[n, ref]: order)
    out.append(ref);
  return out;
}

QStringList Engine3D::nearbyMaterialRefs(const int max) {
  QStringList out;
  if (!m_scene || max <= 0)
    return out;
  // The selected faces, or in Object mode every face of the selected brushes.
  std::vector<quint32> picks(m_selectedFaces.begin(), m_selectedFaces.end());
  if (picks.empty())
    picks = facePicksOfBrushes(m_selectedObjects);
  if (picks.empty())
    return out;
  const eng::FaceGraph &g = faceGraph();
  // Seeds for distances (an even sample of at most 256) and the materials they all have already.
  std::vector<int> picked;
  for (const quint32 pid: picks)
    if (const int i = g.indexOfPick(pid); i >= 0)
      picked.push_back(i);
  if (picked.empty())
    return out;
  std::map<QString, int> onSelected;
  for (const int i: picked)
    ++onSelected[g.nodes[size_t(i)].material];
  std::vector<glm::dvec3> seeds;
  const size_t stride = std::max<size_t>(1, picked.size() / 256);
  for (size_t k = 0; k < picked.size(); k += stride)
    seeds.push_back(g.nodes[size_t(picked[k])].centroid);
  const std::set<int> pickedSet(picked.begin(), picked.end());

  // Nearest face per material: distance from a seed to the face's box.
  std::map<QString, double> best;
  for (int i = 0; i < int(g.nodes.size()); ++i) {
    const eng::FaceNode &n = g.nodes[size_t(i)];
    if (n.hidden || n.material.isEmpty() || pickedSet.count(i))
      continue;
    if (const auto it = onSelected.find(n.material); it != onSelected.end() && it->second == int(picked.size()))
      continue; // already on every selected face
    double d2 = std::numeric_limits<double>::max();
    for (const glm::dvec3 &s: seeds) {
      const glm::dvec3 q = glm::clamp(s, n.mn, n.mx);
      d2 = std::min(d2, glm::dot(s - q, s - q));
    }
    auto [it, fresh] = best.try_emplace(n.material, d2);
    if (!fresh)
      it->second = std::min(it->second, d2);
  }
  std::vector<std::pair<double, QString>> order;
  for (const auto &[name, d2]: best)
    order.emplace_back(d2, name);
  std::sort(order.begin(), order.end());
  for (const auto &[d2, name]: order) {
    const auto m = TextureManager::instance().find(name);
    if (m && m->state == eng::Material::State::Error)
      continue; // missing
    out.append(m && !m->ref.isEmpty() ? m->ref : name);
    if (out.size() >= max)
      break;
  }
  return out;
}

int Engine3D::facesUsingMaterial(const QString &ref, bool selectionOnly) const {
  if (!m_scene)
    return 0;
  const QString key = ref.trimmed().toLower();
  int n = 0;
  if (selectionOnly) {
    for (const auto &[sid, f]: selectedFaceRefs())
      if (const auto o = m_scene->findById(sid); o && o->getMesh())
        if (const auto m = o->getMesh()->faceMaterialRef(f); m && m->name == key)
          ++n;
    return n;
  }
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o->getMesh();
    if (!mesh)
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (const auto m = mesh->faceMaterialRef(f); m && m->name == key)
        ++n;
  }
  return n;
}

void Engine3D::setMaterialProperty(const QString &ref, matprops::Key key, const QVariant &value) {
  if (!m_scene || ref.isEmpty())
    return;
  const eng::MaterialOverrides before = m_scene->materialOverrides;
  eng::MaterialOverrides &ov = m_scene->materialOverrides;
  const MaterialRef r = MaterialRef::parse(ref);

  // Writing the value the layer below gives (1-3 for the map layer, 1-4 for a variant) clears the key instead.
  bool inherited = false;
  if (const eng::MaterialPtr m = TextureManager::instance().find(ref)) {
    const eng::ResolvedProps res = TextureManager::instance().resolvedFor(*m);
    MaterialProps below = MaterialProps::godotDefaults();
    below.overlay(res.defaults);
    below.overlay(res.server);
    if (r.isVariant())
      below.overlay(res.map);
    MaterialProps written = below;
    written.setValue(key, value);
    inherited = written == below;
  }

  if (r.isVariant()) {
    const eng::MaterialVariant *v = ov.variant(ref);
    eng::MaterialVariant nv = v ? *v : eng::MaterialVariant{ref, r.base(), r.variant, {}};
    if (inherited)
      nv.props.unset(key);
    else
      nv.props.setValue(key, value);
    if (v || !nv.props.isEmpty() || !inherited)
      ov.setVariant(nv);
  } else {
    MaterialProps layer = ov.mapLayer(ref) ? *ov.mapLayer(ref) : MaterialProps();
    if (inherited)
      layer.unset(key);
    else
      layer.setValue(key, value);
    ov.setMapLayer(ref, layer);
  }
  if (ov == before)
    return;
  // Slider drags on one key of one material are one undo step.
  pushOverridesChange(QStringLiteral("Material: %1").arg(QLatin1String(matprops::def(key).label)), before,
                      QStringLiteral("matprop:%1:%2").arg(ref.toLower()).arg(int(key)));
}

void Engine3D::resetMaterialProperty(const QString &ref, matprops::Key key) {
  if (!m_scene)
    return;
  const eng::MaterialOverrides before = m_scene->materialOverrides;
  eng::MaterialOverrides &ov = m_scene->materialOverrides;
  if (MaterialRef::parse(ref).isVariant()) {
    if (const eng::MaterialVariant *v = ov.variant(ref)) {
      eng::MaterialVariant nv = *v;
      nv.props.unset(key);
      ov.setVariant(nv);
    }
  } else if (const MaterialProps *layer = ov.mapLayer(ref)) {
    MaterialProps l = *layer;
    l.unset(key);
    ov.setMapLayer(ref, l);
  }
  if (ov == before)
    return;
  pushOverridesChange(QStringLiteral("Reset %1").arg(QLatin1String(matprops::def(key).label)), before);
}

void Engine3D::revertMaterialOverrides(const QString &ref) {
  if (!m_scene)
    return;
  const eng::MaterialOverrides before = m_scene->materialOverrides;
  eng::MaterialOverrides &ov = m_scene->materialOverrides;
  if (MaterialRef::parse(ref).isVariant()) {
    if (const eng::MaterialVariant *v = ov.variant(ref)) {
      eng::MaterialVariant nv = *v;
      nv.props.clear();
      ov.setVariant(nv);
    }
  } else {
    ov.setMapLayer(ref, MaterialProps());
  }
  if (ov == before)
    return;
  pushOverridesChange(QStringLiteral("Revert material overrides"), before);
}

void Engine3D::setProjectPack(const QString &pack) {
  if (!m_scene || m_scene->materialOverrides.projectPack == pack)
    return;
  const eng::MaterialOverrides before = m_scene->materialOverrides;
  m_scene->materialOverrides.projectPack = pack;
  pushOverridesChange(QStringLiteral("Project texture pack"), before);
}

QString Engine3D::projectPack() const { return m_scene ? m_scene->materialOverrides.projectPack : QString(); }

void Engine3D::assignMaterialToFaces(const std::vector<std::pair<quint32, int>> &faces, const eng::MaterialPtr &m,
                                     const QString &label) {
  if (faces.empty() || !m)
    return;
  std::set<quint32> owners;
  for (const auto &f: faces)
    owners.insert(f.first);
  std::vector<ObjectState> before, after;
  for (const quint32 id: owners)
    before.push_back(captureObjectState(id, true));
  for (const auto &[sid, f]: faces)
    if (const auto o = m_scene->findById(sid); o && o->getMesh())
      o->getMesh()->setFaceMaterial(f, m);
  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  if (m_undoStack && !m_undoStack->isApplying())
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, std::move(before), std::move(after)));
  emit objectStateChanged();
  emit sceneChanged();
}

std::vector<std::pair<quint32, int>> Engine3D::facesWithMaterial(const QString &ref, bool selectionOnly) const {
  std::vector<std::pair<quint32, int>> out;
  if (!m_scene)
    return out;
  const QString key = ref.trimmed().toLower();
  if (selectionOnly) {
    for (const auto &[sid, f]: selectedFaceRefs())
      if (const auto o = m_scene->findById(sid); o && o->getMesh())
        if (const auto m = o->getMesh()->faceMaterialRef(f); m && m->name == key)
          out.emplace_back(sid, f);
    return out;
  }
  for (const auto &o: m_scene->gameObjects) {
    const auto mesh = o->getMesh();
    if (!mesh)
      continue;
    for (int f = 0; f < mesh->faceCount(); ++f)
      if (const auto m = mesh->faceMaterialRef(f); m && m->name == key)
        out.emplace_back(o->stableId(), f);
  }
  return out;
}

QString Engine3D::createVariantForSelection(const QString &ref, const QString &name) {
  if (!m_scene)
    return {};
  const QString base = baseOf(ref);
  eng::MaterialOverrides &ov = m_scene->materialOverrides;
  const QString vname = ov.uniqueVariantName(base, name);
  const QString variantRef = base + QLatin1Char('#') + vname;
  // The faces of the selection that use the base (or any of its variants).
  std::vector<std::pair<quint32, int>> faces;
  const QString baseKey = refKey(base);
  for (const auto &[sid, f]: selectedFaceRefs())
    if (const auto o = m_scene->findById(sid); o && o->getMesh())
      if (const auto m = o->getMesh()->faceMaterialRef(f); m && refKey(baseOf(m->ref.isEmpty() ? m->name : m->ref)) == baseKey)
        faces.emplace_back(sid, f);

  m_undoStack->beginMacro(QStringLiteral("Make variant %1").arg(vname));
  const eng::MaterialOverrides before = ov;
  eng::MaterialVariant v;
  v.ref = variantRef;
  v.baseRef = base;
  v.displayName = vname;
  // Start from what the faces show now: a variant of a variant keeps its look.
  if (const eng::MaterialVariant *src = ov.variant(ref))
    v.props = src->props;
  ov.setVariant(v);
  pushOverridesChange(QStringLiteral("Make variant"), before);
  assignMaterialToFaces(faces, TextureManager::instance().materialFor(variantRef), QStringLiteral("Use variant"));
  m_undoStack->endMacro();
  qInfo().noquote() << QStringLiteral("[material] variant %1 on %2 face(s)").arg(variantRef).arg(faces.size());
  return variantRef;
}

void Engine3D::deleteVariant(const QString &variantRef) {
  if (!m_scene || !MaterialRef::parse(variantRef).isVariant())
    return;
  const auto faces = facesWithMaterial(variantRef, false);
  m_undoStack->beginMacro(QStringLiteral("Delete variant"));
  assignMaterialToFaces(faces, TextureManager::instance().materialFor(baseOf(variantRef)),
                        QStringLiteral("Back to base material"));
  const eng::MaterialOverrides before = m_scene->materialOverrides;
  m_scene->materialOverrides.removeVariant(variantRef);
  pushOverridesChange(QStringLiteral("Delete variant"), before);
  m_undoStack->endMacro();
}

void Engine3D::mergeVariantIntoMap(const QString &variantRef) {
  if (!m_scene)
    return;
  const eng::MaterialVariant *v = m_scene->materialOverrides.variant(variantRef);
  if (!v)
    return;
  const QString base = baseOf(variantRef);
  m_undoStack->beginMacro(QStringLiteral("Merge variant into map"));
  const eng::MaterialOverrides before = m_scene->materialOverrides;
  MaterialProps layer = m_scene->materialOverrides.mapLayer(base) ? *m_scene->materialOverrides.mapLayer(base)
                                                                   : MaterialProps();
  layer.overlay(v->props);
  m_scene->materialOverrides.setMapLayer(base, layer);
  pushOverridesChange(QStringLiteral("Merge variant"), before);
  m_undoStack->endMacro();
  deleteVariant(variantRef);  // its own undo step, right after
}

void Engine3D::replaceMaterial(const QString &fromRef, const QString &toRef, bool selectionOnly) {
  if (!m_scene || fromRef.isEmpty() || toRef.isEmpty())
    return;
  const auto faces = facesWithMaterial(fromRef, selectionOnly);
  m_undoStack->beginMacro(QStringLiteral("Replace material"));
  // The look carries over: the old texture's map overrides go to the new one.
  if (const MaterialProps *layer = m_scene->materialOverrides.mapLayer(fromRef);
      layer && !m_scene->materialOverrides.mapLayer(toRef)) {
    const eng::MaterialOverrides before = m_scene->materialOverrides;
    m_scene->materialOverrides.setMapLayer(toRef, *layer);
    pushOverridesChange(QStringLiteral("Copy map overrides"), before);
  }
  assignMaterialToFaces(faces, TextureManager::instance().materialFor(toRef), QStringLiteral("Replace material"));
  m_undoStack->endMacro();
}

void Engine3D::saveMaterialPropsToServer(const QString &ref, bool force,
                                         std::function<void(bool, const QString &, const QJsonObject &)> done) {
  const QString base = baseOf(ref);
  const auto material = TextureManager::instance().materialFor(base);
  if (!material || !material->hasEntry()) {
    if (done)
      done(false, tr("This texture is not on the server"), {});
    return;
  }
  MaterialProps merged = material->entry.props;
  if (m_scene)
    if (const MaterialProps *layer = m_scene->materialOverrides.mapLayer(base))
      merged.overlay(*layer);
  QJsonObject args;
  args.insert(QStringLiteral("ref"), material->entry.ref);
  args.insert(QStringLiteral("props"), merged.toJson());
  if (!force)
    args.insert(QStringLiteral("if_rev"), double(material->entry.rev));
  QPointer<Engine3D> self(this);
  const quint64 level = m_levelSerial;
  net::TextureCatalog::instance().call(
      QStringLiteral("props.set"), args, this, [self, base, done, level](const net::Result &r) {
        if (!self)
          return;
        if (!r.ok) {
          if (done)
            done(false, r.code == QLatin1String("conflict") ? QStringLiteral("conflict") : r.message, r.current);
          return;
        }
        // The server has them now (new props arrive through texture.props): drop the map layer,
        // only in the level that asked (another may be on screen by now).
        if (self->m_levelSerial == level && self->m_scene && self->m_scene->materialOverrides.mapLayer(base)) {
          const eng::MaterialOverrides before = self->m_scene->materialOverrides;
          self->m_scene->materialOverrides.setMapLayer(base, MaterialProps());
          self->pushOverridesChange(QStringLiteral("Save material to server"), before);
        }
        if (done)
          done(true, {}, {});
      });
}

} // namespace vk
