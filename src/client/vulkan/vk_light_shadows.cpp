// Entity light shadows: one R32F atlas of square tiles storing distance-to-light / range (shadow_point.*).
// Omni = six 90° cube-face tiles, spot = one over its cone. Re-rendered only when lights or geometry change.

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

#include "vk_engine.h"
#include "vk_frustum.h"
#include "vk_shared.h"

#include "client/engine/entity.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

// Face f of a point light: forward and up vectors. Mirrored in scene.frag
// (lightFaceBasis) - the two tables must stay identical.
void faceAxes(const int f, glm::vec3 &fwd, glm::vec3 &up) {
  static const glm::vec3 F[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static const glm::vec3 U[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
  fwd = F[f];
  up = U[f];
}

glm::vec3 spotUp(const glm::vec3 &dir) {
  return std::fabs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
}

} // namespace

void Engine3D::ensureLightAtlas() {
  const int want = std::clamp(prefs::i("Render/LocalShadowAtlas"), 1024, 8192);
  if (m_lightAtlasRt && m_lightAtlasSize == want)
    return;
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;
  auto &shared = Shared::instance();
  if (m_lightAtlasRt) {
    shared.retireRenderTarget(m_lightAtlasRt);
    shared.retireRenderPass(m_lightAtlasRp);
    shared.retireRenderBuffer(m_lightAtlasDepth);
    shared.retireTexture(m_lightAtlasTex);
    m_lightAtlasRt = nullptr;
    m_lightAtlasRp = nullptr;
    m_lightAtlasDepth = nullptr;
    m_lightAtlasTex = nullptr;
  }
  const QSize size(want, want);
  auto *tex = rhi->newTexture(QRhiTexture::R32F, size, 1, QRhiTexture::RenderTarget);
  auto *depth = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1);
  if (!tex->create() || !depth->create()) {
    qWarning("light shadow atlas allocation failed (%d)", want);
    delete tex;
    delete depth;
    return;
  }
  QRhiTextureRenderTargetDescription desc;
  desc.setColorAttachments({QRhiColorAttachment(tex)});
  desc.setDepthStencilBuffer(depth);
  auto *rt = rhi->newTextureRenderTarget(desc);
  auto *rp = rt->newCompatibleRenderPassDescriptor();
  rt->setRenderPassDescriptor(rp);
  if (!rt->create()) {
    delete rt;
    delete rp;
    delete depth;
    delete tex;
    return;
  }
  m_lightAtlasTex = tex;
  m_lightAtlasDepth = depth;
  m_lightAtlasRt = rt;
  m_lightAtlasRp = rp;
  m_lightAtlasSize = want;
  m_lightAtlasKey = ~0ull;
  // Every scene SRB samples the atlas: rebuild them.
  ++m_bufferGen;
}

void Engine3D::prepareLightShadows() {
  m_lightJobs.clear();
  m_lightShadowTile.clear();
  if (!m_scene || noLocalLights() || !m_shadowsEnabled)
    return;
  const int maxLights = prefs::i("Render/MaxShadowedLights");
  const int tiles = kLightAtlasTilesPerRow * kLightAtlasTilesPerRow;
  const glm::vec3 cam = m_camera->getPosition();

  struct Cand {
    const eng::Entity *e;
    float d;
  };
  std::vector<Cand> cands;
  for (const eng::Entity &e: m_scene->entities) {
    const eng::EntityClass *c = e.cls();
    if (!c || e.hidden || !e.value(QStringLiteral("enabled")).toBool() || !e.value(QStringLiteral("castShadows")).toBool())
      continue;
    if (c->kind != eng::EntityClass::Kind::PointLight && c->kind != eng::EntityClass::Kind::SpotLight)
      continue;
    const float range = eng::lightRangeWorld(e);
    if (range <= 0.0f)
      continue;
    const float d = std::max(0.0f, glm::length(e.origin - cam) - range);
    if (d > lightRenderDistanceWorld())
      continue;
    cands.push_back({&e, d});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.d < b.d; });

  QRhi *rhi = Shared::instance().rhi();
  const glm::mat4 corr = rhi ? qtToGlm(rhi->clipSpaceCorrMatrix()) : glm::mat4(1.0f);
  int next = 0;
  for (const Cand &cand: cands) {
    if (int(m_lightJobs.size()) >= maxLights)
      break;
    const eng::Entity &e = *cand.e;
    const bool spot = e.cls()->kind == eng::EntityClass::Kind::SpotLight;
    const int need = spot ? 1 : 6;
    if (next + need > tiles)
      break;
    LightShadowJob job;
    job.entity = e.id;
    job.firstTile = next;
    job.tileCount = need;
    job.position = e.origin;
    job.range = eng::lightRangeWorld(e);
    const float nearPlane = std::max(0.02f, job.range * 0.001f);
    if (spot) {
      const glm::vec3 dir = e.forward();
      // The shadow frustum covers the cone; past ~85° a perspective breaks down.
      const float outer = float(std::clamp(e.num(QStringLiteral("spot_angle"), 45.0), 1.0, 85.0));
      job.tanHalf = std::tan(glm::radians(outer));
      const glm::mat4 view = glm::lookAt(e.origin, e.origin + dir, spotUp(dir));
      const glm::mat4 proj = glm::perspective(glm::radians(outer * 2.0f), 1.0f, nearPlane, job.range);
      job.faces[0] = {corr * proj * view, view, glm::vec4(1.0f / job.range, nearPlane, 0, 0)};
    } else {
      job.tanHalf = 1.0f;
      const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, job.range);
      for (int f = 0; f < 6; ++f) {
        glm::vec3 fwd, up;
        faceAxes(f, fwd, up);
        const glm::mat4 view = glm::lookAt(e.origin, e.origin + fwd, up);
        job.faces[f] = {corr * proj * view, view, glm::vec4(1.0f / job.range, nearPlane, 0, 0)};
      }
    }
    m_lightShadowTile[e.id] = next;
    m_lightJobs.push_back(job);
    next += need;
  }
}

