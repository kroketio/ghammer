// Entity I/O lines in the 3D view (Engine3D::ioLinks()): camera-facing strips built on the CPU every frame,
// drawn twice from one buffer: depth tested at full strength, then where hidden at a quarter strength.

#include <QLoggingCategory>

#include "vk_engine.h"
#include "vk_shared.h"
#include "vk_text.h"

#include "client/prefs/preferences.h"

namespace vk {

namespace {
QLoggingCategory ioLineLog("vk.iolines", QtWarningMsg);
constexpr int kFloatsPerVertex = 8; // pos3 color4 dash1

const glm::vec4 kOut(0.31f, 0.82f, 1.0f, 1.0f);   // leaves the selection
const glm::vec4 kIn(1.0f, 0.62f, 0.25f, 1.0f);    // arrives at the selection
const glm::vec4 kOther(0.45f, 0.72f, 0.95f, 0.5f); // not touching the selection
const glm::vec4 kRef(0.78f, 0.78f, 0.85f, 0.75f); // Target property
const glm::vec4 kBroken(1.0f, 0.35f, 0.35f, 1.0f);
const glm::vec4 kPulse(1.0f, 0.95f, 0.45f, 1.0f);
} // namespace

void Engine3D::releaseIOLinePipelines() {
  delete m_ioLineVisiblePipeline;
  delete m_ioLineHiddenPipeline;
  m_ioLineVisiblePipeline = nullptr;
  m_ioLineHiddenPipeline = nullptr;
}

void Engine3D::setupIOLinePipelines(QRhiRenderPassDescriptor *rp, const int sampleCount) {
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi || !m_cutWireSrb)
    return;
  if (!m_ioLineVb) {
    m_ioLineVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 1 << 16);
    if (!m_ioLineVb->create())
      qWarning(ioLineLog, "I/O line vertex buffer failed");
  }
  auto make = [&](QRhiGraphicsPipeline::CompareOp depthOp) -> QRhiGraphicsPipeline * {
    auto *p = rhi->newGraphicsPipeline();
    p->setShaderStages({
        {QRhiShaderStage::Vertex, Shared::instance().shader(QStringLiteral(":/shaders/ioline.vert.qsb"))},
        {QRhiShaderStage::Fragment, Shared::instance().shader(QStringLiteral(":/shaders/ioline.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{kFloatsPerVertex * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                          {0, 1, QRhiVertexInputAttribute::Float4, 3 * sizeof(float)},
                          {0, 2, QRhiVertexInputAttribute::Float, 7 * sizeof(float)}});
    p->setVertexInputLayout(layout);
    p->setTopology(QRhiGraphicsPipeline::Triangles);
    p->setCullMode(QRhiGraphicsPipeline::None);
    p->setDepthTest(true);
    p->setDepthOp(depthOp);
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
      qWarning(ioLineLog, "I/O line pipeline failed");
      delete p;
      return nullptr;
    }
    return p;
  };
  if (!m_ioLineVisiblePipeline)
    m_ioLineVisiblePipeline = make(QRhiGraphicsPipeline::LessOrEqual);
  if (!m_ioLineHiddenPipeline)
    m_ioLineHiddenPipeline = make(QRhiGraphicsPipeline::Greater);
}

void Engine3D::buildIOLineGeometry() {
  m_ioLineVertexCount = 0;
  if (!m_ioLineVb || !m_scene || (!m_showIOLines && !m_ioSim))
    return;
  const std::vector<IOLink> &links = ioLinks();
  const std::vector<IOPulse> pulses = ioPulses();
  if (links.empty() && pulses.empty())
    return;

  const glm::vec3 cam = m_camera->getPosition();
  const glm::vec3 camUp = m_camera->getUp();
  const float viewH = float(std::max(1, m_lastViewSize.height()));
  const float halfFov = 0.5f * glm::radians(m_camera->getFov());
  const float worldPerPx = (2.0f * std::tan(halfFov)) / viewH;
  const float iconPx = billboardHalfPx(false);
  auto pxAt = [&](const glm::vec3 &p) { return worldPerPx * std::max(glm::length(cam - p), 1e-3f); };

  std::vector<float> v;
  v.reserve(links.size() * 6 * 3 * kFloatsPerVertex);
  auto put = [&v](const glm::vec3 &p, const glm::vec4 &c, float dash) {
    v.insert(v.end(), {p.x, p.y, p.z, c.r, c.g, c.b, c.a, dash});
  };
  // A camera-facing strip `widthPx` wide; dashed when `dashed`.
  auto strip = [&](const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &c, float widthPx, bool dashed) {
    const glm::vec3 dir = b - a;
    const float len = glm::length(dir);
    if (len < 1e-5f)
      return;
    const glm::vec3 mid = (a + b) * 0.5f;
    const glm::vec3 toCam = glm::normalize(cam - mid);
    glm::vec3 side = glm::cross(dir / len, toCam);
    if (glm::length(side) < 1e-5f)
      side = camUp;
    side = glm::normalize(side);
    const glm::vec3 sa = side * (pxAt(a) * widthPx * 0.5f), sb = side * (pxAt(b) * widthPx * 0.5f);
    const float da = dashed ? 0.0f : -1.0f;
    const float db = dashed ? len / pxAt(mid) : -1.0f;
    put(a + sa, c, da);
    put(a - sa, c, da);
    put(b - sb, c, db);
    put(a + sa, c, da);
    put(b - sb, c, db);
    put(b + sb, c, db);
  };
  auto arrow = [&](const glm::vec3 &tip, const glm::vec3 &dirIn, const glm::vec4 &c, float sizePx) {
    const glm::vec3 dir = glm::normalize(dirIn);
    const float px = pxAt(tip);
    glm::vec3 side = glm::cross(dir, glm::normalize(cam - tip));
    if (glm::length(side) < 1e-5f)
      side = camUp;
    side = glm::normalize(side) * (px * sizePx * 0.45f);
    const glm::vec3 base = tip - dir * (px * sizePx);
    put(tip, c, -1.0f);
    put(base + side, c, -1.0f);
    put(base - side, c, -1.0f);
  };
  auto diamond = [&](const glm::vec3 &p, const glm::vec4 &c, float sizePx) {
    const float s = pxAt(p) * sizePx;
    const glm::vec3 r = glm::normalize(glm::cross(glm::normalize(cam - p), camUp)) * s;
    const glm::vec3 u = camUp * s;
    put(p + u, c, -1.0f);
    put(p + r, c, -1.0f);
    put(p - u, c, -1.0f);
    put(p + u, c, -1.0f);
    put(p - u, c, -1.0f);
    put(p - r, c, -1.0f);
  };

  m_ioLabelQueue.clear();
  for (const IOLink &l: links) {
    glm::vec4 col = l.kind == IOLink::Reference ? kRef
        : l.kind == IOLink::Broken            ? kBroken
        : l.selectedSide == 1                 ? kOut
        : l.selectedSide == 2                 ? kIn
                                              : kOther;
    const bool sel = l.selectedSide != 0;
    if (l.kind == IOLink::Reference && !sel)
      col.a *= 0.6f;
    const float width = sel ? 3.0f : 1.6f;
    if (l.kind == IOLink::Broken) {
      const glm::vec3 b = l.a + camUp * (pxAt(l.a) * (iconPx + 26.0f));
      strip(l.a, b, col, width, false);
      if (sel || m_ioLinesAll)
        m_ioLabelQueue.push_back({b + camUp * (pxAt(b) * 8.0f), QStringLiteral("? ") + l.label, col});
      continue;
    }
    // Stop short of the target icon so the arrowhead is not hidden.
    const glm::vec3 d = l.b - l.a;
    const float len = glm::length(d);
    if (len < 1e-4f)
      continue;
    const glm::vec3 dir = d / len;
    const float startGap = std::min(len * 0.3f, pxAt(l.a) * iconPx * 0.8f);
    const float endGap = std::min(len * 0.3f, pxAt(l.b) * iconPx * 0.9f);
    const glm::vec3 a = l.a + dir * startGap;
    const glm::vec3 tip = l.b - dir * endGap;
    strip(a, tip, col, width, l.delayed || l.kind == IOLink::Reference);
    arrow(tip, dir, col, sel ? 13.0f : 9.0f);
    if (sel && m_ioLabels && m_ioLabelQueue.size() < 64)
      m_ioLabelQueue.push_back({(a + tip) * 0.5f, l.label, col});
  }
  for (const IOPulse &p: pulses) {
    const glm::vec3 pos = glm::mix(p.a, p.b, p.t);
    diamond(pos, kPulse, 6.0f);
    diamond(pos, glm::vec4(1.0f, 1.0f, 1.0f, 0.9f), 3.0f);
  }
  if (v.empty())
    return;

  // Second copy for the parts behind geometry, at a quarter strength.
  const size_t visibleFloats = v.size();
  v.resize(visibleFloats * 2);
  for (size_t i = 0; i < visibleFloats; i += kFloatsPerVertex) {
    std::copy(v.begin() + long(i), v.begin() + long(i + kFloatsPerVertex), v.begin() + long(visibleFloats + i));
    v[visibleFloats + i + 6] *= 0.28f;
  }

  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;
  const quint32 bytes = quint32(v.size() * sizeof(float));
  if (bytes > m_ioLineVb->size()) {
    quint32 size = m_ioLineVb->size();
    Shared::instance().retire(m_ioLineVb);
    while (size < bytes)
      size *= 2;
    m_ioLineVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, size);
    if (!m_ioLineVb->create()) {
      qWarning(ioLineLog, "I/O line vertex buffer grow failed");
      m_ioLineVb = nullptr;
      return;
    }
  }
  if (!m_ioLineBatch)
    m_ioLineBatch = rhi->nextResourceUpdateBatch();
  m_ioLineBatch->updateDynamicBuffer(m_ioLineVb, 0, bytes, v.data());
  m_ioLineVertexCount = quint32(visibleFloats / kFloatsPerVertex);
}

