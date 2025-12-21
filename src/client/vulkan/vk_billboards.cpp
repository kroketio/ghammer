// Entity icon billboards: camera-facing, constant screen size (Godot's gizmo icons), depth tested,
// picked in screen space with the square they are drawn with.

#include "vk_engine.h"
#include "vk_shared.h"

#include <QImageReader>
#include <QPainter>
#include <rhi/qrhi.h>

#include "client/engine/entity.h"

namespace vk {

namespace {
// Atlas slots, 128² each, left to right: the kind defaults, then each distinct EntityClass::billboard in registry order.
constexpr int kIconPx = 128;
const char *const kKindIconFiles[kEntityKindIconCount] = {
    ":/icons_godot/GizmoLight.svg",       // point light
    ":/icons_godot/GizmoSpotLight.svg",   // spot light
    ":/icons_godot/GizmoDirectionalLight.svg", // sun
    ":/icons_godot/WorldEnvironment.svg", // world environment
    ":/icons_godot/CharacterBody3D.svg",  // player start
    ":/icons_godot/MeshInstance3D.svg",   // prop
    ":/icons_godot/Marker3D.svg",         // anything else
};

// Built once, on first use; classes registered later fall back to the kind icon.
const QStringList &iconFiles() {
  static const QStringList files = [] {
    QStringList f;
    for (const char *k: kKindIconFiles)
      f << QString::fromLatin1(k);
    for (const auto &c: eng::EntityRegistry::instance().classes())
      if (!c.billboard.isEmpty() && !f.contains(c.billboard))
        f << c.billboard;
    return f;
  }();
  return files;
}
} // namespace

int Engine3D::entityIconCount() { return int(iconFiles().size()); }

int Engine3D::entityIconFor(int classKind) {
  using K = eng::EntityClass::Kind;
  switch (K(classKind)) {
    case K::PointLight: return 0;
    case K::SpotLight: return 1;
    case K::Sun: return 2;
    case K::WorldEnv: return 3;
    case K::PlayerStart: return 4;
    case K::Prop: return 5;
    default: return 6;
  }
}

int Engine3D::entityIconFor(const eng::EntityClass *cls) {
  if (!cls)
    return entityIconFor(int(eng::EntityClass::Kind::Generic));
  if (!cls->billboard.isEmpty())
    if (const int i = int(iconFiles().indexOf(cls->billboard)); i >= 0)
      return i;
  return entityIconFor(int(cls->kind));
}

void Engine3D::releaseIconAtlas() {
  if (m_iconAtlas)
    Shared::instance().retireTexture(m_iconAtlas);
  m_iconAtlas = nullptr;
  if (m_billboardSrb)
    Shared::instance().retireShaderResourceBindings(m_billboardSrb);
  m_billboardSrb = nullptr;
  m_billboardPipeline.reset();
}

void Engine3D::ensureIconAtlas(QRhiResourceUpdateBatch *rub) {
  if (m_iconAtlas || !rub)
    return;
  // A transparent margin around each icon keeps mip levels from bleeding into the neighbour.
  const QStringList &files = iconFiles();
  const int count = int(files.size());
  QImage atlas(kIconPx * count, kIconPx, QImage::Format_RGBA8888_Premultiplied);
  atlas.fill(Qt::transparent);
  {
    QPainter p(&atlas);
    const int inner = kIconPx - 12;
    for (int i = 0; i < count; ++i) {
      QImageReader reader(files[i]);
      reader.setScaledSize(QSize(inner, inner));
      const QImage icon = reader.read();
      if (icon.isNull()) {
        qWarning("billboards: cannot load %s", qPrintable(files[i]));
        continue;
      }
      p.drawImage(i * kIconPx + 6, 6, icon);
    }
  }
  atlas = atlas.convertToFormat(QImage::Format_RGBA8888);

  QRhi *rhi = Shared::instance().rhi();
  m_iconAtlas = rhi->newTexture(QRhiTexture::RGBA8, atlas.size(), 1,
                                QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips);
  if (!m_iconAtlas->create()) {
    qWarning("billboards: icon atlas texture failed");
    delete m_iconAtlas;
    m_iconAtlas = nullptr;
    return;
  }
  rub->uploadTexture(m_iconAtlas, atlas);
  rub->generateMips(m_iconAtlas);
  // The billboard SRB binds the atlas, so the pipelines are rebuilt next.
  ++m_bufferGen;
}

void Engine3D::buildBillboardPipeline(QRhiRenderPassDescriptor *rp, int sampleCount) {
  auto &shared = Shared::instance();
  QRhi *rhi = shared.rhi();
  if (m_billboardSrb)
    shared.retireShaderResourceBindings(m_billboardSrb);
  m_billboardSrb = nullptr;
  m_billboardPipeline.reset();
  if (!m_iconAtlas || !m_sceneUbo || !m_entityUbo)
    return;

  m_billboardSrb = rhi->newShaderResourceBindings();
  m_billboardSrb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_sceneUbo),
      QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
          14, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_entityUbo,
          sizeof(EntityUniforms)),
      QRhiShaderResourceBinding::sampledTexture(16, QRhiShaderResourceBinding::FragmentStage, m_iconAtlas,
                                                shared.linearSampler()),
  });
  if (!m_billboardSrb->create()) {
    delete m_billboardSrb;
    m_billboardSrb = nullptr;
    return;
  }

  std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
  p->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/billboard.vert.qsb"))},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/billboard.frag.qsb"))},
  });
  QRhiVertexInputLayout layout;
  layout.setBindings({{2 * sizeof(float)}});
  layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
  p->setVertexInputLayout(layout);
  p->setCullMode(QRhiGraphicsPipeline::None);
  // Depth tested but no depth write: icons do not hide each other or the gizmo.
  p->setDepthTest(true);
  p->setDepthWrite(false);
  p->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  p->setTargetBlends({blend});
  p->setSampleCount(sampleCount);
  p->setRenderPassDescriptor(rp);
  p->setShaderResourceBindings(m_billboardSrb);
  if (!p->create()) {
    qWarning("billboards: pipeline failed");
    return;
  }
  m_billboardPipeline = std::move(p);
}

