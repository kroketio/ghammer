// Tool brushes (func_clip / trigger_*): left out of the lit scene, shadows and SSAO; drawn as a depth-tested
// see-through overlay.

#include <QDebug>
#include <QLoggingCategory>

#include "vk_engine.h"
#include "vk_shared.h"

#include "client/engine/entity.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {
const QString &kCollisionClass = eng::kClipClass;
QLoggingCategory collLog("vk.collision", QtWarningMsg);
} // namespace

void Engine3D::refreshToolEntitySets() const {
  const quint64 key = m_entityEpoch * 1000003ull ^ quint64(m_scene->entities.size()) * 7919ull
                      ^ quint64(quintptr(m_scene.data()));
  if (key == m_collisionEntitiesKey)
    return;
  m_collisionEntitiesKey = key;
  m_collisionEntities.clear();
  m_triggerEntities.clear();
  for (const auto &e: m_scene->entities) {
    if (e.classname == kCollisionClass)
      m_collisionEntities.insert(e.id);
    else if (e.classname.startsWith(QLatin1String("trigger_")))
      m_triggerEntities.insert(e.id);
  }
}

bool Engine3D::isCollisionBrush(const eng::GameObject &o) const {
  if (!o.brushEntity() || !m_scene)
    return false;
  refreshToolEntitySets();
  return m_collisionEntities.count(o.brushEntity()) != 0;
}

bool Engine3D::isTriggerBrush(const eng::GameObject &o) const {
  if (!o.brushEntity() || !m_scene)
    return false;
  refreshToolEntitySets();
  return m_triggerEntities.count(o.brushEntity()) != 0;
}

bool Engine3D::toolBrushHidden(const eng::GameObject &o) const {
  return (!m_showCollision && isCollisionBrush(o)) || (!m_showTriggers && isTriggerBrush(o));
}

void Engine3D::setShowTriggers(const bool on) {
  if (m_showTriggers == on)
    return;
  m_showTriggers = on;
  prefs::get().setValue(QStringLiteral("View/ShowTriggers"), on);
  ++m_viewEpoch;
  m_collisionBuiltKey = ~0ull;
  emit sceneChanged();
  requestRedraw();
}

eng::MaterialPtr Engine3D::triggerMaterial() const {
  auto m = TextureManager::instance().materialFor(QStringLiteral("tools/trigger"));
  if (m && m->albedoTint == glm::vec3(1.0f)) {
    m->albedoTint = glm::vec3(1.0f, 0.55f, 0.2f);
    m->touch();
  }
  return m;
}

void Engine3D::setSpecularEnabled(const bool on) {
  // The preference is the one switch; its watcher redraws.
  prefs::get().setValue(QStringLiteral("Render/Specular"), on);
}

void Engine3D::setNormalMapsEnabled(const bool on) {
  prefs::get().setValue(QStringLiteral("Render/NormalMaps"), on);
}

void Engine3D::setShowCollision(const bool on) {
  if (m_showCollision == on)
    return;
  m_showCollision = on;
  prefs::get().setValue(QStringLiteral("View/ShowCollision"), on);
  ++m_viewEpoch;
  m_collisionBuiltKey = ~0ull;
  emit sceneChanged();
  requestRedraw();
}

eng::MaterialPtr Engine3D::collisionMaterial() const {
  auto m = TextureManager::instance().materialFor(QStringLiteral("tools/collision"));
  if (m && m->albedoTint == glm::vec3(1.0f)) {
    m->albedoTint = glm::vec3(0.22f, 0.85f, 0.72f);
    m->touch();
  }
  return m;
}

quint32 Engine3D::addCollisionEntityRaw(const glm::vec3 &origin) {
  if (!m_scene)
    return 0;
  eng::Entity e;
  e.id = m_scene->mintEntityId();
  e.classname = kCollisionClass;
  e.origin = origin;
  m_scene->entities.push_back(e);
  ++m_entityEpoch;
  emit entitiesChanged();
  return e.id;
}

bool Engine3D::objectsBounds(const std::vector<quint32> &ids, glm::dvec3 &mn, glm::dvec3 &mx) const {
  if (!m_scene)
    return false;
  for (const bool skipRamps: {true, false}) {
    bool any = false;
    for (const quint32 id: ids) {
      const auto o = m_scene->findById(id);
      if (!o || (skipRamps && isProcRamp(*o)))
        continue;
      const auto [lo, hi] = o->getWorldAABB();
      mn = any ? glm::min(mn, glm::dvec3(lo)) : glm::dvec3(lo);
      mx = any ? glm::max(mx, glm::dvec3(hi)) : glm::dvec3(hi);
      any = true;
    }
    if (any)
      return true;
  }
  return false;
}

void Engine3D::refreshCollisionSig() {
  quint64 h = m_showCollision ? 0x9e3779b97f4a7c15ull : 0x51ed270b2f3a1c3dull;
  h ^= m_showTriggers ? 0x2545f4914f6cdd1dull : 0;
  if (m_scene)
    for (const auto &o: m_scene->gameObjects)
      if (isToolBrush(*o))
        h = (h ^ (quint64(o->stableId()) + (o->hidden() ? 0x8000000000ull : 0))) * 1099511628211ull;
  m_collisionSig = h;
}

void Engine3D::releaseCollisionPipelines() {
  delete m_collisionFillPipeline;
  delete m_collisionEdgePipeline;
  m_collisionFillPipeline = nullptr;
  m_collisionEdgePipeline = nullptr;
}