int Engine3D::buildIOLabels() {
  // Must run after buildIOLineGeometry filled the queue and before TextRenderer::prepare.
  int drawn = 0;
  const glm::vec3 cam = m_camera->getPosition();
  const float viewH = float(std::max(1, m_lastViewSize.height()));
  const float worldPerPx = (2.0f * std::tan(0.5f * glm::radians(m_camera->getFov()))) / viewH;
  for (const auto &q: m_ioLabelQueue) {
    const QSize sprite = m_text->size(q.text);
    if (sprite.isEmpty())
      continue;
    glm::vec3 u = m_camera->getRight();
    glm::vec3 vv = -m_camera->getUp();
    if (projectToScreen(q.pos + u).x < projectToScreen(q.pos).x)
      u = -u;
    if (projectToScreen(q.pos + vv).y < projectToScreen(q.pos).y)
      vv = -vv;
    // Text sprites are rendered at 28 px; show them at about half that.
    const float px = worldPerPx * std::max(glm::length(cam - q.pos), 1e-3f) * 0.5f;
    const float w = float(sprite.width()) * px, h = float(sprite.height()) * px;
    const glm::vec3 origin = q.pos - u * (w * 0.5f) - vv * (h * 1.2f);
    m_text->drawDecal(q.text, origin, u, vv, w, h, QColor::fromRgbF(q.color.r, q.color.g, q.color.b), 1.0f);
    ++drawn;
  }
  return drawn;
}

void Engine3D::drawIOLines(QRhiCommandBuffer *cb) {
  if (!m_ioLineVertexCount || !m_ioLineVb || !m_cutWireSrb)
    return;
  const QRhiCommandBuffer::VertexInput vin(m_ioLineVb, 0);
  if (m_ioLineHiddenPipeline) {
    cb->setGraphicsPipeline(m_ioLineHiddenPipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_ioLineVertexCount, 1, m_ioLineVertexCount);
  }
  if (m_ioLineVisiblePipeline) {
    cb->setGraphicsPipeline(m_ioLineVisiblePipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_ioLineVertexCount);
  }
}

} // namespace vk