void Engine3D::renderLightShadowAtlas(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *rub, bool &rubSubmitted) {
  if (m_lightJobs.empty() || !m_lightAtlasRt || !m_lightAtlasPipeline || !m_lightAtlasSrb || !m_lightFaceUbo)
    return;
  // Signature: geometry + every job's placement.
  quint64 key = m_sigGeometry * 1099511628211ull;
  for (const auto &j: m_lightJobs) {
    key = (key ^ j.entity) * 1099511628211ull;
    key = (key ^ quint64(j.firstTile)) * 1099511628211ull;
    for (int f = 0; f < j.tileCount; ++f)
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
          const float v = j.faces[f].clip[c][r];
          quint32 u;
          std::memcpy(&u, &v, 4);
          key = (key ^ u) * 1099511628211ull;
        }
  }
  if (key == m_lightAtlasKey)
    return;
  m_lightAtlasKey = key;

  // This frame's batch, or a fresh one when it was already submitted.
  QRhiResourceUpdateBatch *batch = rubSubmitted ? Shared::instance().rhi()->nextResourceUpdateBatch() : rub;
  for (const auto &j: m_lightJobs)
    for (int f = 0; f < j.tileCount; ++f)
      batch->updateDynamicBuffer(m_lightFaceUbo, (j.firstTile + f) * m_lightFaceStride, sizeof(LightShadowFace), &j.faces[f]);

  cb->beginPass(m_lightAtlasRt, QColor::fromRgbF(1, 1, 1, 1), {1.0f, 0}, batch);
  rubSubmitted = true;
  cb->setGraphicsPipeline(m_lightAtlasPipeline.get());
  if (m_gpu.vertexBuffer() && m_gpu.indexBuffer()) {
    const QRhiCommandBuffer::VertexInput vin(m_gpu.vertexBuffer(), 0);
    cb->setVertexInput(0, 1, &vin, m_gpu.indexBuffer(), 0, QRhiCommandBuffer::IndexUInt32);
  }
  const float tile = float(m_lightAtlasSize) / float(kLightAtlasTilesPerRow);
  const auto &objects = m_scene->gameObjects;
  for (const auto &j: m_lightJobs) {
    const float rangeSq = j.range * j.range;
    for (int f = 0; f < j.tileCount; ++f) {
      const int t = j.firstTile + f;
      cb->setViewport(QRhiViewport(float(t % kLightAtlasTilesPerRow) * tile, float(t / kLightAtlasTilesPerRow) * tile,
                                   tile, tile));
      Frustum fr;
      fr.extract(j.faces[f].clip);
      for (qsizetype i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        if (obj->hidden() || !obj->getMesh() || isToolBrush(*obj))
          continue;
        const MeshGpuRecord *rec = m_gpu.record(obj->stableId());
        if (!rec || rec->indexCount == 0)
          continue;
        const auto [lo, hi] = obj->getWorldAABB();
        const glm::vec3 closest = glm::clamp(j.position, lo, hi);
        if (glm::dot(closest - j.position, closest - j.position) > rangeSq || !fr.isBoxVisible(lo, hi))
          continue;
        const QRhiCommandBuffer::DynamicOffset offs[2] = {{1, objectSlotOf(obj->stableId()) * m_objectStride},
                                                          {15, quint32(t * m_lightFaceStride)}};
        cb->setShaderResources(m_lightAtlasSrb, 2, offs);
        cb->drawIndexed(rec->indexCount, 1, rec->indexOffset, qint32(rec->vertexOffset));
      }
    }
  }
  cb->endPass();
}

void Engine3D::fillLightShadowUniforms() {
  for (auto &v: m_sceneUniforms.lightShadow)
    v = glm::vec4(-1.0f, 1.0f, 0.0f, 0.0f);
  QRhi *rhi = Shared::instance().rhi();
  const float corrY = rhi && rhi->isYUpInNDC() ? 1.0f : -1.0f;
  m_sceneUniforms.lightAtlasInfo = glm::vec4(float(m_lightAtlasSize), corrY, float(kLightAtlasTilesPerRow),
                                             m_lightJobs.empty() ? 0.0f : 1.0f);
}

void Engine3D::assignLightShadowSlot(const quint32 entityId, const int lightIndex) {
  if (lightIndex < 0 || lightIndex >= kMaxSceneLights)
    return;
  const auto it = m_lightShadowTile.find(entityId);
  if (it == m_lightShadowTile.end())
    return;
  float tanHalf = 1.0f;
  for (const auto &j: m_lightJobs)
    if (j.entity == entityId)
      tanHalf = j.tanHalf;
  // Godot's shadow_bias: omni in metres, spot in hundredths of its range; shadow_normal_bias is a factor.
  float depthBias = float(0.1 * eng::kWorldUnitsPerMeter), normalBias = 1.0f;
  if (const eng::Entity *e = entityById(entityId)) {
    const bool spot = e->cls() && e->cls()->kind == eng::EntityClass::Kind::SpotLight;
    depthBias = spot ? float(e->num(QStringLiteral("shadow_bias"), 0.03) / 100.0) * eng::lightRangeWorld(*e)
                     : float(e->num(QStringLiteral("shadow_bias"), 0.1) * eng::kWorldUnitsPerMeter);
    normalBias = float(e->num(QStringLiteral("shadow_normal_bias"), 1.0));
  }
  m_sceneUniforms.lightShadow[lightIndex] = glm::vec4(float(it->second), tanHalf, depthBias, normalBias);
}

} // namespace vk