void Engine3D::setupCollisionPipelines(QRhiRenderPassDescriptor *rp, const int sampleCount) {
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi || !m_cutWireSrb)
    return;
  if (!m_collisionVb) {
    m_collisionVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 1 << 16);
    if (!m_collisionVb->create())
      qWarning(collLog, "collision vertex buffer failed");
  }
  auto make = [&](QRhiGraphicsPipeline::Topology topology) -> QRhiGraphicsPipeline * {
    auto *p = rhi->newGraphicsPipeline();
    p->setShaderStages({
        {QRhiShaderStage::Vertex, Shared::instance().shader(QStringLiteral(":/shaders/collision.vert.qsb"))},
        {QRhiShaderStage::Fragment, Shared::instance().shader(QStringLiteral(":/shaders/collision.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{7 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                          {0, 1, QRhiVertexInputAttribute::Float4, 3 * sizeof(float)}});
    p->setVertexInputLayout(layout);
    p->setTopology(topology);
    p->setCullMode(QRhiGraphicsPipeline::None);
    p->setDepthTest(true);
    p->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    p->setDepthWrite(false);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    p->setTargetBlends({blend});
    p->setSampleCount(sampleCount);
    p->setRenderPassDescriptor(rp);
    p->setShaderResourceBindings(m_cutWireSrb);
    if (!p->create()) {
      qWarning(collLog, "collision pipeline failed");
      delete p;
      return nullptr;
    }
    return p;
  };
  if (!m_collisionFillPipeline)
    m_collisionFillPipeline = make(QRhiGraphicsPipeline::Triangles);
  if (!m_collisionEdgePipeline)
    m_collisionEdgePipeline = make(QRhiGraphicsPipeline::Lines);
}

void Engine3D::buildCollisionOverlay() {
  if (!m_collisionVb)
    return;
  const quint64 key = m_sigGeometry ^ (m_collisionSig * 31);
  if (key == m_collisionBuiltKey)
    return;
  m_collisionBuiltKey = key;
  m_collisionFillCount = m_collisionEdgeCount = 0;
  if (!m_scene || (!m_showCollision && !m_showTriggers))
    return;

  std::vector<float> fill, edges;
  glm::vec3 tint(0.22f, 0.85f, 0.72f);
  auto put = [&tint](std::vector<float> &v, const glm::dvec3 &p, float shade, float alpha) {
    const glm::vec3 c = tint * shade;
    v.insert(v.end(), {float(p.x), float(p.y), float(p.z), c.r, c.g, c.b, alpha});
  };
  for (const auto &o: m_scene->gameObjects) {
    if (o->hidden() || !isToolBrush(*o) || toolBrushHidden(*o))
      continue;
    tint = isTriggerBrush(*o) ? glm::vec3(1.0f, 0.55f, 0.2f) : glm::vec3(0.22f, 0.85f, 0.72f);
    const auto b = eng::ConvexBrush::fromObject(*o);
    if (!b)
      continue;
    for (size_t i = 0; i < b->polygons.size() && i < b->faces.size(); ++i) {
      const auto &poly = b->polygons[i];
      if (poly.size() < 3)
        continue;
      const glm::dvec3 n = b->faces[i].normal;
      // Faces read apart by direction: tops brightest, undersides darkest.
      const float shade = float(0.75 + 0.25 * n.y + 0.08 * n.x);
      for (size_t j = 1; j + 1 < poly.size(); ++j) {
        put(fill, poly[0], shade, 0.26f);
        put(fill, poly[j], shade, 0.26f);
        put(fill, poly[j + 1], shade, 0.26f);
      }
      for (size_t j = 0; j < poly.size(); ++j) {
        put(edges, poly[j], 1.25f, 0.9f);
        put(edges, poly[(j + 1) % poly.size()], 1.25f, 0.9f);
      }
    }
  }
  std::vector<float> all = std::move(fill);
  const quint32 fillCount = quint32(all.size() / 7);
  all.insert(all.end(), edges.begin(), edges.end());
  const quint32 edgeCount = quint32(all.size() / 7) - fillCount;
  if (all.empty())
    return;
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;
  const quint32 bytes = quint32(all.size() * sizeof(float));
  if (bytes > m_collisionVb->size()) {
    quint32 size = m_collisionVb->size();
    Shared::instance().retire(m_collisionVb);
    while (size < bytes)
      size *= 2;
    m_collisionVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, size);
    if (!m_collisionVb->create()) {
      qWarning(collLog, "collision vertex buffer grow failed");
      return;
    }
  }
  if (!m_collisionBatch)
    m_collisionBatch = rhi->nextResourceUpdateBatch();
  m_collisionBatch->updateDynamicBuffer(m_collisionVb, 0, bytes, all.data());
  m_collisionFillCount = fillCount;
  m_collisionEdgeCount = edgeCount;
}

void Engine3D::drawCollisionOverlay(QRhiCommandBuffer *cb) {
  if ((!m_showCollision && !m_showTriggers) || !m_cutWireSrb || !m_collisionVb)
    return;
  const QRhiCommandBuffer::VertexInput vin(m_collisionVb, 0);
  if (m_collisionFillCount && m_collisionFillPipeline) {
    cb->setGraphicsPipeline(m_collisionFillPipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_collisionFillCount);
  }
  if (m_collisionEdgeCount && m_collisionEdgePipeline) {
    cb->setGraphicsPipeline(m_collisionEdgePipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_collisionEdgeCount, 1, m_collisionFillCount);
  }
}

} // namespace vk