float Engine3D::billboardHalfPx(bool selected) const {
  return std::max(4.0f, m_entityIconPx * 0.5f) * (selected ? 1.2f : 1.0f);
}

void Engine3D::appendBillboardDraws(const QVector<EntityMarker> &markers) {
  const float w = std::max(1.0f, float(m_lastViewSize.width()));
  const float h = std::max(1.0f, float(m_lastViewSize.height()));
  QRhi *rhi = Shared::instance().rhi();
  const float ySign = rhi && rhi->isYUpInNDC() ? -1.0f : 1.0f;
  for (const EntityMarker &m: markers) {
    if (m.icon < 0 || m_entityDraws.size() >= size_t(m_entityCapacity))
      continue;
    if (m_previewCamera && m.id == m_previewCamera)
      continue; // looking through it
    const float half = billboardHalfPx(m.selected);
    EntityDraw d;
    d.pass = EntityPass::Billboard;
    d.model = glm::translate(glm::mat4(1.0f), m.position);
    glm::vec4 tint = m.iconTint;
    if (m.selected)
      tint = glm::vec4(glm::mix(glm::vec3(tint), glm::vec3(1.0f, 0.55f, 0.15f), 0.55f), 1.0f);
    d.color = tint;
    d.info = glm::vec4(2.0f * half / w, ySign * 2.0f * half / h, float(m.icon), float(entityIconCount()));
    m_entityDraws.push_back(d);
  }
}

void Engine3D::drawBillboards(QRhiCommandBuffer *cb) {
  if (!m_billboardPipeline || !m_billboardSrb || !m_quadVb || !m_quadIb)
    return;
  bool bound = false;
  for (size_t k = 0; k < m_entityDraws.size(); ++k) {
    if (m_entityDraws[k].pass != EntityPass::Billboard)
      continue;
    if (!bound) {
      cb->setGraphicsPipeline(m_billboardPipeline.get());
      const QRhiCommandBuffer::VertexInput vin(m_quadVb, 0);
      cb->setVertexInput(0, 1, &vin, m_quadIb, 0, QRhiCommandBuffer::IndexUInt32);
      bound = true;
    }
    const QRhiCommandBuffer::DynamicOffset offset(14, quint32(k) * m_entityStride);
    cb->setShaderResources(m_billboardSrb, 1, &offset);
    cb->drawIndexed(6);
    ++m_perf.entityDraws;
  }
}

bool Engine3D::pickBillboard(float x, float y, float maxDist, quint32 &id) const {
  const glm::vec3 eye = m_camera->getPosition();
  const glm::vec3 fwd = m_camera->getDirection();
  float best = maxDist;
  bool hit = false;
  for (const EntityMarker &m: entityMarkers()) {
    if (m.icon < 0)
      continue;
    const glm::vec3 toM = m.position - eye;
    if (glm::dot(toM, fwd) <= 0.01f)
      continue;
    const glm::vec2 s = projectToScreen(m.position);
    const float half = billboardHalfPx(m.selected);
    if (std::fabs(x - s.x) > half || std::fabs(y - s.y) > half)
      continue;
    // Nearest icon wins; one behind the geometry under the cursor cannot be clicked.
    const float dist = glm::length(toM);
    if (dist < best) {
      best = dist;
      id = m.id;
      hit = true;
    }
  }
  return hit;
}

} // namespace vk
