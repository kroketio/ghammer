// FXAA post pass: the 3D frame renders into an offscreen target matching the viewport's format, sample count
// and depth, so pipelines built against the viewport render pass stay valid (render-pass compatibility).

#include "vk_engine.h"
#include "vk_shared.h"

#include <rhi/qrhi.h>
#include <QImage>

namespace vk {

namespace {
QRhiTexture::Format targetFormat(QRhiRenderTarget *target) {
  if (auto *trt = dynamic_cast<QRhiTextureRenderTarget *>(target)) {
    // description() returns by value: keep it alive while its attachment is read.
    const QRhiTextureRenderTargetDescription desc = trt->description();
    const QRhiColorAttachment *ca = desc.cbeginColorAttachments();
    if (ca) {
      if (ca->resolveTexture())
        return ca->resolveTexture()->format();
      if (ca->texture())
        return ca->texture()->format();
      if (ca->renderBuffer())
        return ca->renderBuffer()->backingFormat();
    }
  }
  return QRhiTexture::RGBA8;
}
} // namespace

void Engine3D::releaseFxaa() {
  auto &shared = Shared::instance();
  m_fxaaPipeline.reset();
  if (m_fxaaSrb)
    shared.retireShaderResourceBindings(m_fxaaSrb);
  if (m_fxaaTarget)
    shared.retireRenderTarget(m_fxaaTarget);
  if (m_fxaaRp)
    shared.retireRenderPass(m_fxaaRp);
  if (m_fxaaTex)
    shared.retireTexture(m_fxaaTex);
  if (m_fxaaMsaa)
    shared.retireRenderBuffer(m_fxaaMsaa);
  if (m_fxaaDepth)
    shared.retireRenderBuffer(m_fxaaDepth);
  m_fxaaSrb = nullptr;
  m_fxaaTarget = nullptr;
  m_fxaaRp = nullptr;
  m_fxaaTex = nullptr;
  m_fxaaMsaa = m_fxaaDepth = nullptr;
  m_fxaaSize = QSize();
  m_fxaaSamples = 0;
  m_fxaaPipelineRp = nullptr;
}

QRhiRenderTarget *Engine3D::fxaaSceneTarget(QRhiRenderTarget *target, const QSize &pixelSize, int sampleCount) {
  if (!m_fxaaEnabled || pixelSize.isEmpty()) {
    if (m_fxaaTex)
      releaseFxaa();
    return target;
  }
  if (m_fxaaTarget && m_fxaaSize == pixelSize && m_fxaaSamples == sampleCount)
    return m_fxaaTarget;

  releaseFxaa();
  QRhi *rhi = Shared::instance().rhi();
  const QRhiTexture::Format fmt = targetFormat(target);
  m_fxaaTex = rhi->newTexture(fmt, pixelSize, 1, QRhiTexture::RenderTarget);
  bool ok = m_fxaaTex->create();
  QRhiColorAttachment color;
  if (sampleCount > 1) {
    m_fxaaMsaa = rhi->newRenderBuffer(QRhiRenderBuffer::Color, pixelSize, sampleCount, {}, fmt);
    ok = ok && m_fxaaMsaa->create();
    color.setRenderBuffer(m_fxaaMsaa);
    color.setResolveTexture(m_fxaaTex);
  } else {
    color.setTexture(m_fxaaTex);
  }
  m_fxaaDepth = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, sampleCount);
  ok = ok && m_fxaaDepth->create();
  if (ok) {
    QRhiTextureRenderTargetDescription desc(color);
    desc.setDepthStencilBuffer(m_fxaaDepth);
    m_fxaaTarget = rhi->newTextureRenderTarget(desc);
    m_fxaaRp = m_fxaaTarget->newCompatibleRenderPassDescriptor();
    m_fxaaTarget->setRenderPassDescriptor(m_fxaaRp);
    ok = m_fxaaTarget->create();
  }
  if (ok) {
    m_fxaaSrb = rhi->newShaderResourceBindings();
    m_fxaaSrb->setBindings({QRhiShaderResourceBinding::sampledTexture(
        0, QRhiShaderResourceBinding::FragmentStage, m_fxaaTex, Shared::instance().clampLinearSampler())});
    ok = m_fxaaSrb->create();
  }
  if (!ok) {
    qWarning("fxaa: offscreen target failed, drawing without FXAA");
    releaseFxaa();
    return target;
  }
  m_fxaaSize = pixelSize;
  m_fxaaSamples = sampleCount;
  return m_fxaaTarget;
}

void Engine3D::finishFxaa(QRhiCommandBuffer *cb, QRhiRenderTarget *target, const QSize &pixelSize,
                          int sampleCount) {
  if (!m_fxaaTarget || !m_fxaaSrb)
    return;
  QRhiRenderPassDescriptor *rp = target->renderPassDescriptor();
  if (!m_fxaaPipeline || m_fxaaPipelineRp != rp || m_fxaaPipelineSamples != sampleCount) {
    auto &shared = Shared::instance();
    std::unique_ptr<QRhiGraphicsPipeline> p(shared.rhi()->newGraphicsPipeline());
    p->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/fsquad.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/fxaa.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    p->setVertexInputLayout(layout);
    p->setCullMode(QRhiGraphicsPipeline::None);
    p->setSampleCount(sampleCount);
    p->setRenderPassDescriptor(rp);
    p->setShaderResourceBindings(m_fxaaSrb);
    if (!p->create()) {
      qWarning("fxaa: pipeline failed");
      return;
    }
    m_fxaaPipeline = std::move(p);
    m_fxaaPipelineRp = rp;
    m_fxaaPipelineSamples = sampleCount;
  }
  // EDITOR_LUTDEBUG: prints the frame's mean colour. =1 reads back 40 frames, n reads back n (the last is dumped).
  static int frameDebug = !qEnvironmentVariableIsSet("EDITOR_LUTDEBUG")
                              ? 0
                              : std::max(40, qEnvironmentVariableIntValue("EDITOR_LUTDEBUG"));
  if (frameDebug > 0) {
    --frameDebug;
    auto *res = new QRhiReadbackResult;
    res->completed = [res] {
      const auto *p = reinterpret_cast<const uchar *>(res->data.constData());
      const qsizetype n = res->data.size() / 4;
      double sum[3] = {0, 0, 0};
      for (qsizetype i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
          sum[c] += p[i * 4 + c];
      qInfo("[lutdebug] frame mean rgb %.1f %.1f %.1f", sum[0] / n, sum[1] / n, sum[2] / n);
      // EDITOR_FRAMEDUMP=<file.png>: also save the frame.
      if (const QString dump = qEnvironmentVariable("EDITOR_FRAMEDUMP"); !dump.isEmpty()) {
        QImage img(reinterpret_cast<const uchar *>(res->data.constData()), res->pixelSize.width(),
                   res->pixelSize.height(), QImage::Format_RGBA8888);
        img.convertToFormat(QImage::Format_RGB888).scaledToWidth(960, Qt::SmoothTransformation).save(dump);
      }
      delete res;
    };
    auto *batch = Shared::instance().rhi()->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(m_fxaaTex), res);
    cb->resourceUpdate(batch);
  }
  cb->beginPass(target, Qt::black, {1.0f, 0}, nullptr);
  drawFullscreen(cb, m_fxaaPipeline.get(), m_fxaaSrb, pixelSize);
  cb->endPass();
}

} // namespace vk
