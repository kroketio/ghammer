#include "vk_text.h"

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>

#include <rhi/qrhi.h>

#include "vk_shared.h"

namespace vk {

namespace {
constexpr int kPaddingPx = 4;
const char *kLabelFont = "DejaVu Sans Mono";

// Monospace, whichever font the system has.
QFont labelFont(const int pixelSize) {
  QFont font {QLatin1String(kLabelFont)};
  font.setStyleHint(QFont::Monospace, QFont::PreferAntialias);
  font.setFixedPitch(true);
  font.setPixelSize(pixelSize);
  return font;
}
} // namespace

TextRenderer::TextRenderer(const QString &vertexShader) :
    m_vertexShader(vertexShader) {
}

TextRenderer::~TextRenderer() {
  invalidate();
}

void TextRenderer::clearCache() {
  for (auto &entry: m_cache) {
    delete entry.second.srb;
    delete entry.second.texture;
  }
  m_cache.clear();
}

void TextRenderer::invalidate() {
  clearCache();
  delete m_templateSrb;
  m_templateSrb = nullptr;
  m_pipeline.reset();
  delete m_vbo;
  m_vbo = nullptr;
  m_vboCapacity = 0;
  m_rhi = nullptr;
  m_builtRenderPass = nullptr;
  m_builtSampleCount = 0;
  m_frame.clear();
}

void TextRenderer::setPixelSize(const int pixels) {
  m_pixelSize = std::max(6, pixels);
}

QString TextRenderer::cacheKey(const int pixelSize, const QString &text) {
  return QString::number(pixelSize) + QChar(0x1f) + text;
}

QSize TextRenderer::measure(const int pixelSize, const QString &text) {
  const QFontMetricsF metrics(labelFont(pixelSize));
  return {static_cast<int>(std::ceil(metrics.horizontalAdvance(text))) + kPaddingPx * 2,
          static_cast<int>(std::ceil(metrics.height())) + kPaddingPx * 2};
}

QSize TextRenderer::size(const QString &text) {
  if (text.isEmpty())
    return {};

  const auto cached = m_cache.find(cacheKey(m_pixelSize, text));
  if (cached != m_cache.end() && cached->second.width > 0)
    return {cached->second.width, cached->second.height};
  return measure(m_pixelSize, text);
}

TextRenderer::Sprite *TextRenderer::spriteFor(QRhi *rhi, QRhiResourceUpdateBatch *rub, const QString &text,
                                            const int pixelSize, QRhiBuffer *sceneUbo) {
  if (text.isEmpty())
    return nullptr;

  const QString key = cacheKey(pixelSize, text);
  const auto cached = m_cache.find(key);
  if (cached != m_cache.end() && cached->second.texture) {
    cached->second.lastUsed = m_frameCounter;
    return &cached->second;
  }

  if (m_cache.size() >= MAX_CACHED_LABELS) {
    auto victim = m_cache.end();
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
      // Never evict a label this frame is still drawing: a frame can hold more
      // distinct labels than the cache allows.
      if (it->second.lastUsed >= m_frameCounter)
        continue;
      if (victim == m_cache.end() || it->second.lastUsed < victim->second.lastUsed)
        victim = it;
    }
    // Retire, don't delete: an in-flight command buffer may still bind it.
    if (victim != m_cache.end()) {
      Shared::instance().retireShaderResourceBindings(victim->second.srb);
      Shared::instance().retireTexture(victim->second.texture);
      m_cache.erase(victim);
    }
  }

  Sprite &slot = m_cache[key];
  slot.lastUsed = m_frameCounter;
  slot.srb = nullptr;
  slot.texture = nullptr;

  const QSize sz = measure(pixelSize, text);
  if (sz.isEmpty())
    return nullptr;
  slot.width = sz.width();
  slot.height = sz.height();

  QImage image(sz, QImage::Format_RGBA8888);
  image.fill(Qt::black);

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  painter.setFont(labelFont(pixelSize));
  painter.setPen(Qt::white);
  painter.drawText(QRectF(kPaddingPx, 0, sz.width() - kPaddingPx * 2, sz.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, text);
  painter.end();

  auto *tex = rhi->newTexture(QRhiTexture::RGBA8, sz, 1, {});
  if (!tex->create()) {
    delete tex;
    m_cache.erase(key);
    return nullptr;
  }
  rub->uploadTexture(tex, image);
  slot.texture = tex;

  slot.srb = rhi->newShaderResourceBindings();
  slot.srb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, sceneUbo),
      QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage,
                                              tex, Shared::instance().linearSampler()),
  });
  if (!slot.srb->create()) {
    delete slot.srb;
    slot.srb = nullptr;
    delete tex;
    slot.texture = nullptr;
    m_cache.erase(key);
    return nullptr;
  }

  return &slot;
}

void TextRenderer::drawDecal(const QString &text,
                            const glm::vec3 &origin,
                            const glm::vec3 &uAxis,
                            const glm::vec3 &vAxis,
                            const float worldWidth,
                            const float worldHeight,
                            const QColor &color,
                            const float alpha) {
  if (text.isEmpty() || worldWidth <= 0.0f || worldHeight <= 0.0f)
    return;

  const glm::vec3 du = uAxis * worldWidth;
  const glm::vec3 dv = vAxis * worldHeight;
  const glm::vec4 c(color.redF(), color.greenF(), color.blueF(), alpha);

  const glm::vec3 corners[4] = {origin, origin + du, origin + du + dv, origin + dv};
  const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

  Decal decal;
  decal.text = text;
  decal.pixelSize = m_pixelSize;
  const int order[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) {
    const int k = order[i];
    decal.vertices[i] = Vertex {corners[k], uvs[k], c};
  }
  m_frame.push_back(decal);
}

void TextRenderer::ensurePipeline(QRhi *rhi, QRhiRenderPassDescriptor *renderPass, int sampleCount,
                                    QRhiBuffer *sceneUbo) {
  if (m_pipeline && m_rhi == rhi && m_builtRenderPass == renderPass && m_builtSampleCount == sampleCount)
    return;

  m_pipeline.reset();
  auto &shared = Shared::instance();

  m_pipeline.reset(rhi->newGraphicsPipeline());
  m_pipeline->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(m_vertexShader)},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/text.frag.qsb"))},
  });

  QRhiVertexInputLayout inputLayout;
  inputLayout.setBindings({{sizeof(Vertex)}});
  inputLayout.setAttributes({
      {0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(Vertex, position))},
      {0, 1, QRhiVertexInputAttribute::Float2, static_cast<quint32>(offsetof(Vertex, uv))},
      {0, 2, QRhiVertexInputAttribute::Float4, static_cast<quint32>(offsetof(Vertex, color))},
  });
  m_pipeline->setVertexInputLayout(inputLayout);
  m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
  // Always-readable overlay: no depth test (no z-fighting, never hidden), no depth write (never occludes).
  m_pipeline->setDepthTest(false);
  m_pipeline->setDepthWrite(false);

  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  m_pipeline->setTargetBlends({blend});

  m_pipeline->setSampleCount(sampleCount);
  m_pipeline->setRenderPassDescriptor(renderPass);

  if (!m_templateSrb) {
    m_templateSrb = rhi->newShaderResourceBindings();
    m_templateSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, sceneUbo),
        QRhiShaderResourceBinding::sampledTexture(
            4, QRhiShaderResourceBinding::FragmentStage, Shared::instance().fallbackWhite(),
            Shared::instance().linearSampler()),
    });
    if (!m_templateSrb->create()) {
      delete m_templateSrb;
      m_templateSrb = nullptr;
    }
  }
  if (m_templateSrb)
    m_pipeline->setShaderResourceBindings(m_templateSrb);

  if (!m_pipeline->create()) {
    qWarning("TextRenderer: pipeline build failed");
    m_pipeline.reset();
    return;
  }

  m_rhi = rhi;
  m_builtRenderPass = renderPass;
  m_builtSampleCount = sampleCount;
}

void TextRenderer::prepare(QRhi *rhi, QRhiResourceUpdateBatch *rub, QRhiBuffer *sceneUbo,
                          QRhiRenderPassDescriptor *renderPass, int sampleCount) {
  m_ranges.clear();
  if (m_frame.isEmpty())
    return;

  ++m_frameCounter;

  ensurePipeline(rhi, renderPass, sampleCount, sceneUbo);
  if (!m_pipeline) {
    m_frame.clear();
    return;
  }

  // One contiguous vertex range per distinct label, so each texture is bound once. Group BEFORE filling the buffer:
  // growing a range in place only works when all instances are adjacent; alternating labels overrun other labels.
  QHash<QString, QVector<const Decal *>> groups;
  groups.reserve(m_frame.size() * 2);
  for (const auto &decal: m_frame)
    groups[cacheKey(decal.pixelSize, decal.text)].push_back(&decal);

  QVector<Vertex> vertices;
  vertices.reserve(m_frame.size() * 6);

  for (auto it = groups.begin(); it != groups.end(); ++it) {
    const Decal *first0 = it.value().front();
    Sprite *sprite = spriteFor(rhi, rub, first0->text, first0->pixelSize, sceneUbo);
    if (!sprite || !sprite->srb)
      continue;

    const quint32 first = static_cast<quint32>(vertices.size());
    quint32 count = 0;
    for (const Decal *decal: it.value()) {
      for (const auto &v: decal->vertices) {
        vertices.push_back(v);
        ++count;
      }
    }
    if (count)
      m_ranges.push_back({it.key(), first, count});
  }
  m_frame.clear();

  if (vertices.isEmpty())
    return;

  const quint32 needed = static_cast<quint32>(vertices.size() * sizeof(Vertex));
  if (needed > m_vboCapacity) {
    // Retire, don't delete: the previous frame may still be reading the old VBO.
    Shared::instance().retire(m_vbo);
    m_vbo = nullptr;
    m_vboCapacity = needed * 2;
    m_vbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, m_vboCapacity);
    if (!m_vbo->create()) {
      delete m_vbo;
      m_vbo = nullptr;
      m_vboCapacity = 0;
      m_ranges.clear();
      return;
    }
  }

  rub->updateDynamicBuffer(m_vbo, 0, static_cast<int>(needed), vertices.data());
}

void TextRenderer::draw(QRhiCommandBuffer *cb) {
  if (m_ranges.isEmpty() || !m_pipeline || !m_vbo)
    return;

  cb->setGraphicsPipeline(m_pipeline.get());
  const QRhiCommandBuffer::VertexInput vertexInput(m_vbo, 0);
  cb->setVertexInput(0, 1, &vertexInput);

  for (const auto &range: m_ranges) {
    const auto sprite = m_cache.find(range.key);
    if (sprite == m_cache.end() || !sprite->second.srb)
      continue;
    cb->setShaderResources(sprite->second.srb);
    cb->draw(range.count, 1, range.first);
  }
}

} // namespace vk
